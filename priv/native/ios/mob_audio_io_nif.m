/*
 * mob_audio_io_nif.m (iOS): RemoteIO output and the AVAudioSession for
 * mob_audio_io.
 *
 * Lifted from FMutate's c_src/fm1audio.c (the RemoteIO unit, xruns as gaps
 * in the device's sample time) and ios/FMutateAudioSession.swift, and Choir
 * Coach's choir_audio_ios.m (the play-and-record session options):
 *   - session: :play is the playback category (the silent switch doesn't
 *     mute it); :mix adds mixWithOthers; :play_and_record is PlayAndRecord
 *     with the speaker as default and Bluetooth A2DP allowed.
 *   - A preferred IO buffer of 5 ms (buffer: :low_latency) or the frames
 *     asked for; iOS rounds it to what the hardware does.
 *   - The unit runs float stereo at the hardware's own rate, so it doesn't
 *     resample.
 *   - Interruptions, route changes and media-services resets arrive as
 *     notifications on another thread, which only flag each open stream:
 *     the event thread (../common/mai_nif.c) closes the unit when an
 *     interruption begins, reopens it when it ends, and rebuilds it for a
 *     new rate or a reset.
 *   - The session is deactivated (other apps get their audio back) when the
 *     last stream closes.
 */
#import <AVFoundation/AVFoundation.h>
#import <UIKit/UIKit.h>
#include <AudioToolbox/AudioToolbox.h>
#include <os/log.h>
#include <stdio.h>

#include "../common/mai_nif.c"

#include <mach/mach_time.h>

#define MAX_FRAMES_PER_SLICE 4096 /* iOS renders 4096 frames at a time with the screen locked */

static double g_session_rate;   /* the rate the open units run at (g_lock) */
static bool g_session_active;   /* (g_lock) */
static _Atomic bool g_observing;
static mach_timebase_info_data_t g_timebase;

static const char *be_platform(void) { return "ios"; }

static const char *be_load(void) {
    if (!g_timebase.denom) mach_timebase_info(&g_timebase);
    return NULL;
}

/* The session's output latency in ns: what a frame waits after its
 * AudioTimeStamp.mHostTime (the IO cycle's output time) before it is heard
 * (the convention of Ableton Link's LinkHut and The Amazing Audio Engine;
 * latency_ms in status/0 adds the IO buffer, the wait from the render call). */
static int64_t session_out_latency_ns(void) {
    @autoreleasepool {
        AVAudioSession *as = [AVAudioSession sharedInstance];
        return (int64_t)(as.outputLatency * 1e9);
    }
}

/* ── Session notifications (any thread): flag every open stream ───────── */

static void flag_all(uint32_t pf) {
    for (int i = 0; i < MAI_MAX_STREAMS; i++)
        if (atomic_load(&g_streams[i].open)) atomic_fetch_or(&g_streams[i].pending, pf);
}

