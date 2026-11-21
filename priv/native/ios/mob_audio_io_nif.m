/*
 * mob_audio_io_nif.m (iOS): RemoteIO / VoiceProcessingIO streams and the
 * AVAudioSession for mob_audio_io. The audio callbacks, the unit's setup,
 * the session policy and the ExtAudioFile decode are plain C in mai_apple.c
 * (the macOS host test runs them against real CoreAudio); this file is the
 * AVAudioSession half and the be_* backend of ../common/mai_nif.c.
 *
 * Lifted from FMutate's c_src/fm1audio.c (the RemoteIO unit, xruns as gaps
 * in the device's sample time) and ios/FMutateAudioSession.swift (mixing),
 * and Choir Coach's choir_audio_ios.m (the play-and-record options):
 *   - One session for every stream, configured from all the open ones
 *     (mai_apple_session_for): any input or :play_and_record stream makes
 *     it PlayAndRecord with the speaker as default and Bluetooth A2DP
 *     allowed, else Playback (the silent switch doesn't mute it); it mixes
 *     with other apps only if every stream asked for :mix; an input's
 *     preset picks the mode (VoiceChat, Measurement). Applied at each open
 *     and reopen, and from the event thread within 100 ms of a close.
 *   - A preferred IO buffer of 5 ms (buffer: :low_latency) or the frames
 *     asked for; iOS rounds it to what the hardware does.
 *   - Input needs the record permission already granted: the NIF never
 *     prompts (the app asks with Mob.Permissions).
 *   - The unit runs float interleaved at the hardware's own rate, so it
 *     doesn't resample: output on bus 0, input on bus 1 (an input stream
 *     turns bus 0 off; a duplex stream pulls bus 1 in the render callback).
 *     preset :voice is the VoiceProcessingIO unit (echo cancellation).
 *   - An input device is a port UID (setPreferredInput:); iOS routes the
 *     output itself, so an output device is :unsupported.
 *   - Interruptions, route changes and media-services resets arrive as
 *     notifications on another thread, which only flag each open stream:
 *     the event thread (../common/mai_nif.c) closes the unit when an
 *     interruption begins, reopens it when it ends, and rebuilds a stream
 *     whose unit runs at another rate than the session's now.
 *   - The session is deactivated (other apps get their audio back) when the
 *     last stream closes.
 */
#import <AVFoundation/AVFoundation.h>
#import <UIKit/UIKit.h>
#include <os/log.h>
#include <stdio.h>

#include "../common/mai_nif.c"
#include "mai_apple.c"

static bool g_session_active;          /* (g_lock) */
static bool g_session_applied;         /* g_applied is what the session was set to (g_lock) */
static mai_apple_session g_applied;    /* (g_lock) */
static _Atomic int32_t g_unit_rate[MAI_MAX_STREAMS]; /* each slot's running unit's rate, 0: none */
static _Atomic bool g_observing;

static const char *be_platform(void) { return "ios"; }

static const char *be_load(void) {
    mai_apple_timebase();
    return NULL;
}

/* What the callbacks add to (output) or take from (input) an
 * AudioTimeStamp's host time, the IO cycle's time at the hardware: the
 * session's output latency, what a frame waits after its mHostTime before
 * it is heard, and its input latency, how long before its mHostTime a
 * captured frame reached the microphone (the convention of Ableton Link's
 * LinkHut and The Amazing Audio Engine; latency_ms and input_latency_ms in
 * status/0 add the IO buffer). Refreshed in be_poll: the route changes. */
static void refresh_latencies(mai_stream *s) {
    @autoreleasepool {
        AVAudioSession *as = [AVAudioSession sharedInstance];
        atomic_store(&s->out_latency_ns, (int64_t)(as.outputLatency * 1e9));
        atomic_store(&s->in_latency_ns, (int64_t)(as.inputLatency * 1e9));
    }
}

/* ── Session notifications (any thread): flag every open stream ───────── */

static void flag_all(uint32_t pf) {
    for (int i = 0; i < MAI_MAX_STREAMS; i++)
        if (atomic_load(&g_streams[i].open)) atomic_fetch_or(&g_streams[i].pending, pf);
}

/* A new route (or category): every stream hears of it, and a stream whose
 * unit runs at another rate than the session's now is rebuilt (a headset
 * mic at 16 kHz, a USB interface). Our own setCategory posts
 * CategoryChange: only a new rate counts then. */
