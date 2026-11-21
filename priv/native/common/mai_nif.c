/*
 * mai_nif.c: the NIF half of mob_audio_io shared by both platforms: the
 * mob_audio_io_nif functions, owner monitors and the event thread. The
 * platform file includes it, then defines the backend (be_*):
 *
 *   priv/native/android/mob_audio_io_nif.c   AAudio (dlopen), JNI, AMediaCodec
 *   priv/native/ios/mob_audio_io_nif.m       RemoteIO + AVAudioSession, ExtAudioFile
 *
 * Locking: g_lock serialises open, close, reopen and status (NIF calls on
 * dirty IO schedulers and the event thread). The audio thread takes no lock.
 *
 * The event thread (one, started at load) sleeps on g_wake while no stream
 * is open; with a stream open it wakes every 5 ms and does all the work the
 * audio thread and the OS callbacks must not: it reopens a lost or
 * interrupted stream, grows an Android buffer after an underrun, refreshes
 * the presentation-time anchor, drains mai_post queues, closes a stream
 * whose owner exited, and sends the owner its messages:
 *
 *   {:mob_audio_io, :started, id, info}         opened, or reopened (info.reopened)
 *   {:mob_audio_io, :stopped, id, %{reason: r, posts_dropped: n}}
 *                                               r: :closed | :device_lost | :interrupted | :route_changed | :owner_exit
 *   {:mob_audio_io, :device_lost, id, %{}}      the device went away; reopening follows
 *   {:mob_audio_io, :interrupted, id, %{}}      iOS session interruption began
 *   {:mob_audio_io, :resumed, id, %{}}          ... ended (a :started follows)
 *   {:mob_audio_io, :route_changed, id, %{reason: r}}  r: :new_route | :new_rate | :media_reset
 *   {:mob_audio_io, :xrun, id, %{xruns: n, buffer: frames}}
 *   {:mob_audio_io, :error, id, %{reason: atom, detail: string, while: atom}}  a reopen failed (retried)
 *   {:mob_audio_io, :post, id, binary}          mai_post from the processor
 *
 * File decodes (decode_stream/3) run on a thread each (decode_main), and
 * send their owner, from that thread:
 *
 *   {:mob_audio_io, :decode_started, id, %{rate, channels, frames}}  frames: the stated length or nil
 *   {:mob_audio_io, :decoded, id, binary}       a chunk of f32 frames (unless a processor takes them)
 *   {:mob_audio_io, :decode_done, id, %{frames, rate, channels}}
 *   {:mob_audio_io, :decode_cancelled, id, %{frames}}
 *   {:mob_audio_io, :decode_error, id, %{reason, detail, frames}}
 *
 * Every message but :post and :decoded is also sent to the logger process set with
 * set_logger/1 (MobAudioIo.Log), which logs it with a [mob_audio_io] prefix:
 * native code can't reach Sloppy Joe's `logs`.
 */
#include <erl_nif.h>
#include <unistd.h>

#include "mai_core.c"

/* ── The backend each platform file defines ────────────────────────────── */

/* (g_lock held for be_load .. be_session_term) */
static const char *be_platform(void);
/* Load the platform API; NULL or an error atom. */
static const char *be_load(void);
/* Open (not start) s's stream(s) from s->dir and s->want_*: set s->be (and
 * be2), rate, channels, in_channels, burst, buffer, capacity, exclusive,
 * device_id, in_device_id, preset_granted, present. NULL or an error atom,
 * with s->last_error set: "permission" (the microphone isn't granted),
 * "unsupported", "unavailable", "session", "invalid_device". */
static const char *be_open(mai_stream *s);
/* Start the opened stream; NULL or an error atom (s->last_error set). */
static const char *be_start(mai_stream *s);
/* Stop and close; no callback runs once it returns. s->be = s->be2 = NULL. */
static void be_close(mai_stream *s);
/* The underrun (output) and overrun (input) count so far, since open. */
static int32_t be_xruns(mai_stream *s);
/* After new xruns: Android adds a burst of buffer, up to the capacity. */
static void be_tune(mai_stream *s);
/* Every 100 ms while running, on the event thread: refresh what the
 * callback reads (the presentation anchor, the latencies). */
static void be_poll(mai_stream *s);
/* Output and input latency in ms (< 0: unknown or no such side). */
static void be_latency(mai_stream *s, double *out_ms, double *in_ms);
/* No stream open any more: iOS deactivates the session. */
static void be_idle(void);
/* Platform-wide readback for status/0 (iOS: the session), or nil. */
static ERL_NIF_TERM be_session_term(ErlNifEnv *env);
/* (no lock) The devices of a direction (MAI_DIR_IN / MAI_DIR_OUT):
 * [%{id, name, kind}] or {:error, reason}. */
static ERL_NIF_TERM be_devices(ErlNifEnv *env, int dir);
/* (no lock; the decode's own thread) Decode `path` into `d`: mai__dec_begin
 * once the format is known, then mai__dec_push_* per codec buffer, checking
 * mai__dec_stopping(d) every buffer and stopping when a push answers
 * anything but MAI_DEC_GO (mai__dec_reason names it). NULL at the end of
 * the file, or an error atom (:not_found, :unsupported, :no_audio,
 * :too_long, :no_memory, :decode_error, or "cancelled"), with a platform
 * detail in `detail` (may stay empty). */
static const char *be_decode(const char *path, mai_decode *d, char *detail, size_t detail_n);

/* ── State ─────────────────────────────────────────────────────────────── */

static pthread_mutex_t g_lock = PTHREAD_MUTEX_INITIALIZER;
static pthread_cond_t g_wake = PTHREAD_COND_INITIALIZER; /* a stream opened (g_lock) */

typedef struct {
    ErlNifPid owner;
    void *res; /* monitor resource (mai_mon) */
    ErlNifMonitor mon;
    bool monitored;
} mai_owner;

static mai_owner g_owners[MAI_MAX_STREAMS];
static ErlNifResourceType *g_mon_type;
static ErlNifPid g_logger;
static _Atomic bool g_have_logger;
static bool g_thread_up;
static ErlNifEnv *g_log_env; /* the event thread's, for the logger's copies */

typedef struct {
    int32_t id;
} mai_mon;