static void observe_session(void) {
    if (atomic_exchange(&g_observing, true)) return;
    NSNotificationCenter *nc = [NSNotificationCenter defaultCenter];
    AVAudioSession *as = [AVAudioSession sharedInstance];
    [nc addObserverForName:AVAudioSessionInterruptionNotification
                    object:as
                     queue:nil
                usingBlock:^(NSNotification *n) {
                  NSUInteger type = [n.userInfo[AVAudioSessionInterruptionTypeKey] unsignedIntegerValue];
                  if (type != AVAudioSessionInterruptionTypeBegan) {
                      flag_all(MAI_PF_RESUMED);
                      return;
                  }
                  /* A "began" delivered because the app was suspended
                   * (iOS 14.5+) is stale: the interruption may be long over
                   * and no "ended" will follow. Try to rebuild right away. */
                  NSNumber *reason = n.userInfo[AVAudioSessionInterruptionReasonKey];
                  bool suspended =
                      reason && reason.unsignedIntegerValue == AVAudioSessionInterruptionReasonAppWasSuspended;
                  flag_all(MAI_PF_INTERRUPTED | (suspended ? MAI_PF_FOREGROUND : 0));
                }];
    /* An interruption whose "ended" never came (the app was in the
     * background when it ended): try again when the app is back. */
    [nc addObserverForName:UIApplicationWillEnterForegroundNotification
                    object:nil
                     queue:nil
                usingBlock:^(NSNotification *n) {
                  (void)n;
                  flag_all(MAI_PF_FOREGROUND);
                }];
    [nc addObserverForName:AVAudioSessionRouteChangeNotification
                    object:as
                     queue:nil
                usingBlock:^(NSNotification *n) {
                  NSUInteger reason = [n.userInfo[AVAudioSessionRouteChangeReasonKey] unsignedIntegerValue];
                  /* (our own setCategory posts CategoryChange: not a new route) */
                  if (reason == AVAudioSessionRouteChangeReasonCategoryChange) return;
                  double rate = [AVAudioSession sharedInstance].sampleRate;
                  bool new_rate = g_session_rate > 0 && rate > 0 && fabs(rate - g_session_rate) > 0.5;
                  flag_all(MAI_PF_ROUTE | (new_rate ? MAI_PF_RESET : 0));
                }];
    [nc addObserverForName:AVAudioSessionMediaServicesWereResetNotification
                    object:as
                     queue:nil
                usingBlock:^(NSNotification *n) {
                  (void)n;
                  flag_all(MAI_PF_RESET);
                }];
}

static void set_ns_error(mai_stream *s, const char *what, NSError *err) {
    char buf[MAI_ERRMAX];
    snprintf(buf, sizeof buf, "%s: %s (%ld)", what, err ? err.localizedDescription.UTF8String : "failed",
             err ? (long)err.code : 0L);
    mai__set_error(s, buf);
}

/* (g_lock held) Configure and activate the session for s; NULL or an error atom. */
static const char *activate_session(mai_stream *s) {
    @autoreleasepool {
        AVAudioSession *as = [AVAudioSession sharedInstance];
        NSError *err = nil;
        AVAudioSessionCategory cat = AVAudioSessionCategoryPlayback;
        AVAudioSessionCategoryOptions opts = 0;
        if (s->session == MAI_SESSION_MIX) opts = AVAudioSessionCategoryOptionMixWithOthers;
        if (s->session == MAI_SESSION_PLAY_AND_RECORD) {
            cat = AVAudioSessionCategoryPlayAndRecord;
            opts = AVAudioSessionCategoryOptionDefaultToSpeaker | AVAudioSessionCategoryOptionAllowBluetoothA2DP;
        }
        if (![as setCategory:cat mode:AVAudioSessionModeDefault options:opts error:&err]) {
            set_ns_error(s, "setCategory", err);
            return "session";
        }
        if (s->want_rate > 0) [as setPreferredSampleRate:s->want_rate error:nil];
        double hw = as.sampleRate > 0 ? as.sampleRate : 48000;
        double io = s->want_frames > 0 ? s->want_frames / hw : 0.005;
        [as setPreferredIOBufferDuration:io error:nil];
        if (![as setActive:YES error:&err]) {
            set_ns_error(s, "setActive", err);
            return "session";
        }
        g_session_active = true;
        observe_session();
        return NULL;
    }
}

/* ── The unit ──────────────────────────────────────────────────────────── */

