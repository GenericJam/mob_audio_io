/*
 * mob_audio_io_nif.c (Android): AAudio streams for mob_audio_io.
 *
 * Lifted from FMutate's c_src/fm1audio.c, Magpie's c_src/magpie_audio.c
 * and Humdinger's c_src/hum_audio.c:
 *   - AAudio is loaded with dlopen (libaaudio.so, API 26+), so the app
 *     library needs no extra link flag and loads on older phones.
 *   - Low-latency performance mode, shared by default (sharing: :exclusive
 *     asks for MMAP exclusive and falls back to shared), at the device's own
 *     rate unless one is asked for, float.
 *   - Output: the buffer starts at two bursts (or the frames asked for);
 *     after an underrun the event thread adds a burst, up to the capacity.
 *   - Input: a callback stream with the input preset asked for.
 *   - Duplex: an output stream with the callback and an input stream
 *     without one, at the output's rate, started first. The output callback
 *     reads all the input has, without blocking, into a ring allocated at
 *     open, and takes its block from there once a cushion of an input burst
 *     has built up; a short block is zero-filled (input_underflows), a
 *     backlog over about a buffer is dropped back to the cushion.
 *   - A disconnect (headphones in or out, a USB device) or any other error
 *     arrives on AAudio's error callback of either stream, which only flags
 *     it: the event thread closes the stream and opens a new one on whatever
 *     the system now routes to.
 *   - AAudioStream_getTimestamp only from API 31 (see be_latency).
 *
 * The NIF functions, the event thread and the processor registry are in
 * ../common/mai_nif.c (shared with iOS); Java (the permission, devices) is
 * in mai_jni.c, decode/2 in mai_decode.c.
 */
#include <aaudio/AAudio.h>
#include <android/api-level.h>
#include <dlfcn.h>
#include <stdio.h>

#include "../common/mai_nif.c"
#include "mai_jni.c"
#include "mai_decode.c"

static struct aaudio_api {
    void *h; /* set only once every required function below resolved */
    aaudio_result_t (*createStreamBuilder)(AAudioStreamBuilder **);
    void (*setDirection)(AAudioStreamBuilder *, aaudio_direction_t);
    void (*setFormat)(AAudioStreamBuilder *, aaudio_format_t);
    void (*setChannelCount)(AAudioStreamBuilder *, int32_t);
    void (*setSampleRate)(AAudioStreamBuilder *, int32_t);
    void (*setDeviceId)(AAudioStreamBuilder *, int32_t);
    void (*setPerformanceMode)(AAudioStreamBuilder *, aaudio_performance_mode_t);
    void (*setSharingMode)(AAudioStreamBuilder *, aaudio_sharing_mode_t);
    void (*setUsage)(AAudioStreamBuilder *, aaudio_usage_t);
    void (*setContentType)(AAudioStreamBuilder *, aaudio_content_type_t);
    void (*setInputPreset)(AAudioStreamBuilder *, aaudio_input_preset_t);
    void (*setDataCallback)(AAudioStreamBuilder *, AAudioStream_dataCallback, void *);
    void (*setErrorCallback)(AAudioStreamBuilder *, AAudioStream_errorCallback, void *);
    aaudio_result_t (*openStream)(AAudioStreamBuilder *, AAudioStream **);
    aaudio_result_t (*builderDelete)(AAudioStreamBuilder *);
    aaudio_result_t (*requestStart)(AAudioStream *);
    aaudio_result_t (*close)(AAudioStream *);
    aaudio_result_t (*read)(AAudioStream *, void *, int32_t, int64_t);
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
    aaudio_input_preset_t (*getInputPreset)(AAudioStream *);
    const char *(*convertResultToText)(aaudio_result_t);
} A;

static int g_api; /* the device's API level */

/* Per stream slot, what only this backend keeps. The duplex ring and block
 * are allocated at open and freed at close; while the stream runs only its
 * output callback touches them. */
