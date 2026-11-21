/*
 * mai_core.c: the platform-independent half of mob_audio_io. No erl_nif and
 * no platform audio API, so the host test (test/c/mai_core_test.c) compiles
 * it with any C compiler.
 *
 *   registry   processors by name (mob_audio_io.h mai_register), a fixed
 *              table under a mutex the audio thread never takes. A stream
 *              copies its processor at open and pins the entry (refs).
 *   streams    a fixed table of MAI_MAX_STREAMS slots; an id is the slot
 *              index plus a generation, so a stale id never reaches a reused
 *              slot.
 *   process    mai__process wraps the processor's capture and render with
 *              the stream clock, the input meter and the counters status/0
 *              reports (callbacks, frames per callback, worst callback time,
 *              worst gap between callbacks).
 *   timing     the clock (mai_now_ns), the presentation-time anchor a normal
 *              thread publishes and the audio thread reads (a seqlock), and
 *              the arithmetic both platforms use to turn a timestamp into a
 *              block's presentation time.
 *   posts      per stream, a single-producer single-consumer ring the audio
 *              thread writes (mai_post) and the event thread drains.
 *   pcm        the growing, capped buffer a file decode writes floats into.
 *
 * mai_nif.c includes this file; the platform files include mai_nif.c.
 */
#include <math.h>
#include <pthread.h>
#include <stdatomic.h>
#include <stdbool.h>
#include <stdint.h>
#include <stdlib.h>
#include <string.h>
#include <time.h>

#include "../../include/mob_audio_io.h"

#define MAI_MAX_PROCESSORS 16
#define MAI_MAX_STREAMS 8 /* slot index: the id's low 3 bits */
#define MAI_SLOT_BITS 3
#define MAI_POSTS 64 /* per stream, a power of two */
#define MAI_ERRMAX 160
#define MAI_DEVICE_MAX 128

/* A helper only some platforms (or only the host test) call. */
#define MAI_HELPER static __attribute__((unused))

/* Pending work for the event thread, set from any thread (atomic or). */
#define MAI_PF_DEVICE_LOST 0x01u  /* the platform stream disconnected */
#define MAI_PF_INTERRUPTED 0x02u  /* iOS: interruption began */
#define MAI_PF_RESUMED 0x04u      /* iOS: interruption ended */
#define MAI_PF_ROUTE 0x08u        /* a new route */
#define MAI_PF_RESET 0x10u        /* iOS: the rate changed or media services were reset: rebuild */
#define MAI_PF_OWNER_DOWN 0x20u   /* the owner exited: close */
#define MAI_PF_FOREGROUND 0x40u   /* iOS: back in the foreground (or a stale suspended "began"): one reopen try */

/* Directions: bits, so a duplex stream is both. */
#define MAI_DIR_OUT 1
#define MAI_DIR_IN 2
#define MAI_DIR_DUPLEX (MAI_DIR_OUT | MAI_DIR_IN)

enum { MAI_SHARING_SHARED = 0, MAI_SHARING_EXCLUSIVE = 1 };
enum { MAI_SESSION_PLAY = 0, MAI_SESSION_MIX = 1, MAI_SESSION_PLAY_AND_RECORD = 2 };
enum {
    MAI_PRESET_DEFAULT = 0,     /* Android: the platform's (VOICE_RECOGNITION); iOS: mode Default */
    MAI_PRESET_GENERIC = 1,     /* Android GENERIC; iOS mode Default */
    MAI_PRESET_PERFORMANCE = 2, /* Android VOICE_PERFORMANCE (API 29+); iOS mode Default */
    MAI_PRESET_UNPROCESSED = 3, /* Android UNPROCESSED; iOS mode Measurement */
    MAI_PRESET_VOICE = 4        /* Android VOICE_COMMUNICATION; iOS mode VoiceChat + VoiceProcessingIO */
};
/* Where mai_time.present_ns comes from (status/0 `present`). */
enum { MAI_PRESENT_NONE = 0, MAI_PRESENT_TIMESTAMP = 1, MAI_PRESENT_ESTIMATE = 2 };