#define REOPEN_RETRY_NS 500000000LL
#define REOPEN_RETRY_MAX_NS 8000000000LL
#define FLAP_NS 1000000000LL /* a loss this soon after a start is a quick loss */
#define POLL_NS 100000000LL
#define EVENT_PERIOD_US 5000

static int slot_of(const mai_stream *s) { return (int)(s - g_streams); }

/* ── Terms ─────────────────────────────────────────────────────────────── */

static ERL_NIF_TERM atom(ErlNifEnv *env, const char *a) { return enif_make_atom(env, a); }

static ERL_NIF_TERM error_atom(ErlNifEnv *env, const char *reason) {
    return enif_make_tuple2(env, atom(env, "error"), atom(env, reason));
}

static ERL_NIF_TERM binary_of(ErlNifEnv *env, const void *data, size_t n) {
    ERL_NIF_TERM t;
    unsigned char *p = enif_make_new_binary(env, n, &t);
    if (n) memcpy(p, data, n);
    return t;
}

static ERL_NIF_TERM string_term(ErlNifEnv *env, const char *s) { return binary_of(env, s, strlen(s)); }

static ERL_NIF_TERM map_of(ErlNifEnv *env, ERL_NIF_TERM *k, ERL_NIF_TERM *v, unsigned n) {
    ERL_NIF_TERM m;
    if (!enif_make_map_from_arrays(env, k, v, n, &m)) return enif_make_new_map(env);
    return m;
}

static ERL_NIF_TERM empty_map(ErlNifEnv *env) { return enif_make_new_map(env); }

/* {:error, reason} or, with a platform detail, {:error, {reason, detail}}.
 * :permission and :unsupported stay plain atoms: their detail (logged by
 * the caller's Elixir) adds nothing an app would match on. */
static ERL_NIF_TERM error_detail(ErlNifEnv *env, const char *reason, const char *detail) {
    if (!detail || !detail[0] || strcmp(reason, "permission") == 0 || strcmp(reason, "unsupported") == 0)
        return error_atom(env, reason);
    return enif_make_tuple2(env, atom(env, "error"),
                            enif_make_tuple2(env, atom(env, reason), string_term(env, detail)));
}

static const char *direction_name(int dir) {
    return dir == MAI_DIR_DUPLEX ? "duplex" : dir == MAI_DIR_IN ? "input" : "output";
}

/* The info of a :started message. */
static ERL_NIF_TERM started_info(ErlNifEnv *env, mai_stream *s, bool reopened) {
    ERL_NIF_TERM k[8], v[8];
    k[0] = atom(env, "rate"); v[0] = enif_make_int(env, s->rate);
    k[1] = atom(env, "channels"); v[1] = enif_make_int(env, (s->dir & MAI_DIR_OUT) ? s->channels : 0);
    k[2] = atom(env, "input_channels"); v[2] = enif_make_int(env, (s->dir & MAI_DIR_IN) ? s->in_channels : 0);
    k[3] = atom(env, "burst"); v[3] = enif_make_int(env, s->burst);
    k[4] = atom(env, "buffer"); v[4] = enif_make_int(env, s->buffer);
    k[5] = atom(env, "sharing"); v[5] = atom(env, s->exclusive ? "exclusive" : "shared");
    k[6] = atom(env, "reopened"); v[6] = atom(env, reopened ? "true" : "false");
    k[7] = atom(env, "direction"); v[7] = atom(env, direction_name(s->dir));
    return map_of(env, k, v, 8);
}

static ERL_NIF_TERM reason_info(ErlNifEnv *env, const char *reason) {
    ERL_NIF_TERM k = atom(env, "reason"), v = atom(env, reason);
    return map_of(env, &k, &v, 1);
}

static ERL_NIF_TERM stopped_info(ErlNifEnv *env, const char *reason, uint32_t dropped) {
    ERL_NIF_TERM k[2] = {atom(env, "reason"), atom(env, "posts_dropped")};
    ERL_NIF_TERM v[2] = {atom(env, reason), enif_make_uint(env, dropped)};
    return map_of(env, k, v, 2);
}

/* {:mob_audio_io, kind, id, payload} to the stream's owner (unless
 * `to_owner` is false) and, when `log`, the logger. From a NIF call (`env`
 * process-bound, from_thread false) or the event thread (`env` its message
 * env, cleared by the send). Whether the owner got it. */
static bool notify_to(ErlNifEnv *env, bool from_thread, mai_stream *s, const char *kind, ERL_NIF_TERM payload,
                      bool log, bool to_owner) {
    ERL_NIF_TERM msg = enif_make_tuple4(env, atom(env, "mob_audio_io"), atom(env, kind), enif_make_int(env, s->id),
                                        payload);
    ErlNifPid owner = g_owners[slot_of(s)].owner;
    if (log && atomic_load(&g_have_logger)) {
        ErlNifPid logger = g_logger;
        if (from_thread) {
            ERL_NIF_TERM copy = enif_make_copy(g_log_env, msg);
            enif_send(NULL, &logger, g_log_env, copy);
            enif_clear_env(g_log_env); /* (a failed send leaves the copy) */
        } else {
            enif_send(env, &logger, NULL, msg);
        }
    }
    if (!to_owner) {
        if (from_thread) enif_clear_env(env);
        return false;
    }
    if (from_thread) return enif_send(NULL, &owner, env, msg) != 0;
    return enif_send(env, &owner, NULL, msg) != 0;
}

static void notify(ErlNifEnv *env, bool from_thread, mai_stream *s, const char *kind, ERL_NIF_TERM payload,
                   bool log) {
    notify_to(env, from_thread, s, kind, payload, log, true);
}

/* ── Open / close (g_lock held) ────────────────────────────────────────── */

/* Open and start s's platform stream, telling the processor STARTED before
 * the first callback. NULL or an error atom (s->be NULL then). */
static const char *start_stream_locked(mai_stream *s) {
    s->last_error[0] = 0;
    const char *err = be_open(s);
    if (err) return err;
    mai__reset_callback_state(s);
    mai__event(s, MAI_EVENT_STARTED);
    err = be_start(s);
    if (err) {
        be_close(s);
        mai__event(s, MAI_EVENT_STOPPED);
        return err;
    }
    s->running = true;
    s->xruns_seen = 0;
    s->started_ns = mai_now_ns();
    return NULL;
}

static void stop_stream_locked(mai_stream *s) {
    if (!s->be) return;
    be_close(s);
    s->running = false;
    mai__event(s, MAI_EVENT_STOPPED);
}

