/*
 * mob_audio_io.h: the C interface between mob_audio_io (the plugin that owns
 * the audio streams) and an app's own DSP (a static NIF of the app).
 *
 * The plugin opens and keeps the platform's audio stream (AAudio on Android,
 * a RemoteIO or VoiceProcessingIO unit and the AVAudioSession on iOS) and
 * calls a registered processor from the audio thread. Audio never goes
 * through the BEAM.
 *
 *   app NIF load()          mai_register("my_synth", &proc, ctx)
 *   Elixir                  MobAudioIo.open(self(), :output, processor: "my_synth", module: :my_dsp)
 *   audio thread            proc.capture(ctx, in, frames, &t_in)   (input, duplex)
 *   audio thread            proc.render(ctx, out, frames, &t_out)  (output, duplex)
 *   audio thread            mai_post(stream, msg, n)               (real-time safe)
 *   plugin event thread     {:mob_audio_io, :post, stream, msg} to the owner
 *
 * Both platforms link the plugin and the app's NIFs into one binary (the iOS
 * app executable, the Android app library), so these symbols resolve at link
 * time. The plugin compiles the same functions on both platforms.
 *
 * Getting this header: the plugin manifest can't export an include path yet
 * (MOB-541), so include it by a path relative to your c_src/<nif>.c:
 *
 *   #include "../deps/mob_audio_io/priv/include/mob_audio_io.h"
 *
 * Directions:
 *   output   render(out) per callback.
 *   input    capture(in) per callback.
 *   duplex   capture(in) then render(out) in the same callback, on the same
 *            thread, for the same `frames`: a processor can turn its input
 *            into its output with one buffer of latency.
 *
 * Threads:
 *   render, capture   the audio thread. Must not lock, allocate, free, log,
 *                     call enif_* or block. Write only to `out` (render).
 *   stream_event      a normal thread (the opener's NIF call or the plugin's
 *                     event thread), never at the same time as render or
 *                     capture for the same stream: STARTED comes before the
 *                     first callback, STOPPED after the last.
 *   mai_register,     any normal thread. mai_register is usually called from
 *   mai_unregister    the app NIF's load().
 *   mai_post          the stream's audio thread only (single producer): from
 *                     render or capture.
 *   mai_now_ns        any thread, real-time safe.
 *
 * File decodes: MobAudioIo.decode_stream(path, owner, processor: "name")
 * feeds a processor the decoded PCM instead of the owner, so C analysis of
 * a file never passes through the BEAM. Each decode runs on a thread of its
 * own and calls, all on that thread and in this order:
 *   stream_event(ctx, decode, MAI_EVENT_STARTED, t)  t->rate: the file's,
 *                                                    t->channels: 1 with mono
 *   capture(ctx, in, frames, t)   per chunk; t->frame: the chunk's first
 *                                 frame in the file, t->present_ns 0
 *   stream_event(ctx, decode, MAI_EVENT_STOPPED, t)  after the last chunk,
 *                                 whether the decode finished, failed or
 *                                 was cancelled; t->frame: frames delivered
 * `decode` is the decode's id (never a stream's; mai_post to it answers
 * MAI_ERR_NOT_OPEN). The decode thread is not real-time: there capture may
 * allocate, lock or log, and the decode waits for it. A processor used by
 * several decodes or streams at once gets their calls concurrently, each
 * on its own thread; capture carries no id, so per-decode state is the
 * processor's to key (a thread-local set at STARTED, say). The processor
 * is pinned (mai_unregister answers MAI_ERR_BUSY) until STOPPED.
 */
#ifndef MOB_AUDIO_IO_H
#define MOB_AUDIO_IO_H

#include <stddef.h>
#include <stdint.h>

