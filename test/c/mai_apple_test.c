/*
 * macOS host test of the iOS backend's plain-C half
 * (priv/native/ios/mai_apple.c) against real CoreAudio:
 *
 *   session   the AVAudioSession configuration the open streams need.
 *   output    the render callback driven by a real AudioUnit
 *             (kAudioUnitSubType_GenericOutput, rendered offline with
 *             AudioUnitRender): frames, channels, the output reaching the
 *             unit's output, present_ns from the AudioTimeStamp's host time
 *             on mai_now_ns()'s clock, xruns from sample-time gaps.
 *   input     the input callback pulling its frames with AudioUnitRender
 *             from a real unit (another GenericOutput standing in for
 *             RemoteIO's bus 1), the meter, a failing pull.
 *   duplex    one render: capture of the pulled input, then render, the
 *             same clock; a failing pull is an input underflow.
 *   decode    ExtAudioFile on a WAV written here, an AAC .m4a made with
 *             afconvert, an MP3 when the machine has an MP3 encoder (ffmpeg
 *             with libmp3lame, or lame; macOS has none of its own), and the
 *             errors.
 *   hardware  the same unit setup (EnableIO, formats, callbacks) on
 *             macOS's HALOutput playing silence on the default output
 *             device for 300 ms; skipped without one (CI runners). Input
 *             from hardware needs the microphone permission (TCC) and is
 *             not tried.
 *
 * clang -std=c11 -Wall -Wextra -Werror -Wno-unused-function test/c/mai_apple_test.c -o mai_apple_test \
 *   -framework AudioToolbox -framework CoreAudio -framework CoreFoundation
 * ./mai_apple_test [workdir]
 */
#include <stdio.h>
#include <stdlib.h>
#include <unistd.h>

#include "../../priv/native/common/mai_core.c"
#include "../../priv/native/ios/mai_apple.c"

#include <CoreAudio/CoreAudio.h>

static int failures = 0;