typedef struct {
    char name[MAI_NAME_MAX];
    mai_processor p;
    void *ctx;
    int refs; /* open streams using it */
    bool used;
} mai_entry;

typedef struct {
    uint16_t len;
    uint8_t data[MAI_POST_MAX];
} mai_msg;

typedef struct mai_stream {
    /* Set at open, read by every thread while the slot is open. */
    _Atomic bool open;
    uint32_t gen;
    int32_t id;
    mai_entry *entry; /* pinned registry entry */
    mai_processor p;  /* copies of entry->p / ctx (the audio thread never touches the registry) */
    void *ctx;
    /* What the opener asked for. */
    int dir;                   /* MAI_DIR_* */
    int32_t want_rate;         /* 0: the device's */
    int32_t want_frames;       /* 0: low latency (2 bursts / a 5 ms IO buffer) */
    int32_t want_channels;     /* output channels (1 or 2) */
    int32_t want_in_channels;  /* input channels (1 or 2) */
    int sharing, session, preset;
    char want_device[MAI_DEVICE_MAX];    /* "": the default. Android: the id in decimal; iOS: a port UID */
    char want_in_device[MAI_DEVICE_MAX]; /* duplex: the input's ("" for input streams: want_device) */
    /* The platform stream, under the plugin's lock. */
    void *be;  /* the backend's handle (AAudioStream * / AudioUnit); NULL while closed */
    void *be2; /* a second handle (Android: a duplex stream's input) */
    bool running;     /* be is open and started */
    bool interrupted; /* iOS: closed by an interruption, reopened at its end */
    int32_t rate, channels, in_channels, burst, buffer, capacity;
    int32_t device_id, in_device_id; /* Android: the AAudio device ids; iOS: 0 */
    int32_t preset_granted;          /* Android: getInputPreset; iOS: MAI_PRESET_* asked for */
    bool exclusive;
    int present; /* MAI_PRESENT_* */
    int32_t xruns_seen; /* the last xrun count the event thread reported */
    int64_t next_reopen_ns;
    int reopen_failures;
    int64_t started_ns; /* mai_now_ns() at the platform stream's last start */
    int quick_losses;   /* device losses in a row, each < 1 s after its start */
    char last_error[MAI_ERRMAX];
    /* The audio thread's own (written only by the callback while running). */
    int64_t frame;     /* the stream clock */
    int64_t be_frame0; /* the stream clock when this platform stream started */
    int64_t last_cb_ns;
    double last_sample_time, last_frames; /* iOS: gaps in the device's sample time */
    /* The presentation-time anchor: stream-clock frame `anchor_frame` was
     * (or will be) presented at `anchor_ns`. One normal thread writes it
     * (mai__set_anchor), the audio thread reads it (mai__get_anchor). */
    _Atomic uint32_t anchor_seq;
    _Atomic int64_t anchor_frame, anchor_ns;
    /* Platform latencies in ns (iOS: the session's; Android 8-11: the buffer). */
    _Atomic int64_t out_latency_ns, in_latency_ns;
    /* Counters, written by the audio thread, read by status. */
    _Atomic uint64_t callbacks, frames_total, cb_total_ns;
    _Atomic uint32_t cb_max_ns, gap_max_ns, cb_frames_min, cb_frames_max, cb_frames_last;
    _Atomic uint32_t be_xruns;      /* iOS: gaps found by the callback */
    _Atomic uint32_t in_underflows; /* duplex: callbacks whose input came short (zero-filled) */
    _Atomic uint32_t in_peak_bits, in_rms_bits; /* float bits: peak since reset_stats, last block's RMS */
    _Atomic uint32_t reopens, posts_dropped;
    _Atomic uint32_t pending; /* MAI_PF_* */
    /* posts: the audio thread writes tail, the event thread head. */
    _Atomic uint32_t post_head, post_tail;
    mai_msg posts[MAI_POSTS];
} mai_stream;

static pthread_mutex_t g_reg_lock = PTHREAD_MUTEX_INITIALIZER;
static mai_entry g_reg[MAI_MAX_PROCESSORS];
static mai_stream g_streams[MAI_MAX_STREAMS];
static uint32_t g_next_gen = 1;

