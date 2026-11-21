/*
 * verify_dsp.c: the example app's verification processors (docs/VERIFICATION.md),
 * included by spike_dsp.c. App C again: registered into mob_audio_io from
 * spike_dsp's load().
 *
 *   "tone_loop"     render: a sine at tone/2's frequency (-12 dBFS) while
 *                   playing; capture: the input's first channel into a ring.
 *                   pitch/1 (a NIF, off the audio thread) reads the ring and
 *                   estimates the input's pitch and level. Opened :duplex it
 *                   plays a known tone through the speaker and hears it
 *                   through the microphone; :input only listens.
 *   "loop_latency"  duplex: a 3 ms 2 kHz burst once a second; the capture
 *                   finds its arrival in the input and posts
 *                   <<click_frame, heard_frame, click_present_ns,
 *                   heard_present_ns>> (4 x int64): the acoustic round trip
 *                   in frames on the stream clock, and the plugin's
 *                   presentation times for the same two moments.
 */
#include <math.h>
#include <stdbool.h>

/* ── tone_loop ─────────────────────────────────────────────────────────── */

#define TONE_RING (1 << 16) /* mono samples, a power of two (1.4 s at 48 kHz) */

typedef struct {
    _Atomic int32_t millihz; /* the rendered tone; 0: silent */
    _Atomic int32_t rate;
    _Atomic int64_t written; /* samples captured so far */
    float ring[TONE_RING];
} tone_state;

static tone_state TONE = {.millihz = 1000000};

static void tone_render(void *ctx, float *out, int32_t frames, const mai_time *t) {
    tone_state *s = ctx;
    int32_t mhz = atomic_load_explicit(&s->millihz, memory_order_relaxed);
    if (mhz <= 0 || t->rate <= 0) return;
    const double w = 2.0 * 3.141592653589793 * (mhz / 1000.0) / t->rate;
    const double start = fmod((double)t->frame * w, 2.0 * 3.141592653589793);
    for (int32_t i = 0; i < frames; i++) {
        float x = 0.25f * (float)sin(start + w * i);
        for (int32_t c = 0; c < t->channels; c++) out[i * t->channels + c] = x;
    }
}

static void tone_capture(void *ctx, const float *in, int32_t frames, const mai_time *t) {
    tone_state *s = ctx;
    atomic_store_explicit(&s->rate, t->rate, memory_order_relaxed);
    int64_t w = atomic_load_explicit(&s->written, memory_order_relaxed);
    for (int32_t i = 0; i < frames; i++) s->ring[(w + i) & (TONE_RING - 1)] = in[i * t->channels];
    atomic_store_explicit(&s->written, w + frames, memory_order_release);
}

/* The pitch of `x` (n samples at `rate`) by the normalised square
 * difference (McLeod): the first peak within 80% of the best, refined by a
 * parabola. 0 when nothing periodic between 80 Hz and 4 kHz. */
static double estimate_pitch(const float *x, int n, int rate, double *clarity) {
    int min_lag = rate / 4000, max_lag = rate / 80;
    if (max_lag >= n / 2) max_lag = n / 2 - 1;
    *clarity = 0;
    if (min_lag < 2 || max_lag <= min_lag) return 0;
    static double nsdf[4096];
    if (max_lag >= 4096) max_lag = 4095;
    double best = 0;
    for (int tau = 0; tau <= max_lag; tau++) {
        double r = 0, m = 0;
        for (int i = 0; i + tau < n; i++) {
            r += (double)x[i] * x[i + tau];
            m += (double)x[i] * x[i] + (double)x[i + tau] * x[i + tau];
        }
        nsdf[tau] = m > 0 ? 2 * r / m : 0;
        if (tau >= min_lag && nsdf[tau] > best) best = nsdf[tau];
    }
    if (best <= 0.3) return 0;
    for (int tau = min_lag + 1; tau < max_lag; tau++) {
        if (nsdf[tau] >= 0.8 * best && nsdf[tau] >= nsdf[tau - 1] && nsdf[tau] >= nsdf[tau + 1]) {
            double a = nsdf[tau - 1], b = nsdf[tau], c = nsdf[tau + 1];
            double den = a - 2 * b + c;
            double shift = den != 0 ? 0.5 * (a - c) / den : 0;
            *clarity = b;
            return rate / (tau + shift);
        }
    }
    return 0;
}

/* tone(hz) -> :ok: the frequency tone_loop renders (0 for silence). */
static ERL_NIF_TERM nif_tone(ErlNifEnv *env, int argc, const ERL_NIF_TERM argv[]) {
    (void)argc;
    double hz;
    if (!enif_get_double(env, argv[0], &hz) || hz < 0 || hz > 20000) return enif_make_badarg(env);
    atomic_store(&TONE.millihz, (int32_t)lround(hz * 1000));
    return enif_make_atom(env, "ok");
}