#define CHECK(cond)                                                                      \
    do {                                                                                 \
        if (!(cond)) {                                                                   \
            fprintf(stderr, "%s:%d: CHECK failed: %s\n", __FILE__, __LINE__, #cond);     \
            failures++;                                                                  \
        }                                                                                \
    } while (0)

#define RATE 48000.0

/* ── A probe processor ─────────────────────────────────────────────────── */

typedef struct {
    _Atomic int renders, captures;
    int order[4], n_order;
    int64_t frame_r, frame_c, host_r, host_c, present_r, present_c;
    int32_t frames_r, frames_c, channels_r, channels_c, rate_r;
    float in_first[4];
    double in_sum_sq;
    bool silent; /* render zeros (the hardware test) */
} probe;

static void probe_render(void *ctx, float *out, int32_t frames, const mai_time *t) {
    probe *p = ctx;
    if (p->n_order < 4) p->order[p->n_order++] = 'r';
    p->frame_r = t->frame;
    p->host_r = t->host_ns;
    p->present_r = t->present_ns;
    p->frames_r = frames;
    p->channels_r = t->channels;
    p->rate_r = t->rate;
    if (!p->silent)
        for (int32_t i = 0; i < frames; i++)
            for (int32_t c = 0; c < t->channels; c++) out[i * t->channels + c] = (float)(i + 1) / 1000.0f * (c ? -1 : 1);
    atomic_fetch_add(&p->renders, 1);
}

static void probe_capture(void *ctx, const float *in, int32_t frames, const mai_time *t) {
    probe *p = ctx;
    if (p->n_order < 4) p->order[p->n_order++] = 'c';
    p->frame_c = t->frame;
    p->host_c = t->host_ns;
    p->present_c = t->present_ns;
    p->frames_c = frames;
    p->channels_c = t->channels;
    memcpy(p->in_first, in, sizeof p->in_first);
    p->in_sum_sq = 0;
    for (int32_t i = 0; i < frames * t->channels; i++) p->in_sum_sq += (double)in[i] * in[i];
    atomic_fetch_add(&p->captures, 1);
}

static const mai_processor PROBE = {probe_render, probe_capture, NULL};

static mai_stream *open_stream(probe *p, int dir, int32_t ch, int32_t in_ch) {
    memset(p, 0, sizeof *p);
    mai_register("probe", &PROBE, p);
    mai_stream *s = mai__alloc_stream();
    s->dir = dir;
    s->channels = ch;
    s->in_channels = in_ch;
    s->rate = (int32_t)RATE;
    if (mai__acquire(s, "probe") != MAI_OK) abort();
    atomic_store(&s->open, true);
    mai__reset_callback_state(s);
    return s;
}

static void close_stream(mai_stream *s) {
    atomic_store(&s->open, false);
    mai__release(s);
}

/* ── A source unit: GenericOutput whose input is a sine ────────────────── */

typedef struct {
    AudioUnit unit;
    int32_t channels;
    double phase;
    bool fail;
    UInt32 short_by; /* frames to leave out (the simulator's 557-of-558) */
} source;

static OSStatus source_render(void *ref, AudioUnitRenderActionFlags *flags, const AudioTimeStamp *ts, UInt32 bus,
                              UInt32 n, AudioBufferList *io) {
    (void)flags; (void)ts; (void)bus;
    source *src = ref;
    if (src->fail) return kAudioUnitErr_CannotDoInCurrentContext;
    float *x = io->mBuffers[0].mData;
    for (UInt32 i = 0; i < n; i++) {
        float v = 0.5f * (float)sin(src->phase);
        src->phase += 2 * M_PI * 1000.0 / RATE;
        for (int32_t c = 0; c < src->channels; c++) x[i * (UInt32)src->channels + (UInt32)c] = v;
    }
    if (src->short_by) io->mBuffers[0].mDataByteSize = (n - src->short_by) * (UInt32)src->channels * sizeof(float);
    return noErr;
}

/* A GenericOutput unit with `channels` float interleaved on both sides, at RATE. */
static AudioUnit generic_unit(int32_t channels, AURenderCallback cb, void *ref, bool init) {
    AudioComponentDescription d = {kAudioUnitType_Output, kAudioUnitSubType_GenericOutput,
                                   kAudioUnitManufacturer_Apple, 0, 0};
    AudioUnit u = NULL;
    CHECK(AudioComponentInstanceNew(AudioComponentFindNext(NULL, &d), &u) == noErr);
    AudioStreamBasicDescription f = mai_apple_float_format(RATE, channels);
    CHECK(AudioUnitSetProperty(u, kAudioUnitProperty_StreamFormat, kAudioUnitScope_Output, 0, &f, sizeof f) == noErr);
    if (cb) {
        CHECK(AudioUnitSetProperty(u, kAudioUnitProperty_StreamFormat, kAudioUnitScope_Input, 0, &f, sizeof f) ==
              noErr);
        AURenderCallbackStruct rc = {cb, ref};
        CHECK(AudioUnitSetProperty(u, kAudioUnitProperty_SetRenderCallback, kAudioUnitScope_Input, 0, &rc,
                                   sizeof rc) == noErr);
    }
    if (init) CHECK(AudioUnitInitialize(u) == noErr);
    return u;
}

static AudioTimeStamp stamp(double sample_time, uint64_t host) {
    AudioTimeStamp ts;
    memset(&ts, 0, sizeof ts);
    ts.mSampleTime = sample_time;
    ts.mHostTime = host;
    ts.mFlags = kAudioTimeStampSampleTimeValid | kAudioTimeStampHostTimeValid;
    return ts;
}

/* ── Session ───────────────────────────────────────────────────────────── */

static mai_stream g_test_streams[4];

static void put_stream(int i, int dir, int session, int preset, int32_t frames, int32_t rate, uint32_t gen) {
    mai_stream *s = &g_test_streams[i];
    memset(s, 0, sizeof *s);
    atomic_store(&s->open, true);
    s->dir = dir;
    s->session = session;
    s->preset = preset;
    s->want_frames = frames;
    s->want_rate = rate;
    s->gen = gen;
}

static mai_apple_session policy(void) { return mai_apple_session_for(g_test_streams, 4, 48000); }

static void test_session(void) {
    memset(g_test_streams, 0, sizeof g_test_streams);
    mai_apple_session p = policy();
    CHECK(p.streams == 0 && !p.record && !p.mix);

    put_stream(0, MAI_DIR_OUT, MAI_SESSION_PLAY, MAI_PRESET_DEFAULT, 0, 0, 1);
    p = policy();
    CHECK(p.streams == 1 && !p.record && !p.mix && p.mode == MAI_APPLE_MODE_DEFAULT);
    CHECK(fabs(p.io_s - 0.005) < 1e-9 && p.rate == 0);

    put_stream(0, MAI_DIR_OUT, MAI_SESSION_MIX, MAI_PRESET_DEFAULT, 0, 0, 1);
    p = policy();
    CHECK(!p.record && p.mix);

    /* an :output :mix stream, then an input (its default :play_and_record) */
    put_stream(1, MAI_DIR_IN, MAI_SESSION_PLAY_AND_RECORD, MAI_PRESET_DEFAULT, 0, 0, 2);
    p = policy();
    CHECK(p.streams == 2 && p.record && !p.mix && p.mode == MAI_APPLE_MODE_DEFAULT);
    /* ... an input that asked for :mix too: mixes */
    g_test_streams[1].session = MAI_SESSION_MIX;
    p = policy();
    CHECK(p.record && p.mix);
    /* an output asking :play_and_record records without input */
    memset(&g_test_streams[1], 0, sizeof g_test_streams[1]);
    put_stream(0, MAI_DIR_OUT, MAI_SESSION_PLAY_AND_RECORD, MAI_PRESET_DEFAULT, 0, 0, 1);
    p = policy();
    CHECK(p.record && !p.mix);

    /* presets: input only; voice over unprocessed */
    put_stream(0, MAI_DIR_OUT, MAI_SESSION_PLAY, MAI_PRESET_VOICE, 0, 0, 1);
    CHECK(policy().mode == MAI_APPLE_MODE_DEFAULT);
    put_stream(1, MAI_DIR_IN, MAI_SESSION_PLAY_AND_RECORD, MAI_PRESET_UNPROCESSED, 0, 0, 2);
    CHECK(policy().mode == MAI_APPLE_MODE_MEASUREMENT);
    put_stream(2, MAI_DIR_DUPLEX, MAI_SESSION_PLAY_AND_RECORD, MAI_PRESET_VOICE, 0, 0, 3);
    CHECK(policy().mode == MAI_APPLE_MODE_VOICE_CHAT);
    atomic_store(&g_test_streams[2].open, false); /* closed: back to Measurement */
    CHECK(policy().mode == MAI_APPLE_MODE_MEASUREMENT && policy().streams == 2);

    /* IO buffer: the shortest asked for; rate: the newest stream's */
    memset(g_test_streams, 0, sizeof g_test_streams);
    put_stream(0, MAI_DIR_OUT, MAI_SESSION_PLAY, MAI_PRESET_DEFAULT, 512, 44100, 9);
    p = policy();
    CHECK(fabs(p.io_s - 512 / 48000.0) < 1e-9 && p.rate == 44100);
    put_stream(1, MAI_DIR_OUT, MAI_SESSION_PLAY, MAI_PRESET_DEFAULT, 0, 48000, 12);
    put_stream(2, MAI_DIR_OUT, MAI_SESSION_PLAY, MAI_PRESET_DEFAULT, 128, 0, 15);
    p = policy();
    CHECK(fabs(p.io_s - 128 / 48000.0) < 1e-9 && p.rate == 48000);
    mai_apple_session q = p;
    CHECK(mai_apple_session_eq(&p, &q));
    q.mix = !q.mix;
    CHECK(!mai_apple_session_eq(&p, &q));
}

/* ── Output ────────────────────────────────────────────────────────────── */

static void test_output(void) {
    probe p;
    mai_stream *s = open_stream(&p, MAI_DIR_OUT, 2, 1);
    atomic_store(&s->out_latency_ns, 7000000);
    mai_apple_io *io = mai_apple_io_new(s);
    char err[MAI_ERRMAX] = "";
    CHECK(io && io->in == NULL);
    CHECK(mai_apple_unit_new(io, kAudioUnitSubType_GenericOutput, err, sizeof err) == noErr);
    CHECK(mai_apple_configure(io, MAI_DIR_OUT, RATE, err, sizeof err) == noErr);
    if (err[0]) fprintf(stderr, "output: %s\n", err);
    /* GenericOutput's own output side: the same format, so the unit only copies */
    AudioUnitUninitialize(io->unit);
    AudioStreamBasicDescription f = mai_apple_float_format(RATE, 2);
    CHECK(AudioUnitSetProperty(io->unit, kAudioUnitProperty_StreamFormat, kAudioUnitScope_Output, 0, &f, sizeof f) ==
          noErr);
    CHECK(AudioUnitInitialize(io->unit) == noErr);

    static float out[512 * 2];
    AudioBufferList abl = {1, {{2, sizeof out, out}}};
    uint64_t host = mach_absolute_time();
    AudioTimeStamp ts = stamp(0, host);
    AudioUnitRenderActionFlags flags = 0;
    abl.mBuffers[0].mDataByteSize = 256 * 2 * sizeof(float);
    CHECK(AudioUnitRender(io->unit, &flags, &ts, 0, 256, &abl) == noErr);
    int64_t after = mai_now_ns();
    int64_t host_ns = mai__ticks_to_ns(host, g_timebase.numer, g_timebase.denom);
    CHECK(p.renders == 1 && p.captures == 0);
    CHECK(p.frames_r == 256 && p.channels_r == 2 && p.rate_r == 48000 && p.frame_r == 0);
    CHECK(p.present_r == host_ns + 7000000);
    /* on mai_now_ns's clock: the callback began after the stamp (taken with
     * mach_absolute_time) and before the render returned */
    CHECK(p.host_r >= host_ns && p.host_r <= after);
    CHECK(out[0] == 0.001f && out[1] == -0.001f && out[2 * 255] == 0.256f && out[2 * 255 + 1] == -0.256f);

    ts = stamp(256, host + 1000);
    abl.mBuffers[0].mDataByteSize = 256 * 2 * sizeof(float);
    CHECK(AudioUnitRender(io->unit, &flags, &ts, 0, 256, &abl) == noErr);
    CHECK(p.frame_r == 256 && atomic_load(&s->be_xruns) == 0);
    ts = stamp(1024, host + 2000); /* 512 frames went missing */
    abl.mBuffers[0].mDataByteSize = 256 * 2 * sizeof(float);
    CHECK(AudioUnitRender(io->unit, &flags, &ts, 0, 256, &abl) == noErr);
    CHECK(atomic_load(&s->be_xruns) == 1 && p.renders == 3);
    CHECK(atomic_load(&s->frames_total) == 768 && atomic_load(&s->cb_frames_last) == 256);

    /* no host time: present_ns stays 0 (unknown) */
    ts = stamp(1280, 0);
    ts.mFlags = kAudioTimeStampSampleTimeValid;
    abl.mBuffers[0].mDataByteSize = 256 * 2 * sizeof(float);
    CHECK(AudioUnitRender(io->unit, &flags, &ts, 0, 256, &abl) == noErr);
    CHECK(p.present_r == 0 && p.renders == 4);

    mai_apple_io_free(io);
    close_stream(s);
    printf("output: GenericOutput render, 4 blocks of 256 frames, present = host + 7 ms, 1 xrun\n");
}

/* ── Input ─────────────────────────────────────────────────────────────── */

static void test_input(void) {
    for (int32_t ch = 1; ch <= 2; ch++) {
        probe p;
        mai_stream *s = open_stream(&p, MAI_DIR_IN, 2, ch);
        atomic_store(&s->in_latency_ns, 3000000);
        source src = {NULL, ch, 0, false, 0};
        src.unit = generic_unit(ch, source_render, &src, true);
        mai_apple_io *io = mai_apple_io_new(s);
        CHECK(io && io->in && io->in_cap == MAX_FRAMES_PER_SLICE && io->in_bus == 1);
        io->in_unit = src.unit; /* RemoteIO's bus 1, here another unit's bus 0 */
        io->in_bus = 0;
        uint64_t host = mach_absolute_time();
        AudioTimeStamp ts = stamp(0, host);
        AudioUnitRenderActionFlags flags = 0;
        CHECK(mai_apple_input(io, &flags, &ts, 1, 480, NULL) == noErr);
        int64_t after = mai_now_ns();
        int64_t host_ns = mai__ticks_to_ns(host, g_timebase.numer, g_timebase.denom);
        CHECK(p.captures == 1 && p.renders == 0);
        CHECK(p.frames_c == 480 && p.channels_c == ch && p.frame_c == 0);
        CHECK(p.present_c == host_ns - 3000000);
        CHECK(p.host_c >= host_ns && p.host_c <= after);
        float want1 = 0.5f * (float)sin(2 * M_PI * 1000.0 / RATE);
        CHECK(p.in_first[0] == 0.0f && fabsf(p.in_first[ch == 1 ? 1 : 2] - want1) < 1e-6f);
        if (ch == 2) CHECK(p.in_first[0] == p.in_first[1] && p.in_first[2] == p.in_first[3]);
        double rms = sqrt(p.in_sum_sq / (480.0 * ch)); /* 480 frames: 10 periods of 1 kHz */
        CHECK(fabs(rms - 0.5 / sqrt(2)) < 1e-3);
        double meter = mai__bits_float(atomic_load(&s->in_rms_bits));
        CHECK(fabs(meter - rms) < 1e-4);
        CHECK(fabsf(mai__bits_float(atomic_load(&s->in_peak_bits)) - 0.5f) < 1e-3f);

        ts = stamp(480, host + 10);
        CHECK(mai_apple_input(io, &flags, &ts, 1, 480, NULL) == noErr);
        CHECK(p.captures == 2 && p.frame_c == 480 && atomic_load(&s->be_xruns) == 0);

        /* the pull fails: the frames are lost (an xrun), nothing captured */
        src.fail = true;
        ts = stamp(960, host + 20);
        CHECK(mai_apple_input(io, &flags, &ts, 1, 480, NULL) == noErr);
        CHECK(p.captures == 2 && atomic_load(&s->be_xruns) == 1 && atomic_load(&s->callbacks) == 2);

        mai_apple_io_free(io);
        AudioUnitUninitialize(src.unit);
        AudioComponentInstanceDispose(src.unit);
        close_stream(s);
    }
    printf("input: AudioUnitRender pull, 1 and 2 channels, rms %.4f, present = host - 3 ms\n", 0.5 / sqrt(2));
}

/* ── Duplex ────────────────────────────────────────────────────────────── */

static void test_duplex(void) {
    probe p;
    mai_stream *s = open_stream(&p, MAI_DIR_DUPLEX, 2, 1);
    atomic_store(&s->out_latency_ns, 7000000);
    atomic_store(&s->in_latency_ns, 4000000);
    source src = {NULL, 1, 0, false, 0};
    src.unit = generic_unit(1, source_render, &src, true);
    mai_apple_io *io = mai_apple_io_new(s);
    char err[MAI_ERRMAX] = "";
    io->in_unit = src.unit;
    io->in_bus = 0;
    CHECK(mai_apple_unit_new(io, kAudioUnitSubType_GenericOutput, err, sizeof err) == noErr);
    CHECK(io->in_unit == src.unit);
    /* GenericOutput has no bus 1: only bus 0 is configured, the input comes from src */
    CHECK(mai_apple_configure(io, MAI_DIR_OUT, RATE, err, sizeof err) == noErr);
    AudioUnitUninitialize(io->unit);
    AudioStreamBasicDescription f = mai_apple_float_format(RATE, 2);
    CHECK(AudioUnitSetProperty(io->unit, kAudioUnitProperty_StreamFormat, kAudioUnitScope_Output, 0, &f, sizeof f) ==
          noErr);
    CHECK(AudioUnitInitialize(io->unit) == noErr);

    static float out[256 * 2];
    AudioBufferList abl = {1, {{2, sizeof out, out}}};
    uint64_t host = mach_absolute_time();
    AudioTimeStamp ts = stamp(0, host);
    AudioUnitRenderActionFlags flags = 0;
    CHECK(AudioUnitRender(io->unit, &flags, &ts, 0, 256, &abl) == noErr);
    int64_t host_ns = mai__ticks_to_ns(host, g_timebase.numer, g_timebase.denom);
    CHECK(p.captures == 1 && p.renders == 1 && p.n_order == 2 && p.order[0] == 'c' && p.order[1] == 'r');
    CHECK(p.frames_c == 256 && p.frames_r == 256 && p.channels_c == 1 && p.channels_r == 2);
    CHECK(p.frame_c == 0 && p.frame_r == 0 && p.host_c == p.host_r);
    CHECK(p.present_c == host_ns - 4000000 && p.present_r == host_ns + 7000000);
    float want1 = 0.5f * (float)sin(2 * M_PI * 1000.0 / RATE);
    CHECK(p.in_first[0] == 0.0f && fabsf(p.in_first[1] - want1) < 1e-6f);
    CHECK(out[0] == 0.001f && out[511] == -0.256f);
    CHECK(atomic_load(&s->in_underflows) == 0);

    /* the input pull fails: zero-filled, counted, the render goes on */
    src.fail = true;
    ts = stamp(256, host + 10);
    abl.mBuffers[0].mDataByteSize = sizeof out;
    CHECK(AudioUnitRender(io->unit, &flags, &ts, 0, 256, &abl) == noErr);
    CHECK(p.captures == 2 && p.renders == 2 && p.frame_c == 256 && p.in_sum_sq == 0.0);
    CHECK(atomic_load(&s->in_underflows) == 1 && atomic_load(&s->be_xruns) == 0);
    CHECK(out[0] == 0.001f);

    /* the input comes a frame short (if the unit passes the short buffer
     * on): the frames it gave, a zero for the missing one, counted */
    src.fail = false;
    src.short_by = 1;
    ts = stamp(512, host + 20);
    abl.mBuffers[0].mDataByteSize = sizeof out;
    CHECK(AudioUnitRender(io->unit, &flags, &ts, 0, 256, &abl) == noErr);
    CHECK(p.captures == 3 && p.in_sum_sq > 0.0 && atomic_load(&s->in_underflows) == 2);
    CHECK(io->in[255] == 0.0f && io->in[254] != 0.0f);

    mai_apple_io_free(io);
    AudioUnitUninitialize(src.unit);
    AudioComponentInstanceDispose(src.unit);
    close_stream(s);
    printf("duplex: capture then render in one GenericOutput render, 1 input underflow on a failed pull\n");
}

/* ── Decode ────────────────────────────────────────────────────────────── */

static bool grow(mai_pcm *p, size_t bytes) {
    float *d = realloc(p->data, bytes);
    if (!d) return false;
    p->data = d;
    p->cap = bytes;
    return true;
}

typedef struct {
    const char *err;
    char detail[MAI_ERRMAX];
    int32_t rate;
    mai_pcm pcm;
} decoded;

static decoded decode(const char *path, size_t max_bytes, bool mono) {
    decoded d;
    memset(&d, 0, sizeof d);
    d.pcm.max_bytes = max_bytes;
    d.pcm.mono = mono;
    d.pcm.grow = grow;
    d.err = mai_apple_decode(path, &d.pcm, &d.rate, d.detail, sizeof d.detail);
    return d;
}

/* Positive-going zero crossings per second of channel c over the middle half. */
static double frequency(const decoded *d, int32_t c) {
    size_t a = d->pcm.frames / 4, b = 3 * d->pcm.frames / 4, n = 0;
    int32_t ch = d->pcm.channels;
    for (size_t i = a + 1; i < b; i++)
        if (d->pcm.data[(i - 1) * (size_t)ch + (size_t)c] < 0 && d->pcm.data[i * (size_t)ch + (size_t)c] >= 0) n++;
    return n * (double)d->rate / (double)(b - a);
}

static double rms(const decoded *d, int32_t c) {
    size_t a = d->pcm.frames / 4, b = 3 * d->pcm.frames / 4;
    double sum = 0;
    int32_t ch = d->pcm.channels;
    for (size_t i = a; i < b; i++) sum += (double)d->pcm.data[i * (size_t)ch + (size_t)c] * d->pcm.data[i * (size_t)ch + (size_t)c];
    return sqrt(sum / (double)(b - a));
}

/* 1 s of 44.1 kHz stereo 16-bit: 1 kHz at 0.5 left, at 0.25 right. */
#define WAV_RATE 44100
static void write_wav(const char *path) {
    FILE *f = fopen(path, "wb");
    if (!f) abort();
    uint32_t frames = WAV_RATE, data = frames * 4;
    uint32_t u32;
    uint16_t u16;
#define W32(v) (u32 = (v), fwrite(&u32, 4, 1, f))
#define W16(v) (u16 = (v), fwrite(&u16, 2, 1, f))
    fwrite("RIFF", 1, 4, f);
    W32(36 + data);
    fwrite("WAVEfmt ", 1, 8, f);
    W32(16);
    W16(1);
    W16(2);
    W32(WAV_RATE);
    W32(WAV_RATE * 4);
    W16(4);
    W16(16);
    fwrite("data", 1, 4, f);
    W32(data);
    for (uint32_t i = 0; i < frames; i++) {
        double x = sin(2 * M_PI * 1000.0 * i / WAV_RATE);
        W16((uint16_t)(int16_t)lround(0.5 * x * 32767));
        W16((uint16_t)(int16_t)lround(0.25 * x * 32767));
    }
#undef W32
#undef W16
    fclose(f);
}

static void check_tone(const char *what, const decoded *d, size_t min_frames, size_t max_frames, double rms_tol) {
    CHECK(d->err == NULL);
    if (d->err) {
        fprintf(stderr, "%s: %s (%s)\n", what, d->err, d->detail);
        return;
    }
    double fl = frequency(d, 0), fr = frequency(d, 1), rl = rms(d, 0), rr = rms(d, 1);
    printf("decode %s: rate %d, channels %d, frames %zu, left %.1f Hz rms %.4f, right %.1f Hz rms %.4f\n", what,
           (int)d->rate, (int)d->pcm.channels, d->pcm.frames, fl, rl, fr, rr);
    CHECK(d->rate == WAV_RATE && d->pcm.channels == 2);
    CHECK(d->pcm.frames >= min_frames && d->pcm.frames <= max_frames);
    CHECK(fabs(fl - 1000) < 5 && fabs(fr - 1000) < 5);
    CHECK(fabs(rl - 0.5 / sqrt(2)) < rms_tol && fabs(rr - 0.25 / sqrt(2)) < rms_tol);
}

static bool exists(const char *path) { return access(path, R_OK) == 0; }

static void test_decode(const char *dir) {
    char wav[512], m4a[512], mp3[512], txt[512], cmd[2048];
    snprintf(wav, sizeof wav, "%s/tone.wav", dir);
    snprintf(m4a, sizeof m4a, "%s/tone.m4a", dir);
    snprintf(mp3, sizeof mp3, "%s/tone.mp3", dir);
    snprintf(txt, sizeof txt, "%s/not_audio.wav", dir);
    write_wav(wav);

    decoded d = decode(wav, 64u << 20, false);
    check_tone("wav", &d, WAV_RATE, WAV_RATE, 1e-3);
    /* 16-bit PCM decodes exactly: sample 1 of the left */
    if (!d.err) CHECK(fabsf(d.pcm.data[2] - (float)lround(0.5 * sin(2 * M_PI * 1000.0 / WAV_RATE) * 32767) / 32768.0f) < 1e-6f);
    float l1 = d.err ? 0 : d.pcm.data[2], r1 = d.err ? 0 : d.pcm.data[3];
    free(d.pcm.data);

    d = decode(wav, 64u << 20, true);
    CHECK(!d.err && d.pcm.channels == 1 && d.pcm.frames == WAV_RATE && fabsf(d.pcm.data[1] - (l1 + r1) / 2) < 1e-6f);
    free(d.pcm.data);

    /* AAC: the encoder's priming and padding are trimmed by ExtAudioFile,
     * allow a frame (1024) either way */
    snprintf(cmd, sizeof cmd, "afconvert -f m4af -d aac -b 192000 '%s' '%s'", wav, m4a);
    CHECK(system(cmd) == 0 && exists(m4a));
    d = decode(m4a, 64u << 20, false);
    check_tone("m4a (aac)", &d, WAV_RATE - 1024, WAV_RATE + 2048, 0.02);
    free(d.pcm.data);

    snprintf(cmd, sizeof cmd,
             "(command -v ffmpeg >/dev/null && ffmpeg -loglevel error -y -i '%s' -codec:a libmp3lame -b:a 192k '%s') "
             "|| (command -v lame >/dev/null && lame --quiet -b 192 '%s' '%s')",
             wav, mp3, wav, mp3);
    unlink(mp3);
    if (system(cmd) == 0 && exists(mp3)) {
        d = decode(mp3, 64u << 20, false);
        /* MP3 adds a decoder delay plus padding up to a frame (1152) */
        check_tone("mp3", &d, WAV_RATE - 1152, WAV_RATE + 3 * 1152, 0.02);
        free(d.pcm.data);
    } else {
        printf("SKIP mp3: no MP3 encoder here (ffmpeg with libmp3lame, or lame); macOS ships none\n");
    }

    d = decode("/nonexistent/tone.wav", 1 << 20, false);
    CHECK(d.err && strcmp(d.err, "not_found") == 0 && d.pcm.data == NULL);
    d = decode(dir, 1 << 20, false);
    CHECK(d.err && strcmp(d.err, "not_found") == 0);

    FILE *f = fopen(txt, "w");
    fputs("this is not audio, just text long enough to be read as a header\n", f);
    fclose(f);
    d = decode(txt, 1 << 20, false);
    CHECK(d.err && strcmp(d.err, "unsupported") == 0 && strstr(d.detail, "ExtAudioFileOpenURL") != NULL);
    printf("decode text file: %s (%s)\n", d.err ? d.err : "ok", d.detail);
    free(d.pcm.data);

    d = decode(wav, 100000, false); /* 44100 * 2 * 4 bytes don't fit */
    CHECK(d.err && strcmp(d.err, "too_large") == 0);
    free(d.pcm.data);
    d = decode(wav, 44100 * 4, true); /* mono fits exactly */
    CHECK(!d.err && d.pcm.frames == WAV_RATE);
    free(d.pcm.data);
}

/* ── Hardware (skipped without an output device) ───────────────────────── */

static void test_hardware(void) {
    AudioObjectPropertyAddress a = {kAudioHardwarePropertyDefaultOutputDevice, kAudioObjectPropertyScopeGlobal,
                                    kAudioObjectPropertyElementMain};
    AudioDeviceID dev = kAudioObjectUnknown;
    UInt32 size = sizeof dev;
    if (AudioObjectGetPropertyData(kAudioObjectSystemObject, &a, 0, NULL, &size, &dev) || dev == kAudioObjectUnknown) {
        printf("SKIP hardware: no default output device\n");
        return;
    }
    Float64 rate = 0;
    a.mSelector = kAudioDevicePropertyNominalSampleRate;
    size = sizeof rate;
    if (AudioObjectGetPropertyData(dev, &a, 0, NULL, &size, &rate) || rate <= 0) rate = RATE;

    probe p;
    mai_stream *s = open_stream(&p, MAI_DIR_OUT, 2, 1);
    p.silent = true;
    s->rate = (int32_t)rate;
    mai_apple_io *io = mai_apple_io_new(s);
    char err[MAI_ERRMAX] = "";
    if (mai_apple_unit_new(io, kAudioUnitSubType_HALOutput, err, sizeof err) ||
        mai_apple_enable_io(io, MAI_DIR_OUT, err, sizeof err) ||
        mai_apple_configure(io, MAI_DIR_OUT, rate, err, sizeof err)) {
        printf("SKIP hardware: %s\n", err);
        mai_apple_io_free(io);
        close_stream(s);
        return;
    }
    /* every callback starts between `before` and `after` (the unit is
     * stopped and disposed before `after`), however loaded the machine */
    int64_t before = mai_now_ns();
    OSStatus r = AudioOutputUnitStart(io->unit);
    if (r) {
        printf("SKIP hardware: AudioOutputUnitStart %d\n", (int)r);
    } else {
        usleep(300000);
    }
    mai_apple_io_free(io); /* stops first: no callback after */
    int64_t after = mai_now_ns();
    int renders = atomic_load(&p.renders);
    if (!r && renders == 0) printf("SKIP hardware: the device never called back\n");
    if (renders > 0) {
        CHECK(p.channels_r == 2 && p.rate_r == (int32_t)rate && p.frames_r > 0);
        CHECK(p.host_r >= before && p.host_r <= after);
        /* the timestamp's host time is the IO cycle's, near the callback
         * on the same clock: within a second either way even on a loaded
         * runner (a different clock would be off by hours) */
        CHECK(p.present_r > 0 && p.present_r - p.host_r > -1000000000LL && p.present_r - p.host_r < 1000000000LL);
        CHECK(atomic_load(&s->frames_total) >= (uint64_t)p.frames_r);
        printf("hardware: HALOutput %.0f Hz, %d callbacks of %d frames, present - host %.2f ms, xruns %u\n", rate,
               renders, (int)p.frames_r, (p.present_r - p.host_r) / 1e6, (unsigned)atomic_load(&s->be_xruns));
    }
    close_stream(s);
}

int main(int argc, char **argv) {
    char dir[512];
    if (argc > 1) {
        snprintf(dir, sizeof dir, "%s", argv[1]);
    } else {
        snprintf(dir, sizeof dir, "/tmp/mai_apple_test_%d", (int)getpid());
    }
    char cmd[600];
    snprintf(cmd, sizeof cmd, "mkdir -p '%s'", dir);
    if (system(cmd) != 0) return 2;
    mai_apple_timebase();
    test_session();
    test_output();
    test_input();
    test_duplex();
    test_decode(dir);
    test_hardware();
    if (failures) {
        fprintf(stderr, "%d check(s) failed\n", failures);
        return 1;
    }
    printf("mai_apple_test: all passed\n");
    return 0;
}