typedef struct {
    int32_t in_burst;      /* the input stream's burst (input, duplex) */
    float *ring;           /* duplex: the input read so far and not yet processed */
    int32_t ring_frames;
    int64_t ring_w, ring_r; /* frames written into / taken out of the ring */
    float *block;          /* duplex: one block of input, contiguous, for capture */
    int32_t block_frames;
    int32_t keep;          /* duplex: the cushion of input kept behind the output (an input burst) */
    int32_t limit;         /* ... and the most left over after a callback before the excess is dropped */
    bool primed;           /* duplex: the input has delivered a block and the cushion */
    _Atomic int32_t waiting; /* duplex: frames left in the ring after the last callback (be_latency) */
} android_side;

static android_side g_side[MAI_MAX_STREAMS];

static android_side *side_of(mai_stream *s) { return &g_side[slot_of(s)]; }

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
    S(setDeviceId, "AAudioStreamBuilder_setDeviceId");
    S(setPerformanceMode, "AAudioStreamBuilder_setPerformanceMode");
    S(setSharingMode, "AAudioStreamBuilder_setSharingMode");
    S(setDataCallback, "AAudioStreamBuilder_setDataCallback");
    S(setErrorCallback, "AAudioStreamBuilder_setErrorCallback");
    S(openStream, "AAudioStreamBuilder_openStream");
    S(builderDelete, "AAudioStreamBuilder_delete");
    S(requestStart, "AAudioStream_requestStart");
    S(close, "AAudioStream_close");
    S(read, "AAudioStream_read");
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
    *(void **)&a.setInputPreset = dlsym(a.h, "AAudioStreamBuilder_setInputPreset");
    *(void **)&a.getInputPreset = dlsym(a.h, "AAudioStream_getInputPreset");
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

static int64_t frames_ns(const mai_stream *s, int64_t frames) {
    return s->rate > 0 ? frames * 1000000000LL / s->rate : 0;
}

/* ── The audio callbacks (no lock, no allocation, no JNI) ──────────────── */

/* When the output block about to be rendered is heard: from the anchor
 * be_poll publishes (AAudio timestamps, API 31+), else `t` plus the
 * buffer. */
static int64_t out_present(mai_stream *s, int64_t t) {
    if (s->present == MAI_PRESENT_TIMESTAMP) return mai__present_from_anchor(s, s->frame);
    return t + atomic_load_explicit(&s->out_latency_ns, memory_order_relaxed);
}

/* When the first of `n` input frames delivered at `t0` was captured: the
 * block, plus the input's own latency (a burst) before it. */
static int64_t in_estimate(mai_stream *s, int64_t t0, int32_t n) {
    return t0 - frames_ns(s, n) - atomic_load_explicit(&s->in_latency_ns, memory_order_relaxed);
}

static aaudio_data_callback_result_t on_data(AAudioStream *st, void *user, void *audio, int32_t n) {
    (void)st;
    mai_stream *s = user;
    int64_t t0 = mai_now_ns();
    mai__process(s, NULL, (float *)audio, n, t0, 0, out_present(s, t0));
    return AAUDIO_CALLBACK_RESULT_CONTINUE;
}

/* Input: capture times from the input's timestamps (API 31+, be_poll's
 * anchor) once there is one, else the estimate. */
static aaudio_data_callback_result_t on_in_data(AAudioStream *st, void *user, void *audio, int32_t n) {
    (void)st;
    mai_stream *s = user;
    int64_t t0 = mai_now_ns();
    int64_t at = s->present == MAI_PRESENT_TIMESTAMP ? mai__present_from_anchor(s, s->frame) : 0;
    mai__process(s, (const float *)audio, NULL, n, t0, at ? at : in_estimate(s, t0, n), 0);
    return AAUDIO_CALLBACK_RESULT_CONTINUE;
}

/* Duplex: everything the input (be2, no callback) has, read without
 * blocking into the ring, as far as it has room. */
