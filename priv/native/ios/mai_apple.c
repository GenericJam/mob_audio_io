/*
 * mai_apple.c: the plain-C half of the iOS backend, the part that needs only
 * AudioToolbox, so the macOS host test (test/c/mai_apple_test.c) runs it
 * against real CoreAudio:
 *
 *   callbacks  the output / duplex render callback and the input callback,
 *              on the audio thread: no lock, no allocation, no ObjC.
 *   the unit   RemoteIO / VoiceProcessingIO setup (also macOS's HALOutput):
 *              which buses run, the client formats, the callbacks.
 *   session    which AVAudioSession configuration the open streams need
 *              (mob_audio_io_nif.m applies it).
 *   decode     ExtAudioFile into a mai_pcm.
 *
 * mob_audio_io_nif.m includes it after ../common/mai_nif.c; the host test
 * after ../common/mai_core.c.
 */
#include <AudioToolbox/AudioToolbox.h>
#include <mach/mach_time.h>
#include <stdio.h>
#include <sys/stat.h>

#define MAX_FRAMES_PER_SLICE 4096 /* iOS renders 4096 frames at a time with the screen locked */
#define MAI_APPLE_OUT_BUS 0
#define MAI_APPLE_IN_BUS 1

static mach_timebase_info_data_t g_timebase;

static void mai_apple_timebase(void) {
    if (!g_timebase.denom) mach_timebase_info(&g_timebase);
}

/* A unit and what its callbacks need, the refcon of both callbacks. */
typedef struct mai_apple_io {
    mai_stream *s;
    AudioUnit unit;
    /* Where the input is pulled from: the unit's bus 1 (the host test points
     * it at another unit). */
    AudioUnit in_unit;
    UInt32 in_bus;
    float *in; /* in_cap frames * s->in_channels, allocated at open */
    UInt32 in_cap;
    AudioBufferList in_abl;
} mai_apple_io;

/* An AudioTimeStamp's host time on mai_now_ns()'s clock, or 0. */
static int64_t mai_apple_host_ns(const AudioTimeStamp *ts) {
    if (!ts || !(ts->mFlags & kAudioTimeStampHostTimeValid)) return 0;
    return mai__ticks_to_ns(ts->mHostTime, g_timebase.numer, g_timebase.denom);
}

/* A gap in the device's sample time: frames lost (an xrun). */
static void mai_apple_count_gap(mai_stream *s, const AudioTimeStamp *ts, UInt32 n) {
    if (!ts || !(ts->mFlags & kAudioTimeStampSampleTimeValid)) return;
    if (s->last_sample_time >= 0 && fabs(ts->mSampleTime - (s->last_sample_time + s->last_frames)) > 0.5)
        atomic_fetch_add_explicit(&s->be_xruns, 1, memory_order_relaxed);
    s->last_sample_time = ts->mSampleTime;
    s->last_frames = n;
}

/* Pull n input frames into io->in: the frames the unit gave, with the rest
 * zeroed (all of them when the render failed). The simulator's input can
 * come a frame short of its output's 557/558-frame blocks. */
static UInt32 mai_apple_pull_input(mai_apple_io *io, AudioUnitRenderActionFlags *flags, const AudioTimeStamp *ts,
                                   UInt32 n) {
    UInt32 frame_bytes = (UInt32)io->s->in_channels * (UInt32)sizeof(float), bytes = n * frame_bytes;
    io->in_abl.mNumberBuffers = 1;
    io->in_abl.mBuffers[0].mNumberChannels = (UInt32)io->s->in_channels;
    io->in_abl.mBuffers[0].mDataByteSize = bytes;
    io->in_abl.mBuffers[0].mData = io->in;
    AudioUnitRenderActionFlags f = flags ? *flags : 0;
    UInt32 got = 0;
    if (AudioUnitRender(io->in_unit, &f, ts, io->in_bus, n, &io->in_abl) == noErr)
        got = io->in_abl.mBuffers[0].mDataByteSize / frame_bytes;
    if (got > n) got = n;
    if (got < n) memset((char *)io->in + got * frame_bytes, 0, (n - got) * frame_bytes);
    return got;
}

/* Output and duplex: the unit asks for n frames of bus 0. A duplex stream
 * first pulls the same n frames of input from bus 1. */
