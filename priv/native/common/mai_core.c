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
 *   decode     a file decode's chunking, mono fold, length cap, cancel flag
 *              and back-pressure (the owner acknowledges chunks), between
 *              the platform decoder and the decode's sink or processor.
 *
 * mai_nif.c includes this file; the platform files include mai_nif.c.
 */
#include <math.h>
#include <pthread.h>
#include <stdatomic.h>
#include <stdbool.h>
#include <stdint.h>
#include <stdlib.h>
#include <stdio.h>
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
static _Atomic uint32_t g_next_gen = 1;

/* A fresh, nonzero 28-bit generation. Streams and decodes both take theirs
 * from here, so a decode's id is never a stream's. */
static uint32_t mai__next_gen(void) {
    uint32_t g;
    do g = atomic_fetch_add(&g_next_gen, 1) & 0x0fffffffu;
    while (g == 0);
    return g;
}

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

/* Pin `name`: copies of its processor and ctx; its entry's refs count the
 * pins (mai_register and mai_unregister answer MAI_ERR_BUSY meanwhile). */
static int32_t mai__pin(const char *name, mai_entry **entry, mai_processor *p, void **ctx) {
    pthread_mutex_lock(&g_reg_lock);
    mai_entry *e = mai__name_ok(name) ? mai__find_locked(name) : NULL;
    if (e) {
        e->refs++;
        *entry = e;
        *p = e->p;
        *ctx = e->ctx;
    }
    pthread_mutex_unlock(&g_reg_lock);
    return e ? MAI_OK : MAI_ERR_NOT_FOUND;
}

static void mai__unpin(mai_entry **entry) {
    pthread_mutex_lock(&g_reg_lock);
    if (*entry) (*entry)->refs--;
    *entry = NULL;
    pthread_mutex_unlock(&g_reg_lock);
}

/* Pin `name` for a stream. */
static int32_t mai__acquire(mai_stream *s, const char *name) { return mai__pin(name, &s->entry, &s->p, &s->ctx); }

static void mai__release(mai_stream *s) { mai__unpin(&s->entry); }

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
        s->gen = mai__next_gen();
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

/* ── Streaming decode ──────────────────────────────────────────────────── */

/* A file decode runs on a thread of its own (mai_nif.c decode_main). The
 * platform decoder (be_decode) calls mai__dec_begin once it knows the
 * format, then pushes each codec buffer's frames; the core folds them to
 * mono if asked and cuts them into chunks of chunk_frames, which go to the
 * decode's processor (its capture, on this thread) or to its sink (the
 * owner's mailbox). The sink may hold `window` chunks the owner hasn't
 * acknowledged (mai__dec_ack); the next waits, so memory stays bounded
 * whatever the file's length. The pushes and the platform's loop check the
 * cancel flag: a cancel (mai__dec_cancel) stops a decode within one codec
 * buffer, or at once while it waits for an ack.
 *
 * g_dec_lock guards the slots (used, id), cancelled and inflight, and is
 * held for the sink's started, chunk and ended: once mai__dec_cancel has
 * answered MAI_OK, the owner gets nothing for the decode but its cancelled
 * end. */

#define MAI_MAX_DECODES 4 /* slot index: the id's low MAI_SLOT_BITS, as streams */
_Static_assert(MAI_MAX_DECODES <= (1 << MAI_SLOT_BITS), "a decode's slot fits the id's slot bits");

/* What mai__dec_begin and the pushes answer. */
#define MAI_DEC_GO 0
#define MAI_DEC_STOP 1      /* cancelled, or the owner is gone */
#define MAI_DEC_TOO_LONG 2  /* past max_ms */
#define MAI_DEC_NO_MEMORY 3
#define MAI_DEC_FORMAT 4    /* no rate or channels, or the channel count changed mid-file */

/* How a decode ended: its owner's last message. */
enum { MAI_DEC_DONE = 0, MAI_DEC_CANCELLED = 1, MAI_DEC_FAILED = 2 };

typedef struct mai_decode mai_decode;

/* Where a decode's messages go (the NIF: the owner). started, chunk and
 * ended run under g_dec_lock; buffer and release don't. */