static void ring_read(AAudioStream *in, android_side *d, int32_t ich) {
    for (;;) {
        int32_t space = d->ring_frames - (int32_t)(d->ring_w - d->ring_r);
        int32_t at = (int32_t)(d->ring_w % d->ring_frames);
        int32_t c = space < d->ring_frames - at ? space : d->ring_frames - at;
        if (c <= 0) return;
        int32_t r = A.read(in, d->ring + (size_t)at * (size_t)ich, c, 0);
        if (r <= 0) return;
        d->ring_w += r;
        if (r < c) return;
    }
}

/* Up to `n` frames out of the ring into `dst`; how many. */
static int32_t ring_take(android_side *d, float *dst, int32_t n, int32_t ich) {
    int64_t fill = d->ring_w - d->ring_r;
    int32_t want = fill < n ? (int32_t)fill : n;
    for (int32_t done = 0; done < want;) {
        int32_t at = (int32_t)(d->ring_r % d->ring_frames);
        int32_t c = want - done < d->ring_frames - at ? want - done : d->ring_frames - at;
        memcpy(dst + (size_t)done * (size_t)ich, d->ring + (size_t)at * (size_t)ich, sizeof(float) * (size_t)c * (size_t)ich);
        done += c;
        d->ring_r += c;
    }
    return want;
}

/* Duplex: the output's callback. The input goes through the ring: it
 * starts once a block and the cushion (an input burst) have arrived, so an
 * input that delivers in bursts larger than the output's callbacks doesn't
 * come short between them; more than the limit left over (a stall, clock
 * drift) and the oldest is dropped, back to the cushion. A block longer
 * than `block` is processed in pieces. */
static aaudio_data_callback_result_t on_duplex(AAudioStream *st, void *user, void *audio, int32_t n) {
    (void)st;
    mai_stream *s = user;
    android_side *d = side_of(s);
    int32_t ich = s->in_channels;
    int64_t t0 = mai_now_ns();
    ring_read(s->be2, d, ich);
    int64_t fill = d->ring_w - d->ring_r;
    if (!d->primed && fill >= (int64_t)n + d->keep) d->primed = true;
    if (d->primed && fill > (int64_t)n + d->limit) d->ring_r += fill - n - d->keep;
    int64_t in_latency = atomic_load_explicit(&s->in_latency_ns, memory_order_relaxed);
    float *out = audio;
    for (int32_t off = 0; off < n;) {
        int32_t c = n - off < d->block_frames ? n - off : d->block_frames;
        /* the newest frame in the ring was captured about in_latency ago */
        int64_t in_at = t0 - in_latency - frames_ns(s, d->ring_w - d->ring_r);
        int32_t r = d->primed ? ring_take(d, d->block, c, ich) : 0;
        if (r < c) {
            memset(d->block + (size_t)r * (size_t)ich, 0, sizeof(float) * (size_t)(c - r) * (size_t)ich);
            if (d->primed) atomic_fetch_add_explicit(&s->in_underflows, 1, memory_order_relaxed);
        }
        int64_t off_ns = frames_ns(s, off);
        mai__process(s, d->block, out + (size_t)off * (size_t)s->channels, c, t0, in_at, out_present(s, t0 + off_ns));
        off += c;
    }
    atomic_store_explicit(&d->waiting, (int32_t)(d->ring_w - d->ring_r), memory_order_relaxed);
    return AAUDIO_CALLBACK_RESULT_CONTINUE;
}

/* AAudio's own thread, for either stream: only flag it (closing here would
 * deadlock AAudio). A stream that reported an error is dead either way. */
static void on_error(AAudioStream *st, void *user, aaudio_result_t err) {
    (void)st; (void)err;
    atomic_fetch_or(&((mai_stream *)user)->pending, MAI_PF_DEVICE_LOST);
}

/* ── Open / close (g_lock held) ────────────────────────────────────────── */

/* The AAudio input preset for MAI_PRESET_*; 0: leave AAudio's default
 * (VOICE_RECOGNITION). */