static bool any_open_locked(void) {
    for (int i = 0; i < MAI_MAX_STREAMS; i++)
        if (atomic_load(&g_streams[i].open)) return true;
    return false;
}

/* Deliver what the processor queued, in order (or count it, with no owner
 * to deliver to, or when the send fails: the owner died before its monitor
 * fired). The stream must be stopped, or this is the event thread's
 * periodic drain: one consumer. */
static uint32_t drain_posts_locked(ErlNifEnv *env, bool from_thread, mai_stream *s, bool deliver) {
    uint32_t dropped = 0;
    mai_msg m;
    while (mai__pop_post(s, &m)) {
        if (!deliver) {
            dropped++;
            continue;
        }
        if (!notify_to(env, from_thread, s, "post", binary_of(env, m.data, m.len), false, true)) dropped++;
        if (from_thread) enif_clear_env(env);
    }
    if (dropped) atomic_fetch_add(&s->posts_dropped, dropped);
    return dropped;
}

/* Stop the platform stream for `reason` (device lost, interrupted, a
 * rebuild): what the processor queued before it reaches the owner first,
 * then :stopped. */
static void stop_and_report_locked(ErlNifEnv *env, mai_stream *s, const char *reason) {
    stop_stream_locked(s);
    uint32_t dropped = drain_posts_locked(env, true, s, true);
    notify(env, true, s, "stopped", stopped_info(env, reason, dropped), true);
}

/* Close the stream and free its slot: stops the platform stream, delivers
 * (or, when the owner exited, drops and counts) the queued posts, sends
 * :stopped, unpins the processor, drops the monitor. `env`: the caller's
 * (from_thread false), or the event thread's. */
static void free_stream_locked(ErlNifEnv *env, bool from_thread, mai_stream *s, const char *reason, bool owner_alive) {
    stop_stream_locked(s);
    uint32_t dropped = drain_posts_locked(env, from_thread, s, owner_alive);
    notify_to(env, from_thread, s, "stopped", stopped_info(env, reason, dropped), true, owner_alive);
    if (from_thread) enif_clear_env(env);
    atomic_store_explicit(&s->open, false, memory_order_release);
    mai__release(s);
    mai_owner *o = &g_owners[slot_of(s)];
    if (o->monitored) enif_demonitor_process(from_thread ? NULL : env, o->res, &o->mon);
    if (o->res) enif_release_resource(o->res);
    o->res = NULL;
    o->monitored = false;
    if (!any_open_locked()) be_idle();
}

/* ── Event thread ──────────────────────────────────────────────────────── */

/* Open the platform stream again. On failure: `retry` schedules another
 * try with backoff; without it the stream stays interrupted (an iOS
 * session another app or a call may still hold) until the interruption's
 * end or the next return to the foreground. */
static bool try_reopen_locked(ErlNifEnv *env, mai_stream *s, int64_t now, const char *why, bool retry) {
    const char *err = start_stream_locked(s);
    if (!err) {
        atomic_fetch_add(&s->reopens, 1);
        s->next_reopen_ns = 0;
        s->reopen_failures = 0;
        /* the end of an interruption whose "ended" never came */
        if (s->interrupted) notify(env, true, s, "resumed", empty_map(env), true);
        s->interrupted = false;
        notify(env, true, s, "started", started_info(env, s, true), true);
        return true;
    }
    if (retry) {
        /* back off: 0.5 s, 1 s, 2 s ... 8 s */
        int64_t wait = REOPEN_RETRY_NS << (s->reopen_failures < 4 ? s->reopen_failures : 4);
        s->next_reopen_ns = now + (wait < REOPEN_RETRY_MAX_NS ? wait : REOPEN_RETRY_MAX_NS);
        s->reopen_failures++;
    } else {
        s->interrupted = true;
        s->next_reopen_ns = 0;
    }
    ERL_NIF_TERM k[3] = {atom(env, "reason"), atom(env, "detail"), atom(env, "while")};
    ERL_NIF_TERM v[3] = {atom(env, err), string_term(env, s->last_error), atom(env, why)};
    notify(env, true, s, "error", map_of(env, k, v, 3), true);
    return false;
}

static void handle_pending_locked(ErlNifEnv *env, mai_stream *s, uint32_t pf, int64_t now) {
    if (pf & MAI_PF_INTERRUPTED) {
        bool was_running = s->be != NULL;
        stop_stream_locked(s);
        s->interrupted = true;
        s->next_reopen_ns = 0;
        mai__event(s, MAI_EVENT_INTERRUPTED);
        notify(env, true, s, "interrupted", empty_map(env), true);
        if (was_running) {
            uint32_t dropped = drain_posts_locked(env, true, s, true);
            notify(env, true, s, "stopped", stopped_info(env, "interrupted", dropped), true);
        }
    }
    if ((pf & MAI_PF_RESUMED) && s->interrupted) {
        /* the interruption's end (iOS says it may resume): retried with
         * backoff if the session isn't ours yet */
        notify(env, true, s, "resumed", empty_map(env), true);
        s->interrupted = false;
        s->reopen_failures = 0;
        try_reopen_locked(env, s, now, "resume", true);
    } else if ((pf & MAI_PF_FOREGROUND) && s->interrupted) {
        /* back in the foreground (or a stale "began" from a suspension)
         * with an interruption whose end never came: one try, :resumed
         * and :started if it works. A call still going keeps the stream
         * interrupted, with no retry loop. */
        try_reopen_locked(env, s, now, "foreground", false);
    }
    if (pf & MAI_PF_DEVICE_LOST) {
        notify(env, true, s, "device_lost", empty_map(env), true);
        if (s->be) stop_and_report_locked(env, s, "device_lost");
        /* A stream lost within a second of its start, again and again (an
         * explicit device the system won't route to: Android disconnects
         * it ~60 ms in), is a flap: wait 0.5 s, 1 s ... 8 s between tries
         * instead of reopening at once forever. A stream that ran longer
         * reopens at once. */
        bool quick = s->started_ns && now - s->started_ns < FLAP_NS;
        s->quick_losses = quick ? s->quick_losses + 1 : 0;
        if (!s->interrupted && s->quick_losses > 1) {
            int k = s->quick_losses - 2 < 4 ? s->quick_losses - 2 : 4;
            int64_t wait = REOPEN_RETRY_NS << k;
            s->next_reopen_ns = now + (wait < REOPEN_RETRY_MAX_NS ? wait : REOPEN_RETRY_MAX_NS);
            mai__set_error(s, "lost within 1 s of each start: the system routes this stream elsewhere");
        } else if (!s->interrupted) {
            try_reopen_locked(env, s, now, "device_lost", true);
        }
    }
    if (pf & (MAI_PF_ROUTE | MAI_PF_RESET)) {
        /* ROUTE alone: a new route at the same rate (the unit follows it).
         * ROUTE|RESET: the rate changed; RESET alone: media services were
         * reset. Both need the platform stream rebuilt, and only then does
         * the processor hear of it: stream_event never runs beside a
         * callback. */
        const char *why = !(pf & MAI_PF_RESET) ? "new_route" : (pf & MAI_PF_ROUTE) ? "new_rate" : "media_reset";
        notify(env, true, s, "route_changed", reason_info(env, why), true);
        if ((pf & MAI_PF_RESET) && !s->interrupted) {
            if (s->be) stop_and_report_locked(env, s, "route_changed");
            mai__event(s, MAI_EVENT_ROUTE_CHANGED);
            try_reopen_locked(env, s, now, "route_changed", true);
        }
    }
}