/* ── Clock ─────────────────────────────────────────────────────────────── */

int64_t mai_now_ns(void) {
    struct timespec ts;
#ifdef __APPLE__
    /* mach_absolute_time's clock, the one AudioTimeStamp.mHostTime counts */
    clock_gettime(CLOCK_UPTIME_RAW, &ts);
#else
    clock_gettime(CLOCK_MONOTONIC, &ts);
#endif
    return (int64_t)ts.tv_sec * 1000000000LL + ts.tv_nsec;
}

MAI_HELPER void mai__set_error(mai_stream *s, const char *what) {
    strncpy(s->last_error, what, MAI_ERRMAX - 1);
    s->last_error[MAI_ERRMAX - 1] = 0;
}

/* ── Registry ──────────────────────────────────────────────────────────── */

static bool mai__name_ok(const char *name) {
    if (!name || !name[0]) return false;
    return strlen(name) < MAI_NAME_MAX;
}

/* (g_reg_lock held) */
static mai_entry *mai__find_locked(const char *name) {
    for (int i = 0; i < MAI_MAX_PROCESSORS; i++)
        if (g_reg[i].used && strcmp(g_reg[i].name, name) == 0) return &g_reg[i];
    return NULL;
}

int32_t mai_abi_version(void) { return MAI_ABI_VERSION; }

int32_t mai_register(const char *name, const mai_processor *p, void *ctx) {
    if (!mai__name_ok(name) || !p) return MAI_ERR_INVALID;
    pthread_mutex_lock(&g_reg_lock);
    mai_entry *e = mai__find_locked(name);
    if (e && e->refs > 0) {
        pthread_mutex_unlock(&g_reg_lock);
        return MAI_ERR_BUSY;
    }
    if (!e) {
        for (int i = 0; i < MAI_MAX_PROCESSORS && !e; i++)
            if (!g_reg[i].used) e = &g_reg[i];
    }
    if (!e) {
        pthread_mutex_unlock(&g_reg_lock);
        return MAI_ERR_FULL;
    }
    memset(e, 0, sizeof *e);
    strcpy(e->name, name);
    e->p = *p;
    e->ctx = ctx;
    e->used = true;
    pthread_mutex_unlock(&g_reg_lock);
    return MAI_OK;
}

int32_t mai_unregister(const char *name) {
    if (!mai__name_ok(name)) return MAI_ERR_INVALID;
    pthread_mutex_lock(&g_reg_lock);
    mai_entry *e = mai__find_locked(name);
    int32_t r = !e ? MAI_ERR_NOT_FOUND : e->refs > 0 ? MAI_ERR_BUSY : MAI_OK;
    if (r == MAI_OK) memset(e, 0, sizeof *e);
    pthread_mutex_unlock(&g_reg_lock);
    return r;
}

/* Pin `name` for a stream: copies of its processor and ctx into `s`. */
static int32_t mai__acquire(mai_stream *s, const char *name) {
    pthread_mutex_lock(&g_reg_lock);
    mai_entry *e = mai__name_ok(name) ? mai__find_locked(name) : NULL;
    if (e) {
        e->refs++;
        s->entry = e;
        s->p = e->p;
        s->ctx = e->ctx;
    }
    pthread_mutex_unlock(&g_reg_lock);
    return e ? MAI_OK : MAI_ERR_NOT_FOUND;
}

static void mai__release(mai_stream *s) {
    pthread_mutex_lock(&g_reg_lock);
    if (s->entry) s->entry->refs--;
    s->entry = NULL;
    pthread_mutex_unlock(&g_reg_lock);
}

/* Names of the registered processors into `names` (up to `max`); the count. */
static int mai__processor_names(char names[][MAI_NAME_MAX], int max) {
    int n = 0;
    pthread_mutex_lock(&g_reg_lock);
    for (int i = 0; i < MAI_MAX_PROCESSORS && n < max; i++)
        if (g_reg[i].used) strcpy(names[n++], g_reg[i].name);
    pthread_mutex_unlock(&g_reg_lock);
    return n;
}

