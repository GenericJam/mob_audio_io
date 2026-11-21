/*
 * Host test of priv/native/common/mai_core.c: the processor registry, the
 * callback wrapper (clock, capture/render order, counters, the input
 * meter), the mai_post ring (including a two-thread stress run), the timing
 * math (frame times, host ticks to ns, the anchor seqlock under a racing
 * writer) and the streaming decode's state machine (chunks, mono fold,
 * length cap, back-pressure, cancel, processors). Run by
 * test/mob_audio_io_c_core_test.exs (cc on the PATH).
 */
#include <stdio.h>
#include <stdlib.h>
#include <unistd.h>
#ifdef __APPLE__
#include <mach/mach_time.h>
#endif

#include "../../priv/native/common/mai_core.c"

static int failures = 0;

#define CHECK(cond)                                                                      \
    do {                                                                                 \
        if (!(cond)) {                                                                   \
            fprintf(stderr, "%s:%d: CHECK failed: %s\n", __FILE__, __LINE__, #cond);     \
            failures++;                                                                  \
        }                                                                                \
    } while (0)

/* ── A probe processor ─────────────────────────────────────────────────── */

typedef struct {
    int renders, captures;
    int order[8]; /* 'c' / 'r' per call, the first 8 */
    int n_order;
    int64_t last_frame, last_present, last_in_present;
    int32_t last_rate, last_channels, last_in_channels;
    int zeroed;
    float first_in;
    int events[8];
    int n_events;
    int32_t stream;
} probe;

static void probe_render(void *ctx, float *out, int32_t frames, const mai_time *t) {
    probe *p = ctx;
    p->zeroed = 1;
    for (int32_t i = 0; i < frames * t->channels; i++)
        if (out[i] != 0.0f) p->zeroed = 0;
    for (int32_t i = 0; i < frames * t->channels; i++) out[i] = 0.5f;
    p->renders++;
    if (p->n_order < 8) p->order[p->n_order++] = 'r';
    p->last_frame = t->frame;
    p->last_present = t->present_ns;
    p->last_rate = t->rate;
    p->last_channels = t->channels;
    uint8_t msg[4] = {1, 2, 3, (uint8_t)p->renders};
    mai_post(p->stream, msg, sizeof msg);
}

static void probe_capture(void *ctx, const float *in, int32_t frames, const mai_time *t) {
    probe *p = ctx;
    (void)frames;
    p->captures++;
    if (p->n_order < 8) p->order[p->n_order++] = 'c';
    p->first_in = in[0];
    p->last_in_present = t->present_ns;
    p->last_in_channels = t->channels;
}

static void probe_event(void *ctx, int32_t stream, int32_t event, const mai_time *t) {
    probe *p = ctx;
    (void)t;
    if (p->n_events < 8) p->events[p->n_events++] = event;
    p->stream = stream;
}

static const mai_processor PROBE = {probe_render, probe_capture, probe_event};

/* A stream as mai_nif.c opens one, without a platform. */
static mai_stream *open_stream(const char *processor, int dir, int32_t rate, int32_t ch, int32_t in_ch) {
    mai_stream *s = mai__alloc_stream();
    if (!s) return NULL;
    s->dir = dir;
    s->rate = rate;
    s->channels = ch;
    s->in_channels = in_ch;
    if (mai__acquire(s, processor) != MAI_OK) return NULL;
    atomic_store(&s->open, true);
    mai__reset_callback_state(s);
    mai__event(s, MAI_EVENT_STARTED);
    return s;
}

static void close_stream(mai_stream *s) {
    mai__event(s, MAI_EVENT_STOPPED);
    atomic_store(&s->open, false);
    mai__release(s);
}

/* ── Registry ──────────────────────────────────────────────────────────── */

static void test_registry(void) {
    probe p = {0};
    CHECK(mai_abi_version() == MAI_ABI_VERSION);
    CHECK(mai_register(NULL, &PROBE, &p) == MAI_ERR_INVALID);
    CHECK(mai_register("", &PROBE, &p) == MAI_ERR_INVALID);
    char long_name[MAI_NAME_MAX + 1];
    memset(long_name, 'x', MAI_NAME_MAX);
    long_name[MAI_NAME_MAX] = 0;
    CHECK(mai_register(long_name, &PROBE, &p) == MAI_ERR_INVALID);
    CHECK(mai_register("probe", NULL, &p) == MAI_ERR_INVALID);

    mai__register_builtins();
    mai__register_builtins(); /* idempotent */
    CHECK(mai_register("probe", &PROBE, &p) == MAI_OK);
    CHECK(mai_register("probe", &PROBE, &p) == MAI_OK); /* replace while unused */

    char names[MAI_MAX_PROCESSORS][MAI_NAME_MAX];
    int n = mai__processor_names(names, MAI_MAX_PROCESSORS);
    CHECK(n == 3);
    CHECK(strcmp(names[0], "mai_silence") == 0 && strcmp(names[1], "mai_sine") == 0 && strcmp(names[2], "probe") == 0);

    /* busy while a stream pins it */
    mai_stream *s = open_stream("probe", MAI_DIR_OUT, 48000, 2, 1);
    CHECK(s != NULL);
    CHECK(mai_register("probe", &PROBE, &p) == MAI_ERR_BUSY);
    CHECK(mai_unregister("probe") == MAI_ERR_BUSY);
    close_stream(s);
    CHECK(mai_unregister("probe") == MAI_OK);
    CHECK(mai_unregister("probe") == MAI_ERR_NOT_FOUND);

    /* full */
    char name[16];
    int added = 0;
    for (int i = 0; i < MAI_MAX_PROCESSORS + 2; i++) {
        snprintf(name, sizeof name, "fill%d", i);
        if (mai_register(name, &PROBE, &p) == MAI_OK) added++;
        else CHECK(mai_register(name, &PROBE, &p) == MAI_ERR_FULL);
    }
    CHECK(added == MAI_MAX_PROCESSORS - 2);
    for (int i = 0; i < MAI_MAX_PROCESSORS + 2; i++) {
        snprintf(name, sizeof name, "fill%d", i);
        mai_unregister(name);
    }
}

/* ── The callback ──────────────────────────────────────────────────────── */

static void test_output(void) {
    probe p = {0};
    CHECK(mai_register("probe", &PROBE, &p) == MAI_OK);
    mai_stream *s = open_stream("probe", MAI_DIR_OUT, 48000, 2, 1);
    CHECK(p.n_events == 1 && p.events[0] == MAI_EVENT_STARTED && p.stream == s->id);

    float buf[2 * 240];
    for (int i = 0; i < 2 * 240; i++) buf[i] = 9.0f; /* garbage the plugin must clear */
    int64_t t0 = mai_now_ns();
    mai__process(s, NULL, buf, 192, t0, 0, t0 + 5000000);
    CHECK(p.zeroed == 1 && p.renders == 1 && p.captures == 0);
    CHECK(p.last_frame == 0 && p.last_rate == 48000 && p.last_channels == 2);
    CHECK(p.last_present == t0 + 5000000);
    CHECK(buf[0] == 0.5f && buf[2 * 192 - 1] == 0.5f);
    mai__process(s, NULL, buf, 240, mai_now_ns(), 0, 0);
    CHECK(p.last_frame == 192);
    CHECK(atomic_load(&s->callbacks) == 2 && atomic_load(&s->frames_total) == 432);
    CHECK(atomic_load(&s->cb_frames_last) == 240);
    CHECK(atomic_load(&s->cb_frames_min) == 192 && atomic_load(&s->cb_frames_max) == 240);
    CHECK(atomic_load(&s->gap_max_ns) > 0);

    /* posts: one per render, in order, then empty */
    mai_msg m;
    CHECK(mai__pop_post(s, &m) && m.len == 4 && m.data[3] == 1);
    CHECK(mai__pop_post(s, &m) && m.data[3] == 2);
    CHECK(!mai__pop_post(s, &m));

    /* a reopen: the stream clock goes on, the gap tracking restarts */
    mai__reset_callback_state(s);
    CHECK(s->last_cb_ns == 0 && s->be_frame0 == 432);
    mai__process(s, NULL, buf, 96, mai_now_ns(), 0, 0);
    CHECK(p.last_frame == 432);

    mai__reset_stats(s);
    CHECK(atomic_load(&s->cb_max_ns) == 0 && atomic_load(&s->cb_frames_min) == UINT32_MAX);
    CHECK(atomic_load(&s->callbacks) == 3);

    close_stream(s);
    CHECK(p.events[p.n_events - 1] == MAI_EVENT_STOPPED);
    CHECK(mai_post(s->id, "x", 1) == MAI_ERR_NOT_OPEN); /* a closed id */
    mai_unregister("probe");
}

static void test_input_and_duplex(void) {
    probe p = {0};
    CHECK(mai_register("probe", &PROBE, &p) == MAI_OK);

    /* input: capture only, mono, metered */
    mai_stream *s = open_stream("probe", MAI_DIR_IN, 48000, 2, 1);
    float in[256];
    for (int i = 0; i < 256; i++) in[i] = (i % 2) ? 0.25f : -0.25f;
    in[10] = -0.75f;
    int64_t t0 = mai_now_ns();
    mai__process(s, in, NULL, 256, t0, t0 - 1000, 0);
    CHECK(p.captures == 1 && p.renders == 0);
    CHECK(p.first_in == -0.25f && p.last_in_channels == 1 && p.last_in_present == t0 - 1000);
    CHECK(mai__bits_float(atomic_load(&s->in_peak_bits)) == 0.75f);
    float rms = mai__bits_float(atomic_load(&s->in_rms_bits));
    CHECK(rms > 0.25f && rms < 0.26f);
    /* the peak holds until reset_stats, the RMS follows the last block */
    for (int i = 0; i < 256; i++) in[i] = 0.0f;
    mai__process(s, in, NULL, 256, mai_now_ns(), 0, 0);
    CHECK(mai__bits_float(atomic_load(&s->in_peak_bits)) == 0.75f);
    CHECK(mai__bits_float(atomic_load(&s->in_rms_bits)) == 0.0f);
    mai__reset_stats(s);
    CHECK(mai__bits_float(atomic_load(&s->in_peak_bits)) == 0.0f);
    /* a stream_event of an input stream carries the input's channels */
    mai_time et = mai__time(s);
    CHECK(et.channels == 1);
    close_stream(s);

    /* duplex: capture, then render, in each callback, same frame */
    memset(&p, 0, sizeof p);
    s = open_stream("probe", MAI_DIR_DUPLEX, 44100, 2, 2);
    float din[2 * 64], dout[2 * 64];
    for (int i = 0; i < 2 * 64; i++) din[i] = 0.1f;
    mai__process(s, din, dout, 64, mai_now_ns(), 0, 0);
    mai__process(s, din, dout, 64, mai_now_ns(), 0, 0);
    CHECK(p.n_order == 4 && p.order[0] == 'c' && p.order[1] == 'r' && p.order[2] == 'c' && p.order[3] == 'r');
    CHECK(p.last_frame == 64 && p.last_in_channels == 2 && p.last_channels == 2);
    close_stream(s);
    mai_unregister("probe");
}

static void test_sine(void) {
    mai__register_builtins();
    mai_stream *s = open_stream("mai_sine", MAI_DIR_OUT, 48000, 2, 1);
    float a[2 * 100], b[2 * 200];
    mai__process(s, NULL, a, 100, mai_now_ns(), 0, 0);
    mai__process(s, NULL, a, 100, mai_now_ns(), 0, 0);
    /* the second block continues the first: render 200 from 0 and compare */
    mai__reset_callback_state(s);
    s->frame = 0;
    mai__process(s, NULL, b, 200, mai_now_ns(), 0, 0);
    for (int i = 0; i < 2 * 100; i++) CHECK(fabsf(a[i] - b[200 + i]) < 1e-4f);
    float peak = 0;
    for (int i = 0; i < 2 * 200; i++) peak = fmaxf(peak, fabsf(b[i]));
    CHECK(peak > 0.12f && peak <= 0.125f);
    close_stream(s);
}

static void test_slots(void) {
    mai__register_builtins();
    mai_stream *all[MAI_MAX_STREAMS];
    for (int i = 0; i < MAI_MAX_STREAMS; i++) {
        all[i] = open_stream("mai_silence", MAI_DIR_OUT, 48000, 2, 1);
        CHECK(all[i] != NULL);
    }
    CHECK(mai__alloc_stream() == NULL);
    int32_t old = all[3]->id;
    close_stream(all[3]);
    mai_stream *again = open_stream("mai_silence", MAI_DIR_OUT, 48000, 2, 1);
    CHECK(again == all[3] && again->id != old);
    CHECK(mai__stream(old) == NULL && mai__stream(again->id) == again); /* a stale id never reaches the reused slot */
    for (int i = 0; i < MAI_MAX_STREAMS; i++) close_stream(all[i]);
}

/* ── Posts ─────────────────────────────────────────────────────────────── */

static void test_posts(void) {
    mai__register_builtins();
    mai_stream *s = open_stream("mai_silence", MAI_DIR_OUT, 48000, 2, 1);
    uint8_t big[MAI_POST_MAX + 1] = {0};
    CHECK(mai_post(s->id, big, sizeof big) == MAI_ERR_TOO_LARGE);
    CHECK(mai_post(s->id, NULL, 3) == MAI_ERR_INVALID);
    CHECK(mai_post(s->id, NULL, 0) == MAI_OK); /* an empty message is a message */
    CHECK(mai_post(s->id, big, MAI_POST_MAX) == MAI_OK);
    mai_msg m;
    CHECK(mai__pop_post(s, &m) && m.len == 0);
    CHECK(mai__pop_post(s, &m) && m.len == MAI_POST_MAX);

    /* full at MAI_POSTS, counted; wraps around the ring after draining */
    for (int round = 0; round < 3; round++) {
        for (uint32_t i = 0; i < MAI_POSTS; i++) CHECK(mai_post(s->id, &i, sizeof i) == MAI_OK);
        CHECK(mai_post(s->id, "x", 1) == MAI_ERR_FULL);
        for (uint32_t i = 0; i < MAI_POSTS; i++) {
            uint32_t v = 99;
            CHECK(mai__pop_post(s, &m));
            memcpy(&v, m.data, sizeof v);
            CHECK(v == i);
        }
        CHECK(!mai__pop_post(s, &m));
    }
    CHECK(atomic_load(&s->posts_dropped) == 3);
    close_stream(s);
}

/* One producer (the audio thread) and one consumer (the event thread):
 * every message arrives once, in order, or is counted as dropped. */
#define STRESS_N 200000

static void *stress_producer(void *arg) {
    int32_t id = *(int32_t *)arg;
    for (uint32_t i = 0; i < STRESS_N; i++) mai_post(id, &i, sizeof i);
    return NULL;
}

static void test_posts_two_threads(void) {
    mai_stream *s = open_stream("mai_silence", MAI_DIR_OUT, 48000, 2, 1);
    pthread_t t;
    pthread_create(&t, NULL, stress_producer, &s->id);
    uint32_t got = 0, last = 0;
    bool ordered = true, first = true;
    mai_msg m;
    for (;;) {
        if (mai__pop_post(s, &m)) {
            uint32_t v;
            memcpy(&v, m.data, sizeof v);
            if (!first && v <= last) ordered = false;
            first = false;
            last = v;
            got++;
            if (v == STRESS_N - 1) break;
        } else if (got + atomic_load(&s->posts_dropped) >= STRESS_N) {
            break; /* the last one was dropped */
        }
    }
    pthread_join(t, NULL);
    while (mai__pop_post(s, &m)) got++;
    CHECK(ordered);
    CHECK(got + atomic_load(&s->posts_dropped) == STRESS_N);
    close_stream(s);
}

/* ── Timing ────────────────────────────────────────────────────────────── */

static void test_timing_math(void) {
    /* frame times: exact at whole frames, both directions from the anchor */
    CHECK(mai__frame_time_ns(1000, 5000000000LL, 1000, 48000) == 5000000000LL);
    CHECK(mai__frame_time_ns(1000, 5000000000LL, 1000 + 48000, 48000) == 6000000000LL);
    CHECK(mai__frame_time_ns(1000, 5000000000LL, 1000 + 240, 48000) == 5005000000LL);
    CHECK(mai__frame_time_ns(48000, 5000000000LL, 0, 48000) == 4000000000LL);
    CHECK(mai__frame_time_ns(0, 0, 441, 44100) == 10000000LL);
    CHECK(mai__frame_time_ns(0, 7, 100, 0) == 0); /* no rate */
    /* a day of 192 kHz frames from the anchor doesn't overflow */
    int64_t day = 86400LL * 192000;
    CHECK(mai__frame_time_ns(0, 1, day, 192000) == 1 + 86400LL * 1000000000LL);

    /* mach ticks: an identity timebase, Apple silicon's 125/3, and ticks
     * big enough that ticks * numer would overflow 64 bits */
    CHECK(mai__ticks_to_ns(123456789, 1, 1) == 123456789);
    CHECK(mai__ticks_to_ns(24000000, 125, 3) == 1000000000LL); /* 24 MHz counter: 1 s */
    CHECK(mai__ticks_to_ns(3, 125, 3) == 125);
    CHECK(mai__ticks_to_ns(1, 125, 3) == 41);
    uint64_t big = 24000000ULL * 86400 * 365 * 10; /* ten years of a 24 MHz counter */
    CHECK(mai__ticks_to_ns(big, 125, 3) == (int64_t)(86400LL * 365 * 10 * 1000000000LL));

#ifdef __APPLE__
    /* the clock mai_now_ns reads is mach_absolute_time's, the clock of
     * AudioTimeStamp.mHostTime and mob_midi's timestamps (not Darwin's
     * CLOCK_MONOTONIC, which counts through sleep) */
    mach_timebase_info_data_t tb;
    mach_timebase_info(&tb);
    int64_t a = mai_now_ns();
    int64_t m = mai__ticks_to_ns(mach_absolute_time(), tb.numer, tb.denom);
    int64_t b = mai_now_ns();
    CHECK(m >= a - 1000 && m <= b + 1000);
#endif
    int64_t x = mai_now_ns(), y = mai_now_ns();
    CHECK(x > 0 && y >= x);
}

static void test_anchor(void) {
    mai_stream *s = open_stream("mai_silence", MAI_DIR_OUT, 48000, 2, 1);
    int64_t f, n;
    CHECK(!mai__get_anchor(s, &f, &n)); /* none yet */
    CHECK(mai__present_from_anchor(s, 100) == 0);
    mai__set_anchor(s, 4800, 2000000000LL);
    CHECK(mai__get_anchor(s, &f, &n) && f == 4800 && n == 2000000000LL);
    CHECK(mai__present_from_anchor(s, 9600) == 2100000000LL);
    mai__reset_callback_state(s); /* a new platform stream: the old anchor is gone */
    CHECK(mai__present_from_anchor(s, 9600) == 0);
    close_stream(s);
}

/* A writer publishing (k, k * 1000) pairs while a reader checks it never
 * sees a torn pair. */
static _Atomic bool anchor_stop;

static void *anchor_writer(void *arg) {
    mai_stream *s = arg;
    for (int64_t k = 1; !atomic_load(&anchor_stop); k++) mai__set_anchor(s, k, k * 1000);
    return NULL;
}

static void test_anchor_two_threads(void) {
    mai_stream *s = open_stream("mai_silence", MAI_DIR_OUT, 48000, 2, 1);
    atomic_store(&anchor_stop, false);
    pthread_t t;
    pthread_create(&t, NULL, anchor_writer, s);
    int torn = 0, seen = 0;
    for (int i = 0; i < 2000000; i++) {
        int64_t f, n;
        if (!mai__get_anchor(s, &f, &n)) continue;
        seen++;
        if (n != f * 1000) torn++;
    }
    atomic_store(&anchor_stop, true);
    pthread_join(t, NULL);
    CHECK(torn == 0);
    CHECK(seen > 0);
    close_stream(s);
}

/* ── Streaming decode ──────────────────────────────────────────────────── */

/* A sink standing in for the NIF's: records what the owner would get. A
 * chunk's buffer stays "held" (in the owner's mailbox) until the test acks
 * it, so `live` counts the chunk buffers in memory. The chunk/started/
 * ended fields are written under g_dec_lock (where the sink runs) and read
 * by the test under it too. */
typedef struct {
    int started, chunks, ended, how;
    char reason[32], detail[MAI_ERRMAX];
    int32_t rate, channels;
    int64_t declared, ended_frames;
    int32_t sizes[64];
    float samples[256];
    size_t n_samples;
    float *held[64];
    int n_held;
    bool refuse;             /* started/chunk answer false: the owner is gone */
    bool cancel_returned;    /* set by the test once mai__dec_cancel answered */
    int chunks_after_cancel; /* must stay 0 */
} rec;

static _Atomic int live, live_max;

static float *rec_buffer(mai_decode *d, size_t samples) {
    (void)d;
    int n = atomic_fetch_add(&live, 1) + 1;
    for (int m = atomic_load(&live_max); n > m && !atomic_compare_exchange_weak(&live_max, &m, n);)
        ;
    return malloc(samples * sizeof(float));
}

static void rec_free(float *buf) {
    free(buf);
    atomic_fetch_sub(&live, 1);
}

static void rec_release(mai_decode *d, float *buf) {
    (void)d;
    rec_free(buf);
}

static bool rec_started(mai_decode *d) {
    rec *r = d->user;
    r->started++;
    r->rate = d->rate;
    r->channels = d->channels;
    r->declared = d->declared_frames;
    return !r->refuse;
}

static bool rec_chunk(mai_decode *d, float *buf, int32_t frames) {
    rec *r = d->user;
    if (r->cancel_returned) r->chunks_after_cancel++;
    if (r->chunks < 64) r->sizes[r->chunks] = frames;
    r->chunks++;
    for (int32_t i = 0; i < frames * d->channels && r->n_samples < 256; i++) r->samples[r->n_samples++] = buf[i];
    if (r->refuse || r->n_held == 64) {
        rec_free(buf);
        return !r->refuse;
    }
    r->held[r->n_held++] = buf;
    return true;
}

static void rec_ended(mai_decode *d, int how, const char *reason, const char *detail) {
    rec *r = d->user;
    r->ended++;
    r->how = how;
    snprintf(r->reason, sizeof r->reason, "%s", reason ? reason : "");
    snprintf(r->detail, sizeof r->detail, "%s", detail);
    r->ended_frames = d->frames;
}

static const mai_dec_sink REC = {rec_buffer, rec_release, rec_started, rec_chunk, rec_ended};

/* The owner is done with (up to `max` of) the chunks it holds, oldest
 * first: free them, then ack. */
static void owner_ack(rec *r, int32_t id, int max) {
    pthread_mutex_lock(&g_dec_lock);
    int n = r->n_held < max ? r->n_held : max;
    for (int i = 0; i < n; i++) rec_free(r->held[i]);
    memmove(r->held, r->held + n, sizeof(float *) * (size_t)(r->n_held - n));
    r->n_held -= n;
    pthread_mutex_unlock(&g_dec_lock);
    if (n) mai__dec_ack(id, n);
}

static void owner_ack_all(rec *r, int32_t id) { owner_ack(r, id, 64); }

static mai_decode *new_decode(rec *r, bool mono, int32_t chunk_frames, int32_t window, int64_t max_ms) {
    memset(r, 0, sizeof *r);
    mai_decode *d = mai__dec_alloc();
    if (!d) return NULL;
    d->mono = mono;
    d->chunk_frames = chunk_frames;
    d->window = window;
    d->max_ms = max_ms;
    d->sink = &REC;
    d->user = r;
    return d;
}

static void test_decode_chunks(void) {
    rec r;
    /* 16-bit stereo folded to mono, in chunks of 4 frames: 4, 4, then the rest at the end */
    mai_decode *d = new_decode(&r, true, 4, 100, 0);
    int32_t id = d->id;
    CHECK(mai__dec_begin(d, 44100, 2, 10) == MAI_DEC_GO);
    CHECK(r.started == 1 && r.rate == 44100 && r.channels == 1 && r.declared == 10);
    int16_t s16[20];
    for (int i = 0; i < 10; i++) {
        s16[2 * i] = (int16_t)(i * 1000);
        s16[2 * i + 1] = (int16_t)(i * 3000);
    }
    CHECK(mai__dec_push_s16(d, s16, 10, 2) == MAI_DEC_GO);
    CHECK(r.chunks == 2 && r.sizes[0] == 4 && r.sizes[1] == 4);
    mai__dec_end(d, NULL, NULL);
    CHECK(r.chunks == 3 && r.sizes[2] == 2 && r.ended == 1 && r.how == MAI_DEC_DONE && r.ended_frames == 10);
    CHECK(r.n_samples == 10);
    for (int i = 0; i < 10 && i < (int)r.n_samples; i++) CHECK(fabsf(r.samples[i] - i * 2000 / 32768.0f) < 1e-6f);
    /* the slot is free: its id is unknown, no chunk buffer leaked */
    CHECK(mai__dec_cancel(id) == MAI_ERR_NOT_OPEN && mai__dec_ack(id, 1) == MAI_ERR_NOT_OPEN);
    owner_ack_all(&r, id);
    CHECK(atomic_load(&live) == 0);

    /* float stereo kept stereo; chunks of 3 frames across pushes; a stated length of 0 is unknown */
    d = new_decode(&r, false, 3, 100, 0);
    CHECK(mai__dec_begin(d, 48000, 2, 0) == MAI_DEC_GO && r.channels == 2 && r.declared == -1);
    float st[14];
    for (int i = 0; i < 14; i++) st[i] = (float)i;
    CHECK(mai__dec_push_f32(d, st, 2, 2) == MAI_DEC_GO && r.chunks == 0);
    CHECK(mai__dec_push_f32(d, st + 4, 5, 2) == MAI_DEC_GO && r.chunks == 2);
    id = d->id;
    mai__dec_end(d, NULL, NULL);
    CHECK(r.chunks == 3 && r.sizes[0] == 3 && r.sizes[1] == 3 && r.sizes[2] == 1 && r.how == MAI_DEC_DONE);
    CHECK(r.n_samples == 14);
    for (int i = 0; i < 14 && i < (int)r.n_samples; i++) CHECK(r.samples[i] == (float)i);
    owner_ack_all(&r, id);
    CHECK(atomic_load(&live) == 0);
}

static void test_decode_errors(void) {
    rec r;
    float x[8] = {0};

    /* the channel count changes mid-file; begin twice; no format */
    mai_decode *d = new_decode(&r, false, 4, 100, 0);
    CHECK(mai__dec_push_f32(d, x, 1, 1) == MAI_DEC_FORMAT); /* before begin */
    CHECK(mai__dec_begin(d, 0, 1, -1) == MAI_DEC_FORMAT && r.started == 0);
    CHECK(mai__dec_begin(d, 8000, 1, -1) == MAI_DEC_GO);
    CHECK(mai__dec_begin(d, 8000, 1, -1) == MAI_DEC_FORMAT);
    int dr = mai__dec_push_f32(d, x, 2, 2);
    CHECK(dr == MAI_DEC_FORMAT);
    char detail[MAI_ERRMAX] = "";
    const char *why = mai__dec_reason(dr, detail, sizeof detail);
    int32_t id = d->id;
    mai__dec_end(d, why, detail);
    CHECK(r.how == MAI_DEC_FAILED && strcmp(r.reason, "decode_error") == 0 && strstr(r.detail, "mid-file") != NULL);
    owner_ack_all(&r, id);

    /* nothing decoded: no_audio, whether or not the format came */
    d = new_decode(&r, false, 4, 100, 0);
    mai__dec_end(d, NULL, NULL);
    CHECK(r.how == MAI_DEC_FAILED && strcmp(r.reason, "no_audio") == 0 && r.started == 0);
    d = new_decode(&r, false, 4, 100, 0);
    CHECK(mai__dec_begin(d, 8000, 1, -1) == MAI_DEC_GO);
    mai__dec_end(d, NULL, NULL);
    CHECK(r.how == MAI_DEC_FAILED && strcmp(r.reason, "no_audio") == 0 && r.started == 1);

    /* max_ms: a stated length past it fails at once (after the format went
     * out); decoded frames past it fail at the push that crosses it */
    d = new_decode(&r, false, 4, 100, 1000);
    CHECK(mai__dec_begin(d, 100, 1, 101) == MAI_DEC_TOO_LONG && r.started == 1);
    mai__dec_end(d, mai__dec_reason(MAI_DEC_TOO_LONG, NULL, 0), "");
    CHECK(r.how == MAI_DEC_FAILED && strcmp(r.reason, "too_long") == 0);
    d = new_decode(&r, false, 4, 100, 1000);
    id = d->id;
    CHECK(mai__dec_begin(d, 100, 1, 100) == MAI_DEC_GO);
    float hundred[100] = {0};
    CHECK(mai__dec_push_f32(d, hundred, 100, 1) == MAI_DEC_GO);
    CHECK(mai__dec_push_f32(d, x, 1, 1) == MAI_DEC_TOO_LONG);
    mai__dec_end(d, "too_long", "");
    CHECK(r.how == MAI_DEC_FAILED && r.ended_frames == 100 && r.chunks == 25);
    owner_ack_all(&r, id);

    /* the owner is gone (the send fails): the decode stops as cancelled */
    d = new_decode(&r, false, 4, 100, 0);
    r.refuse = true;
    CHECK(mai__dec_begin(d, 8000, 1, -1) == MAI_DEC_STOP);
    mai__dec_end(d, mai__dec_reason(MAI_DEC_STOP, NULL, 0), "");
    CHECK(r.how == MAI_DEC_CANCELLED);
    d = new_decode(&r, false, 4, 100, 0);
    CHECK(mai__dec_begin(d, 8000, 1, -1) == MAI_DEC_GO);
    r.refuse = true;
    CHECK(mai__dec_push_f32(d, x, 8, 1) == MAI_DEC_STOP && r.chunks == 1);
    mai__dec_end(d, "cancelled", "");
    CHECK(r.how == MAI_DEC_CANCELLED);
    CHECK(atomic_load(&live) == 0);
}

static void test_decode_slots(void) {
    rec r[MAI_MAX_DECODES];
    mai_decode *d[MAI_MAX_DECODES];
    mai_stream *s = open_stream("mai_silence", MAI_DIR_OUT, 48000, 2, 1);
    for (int i = 0; i < MAI_MAX_DECODES; i++) {
        d[i] = new_decode(&r[i], false, 4, 1, 0);
        CHECK(d[i] && d[i]->id > 0 && d[i]->id != s->id);
        for (int j = 0; j < i; j++) CHECK(d[i] && d[j] && d[i]->id != d[j]->id);
    }
    rec extra;
    CHECK(new_decode(&extra, false, 4, 1, 0) == NULL); /* MAI_MAX_DECODES run */
    /* a decode id never reaches a stream, nor a stream id a decode */
    CHECK(mai_post(d[0]->id, "x", 1) == MAI_ERR_NOT_OPEN);
    CHECK(mai__dec_cancel(s->id) == MAI_ERR_NOT_OPEN && mai__dec_ack(0, 1) == MAI_ERR_NOT_OPEN);
    CHECK(mai__dec_ack(d[0]->id, 0) == MAI_ERR_INVALID);
    int32_t old = d[0]->id;
    mai__dec_discard(d[0]);
    CHECK(mai__dec_cancel(old) == MAI_ERR_NOT_OPEN);
    mai_decode *again = new_decode(&extra, false, 4, 1, 0);
    CHECK(again == d[0] && again->id != old); /* the slot, with a new id */
    mai__dec_discard(again);
    for (int i = 1; i < MAI_MAX_DECODES; i++) mai__dec_discard(d[i]);
    close_stream(s);
}

/* A cancel that comes before the end wins over it: the owner gets the
 * cancelled end, and the chunk still being filled isn't sent. */
static void test_decode_cancel_wins(void) {
    rec r;
    mai_decode *d = new_decode(&r, false, 4, 100, 0);
    int32_t id = d->id;
    float x[6] = {0};
    CHECK(mai__dec_begin(d, 8000, 1, -1) == MAI_DEC_GO);
    CHECK(mai__dec_push_f32(d, x, 6, 1) == MAI_DEC_GO && r.chunks == 1);
    CHECK(mai__dec_cancel(id) == MAI_OK);
    CHECK(mai__dec_stopping(d));
    CHECK(mai__dec_push_f32(d, x, 1, 1) == MAI_DEC_STOP);
    mai__dec_end(d, NULL, NULL);
    CHECK(r.how == MAI_DEC_CANCELLED && r.chunks == 1 && r.ended == 1);
    CHECK(mai__dec_cancel(id) == MAI_ERR_NOT_OPEN);
    owner_ack_all(&r, id);
    CHECK(atomic_load(&live) == 0);
}

/* A worker thread feeding codec buffers of 1024 frames until a push stops it. */
typedef struct {
    mai_decode *d;
    int64_t limit;            /* frames to push; 0: until stopped */
    _Atomic int64_t ended_ns; /* when mai__dec_end returned */
} worker;

static void *decode_worker(void *arg) {
    worker *w = arg;
    float buf[1024 * 2];
    for (int i = 0; i < 1024 * 2; i++) buf[i] = 0.25f;
    int r = mai__dec_begin(w->d, 48000, 2, -1);
    for (int64_t pushed = 0; r == MAI_DEC_GO && (w->limit == 0 || pushed < w->limit); pushed += 1024)
        r = mai__dec_push_f32(w->d, buf, 1024, 2);
    mai__dec_end(w->d, r == MAI_DEC_GO ? NULL : mai__dec_reason(r, NULL, 0), "");
    atomic_store(&w->ended_ns, mai_now_ns());
    return NULL;
}

static int read_locked(const int *v) {
    pthread_mutex_lock(&g_dec_lock);
    int x = *v;
    pthread_mutex_unlock(&g_dec_lock);
    return x;
}

static void cancel_marking(rec *r, int32_t id) {
    CHECK(mai__dec_cancel(id) == MAI_OK);
    pthread_mutex_lock(&g_dec_lock);
    r->cancel_returned = true;
    pthread_mutex_unlock(&g_dec_lock);
}

/* Back-pressure: with nobody acking, the worker sends `window` chunks and
 * waits; each ack lets one more go; the chunk buffers in memory never pass
 * window + 1 (those held, the one being filled). A cancel while it waits
 * ends it at once, and nothing reaches the owner after the cancel. */
static void test_decode_backpressure(void) {
    rec r;
    atomic_store(&live_max, 0);
    mai_decode *d = new_decode(&r, false, 256, 3, 0);
    int32_t id = d->id;
    worker w = {d, 0, 0};
    pthread_t t;
    CHECK(pthread_create(&t, NULL, decode_worker, &w) == 0);
    usleep(50000);
    CHECK(read_locked(&r.chunks) == 3 && atomic_load(&w.ended_ns) == 0);
    owner_ack(&r, id, 1);
    for (int i = 0; i < 1000 && read_locked(&r.chunks) < 4; i++) usleep(1000);
    usleep(20000);
    CHECK(read_locked(&r.chunks) == 4);
    /* acking everything as it comes: many more chunks, the bound holds */
    for (int i = 0; i < 200; i++) {
        owner_ack_all(&r, id);
        usleep(100);
    }
    CHECK(read_locked(&r.chunks) > 10);
    CHECK(atomic_load(&live_max) <= 3 + 1);
    usleep(20000); /* blocked again: nobody acks */
    int64_t t0 = mai_now_ns();
    cancel_marking(&r, id);
    pthread_join(t, NULL);
    int64_t latency_ns = atomic_load(&w.ended_ns) - t0;
    printf("decode cancel while waiting for an ack: ended %.3f ms after the cancel; at most %d chunk buffers live\n",
           latency_ns / 1e6, atomic_load(&live_max));
    CHECK(latency_ns < 50000000LL);
    CHECK(r.how == MAI_DEC_CANCELLED && r.chunks_after_cancel == 0);
    owner_ack_all(&r, id);
    CHECK(atomic_load(&live) == 0);
}

/* A cancel while the worker is busy pushing (the owner acks at once): it
 * stops at its next push, and no chunk reaches the owner after the cancel. */
static void test_decode_cancel_running(void) {
    rec r;
    mai_decode *d = new_decode(&r, true, 512, 1000000, 0);
    int32_t id = d->id;
    worker w = {d, 0, 0};
    pthread_t t;
    CHECK(pthread_create(&t, NULL, decode_worker, &w) == 0);
    for (int i = 0; i < 20; i++) {
        owner_ack_all(&r, id);
        usleep(1000);
    }
    int64_t t0 = mai_now_ns();
    cancel_marking(&r, id);
    pthread_join(t, NULL);
    int64_t latency_ns = atomic_load(&w.ended_ns) - t0;
    printf("decode cancel while decoding: ended %.3f ms after the cancel, %d chunks before it\n", latency_ns / 1e6,
           read_locked(&r.chunks));
    CHECK(latency_ns < 50000000LL);
    CHECK(r.how == MAI_DEC_CANCELLED && r.chunks > 0 && r.chunks_after_cancel == 0);
    owner_ack_all(&r, id);
    CHECK(atomic_load(&live) == 0);
}

/* A decode into a processor: STARTED, captures on the decode's thread with
 * the file's clock, STOPPED (after a cancel too); the processor is pinned
 * meanwhile; the owner gets the format and the end, no chunks. */
typedef struct {
    int events[4], n_events;
    int32_t event_stream;
    int64_t stop_frame;
    int captures;
    int64_t frames_seen, next_frame;
    bool clock_ok;
    int32_t rate, channels;
} dec_probe;

static void dec_probe_capture(void *ctx, const float *in, int32_t frames, const mai_time *t) {
    dec_probe *p = ctx;
    (void)in;
    if (t->frame != p->next_frame || t->rate != p->rate || t->channels != p->channels) p->clock_ok = false;
    p->next_frame = t->frame + frames;
    p->frames_seen += frames;
    p->captures++;
}

static void dec_probe_event(void *ctx, int32_t stream, int32_t event, const mai_time *t) {
    dec_probe *p = ctx;
    if (p->n_events < 4) p->events[p->n_events++] = event;
    p->event_stream = stream;
    if (event == MAI_EVENT_STARTED) {
        p->rate = t->rate;
        p->channels = t->channels;
        p->clock_ok = t->frame == 0;
    }
    if (event == MAI_EVENT_STOPPED) p->stop_frame = t->frame;
}

static void test_decode_processor(void) {
    static const mai_processor P = {NULL, dec_probe_capture, dec_probe_event};
    dec_probe p;
    memset(&p, 0, sizeof p);
    CHECK(mai_register("dec_probe", &P, &p) == MAI_OK);
    rec r;
    mai_decode *d = new_decode(&r, true, 100, 1, 0);
    CHECK(mai__dec_use_processor(d, "nope") == MAI_ERR_NOT_FOUND);
    CHECK(mai__dec_use_processor(d, "dec_probe") == MAI_OK);
    CHECK(mai_unregister("dec_probe") == MAI_ERR_BUSY);
    int32_t id = d->id;
    CHECK(mai__dec_begin(d, 22050, 2, 1000) == MAI_DEC_GO);
    CHECK(p.n_events == 1 && p.events[0] == MAI_EVENT_STARTED && p.event_stream == id);
    CHECK(p.rate == 22050 && p.channels == 1 && r.started == 1);
    int16_t s16[2 * 250] = {0};
    /* window 1 and no acks: a processor takes chunks without them */
    CHECK(mai__dec_push_s16(d, s16, 250, 2) == MAI_DEC_GO && p.captures == 2);
    mai__dec_end(d, NULL, NULL);
    CHECK(p.captures == 3 && p.frames_seen == 250 && p.clock_ok);
    CHECK(p.n_events == 2 && p.events[1] == MAI_EVENT_STOPPED && p.stop_frame == 250);
    CHECK(r.chunks == 0 && r.how == MAI_DEC_DONE && r.ended_frames == 250);
    CHECK(atomic_load(&live) == 0);

    /* cancelled: the next push stops, STOPPED still comes */
    memset(&p, 0, sizeof p);
    d = new_decode(&r, false, 100, 1, 0);
    CHECK(mai__dec_use_processor(d, "dec_probe") == MAI_OK);
    id = d->id;
    CHECK(mai__dec_begin(d, 8000, 1, -1) == MAI_DEC_GO);
    CHECK(mai__dec_cancel(id) == MAI_OK);
    float x[100] = {0};
    int dr = mai__dec_push_f32(d, x, 100, 1);
    CHECK(dr == MAI_DEC_STOP && p.captures == 0);
    mai__dec_end(d, mai__dec_reason(dr, NULL, 0), "");
    CHECK(p.n_events == 2 && p.events[1] == MAI_EVENT_STOPPED && r.how == MAI_DEC_CANCELLED);
    CHECK(mai_unregister("dec_probe") == MAI_OK);
}

int main(void) {
    test_registry();
    test_output();
    test_input_and_duplex();
    test_sine();
    test_slots();
    test_posts();
    test_posts_two_threads();
    test_timing_math();
    test_anchor();
    test_anchor_two_threads();
    test_decode_chunks();
    test_decode_errors();
    test_decode_slots();
    test_decode_cancel_wins();
    test_decode_backpressure();
    test_decode_cancel_running();
    test_decode_processor();
    if (failures) {
        fprintf(stderr, "%d check(s) failed\n", failures);
        return 1;
    }
    printf("mai_core_test: all checks passed\n");
    return 0;
}