static void poll_stream_locked(ErlNifEnv *env, mai_stream *s, int64_t now, bool poll_tick) {
    uint32_t pf = atomic_exchange(&s->pending, 0);
    if (pf & MAI_PF_OWNER_DOWN) {
        free_stream_locked(env, true, s, "owner_exit", false);
        return;
    }
    if (pf) handle_pending_locked(env, s, pf, now);
    if (!s->be && !s->interrupted && s->next_reopen_ns && now >= s->next_reopen_ns)
        try_reopen_locked(env, s, now, "retry", true);
    if (poll_tick && s->be) {
        be_poll(s);
        int32_t x = be_xruns(s);
        if (x > s->xruns_seen) {
            s->xruns_seen = x;
            be_tune(s);
            ERL_NIF_TERM k[2] = {atom(env, "xruns"), atom(env, "buffer")};
            ERL_NIF_TERM v[2] = {enif_make_int(env, x), enif_make_int(env, s->buffer)};
            notify(env, true, s, "xrun", map_of(env, k, v, 2), true);
        }
    }
    drain_posts_locked(env, true, s, true);
}

static void *event_main(void *arg) {
    (void)arg;
    ErlNifEnv *env = enif_alloc_env();
    int64_t next_poll = 0;
    pthread_mutex_lock(&g_lock);
    for (;;) {
        /* idle: no wake-ups at all until a stream opens */
        while (!any_open_locked()) pthread_cond_wait(&g_wake, &g_lock);
        pthread_mutex_unlock(&g_lock);
        usleep(EVENT_PERIOD_US);
        int64_t now = mai_now_ns();
        bool poll_tick = now >= next_poll;
        if (poll_tick) next_poll = now + POLL_NS;
        pthread_mutex_lock(&g_lock);
        for (int i = 0; i < MAI_MAX_STREAMS; i++) {
            mai_stream *s = &g_streams[i];
            if (!atomic_load(&s->open)) continue;
            poll_stream_locked(env, s, now, poll_tick);
            enif_clear_env(env);
        }
    }
    return NULL;
}

/* ── Monitors ──────────────────────────────────────────────────────────── */

static void mon_down(ErlNifEnv *env, void *obj, ErlNifPid *pid, ErlNifMonitor *mon) {
    (void)env; (void)pid; (void)mon;
    mai_stream *s = mai__stream(((mai_mon *)obj)->id);
    if (s) atomic_fetch_or(&s->pending, MAI_PF_OWNER_DOWN);
}

/* ── Option parsing ────────────────────────────────────────────────────── */

static bool get_atom(ErlNifEnv *env, ERL_NIF_TERM t, char *buf, unsigned n) {
    return enif_get_atom(env, t, buf, n, ERL_NIF_LATIN1) > 0;
}

static bool get_string(ErlNifEnv *env, ERL_NIF_TERM t, char *buf, size_t max) {
    ErlNifBinary b;
    if (!enif_inspect_binary(env, t, &b) || b.size >= max) return false;
    memcpy(buf, b.data, b.size);
    buf[b.size] = 0;
    return true;
}

static bool opt_int(ErlNifEnv *env, ERL_NIF_TERM map, const char *key, int *out) {
    ERL_NIF_TERM v;
    return enif_get_map_value(env, map, atom(env, key), &v) && enif_get_int(env, v, out);
}

static bool opt_atom(ErlNifEnv *env, ERL_NIF_TERM map, const char *key, char *buf, unsigned n) {
    ERL_NIF_TERM v;
    return enif_get_map_value(env, map, atom(env, key), &v) && get_atom(env, v, buf, n);
}

static bool opt_string(ErlNifEnv *env, ERL_NIF_TERM map, const char *key, char *buf, size_t n) {
    ERL_NIF_TERM v;
    return enif_get_map_value(env, map, atom(env, key), &v) && get_string(env, v, buf, n);
}

static int pick(const char *v, const char *const *names, int n) {
    for (int i = 0; i < n; i++)
        if (strcmp(v, names[i]) == 0) return i;
    return -1;
}

/* The opener's options into `s`; false on a bad one. */
static bool parse_options(ErlNifEnv *env, ERL_NIF_TERM m, mai_stream *s) {
    static const char *const sharings[] = {"shared", "exclusive"};
    static const char *const sessions[] = {"play", "mix", "play_and_record"};
    static const char *const presets[] = {"default", "generic", "performance", "unprocessed", "voice"};
    char sharing[16], session[24], preset[16];
    int rate, frames, channels, in_channels;
    if (!opt_int(env, m, "rate", &rate) || !opt_int(env, m, "frames", &frames) ||
        !opt_int(env, m, "channels", &channels) || !opt_int(env, m, "input_channels", &in_channels) ||
        !opt_atom(env, m, "sharing", sharing, sizeof sharing) || !opt_atom(env, m, "session", session, sizeof session) ||
        !opt_atom(env, m, "preset", preset, sizeof preset) ||
        !opt_string(env, m, "device", s->want_device, sizeof s->want_device) ||
        !opt_string(env, m, "input_device", s->want_in_device, sizeof s->want_in_device))
        return false;
    s->sharing = pick(sharing, sharings, 2);
    s->session = pick(session, sessions, 3);
    s->preset = pick(preset, presets, 5);
    if (rate < 0 || frames < 0 || channels < 1 || channels > 2 || in_channels < 1 || in_channels > 2 || s->sharing < 0 ||
        s->session < 0 || s->preset < 0)
        return false;
    s->want_rate = rate;
    s->want_frames = frames;
    s->want_channels = channels;
    s->want_in_channels = in_channels;
    s->channels = channels;
    s->in_channels = in_channels;
    return true;
}