/* ── Built-in processors ───────────────────────────────────────────────── */

/* "mai_silence": what the self-test opens. Plays nothing, drops its input
 * (the plugin's meter still measures it: status/0 in_peak_db, in_rms_db). */
static void mai__silence_render(void *ctx, float *out, int32_t frames, const mai_time *t) {
    (void)ctx; (void)out; (void)frames; (void)t;
}

static void mai__silence_capture(void *ctx, const float *in, int32_t frames, const mai_time *t) {
    (void)ctx; (void)in; (void)frames; (void)t;
}

/* "mai_sine": 440 Hz at -18 dBFS, for a listening check without app code.
 * The phase comes from the stream clock, so it needs no state. */
static void mai__sine_render(void *ctx, float *out, int32_t frames, const mai_time *t) {
    (void)ctx;
    const double two_pi = 6.283185307179586;
    const double w = two_pi * 440.0 / (t->rate > 0 ? t->rate : 48000);
    const double start = fmod((double)t->frame * w, two_pi);
    for (int32_t i = 0; i < frames; i++) {
        float x = 0.125f * (float)sin(start + w * i);
        for (int32_t c = 0; c < t->channels; c++) out[i * t->channels + c] = x;
    }
}

static void mai__register_builtins(void) {
    static const mai_processor silence = {mai__silence_render, mai__silence_capture, NULL};
    static const mai_processor sine = {mai__sine_render, NULL, NULL};
    pthread_mutex_lock(&g_reg_lock);
    bool have = mai__find_locked("mai_silence") != NULL;
    pthread_mutex_unlock(&g_reg_lock);
    if (have) return;
    mai_register("mai_silence", &silence, NULL);
    mai_register("mai_sine", &sine, NULL);
}

/* ── Streams ───────────────────────────────────────────────────────────── */

/* (the plugin's lock held) A free slot, reset, with a fresh id; NULL if full. */
static mai_stream *mai__alloc_stream(void) {
    for (int i = 0; i < MAI_MAX_STREAMS; i++) {
        mai_stream *s = &g_streams[i];
        if (atomic_load(&s->open)) continue;
        memset(s, 0, sizeof *s);
        s->gen = g_next_gen++ & 0x0fffffffu;
        if (s->gen == 0) s->gen = g_next_gen++;
        s->id = (int32_t)((s->gen << MAI_SLOT_BITS) | (uint32_t)i);
        s->last_sample_time = -1;
        atomic_store(&s->cb_frames_min, UINT32_MAX);
        return s;
    }
    return NULL;
}

/* An open stream by id, or NULL. */
static mai_stream *mai__stream(int32_t id) {
    if (id <= 0) return NULL;
    mai_stream *s = &g_streams[(uint32_t)id & (MAI_MAX_STREAMS - 1)];
    return atomic_load_explicit(&s->open, memory_order_acquire) && s->id == id ? s : NULL;
}

/* The mai_time a stream_event gets: the output's channels, or the input's
 * for an input stream. */
static mai_time mai__time(const mai_stream *s) {
    mai_time t = {s->frame, mai_now_ns(), 0, s->rate, (s->dir & MAI_DIR_OUT) ? s->channels : s->in_channels};
    return t;
}

static void mai__event(mai_stream *s, int32_t event) {
    if (!s->p.stream_event) return;
    mai_time t = mai__time(s);
    s->p.stream_event(s->ctx, s->id, event, &t);
}

/* Before a (re)start, with no callback running: the callback's own state
 * and the anchor of the previous platform stream. The stream clock goes on. */
static void mai__reset_callback_state(mai_stream *s) {
    s->last_cb_ns = 0;
    s->last_sample_time = -1;
    s->be_frame0 = s->frame;
    atomic_store(&s->anchor_ns, 0);
}

/* ── Timing ────────────────────────────────────────────────────────────── */

/* When stream-clock frame `frame` is presented, given that `anchor_frame`
 * is presented at `anchor_ns`, at `rate` frames per second. */