static aaudio_input_preset_t input_preset(int preset) {
    switch (preset) {
    case MAI_PRESET_GENERIC: return AAUDIO_INPUT_PRESET_GENERIC;
    case MAI_PRESET_PERFORMANCE:
        return g_api >= 29 ? AAUDIO_INPUT_PRESET_VOICE_PERFORMANCE : AAUDIO_INPUT_PRESET_VOICE_RECOGNITION;
    case MAI_PRESET_UNPROCESSED: return AAUDIO_INPUT_PRESET_UNPROCESSED;
    case MAI_PRESET_VOICE: return AAUDIO_INPUT_PRESET_VOICE_COMMUNICATION;
    default: return 0;
    }
}

/* One AAudio stream: `cb` its data callback (NULL: read with
 * AAudioStream_read), at `rate` (0: the device's) on `device` (0: the
 * system's choice). */
static aaudio_result_t try_open(mai_stream *s, aaudio_direction_t dir, AAudioStream_dataCallback cb, int32_t rate,
                                int32_t device, aaudio_sharing_mode_t mode, AAudioStream **out) {
    AAudioStreamBuilder *b;
    aaudio_result_t r = A.createStreamBuilder(&b);
    if (r != AAUDIO_OK) return r;
    bool input = dir == AAUDIO_DIRECTION_INPUT;
    A.setDirection(b, dir);
    A.setFormat(b, AAUDIO_FORMAT_PCM_FLOAT);
    A.setChannelCount(b, input ? s->want_in_channels : s->want_channels);
    A.setSampleRate(b, rate > 0 ? rate : AAUDIO_UNSPECIFIED);
    A.setPerformanceMode(b, AAUDIO_PERFORMANCE_MODE_LOW_LATENCY);
    A.setSharingMode(b, mode);
    if (device > 0) A.setDeviceId(b, device);
    if (input) {
        aaudio_input_preset_t preset = input_preset(s->preset);
        if (preset && A.setInputPreset) A.setInputPreset(b, preset);
    } else {
        if (A.setUsage) A.setUsage(b, AAUDIO_USAGE_MEDIA);
        if (A.setContentType) A.setContentType(b, AAUDIO_CONTENT_TYPE_MUSIC);
    }
    if (cb) A.setDataCallback(b, cb, s);
    A.setErrorCallback(b, on_error, s);
    r = A.openStream(b, out);
    A.builderDelete(b);
    return r;
}

/* Exclusive first when asked (falling back to shared), else shared. */
static aaudio_result_t open_one(mai_stream *s, aaudio_direction_t dir, AAudioStream_dataCallback cb, int32_t rate,
                                int32_t device, AAudioStream **out) {
    aaudio_result_t r = AAUDIO_ERROR_INTERNAL;
    if (s->sharing == MAI_SHARING_EXCLUSIVE) r = try_open(s, dir, cb, rate, device, AAUDIO_SHARING_MODE_EXCLUSIVE, out);
    if (r != AAUDIO_OK) r = try_open(s, dir, cb, rate, device, AAUDIO_SHARING_MODE_SHARED, out);
    return r;
}

/* The buffer's length in ns: the latency estimate below API 31. */
static void set_buffer_latency(mai_stream *s) {
    if (s->rate > 0) atomic_store(&s->out_latency_ns, frames_ns(s, s->buffer));
}

/* A device option ("" or an AAudio id in decimal) into `*id` (0: none). */
static bool parse_device(mai_stream *s, const char *want, const char *what, int32_t *id) {
    *id = 0;
    if (!want[0]) return true;
    char *end;
    long long v = strtoll(want, &end, 10);
    if (*end || v <= 0 || v > INT32_MAX) {
        char buf[MAI_ERRMAX];
        snprintf(buf, sizeof buf, "%s device \"%.64s\" is not an AAudio device id", what, want);
        mai__set_error(s, buf);
        return false;
    }
    *id = (int32_t)v;
    return true;
}