/* ── NIFs ──────────────────────────────────────────────────────────────── */

/* open(owner, direction, processor, opts) -> {:ok, id} | {:error, reason}
 *   | {:error, {reason, detail}}. opts: %{rate, frames, channels,
 *   input_channels, sharing, session, preset, device, input_device}
 *   (device strings, "" for the default). */
static ERL_NIF_TERM nif_open(ErlNifEnv *env, int argc, const ERL_NIF_TERM argv[]) {
    (void)argc;
    ErlNifPid owner;
    char dir[16], name[MAI_NAME_MAX];
    if (!enif_get_local_pid(env, argv[0], &owner) || !get_atom(env, argv[1], dir, sizeof dir) ||
        !get_string(env, argv[2], name, sizeof name) || !name[0] || !enif_is_map(env, argv[3]))
        return enif_make_badarg(env);
    int d = strcmp(dir, "output") == 0 ? MAI_DIR_OUT : strcmp(dir, "input") == 0 ? MAI_DIR_IN
            : strcmp(dir, "duplex") == 0 ? MAI_DIR_DUPLEX : 0;
    if (!d) return enif_make_badarg(env);

    mai__register_builtins();
    pthread_mutex_lock(&g_lock);
    const char *err = be_load();
    if (err) {
        pthread_mutex_unlock(&g_lock);
        return error_atom(env, err);
    }
    mai_stream *s = mai__alloc_stream();
    if (!s) {
        pthread_mutex_unlock(&g_lock);
        return error_atom(env, "busy");
    }
    if (!parse_options(env, argv[3], s)) {
        pthread_mutex_unlock(&g_lock);
        return enif_make_badarg(env);
    }
    s->dir = d;
    if (mai__acquire(s, name) != MAI_OK) {
        pthread_mutex_unlock(&g_lock);
        return error_atom(env, "unknown_processor");
    }
    mai_owner *o = &g_owners[slot_of(s)];
    o->owner = owner;
    o->monitored = false;
    o->res = NULL;
    /* open before the platform stream starts: mai_post works from the first callback */
    atomic_store_explicit(&s->open, true, memory_order_release);
    err = start_stream_locked(s);
    if (err) {
        ERL_NIF_TERM r = error_detail(env, err, s->last_error);
        atomic_store_explicit(&s->open, false, memory_order_release);
        mai__release(s);
        if (!any_open_locked()) be_idle();
        pthread_mutex_unlock(&g_lock);
        return r;
    }
    mai_mon *m = enif_alloc_resource(g_mon_type, sizeof(mai_mon));
    m->id = s->id;
    o->res = m;
    int mr = enif_monitor_process(env, m, &owner, &o->mon);
    o->monitored = mr == 0;
    if (mr > 0) atomic_fetch_or(&s->pending, MAI_PF_OWNER_DOWN); /* already gone: the event thread closes it */
    notify(env, false, s, "started", started_info(env, s, false), true);
    ERL_NIF_TERM ok = enif_make_tuple2(env, atom(env, "ok"), enif_make_int(env, s->id));
    pthread_cond_signal(&g_wake);
    pthread_mutex_unlock(&g_lock);
    return ok;
}

static ERL_NIF_TERM nif_close(ErlNifEnv *env, int argc, const ERL_NIF_TERM argv[]) {
    (void)argc;
    int id;
    if (!enif_get_int(env, argv[0], &id)) return enif_make_badarg(env);
    pthread_mutex_lock(&g_lock);
    mai_stream *s = mai__stream(id);
    if (!s) {
        pthread_mutex_unlock(&g_lock);
        return error_atom(env, "not_open");
    }
    free_stream_locked(env, false, s, "closed", true);
    pthread_mutex_unlock(&g_lock);
    return atom(env, "ok");
}

static ERL_NIF_TERM db_term(ErlNifEnv *env, float lin) {
    return enif_make_double(env, lin > 1e-10f ? 20.0 * log10((double)lin) : -200.0);
}

static ERL_NIF_TERM ms_term(ErlNifEnv *env, double ms) { return ms >= 0 ? enif_make_double(env, ms) : atom(env, "nil"); }