static OSStatus mai_apple_render(void *ref, AudioUnitRenderActionFlags *flags, const AudioTimeStamp *ts, UInt32 bus,
                                 UInt32 n, AudioBufferList *io_data) {
    (void)bus;
    mai_apple_io *io = ref;
    mai_stream *s = io->s;
    int64_t t0 = mai_now_ns();
    float *out = io_data->mBuffers[0].mData;
    if (!out || io_data->mBuffers[0].mDataByteSize < n * (UInt32)s->channels * sizeof(float)) return noErr;
    mai_apple_count_gap(s, ts, n);
    int64_t host = mai_apple_host_ns(ts);
    int64_t out_present = host ? host + atomic_load_explicit(&s->out_latency_ns, memory_order_relaxed) : 0;
    const float *in = NULL;
    int64_t in_present = 0;
    if (s->dir & MAI_DIR_IN) {
        if (n > io->in_cap) { /* (never: MaximumFramesPerSlice is the capacity) */
            memset(out, 0, n * (UInt32)s->channels * sizeof(float));
            atomic_fetch_add_explicit(&s->in_underflows, 1, memory_order_relaxed);
            return noErr;
        }
        if (mai_apple_pull_input(io, flags, ts, n) < n) /* short: the rest is zeros */
            atomic_fetch_add_explicit(&s->in_underflows, 1, memory_order_relaxed);
        in = io->in;
        in_present = host ? host - atomic_load_explicit(&s->in_latency_ns, memory_order_relaxed) : 0;
    }
    mai__process(s, in, out, (int32_t)n, t0, in_present, out_present);
    return noErr;
}

/* Input: n frames arrived on bus 1; render them into io->in. */
static OSStatus mai_apple_input(void *ref, AudioUnitRenderActionFlags *flags, const AudioTimeStamp *ts, UInt32 bus,
                                UInt32 n, AudioBufferList *io_data) {
    (void)bus; (void)io_data;
    mai_apple_io *io = ref;
    mai_stream *s = io->s;
    int64_t t0 = mai_now_ns();
    mai_apple_count_gap(s, ts, n);
    if (n > io->in_cap) {
        atomic_fetch_add_explicit(&s->be_xruns, 1, memory_order_relaxed);
        return noErr;
    }
    UInt32 got = mai_apple_pull_input(io, flags, ts, n);
    if (got < n) atomic_fetch_add_explicit(&s->be_xruns, 1, memory_order_relaxed); /* frames lost */
    if (got == 0) return noErr;
    int64_t host = mai_apple_host_ns(ts);
    int64_t in_present = host ? host - atomic_load_explicit(&s->in_latency_ns, memory_order_relaxed) : 0;
    mai__process(s, io->in, NULL, (int32_t)got, t0, in_present, 0);
    return noErr;
}

/* ── The unit ──────────────────────────────────────────────────────────── */

static void mai_apple_status(char *buf, size_t n, const char *what, OSStatus r) {
    uint32_t u = (uint32_t)r;
    char cc[5] = {(char)(u >> 24), (char)(u >> 16), (char)(u >> 8), (char)u, 0};
    bool printable = true;
    for (int i = 0; i < 4; i++)
        if (cc[i] < 0x20 || cc[i] > 0x7e) printable = false;
    if (printable) snprintf(buf, n, "%s: '%s' (%d)", what, cc, (int)r);
    else snprintf(buf, n, "%s: %d", what, (int)r);
}

/* The callbacks' state for s, with its input buffer when s has input. NULL
 * when out of memory. */
static mai_apple_io *mai_apple_io_new(mai_stream *s) {
    mai_apple_io *io = calloc(1, sizeof *io);
    if (!io) return NULL;
    io->s = s;
    if (s->dir & MAI_DIR_IN) {
        io->in = calloc((size_t)MAX_FRAMES_PER_SLICE * (size_t)s->in_channels, sizeof(float));
        if (!io->in) {
            free(io);
            return NULL;
        }
        io->in_cap = MAX_FRAMES_PER_SLICE;
        io->in_bus = MAI_APPLE_IN_BUS;
    }
    return io;
}

/* Stop, uninitialize and dispose of the unit (no callback runs once
 * AudioOutputUnitStop returns), then free io. */
static void mai_apple_io_free(mai_apple_io *io) {
    if (!io) return;
    if (io->unit) {
        AudioOutputUnitStop(io->unit);
        AudioUnitUninitialize(io->unit);
        AudioComponentInstanceDispose(io->unit);
    }
    free(io->in);
    free(io);
}