static int64_t mai__frame_time_ns(int64_t anchor_frame, int64_t anchor_ns, int64_t frame, int32_t rate) {
    if (rate <= 0) return 0;
    int64_t d = frame - anchor_frame;
    /* (whole seconds apart, so d * 1e9 can't overflow for any real stream) */
    return anchor_ns + (d / rate) * 1000000000LL + (d % rate) * 1000000000LL / rate;
}

/* Host-clock ticks (mach_absolute_time) to ns with a timebase numer/denom. */
MAI_HELPER int64_t mai__ticks_to_ns(uint64_t ticks, uint32_t numer, uint32_t denom) {
    if (numer == denom || denom == 0) return (int64_t)ticks;
    /* split so ticks * numer can't overflow */
    uint64_t q = ticks / denom, r = ticks % denom;
    return (int64_t)(q * numer + r * numer / denom);
}

/* Publish the anchor (one writer at a time: the event thread, or the
 * opener before the stream starts). `frame` is on the stream clock. */
MAI_HELPER void mai__set_anchor(mai_stream *s, int64_t frame, int64_t ns) {
    uint32_t seq = atomic_load_explicit(&s->anchor_seq, memory_order_relaxed);
    atomic_store_explicit(&s->anchor_seq, seq + 1, memory_order_relaxed);
    atomic_thread_fence(memory_order_release);
    atomic_store_explicit(&s->anchor_frame, frame, memory_order_relaxed);
    atomic_store_explicit(&s->anchor_ns, ns, memory_order_relaxed);
    atomic_store_explicit(&s->anchor_seq, seq + 2, memory_order_release);
}

/* Read the anchor without blocking (the audio thread): false if there is
 * none yet, or a writer kept it busy for three tries. */
static bool mai__get_anchor(mai_stream *s, int64_t *frame, int64_t *ns) {
    for (int tries = 0; tries < 3; tries++) {
        uint32_t a = atomic_load_explicit(&s->anchor_seq, memory_order_acquire);
        if (a & 1u) continue;
        int64_t f = atomic_load_explicit(&s->anchor_frame, memory_order_relaxed);
        int64_t n = atomic_load_explicit(&s->anchor_ns, memory_order_relaxed);
        atomic_thread_fence(memory_order_acquire);
        if (atomic_load_explicit(&s->anchor_seq, memory_order_relaxed) != a) continue;
        if (n == 0) return false;
        *frame = f;
        *ns = n;
        return true;
    }
    return false;
}

/* The presentation time of stream-clock frame `frame` from the anchor; 0
 * without one. */
MAI_HELPER int64_t mai__present_from_anchor(mai_stream *s, int64_t frame) {
    int64_t af, an;
    return mai__get_anchor(s, &af, &an) ? mai__frame_time_ns(af, an, frame, s->rate) : 0;
}

/* ── The audio callback ────────────────────────────────────────────────── */

static void mai__max_u32(_Atomic uint32_t *a, uint32_t v) {
    if (v > atomic_load_explicit(a, memory_order_relaxed)) atomic_store_explicit(a, v, memory_order_relaxed);
}

static void mai__min_u32(_Atomic uint32_t *a, uint32_t v) {
    if (v < atomic_load_explicit(a, memory_order_relaxed)) atomic_store_explicit(a, v, memory_order_relaxed);
}

static uint32_t mai__clamp_u32(int64_t v) { return v < 0 ? 0 : v > UINT32_MAX ? UINT32_MAX : (uint32_t)v; }

static uint32_t mai__float_bits(float f) {
    uint32_t u;
    memcpy(&u, &f, sizeof u);
    return u;
}

static float mai__bits_float(uint32_t u) {
    float f;
    memcpy(&f, &u, sizeof f);
    return f;
}