/* (g_lock held) */
static ERL_NIF_TERM stream_term(ErlNifEnv *env, mai_stream *s) {
    static const char *const sessions[] = {"play", "mix", "play_and_record"};
    static const char *const presets[] = {"default", "generic", "performance", "unprocessed", "voice"};
    static const char *const presents[] = {"none", "timestamp", "estimate"};
    enum { N = 36 };
    ERL_NIF_TERM k[N], v[N];
    int i = 0;
    uint64_t cbs = atomic_load(&s->callbacks);
    uint32_t fmin = atomic_load(&s->cb_frames_min);
    double out_ms = -1, in_ms = -1;
    if (s->be) be_latency(s, &out_ms, &in_ms);
    bool in = (s->dir & MAI_DIR_IN) != 0, out = (s->dir & MAI_DIR_OUT) != 0;
#define KV(key, val) (k[i] = atom(env, key), v[i] = (val), i++)
    KV("id", enif_make_int(env, s->id));
    KV("processor", string_term(env, s->entry ? s->entry->name : ""));
    KV("direction", atom(env, direction_name(s->dir)));
    KV("running", atom(env, s->be ? "true" : "false"));
    KV("interrupted", atom(env, s->interrupted ? "true" : "false"));
    KV("rate", enif_make_int(env, s->rate));
    KV("channels", enif_make_int(env, out ? s->channels : 0));
    KV("input_channels", enif_make_int(env, in ? s->in_channels : 0));
    KV("burst", enif_make_int(env, s->burst));
    KV("buffer", enif_make_int(env, s->buffer));
    KV("capacity", enif_make_int(env, s->capacity));
    KV("sharing", atom(env, s->exclusive ? "exclusive" : "shared"));
    KV("session", atom(env, sessions[s->session]));
    KV("preset", in ? atom(env, presets[s->preset]) : atom(env, "nil"));
    KV("preset_granted", in ? enif_make_int(env, s->preset_granted) : atom(env, "nil"));
    KV("device_id", enif_make_int(env, s->device_id));
    KV("input_device_id", enif_make_int(env, s->in_device_id));
    KV("latency_ms", ms_term(env, out ? out_ms : -1));
    KV("input_latency_ms", ms_term(env, in ? in_ms : -1));
    KV("present", atom(env, presents[s->present]));
    KV("xruns", enif_make_int(env, s->be ? be_xruns(s) : s->xruns_seen));
    KV("input_underflows", enif_make_uint(env, atomic_load(&s->in_underflows)));
    KV("callbacks", enif_make_uint64(env, cbs));
    KV("frames", enif_make_uint64(env, atomic_load(&s->frames_total)));
    KV("cb_frames", enif_make_uint(env, atomic_load(&s->cb_frames_last)));
    KV("cb_frames_min", enif_make_uint(env, fmin == UINT32_MAX ? 0 : fmin));
    KV("cb_frames_max", enif_make_uint(env, atomic_load(&s->cb_frames_max)));
    KV("cb_max_us", enif_make_double(env, atomic_load(&s->cb_max_ns) / 1000.0));
    KV("cb_avg_us", enif_make_double(env, cbs ? atomic_load(&s->cb_total_ns) / 1000.0 / (double)cbs : 0.0));
    KV("gap_max_us", enif_make_double(env, atomic_load(&s->gap_max_ns) / 1000.0));
    KV("in_peak_db", in ? db_term(env, mai__bits_float(atomic_load(&s->in_peak_bits))) : atom(env, "nil"));
    KV("in_rms_db", in ? db_term(env, mai__bits_float(atomic_load(&s->in_rms_bits))) : atom(env, "nil"));
    KV("reopens", enif_make_uint(env, atomic_load(&s->reopens)));
    KV("posts_dropped", enif_make_uint(env, atomic_load(&s->posts_dropped)));
    KV("last_error", s->last_error[0] ? string_term(env, s->last_error) : atom(env, "nil"));
#undef KV
    return map_of(env, k, v, (unsigned)i);
}

static ERL_NIF_TERM processors_term(ErlNifEnv *env) {
    char names[MAI_MAX_PROCESSORS][MAI_NAME_MAX];
    int n = mai__processor_names(names, MAI_MAX_PROCESSORS);
    ERL_NIF_TERM list = enif_make_list(env, 0);
    for (int i = n - 1; i >= 0; i--) list = enif_make_list_cell(env, string_term(env, names[i]), list);
    return list;
}

/* status() -> %{platform, abi, processors, streams, session} */
static ERL_NIF_TERM nif_status(ErlNifEnv *env, int argc, const ERL_NIF_TERM argv[]) {
    (void)argc; (void)argv;
    mai__register_builtins();
    ERL_NIF_TERM streams = enif_make_list(env, 0);
    pthread_mutex_lock(&g_lock);
    for (int i = MAI_MAX_STREAMS - 1; i >= 0; i--)
        if (atomic_load(&g_streams[i].open)) streams = enif_make_list_cell(env, stream_term(env, &g_streams[i]), streams);
    ERL_NIF_TERM session = be_session_term(env);
    pthread_mutex_unlock(&g_lock);
    ERL_NIF_TERM k[5] = {atom(env, "platform"), atom(env, "abi"), atom(env, "processors"), atom(env, "streams"),
                         atom(env, "session")};
    ERL_NIF_TERM v[5] = {atom(env, be_platform()), enif_make_int(env, MAI_ABI_VERSION), processors_term(env),
                         streams, session};
    return map_of(env, k, v, 5);
}

/* stream_status(id) -> map | {:error, :not_open} */
static ERL_NIF_TERM nif_stream_status(ErlNifEnv *env, int argc, const ERL_NIF_TERM argv[]) {
    (void)argc;
    int id;
    if (!enif_get_int(env, argv[0], &id)) return enif_make_badarg(env);
    pthread_mutex_lock(&g_lock);
    mai_stream *s = mai__stream(id);
    ERL_NIF_TERM r = s ? stream_term(env, s) : error_atom(env, "not_open");
    pthread_mutex_unlock(&g_lock);
    return r;
}

/* reset_stats(id): zero the worst-case counters and the input peak. */
static ERL_NIF_TERM nif_reset_stats(ErlNifEnv *env, int argc, const ERL_NIF_TERM argv[]) {
    (void)argc;
    int id;
    if (!enif_get_int(env, argv[0], &id)) return enif_make_badarg(env);
    pthread_mutex_lock(&g_lock);
    mai_stream *s = mai__stream(id);
    if (s) mai__reset_stats(s);
    pthread_mutex_unlock(&g_lock);
    return s ? atom(env, "ok") : error_atom(env, "not_open");
}

static ERL_NIF_TERM nif_processors(ErlNifEnv *env, int argc, const ERL_NIF_TERM argv[]) {
    (void)argc; (void)argv;
    mai__register_builtins();
    return processors_term(env);
}

/* devices(:input | :output) -> [%{id, name, kind}] | {:error, reason} */
static ERL_NIF_TERM nif_devices(ErlNifEnv *env, int argc, const ERL_NIF_TERM argv[]) {
    (void)argc;
    char dir[8];
    if (!get_atom(env, argv[0], dir, sizeof dir)) return enif_make_badarg(env);
    int d = strcmp(dir, "input") == 0 ? MAI_DIR_IN : strcmp(dir, "output") == 0 ? MAI_DIR_OUT : 0;
    if (!d) return enif_make_badarg(env);
    return be_devices(env, d);
}

/* ── Decodes ───────────────────────────────────────────────────────────── */

/* A decode's sink state (mai_decode.user): its owner and messages. */
typedef struct {
    ErlNifPid owner;
    ErlNifPid logger;
    bool have_logger;
    ErlNifEnv *env;   /* the messages' */
    ErlNifBinary bin; /* the chunk being filled */
    bool have_bin;
    void *mon; /* the owner's monitor (mai_dec_mon) */
    ErlNifMonitor mref;
    bool monitored;
    char path[4096]; /* (MobAudioIo refuses longer paths) */
} mai_dec_owner;