static void flag_route(bool category_change) {
    double rate = [AVAudioSession sharedInstance].sampleRate;
    for (int i = 0; i < MAI_MAX_STREAMS; i++) {
        if (!atomic_load(&g_streams[i].open)) continue;
        int32_t unit_rate = atomic_load(&g_unit_rate[i]);
        bool new_rate = unit_rate > 0 && rate > 0 && fabs(rate - unit_rate) > 0.5;
        if (category_change && !new_rate) continue;
        atomic_fetch_or(&g_streams[i].pending, MAI_PF_ROUTE | (new_rate ? MAI_PF_RESET : 0));
    }
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
                  /* (deprecated in iOS 16, which no longer sends it: iOS 15 does) */
#pragma clang diagnostic push
#pragma clang diagnostic ignored "-Wdeprecated-declarations"
                  bool suspended =
                      reason && reason.unsignedIntegerValue == AVAudioSessionInterruptionReasonAppWasSuspended;
#pragma clang diagnostic pop
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
                  flag_route(reason == AVAudioSessionRouteChangeReasonCategoryChange);
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
    if (s) mai__set_error(s, buf);
    else os_log(OS_LOG_DEFAULT, "[mob_audio_io] %{public}s", buf);
}

/* ── The session ───────────────────────────────────────────────────────── */

static AVAudioSessionMode mode_of(int mode) {
    return mode == MAI_APPLE_MODE_VOICE_CHAT    ? AVAudioSessionModeVoiceChat
           : mode == MAI_APPLE_MODE_MEASUREMENT ? AVAudioSessionModeMeasurement
                                                : AVAudioSessionModeDefault;
}

/* (g_lock held) Configure the session for every open stream, and activate
 * it when `activate` (an open). `s`: the stream opening, which gets the
 * error detail (NULL from the event thread). NULL or "session". */
static const char *apply_session(mai_stream *s, bool activate) {
    @autoreleasepool {
        AVAudioSession *as = [AVAudioSession sharedInstance];
        NSError *err = nil;
        mai_apple_session want = mai_apple_session_for(g_streams, MAI_MAX_STREAMS, as.sampleRate);
        bool fresh = !g_session_applied;
        if (fresh || want.record != g_applied.record || want.mix != g_applied.mix || want.mode != g_applied.mode) {
            AVAudioSessionCategory cat = want.record ? AVAudioSessionCategoryPlayAndRecord : AVAudioSessionCategoryPlayback;
            AVAudioSessionCategoryOptions opts = want.mix ? AVAudioSessionCategoryOptionMixWithOthers : 0;
            if (want.record)
                opts |= AVAudioSessionCategoryOptionDefaultToSpeaker | AVAudioSessionCategoryOptionAllowBluetoothA2DP;
            if (![as setCategory:cat mode:mode_of(want.mode) options:opts error:&err]) {
                set_ns_error(s, "setCategory", err);
                return "session";
            }
        }
        if (want.rate > 0 && (fresh || want.rate != g_applied.rate)) [as setPreferredSampleRate:want.rate error:nil];
        if (fresh || fabs(want.io_s - g_applied.io_s) > 1e-7) [as setPreferredIOBufferDuration:want.io_s error:nil];
        g_applied = want;
        g_session_applied = true;
        if (activate) {
            if (![as setActive:YES error:&err]) {
                set_ns_error(s, "setActive", err);
                return "session";
            }
            g_session_active = true;
            observe_session();
        }
        return NULL;
    }
}

/* follow_open_streams' pacing: one attempt per event-thread tick however
 * many streams poll, and a configuration that failed to apply isn't
 * tried again for FOLLOW_RETRY_NS (g_lock). */
#define FOLLOW_GAP_NS 50000000LL     /* under POLL_NS: once per 100 ms tick */
#define FOLLOW_RETRY_NS 2000000000LL
static int64_t g_follow_at;
static bool g_follow_failed;
static mai_apple_session g_follow_failed_want;
static int64_t g_follow_failed_until;

/* (g_lock held) An open stream may have closed since the session was set
 * (mai_nif.c has no close hook): bring the session back in line with the
 * streams still open. */
static void follow_open_streams(void) {
    if (!g_session_applied) return;
    int64_t now = mai_now_ns();
    if (now < g_follow_at) return;
    g_follow_at = now + FOLLOW_GAP_NS;
    double hw;
    @autoreleasepool {
        hw = [AVAudioSession sharedInstance].sampleRate;
    }
    mai_apple_session want = mai_apple_session_for(g_streams, MAI_MAX_STREAMS, hw);
    if (!want.streams || mai_apple_session_eq(&want, &g_applied)) return;
    if (g_follow_failed && mai_apple_session_eq(&want, &g_follow_failed_want) && now < g_follow_failed_until) return;
    g_follow_failed = apply_session(NULL, false) != NULL;
    g_follow_failed_want = want;
    g_follow_failed_until = now + FOLLOW_RETRY_NS;
}