static OSStatus on_render(void *ref, AudioUnitRenderActionFlags *flags, const AudioTimeStamp *ts, UInt32 bus,
                          UInt32 n, AudioBufferList *io) {
    (void)flags; (void)bus;
    mai_stream *s = ref;
    int64_t t0 = mai_now_ns(), present = 0;
    if (ts && (ts->mFlags & kAudioTimeStampSampleTimeValid)) { /* a gap in the device's time: frames lost */
        if (s->last_sample_time >= 0 && fabs(ts->mSampleTime - (s->last_sample_time + s->last_frames)) > 0.5)
            atomic_fetch_add_explicit(&s->be_xruns, 1, memory_order_relaxed);
        s->last_sample_time = ts->mSampleTime;
        s->last_frames = n;
    }
    if (ts && (ts->mFlags & kAudioTimeStampHostTimeValid))
        present = mai__ticks_to_ns(ts->mHostTime, g_timebase.numer, g_timebase.denom) +
                  atomic_load_explicit(&s->out_latency_ns, memory_order_relaxed);
    mai__process(s, NULL, (float *)io->mBuffers[0].mData, (int32_t)n, t0, 0, present);
    return noErr;
}

static const char *be_open(mai_stream *s) {
    if (s->dir != MAI_DIR_OUT) {
        mai__set_error(s, "input streams are not in this build");
        return "unsupported";
    }
    const char *err = activate_session(s);
    if (err) return err;
    AudioComponentDescription d = {kAudioUnitType_Output, kAudioUnitSubType_RemoteIO, kAudioUnitManufacturer_Apple,
                                   0, 0};
    AudioComponent comp = AudioComponentFindNext(NULL, &d);
    AudioUnit unit = NULL;
    OSStatus r;
    char buf[MAI_ERRMAX];
    if (!comp) {
        mai__set_error(s, "no RemoteIO component");
        return "unavailable";
    }
    if ((r = AudioComponentInstanceNew(comp, &unit))) {
        snprintf(buf, sizeof buf, "AudioComponentInstanceNew: %d", (int)r);
        mai__set_error(s, buf);
        return "unavailable";
    }
    double rate, io_s;
    @autoreleasepool {
        AVAudioSession *as = [AVAudioSession sharedInstance];
        rate = as.sampleRate > 0 ? as.sampleRate : 48000;
        io_s = as.IOBufferDuration;
    }
    AudioStreamBasicDescription f;
    memset(&f, 0, sizeof f);
    f.mSampleRate = rate; /* the hardware's own: no resampling in the unit */
    f.mFormatID = kAudioFormatLinearPCM;
    f.mFormatFlags = kAudioFormatFlagIsFloat | kAudioFormatFlagIsPacked;
    f.mChannelsPerFrame = (UInt32)s->channels;
    f.mBitsPerChannel = 32;
    f.mBytesPerFrame = 4 * (UInt32)s->channels;
    f.mFramesPerPacket = 1;
    f.mBytesPerPacket = f.mBytesPerFrame;
    UInt32 max_frames = MAX_FRAMES_PER_SLICE;
    AURenderCallbackStruct cb = {on_render, s};
    if ((r = AudioUnitSetProperty(unit, kAudioUnitProperty_StreamFormat, kAudioUnitScope_Input, 0, &f, sizeof f)) ||
        (r = AudioUnitSetProperty(unit, kAudioUnitProperty_MaximumFramesPerSlice, kAudioUnitScope_Global, 0,
                                  &max_frames, sizeof max_frames)) ||
        (r = AudioUnitSetProperty(unit, kAudioUnitProperty_SetRenderCallback, kAudioUnitScope_Input, 0, &cb,
                                  sizeof cb)) ||
        (r = AudioUnitInitialize(unit))) {
        AudioComponentInstanceDispose(unit);
        snprintf(buf, sizeof buf, "RemoteIO setup: %d", (int)r);
        mai__set_error(s, buf);
        return "unavailable";
    }
    s->rate = (int32_t)lround(rate);
    s->burst = (int32_t)lround(io_s * rate);
    s->buffer = s->burst;
    s->capacity = MAX_FRAMES_PER_SLICE;
    s->exclusive = false;
    s->device_id = 0;
    s->present = MAI_PRESENT_TIMESTAMP;
    atomic_store(&s->out_latency_ns, session_out_latency_ns());
    atomic_store(&s->be_xruns, 0);
    g_session_rate = rate;
    s->be = unit;
    os_log(OS_LOG_DEFAULT, "[mob_audio_io] RemoteIO open: %.0f Hz, IO buffer %.2f ms", rate, io_s * 1000.0);
    return NULL;
}