/* A new instance of an Apple output unit (kAudioUnitSubType_RemoteIO,
 * _VoiceProcessingIO, macOS _HALOutput, _GenericOutput) into io->unit. */
static OSStatus mai_apple_unit_new(mai_apple_io *io, OSType subtype, char *err, size_t err_n) {
    AudioComponentDescription d = {kAudioUnitType_Output, subtype, kAudioUnitManufacturer_Apple, 0, 0};
    AudioComponent comp = AudioComponentFindNext(NULL, &d);
    if (!comp) {
        snprintf(err, err_n, "no '%c%c%c%c' output unit", (char)(subtype >> 24), (char)(subtype >> 16),
                 (char)(subtype >> 8), (char)subtype);
        return kAudioUnitErr_InvalidElement;
    }
    OSStatus r = AudioComponentInstanceNew(comp, &io->unit);
    if (r) {
        io->unit = NULL;
        mai_apple_status(err, err_n, "AudioComponentInstanceNew", r);
    }
    if (!io->in_unit) io->in_unit = io->unit;
    return r;
}

/* Enable the IO unit's input bus for `dir` & IN and its output bus for
 * `dir` & OUT (an input stream runs with the output bus off). */
static OSStatus mai_apple_enable_io(mai_apple_io *io, int dir, char *err, size_t err_n) {
    UInt32 in_on = (dir & MAI_DIR_IN) ? 1 : 0, out_on = (dir & MAI_DIR_OUT) ? 1 : 0;
    OSStatus r = AudioUnitSetProperty(io->unit, kAudioOutputUnitProperty_EnableIO, kAudioUnitScope_Input,
                                      MAI_APPLE_IN_BUS, &in_on, sizeof in_on);
    if (r) {
        mai_apple_status(err, err_n, "EnableIO (input)", r);
        return r;
    }
    r = AudioUnitSetProperty(io->unit, kAudioOutputUnitProperty_EnableIO, kAudioUnitScope_Output, MAI_APPLE_OUT_BUS,
                             &out_on, sizeof out_on);
    if (r) mai_apple_status(err, err_n, "EnableIO (output)", r);
    return r;
}

/* Float32 interleaved: what every callback buffer is. */
static AudioStreamBasicDescription mai_apple_float_format(double rate, int32_t channels) {
    AudioStreamBasicDescription f;
    memset(&f, 0, sizeof f);
    f.mSampleRate = rate;
    f.mFormatID = kAudioFormatLinearPCM;
    f.mFormatFlags = kAudioFormatFlagIsFloat | kAudioFormatFlagIsPacked;
    f.mChannelsPerFrame = (UInt32)channels;
    f.mBitsPerChannel = 32;
    f.mBytesPerFrame = 4 * (UInt32)channels;
    f.mFramesPerPacket = 1;
    f.mBytesPerPacket = f.mBytesPerFrame;
    return f;
}

/* Client formats (float interleaved at `rate`, the hardware's: the unit
 * doesn't resample) and callbacks for the buses in `buses` (MAI_DIR_* bits:
 * OUT is bus 0 with the render callback, which pulls the input for a duplex
 * stream; IN alone is bus 1 with the input callback), then initialize. */