typedef struct mai_dec_sink {
    /* A buffer for the next chunk, room for `samples` floats; NULL: no memory. */
    float *(*buffer)(mai_decode *d, size_t samples);
    /* A buffer from `buffer` that won't be sent. */
    void (*release)(mai_decode *d, float *buf);
    /* The format is known (d->rate, channels, declared_frames). False: the owner is gone. */
    bool (*started)(mai_decode *d);
    /* `frames` frames in `buf` (from `buffer`, the sink's from now on). False: the owner is gone. */
    bool (*chunk)(mai_decode *d, float *buf, int32_t frames);
    /* The end, `how` a MAI_DEC_DONE/CANCELLED/FAILED; MAI_DEC_FAILED with an
     * error atom in `reason` and maybe a `detail` ("" if none). */
    void (*ended)(mai_decode *d, int how, const char *reason, const char *detail);
} mai_dec_sink;

struct mai_decode {
    /* g_dec_lock */
    bool used;
    int32_t id;
    bool cancelled;
    int32_t inflight; /* chunks sent and not yet acknowledged */
    pthread_cond_t acked;
    /* Set before the worker starts. */
    bool mono;
    int32_t chunk_frames; /* frames per chunk, the last one shorter */
    int32_t window;       /* chunks the owner may hold unacknowledged */
    int64_t max_ms;       /* 0: no cap */
    const mai_dec_sink *sink;
    void *user;       /* the sink's own */
    mai_entry *entry; /* the processor chunks go to (pinned); NULL: the sink's owner */
    mai_processor p;
    void *ctx;
    /* The worker's. */
    _Atomic bool stop; /* cancelled: what the worker polls without the lock */
    bool begun;        /* the sink's started went out (and the processor's STARTED) */
    int32_t rate, channels, src_channels;
    int64_t declared_frames; /* the length the file states, at `rate`; -1: unknown */
    int64_t max_frames;      /* 0: no cap */
    float *buf;              /* the chunk being filled */
    int32_t buf_n;           /* its frames */
    int64_t frames;          /* frames delivered, and those in buf */
};

static pthread_mutex_t g_dec_lock = PTHREAD_MUTEX_INITIALIZER;
static mai_decode g_decodes[MAI_MAX_DECODES];

/* A free slot, zeroed, with a fresh id; NULL while MAI_MAX_DECODES run.
 * The caller sets the options and the sink, then starts the worker or
 * gives the slot back with mai__dec_discard. */
MAI_HELPER mai_decode *mai__dec_alloc(void) {
    pthread_mutex_lock(&g_dec_lock);
    for (int i = 0; i < MAI_MAX_DECODES; i++) {
        mai_decode *d = &g_decodes[i];
        if (d->used) continue;
        memset(d, 0, sizeof *d);
        d->used = true;
        d->id = (int32_t)((mai__next_gen() << MAI_SLOT_BITS) | (uint32_t)i);
        d->declared_frames = -1;
        pthread_cond_init(&d->acked, NULL);
        pthread_mutex_unlock(&g_dec_lock);
        return d;
    }
    pthread_mutex_unlock(&g_dec_lock);
    return NULL;
}

/* (g_dec_lock held) The running decode `id`, or NULL. */
static mai_decode *mai__dec_find_locked(int32_t id) {
    if (id <= 0) return NULL;
    uint32_t slot = (uint32_t)id & ((1u << MAI_SLOT_BITS) - 1);
    if (slot >= MAI_MAX_DECODES) return NULL;
    mai_decode *d = &g_decodes[slot];
    return d->used && d->id == id ? d : NULL;
}

/* Send the chunks to processor `name` instead of the owner. */
MAI_HELPER int32_t mai__dec_use_processor(mai_decode *d, const char *name) {
    return mai__pin(name, &d->entry, &d->p, &d->ctx);
}

/* Give back a slot whose worker never started. */
MAI_HELPER void mai__dec_discard(mai_decode *d) {
    mai__unpin(&d->entry);
    pthread_mutex_lock(&g_dec_lock);
    pthread_cond_destroy(&d->acked);
    d->used = false;
    pthread_mutex_unlock(&g_dec_lock);
}

/* Cancel decode `id` (any thread): MAI_OK, and the owner gets nothing more
 * for it but its cancelled end; MAI_ERR_NOT_OPEN when it has ended (its
 * last message is already sent). */
MAI_HELPER int32_t mai__dec_cancel(int32_t id) {
    pthread_mutex_lock(&g_dec_lock);
    mai_decode *d = mai__dec_find_locked(id);
    if (d) {
        d->cancelled = true;
        atomic_store_explicit(&d->stop, true, memory_order_relaxed);
        pthread_cond_signal(&d->acked);
    }
    pthread_mutex_unlock(&g_dec_lock);
    return d ? MAI_OK : MAI_ERR_NOT_OPEN;
}

