/*
 * mai_nif.c: the NIF half of mob_audio_io shared by both platforms: the
 * mob_audio_io_nif functions, owner monitors and the event thread. The
 * platform file includes it, then defines the backend (be_*):
 *
 *   priv/native/android/mob_audio_io_nif.c   AAudio, loaded with dlopen
 *   priv/native/ios/mob_audio_io_nif.m       RemoteIO + AVAudioSession
 *
 * Locking: g_lock serialises open, close, reopen and status (NIF calls on
 * dirty IO schedulers and the event thread). The audio thread takes no lock.
 *
 * The event thread (one, started at load, polls every 5 ms) does all the
 * work the audio thread and the OS callbacks must not: it reopens a lost or
 * interrupted stream, grows an Android buffer after an underrun, drains
 * mai_post queues, closes a stream whose owner exited, and sends the owner
 * its messages:
 *
 *   {:mob_audio_io, :started, id, info}         opened, or reopened (info.reopened)
 *   {:mob_audio_io, :stopped, id, %{reason: r}} :closed | :device_lost | :interrupted
 *   {:mob_audio_io, :device_lost, id, %{}}      Android disconnect; reopening follows
 *   {:mob_audio_io, :interrupted, id, %{}}      iOS session interruption began
 *   {:mob_audio_io, :resumed, id, %{}}          ... ended (a :started follows)
 *   {:mob_audio_io, :route_changed, id, %{}}    new output route (reopened if the rate changed)
 *   {:mob_audio_io, :xrun, id, %{xruns: n, buffer: frames}}
 *   {:mob_audio_io, :error, id, %{reason: atom, detail: string}}  a reopen failed (retried)
 *   {:mob_audio_io, :post, id, binary}          mai_post from the processor
 *
 * Every message but :post is also sent to the logger process set with
 * set_logger/1 (MobAudioIo.Log), which logs it with a [mob_audio_io] prefix:
 * native code can't reach Sloppy Joe's `logs`.
 */
#include <erl_nif.h>
#include <unistd.h>

#include "mai_core.c"

/* ── The backend each platform file defines ────────────────────────────── */

/* (g_lock held for all but be_platform) */
static const char *be_platform(void);
/* Load the platform API; NULL or an error atom. */
static const char *be_load(void);
/* Open (not start) s's stream from s->want_*: set s->be, rate, channels,
 * burst, buffer, capacity, exclusive, device_id. NULL or an error atom, with
 * s->last_error set. */
static const char *be_open(mai_stream *s);
/* Start the opened stream; NULL or an error atom. */
static const char *be_start(mai_stream *s);
/* Stop and close; no callback runs once it returns. s->be = NULL. */
static void be_close(mai_stream *s);
/* The underrun count so far (since open). */
static int32_t be_xruns(mai_stream *s);
/* After new underruns: Android adds a burst of buffer, up to the capacity. */
static void be_tune(mai_stream *s);
/* Output latency in ms (< 0 unknown). */
static double be_latency_ms(mai_stream *s);
/* No stream open any more: iOS deactivates the session. */
static void be_idle(void);
/* Platform-wide readback for status/0 (iOS: the session), or nil. */
static ERL_NIF_TERM be_session_term(ErlNifEnv *env);

/* ── State ─────────────────────────────────────────────────────────────── */

static pthread_mutex_t g_lock = PTHREAD_MUTEX_INITIALIZER;

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
#define XRUN_POLL_NS 100000000LL
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

/* The info of a :started message. */
static ERL_NIF_TERM started_info(ErlNifEnv *env, mai_stream *s, bool reopened) {
    ERL_NIF_TERM k[6], v[6];
    k[0] = atom(env, "rate"); v[0] = enif_make_int(env, s->rate);
    k[1] = atom(env, "channels"); v[1] = enif_make_int(env, s->channels);
    k[2] = atom(env, "burst"); v[2] = enif_make_int(env, s->burst);
    k[3] = atom(env, "buffer"); v[3] = enif_make_int(env, s->buffer);
    k[4] = atom(env, "sharing"); v[4] = atom(env, s->exclusive ? "exclusive" : "shared");
    k[5] = atom(env, "reopened"); v[5] = atom(env, reopened ? "true" : "false");
    return map_of(env, k, v, 6);
}

