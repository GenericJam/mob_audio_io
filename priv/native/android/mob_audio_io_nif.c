/*
 * mob_audio_io_nif.c (Android): AAudio output for mob_audio_io.
 *
 * Lifted from FMutate's c_src/fm1audio.c and Magpie's c_src/magpie_audio.c:
 *   - AAudio is loaded with dlopen (libaaudio.so, API 26+), so the app
 *     library needs no extra link flag and loads on older phones.
 *   - Low-latency performance mode, shared by default (sharing: :exclusive
 *     asks for MMAP exclusive and falls back to shared), at the device's own
 *     rate unless one is asked for, float stereo.
 *   - The buffer starts at two bursts (or the frames asked for); after an
 *     underrun the event thread adds a burst, up to the capacity.
 *   - A disconnect (headphones in or out, a USB device) arrives on AAudio's
 *     error callback, which only flags it: the event thread closes the
 *     stream and opens a new one on whatever the system now routes to.
 *
 * The NIF functions, the event thread and the processor registry are in
 * ../common/mai_nif.c (shared with iOS).
 */
#include <aaudio/AAudio.h>
#include <dlfcn.h>
#include <android/api-level.h>
#include <stdio.h>

#include "../common/mai_nif.c"

static struct aaudio_api {
    void *h; /* set only once every function below resolved */
    aaudio_result_t (*createStreamBuilder)(AAudioStreamBuilder **);
    void (*setDirection)(AAudioStreamBuilder *, aaudio_direction_t);
    void (*setFormat)(AAudioStreamBuilder *, aaudio_format_t);
    void (*setChannelCount)(AAudioStreamBuilder *, int32_t);
    void (*setSampleRate)(AAudioStreamBuilder *, int32_t);
    void (*setPerformanceMode)(AAudioStreamBuilder *, aaudio_performance_mode_t);
    void (*setSharingMode)(AAudioStreamBuilder *, aaudio_sharing_mode_t);
    void (*setUsage)(AAudioStreamBuilder *, aaudio_usage_t);
    void (*setContentType)(AAudioStreamBuilder *, aaudio_content_type_t);
    void (*setDataCallback)(AAudioStreamBuilder *, AAudioStream_dataCallback, void *);
    void (*setErrorCallback)(AAudioStreamBuilder *, AAudioStream_errorCallback, void *);
    aaudio_result_t (*openStream)(AAudioStreamBuilder *, AAudioStream **);
    aaudio_result_t (*builderDelete)(AAudioStreamBuilder *);
    aaudio_result_t (*requestStart)(AAudioStream *);
    aaudio_result_t (*close)(AAudioStream *);
    int32_t (*getSampleRate)(AAudioStream *);
    int32_t (*getChannelCount)(AAudioStream *);
    int32_t (*getFramesPerBurst)(AAudioStream *);
    aaudio_result_t (*setBufferSizeInFrames)(AAudioStream *, int32_t);
    int32_t (*getBufferSizeInFrames)(AAudioStream *);
    int32_t (*getBufferCapacityInFrames)(AAudioStream *);
    aaudio_sharing_mode_t (*getSharingMode)(AAudioStream *);
    int32_t (*getXRunCount)(AAudioStream *);
    int64_t (*getFramesWritten)(AAudioStream *);
    int64_t (*getFramesRead)(AAudioStream *);
    aaudio_result_t (*getTimestamp)(AAudioStream *, clockid_t, int64_t *, int64_t *);
    int32_t (*getDeviceId)(AAudioStream *);
    const char *(*convertResultToText)(aaudio_result_t);
} A;

static int g_api; /* the device's API level */

static const char *be_platform(void) { return "android"; }

