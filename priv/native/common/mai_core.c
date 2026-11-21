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
 *   render     mai__render wraps the processor's render with the stream
 *              clock and the counters status/0 reports (callbacks, worst
 *              callback time, worst gap between callbacks).
 *   posts      per stream, a single-producer single-consumer ring the audio
 *              thread writes (mai_post) and the event thread drains.
 *
 * mai_nif.c includes this file; the platform files include mai_nif.c.
 */
#include <math.h>
#include <pthread.h>
#include <stdatomic.h>
#include <stdbool.h>
#include <stdint.h>
#include <string.h>
#include <time.h>

#include "../../include/mob_audio_io.h"

#define MAI_MAX_PROCESSORS 16
#define MAI_MAX_STREAMS 8   /* slot index: the id's low 3 bits */
#define MAI_SLOT_BITS 3
#define MAI_POSTS 64        /* per stream, a power of two */
#define MAI_ERRMAX 96

/* Pending work for the event thread, set from any thread (atomic or). */
#define MAI_PF_DEVICE_LOST 0x01u  /* Android: the stream disconnected */
#define MAI_PF_INTERRUPTED 0x02u  /* iOS: interruption began */
#define MAI_PF_RESUMED 0x04u      /* iOS: interruption ended */
#define MAI_PF_ROUTE 0x08u        /* a new route */
#define MAI_PF_RESET 0x10u        /* iOS: media services were reset */
#define MAI_PF_OWNER_DOWN 0x20u   /* the owner exited: close */

enum { MAI_SHARING_SHARED = 0, MAI_SHARING_EXCLUSIVE = 1 };
enum { MAI_SESSION_PLAY = 0, MAI_SESSION_MIX = 1, MAI_SESSION_PLAY_AND_RECORD = 2 };

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
    mai_processor p;  /* copies of entry->p / ctx (render never touches the registry) */
    void *ctx;
    /* What the opener asked for. */
    int32_t want_rate;   /* 0: the device's */
    int32_t want_frames; /* 0: low latency (2 bursts / a 5 ms IO buffer) */
    int sharing, session;
    /* The platform stream, under the plugin's lock. */
    void *be;             /* AAudioStream * / AudioUnit; NULL while closed */
    bool running;         /* be is open and started */
    bool interrupted;     /* iOS: closed by an interruption, reopened at its end */
    int32_t rate, channels, burst, buffer, capacity, device_id;
    bool exclusive;
    int32_t xruns_seen;   /* the last xrun count the event thread reported */
    int64_t next_reopen_ns;
    char last_error[MAI_ERRMAX];
    /* The audio thread's own (written only by mai__render while running). */
    int64_t frame;
    int64_t last_cb_ns;
    double last_sample_time, last_frames; /* iOS: gaps in the device's sample time */
    /* Counters, written by the audio thread, read by status. */
    _Atomic uint64_t callbacks, frames_total, cb_total_ns;
    _Atomic uint32_t cb_max_ns, gap_max_ns, cb_frames_min, cb_frames_max;
    _Atomic uint32_t be_xruns; /* iOS: gaps found by the callback */
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

static int64_t mai__now_ns(void) {
    struct timespec ts;
    clock_gettime(CLOCK_MONOTONIC, &ts);
    return (int64_t)ts.tv_sec * 1000000000LL + ts.tv_nsec;
}

static void mai__set_error(mai_stream *s, const char *what) {
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

/* "mai_silence": what the self-test opens (nothing audible). */
static void mai__silence_render(void *ctx, float *out, int32_t frames, const mai_time *t) {
    (void)ctx; (void)out; (void)frames; (void)t;
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
    static const mai_processor silence = {mai__silence_render, NULL, NULL};
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

static mai_time mai__time(const mai_stream *s) {
    mai_time t = {s->frame, mai__now_ns(), s->rate, s->channels};
    return t;
}

static void mai__event(mai_stream *s, int32_t event) {
    if (!s->p.stream_event) return;
    mai_time t = mai__time(s);
    s->p.stream_event(s->ctx, s->id, event, &t);
}

/* Before a (re)start: the callback's own state. The stream clock goes on. */
static void mai__reset_callback_state(mai_stream *s) {
    s->last_cb_ns = 0;
    s->last_sample_time = -1;
}

static void mai__max_u32(_Atomic uint32_t *a, uint32_t v) {
    if (v > atomic_load_explicit(a, memory_order_relaxed)) atomic_store_explicit(a, v, memory_order_relaxed);
}

static void mai__min_u32(_Atomic uint32_t *a, uint32_t v) {
    if (v < atomic_load_explicit(a, memory_order_relaxed)) atomic_store_explicit(a, v, memory_order_relaxed);
}

static uint32_t mai__clamp_u32(int64_t v) { return v < 0 ? 0 : v > UINT32_MAX ? UINT32_MAX : (uint32_t)v; }

/* The audio callback: `out` holds frames * channels floats. */
static void mai__render(mai_stream *s, float *out, int32_t frames) {
    int64_t t0 = mai__now_ns();
    if (s->last_cb_ns) mai__max_u32(&s->gap_max_ns, mai__clamp_u32(t0 - s->last_cb_ns));
    s->last_cb_ns = t0;
    memset(out, 0, sizeof(float) * (size_t)frames * (size_t)s->channels);
    if (s->p.render) {
        mai_time t = {s->frame, t0, s->rate, s->channels};
        s->p.render(s->ctx, out, frames, &t);
    }
    s->frame += frames;
    int64_t dt = mai__now_ns() - t0;
    mai__max_u32(&s->cb_max_ns, mai__clamp_u32(dt));
    mai__min_u32(&s->cb_frames_min, (uint32_t)frames);
    mai__max_u32(&s->cb_frames_max, (uint32_t)frames);
    atomic_fetch_add_explicit(&s->cb_total_ns, (uint64_t)dt, memory_order_relaxed);
    atomic_fetch_add_explicit(&s->frames_total, (uint64_t)frames, memory_order_relaxed);
    atomic_fetch_add_explicit(&s->callbacks, 1, memory_order_release);
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