/* The microphone, without prompting: the app asks (Mob.Permissions). */
static bool record_granted(void) {
    if (@available(iOS 17.0, *))
        return [AVAudioApplication sharedInstance].recordPermission == AVAudioApplicationRecordPermissionGranted;
    /* (iOS 15-16; deprecated from 17, which mob_dev's simulator build targets) */
#pragma clang diagnostic push
#pragma clang diagnostic ignored "-Wdeprecated-declarations"
    return [AVAudioSession sharedInstance].recordPermission == AVAudioSessionRecordPermissionGranted;
#pragma clang diagnostic pop
}

/* (g_lock held, the session PlayAndRecord and active) The input s asked for
 * (a port UID), or back to the system's choice when no open stream asks
 * for one. NULL or "invalid_device". */
static const char *prefer_input(mai_stream *s) {
    const char *uid = s->dir == MAI_DIR_DUPLEX ? s->want_in_device : s->want_device;
    AVAudioSession *as = [AVAudioSession sharedInstance];
    NSError *err = nil;
    if (!uid[0]) {
        for (int i = 0; i < MAI_MAX_STREAMS; i++) {
            mai_stream *t = &g_streams[i];
            if (t == s || !atomic_load(&t->open) || !(t->dir & MAI_DIR_IN)) continue;
            if ((t->dir == MAI_DIR_DUPLEX ? t->want_in_device : t->want_device)[0]) return NULL;
        }
        if (as.preferredInput) [as setPreferredInput:nil error:nil];
        return NULL;
    }
    NSString *want = [NSString stringWithUTF8String:uid];
    NSArray<AVAudioSessionPortDescription *> *ports = as.availableInputs;
    for (AVAudioSessionPortDescription *p in ports) {
        if (![p.UID isEqualToString:want]) continue;
        if (![as setPreferredInput:p error:&err]) {
            set_ns_error(s, "setPreferredInput", err);
            return "invalid_device";
        }
        return NULL;
    }
    char buf[MAI_ERRMAX];
    snprintf(buf, sizeof buf, "no input port with UID %s (%lu available)", uid, (unsigned long)ports.count);
    mai__set_error(s, buf);
    return "invalid_device";
}

/* ── The unit ──────────────────────────────────────────────────────────── */

static const char *be_open(mai_stream *s) {
    char buf[MAI_ERRMAX] = "";
    if ((s->dir & MAI_DIR_OUT) && s->want_device[0]) {
        mai__set_error(s, "iOS routes the output itself: no output device");
        return "unsupported";
    }
    if ((s->dir & MAI_DIR_IN) && !record_granted()) {
        mai__set_error(s, "the microphone permission is not granted");
        return "permission";
    }
    double rate, io_s;
    @autoreleasepool {
        const char *err = apply_session(s, true);
        if (err) return err;
        AVAudioSession *as = [AVAudioSession sharedInstance];
        if (s->dir & MAI_DIR_IN) {
            if (!as.isInputAvailable) {
                mai__set_error(s, "no audio input available");
                return "unavailable";
            }
            if ((err = prefer_input(s))) return err;
        }
        rate = as.sampleRate > 0 ? as.sampleRate : 48000;
        io_s = as.IOBufferDuration;
    }
    mai_apple_io *io = mai_apple_io_new(s);
    if (!io) {
        mai__set_error(s, "out of memory");
        return "unavailable";
    }
    OSType sub = (s->dir & MAI_DIR_IN) && s->preset == MAI_PRESET_VOICE ? kAudioUnitSubType_VoiceProcessingIO
                                                                         : kAudioUnitSubType_RemoteIO;
    if (mai_apple_unit_new(io, sub, buf, sizeof buf) || mai_apple_enable_io(io, s->dir, buf, sizeof buf) ||
        mai_apple_configure(io, s->dir, rate, buf, sizeof buf)) {
        mai_apple_io_free(io);
        mai__set_error(s, buf);
        return "unavailable";
    }
    s->rate = (int32_t)lround(rate);
    s->burst = (int32_t)lround(io_s * rate);
    s->buffer = s->burst;
    s->capacity = MAX_FRAMES_PER_SLICE;
    s->exclusive = false;
    s->device_id = 0;
    s->in_device_id = 0;
    s->preset_granted = (s->dir & MAI_DIR_IN) ? s->preset : 0;
    s->present = MAI_PRESENT_TIMESTAMP;
    refresh_latencies(s);
    atomic_store(&s->be_xruns, 0);
    atomic_store(&g_unit_rate[slot_of(s)], s->rate);
    /* a rate change between the read above and the store slips past the
     * route observer: catch it here */
    @autoreleasepool {
        double now_rate = [AVAudioSession sharedInstance].sampleRate;
        if (now_rate > 0 && fabs(now_rate - s->rate) > 0.5) atomic_fetch_or(&s->pending, MAI_PF_ROUTE | MAI_PF_RESET);
    }
    /* the session may have moved to another rate under the other streams'
     * units: rebuild those at this one */
    for (int i = 0; i < MAI_MAX_STREAMS; i++) {
        mai_stream *t = &g_streams[i];
        if (t != s && t->be && atomic_load(&t->open) && t->rate != s->rate)
            atomic_fetch_or(&t->pending, MAI_PF_ROUTE | MAI_PF_RESET);
    }
    s->be = io->unit;
    s->be2 = io;
    os_log(OS_LOG_DEFAULT, "[mob_audio_io] %{public}s open: %.0f Hz, IO buffer %.2f ms",
           s->dir == MAI_DIR_DUPLEX ? "duplex" : s->dir == MAI_DIR_IN ? "input" : "output", rate, io_s * 1000.0);
    return NULL;
}