static const char *be_load(void) {
    struct aaudio_api a;
    if (A.h) return NULL;
    g_api = android_get_device_api_level();
    if (!(a.h = dlopen("libaaudio.so", RTLD_NOW))) return "unavailable";
#define S(f, n) \
    if (!(*(void **)&a.f = dlsym(a.h, n))) goto fail
    S(createStreamBuilder, "AAudio_createStreamBuilder");
    S(setDirection, "AAudioStreamBuilder_setDirection");
    S(setFormat, "AAudioStreamBuilder_setFormat");
    S(setChannelCount, "AAudioStreamBuilder_setChannelCount");
    S(setSampleRate, "AAudioStreamBuilder_setSampleRate");
    S(setPerformanceMode, "AAudioStreamBuilder_setPerformanceMode");
    S(setSharingMode, "AAudioStreamBuilder_setSharingMode");
    S(setDataCallback, "AAudioStreamBuilder_setDataCallback");
    S(setErrorCallback, "AAudioStreamBuilder_setErrorCallback");
    S(openStream, "AAudioStreamBuilder_openStream");
    S(builderDelete, "AAudioStreamBuilder_delete");
    S(requestStart, "AAudioStream_requestStart");
    S(close, "AAudioStream_close");
    S(getSampleRate, "AAudioStream_getSampleRate");
    S(getChannelCount, "AAudioStream_getChannelCount");
    S(getFramesPerBurst, "AAudioStream_getFramesPerBurst");
    S(setBufferSizeInFrames, "AAudioStream_setBufferSizeInFrames");
    S(getBufferSizeInFrames, "AAudioStream_getBufferSizeInFrames");
    S(getBufferCapacityInFrames, "AAudioStream_getBufferCapacityInFrames");
    S(getSharingMode, "AAudioStream_getSharingMode");
    S(getXRunCount, "AAudioStream_getXRunCount");
    S(getFramesWritten, "AAudioStream_getFramesWritten");
    S(getFramesRead, "AAudioStream_getFramesRead");
    S(getTimestamp, "AAudioStream_getTimestamp");
    S(getDeviceId, "AAudioStream_getDeviceId");
    S(convertResultToText, "AAudio_convertResultToText");
#undef S
    /* API 28+: optional, so API 26/27 still open streams */
    *(void **)&a.setUsage = dlsym(a.h, "AAudioStreamBuilder_setUsage");
    *(void **)&a.setContentType = dlsym(a.h, "AAudioStreamBuilder_setContentType");
    A = a;
    return NULL;
fail: /* a library without one of them: as if there were none */
    dlclose(a.h);
    return "unavailable";
}

static void set_aaudio_error(mai_stream *s, const char *what, aaudio_result_t r) {
    char buf[MAI_ERRMAX];
    snprintf(buf, sizeof buf, "%s: %s (%d)", what, A.convertResultToText(r), (int)r);
    mai__set_error(s, buf);
}

/* When the block about to be rendered is heard: from the anchor be_poll
 * publishes (AAudio timestamps, API 31+), else now plus the buffer. */
static int64_t out_present(mai_stream *s, int64_t t0) {
    if (s->present == MAI_PRESENT_TIMESTAMP) return mai__present_from_anchor(s, s->frame);
    return t0 + atomic_load_explicit(&s->out_latency_ns, memory_order_relaxed);
}

static aaudio_data_callback_result_t on_data(AAudioStream *st, void *user, void *audio, int32_t n) {
    (void)st;
    mai_stream *s = user;
    int64_t t0 = mai_now_ns();
    mai__process(s, NULL, (float *)audio, n, t0, 0, out_present(s, t0));
    return AAUDIO_CALLBACK_RESULT_CONTINUE;
}

/* AAudio's own thread: only flag it (closing here would deadlock AAudio). */
static void on_error(AAudioStream *st, void *user, aaudio_result_t err) {
    (void)st;
    if (err == AAUDIO_ERROR_DISCONNECTED) atomic_fetch_or(&((mai_stream *)user)->pending, MAI_PF_DEVICE_LOST);
}

static aaudio_result_t try_open(mai_stream *s, aaudio_sharing_mode_t mode, AAudioStream **out) {
    AAudioStreamBuilder *b;
    aaudio_result_t r = A.createStreamBuilder(&b);
    if (r != AAUDIO_OK) return r;
    A.setDirection(b, AAUDIO_DIRECTION_OUTPUT);
    A.setFormat(b, AAUDIO_FORMAT_PCM_FLOAT);
    A.setChannelCount(b, s->channels);
    A.setSampleRate(b, s->want_rate > 0 ? s->want_rate : AAUDIO_UNSPECIFIED);
    A.setPerformanceMode(b, AAUDIO_PERFORMANCE_MODE_LOW_LATENCY);
    A.setSharingMode(b, mode);
    if (A.setUsage) A.setUsage(b, AAUDIO_USAGE_MEDIA);
    if (A.setContentType) A.setContentType(b, AAUDIO_CONTENT_TYPE_MUSIC);
    A.setDataCallback(b, on_data, s);
    A.setErrorCallback(b, on_error, s);
    r = A.openStream(b, out);
    A.builderDelete(b);
    return r;
}

/* The buffer's length in ns: the latency estimate below API 31. */
static void set_buffer_latency(mai_stream *s) {
    if (s->rate > 0) atomic_store(&s->out_latency_ns, (int64_t)s->buffer * 1000000000LL / s->rate);
}