static OSStatus mai_apple_configure(mai_apple_io *io, int buses, double rate, char *err, size_t err_n) {
    mai_stream *s = io->s;
    OSStatus r = noErr;
    const char *what = "";
    UInt32 max_frames = MAX_FRAMES_PER_SLICE, no = 0;
    if (buses & MAI_DIR_IN) {
        AudioStreamBasicDescription f = mai_apple_float_format(rate, s->in_channels);
        if ((r = AudioUnitSetProperty(io->unit, kAudioUnitProperty_StreamFormat, kAudioUnitScope_Output,
                                      MAI_APPLE_IN_BUS, &f, sizeof f))) {
            what = "StreamFormat (input)";
            goto fail;
        }
        /* the callbacks render into io->in */
        AudioUnitSetProperty(io->unit, kAudioUnitProperty_ShouldAllocateBuffer, kAudioUnitScope_Output,
                             MAI_APPLE_IN_BUS, &no, sizeof no);
    }
    if (buses & MAI_DIR_OUT) {
        AudioStreamBasicDescription f = mai_apple_float_format(rate, s->channels);
        AURenderCallbackStruct cb = {mai_apple_render, io};
        if ((r = AudioUnitSetProperty(io->unit, kAudioUnitProperty_StreamFormat, kAudioUnitScope_Input,
                                      MAI_APPLE_OUT_BUS, &f, sizeof f))) {
            what = "StreamFormat (output)";
            goto fail;
        }
        if ((r = AudioUnitSetProperty(io->unit, kAudioUnitProperty_SetRenderCallback, kAudioUnitScope_Input,
                                      MAI_APPLE_OUT_BUS, &cb, sizeof cb))) {
            what = "SetRenderCallback";
            goto fail;
        }
    } else if (buses & MAI_DIR_IN) {
        AURenderCallbackStruct cb = {mai_apple_input, io};
        if ((r = AudioUnitSetProperty(io->unit, kAudioOutputUnitProperty_SetInputCallback, kAudioUnitScope_Global,
                                      MAI_APPLE_IN_BUS, &cb, sizeof cb))) {
            what = "SetInputCallback";
            goto fail;
        }
    }
    if ((r = AudioUnitSetProperty(io->unit, kAudioUnitProperty_MaximumFramesPerSlice, kAudioUnitScope_Global, 0,
                                  &max_frames, sizeof max_frames))) {
        what = "MaximumFramesPerSlice";
        goto fail;
    }
    if ((r = AudioUnitInitialize(io->unit))) {
        what = "AudioUnitInitialize";
        goto fail;
    }
    return noErr;
fail:
    mai_apple_status(err, err_n, what, r);
    return r;
}

/* ── The session the open streams need ─────────────────────────────────── */

enum { MAI_APPLE_MODE_DEFAULT = 0, MAI_APPLE_MODE_MEASUREMENT = 1, MAI_APPLE_MODE_VOICE_CHAT = 2 };

typedef struct {
    bool record;    /* PlayAndRecord (DefaultToSpeaker | AllowBluetoothA2DP), else Playback */
    bool mix;       /* MixWithOthers */
    int mode;       /* MAI_APPLE_MODE_* */
    double io_s;    /* preferred IO buffer duration */
    int32_t rate;   /* preferred sample rate; 0: leave it */
    int streams;    /* how many streams it was made from */
} mai_apple_session;

/* One AVAudioSession serves every stream: configure it from all the open
 * ones (`streams`, `n` slots; only those with `open` count). Any input, or
 * a :play_and_record stream, makes it PlayAndRecord; it mixes only if every
 * stream asked for :mix. An input with preset :voice makes the mode
 * VoiceChat, else one with :unprocessed makes it Measurement. The IO buffer
 * is the shortest any stream asked for (5 ms for low latency, else its
 * frames at `hw_rate`); the rate is the one the newest stream (the highest
 * generation: the one opening) asked for, 0 when none did. */
static mai_apple_session mai_apple_session_for(mai_stream *streams, int n, double hw_rate) {
    mai_apple_session p = {false, true, MAI_APPLE_MODE_DEFAULT, 0, 0, 0};
    if (hw_rate <= 0) hw_rate = 48000;
    bool voice = false, measurement = false;
    uint32_t rate_gen = 0;
    for (int i = 0; i < n; i++) {
        mai_stream *s = &streams[i];
        if (!atomic_load(&s->open)) continue;
        p.streams++;
        if ((s->dir & MAI_DIR_IN) || s->session == MAI_SESSION_PLAY_AND_RECORD) p.record = true;
        if (s->session != MAI_SESSION_MIX) p.mix = false;
        if (s->dir & MAI_DIR_IN) {
            if (s->preset == MAI_PRESET_VOICE) voice = true;
            if (s->preset == MAI_PRESET_UNPROCESSED) measurement = true;
        }
        double io = s->want_frames > 0 ? s->want_frames / hw_rate : 0.005;
        if (p.io_s == 0 || io < p.io_s) p.io_s = io;
        if (s->want_rate > 0 && s->gen >= rate_gen) {
            p.rate = s->want_rate;
            rate_gen = s->gen;
        }
    }
    if (!p.streams) p.mix = false;
    p.mode = voice ? MAI_APPLE_MODE_VOICE_CHAT : measurement ? MAI_APPLE_MODE_MEASUREMENT : MAI_APPLE_MODE_DEFAULT;
    if (p.io_s == 0) p.io_s = 0.005;
    return p;
}

static bool mai_apple_session_eq(const mai_apple_session *a, const mai_apple_session *b) {
    return a->record == b->record && a->mix == b->mix && a->mode == b->mode && fabs(a->io_s - b->io_s) < 1e-7 &&
           a->rate == b->rate;
}