/* What needs Java before a stream opens: RECORD_AUDIO for input, and that
 * the devices asked for are AudioManager's. Without Java (no JVM or
 * activity) AAudio's open decides instead. */
static const char *java_checks(mai_stream *s, int32_t *out_dev, int32_t *in_dev) {
    const char *out_want = (s->dir & MAI_DIR_OUT) ? s->want_device : "";
    const char *in_want = s->dir == MAI_DIR_IN ? s->want_device : s->dir == MAI_DIR_DUPLEX ? s->want_in_device : "";
    if (!parse_device(s, out_want, "output", out_dev) || !parse_device(s, in_want, "input", in_dev))
        return "invalid_device";
    if (!(s->dir & MAI_DIR_IN) && !*out_dev) return NULL;
    bool attached;
    JNIEnv *j = jni_env(&attached);
    if (!j || !g_activity) {
        jni_done(attached);
        return NULL;
    }
    const char *err = NULL;
    char buf[MAI_ERRMAX];
    if ((*j)->PushLocalFrame(j, 16) == JNI_OK) {
        if ((s->dir & MAI_DIR_IN) && !mic_permitted(j)) {
            mai__set_error(s, "android.permission.RECORD_AUDIO is not granted");
            err = "permission";
        } else if (*out_dev && device_listed(j, GET_DEVICES_OUTPUTS, *out_dev) == 0) {
            snprintf(buf, sizeof buf, "no output device %d (AudioManager.getDevices)", (int)*out_dev);
            mai__set_error(s, buf);
            err = "invalid_device";
        } else if (*in_dev && device_listed(j, GET_DEVICES_INPUTS, *in_dev) == 0) {
            snprintf(buf, sizeof buf, "no input device %d (AudioManager.getDevices)", (int)*in_dev);
            mai__set_error(s, buf);
            err = "invalid_device";
        }
        (*j)->PopLocalFrame(j, NULL);
    } else {
        jni_failed(j);
    }
    jni_done(attached);
    return err;
}

static void close_streams(AAudioStream *out, AAudioStream *in) {
    /* (close stops a stream first; no callback runs once it returns) */
    if (out) A.close(out);
    if (in) A.close(in);
}

static void duplex_free(android_side *d) {
    free(d->ring);
    free(d->block);
    d->ring = d->block = NULL;
    d->ring_frames = d->block_frames = 0;
}

/* Duplex, once both streams are open: the ring, the block and the
 * cushion. The ring holds the limit, the largest block and a burst more,
 * twice over. */
static bool duplex_alloc(mai_stream *s, android_side *d) {
    int32_t block = s->capacity;
    if (block < 8 * s->burst) block = 8 * s->burst;
    if (block < 1024) block = 1024;
    d->keep = d->in_burst > s->burst ? d->in_burst : s->burst;
    d->limit = d->keep + d->in_burst + s->buffer;
    d->ring_frames = 2 * (d->limit + block + d->in_burst);
    d->block_frames = block;
    d->ring_w = d->ring_r = 0;
    d->primed = false;
    d->ring = malloc(sizeof(float) * (size_t)d->ring_frames * (size_t)s->in_channels);
    d->block = malloc(sizeof(float) * (size_t)block * (size_t)s->in_channels);
    if (d->ring && d->block) return true;
    duplex_free(d);
    return false;
}