static const char *be_open(mai_stream *s) {
    if (s->dir != MAI_DIR_OUT) {
        mai__set_error(s, "input streams are not in this build");
        return "unsupported";
    }
    AAudioStream *st = NULL;
    aaudio_result_t r = AAUDIO_ERROR_INTERNAL;
    if (s->sharing == MAI_SHARING_EXCLUSIVE) r = try_open(s, AAUDIO_SHARING_MODE_EXCLUSIVE, &st);
    if (r != AAUDIO_OK) r = try_open(s, AAUDIO_SHARING_MODE_SHARED, &st);
    if (r != AAUDIO_OK) {
        set_aaudio_error(s, "open", r);
        return "unavailable";
    }
    s->rate = A.getSampleRate(st);
    s->channels = A.getChannelCount(st);
    s->burst = A.getFramesPerBurst(st);
    s->capacity = A.getBufferCapacityInFrames(st);
    s->exclusive = A.getSharingMode(st) == AAUDIO_SHARING_MODE_EXCLUSIVE;
    s->device_id = A.getDeviceId(st);
    int32_t want = s->want_frames > 0 ? s->want_frames : 2 * s->burst; /* double buffering */
    if (want > 0) A.setBufferSizeInFrames(st, want);
    s->buffer = A.getBufferSizeInFrames(st);
    /* AAudioStream_getTimestamp only from API 31 (see be_latency) */
    s->present = g_api >= 31 ? MAI_PRESENT_TIMESTAMP : MAI_PRESENT_ESTIMATE;
    set_buffer_latency(s);
    s->be = st;
    return NULL;
}

static const char *be_start(mai_stream *s) {
    aaudio_result_t r = A.requestStart((AAudioStream *)s->be);
    if (r == AAUDIO_OK) return NULL;
    set_aaudio_error(s, "start", r);
    return "unavailable";
}

static void be_close(mai_stream *s) {
    if (!s->be) return;
    /* (close stops the stream first; no callback runs once it returns) */
    A.close((AAudioStream *)s->be);
    s->be = NULL;
}

static int32_t be_xruns(mai_stream *s) { return s->be ? A.getXRunCount((AAudioStream *)s->be) : 0; }

/* After new underruns: a burst more of buffer, up to the capacity (Magpie's
 * tune_buffer). An app that asked for a fixed size keeps it. */
static void be_tune(mai_stream *s) {
    AAudioStream *st = s->be;
    if (!st || s->want_frames > 0 || s->burst <= 0) return;
    int32_t size = A.getBufferSizeInFrames(st);
    if (size + s->burst <= s->capacity) {
        A.setBufferSizeInFrames(st, size + s->burst);
        s->buffer = A.getBufferSizeInFrames(st);
        set_buffer_latency(s);
    }
}

/* API 31+: the presentation anchor from AAudio's timestamp (the stream's
 * frame position `pos` is heard at `ns`; the stream started at be_frame0). */
static void be_poll(mai_stream *s) {
    AAudioStream *st = s->be;
    int64_t pos, ns;
    if (!st || s->present != MAI_PRESENT_TIMESTAMP) return;
    if (A.getTimestamp(st, CLOCK_MONOTONIC, &pos, &ns) == AAUDIO_OK && ns > 0) mai__set_anchor(s, s->be_frame0 + pos, ns);
}

/* Output latency. AAudioStream_getTimestamp aborts the process on Android
 * 11's legacy (AudioTrack) path: SIGABRT in
 * AudioStreamLegacy::getBestTimestamp, in both FMutate and Magpie on the
 * moto g power 2021 (its crash buffer, 2026-11-20). Below API 31 this is
 * only the frames written but not yet read by the device; from 31 on, the
 * frames written but not yet presented (fm1audio's stats). */
static void be_latency(mai_stream *s, double *out_ms, double *in_ms) {
    AAudioStream *st = s->be;
    int64_t pos, ns;
    *in_ms = -1;
    *out_ms = -1;
    if (!st || s->rate <= 0) return;
    int64_t written = A.getFramesWritten(st);
    if (g_api < 31) {
        *out_ms = (double)(written - A.getFramesRead(st)) * 1000.0 / s->rate;
        return;
    }
    if (A.getTimestamp(st, CLOCK_MONOTONIC, &pos, &ns) != AAUDIO_OK) return;
    double since = (mai_now_ns() - ns) / 1e9;
    *out_ms = ((double)(written - pos) / s->rate - since) * 1000.0;
}

static void be_idle(void) {}

static ERL_NIF_TERM be_session_term(ErlNifEnv *env) { return atom(env, "nil"); }

static ERL_NIF_TERM be_devices(ErlNifEnv *env, int dir) {
    (void)dir;
    return error_atom(env, "unsupported");
}

static const char *be_decode(const char *path, mai_pcm *pcm, int32_t *rate, char *detail, size_t detail_n) {
    (void)path; (void)pcm; (void)rate; (void)detail; (void)detail_n;
    return "unsupported";
}