/* ── Decode ────────────────────────────────────────────────────────────── */

#define MAI_APPLE_DECODE_CHUNK 4096 /* frames per ExtAudioFileRead */

static const char *mai_apple_pcm_error(int r) {
    return r == MAI_PCM_TOO_LARGE ? "too_large" : r == MAI_PCM_NO_MEMORY ? "no_memory" : "decode_error";
}

/* `path` (any file ExtAudioFile reads: WAV, AIFF, CAF, MP3, AAC/M4A, ALAC,
 * FLAC) into `pcm` as float32 interleaved, at the file's rate and channel
 * count. NULL or an error atom, with the OSStatus in `detail`. */
static const char *mai_apple_decode(const char *path, mai_pcm *pcm, int32_t *rate, char *detail, size_t detail_n) {
    struct stat st;
    if (stat(path, &st) != 0 || !S_ISREG(st.st_mode)) {
        snprintf(detail, detail_n, "no such file: %s", path);
        return "not_found";
    }
    CFURLRef url = CFURLCreateFromFileSystemRepresentation(NULL, (const UInt8 *)path, (CFIndex)strlen(path), false);
    if (!url) return "no_memory";
    ExtAudioFileRef f = NULL;
    OSStatus r = ExtAudioFileOpenURL(url, &f);
    CFRelease(url);
    if (r) {
        mai_apple_status(detail, detail_n, "ExtAudioFileOpenURL", r);
        return r == kAudioFilePermissionsError ? "not_found" : "unsupported";
    }
    const char *err = NULL;
    float *chunk = NULL;
    AudioStreamBasicDescription file_fmt;
    UInt32 size = sizeof file_fmt;
    if ((r = ExtAudioFileGetProperty(f, kExtAudioFileProperty_FileDataFormat, &size, &file_fmt))) {
        mai_apple_status(detail, detail_n, "FileDataFormat", r);
        err = "unsupported";
        goto done;
    }
    if (file_fmt.mChannelsPerFrame == 0 || file_fmt.mSampleRate <= 0) {
        err = "no_audio";
        goto done;
    }
    int32_t ch = (int32_t)file_fmt.mChannelsPerFrame;
    AudioStreamBasicDescription client = mai_apple_float_format(file_fmt.mSampleRate, ch);
    if ((r = ExtAudioFileSetProperty(f, kExtAudioFileProperty_ClientDataFormat, sizeof client, &client))) {
        mai_apple_status(detail, detail_n, "ClientDataFormat", r);
        err = "unsupported";
        goto done;
    }
    /* fail fast when the file's length says it won't fit */
    SInt64 length = 0;
    size = sizeof length;
    if (ExtAudioFileGetProperty(f, kExtAudioFileProperty_FileLengthFrames, &size, &length) == noErr && length > 0 &&
        (uint64_t)length * (uint64_t)(pcm->mono ? 1 : ch) * sizeof(float) > pcm->max_bytes) {
        snprintf(detail, detail_n, "%lld frames of %d channels", (long long)length, (int)ch);
        err = "too_large";
        goto done;
    }
    chunk = malloc((size_t)MAI_APPLE_DECODE_CHUNK * (size_t)ch * sizeof(float));
    if (!chunk) {
        err = "no_memory";
        goto done;
    }
    for (;;) {
        AudioBufferList abl;
        abl.mNumberBuffers = 1;
        abl.mBuffers[0].mNumberChannels = (UInt32)ch;
        abl.mBuffers[0].mDataByteSize = MAI_APPLE_DECODE_CHUNK * (UInt32)ch * (UInt32)sizeof(float);
        abl.mBuffers[0].mData = chunk;
        UInt32 frames = MAI_APPLE_DECODE_CHUNK;
        if ((r = ExtAudioFileRead(f, &frames, &abl))) {
            mai_apple_status(detail, detail_n, "ExtAudioFileRead", r);
            err = "decode_error";
            goto done;
        }
        if (frames == 0) break;
        int pr = mai__pcm_push_f32(pcm, chunk, frames, ch);
        if (pr != MAI_PCM_OK) {
            err = mai_apple_pcm_error(pr);
            goto done;
        }
    }
    *rate = (int32_t)lround(file_fmt.mSampleRate);
done:
    free(chunk);
    ExtAudioFileDispose(f);
    return err;
}