/* The input meter: the peak since reset_stats and this block's RMS. */
static void mai__meter(mai_stream *s, const float *in, int32_t samples) {
    if (samples <= 0) return;
    float peak = 0.0f;
    double sum = 0.0;
    for (int32_t i = 0; i < samples; i++) {
        float x = in[i];
        float a = x < 0 ? -x : x;
        if (a > peak) peak = a;
        sum += (double)x * x;
    }
    if (peak > mai__bits_float(atomic_load_explicit(&s->in_peak_bits, memory_order_relaxed)))
        atomic_store_explicit(&s->in_peak_bits, mai__float_bits(peak), memory_order_relaxed);
    atomic_store_explicit(&s->in_rms_bits, mai__float_bits((float)sqrt(sum / samples)), memory_order_relaxed);
}

/* One callback. `in`: frames * in_channels captured floats (input, duplex;
 * else NULL). `out`: frames * channels floats to fill (output, duplex; else
 * NULL). `t0`: mai_now_ns() at the callback's start. `in_present` and
 * `out_present`: the blocks' presentation times (0: unknown). */
static void mai__process(mai_stream *s, const float *in, float *out, int32_t frames, int64_t t0, int64_t in_present,
                         int64_t out_present) {
    if (s->last_cb_ns) mai__max_u32(&s->gap_max_ns, mai__clamp_u32(t0 - s->last_cb_ns));
    s->last_cb_ns = t0;
    if (in) {
        if (s->p.capture) {
            mai_time t = {s->frame, t0, in_present, s->rate, s->in_channels};
            s->p.capture(s->ctx, in, frames, &t);
        }
        mai__meter(s, in, frames * s->in_channels);
    }
    if (out) {
        memset(out, 0, sizeof(float) * (size_t)frames * (size_t)s->channels);
        if (s->p.render) {
            mai_time t = {s->frame, t0, out_present, s->rate, s->channels};
            s->p.render(s->ctx, out, frames, &t);
        }
    }
    s->frame += frames;
    int64_t dt = mai_now_ns() - t0;
    mai__max_u32(&s->cb_max_ns, mai__clamp_u32(dt));
    mai__min_u32(&s->cb_frames_min, (uint32_t)frames);
    mai__max_u32(&s->cb_frames_max, (uint32_t)frames);
    atomic_store_explicit(&s->cb_frames_last, (uint32_t)frames, memory_order_relaxed);
    atomic_fetch_add_explicit(&s->cb_total_ns, (uint64_t)dt, memory_order_relaxed);
    atomic_fetch_add_explicit(&s->frames_total, (uint64_t)frames, memory_order_relaxed);
    atomic_fetch_add_explicit(&s->callbacks, 1, memory_order_release);
}

/* Zero the worst-case counters and the input peak (callbacks and frames go on). */
static void mai__reset_stats(mai_stream *s) {
    atomic_store(&s->cb_max_ns, 0);
    atomic_store(&s->gap_max_ns, 0);
    atomic_store(&s->cb_frames_min, UINT32_MAX);
    atomic_store(&s->cb_frames_max, 0);
    atomic_store(&s->in_peak_bits, 0);
}

/* ── Posts ─────────────────────────────────────────────────────────────── */

int32_t mai_post(int32_t stream, const void *msg, size_t n) {
    if (n > MAI_POST_MAX) return MAI_ERR_TOO_LARGE;
    if (n > 0 && !msg) return MAI_ERR_INVALID;
    mai_stream *s = mai__stream(stream);
    if (!s) return MAI_ERR_NOT_OPEN;
    uint32_t tail = atomic_load_explicit(&s->post_tail, memory_order_relaxed);
    uint32_t head = atomic_load_explicit(&s->post_head, memory_order_acquire);
    if (tail - head >= MAI_POSTS) {
        atomic_fetch_add_explicit(&s->posts_dropped, 1, memory_order_relaxed);
        return MAI_ERR_FULL;
    }
    mai_msg *m = &s->posts[tail & (MAI_POSTS - 1)];
    m->len = (uint16_t)n;
    if (n) memcpy(m->data, msg, n);
    atomic_store_explicit(&s->post_tail, tail + 1, memory_order_release);
    return MAI_OK;
}