static const char *be_start(mai_stream *s) {
    OSStatus r = AudioOutputUnitStart((AudioUnit)s->be);
    if (r == noErr) return NULL;
    char buf[MAI_ERRMAX];
    mai_apple_status(buf, sizeof buf, "AudioOutputUnitStart", r);
    mai__set_error(s, buf);
    return "unavailable";
}

static void be_close(mai_stream *s) {
    if (!s->be2) return;
    mai_apple_io_free(s->be2); /* stops the unit first: no callback runs after */
    atomic_store(&g_unit_rate[slot_of(s)], 0);
    s->be = NULL;
    s->be2 = NULL;
}

static int32_t be_xruns(mai_stream *s) { return (int32_t)atomic_load(&s->be_xruns); }

static void be_tune(mai_stream *s) { (void)s; }

/* The route's latency can change under a running unit (a headset): keep
 * the callback's copies current. And the session follows the open
 * streams: one may have closed. */
static void be_poll(mai_stream *s) {
    refresh_latencies(s);
    follow_open_streams();
}

/* What a frame waits: output, after the render; input, before the capture. */
static void be_latency(mai_stream *s, double *out_ms, double *in_ms) {
    (void)s;
    @autoreleasepool {
        AVAudioSession *as = [AVAudioSession sharedInstance];
        *out_ms = (as.outputLatency + as.IOBufferDuration) * 1000.0;
        *in_ms = (as.inputLatency + as.IOBufferDuration) * 1000.0;
    }
}

static void be_idle(void) {
    g_session_applied = false;
    if (!g_session_active) return;
    g_session_active = false;
    @autoreleasepool {
        [[AVAudioSession sharedInstance] setActive:NO
                                       withOptions:AVAudioSessionSetActiveOptionNotifyOthersOnDeactivation
                                             error:nil];
    }
}

static ERL_NIF_TERM port_types_term(ErlNifEnv *env, NSArray<AVAudioSessionPortDescription *> *ports) {
    ERL_NIF_TERM list = enif_make_list(env, 0);
    for (AVAudioSessionPortDescription *p in ports.reverseObjectEnumerator)
        list = enif_make_list_cell(env, string_term(env, p.portType.UTF8String), list);
    return list;
}

/* status/0's session: what iOS granted (the latency probe's numbers). */
static ERL_NIF_TERM be_session_term(ErlNifEnv *env) {
    @autoreleasepool {
        AVAudioSession *as = [AVAudioSession sharedInstance];
        AVAudioSessionRouteDescription *route = as.currentRoute;
        ERL_NIF_TERM k[11], v[11];
        int i = 0;
#define KV(key, val) (k[i] = atom(env, key), v[i] = (val), i++)
        KV("active", atom(env, g_session_active ? "true" : "false"));
        KV("category", string_term(env, as.category.UTF8String));
        KV("mode", string_term(env, as.mode.UTF8String));
        KV("rate", enif_make_double(env, as.sampleRate));
        KV("io_buffer_ms", enif_make_double(env, as.IOBufferDuration * 1000.0));
        KV("preferred_io_buffer_ms", enif_make_double(env, as.preferredIOBufferDuration * 1000.0));
        KV("output_latency_ms", enif_make_double(env, as.outputLatency * 1000.0));
        KV("input_latency_ms", enif_make_double(env, as.inputLatency * 1000.0));
        KV("input_available", atom(env, as.isInputAvailable ? "true" : "false"));
        KV("outputs", port_types_term(env, route.outputs));
        KV("inputs", port_types_term(env, route.inputs));
#undef KV
        return map_of(env, k, v, (unsigned)i);
    }
}