/* pitch(window_ms) -> %{hz, clarity, rms_db, samples}: the last window of
 * tone_loop's input. (A dirty CPU job.) */
static ERL_NIF_TERM nif_pitch(ErlNifEnv *env, int argc, const ERL_NIF_TERM argv[]) {
    (void)argc;
    int ms;
    if (!enif_get_int(env, argv[0], &ms) || ms <= 0 || ms > 500) return enif_make_badarg(env);
    int rate = atomic_load(&TONE.rate);
    int64_t w = atomic_load_explicit(&TONE.written, memory_order_acquire);
    int n = rate > 0 ? (int)((int64_t)rate * ms / 1000) : 0;
    if (n > TONE_RING / 2) n = TONE_RING / 2; /* half the ring: the writer can't lap the copy */
    if (n > w) n = (int)w;
    static float x[TONE_RING / 2];
    double sum = 0, clarity = 0, hz = 0;
    for (int i = 0; i < n; i++) {
        x[i] = TONE.ring[(w - n + i) & (TONE_RING - 1)];
        sum += (double)x[i] * x[i];
    }
    if (n > 0) hz = estimate_pitch(x, n, rate, &clarity);
    double rms = n > 0 ? sqrt(sum / n) : 0;
    ERL_NIF_TERM k[4] = {enif_make_atom(env, "hz"), enif_make_atom(env, "clarity"), enif_make_atom(env, "rms_db"),
                         enif_make_atom(env, "samples")};
    ERL_NIF_TERM v[4] = {enif_make_double(env, hz), enif_make_double(env, clarity),
                         enif_make_double(env, rms > 1e-10 ? 20 * log10(rms) : -200.0), enif_make_int(env, n)};
    ERL_NIF_TERM m;
    enif_make_map_from_arrays(env, k, v, 4, &m);
    return m;
}

/* ── loop_latency ──────────────────────────────────────────────────────── */

typedef struct {
    _Atomic int32_t stream;
    /* the audio thread's own */
    int64_t click_frame, click_present;
    bool waiting;
    double noise; /* running mean of |x| while not waiting */
} latency_state;

static latency_state LAT;

#define BURST_HZ 2000.0
#define BURST_MS 3

static void latency_render(void *ctx, float *out, int32_t frames, const mai_time *t) {
    latency_state *s = ctx;
    int64_t period = t->rate, len = (int64_t)t->rate * BURST_MS / 1000;
    for (int32_t i = 0; i < frames; i++) {
        int64_t f = t->frame + i, k = f % period;
        if (k == 0) {
            s->click_frame = f;
            s->click_present = t->present_ns ? t->present_ns + (int64_t)i * 1000000000LL / t->rate : 0;
            s->waiting = true;
        }
        if (k < len) {
            double win = 0.5 - 0.5 * cos(2 * 3.141592653589793 * (double)k / (double)len);
            float x = (float)(0.7 * win * sin(2 * 3.141592653589793 * BURST_HZ * (double)k / t->rate));
            for (int32_t c = 0; c < t->channels; c++) out[i * t->channels + c] = x;
        }
    }
}

static void latency_capture(void *ctx, const float *in, int32_t frames, const mai_time *t) {
    latency_state *s = ctx;
    for (int32_t i = 0; i < frames; i++) {
        int64_t f = t->frame + i;
        float a = fabsf(in[i * t->channels]);
        if (!s->waiting) {
            s->noise += (a - s->noise) * 0.001;
            continue;
        }
        double threshold = fmax(0.02, 8 * s->noise);
        if (f > s->click_frame && a > threshold) {
            int64_t msg[4] = {s->click_frame, f, s->click_present,
                              t->present_ns ? t->present_ns + (int64_t)i * 1000000000LL / t->rate : 0};
            mai_post(atomic_load(&s->stream), msg, sizeof msg);
            s->waiting = false;
        } else if (f - s->click_frame > t->rate / 2) {
            int64_t msg[4] = {s->click_frame, -1, s->click_present, 0}; /* not heard */
            mai_post(atomic_load(&s->stream), msg, sizeof msg);
            s->waiting = false;
        }
    }
}

static void latency_event(void *ctx, int32_t stream, int32_t event, const mai_time *t) {
    latency_state *s = ctx;
    (void)t;
    if (event != MAI_EVENT_STARTED) return;
    atomic_store(&s->stream, stream);
    s->waiting = false;
    s->noise = 0;
}