/* The consumer side (one thread): the next message into `out`; false if none. */
static bool mai__pop_post(mai_stream *s, mai_msg *out) {
    uint32_t head = atomic_load_explicit(&s->post_head, memory_order_relaxed);
    uint32_t tail = atomic_load_explicit(&s->post_tail, memory_order_acquire);
    if (head == tail) return false;
    *out = s->posts[head & (MAI_POSTS - 1)];
    atomic_store_explicit(&s->post_head, head + 1, memory_order_release);
    return true;
}

/* ── PCM (file decoding) ───────────────────────────────────────────────── */

/* Interleaved floats a decoder appends to, grown by `grow` (which sets
 * `data` and `cap` to at least `bytes`, or answers false) and capped at
 * `max_bytes`. `mono` folds every source frame to one channel. */
typedef struct mai_pcm {
    float *data;
    size_t cap;    /* bytes */
    size_t frames; /* frames written */
    size_t max_bytes;
    int32_t channels; /* of `data`: 1 when mono, else the source's; 0 until the first push */
    bool mono;
    bool (*grow)(struct mai_pcm *pcm, size_t bytes);
    void *user;
} mai_pcm;

/* Results of the pushes. */
#define MAI_PCM_OK 0
#define MAI_PCM_TOO_LARGE 1 /* over max_bytes */
#define MAI_PCM_NO_MEMORY 2 /* grow failed */
#define MAI_PCM_CHANNELS 3  /* the source's channel count changed mid-file */

static int mai__pcm_reserve(mai_pcm *p, size_t frames, int32_t src_channels) {
    int32_t ch = p->mono ? 1 : src_channels;
    if (ch <= 0) return MAI_PCM_CHANNELS;
    if (p->channels == 0) p->channels = ch;
    if (p->channels != ch) return MAI_PCM_CHANNELS;
    size_t need = (p->frames + frames) * (size_t)ch * sizeof(float);
    if (need > p->max_bytes) return MAI_PCM_TOO_LARGE;
    if (need <= p->cap) return MAI_PCM_OK;
    size_t cap = p->cap ? p->cap : 1u << 16;
    if (cap > p->max_bytes) cap = p->max_bytes;
    while (cap < need) cap = cap > p->max_bytes / 2 ? p->max_bytes : cap * 2;
    return p->grow(p, cap) ? MAI_PCM_OK : MAI_PCM_NO_MEMORY;
}

/* Append `frames` interleaved float frames of `src_channels`. */
MAI_HELPER int mai__pcm_push_f32(mai_pcm *p, const float *x, size_t frames, int32_t src_channels) {
    int r = mai__pcm_reserve(p, frames, src_channels);
    if (r != MAI_PCM_OK) return r;
    float *o = p->data + p->frames * (size_t)p->channels;
    if (!p->mono) {
        memcpy(o, x, frames * (size_t)src_channels * sizeof(float));
    } else {
        for (size_t f = 0; f < frames; f++) {
            float sum = 0.0f;
            for (int32_t c = 0; c < src_channels; c++) sum += x[f * (size_t)src_channels + (size_t)c];
            o[f] = sum / (float)src_channels;
        }
    }
    p->frames += frames;
    return MAI_PCM_OK;
}

/* Append `frames` interleaved 16-bit frames of `src_channels`, as floats. */
MAI_HELPER int mai__pcm_push_s16(mai_pcm *p, const int16_t *x, size_t frames, int32_t src_channels) {
    int r = mai__pcm_reserve(p, frames, src_channels);
    if (r != MAI_PCM_OK) return r;
    float *o = p->data + p->frames * (size_t)p->channels;
    for (size_t f = 0; f < frames; f++) {
        if (!p->mono) {
            for (int32_t c = 0; c < src_channels; c++)
                o[f * (size_t)src_channels + (size_t)c] = x[f * (size_t)src_channels + (size_t)c] / 32768.0f;
        } else {
            float sum = 0.0f;
            for (int32_t c = 0; c < src_channels; c++) sum += x[f * (size_t)src_channels + (size_t)c] / 32768.0f;
            o[f] = sum / (float)src_channels;
        }
    }
    p->frames += frames;
    return MAI_PCM_OK;
}