static const char *be_open(mai_stream *s) {
    android_side *d = side_of(s);
    bool want_out = (s->dir & MAI_DIR_OUT) != 0, want_in = (s->dir & MAI_DIR_IN) != 0;
    int32_t out_dev, in_dev;
    const char *err = java_checks(s, &out_dev, &in_dev);
    if (err) return err;

    AAudioStream *o = NULL, *i = NULL;
    aaudio_result_t r;
    if (want_out) {
        r = open_one(s, AAUDIO_DIRECTION_OUTPUT, want_in ? on_duplex : on_data, s->want_rate, out_dev, &o);
        if (r != AAUDIO_OK) {
            set_aaudio_error(s, "open", r);
            return "unavailable";
        }
        s->rate = A.getSampleRate(o);
        s->channels = A.getChannelCount(o);
        s->burst = A.getFramesPerBurst(o);
        s->capacity = A.getBufferCapacityInFrames(o);
        s->exclusive = A.getSharingMode(o) == AAUDIO_SHARING_MODE_EXCLUSIVE;
        s->device_id = A.getDeviceId(o);
        int32_t want = s->want_frames > 0 ? s->want_frames : 2 * s->burst; /* double buffering */
        if (want > 0) A.setBufferSizeInFrames(o, want);
        s->buffer = A.getBufferSizeInFrames(o);
        set_buffer_latency(s);
    }
    if (want_in) {
        /* duplex: no callback, at the output's rate (AAudio resamples) */
        r = open_one(s, AAUDIO_DIRECTION_INPUT, want_out ? NULL : on_in_data, want_out ? s->rate : s->want_rate, in_dev,
                     &i);
        if (r != AAUDIO_OK) {
            close_streams(o, NULL);
            set_aaudio_error(s, want_out ? "open input" : "open", r);
            return "unavailable";
        }
        if (want_out && A.getSampleRate(i) != s->rate) {
            char buf[MAI_ERRMAX];
            snprintf(buf, sizeof buf, "the input runs at %d Hz, the output at %d Hz", (int)A.getSampleRate(i),
                     (int)s->rate);
            close_streams(o, i);
            mai__set_error(s, buf);
            return "unavailable";
        }
        s->in_channels = A.getChannelCount(i);
        s->in_device_id = A.getDeviceId(i);
        s->preset_granted = A.getInputPreset ? A.getInputPreset(i) : AAUDIO_INPUT_PRESET_VOICE_RECOGNITION;
        d->in_burst = A.getFramesPerBurst(i);
        bool in_exclusive = A.getSharingMode(i) == AAUDIO_SHARING_MODE_EXCLUSIVE;
        if (want_out) {
            s->exclusive = s->exclusive && in_exclusive;
        } else {
            s->rate = A.getSampleRate(i);
            s->burst = d->in_burst;
            s->capacity = A.getBufferCapacityInFrames(i);
            s->buffer = A.getBufferSizeInFrames(i);
            s->exclusive = in_exclusive;
            s->device_id = 0;
        }
        atomic_store(&s->in_latency_ns, frames_ns(s, d->in_burst));
    }
    if (want_out && want_in && !duplex_alloc(s, d)) {
        close_streams(o, i);
        mai__set_error(s, "no memory for the input ring");
        return "unavailable";
    }
    atomic_store(&d->waiting, 0);
    /* AAudioStream_getTimestamp only from API 31 (see be_latency) */
    s->present = g_api >= 31 ? MAI_PRESENT_TIMESTAMP : MAI_PRESENT_ESTIMATE;
    s->be = want_out ? o : i;
    s->be2 = want_out ? i : NULL;
    return NULL;
}

/* Duplex: the input first, so the first output callback finds it running. */
static const char *be_start(mai_stream *s) {
    aaudio_result_t r;
    if (s->be2 && (r = A.requestStart((AAudioStream *)s->be2)) != AAUDIO_OK) {
        set_aaudio_error(s, "start input", r);
        return "unavailable";
    }
    if ((r = A.requestStart((AAudioStream *)s->be)) != AAUDIO_OK) {
        set_aaudio_error(s, "start", r);
        return "unavailable";
    }
    return NULL;
}

/* The output (whose callback reads the input) first, then the input. */
static void be_close(mai_stream *s) {
    android_side *d = side_of(s);
    close_streams(s->be, s->be2);
    s->be = s->be2 = NULL;
    duplex_free(d);
}

static int32_t be_xruns(mai_stream *s) {
    int32_t x = s->be ? A.getXRunCount((AAudioStream *)s->be) : 0;
    if (s->be2) x += A.getXRunCount((AAudioStream *)s->be2);
    return x;
}