static ERL_NIF_TERM reason_info(ErlNifEnv *env, const char *reason) {
    ERL_NIF_TERM k = atom(env, "reason"), v = atom(env, reason);
    return map_of(env, &k, &v, 1);
}

/* {:mob_audio_io, kind, id, payload} to the stream's owner and (for all but
 * posts) the logger. From a NIF call (`env` process-bound, from_thread
 * false) or the event thread (`env` its message env, cleared by the send). */
static void notify(ErlNifEnv *env, bool from_thread, mai_stream *s, const char *kind, ERL_NIF_TERM payload,
                   bool log) {
    ERL_NIF_TERM msg = enif_make_tuple4(env, atom(env, "mob_audio_io"), atom(env, kind), enif_make_int(env, s->id),
                                        payload);
    ErlNifPid owner = g_owners[slot_of(s)].owner;
    if (log && atomic_load(&g_have_logger)) {
        ErlNifPid logger = g_logger;
        if (from_thread) {
            ERL_NIF_TERM copy = enif_make_copy(g_log_env, msg);
            enif_send(NULL, &logger, g_log_env, copy);
        } else {
            enif_send(env, &logger, NULL, msg);
        }
    }
    if (from_thread) enif_send(NULL, &owner, env, msg);
    else enif_send(env, &owner, NULL, msg);
}

/* ── Open / close (g_lock held) ────────────────────────────────────────── */

/* Open and start s's platform stream, telling the processor STARTED before
 * the first render. NULL or an error atom (s->be NULL then). */