#ifdef __cplusplus
extern "C" {
#endif

/* Bumped on any incompatible change to the structs or functions below.
 * Version 1 is the layout first released (mob_audio_io 0.1.0); the
 * unreleased spike's mai_time had no present_ns and isn't compatible. */
#define MAI_ABI_VERSION 1

#define MAI_NAME_MAX 48   /* processor name, bytes, including the NUL */
#define MAI_POST_MAX 240  /* bytes per mai_post message */

/* Results. */
#define MAI_OK 0
#define MAI_ERR_FULL (-1)      /* registry or post queue full */
#define MAI_ERR_INVALID (-2)   /* bad argument (NULL, empty or long name) */
#define MAI_ERR_NOT_OPEN (-3)  /* no such stream (closed, or a stale id) */
#define MAI_ERR_TOO_LARGE (-4) /* message over MAI_POST_MAX */
#define MAI_ERR_BUSY (-5)      /* the processor is in use by an open stream */
#define MAI_ERR_NOT_FOUND (-6) /* no processor by that name */

/* Stream events (stream_event's `event`). */
#define MAI_EVENT_STARTED 1       /* opened (again): t->rate is the stream's rate */
#define MAI_EVENT_STOPPED 2       /* closed, or lost and not yet reopened */
#define MAI_EVENT_INTERRUPTED 3   /* iOS: another app or a call took the session */
#define MAI_EVENT_ROUTE_CHANGED 4 /* a new rate or a media reset: the stream is stopped and reopens (STARTED follows) */

typedef struct mai_time {
    /* The stream clock: frames processed on this stream before this block.
     * Starts at 0 at open and keeps counting across a reopen (route change,
     * lost device, interruption), so a processor's schedule never jumps
     * back. In a duplex stream capture and render see the same value. */
    int64_t frame;
    /* mai_now_ns() when the callback began. */
    int64_t host_ns;
    /* On mai_now_ns()'s clock: render, when the block's first frame reaches
     * the output (the speaker, the headphones); capture, when the block's
     * first frame reached the input. From the platform's timestamps where
     * they exist (iOS AudioTimeStamp.mHostTime plus the session's latency,
     * AAudio timestamps on Android 12+), an estimate from the buffer size
     * elsewhere (Android 8-11, see MobAudioIo.status/0 `present`). 0 while
     * unknown (a stream's first callbacks on Android 12+). */
    int64_t present_ns;
    int32_t rate;     /* frames per second, the device's own rate */
    int32_t channels; /* interleaved channels in this buffer (`out` or `in`) */
} mai_time;

typedef struct mai_processor {
    /* Fill `out` (frames * t->channels floats, interleaved, -1..1). `out`
     * arrives zeroed. NULL: the stream plays silence. */
    void (*render)(void *ctx, float *out, int32_t frames, const mai_time *t);
    /* Input and duplex streams: `in` holds frames * t->channels captured
     * floats, interleaved; valid only during the call. NULL: discarded. */
    void (*capture)(void *ctx, const float *in, int32_t frames, const mai_time *t);
    /* MAI_EVENT_*; `t` carries the stream's rate and output channels (input
     * channels for an input stream), and the stream clock at the event.
     * `stream` is the id to mai_post to. May be NULL. */
    void (*stream_event)(void *ctx, int32_t stream, int32_t event, const mai_time *t);
} mai_processor;

/* MAI_ABI_VERSION of the linked plugin: compare with the header's. */
int32_t mai_abi_version(void);

/* The clock of mai_time's host_ns and present_ns, in ns: CLOCK_MONOTONIC on
 * Android, mach_absolute_time (CLOCK_UPTIME_RAW) on iOS. */
int64_t mai_now_ns(void);

/* Register (or replace, while no stream uses it) a processor under `name`.
 * The struct is copied; `ctx` must stay valid until mai_unregister. */
int32_t mai_register(const char *name, const mai_processor *p, void *ctx);

/* Remove a processor. MAI_ERR_BUSY while an open stream uses it. */
int32_t mai_unregister(const char *name);

/* From the stream's audio thread (render or capture): queue up to
 * MAI_POST_MAX bytes for the stream's owner, which receives
 * {:mob_audio_io, :post, stream, binary} from the plugin's event thread
 * (within ~5 ms). Lock-free, never blocks; MAI_ERR_FULL when the queue (64
 * messages) is full, counted as posts_dropped. Posts still queued when the
 * stream closes reach the owner before its :stopped message; if the owner
 * has exited they are counted in posts_dropped. */
int32_t mai_post(int32_t stream, const void *msg, size_t n);

#ifdef __cplusplus
}
#endif

#endif /* MOB_AUDIO_IO_H */
