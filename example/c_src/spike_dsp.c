/*
 * c_src/spike_dsp.c: the example app's own C (a static NIF, mob.exs
 * :static_nifs, archs: [:all]). It registers two processors into
 * mob_audio_io from its load(), through the plugin's C ABI:
 *
 *   "spike_click"      a 1 kHz blip every half second; each click posts
 *                      {frame, host_ns} to the stream's owner (mai_post).
 *   "magpie_sampler"   Magpie's sampler core (c_src/magpie/sampler.c, a copy
 *                      of magpie/c_src/sampler.c at f397b7b) rendered by
 *                      sp_render; count_in/2 plays its metronome clicks, and
 *                      its engine events are posted to the owner.
 *
 * The header comes by a relative path: the plugin manifest can't export an
 * include path (MOB-541). This example uses the plugin as a path dependency
 * (the repo root, ../..); an app with the Hex package would include
 * "../deps/mob_audio_io/priv/include/mob_audio_io.h".
 */
#include <erl_nif.h>
#include <pthread.h>
#include <stdatomic.h>

#include "../../priv/include/mob_audio_io.h"
#include "magpie/sampler.c"

/* ── spike_click ───────────────────────────────────────────────────────── */

typedef struct {
    _Atomic int32_t stream; /* set by stream_event(STARTED), before the first render */
    _Atomic uint32_t clicks;
} click_state;

static click_state CLICK;

static void click_render(void *ctx, float *out, int32_t frames, const mai_time *t) {
    click_state *c = ctx;
    int64_t period = t->rate / 2, len = t->rate / 200; /* every 0.5 s, 5 ms long */
    for (int32_t i = 0; i < frames; i++) {
        int64_t f = t->frame + i, k = f % period;
        if (k == 0) {
            int64_t msg[2] = {f, t->host_ns + (int64_t)i * 1000000000LL / t->rate};
            mai_post(atomic_load(&c->stream), msg, sizeof msg);
            atomic_fetch_add(&c->clicks, 1);
        }
        if (k < len) {
            float x = 0.3f * (float)sin(2.0 * 3.141592653589793 * 1000.0 * (double)k / t->rate) *
                      (1.0f - (float)k / (float)len);
            for (int32_t ch = 0; ch < t->channels; ch++) out[i * t->channels + ch] = x;
        }
    }
}

static void click_event(void *ctx, int32_t stream, int32_t event, const mai_time *t) {
    (void)t;
    if (event == MAI_EVENT_STARTED) atomic_store(&((click_state *)ctx)->stream, stream);
}

/* ── magpie_sampler ────────────────────────────────────────────────────── */

static sp_engine SP;
static pthread_mutex_t sp_producers = PTHREAD_MUTEX_INITIALIZER; /* sp_push's producers (sampler.c) */
static _Atomic int32_t sp_stream;

static bool push(const sp_cmd *c) {
    pthread_mutex_lock(&sp_producers);
    bool ok = sp_push(&SP, c);
    pthread_mutex_unlock(&sp_producers);
    return ok;
}

static void sampler_render(void *ctx, float *out, int32_t frames, const mai_time *t) {
    sp_engine *e = ctx;
    (void)t;
    sp_render(e, out, frames);
    /* the engine's events to the owner: {type, pad, a, b, frame} */
    sp_event ev;
    for (int n = 0; n < 16 && sp_pop_event(e, &ev); n++) {
        int64_t msg[5] = {ev.type, ev.pad, ev.a, ev.b, ev.frame};
        mai_post(atomic_load(&sp_stream), msg, sizeof msg);
    }
}

/* Non-real-time: the engine runs at the stream's rate (Magpie's SP_CMD_RATE
 * at open, magpie_audio.c:538). */
static void sampler_event(void *ctx, int32_t stream, int32_t event, const mai_time *t) {
    (void)ctx;
    if (event != MAI_EVENT_STARTED) return;
    atomic_store(&sp_stream, stream);
    sp_cmd c = {.type = SP_CMD_RATE, .u.rate = t->rate};
    push(&c);
}

/* ── NIFs ──────────────────────────────────────────────────────────────── */

static int32_t reg_click = 1, reg_sampler = 1;

static ERL_NIF_TERM nif_registered(ErlNifEnv *env, int argc, const ERL_NIF_TERM argv[]) {
    (void)argc; (void)argv;
    return enif_make_list2(
        env, enif_make_tuple2(env, enif_make_atom(env, "spike_click"), enif_make_int(env, reg_click)),
        enif_make_tuple2(env, enif_make_atom(env, "magpie_sampler"), enif_make_int(env, reg_sampler)));
}

/* abi() -> the linked plugin's MAI_ABI_VERSION (a call into plugin C) */
static ERL_NIF_TERM nif_abi(ErlNifEnv *env, int argc, const ERL_NIF_TERM argv[]) {
    (void)argc; (void)argv;
    return enif_make_int(env, mai_abi_version());
}

/* count_in(beats, bpm) -> :ok | {:error, :full} */
static ERL_NIF_TERM nif_count_in(ErlNifEnv *env, int argc, const ERL_NIF_TERM argv[]) {
    (void)argc;
    int beats;
    double bpm;
    if (!enif_get_int(env, argv[0], &beats) || !enif_get_double(env, argv[1], &bpm)) return enif_make_badarg(env);
    sp_cmd tempo = {.type = SP_CMD_TEMPO, .u.value = bpm};
    sp_cmd c = {.type = SP_CMD_COUNT_IN, .t_ns = 0, .u.count_in = {.id = 1, .beats = beats}};
    if (push(&tempo) && push(&c)) return enif_make_atom(env, "ok");
    return enif_make_tuple2(env, enif_make_atom(env, "error"), enif_make_atom(env, "full"));
}

/* stats() -> %{sampler_frame, sampler_rate, clicks} */
static ERL_NIF_TERM nif_stats(ErlNifEnv *env, int argc, const ERL_NIF_TERM argv[]) {
    (void)argc; (void)argv;
    ERL_NIF_TERM k[3] = {enif_make_atom(env, "sampler_frame"), enif_make_atom(env, "sampler_rate"),
                         enif_make_atom(env, "clicks")};
    ERL_NIF_TERM v[3] = {enif_make_int64(env, SP.frame), enif_make_int(env, SP.rate),
                         enif_make_uint(env, atomic_load(&CLICK.clicks))};
    ERL_NIF_TERM m;
    enif_make_map_from_arrays(env, k, v, 3, &m);
    return m;
}

static int load(ErlNifEnv *env, void **priv, ERL_NIF_TERM info) {
    (void)env; (void)priv; (void)info;
    static const mai_processor click = {click_render, NULL, click_event};
    static const mai_processor sampler = {sampler_render, NULL, sampler_event};
    sp_init(&SP, 48000);
    reg_click = mai_register("spike_click", &click, &CLICK);
    reg_sampler = mai_register("magpie_sampler", &sampler, &SP);
    return 0;
}

static ErlNifFunc funcs[] = {
    {"registered", 0, nif_registered, 0},
    {"abi", 0, nif_abi, 0},
    {"count_in", 2, nif_count_in, 0},
    {"stats", 0, nif_stats, 0},
};

ERL_NIF_INIT(spike_dsp, funcs, load, NULL, NULL, NULL)
