/*
 * mob_audio_io.h: the C interface between mob_audio_io (the plugin that owns
 * the audio streams) and an app's own DSP (a static NIF of the app).
 *
 * The plugin opens and keeps the platform's audio stream (AAudio on Android,
 * a RemoteIO unit and the AVAudioSession on iOS) and calls a registered
 * processor from the audio thread. Rendering never goes through the BEAM.
 *
 *   app NIF load()          mai_register("my_synth", &proc, ctx)
 *   Elixir                  MobAudioIo.open(self(), :output, processor: "my_synth")
 *   audio thread            proc.render(ctx, out, frames, &t)   (real time)
 *   audio thread            mai_post(stream, msg, n)            (real-time safe)
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
 * Threads:
 *   render, capture   the audio thread. Must not lock, allocate, free, log,
 *                     call enif_* or block. Write only to `out`.
 *   stream_event      a normal thread (the opener's NIF call or the plugin's
 *                     event thread), never at the same time as render for
 *                     the same stream: STARTED comes before the first render,
 *                     STOPPED after the last.
 *   mai_register,     any normal thread. mai_register is usually called from
 *   mai_unregister    the app NIF's load().
 *   mai_post          the stream's audio thread only (single producer).
 */
#ifndef MOB_AUDIO_IO_H
#define MOB_AUDIO_IO_H

#include <stddef.h>
#include <stdint.h>

#ifdef __cplusplus
extern "C" {
#endif

/* Bumped on any incompatible change to the structs or functions below. */
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
#define MAI_EVENT_ROUTE_CHANGED 4 /* a new output route (headphones, Bluetooth, ...) */

typedef struct mai_time {
    /* The stream clock: frames rendered on this stream before this block.
     * Starts at 0 at open and keeps counting across a reopen (route change,
     * lost device), so a processor's schedule never jumps back. */
    int64_t frame;
    /* CLOCK_MONOTONIC ns when the callback began. The block is heard about
     * `latency_ms` (MobAudioIo.status/0) later. */
    int64_t host_ns;
    int32_t rate;     /* frames per second, the device's own rate */
    int32_t channels; /* interleaved channels in `out` (2 today) */
} mai_time;

typedef struct mai_processor {
    /* Fill `out` (frames * t->channels floats, interleaved, -1..1). `out`
     * arrives zeroed. NULL: the stream plays silence. */
    void (*render)(void *ctx, float *out, int32_t frames, const mai_time *t);
    /* Input streams (not in this version): `in` holds the captured frames. */
    void (*capture)(void *ctx, const float *in, int32_t frames, const mai_time *t);
    /* MAI_EVENT_*; `t` carries the stream's rate and channels, and the stream
     * clock at the event. May be NULL. */
    void (*stream_event)(void *ctx, int32_t stream, int32_t event, const mai_time *t);
} mai_processor;

/* MAI_ABI_VERSION of the linked plugin: compare with the header's. */
int32_t mai_abi_version(void);

/* Register (or replace, while no stream uses it) a processor under `name`.
 * The struct is copied; `ctx` must stay valid until mai_unregister. */
int32_t mai_register(const char *name, const mai_processor *p, void *ctx);

/* Remove a processor. MAI_ERR_BUSY while an open stream uses it. */
int32_t mai_unregister(const char *name);

/* From the stream's render callback: queue up to MAI_POST_MAX bytes for the
 * stream's owner, which receives {:mob_audio_io, :post, stream, binary} from
 * the plugin's event thread (within ~5 ms). Lock-free, never blocks;
 * MAI_ERR_FULL when the owner's queue is full (counted as posts_dropped). */
int32_t mai_post(int32_t stream, const void *msg, size_t n);

#ifdef __cplusplus
}
#endif

#endif /* MOB_AUDIO_IO_H */