/* The owner is done with `n` chunks of decode `id`: as many more may go out
 * (more acks than chunks sent count as all of them). MAI_ERR_NOT_OPEN when
 * it has ended. */
MAI_HELPER int32_t mai__dec_ack(int32_t id, int32_t n) {
    if (n <= 0) return MAI_ERR_INVALID;
    pthread_mutex_lock(&g_dec_lock);
    mai_decode *d = mai__dec_find_locked(id);
    if (d) {
        d->inflight = d->inflight > n ? d->inflight - n : 0;
        pthread_cond_signal(&d->acked);
    }
    pthread_mutex_unlock(&g_dec_lock);
    return d ? MAI_OK : MAI_ERR_NOT_OPEN;
}

/* (the worker) Cancelled, or the owner is gone: the platform's loop polls
 * it every codec buffer and stops. */
MAI_HELPER bool mai__dec_stopping(mai_decode *d) { return atomic_load_explicit(&d->stop, memory_order_relaxed); }

/* A MAI_DEC_* push result as the decode's error atom ("cancelled" for
 * MAI_DEC_STOP), with a detail where it needs one. */
MAI_HELPER const char *mai__dec_reason(int r, char *detail, size_t detail_n) {
    switch (r) {
    case MAI_DEC_STOP: return "cancelled";
    case MAI_DEC_TOO_LONG: return "too_long";
    case MAI_DEC_NO_MEMORY: return "no_memory";
    default:
        if (detail && detail_n) snprintf(detail, detail_n, "no rate or channel count, or it changed mid-file");
        return "decode_error";
    }
}

/* Hand the filled part of the chunk on: to the processor's capture, or to
 * the owner once fewer than `window` chunks are unacknowledged (waiting for
 * an ack, or a cancel). */
static int mai__dec_deliver(mai_decode *d) {
    int32_t n = d->buf_n;
    if (n == 0) return MAI_DEC_GO;
    if (mai__dec_stopping(d)) return MAI_DEC_STOP;
    if (d->entry) {
        if (d->p.capture) {
            mai_time t = {d->frames - n, mai_now_ns(), 0, d->rate, d->channels};
            d->p.capture(d->ctx, d->buf, n, &t);
        }
        d->buf_n = 0;
        return MAI_DEC_GO;
    }
    pthread_mutex_lock(&g_dec_lock);
    while (!d->cancelled && d->inflight >= d->window) pthread_cond_wait(&d->acked, &g_dec_lock);
    bool ok = !d->cancelled;
    if (ok) {
        ok = d->sink->chunk(d, d->buf, n);
        d->buf = NULL;
        d->buf_n = 0;
        if (ok) d->inflight++;
    }
    pthread_mutex_unlock(&g_dec_lock);
    return ok ? MAI_DEC_GO : MAI_DEC_STOP;
}

/* (the worker, once) The source's format: its rate, its channel count and
 * the length it states in frames (<= 0: unknown). The owner learns it
 * (started) and the processor gets STARTED; MAI_DEC_TOO_LONG at once when
 * the stated length is past max_ms. */
MAI_HELPER int mai__dec_begin(mai_decode *d, int32_t rate, int32_t src_channels, int64_t declared_frames) {
    if (d->begun || rate <= 0 || src_channels <= 0) return MAI_DEC_FORMAT;
    d->rate = rate;
    d->src_channels = src_channels;
    d->channels = d->mono ? 1 : src_channels;
    d->declared_frames = declared_frames > 0 ? declared_frames : -1;
    d->max_frames = d->max_ms > 0 ? d->max_ms * rate / 1000 : 0;
    if (d->entry) {
        d->buf = malloc(sizeof(float) * (size_t)d->chunk_frames * (size_t)d->channels);
        if (!d->buf) return MAI_DEC_NO_MEMORY;
    }
    pthread_mutex_lock(&g_dec_lock);
    bool ok = !d->cancelled && d->sink->started(d);
    pthread_mutex_unlock(&g_dec_lock);
    if (!ok) return MAI_DEC_STOP;
    d->begun = true;
    if (d->entry && d->p.stream_event) {
        mai_time t = {0, mai_now_ns(), 0, d->rate, d->channels};
        d->p.stream_event(d->ctx, d->id, MAI_EVENT_STARTED, &t);
    }
    if (d->max_frames && d->declared_frames > d->max_frames) return MAI_DEC_TOO_LONG;
    return MAI_DEC_GO;
}