typedef struct {
    int32_t id;
} mai_dec_mon;

static ErlNifResourceType *g_dec_mon_type;

/* The owner exited: the decode stops (its end goes nowhere). */
static void dec_mon_down(ErlNifEnv *env, void *obj, ErlNifPid *pid, ErlNifMonitor *mon) {
    (void)env; (void)pid; (void)mon;
    mai__dec_cancel(((mai_dec_mon *)obj)->id);
}

/* `caller`: the NIF's env, or NULL on the decode's thread. */
static void dec_owner_free(ErlNifEnv *caller, mai_dec_owner *u) {
    if (u->monitored) enif_demonitor_process(caller, u->mon, &u->mref);
    if (u->mon) enif_release_resource(u->mon);
    if (u->env) enif_free_env(u->env);
    free(u);
}

/* {:mob_audio_io, kind, id, payload} to the owner (and, when `log`, the
 * logger). Whether the owner got it. Under g_dec_lock (the sink's calls). */
static bool dec_send(mai_decode *d, const char *kind, ERL_NIF_TERM payload, bool log) {
    mai_dec_owner *u = d->user;
    ErlNifEnv *env = u->env;
    ERL_NIF_TERM msg = enif_make_tuple4(env, atom(env, "mob_audio_io"), atom(env, kind), enif_make_int(env, d->id),
                                        payload);
    if (log && u->have_logger) {
        ErlNifEnv *le = enif_alloc_env();
        if (le) {
            enif_send(NULL, &u->logger, le, enif_make_copy(le, msg));
            enif_free_env(le);
        }
    }
    bool ok = enif_send(NULL, &u->owner, env, msg) != 0;
    enif_clear_env(env); /* (a failed send leaves the message) */
    return ok;
}

static float *dec_buffer(mai_decode *d, size_t samples) {
    mai_dec_owner *u = d->user;
    if (!enif_alloc_binary(samples * sizeof(float), &u->bin)) return NULL;
    u->have_bin = true;
    return (float *)(void *)u->bin.data;
}

static void dec_release(mai_decode *d, float *buf) {
    (void)buf;
    mai_dec_owner *u = d->user;
    if (u->have_bin) enif_release_binary(&u->bin);
    u->have_bin = false;
}

static bool dec_started(mai_decode *d) {
    ErlNifEnv *env = ((mai_dec_owner *)d->user)->env;
    ERL_NIF_TERM k[3] = {atom(env, "rate"), atom(env, "channels"), atom(env, "frames")};
    ERL_NIF_TERM v[3] = {enif_make_int(env, d->rate), enif_make_int(env, d->channels),
                         d->declared_frames > 0 ? enif_make_int64(env, d->declared_frames) : atom(env, "nil")};
    return dec_send(d, "decode_started", map_of(env, k, v, 3), true);
}

/* The chunk's binary goes to the owner as it is (the last, shorter one as
 * a sub-binary): no copy. */
static bool dec_chunk(mai_decode *d, float *buf, int32_t frames) {
    (void)buf;
    mai_dec_owner *u = d->user;
    size_t bytes = (size_t)frames * (size_t)d->channels * sizeof(float), size = u->bin.size;
    ERL_NIF_TERM t = enif_make_binary(u->env, &u->bin);
    u->have_bin = false;
    if (bytes < size) t = enif_make_sub_binary(u->env, t, 0, bytes);
    return dec_send(d, "decoded", t, false);
}

static void dec_ended(mai_decode *d, int how, const char *reason, const char *detail) {
    ErlNifEnv *env = ((mai_dec_owner *)d->user)->env;
    ERL_NIF_TERM k[3], v[3];
    unsigned n = 0;
    k[n] = atom(env, "frames"), v[n++] = enif_make_int64(env, d->frames);
    const char *kind = "decode_cancelled";
    if (how == MAI_DEC_DONE) {
        kind = "decode_done";
        k[n] = atom(env, "rate"), v[n++] = enif_make_int(env, d->rate);
        k[n] = atom(env, "channels"), v[n++] = enif_make_int(env, d->channels);
    } else if (how == MAI_DEC_FAILED) {
        kind = "decode_error";
        k[n] = atom(env, "reason"), v[n++] = atom(env, reason);
        k[n] = atom(env, "detail"), v[n++] = detail[0] ? string_term(env, detail) : atom(env, "nil");
    }
    dec_send(d, kind, map_of(env, k, v, n), true);
}

static const mai_dec_sink g_dec_sink = {dec_buffer, dec_release, dec_started, dec_chunk, dec_ended};

static void *decode_main(void *arg) {
    mai_decode *d = arg;
    mai_dec_owner *u = d->user;
    char detail[MAI_ERRMAX] = "";
    const char *err = be_decode(u->path, d, detail, sizeof detail);
    mai__dec_end(d, err, detail); /* (d is free from here) */
    dec_owner_free(NULL, u);
    return NULL;
}

/* decode_stream(path, owner, opts) -> {:ok, id} | {:error, :busy |
 *   :unknown_processor | :no_memory}. opts: %{mono, chunk_frames, window,
 *   max_ms (0: none), processor ("": the owner gets the chunks)}. The
 *   decode runs on a thread of its own; everything else (a missing file
 *   too) reaches the owner as messages. Dirty IO: it reads the logger
 *   under g_lock. */