static const char *start_stream_locked(mai_stream *s) {
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

/* Free the slot: closes the platform stream, unpins the processor, drops the
 * monitor. `env`: the caller's (NULL on the event thread). */
static void free_stream_locked(ErlNifEnv *env, mai_stream *s) {
    stop_stream_locked(s);
    atomic_store_explicit(&s->open, false, memory_order_release);
    mai__release(s);
    mai_owner *o = &g_owners[slot_of(s)];
    if (o->monitored) enif_demonitor_process(env, o->res, &o->mon);
    if (o->res) enif_release_resource(o->res);
    o->res = NULL;
    o->monitored = false;
    if (!any_open_locked()) be_idle();
}

/* ── Event thread ──────────────────────────────────────────────────────── */

static void try_reopen_locked(ErlNifEnv *env, mai_stream *s, int64_t now, const char *why) {
    const char *err = start_stream_locked(s);
    if (!err) {
        atomic_fetch_add(&s->reopens, 1);
        s->interrupted = false;
        s->next_reopen_ns = 0;
        notify(env, true, s, "started", started_info(env, s, true), true);
        return;
    }
    s->next_reopen_ns = now + REOPEN_RETRY_NS;
    ERL_NIF_TERM k[3] = {atom(env, "reason"), atom(env, "detail"), atom(env, "while")};
    ERL_NIF_TERM v[3] = {atom(env, err), string_term(env, s->last_error), atom(env, why)};
    notify(env, true, s, "error", map_of(env, k, v, 3), true);
}

static void handle_pending_locked(ErlNifEnv *env, mai_stream *s, uint32_t pf, int64_t now) {
    if (pf & MAI_PF_INTERRUPTED) {
        stop_stream_locked(s);
        s->interrupted = true;
        mai__event(s, MAI_EVENT_INTERRUPTED);
        notify(env, true, s, "interrupted", empty_map(env), true);
        notify(env, true, s, "stopped", reason_info(env, "interrupted"), true);
    }
    if (pf & MAI_PF_RESUMED) {
        notify(env, true, s, "resumed", empty_map(env), true);
        s->interrupted = false; /* (a failed reopen is retried from here on) */
        if (!s->be) try_reopen_locked(env, s, now, "resume");
    }
    if (pf & MAI_PF_DEVICE_LOST) {
        notify(env, true, s, "device_lost", empty_map(env), true);
        stop_stream_locked(s);
        notify(env, true, s, "stopped", reason_info(env, "device_lost"), true);
        try_reopen_locked(env, s, now, "device_lost");
    }
    if (pf & (MAI_PF_ROUTE | MAI_PF_RESET)) {
        /* ROUTE alone: a new route at the same rate (the unit follows it).
         * ROUTE|RESET: the rate changed; RESET alone: media services were
         * reset. Both need the platform stream rebuilt. */
        const char *why = !(pf & MAI_PF_RESET) ? "new_route" : (pf & MAI_PF_ROUTE) ? "new_rate" : "media_reset";
        mai__event(s, MAI_EVENT_ROUTE_CHANGED);
        notify(env, true, s, "route_changed", reason_info(env, why), true);
        if ((pf & MAI_PF_RESET) && !s->interrupted) {
            stop_stream_locked(s);
            try_reopen_locked(env, s, now, "route_changed");
        }
    }
}

static void poll_stream_locked(ErlNifEnv *env, mai_stream *s, int64_t now, bool xrun_tick) {
    uint32_t pf = atomic_exchange(&s->pending, 0);
    if (pf & MAI_PF_OWNER_DOWN) {
        free_stream_locked(NULL, s);
        return;
    }
    if (pf) handle_pending_locked(env, s, pf, now);
    if (!s->be && !s->interrupted && s->next_reopen_ns && now >= s->next_reopen_ns)
        try_reopen_locked(env, s, now, "retry");
    if (xrun_tick && s->be) {
        int32_t x = be_xruns(s);
        if (x > s->xruns_seen) {
            s->xruns_seen = x;
            be_tune(s);
            ERL_NIF_TERM k[2] = {atom(env, "xruns"), atom(env, "buffer")};
            ERL_NIF_TERM v[2] = {enif_make_int(env, x), enif_make_int(env, s->buffer)};
            notify(env, true, s, "xrun", map_of(env, k, v, 2), true);
        }
    }
    mai_msg m;
    while (mai__pop_post(s, &m)) {
        notify(env, true, s, "post", binary_of(env, m.data, m.len), false);
        enif_clear_env(env); /* (no-op after a send; the env stays usable) */
    }
}

static void *event_main(void *arg) {
    (void)arg;
    ErlNifEnv *env = enif_alloc_env();
    int64_t next_xrun = 0;
    for (;;) {
        usleep(EVENT_PERIOD_US);
        int64_t now = mai__now_ns();
        bool xrun_tick = now >= next_xrun;
        if (xrun_tick) next_xrun = now + XRUN_POLL_NS;
        pthread_mutex_lock(&g_lock);
        for (int i = 0; i < MAI_MAX_STREAMS; i++) {
            mai_stream *s = &g_streams[i];
            if (!atomic_load(&s->open)) continue;
            poll_stream_locked(env, s, now, xrun_tick);
            enif_clear_env(env);
        }
        pthread_mutex_unlock(&g_lock);
    }
    return NULL;
}

/* ── Monitors ──────────────────────────────────────────────────────────── */

static void mon_down(ErlNifEnv *env, void *obj, ErlNifPid *pid, ErlNifMonitor *mon) {
    (void)env; (void)pid; (void)mon;
    mai_stream *s = mai__stream(((mai_mon *)obj)->id);
    if (s) atomic_fetch_or(&s->pending, MAI_PF_OWNER_DOWN);
}

/* ── NIFs ──────────────────────────────────────────────────────────────── */

static int get_atom(ErlNifEnv *env, ERL_NIF_TERM t, char *buf, unsigned n) {
    return enif_get_atom(env, t, buf, n, ERL_NIF_LATIN1) > 0;
}

static bool get_name(ErlNifEnv *env, ERL_NIF_TERM t, char *buf) {
    ErlNifBinary b;
    if (!enif_inspect_binary(env, t, &b) || b.size == 0 || b.size >= MAI_NAME_MAX) return false;
    memcpy(buf, b.data, b.size);
    buf[b.size] = 0;
    return true;
}

/* open(owner, direction, processor, rate, frames, sharing, session)
 *   -> {:ok, id} | {:error, reason} */
static ERL_NIF_TERM nif_open(ErlNifEnv *env, int argc, const ERL_NIF_TERM argv[]) {
    (void)argc;
    ErlNifPid owner;
    char dir[16], sharing[16], session[24], name[MAI_NAME_MAX];
    int rate, frames;
    if (!enif_get_local_pid(env, argv[0], &owner) || !get_atom(env, argv[1], dir, sizeof dir) ||
        !get_name(env, argv[2], name) || !enif_get_int(env, argv[3], &rate) || !enif_get_int(env, argv[4], &frames) ||
        !get_atom(env, argv[5], sharing, sizeof sharing) || !get_atom(env, argv[6], session, sizeof session) ||
        rate < 0 || frames < 0)
        return enif_make_badarg(env);
    if (strcmp(dir, "output") != 0) return error_atom(env, "unsupported");

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
    if (mai__acquire(s, name) != MAI_OK) {
        pthread_mutex_unlock(&g_lock);
        return error_atom(env, "unknown_processor");
    }
    s->want_rate = rate;
    s->want_frames = frames;
    s->sharing = strcmp(sharing, "exclusive") == 0 ? MAI_SHARING_EXCLUSIVE : MAI_SHARING_SHARED;
    s->session = strcmp(session, "mix") == 0               ? MAI_SESSION_MIX
                 : strcmp(session, "play_and_record") == 0 ? MAI_SESSION_PLAY_AND_RECORD
                                                           : MAI_SESSION_PLAY;
    s->channels = 2;
    mai_owner *o = &g_owners[slot_of(s)];
    o->owner = owner;
    o->monitored = false;
    o->res = NULL;
    /* open before the platform stream starts: mai_post works from the first render */
    atomic_store_explicit(&s->open, true, memory_order_release);
    err = start_stream_locked(s);
    if (err) {
        atomic_store_explicit(&s->open, false, memory_order_release);
        mai__release(s);
        if (!any_open_locked()) be_idle();
        pthread_mutex_unlock(&g_lock);
        return error_atom(env, err);
    }
    mai_mon *m = enif_alloc_resource(g_mon_type, sizeof(mai_mon));
    m->id = s->id;
    o->res = m;
    int mr = enif_monitor_process(env, m, &owner, &o->mon);
    o->monitored = mr == 0;
    if (mr > 0) atomic_fetch_or(&s->pending, MAI_PF_OWNER_DOWN); /* already gone: the event thread closes it */
    notify(env, false, s, "started", started_info(env, s, false), true);
    ERL_NIF_TERM ok = enif_make_tuple2(env, atom(env, "ok"), enif_make_int(env, s->id));
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
    notify(env, false, s, "stopped", reason_info(env, "closed"), true);
    free_stream_locked(env, s);
    pthread_mutex_unlock(&g_lock);
    return atom(env, "ok");
}

/* (g_lock held) */
static ERL_NIF_TERM stream_term(ErlNifEnv *env, mai_stream *s) {
    enum { N = 23 };
    ERL_NIF_TERM k[N], v[N];
    int i = 0;
    uint64_t cbs = atomic_load(&s->callbacks);
    uint32_t fmin = atomic_load(&s->cb_frames_min);
    double lat = s->be ? be_latency_ms(s) : -1;
#define KV(key, val) (k[i] = atom(env, key), v[i] = (val), i++)
    KV("id", enif_make_int(env, s->id));
    KV("processor", string_term(env, s->entry ? s->entry->name : ""));
    KV("direction", atom(env, "output"));
    KV("running", atom(env, s->be ? "true" : "false"));
    KV("interrupted", atom(env, s->interrupted ? "true" : "false"));
    KV("rate", enif_make_int(env, s->rate));
    KV("channels", enif_make_int(env, s->channels));
    KV("burst", enif_make_int(env, s->burst));
    KV("buffer", enif_make_int(env, s->buffer));
    KV("capacity", enif_make_int(env, s->capacity));
    KV("sharing", atom(env, s->exclusive ? "exclusive" : "shared"));
    KV("device_id", enif_make_int(env, s->device_id));
    KV("latency_ms", lat >= 0 ? enif_make_double(env, lat) : atom(env, "nil"));
    KV("xruns", enif_make_int(env, s->be ? be_xruns(s) : s->xruns_seen));
    KV("callbacks", enif_make_uint64(env, cbs));
    KV("frames", enif_make_uint64(env, atomic_load(&s->frames_total)));
    KV("cb_max_us", enif_make_double(env, atomic_load(&s->cb_max_ns) / 1000.0));
    KV("cb_avg_us", enif_make_double(env, cbs ? atomic_load(&s->cb_total_ns) / 1000.0 / (double)cbs : 0.0));
    KV("gap_max_us", enif_make_double(env, atomic_load(&s->gap_max_ns) / 1000.0));
    KV("cb_frames", enif_make_tuple2(env, enif_make_uint(env, fmin == UINT32_MAX ? 0 : fmin),
                                     enif_make_uint(env, atomic_load(&s->cb_frames_max))));
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

/* reset_stats(id): zero the worst-case counters (callbacks and frames go on). */
static ERL_NIF_TERM nif_reset_stats(ErlNifEnv *env, int argc, const ERL_NIF_TERM argv[]) {
    (void)argc;
    int id;
    if (!enif_get_int(env, argv[0], &id)) return enif_make_badarg(env);
    pthread_mutex_lock(&g_lock);
    mai_stream *s = mai__stream(id);
    if (s) {
        atomic_store(&s->cb_max_ns, 0);
        atomic_store(&s->gap_max_ns, 0);
        atomic_store(&s->cb_frames_min, UINT32_MAX);
        atomic_store(&s->cb_frames_max, 0);
    }
    pthread_mutex_unlock(&g_lock);
    return s ? atom(env, "ok") : error_atom(env, "not_open");
}

static ERL_NIF_TERM nif_processors(ErlNifEnv *env, int argc, const ERL_NIF_TERM argv[]) {
    (void)argc; (void)argv;
    mai__register_builtins();
    return processors_term(env);
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
    g_mon_type = enif_open_resource_type_x(env, "mai_mon", &init, ERL_NIF_RT_CREATE, NULL);
    if (!g_mon_type) return 1;
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
    {"open", 7, nif_open, ERL_NIF_DIRTY_JOB_IO_BOUND},
    {"close", 1, nif_close, ERL_NIF_DIRTY_JOB_IO_BOUND},
    {"status", 0, nif_status, ERL_NIF_DIRTY_JOB_IO_BOUND},
    {"stream_status", 1, nif_stream_status, ERL_NIF_DIRTY_JOB_IO_BOUND},
    {"reset_stats", 1, nif_reset_stats, ERL_NIF_DIRTY_JOB_IO_BOUND},
    {"processors", 0, nif_processors, 0},
    {"set_logger", 1, nif_set_logger, 0},
    {"abi_version", 0, nif_abi_version, 0},
};

ERL_NIF_INIT(mob_audio_io_nif, nif_funcs, nif_load, NULL, nif_upgrade, NULL)