enum { MAI_DEC_F32 = 0, MAI_DEC_S16 = 1 };

static inline float mai__dec_sample(const void *x, size_t i, int fmt) {
    return fmt == MAI_DEC_S16 ? ((const int16_t *)x)[i] / 32768.0f : ((const float *)x)[i];
}

/* `frames` interleaved frames of the begun format into chunks. */
static int mai__dec_push(mai_decode *d, const void *x, size_t frames, int32_t src_channels, int fmt) {
    if (!d->begun || src_channels != d->src_channels) return MAI_DEC_FORMAT;
    const size_t ch = (size_t)src_channels;
    size_t at = 0; /* the next source sample */
    while (frames > 0) {
        if (mai__dec_stopping(d)) return MAI_DEC_STOP;
        if (!d->buf && !(d->buf = d->sink->buffer(d, (size_t)d->chunk_frames * (size_t)d->channels)))
            return MAI_DEC_NO_MEMORY;
        size_t room = (size_t)(d->chunk_frames - d->buf_n);
        size_t n = frames < room ? frames : room;
        if (d->max_frames && d->frames + (int64_t)n > d->max_frames) return MAI_DEC_TOO_LONG;
        float *o = d->buf + (size_t)d->buf_n * (size_t)d->channels;
        if (d->mono) {
            for (size_t f = 0; f < n; f++, at += ch) {
                float sum = 0.0f;
                for (size_t c = 0; c < ch; c++) sum += mai__dec_sample(x, at + c, fmt);
                o[f] = sum / (float)ch;
            }
        } else if (fmt == MAI_DEC_F32) {
            memcpy(o, (const float *)x + at, n * ch * sizeof(float));
            at += n * ch;
        } else {
            for (size_t i = 0; i < n * ch; i++, at++) o[i] = mai__dec_sample(x, at, fmt);
        }
        d->buf_n += (int32_t)n;
        d->frames += (int64_t)n;
        frames -= n;
        if (d->buf_n == d->chunk_frames) {
            int r = mai__dec_deliver(d);
            if (r != MAI_DEC_GO) return r;
        }
    }
    return MAI_DEC_GO;
}

/* Append interleaved float or 16-bit frames of `src_channels`. */
MAI_HELPER int mai__dec_push_f32(mai_decode *d, const float *x, size_t frames, int32_t src_channels) {
    return mai__dec_push(d, x, frames, src_channels, MAI_DEC_F32);
}

MAI_HELPER int mai__dec_push_s16(mai_decode *d, const int16_t *x, size_t frames, int32_t src_channels) {
    return mai__dec_push(d, x, frames, src_channels, MAI_DEC_S16);
}

/* (the worker's last call) `reason` NULL: the file ended, the rest goes
 * out; else the error atom that stopped the decode ("cancelled" for a
 * push's MAI_DEC_STOP) and its `detail`. The processor gets STOPPED (after
 * every STARTED), the processor is unpinned and the owner gets the end: a
 * cancel wins over the rest. The slot is free once this returns. */
MAI_HELPER void mai__dec_end(mai_decode *d, const char *reason, const char *detail) {
    char own[MAI_ERRMAX] = "";
    if (!reason) {
        int r = mai__dec_deliver(d);
        if (r != MAI_DEC_GO) {
            reason = mai__dec_reason(r, own, sizeof own);
            detail = own;
        } else if (d->frames == 0) {
            reason = "no_audio";
        }
    }
    /* the chunk left unsent (a failure, a cancel) doesn't count: frames
     * is now what the owner or the processor got */
    d->frames -= d->buf_n;
    d->buf_n = 0;
    if (d->entry) {
        if (d->begun && d->p.stream_event) {
            mai_time t = {d->frames, mai_now_ns(), 0, d->rate, d->channels};
            d->p.stream_event(d->ctx, d->id, MAI_EVENT_STOPPED, &t);
        }
        free(d->buf);
    } else if (d->buf) {
        d->sink->release(d, d->buf);
    }
    d->buf = NULL;
    mai__unpin(&d->entry);
    pthread_mutex_lock(&g_dec_lock);
    int how = d->cancelled || (reason && strcmp(reason, "cancelled") == 0) ? MAI_DEC_CANCELLED
              : reason                                                     ? MAI_DEC_FAILED
                                                                           : MAI_DEC_DONE;
    d->sink->ended(d, how, reason, detail ? detail : "");
    pthread_cond_destroy(&d->acked);
    d->used = false;
    pthread_mutex_unlock(&g_dec_lock);
}
