/*
 * Host test of priv/native/common/mai_core.c: the processor registry, the
 * callback wrapper (clock, capture/render order, counters, the input
 * meter), the mai_post ring (including a two-thread stress run), the timing
 * math (frame times, host ticks to ns, the anchor seqlock under a racing
 * writer) and the PCM buffer decoders write into. Run by
 * test/mob_audio_io_c_core_test.exs (cc on the PATH).
 */
#include <stdio.h>
#include <stdlib.h>
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

/* ── PCM ───────────────────────────────────────────────────────────────── */

static int grows;

static bool host_grow(mai_pcm *p, size_t bytes) {
    float *d = realloc(p->data, bytes);
    if (!d) return false;
    p->data = d;
    p->cap = bytes;
    grows++;
    return true;
}

static bool failing_grow(mai_pcm *p, size_t bytes) {
    (void)p; (void)bytes;
    return false;
}

static void test_pcm(void) {
    /* stereo floats, grown by doubling */
    mai_pcm p = {.max_bytes = 1 << 20, .grow = host_grow};
    float st[2 * 1000];
    for (int i = 0; i < 2 * 1000; i++) st[i] = (float)i;
    grows = 0;
    for (int k = 0; k < 20; k++) CHECK(mai__pcm_push_f32(&p, st, 1000, 2) == MAI_PCM_OK);
    CHECK(p.frames == 20000 && p.channels == 2);
    CHECK(grows <= 4); /* 64 KiB doubling to 160 KB */
    CHECK(p.data[0] == 0.0f && p.data[2 * 19000 + 1999] == 1999.0f);
    /* a channel count that changes mid-file */
    CHECK(mai__pcm_push_f32(&p, st, 10, 1) == MAI_PCM_CHANNELS);
    free(p.data);

    /* mono folding from 16-bit stereo */
    mai_pcm m = {.max_bytes = 1 << 20, .mono = true, .grow = host_grow};
    int16_t s16[4] = {16384, -16384, 32767, 32767};
    CHECK(mai__pcm_push_s16(&m, s16, 2, 2) == MAI_PCM_OK);
    CHECK(m.channels == 1 && m.frames == 2);
    CHECK(m.data[0] == 0.0f && fabsf(m.data[1] - 32767 / 32768.0f) < 1e-6f);
    /* mono accepts any source channel count */
    CHECK(mai__pcm_push_s16(&m, s16, 1, 4) == MAI_PCM_OK && m.frames == 3);
    free(m.data);

    /* 16-bit stereo kept stereo */
    mai_pcm k = {.max_bytes = 1 << 20, .grow = host_grow};
    CHECK(mai__pcm_push_s16(&k, s16, 2, 2) == MAI_PCM_OK);
    CHECK(k.data[0] == 0.5f && k.data[1] == -0.5f);
    free(k.data);

    /* the cap: exactly max_bytes fits, one frame more doesn't; a cap
     * under the first allocation is honoured */
    mai_pcm c = {.max_bytes = 400, .grow = host_grow};
    float mono[100] = {0};
    CHECK(mai__pcm_push_f32(&c, mono, 100, 1) == MAI_PCM_OK);
    CHECK(c.cap == 400);
    CHECK(mai__pcm_push_f32(&c, mono, 1, 1) == MAI_PCM_TOO_LARGE);
    CHECK(c.frames == 100);
    free(c.data);

    /* the allocator saying no */
    mai_pcm f = {.max_bytes = 1 << 20, .grow = failing_grow};
    CHECK(mai__pcm_push_f32(&f, mono, 10, 1) == MAI_PCM_NO_MEMORY);
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
    test_pcm();
    if (failures) {
        fprintf(stderr, "%d check(s) failed\n", failures);
        return 1;
    }
    printf("mai_core_test: all checks passed\n");
    return 0;
}