static const char *be_start(mai_stream *s) {
    OSStatus r = AudioOutputUnitStart((AudioUnit)s->be);
    if (r == noErr) return NULL;
    char buf[MAI_ERRMAX];
    snprintf(buf, sizeof buf, "AudioOutputUnitStart: %d", (int)r);
    mai__set_error(s, buf);
    return "unavailable";
}

static void be_close(mai_stream *s) {
    AudioUnit unit = s->be;
    if (!unit) return;
    AudioOutputUnitStop(unit); /* no callback runs once it returns */
    AudioUnitUninitialize(unit);
    AudioComponentInstanceDispose(unit);
    s->be = NULL;
}

static int32_t be_xruns(mai_stream *s) { return (int32_t)atomic_load(&s->be_xruns); }

static void be_tune(mai_stream *s) { (void)s; }

/* The route's latency can change under a running unit (a headset): keep
 * the callback's copy current. */
static void be_poll(mai_stream *s) { atomic_store(&s->out_latency_ns, session_out_latency_ns()); }

/* What a frame waits after the render: one IO buffer and the output's latency. */
static void be_latency(mai_stream *s, double *out_ms, double *in_ms) {
    (void)s;
    @autoreleasepool {
        AVAudioSession *as = [AVAudioSession sharedInstance];
        *out_ms = (as.outputLatency + as.IOBufferDuration) * 1000.0;
        *in_ms = (as.inputLatency + as.IOBufferDuration) * 1000.0;
    }
}

static void be_idle(void) {
    if (!g_session_active) return;
    g_session_active = false;
    g_session_rate = 0;
    @autoreleasepool {
        [[AVAudioSession sharedInstance] setActive:NO
                                       withOptions:AVAudioSessionSetActiveOptionNotifyOthersOnDeactivation
                                             error:nil];
    }
}

/* status/0's session: what iOS granted (the latency probe's numbers). */
static ERL_NIF_TERM be_session_term(ErlNifEnv *env) {
    @autoreleasepool {
        AVAudioSession *as = [AVAudioSession sharedInstance];
        ERL_NIF_TERM outs = enif_make_list(env, 0);
        for (AVAudioSessionPortDescription *p in as.currentRoute.outputs.reverseObjectEnumerator)
            outs = enif_make_list_cell(env, string_term(env, p.portType.UTF8String), outs);
        ERL_NIF_TERM k[9], v[9];
        int i = 0;
#define KV(key, val) (k[i] = atom(env, key), v[i] = (val), i++)
        KV("active", atom(env, g_session_active ? "true" : "false"));
        KV("category", string_term(env, as.category.UTF8String));
        KV("rate", enif_make_double(env, as.sampleRate));
        KV("io_buffer_ms", enif_make_double(env, as.IOBufferDuration * 1000.0));
        KV("preferred_io_buffer_ms", enif_make_double(env, as.preferredIOBufferDuration * 1000.0));
        KV("output_latency_ms", enif_make_double(env, as.outputLatency * 1000.0));
        KV("input_latency_ms", enif_make_double(env, as.inputLatency * 1000.0));
        KV("input_available", atom(env, as.isInputAvailable ? "true" : "false"));
        KV("outputs", outs);
#undef KV
        return map_of(env, k, v, (unsigned)i);
    }
}

static ERL_NIF_TERM be_devices(ErlNifEnv *env, int dir) {
    (void)dir;
    return error_atom(env, "unsupported");
}

static const char *be_decode(const char *path, mai_pcm *pcm, int32_t *rate, char *detail, size_t detail_n) {
    (void)path; (void)pcm; (void)rate; (void)detail; (void)detail_n;
    return "unsupported";
}