static ERL_NIF_TERM nif_decode_stream(ErlNifEnv *env, int argc, const ERL_NIF_TERM argv[]) {
    (void)argc;
    ErlNifPid owner;
    ERL_NIF_TERM mv;
    char mono[8], name[MAI_NAME_MAX];
    int chunk_frames, window;
    ErlNifSInt64 max_ms;
    mai_dec_owner *u = calloc(1, sizeof *u);
    if (!u) return error_atom(env, "no_memory");
    if (!get_string(env, argv[0], u->path, sizeof u->path) || !u->path[0] || !enif_get_local_pid(env, argv[1], &owner) ||
        !enif_is_map(env, argv[2]) || !opt_atom(env, argv[2], "mono", mono, sizeof mono) ||
        !opt_int(env, argv[2], "chunk_frames", &chunk_frames) || !opt_int(env, argv[2], "window", &window) ||
        !enif_get_map_value(env, argv[2], atom(env, "max_ms"), &mv) || !enif_get_int64(env, mv, &max_ms) ||
        !opt_string(env, argv[2], "processor", name, sizeof name) || chunk_frames < 1 || window < 1 || max_ms < 0) {
        free(u);
        return enif_make_badarg(env);
    }
    mai__register_builtins();
    mai_decode *d = mai__dec_alloc();
    if (!d) {
        free(u);
        return error_atom(env, "busy");
    }
    d->mono = strcmp(mono, "true") == 0;
    d->chunk_frames = chunk_frames;
    d->window = window;
    d->max_ms = max_ms;
    d->sink = &g_dec_sink;
    d->user = u;
    if (name[0] && mai__dec_use_processor(d, name) != MAI_OK) {
        mai__dec_discard(d);
        free(u);
        return error_atom(env, "unknown_processor");
    }
    u->owner = owner;
    u->env = enif_alloc_env();
    pthread_mutex_lock(&g_lock);
    u->logger = g_logger;
    u->have_logger = atomic_load(&g_have_logger);
    pthread_mutex_unlock(&g_lock);
    mai_dec_mon *m = enif_alloc_resource(g_dec_mon_type, sizeof(mai_dec_mon));
    int32_t id = d->id;
    m->id = id;
    u->mon = m;
    int mr = enif_monitor_process(env, m, &owner, &u->mref);
    u->monitored = mr == 0;
    pthread_attr_t attr;
    pthread_t t;
    pthread_attr_init(&attr);
    pthread_attr_setdetachstate(&attr, PTHREAD_CREATE_DETACHED);
    int tr = !u->env ? -1 : pthread_create(&t, &attr, decode_main, d);
    pthread_attr_destroy(&attr);
    if (tr != 0) {
        mai__dec_discard(d);
        dec_owner_free(env, u);
        return error_atom(env, "no_memory");
    }
    if (mr > 0) mai__dec_cancel(id); /* the owner is already gone */
    return enif_make_tuple2(env, atom(env, "ok"), enif_make_int(env, id));
}

/* decode_ack(id, n) -> :ok | {:error, :not_running} */
static ERL_NIF_TERM nif_decode_ack(ErlNifEnv *env, int argc, const ERL_NIF_TERM argv[]) {
    (void)argc;
    int id, n;
    if (!enif_get_int(env, argv[0], &id) || !enif_get_int(env, argv[1], &n) || n < 1) return enif_make_badarg(env);
    return mai__dec_ack(id, n) == MAI_OK ? atom(env, "ok") : error_atom(env, "not_running");
}

/* decode_cancel(id) -> :ok (a :decode_cancelled follows, and nothing else)
 *   | {:error, :not_running} (its last message is already sent) */
static ERL_NIF_TERM nif_decode_cancel(ErlNifEnv *env, int argc, const ERL_NIF_TERM argv[]) {
    (void)argc;
    int id;
    if (!enif_get_int(env, argv[0], &id)) return enif_make_badarg(env);
    return mai__dec_cancel(id) == MAI_OK ? atom(env, "ok") : error_atom(env, "not_running");
}

/* set_logger(pid | nil) -> :ok */
static ERL_NIF_TERM nif_set_logger(ErlNifEnv *env, int argc, const ERL_NIF_TERM argv[]) {
    (void)argc;
    ErlNifPid pid;
    pthread_mutex_lock(&g_lock);
    if (enif_get_local_pid(env, argv[0], &pid)) {
        g_logger = pid;
        atomic_store(&g_have_logger, true);
    } else {
        atomic_store(&g_have_logger, false);
    }
    pthread_mutex_unlock(&g_lock);
    return atom(env, "ok");
}

static ERL_NIF_TERM nif_abi_version(ErlNifEnv *env, int argc, const ERL_NIF_TERM argv[]) {
    (void)argc; (void)argv;
    return enif_make_int(env, mai_abi_version());
}

static int nif_load(ErlNifEnv *env, void **priv, ERL_NIF_TERM info) {
    (void)priv; (void)info;
    ErlNifResourceTypeInit init = {.dtor = NULL, .stop = NULL, .down = mon_down};
    g_mon_type = enif_open_resource_type_x(env, "mai_mon", &init, ERL_NIF_RT_CREATE | ERL_NIF_RT_TAKEOVER, NULL);
    if (!g_mon_type) return 1;
    ErlNifResourceTypeInit dec_init = {.dtor = NULL, .stop = NULL, .down = dec_mon_down};
    g_dec_mon_type =
        enif_open_resource_type_x(env, "mai_dec_mon", &dec_init, ERL_NIF_RT_CREATE | ERL_NIF_RT_TAKEOVER, NULL);
    if (!g_dec_mon_type) return 1;
    mai__register_builtins();
    if (!g_log_env) g_log_env = enif_alloc_env();
    if (!g_thread_up) {
        pthread_t t;
        if (pthread_create(&t, NULL, event_main, NULL) != 0) return 2;
        pthread_detach(t);
        g_thread_up = true;
    }
    return 0;
}

static int nif_upgrade(ErlNifEnv *env, void **priv, void **old, ERL_NIF_TERM info) {
    (void)old;
    return nif_load(env, priv, info);
}

static ErlNifFunc nif_funcs[] = {
    {"open", 4, nif_open, ERL_NIF_DIRTY_JOB_IO_BOUND},
    {"close", 1, nif_close, ERL_NIF_DIRTY_JOB_IO_BOUND},
    {"status", 0, nif_status, ERL_NIF_DIRTY_JOB_IO_BOUND},
    {"stream_status", 1, nif_stream_status, ERL_NIF_DIRTY_JOB_IO_BOUND},
    {"reset_stats", 1, nif_reset_stats, ERL_NIF_DIRTY_JOB_IO_BOUND},
    {"processors", 0, nif_processors, 0},
    {"devices", 1, nif_devices, ERL_NIF_DIRTY_JOB_IO_BOUND},
    {"decode_stream", 3, nif_decode_stream, ERL_NIF_DIRTY_JOB_IO_BOUND},
    {"decode_ack", 2, nif_decode_ack, 0},
    {"decode_cancel", 1, nif_decode_cancel, 0},
    {"set_logger", 1, nif_set_logger, ERL_NIF_DIRTY_JOB_IO_BOUND}, /* takes g_lock */
    {"abi_version", 0, nif_abi_version, 0},
};

ERL_NIF_INIT(mob_audio_io_nif, nif_funcs, nif_load, NULL, nif_upgrade, NULL)