/* ── Devices ───────────────────────────────────────────────────────────── */

static const char *port_kind(AVAudioSessionPort t) {
    if ([t isEqualToString:AVAudioSessionPortBuiltInMic]) return "builtin_mic";
    if ([t isEqualToString:AVAudioSessionPortBuiltInSpeaker]) return "builtin_speaker";
    if ([t isEqualToString:AVAudioSessionPortBuiltInReceiver]) return "builtin_earpiece";
    if ([t isEqualToString:AVAudioSessionPortHeadsetMic]) return "wired_headset";
    if ([t isEqualToString:AVAudioSessionPortHeadphones]) return "wired_headphones";
    if ([t isEqualToString:AVAudioSessionPortBluetoothHFP] || [t isEqualToString:AVAudioSessionPortBluetoothLE])
        return "bluetooth";
    if ([t isEqualToString:AVAudioSessionPortBluetoothA2DP]) return "bluetooth_a2dp";
    if ([t isEqualToString:AVAudioSessionPortUSBAudio]) return "usb";
    if ([t isEqualToString:AVAudioSessionPortHDMI] || [t isEqualToString:AVAudioSessionPortDisplayPort]) return "hdmi";
    if ([t isEqualToString:AVAudioSessionPortAirPlay]) return "airplay";
    if ([t isEqualToString:AVAudioSessionPortCarAudio]) return "car";
    return "other";
}

/* :input: the session's available inputs (their UIDs are what :device
 * takes); :output: the current route's outputs (iOS picks them).
 * availableInputs is empty unless the category records (QA1799): with no
 * session active, switch to PlayAndRecord (not activated) just to read
 * them, then put back the app's category, mode and options (the array is a
 * snapshot), so other code (AVPlayer) finds them unchanged. While streams
 * hold an active session of another category, switching it would reroute
 * them: the list is what iOS gives then (often empty; open an input first). */
static ERL_NIF_TERM be_devices(ErlNifEnv *env, int dir) {
    @autoreleasepool {
        AVAudioSession *as = [AVAudioSession sharedInstance];
        NSArray<AVAudioSessionPortDescription *> *ports;
        if (dir == MAI_DIR_IN) {
            pthread_mutex_lock(&g_lock);
            if (!g_session_active && ![as.category isEqualToString:AVAudioSessionCategoryPlayAndRecord]) {
                AVAudioSessionCategory cat = as.category;
                AVAudioSessionMode mode = as.mode;
                AVAudioSessionCategoryOptions opts = as.categoryOptions;
                NSError *err = nil;
                if ([as setCategory:AVAudioSessionCategoryPlayAndRecord
                               mode:AVAudioSessionModeDefault
                            options:AVAudioSessionCategoryOptionDefaultToSpeaker |
                                    AVAudioSessionCategoryOptionAllowBluetoothA2DP
                              error:&err]) {
                    ports = as.availableInputs;
                    if (![as setCategory:cat mode:mode options:opts error:&err])
                        set_ns_error(NULL, "setCategory (devices, restore)", err);
                } else {
                    set_ns_error(NULL, "setCategory (devices)", err);
                    ports = as.availableInputs;
                }
                g_session_applied = false; /* (idle: the next open applies its own anyway) */
            } else {
                ports = as.availableInputs;
            }
            pthread_mutex_unlock(&g_lock);
        } else {
            ports = as.currentRoute.outputs;
        }
        ERL_NIF_TERM list = enif_make_list(env, 0);
        for (AVAudioSessionPortDescription *p in ports.reverseObjectEnumerator) {
            ERL_NIF_TERM k[3] = {atom(env, "id"), atom(env, "name"), atom(env, "kind")};
            ERL_NIF_TERM v[3] = {string_term(env, p.UID.UTF8String ?: ""), string_term(env, p.portName.UTF8String ?: ""),
                                 atom(env, port_kind(p.portType))};
            list = enif_make_list_cell(env, map_of(env, k, v, 3), list);
        }
        return list;
    }
}

static const char *be_decode(const char *path, mai_pcm *pcm, int32_t *rate, char *detail, size_t detail_n) {
    return mai_apple_decode(path, pcm, rate, detail, detail_n);
}