/* After new underruns: a burst more of output buffer, up to the capacity
 * (Magpie's tune_buffer). An app that asked for a fixed size keeps it; an
 * input's buffer is AAudio's. */
static void be_tune(mai_stream *s) {
    AAudioStream *st = s->be;
    if (!st || !(s->dir & MAI_DIR_OUT) || s->want_frames > 0 || s->burst <= 0) return;
    int32_t size = A.getBufferSizeInFrames(st);
    if (size + s->burst <= s->capacity) {
        A.setBufferSizeInFrames(st, size + s->burst);
        s->buffer = A.getBufferSizeInFrames(st);
        set_buffer_latency(s);
    }
}

/* API 31+: the anchor from AAudio's timestamp of s->be: the stream's frame
 * position `pos` is heard (output) or was captured (input) at `ns`; the
 * stream started at be_frame0. */
static void be_poll(mai_stream *s) {
    AAudioStream *st = s->be;
    int64_t pos, ns;
    if (!st) return;
    if (s->present != MAI_PRESENT_TIMESTAMP) return;
    if (A.getTimestamp(st, CLOCK_MONOTONIC, &pos, &ns) == AAUDIO_OK && ns > 0) mai__set_anchor(s, s->be_frame0 + pos, ns);
}

/* AAudioStream_getTimestamp aborts the process on Android 11's legacy
 * (AudioTrack) path: SIGABRT in AudioStreamLegacy::getBestTimestamp, in
 * both FMutate and Magpie on the moto g power 2021 (its crash buffer,
 * 2026-11-20).
 *
 * Output: below API 31 only the frames written but not yet read by the
 * device; from 31 on, the frames written but not yet presented (fm1audio's
 * stats). Input streams: from 31 on, how long ago the frame the app reads
 * next was captured (Oboe's calculateLatencyMillis); below 31, a burst plus
 * the frames waiting in AAudio's buffer (at most its capacity: the device's
 * position also counts frames an overrun lost). Duplex input: a burst plus
 * the frames waiting in the ring after the last callback. */
static void be_latency(mai_stream *s, double *out_ms, double *in_ms) {
    int64_t pos, ns;
    *in_ms = -1;
    *out_ms = -1;
    if (!s->be || s->rate <= 0) return;
    if (s->dir & MAI_DIR_OUT) {
        AAudioStream *st = s->be;
        int64_t written = A.getFramesWritten(st);
        if (g_api < 31) {
            *out_ms = (double)(written - A.getFramesRead(st)) * 1000.0 / s->rate;
        } else if (A.getTimestamp(st, CLOCK_MONOTONIC, &pos, &ns) == AAUDIO_OK) {
            double since = (mai_now_ns() - ns) / 1e9;
            *out_ms = ((double)(written - pos) / s->rate - since) * 1000.0;
        }
    }
    if (s->dir & MAI_DIR_IN) {
        if (s->dir == MAI_DIR_DUPLEX) {
            int32_t waiting = atomic_load_explicit(&side_of(s)->waiting, memory_order_relaxed);
            *in_ms = (double)(waiting + side_of(s)->in_burst) * 1000.0 / s->rate;
        } else if (g_api >= 31) {
            if (A.getTimestamp(s->be, CLOCK_MONOTONIC, &pos, &ns) == AAUDIO_OK) {
                int64_t read = A.getFramesRead(s->be);
                double ms = (mai_now_ns() - ns) / 1e6 - (double)(read - pos) * 1000.0 / s->rate;
                *in_ms = ms > 0 ? ms : 0;
            }
        } else {
            int64_t waiting = A.getFramesWritten(s->be) - A.getFramesRead(s->be);
            if (waiting < 0) waiting = 0;
            if (waiting > s->capacity) waiting = s->capacity;
            *in_ms = (double)(waiting + side_of(s)->in_burst) * 1000.0 / s->rate;
        }
    }
}

static void be_idle(void) {}

static ERL_NIF_TERM be_session_term(ErlNifEnv *env) { return atom(env, "nil"); }
