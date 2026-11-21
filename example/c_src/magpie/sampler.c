/*
 * c_src/sampler.c — Magpie's sampler core: pads, a voice pool with choke
 * groups and stealing, the step sequencer, the offline resampler, and the
 * lock-free queues between the BEAM and the audio thread.
 *
 * Plain C, no Android and no erl_nif: c_src/magpie_audio.c includes it for
 * the phone, c_src/test/sampler_host.c for the host harness.
 *
 * Roles (one thread each at any moment):
 *
 *   producer   NIF calls push commands (sp_push). Several BEAM threads may
 *              call, so the caller serialises producers with a mutex the
 *              renderer never takes.
 *   renderer   sp_render, the AAudio data callback: drains the commands,
 *              runs the sequencer, mixes the voices. It never locks,
 *              allocates or frees. While no stream runs the control path
 *              takes this role with sp_drain (under its own lock).
 *   consumer   sp_pop_event / sp_pop_garbage: the NIF's event thread turns
 *              events into messages and frees retired samples and patterns.
 *
 * Time: engine frames count what the renderer has rendered since start.
 * The sequencer's position m is in ticks (96 PPQN, a 16th = 24 ticks) since
 * play, linear in frames: m(f) = base_m + (f − base_frame) · ticks_per_frame,
 * rebased on a tempo change so nothing accumulates. A position x sounds at
 * the first frame f with m(f) ≥ x (to 1e-6 frame), so every event lands on
 * one exact frame whatever the block sizes.
 *
 * Swing (fraction s, 0.5 straight … 0.75) warps time inside each 8th-note
 * pair (48 ticks): straight [0, 24) maps onto [0, 48s) and [24, 48) onto
 * [48s, 48). A step's tick 0 lands on its swung start and an off-grid tick
 * keeps its relative place; hits are unwarped the same way, so a recorded
 * hit plays back where it was heard.
 *
 * MIDI (c_src/midi.c holds the parser and the out ring): each incoming
 * message is a command (SP_CMD_MIDI, with its CLOCK_MONOTONIC timestamp)
 * the renderer maps through the kit's table (sp_midi_map: per note the
 * pads it plays, per pad the note it sends). Voices started by touch or by
 * the sequencer send note-on on their first frame and note-off when they
 * stop sounding at full (release, cut or end). Clock send puts a tick on
 * every 4th position (24 PPQN), exact to the frame like the steps. An
 * incoming clock is not followed (yet): clock and transport in do nothing.
 * Out messages carry their engine frame and go, sorted, into an SPSC ring
 * the sending thread drains.
 */

#include <math.h>
#include <stdatomic.h>
#include <stdbool.h>
#include <stdint.h>
#include <stdlib.h>
#include <string.h>

#include "midi.c"

#define SP_PADS 64
#define SP_POLY 32       /* voices sounding at full level */
#define SP_VOICES 48     /* plus room for voices fading out (choked, stolen, released) */
#define SP_SLOTS 256     /* loaded samples */
#define SP_CMDS 1024     /* power of two */
#define SP_EVENTS 2048   /* power of two */
#define SP_GARBAGE 512   /* power of two */
#define SP_PENDING 32    /* quantized triggers waiting for their grid point */
#define SP_MARKS 512     /* power of two: rendered segments kept for stamping hits */
#define SP_ACTIONS 512   /* sequencer actions in one block */
#define SP_MAX_STEPS 4096 /* events in one pattern */
#define SP_MIDI_BLOCK 256 /* MIDI out messages in one block */
#define SP_MIDI_SOURCES 64 /* MIDI inputs (sources 0..63): the bits of midi.gone, mob_midi's MOB_MIDI_MAX_SOURCES */

#define SP_PPQN 96
#define SP_STEP 24
#define SP_PAIR 48
#define SP_BAR 384
#define SP_DEFAULT_LENGTH (16 * SP_STEP) /* the playhead's cycle with no pattern */

#define SP_CUT_MS 4.0           /* choke and steal ramp */
#define SP_MIN_RELEASE_MS 5.0   /* release of a gate, loop, keys or toggle voice */
#define SP_DECLICK_IN_MS 0.5    /* every voice fades in at least this long */
#define SP_DECLICK_OUT_MS 1.0   /* and out at its region's end */
#define SP_FRAME_TOL 1e-6       /* frames: a position on a frame sounds on it */
#define SP_SEG_EPS 1e-7         /* ticks: just before a change point */

#define SP_PEAK_FRAMES 256      /* frames per peak byte */
#define SP_SOFT_KNEE 0.8f

enum { SP_ONE_SHOT, SP_GATE, SP_LOOP, SP_TOGGLE, SP_KEYS };

/* Who started a voice: played (touch, the sequencer, a BEAM call) sends
 * MIDI out; a MIDI note doesn't echo. */
enum { SP_SRC_PLAYED, SP_SRC_MIDI };

enum { SP_CLOCK_INTERNAL, SP_CLOCK_SEND };

/* ── Data ─────────────────────────────────────────────────────────────────── */

/* A decoded sample, interleaved 16-bit at `rate`. The renderer counts the
 * voices playing it; one that is replaced or unloaded is retired and goes to
 * the garbage queue once no voice plays it. */
typedef struct sp_sample {
    int16_t *data;
    int64_t frames;
    int channels; /* 1 or 2 */
    int rate;
    size_t bytes;
    int refs;                       /* renderer only */
    struct sp_sample *next_retired; /* renderer only */
} sp_sample;

typedef struct {
    int slot;           /* −1: empty */
    int64_t start, end; /* region in sample frames, end exclusive; end ≤ 0: the sample's end */
    float gain;         /* linear */
    float pitch;        /* semitones */
    float fade_in_ms, fade_out_ms;
    bool reverse;
    uint8_t mode, choke, root;
} sp_pad;

typedef struct {
    int32_t pos;  /* straight ticks in [0, length): step · 24 + tick, wrapped */
    uint8_t pad, vel;
    int16_t note; /* −1: the pad's root */
} sp_step;

typedef struct sp_pattern {
    uint32_t id;
    int32_t length; /* ticks: 16 or 32 steps */
    int32_t count;
    sp_step steps[]; /* sorted by pos */
} sp_pattern;

/* The kit's MIDI, compiled by Magpie.Midi.Map: which pads a note plays and
 * which note a pad sends. Allocated by the control side, swapped in whole
 * (SP_CMD_MIDI_MAP) and retired through the garbage queue. */
typedef struct sp_midi_map {
    uint8_t in_channel;  /* 0: all; else 1..16 */
    uint8_t out_channel; /* 1..16 */
    uint8_t clock;       /* SP_CLOCK_* */
    uint64_t notes[128]; /* per note, a bit per pad */
    int16_t out_note[SP_PADS]; /* −1: the pad sends nothing */
} sp_midi_map;

enum {
    SP_CMD_TRIGGER,
    SP_CMD_RELEASE,
    SP_CMD_NOTE_ON,
    SP_CMD_NOTE_OFF,
    SP_CMD_PAD,
    SP_CMD_SAMPLE,
    SP_CMD_PATTERN,
    SP_CMD_TEMPO,
    SP_CMD_SWING,
    SP_CMD_PLAY,
    SP_CMD_STOP,
    SP_CMD_RATE,
    SP_CMD_RESET,
    SP_CMD_COUNT_IN,
    SP_CMD_MIDI,
    SP_CMD_MIDI_GONE,
    SP_CMD_MIDI_MAP,
};

typedef struct {
    uint8_t type, pad, note, vel;
    bool stop;        /* pad: its voices fade and its waiting launches go before the change */
    int32_t quantize; /* trigger: grid in straight ticks to wait for while playing; 0 now */
    int64_t t_ns;     /* when the call was made (CLOCK_MONOTONIC) */
    union {
        sp_pad pad;
        struct {
            int slot;
            sp_sample *sample; /* NULL: unload */
        } sample;
        struct {
            sp_pattern *pattern;
            bool at_bar;
        } pattern;
        double value;
        int rate;
        struct {
            uint32_t id; /* reported with the count-in's event */
            int beats;   /* 0: cancel the count-in */
        } count_in;
        struct {
            uint8_t status, d1, d2; /* one message; t_ns is when it arrived */
            uint8_t src;            /* the input it came from (MIDI_GONE: the input gone) */
        } midi;
        sp_midi_map *map; /* NULL: MIDI off */
    } u;
} sp_cmd;

enum {
    SP_EV_PLAYHEAD,
    SP_EV_VOICE_START,
    SP_EV_VOICE_END,
    SP_EV_HIT,
    SP_EV_PATTERN,
    SP_EV_TRIGGERED,
    SP_EV_COUNT_IN,
};

typedef struct {
    uint8_t type, pad, vel;
    int16_t note;  /* hit: −1 unless a keys pad */
    int32_t a;     /* playhead, hit: step; pattern: id; count-in: id */
    int32_t b;     /* hit: tick (−12..12); count-in: beats; triggered: 1 from MIDI in */
    int64_t frame; /* engine frame; count-in: its first click */
    int64_t t_ns;  /* triggered: when the call was made (MIDI: when the message arrived) */
    int64_t until; /* count-in: the downbeat after its last click */
} sp_event;

enum { SP_GB_SAMPLE = 1, SP_GB_PATTERN = 2, SP_GB_MIDI_MAP = 3 };

typedef struct {
    int kind;
    void *ptr;
} sp_garbage;

enum { SP_V_FREE, SP_V_ON, SP_V_FADING };

typedef struct {
    uint8_t state, pad, mode, choke;
    int16_t note;
    bool reverse, loop, first_pass;
    sp_sample *smp;
    double pos, step;     /* source frames; step per output frame, > 0 */
    int64_t start, end;   /* region */
    float gain;           /* velocity curve × pad gain */
    float fade_in;        /* source frames */
    float fade_out;       /* source frames before the region's end (not looping) */
    float release_frames; /* output frames of a release */
    float rel, rel_step;  /* release ramp while fading */
    double gate_at;       /* sequencer position of an automatic release (a sequenced step's end); < 0 none */
    float level;          /* gain × envelope at the last frame, for stealing */
    uint64_t serial;      /* start order */
    bool from_midi;       /* started by a MIDI note … */
    uint8_t in_src, in_ch, in_note; /* … this one on this channel from this input: its note-off (or CC 120/123,
                                       or the input going) releases it, whatever the table says now */
    uint8_t out_ch;       /* the channel its note-on went out on … */
    int16_t out_note;     /* … and the note; −1: nothing sent (or its note-off is out) */
} sp_voice;

/* A linear stretch of the sequencer's time, for stamping hits. */
typedef struct {
    int64_t frame;
    double m, tpf, origin, swing;
    int32_t length;
    uint32_t play;
} sp_mark;

typedef struct {
    double target; /* absolute position */
    uint8_t pad, vel;
    int16_t note;
    bool note_on;
} sp_pending;

enum { SP_A_STEP, SP_A_FIRE, SP_A_GATE };

typedef struct {
    int off;
    uint8_t kind, pad, vel;
    int16_t note;
    bool note_on;
    double gate_at;  /* step: where the next step starts (gate, loop and keys pads release there) */
    uint8_t voice;   /* gate: the voice to release … */
    uint64_t serial; /* … if it is still the one that was there */
} sp_action;

typedef struct {
    /* Commands: producers serialised by the caller. */
    sp_cmd cmds[SP_CMDS];
    _Atomic uint32_t cmd_head, cmd_tail;
    _Atomic uint64_t cmds_dropped;

    sp_event events[SP_EVENTS];
    _Atomic uint32_t ev_head, ev_tail;
    _Atomic uint64_t events_dropped;

    sp_garbage garbage[SP_GARBAGE];
    _Atomic uint32_t gb_head, gb_tail;

    /* Engine frame `clock_frame` is heard at CLOCK_MONOTONIC `clock_ns`
     * (seqlock; one writer, the event thread). `latency_frames` stands in
     * before the first timestamp. */
    _Atomic uint32_t clock_seq;
    _Atomic int64_t clock_frame, clock_ns;
    _Atomic int64_t latency_frames;

    /* Renderer state. */
    int rate;
    bool offline; /* sp_drain: no audio runs, cuts are immediate */
    int64_t frame;
    int64_t at; /* the frame being processed: a block's first for commands, an action's own */
    sp_pad pads[SP_PADS];
    sp_sample *slots[SP_SLOTS];
    sp_sample *retired;
    sp_voice voices[SP_VOICES];
    uint64_t serial;
    int pad_voices[SP_PADS];

    struct {
        bool playing;
        double bpm, swing, pending_swing; /* pending_swing < 0: none */
        double tpf;                       /* ticks per frame */
        int64_t base_frame;
        double base_m;
        double done; /* positions ≤ done have sounded */
        double origin;
        sp_pattern *pattern, *next;
        sp_pending pending[SP_PENDING];
        int n_pending;
        uint32_t play; /* bumps on every play */
    } seq;

    /* The count-in: `beats` clicks, beat k on frame ceil(origin + k ·
     * interval), accented on the first. */
    struct {
        double origin, interval;
        int beats, next;
        int64_t next_frame;
        int t;          /* frames into the sounding click; < 0 silent */
        double phase, step;
        float gain;
    } click;

    sp_mark marks[SP_MARKS];
    uint32_t n_marks;

    sp_action actions[SP_ACTIONS];
    int n_actions;

    /* MIDI. The renderer's, except the out ring (its consumer is the
     * sending thread) and the counters. */
    struct {
        sp_midi_map *map;              /* NULL: MIDI off */
        mo_event block[SP_MIDI_BLOCK]; /* this block's messages, sorted by frame at its end */
        int n_block;
        mo_ring out;
        double free_next; /* clock send while stopped: the next tick's frame; < 0 from the next block */
        _Atomic uint64_t in_messages;
        _Atomic uint64_t gone;            /* a bit per input gone whose MIDI_GONE the full queue refused */
    } midi;

    /* Counters for info (relaxed). */
    _Atomic int32_t voices_active, voices_peak, step_now;
    _Atomic bool playing_now;
    _Atomic uint64_t steals, hard_cuts, actions_dropped;
} sp_engine;

/* ── Small helpers ────────────────────────────────────────────────────────── */

static float sp_db_to_gain(double db) { return (float)pow(10.0, db / 20.0); }

/* Velocity curve (DLS / General MIDI): amplitude ∝ (v / 127)². */
static float sp_velocity_gain(int vel) {
    float v = (float)vel / 127.0f;
    return v * v;
}

static double sp_warp(double t, double s) {
    double pair = floor(t / SP_PAIR), r = t - pair * SP_PAIR, half = SP_PAIR * s;
    return pair * SP_PAIR + (r < SP_STEP ? r * half / SP_STEP : half + (r - SP_STEP) * (SP_PAIR - half) / SP_STEP);
}

static double sp_unwarp(double x, double s) {
    double pair = floor(x / SP_PAIR), r = x - pair * SP_PAIR, half = SP_PAIR * s;
    return pair * SP_PAIR + (r < half ? r * SP_STEP / half : SP_STEP + (r - half) * SP_STEP / (SP_PAIR - half));
}

static double sp_tpf(double bpm, int rate) { return bpm * SP_PPQN / (60.0 * rate); }

static float sp_soft_clip(float x) {
    float a = fabsf(x);
    if (a <= SP_SOFT_KNEE) return x;
    float y = SP_SOFT_KNEE + (1.0f - SP_SOFT_KNEE) * tanhf((a - SP_SOFT_KNEE) / (1.0f - SP_SOFT_KNEE));
    return x < 0 ? -y : y;
}

/* ── Allocation (control side) ────────────────────────────────────────────── */

/* A sample owning `data` (malloc'd, interleaved), or NULL (and `data`
 * freed) when out of memory. */
static sp_sample *sp_sample_wrap(int16_t *data, int64_t frames, int channels, int rate) {
    sp_sample *s = data ? calloc(1, sizeof(*s)) : NULL;
    if (!s) {
        free(data);
        return NULL;
    }
    *s = (sp_sample){.data = data, .frames = frames, .channels = channels, .rate = rate,
                     .bytes = (size_t)frames * (size_t)channels * sizeof(int16_t)};
    return s;
}

static void sp_sample_free(sp_sample *s) {
    if (!s) return;
    free(s->data);
    free(s);
}

static sp_pattern *sp_pattern_new(int count) {
    sp_pattern *p = calloc(1, sizeof(*p) + sizeof(sp_step) * (size_t)(count > 0 ? count : 1));
    if (p) p->count = count;
    return p;
}

static int sp_step_cmp(const void *a, const void *b) {
    const sp_step *x = a, *y = b;
    if (x->pos != y->pos) return x->pos < y->pos ? -1 : 1;
    return (int)x->pad - (int)y->pad;
}

static void sp_pattern_sort(sp_pattern *p) { qsort(p->steps, (size_t)p->count, sizeof(sp_step), sp_step_cmp); }

static void sp_free_garbage(const sp_garbage *g) {
    if (g->kind == SP_GB_SAMPLE) sp_sample_free(g->ptr);
    else if (g->kind == SP_GB_PATTERN || g->kind == SP_GB_MIDI_MAP) free(g->ptr);
}

/* ── Queues ───────────────────────────────────────────────────────────────── */

/* Producer side; the caller serialises producers. False when full. */
static bool sp_push(sp_engine *e, const sp_cmd *c) {
    uint32_t tail = atomic_load_explicit(&e->cmd_tail, memory_order_relaxed);
    uint32_t head = atomic_load_explicit(&e->cmd_head, memory_order_acquire);
    if (tail - head >= SP_CMDS) {
        atomic_fetch_add_explicit(&e->cmds_dropped, 1, memory_order_relaxed);
        return false;
    }
    e->cmds[tail & (SP_CMDS - 1)] = *c;
    atomic_store_explicit(&e->cmd_tail, tail + 1, memory_order_release);
    return true;
}

static bool sp_pop_cmd(sp_engine *e, sp_cmd *c) {
    uint32_t head = atomic_load_explicit(&e->cmd_head, memory_order_relaxed);
    uint32_t tail = atomic_load_explicit(&e->cmd_tail, memory_order_acquire);
    if (head == tail) return false;
    *c = e->cmds[head & (SP_CMDS - 1)];
    atomic_store_explicit(&e->cmd_head, head + 1, memory_order_release);
    return true;
}

static void sp_emit(sp_engine *e, const sp_event *ev) {
    uint32_t tail = atomic_load_explicit(&e->ev_tail, memory_order_relaxed);
    uint32_t head = atomic_load_explicit(&e->ev_head, memory_order_acquire);
    if (tail - head >= SP_EVENTS) {
        atomic_fetch_add_explicit(&e->events_dropped, 1, memory_order_relaxed);
        return;
    }
    e->events[tail & (SP_EVENTS - 1)] = *ev;
    atomic_store_explicit(&e->ev_tail, tail + 1, memory_order_release);
}

static bool sp_pop_event(sp_engine *e, sp_event *ev) {
    uint32_t head = atomic_load_explicit(&e->ev_head, memory_order_relaxed);
    uint32_t tail = atomic_load_explicit(&e->ev_tail, memory_order_acquire);
    if (head == tail) return false;
    *ev = e->events[head & (SP_EVENTS - 1)];
    atomic_store_explicit(&e->ev_head, head + 1, memory_order_release);
    return true;
}

static bool sp_discard(sp_engine *e, int kind, void *ptr) {
    uint32_t tail = atomic_load_explicit(&e->gb_tail, memory_order_relaxed);
    uint32_t head = atomic_load_explicit(&e->gb_head, memory_order_acquire);
    if (tail - head >= SP_GARBAGE) return false;
    e->garbage[tail & (SP_GARBAGE - 1)] = (sp_garbage){kind, ptr};
    atomic_store_explicit(&e->gb_tail, tail + 1, memory_order_release);
    return true;
}

static bool sp_pop_garbage(sp_engine *e, sp_garbage *g) {
    uint32_t head = atomic_load_explicit(&e->gb_head, memory_order_relaxed);
    uint32_t tail = atomic_load_explicit(&e->gb_tail, memory_order_acquire);
    if (head == tail) return false;
    *g = e->garbage[head & (SP_GARBAGE - 1)];
    atomic_store_explicit(&e->gb_head, head + 1, memory_order_release);
    return true;
}

/* A pattern the renderer no longer needs. The garbage queue holds 512 and
 * the event thread empties it every few ms; if it is ever full the pattern
 * leaks rather than the renderer blocking. */
static void sp_drop_pattern(sp_engine *e, sp_pattern *p) {
    if (p) sp_discard(e, SP_GB_PATTERN, p);
}

/* ── The clock (event thread writes, renderer and NIFs read) ──────────────── */

static void sp_set_clock(sp_engine *e, int64_t frame, int64_t ns) {
    uint32_t s = atomic_load_explicit(&e->clock_seq, memory_order_relaxed);
    atomic_store_explicit(&e->clock_seq, s + 1, memory_order_relaxed);
    atomic_thread_fence(memory_order_release);
    atomic_store_explicit(&e->clock_frame, frame, memory_order_relaxed);
    atomic_store_explicit(&e->clock_ns, ns, memory_order_relaxed);
    atomic_store_explicit(&e->clock_seq, s + 2, memory_order_release);
}

/* False before the first timestamp (or, rarely, mid-write: the renderer
 * doesn't spin). */
static bool sp_get_clock(sp_engine *e, int64_t *frame, int64_t *ns) {
    for (int tries = 0; tries < 4; tries++) {
        uint32_t a = atomic_load_explicit(&e->clock_seq, memory_order_acquire);
        int64_t f = atomic_load_explicit(&e->clock_frame, memory_order_relaxed);
        int64_t n = atomic_load_explicit(&e->clock_ns, memory_order_relaxed);
        atomic_thread_fence(memory_order_acquire);
        uint32_t b = atomic_load_explicit(&e->clock_seq, memory_order_relaxed);
        if (!(a & 1) && a == b) {
            if (a == 0) return false;
            *frame = f;
            *ns = n;
            return true;
        }
    }
    return false;
}

/* ── Init ─────────────────────────────────────────────────────────────────── */

static void sp_init(sp_engine *e, int rate) {
    memset(e, 0, sizeof(*e));
    e->rate = rate;
    for (int i = 0; i < SP_PADS; i++) e->pads[i].slot = -1;
    e->seq.bpm = 120.0;
    e->seq.swing = 0.5;
    e->seq.pending_swing = -1;
    e->seq.tpf = sp_tpf(120.0, rate);
    e->click.t = -1;
    atomic_store(&e->step_now, -1);
    e->midi.free_next = -1;
}

/* ── MIDI out (renderer) ──────────────────────────────────────────────────── */

static bool midi_is_note_off(const mo_event *ev) { return ev->kind == MO_NOTE && (ev->b[0] & 0xF0) == 0x80; }

/* A message for engine frame `frame` into this block's list (sorted and
 * queued at the block's end). A note-off always gets in: the list keeps
 * room for one per voice, so every note-on that went out gets its
 * note-off. While no audio runs (sp_drain) only note-offs go in, and wait
 * there for the next block (they go out late, not never); anything else
 * would be stamped in the past. */
static bool midi_push(sp_engine *e, int64_t frame, uint8_t kind, uint8_t len, uint8_t b0, uint8_t b1, uint8_t b2) {
    mo_event ev = {.frame = frame, .len = len, .kind = kind, .b = {b0, b1, b2}};
    bool off = midi_is_note_off(&ev);
    if (e->offline && !off) return false;
    if (e->midi.n_block >= (off ? SP_MIDI_BLOCK : SP_MIDI_BLOCK - SP_VOICES)) {
        atomic_fetch_add_explicit(&e->midi.out.dropped, 1, memory_order_relaxed);
        return false;
    }
    e->midi.block[e->midi.n_block++] = ev;
    return true;
}

/* The voice stops sounding at full (its release starts, it is cut, or it
 * ends): the note-off of its note-on. */
static void voice_note_off(sp_engine *e, sp_voice *v, int64_t frame) {
    if (v->state != SP_V_ON || v->out_note < 0) return;
    midi_push(e, frame, MO_NOTE, 3, (uint8_t)(0x80 | v->out_ch), (uint8_t)v->out_note, 64);
    v->out_note = -1;
}

/* The note-on of voice `v` of `pad` starting at `frame`: the pad's note,
 * or for a keys pad the note it plays (`played`). Another voice still
 * holding that note on that channel sends its note-off first (receivers
 * keep one note per channel and note: its later note-off would end this
 * one). Returns the note queued, −1 when nothing was (if the ring refuses
 * it at the block's end, midi_unsent takes it back from the voice). */
static int16_t midi_note_on(sp_engine *e, sp_voice *v, int pad, int played, int vel, int64_t frame) {
    const sp_midi_map *m = e->midi.map;
    if (!m || e->offline || m->out_note[pad] < 0) return -1;
    int note = played >= 0 ? played : m->out_note[pad];
    uint8_t ch = (uint8_t)((m->out_channel - 1) & 0x0F);
    for (int i = 0; i < SP_VOICES; i++) {
        sp_voice *w = &e->voices[i];
        if (w != v && w->state == SP_V_ON && w->out_note == note && w->out_ch == ch) voice_note_off(e, w, frame);
    }
    if (!midi_push(e, frame, MO_NOTE, 3, (uint8_t)(0x90 | ch), (uint8_t)note, (uint8_t)vel)) return -1;
    v->out_ch = ch;
    return (int16_t)note;
}

/* ── Voices ───────────────────────────────────────────────────────────────── */

static void voice_finish(sp_engine *e, sp_voice *v, int64_t frame) {
    if (v->state == SP_V_FREE) return;
    voice_note_off(e, v, frame);
    v->state = SP_V_FREE;
    v->smp->refs--;
    v->smp = NULL;
    if (--e->pad_voices[v->pad] == 0)
        sp_emit(e, &(sp_event){.type = SP_EV_VOICE_END, .pad = v->pad, .frame = frame});
}

/* Fade to silence over `frames` output frames from where the envelope is
 * now; a voice already fading only ever fades faster. Offline (no audio
 * runs) a fade would never finish: cut at once. */
static void voice_release(sp_engine *e, sp_voice *v, double frames) {
    if (v->state == SP_V_FREE) return;
    voice_note_off(e, v, e->at);
    if (e->offline) {
        voice_finish(e, v, e->frame);
        return;
    }
    float step = v->rel / (float)(frames > 1 ? frames : 1);
    if (v->state == SP_V_ON) {
        v->state = SP_V_FADING;
        v->rel_step = step;
    } else if (step > v->rel_step) {
        v->rel_step = step;
    }
    v->gate_at = -1;
}

static double cut_frames(const sp_engine *e) { return SP_CUT_MS * e->rate / 1000.0; }

static int count_on(const sp_engine *e) {
    int n = 0;
    for (int i = 0; i < SP_VOICES; i++) n += e->voices[i].state == SP_V_ON;
    return n;
}

/* The voice to give up: the quietest, then the oldest. */
static sp_voice *pick_victim(sp_engine *e, int state) {
    sp_voice *best = NULL;
    for (int i = 0; i < SP_VOICES; i++) {
        sp_voice *v = &e->voices[i];
        if (v->state != state) continue;
        if (!best || v->level < best->level - 1e-6f ||
            (fabsf(v->level - best->level) <= 1e-6f && v->serial < best->serial))
            best = v;
    }
    return best;
}

static bool pad_sounding(const sp_engine *e, int pad) {
    for (int i = 0; i < SP_VOICES; i++)
        if (e->voices[i].state == SP_V_ON && e->voices[i].pad == pad) return true;
    return false;
}

static void release_pad_voices(sp_engine *e, int pad, int note) {
    for (int i = 0; i < SP_VOICES; i++) {
        sp_voice *v = &e->voices[i];
        if (v->state != SP_V_ON || v->pad != pad || (note >= 0 && v->note != note)) continue;
        voice_release(e, v, v->release_frames);
    }
}

/* Start pad `pad` at `frame`. `note` is the keys-mode note (−1: the root);
 * `gate_at` the sequencer position of an automatic release (< 0: none);
 * `src` SP_SRC_PLAYED (sends MIDI out) or SP_SRC_MIDI. Returns false when
 * the pad has nothing to play. */
static bool voice_start(sp_engine *e, int pad, int vel, int note, double gate_at, int64_t frame, uint8_t src) {
    const sp_pad *pd = &e->pads[pad];
    if (pd->slot < 0) return false;
    e->at = frame;
    sp_sample *s = e->slots[pd->slot];
    if (!s || s->frames == 0) return false;
    int64_t start = pd->start < 0 ? 0 : pd->start;
    int64_t end = pd->end <= 0 || pd->end > s->frames ? s->frames : pd->end;
    if (start >= end) return false;

    if (pd->mode == SP_TOGGLE && pad_sounding(e, pad)) {
        release_pad_voices(e, pad, -1);
        return true;
    }

    if (pd->choke > 0) {
        for (int i = 0; i < SP_VOICES; i++) {
            sp_voice *v = &e->voices[i];
            if (v->state != SP_V_FREE && v->pad != pad && v->choke == pd->choke) voice_release(e, v, cut_frames(e));
        }
    }

    if (count_on(e) >= SP_POLY) {
        sp_voice *victim = pick_victim(e, SP_V_ON);
        if (victim) {
            voice_release(e, victim, cut_frames(e));
            atomic_fetch_add_explicit(&e->steals, 1, memory_order_relaxed);
        }
    }
    sp_voice *v = NULL;
    for (int i = 0; i < SP_VOICES && !v; i++)
        if (e->voices[i].state == SP_V_FREE) v = &e->voices[i];
    if (!v) {
        /* Every slot holds a sounding or fading voice: cut the quietest
         * fading one (it is on its way out already). */
        v = pick_victim(e, SP_V_FADING);
        if (!v) return false;
        voice_finish(e, v, frame);
        atomic_fetch_add_explicit(&e->hard_cuts, 1, memory_order_relaxed);
    }

    int root = pd->root;
    int played = pd->mode == SP_KEYS ? (note >= 0 ? note : root) : -1;
    double semis = pd->pitch + (played >= 0 ? played - root : 0);
    double step = pow(2.0, semis / 12.0) * (double)s->rate / (double)e->rate;
    if (step < 1.0 / 64) step = 1.0 / 64;
    if (step > 64) step = 64;
    double src_ms = s->rate / 1000.0;

    *v = (sp_voice){
        .state = SP_V_ON,
        .pad = (uint8_t)pad,
        .mode = pd->mode,
        .choke = pd->choke,
        .note = (int16_t)played,
        .reverse = pd->reverse,
        .loop = pd->mode == SP_LOOP || pd->mode == SP_TOGGLE,
        .first_pass = true,
        .smp = s,
        .pos = pd->reverse ? (double)(end - 1) : (double)start,
        .step = step,
        .start = start,
        .end = end,
        .gain = sp_velocity_gain(vel) * pd->gain,
        .fade_in = (float)(fmax(pd->fade_in_ms, SP_DECLICK_IN_MS) * src_ms),
        .fade_out = (float)(fmax(pd->fade_out_ms, SP_DECLICK_OUT_MS) * src_ms),
        .release_frames = (float)(fmax(pd->fade_out_ms, SP_MIN_RELEASE_MS) * e->rate / 1000.0),
        .rel = 1.0f,
        .gate_at = gate_at,
        .level = sp_velocity_gain(vel) * pd->gain,
        .serial = ++e->serial,
        .from_midi = src == SP_SRC_MIDI,
    };
    v->out_note = src == SP_SRC_PLAYED ? midi_note_on(e, v, pad, played, vel, frame) : -1;
    s->refs++;
    e->pad_voices[pad]++;
    sp_emit(e, &(sp_event){.type = SP_EV_VOICE_START, .pad = (uint8_t)pad, .frame = frame});
    return true;
}

/* A tap the fast path can't take: wrapped into a looping region, clamped
 * to the sample otherwise. */
static int64_t tap_index(int64_t j, bool loop, int64_t lo, int64_t hi, int64_t len) {
    if (!loop) return j < lo ? lo : j > hi ? hi : j;
    while (j < lo) j += len;
    while (j > hi) j -= len;
    return j;
}

/* 4-point, 3rd-order Hermite. */
static float hermite(float xm1, float x0, float x1, float x2, float t) {
    float c1 = 0.5f * (x1 - xm1);
    float c2 = xm1 - 2.5f * x0 + 2.0f * x1 - 0.5f * x2;
    float c3 = 0.5f * (x2 - xm1) + 1.5f * (x0 - x1);
    return ((c3 * t + c2) * t + c1) * t + x0;
}

/* Mix `n` frames of voice `v` into `out` (interleaved stereo), the first
 * being engine frame `frame`. The hot loop: no division or modulo, the four
 * taps straight from the sample unless they straddle an edge, and the
 * voice's state in locals (`out` could alias its float fields otherwise,
 * forcing a reload per frame). */
static void voice_render(sp_engine *e, sp_voice *v, float *out, int n, int64_t frame) {
    const int16_t *data = v->smp->data;
    const int ch = v->smp->channels;
    const bool loop = v->loop, reverse = v->reverse;
    const int64_t len = v->end - v->start;
    const int64_t lo = loop ? v->start : 0, hi = loop ? v->end - 1 : v->smp->frames - 1;
    const double start = (double)v->start, end = (double)v->end, step = v->step;
    const double fade_in = v->fade_in, fade_out = v->fade_out;
    const float gain = v->gain;
    double pos = v->pos;
    float rel = v->rel, rel_step = v->rel_step, level = v->level;
    const bool fading = v->state == SP_V_FADING;
    bool first = v->first_pass;

    for (int i = 0; i < n; i++) {
        if (fading) {
            rel -= rel_step;
            if (rel <= 0.0f) {
                v->pos = pos;
                v->rel = 0.0f;
                voice_finish(e, v, frame + i);
                return;
            }
        }

        float env = 1.0f;
        if (first) {
            double in = (reverse ? end - 1.0 - pos : pos - start) + step;
            if (in < fade_in) env = (float)(in / fade_in);
        }
        if (!loop) {
            double remaining = reverse ? pos - start + 1.0 : end - pos;
            if (remaining < fade_out) env *= (float)(remaining / fade_out);
        }
        level = gain * env * rel;
        float amp = level * (1.0f / 32768.0f);

        int64_t i0 = (int64_t)pos; /* pos ≥ 0, so this is floor */
        float t = (float)(pos - (double)i0);
        const int16_t *a, *b, *c, *d;
        if (i0 - 1 >= lo && i0 + 2 <= hi) {
            b = data + i0 * ch;
            a = b - ch;
            c = b + ch;
            d = c + ch;
        } else {
            a = data + tap_index(i0 - 1, loop, lo, hi, len) * ch;
            b = data + tap_index(i0, loop, lo, hi, len) * ch;
            c = data + tap_index(i0 + 1, loop, lo, hi, len) * ch;
            d = data + tap_index(i0 + 2, loop, lo, hi, len) * ch;
        }
        if (ch == 1) {
            float x = amp * hermite(a[0], b[0], c[0], d[0], t);
            out[2 * i] += x;
            out[2 * i + 1] += x;
        } else {
            out[2 * i] += amp * hermite(a[0], b[0], c[0], d[0], t);
            out[2 * i + 1] += amp * hermite(a[1], b[1], c[1], d[1], t);
        }

        bool ended = false;
        if (reverse) {
            pos -= step;
            if (pos < start) {
                ended = !loop;
                if (loop) {
                    pos = end - fmod(start - pos, (double)len);
                    if (pos >= end) pos -= (double)len;
                    first = false;
                }
            }
        } else {
            pos += step;
            if (pos >= end) {
                ended = !loop;
                if (loop) {
                    pos = start + fmod(pos - start, (double)len);
                    first = false;
                }
            }
        }
        if (ended) {
            v->pos = pos;
            voice_finish(e, v, frame + i + 1);
            return;
        }
    }
    v->pos = pos;
    v->rel = rel;
    v->level = level;
    v->first_pass = first;
}

static void mix(sp_engine *e, float *out, int from, int to) {
    if (to <= from) return;
    for (int i = 0; i < SP_VOICES; i++)
        if (e->voices[i].state != SP_V_FREE) voice_render(e, &e->voices[i], out + 2 * from, to - from, e->frame + from);
}

/* ── Sequencer ────────────────────────────────────────────────────────────── */

static int32_t seq_length(const sp_engine *e) { return e->seq.pattern ? e->seq.pattern->length : SP_DEFAULT_LENGTH; }

static double m_at(const sp_engine *e, int64_t frame) {
    return e->seq.base_m + (double)(frame - e->seq.base_frame) * e->seq.tpf;
}

/* The frame position `x` sounds on. */
static int64_t frame_of(const sp_engine *e, double x) {
    return e->seq.base_frame + (int64_t)ceil((x - e->seq.base_m) / e->seq.tpf - SP_FRAME_TOL);
}

static void push_mark(sp_engine *e, int64_t frame) {
    sp_mark *k = &e->marks[e->n_marks++ & (SP_MARKS - 1)];
    *k = (sp_mark){
        .frame = frame,
        .m = m_at(e, frame),
        .tpf = e->seq.tpf,
        .origin = e->seq.origin,
        .swing = e->seq.swing,
        .length = seq_length(e),
        .play = e->seq.play,
    };
}

static void add_action(sp_engine *e, sp_action a) {
    if (e->n_actions >= SP_ACTIONS) {
        atomic_fetch_add_explicit(&e->actions_dropped, 1, memory_order_relaxed);
        return;
    }
    e->actions[e->n_actions++] = a;
}

static int clamp_off(int64_t off, int frames) { return off < 0 ? 0 : off >= frames ? frames - 1 : (int)off; }

/* The queued pattern becomes the current one, its tick 0 at `origin`. */
static void swap_pattern(sp_engine *e, sp_pattern *p, double origin, int64_t frame) {
    sp_drop_pattern(e, e->seq.pattern);
    e->seq.pattern = p;
    e->seq.origin = origin;
    if (p) sp_emit(e, &(sp_event){.type = SP_EV_PATTERN, .a = (int32_t)p->id, .frame = frame});
}

/* Steps, playhead and quantized triggers at positions in (a, b]. */
static void scan(sp_engine *e, double a, double b, int frames) {
    if (b <= a) return;
    const int64_t F = e->frame;
    const sp_pattern *p = e->seq.pattern;
    const int32_t L = seq_length(e);
    const double s = e->seq.swing, o = e->seq.origin;
    long k0 = (long)floor((a - o) / L), k1 = (long)floor((b - o) / L);
    for (long k = k0; k <= k1; k++) {
        double base = o + (double)k * L, la = a - base, lb = b - base;

        int steps = L / SP_STEP;
        int first = (int)floor(sp_unwarp(la > 0 ? la : 0, s) / SP_STEP) - 1;
        for (int st = first < 0 ? 0 : first; st < steps; st++) {
            double x = sp_warp((double)st * SP_STEP, s);
            if (x > lb) break;
            if (x <= la) continue;
            int64_t f = F + clamp_off(frame_of(e, base + x) - F, frames);
            atomic_store_explicit(&e->step_now, st, memory_order_relaxed);
            sp_emit(e, &(sp_event){.type = SP_EV_PLAYHEAD, .a = st, .frame = f});
        }

        if (!p || p->count == 0) continue;
        int lo = 0, hi = p->count; /* first step with warp(pos) > la */
        while (lo < hi) {
            int mid = (lo + hi) / 2;
            if (sp_warp(p->steps[mid].pos, s) > la) hi = mid;
            else lo = mid + 1;
        }
        for (int i = lo; i < p->count; i++) {
            double x = sp_warp(p->steps[i].pos, s);
            if (x > lb) break;
            add_action(e, (sp_action){
                              .off = clamp_off(frame_of(e, base + x) - F, frames),
                              .kind = SP_A_STEP,
                              .pad = p->steps[i].pad,
                              .vel = p->steps[i].vel,
                              .note = p->steps[i].note,
                              .gate_at = base + sp_warp(p->steps[i].pos + SP_STEP, s),
                          });
        }
    }

    for (int i = 0; i < e->seq.n_pending;) {
        sp_pending *q = &e->seq.pending[i];
        if (q->target > a && q->target <= b) {
            add_action(e, (sp_action){
                              .off = clamp_off(frame_of(e, q->target) - F, frames),
                              .kind = SP_A_FIRE,
                              .pad = q->pad,
                              .vel = q->vel,
                              .note = q->note,
                              .note_on = q->note_on,
                          });
            e->seq.pending[i] = e->seq.pending[--e->seq.n_pending];
        } else {
            i++;
        }
    }
}

/* The next change point after `x`: the next bar if a pattern waits for
 * one, the next 8th-note pair if a swing change waits (swing doesn't move
 * pair boundaries, so nothing sounds twice or not at all). */
static double next_change(const sp_engine *e, double x) {
    double cp = INFINITY;
    if (e->seq.next) cp = (floor(x / SP_BAR) + 1) * SP_BAR;
    if (e->seq.pending_swing >= 0) {
        double pair = (floor(x / SP_PAIR) + 1) * SP_PAIR;
        if (pair < cp) cp = pair;
    }
    return cp;
}

/* Actions run by frame offset; at the same frame a gate's release comes
 * before any start, so an ending step frees its voice before the next one
 * needs a slot (no needless steal at full polyphony). */
static bool action_before(const sp_action *a, const sp_action *b) {
    if (a->off != b->off) return a->off < b->off;
    return a->kind == SP_A_GATE && b->kind != SP_A_GATE;
}

static void sort_actions(sp_engine *e) {
    for (int i = 1; i < e->n_actions; i++) {
        sp_action a = e->actions[i];
        int j = i - 1;
        while (j >= 0 && action_before(&a, &e->actions[j])) {
            e->actions[j + 1] = e->actions[j];
            j--;
        }
        e->actions[j + 1] = a;
    }
}

/* Insert `a` after index `after`, keeping the order (the render loop is at
 * `after`; `a` is never due before it). */
static void insert_action(sp_engine *e, int after, sp_action a) {
    if (e->n_actions >= SP_ACTIONS) {
        atomic_fetch_add_explicit(&e->actions_dropped, 1, memory_order_relaxed);
        return;
    }
    int j = e->n_actions++;
    while (j > after + 1 && action_before(&a, &e->actions[j - 1])) {
        e->actions[j] = e->actions[j - 1];
        j--;
    }
    e->actions[j] = a;
}

/* The sequencer's work for this block, as actions at frame offsets. */
static void sequence(sp_engine *e, int frames) {
    e->n_actions = 0;
    if (!e->seq.playing) return;
    push_mark(e, e->frame);
    double tol = SP_FRAME_TOL * e->seq.tpf;
    double x0 = e->seq.done, x1 = m_at(e, e->frame + frames - 1) + tol;
    const double from = x0;
    for (;;) {
        double cp = next_change(e, x0);
        if (cp > x1) {
            scan(e, x0, x1, frames);
            break;
        }
        scan(e, x0, cp - SP_SEG_EPS, frames);
        int64_t f = e->frame + clamp_off(frame_of(e, cp) - e->frame, frames);
        if (e->seq.next && fmod(cp, SP_BAR) == 0) {
            sp_pattern *p = e->seq.next;
            e->seq.next = NULL;
            swap_pattern(e, p, cp, f);
        }
        if (e->seq.pending_swing >= 0 && fmod(cp, SP_PAIR) == 0) {
            e->seq.swing = e->seq.pending_swing;
            e->seq.pending_swing = -1;
        }
        push_mark(e, f);
        x0 = cp - SP_SEG_EPS;
    }
    e->seq.done = x1;

    /* MIDI clock out: a tick on every 4th position (24 PPQN, straight). */
    if (e->midi.map && e->midi.map->clock == SP_CLOCK_SEND)
        for (double t = (floor(from / 4) + 1) * 4; t <= x1; t += 4)
            midi_push(e, e->frame + clamp_off(frame_of(e, t) - e->frame, frames), MO_CLOCK, 1, 0xF8, 0, 0);

    /* Sequenced gate, loop and keys voices end where the next step starts
     * (swing and tempo changes included). Voices this block's steps start
     * get theirs as they start (sp_render). */
    for (int i = 0; i < SP_VOICES; i++) {
        const sp_voice *v = &e->voices[i];
        if (v->state == SP_V_ON && v->gate_at >= 0 && v->gate_at <= x1)
            add_action(e, (sp_action){
                              .off = clamp_off(frame_of(e, v->gate_at) - e->frame, frames),
                              .kind = SP_A_GATE,
                              .voice = (uint8_t)i,
                              .serial = v->serial,
                          });
    }
    sort_actions(e);
}

/* Where the sequencer was (step, tick from that step's swung start) when
 * engine frame `heard` was heard. */
static void stamp(const sp_engine *e, double heard, int *step, int *tick) {
    *step = 0;
    *tick = 0;
    const sp_mark *k = NULL;
    uint32_t n = e->n_marks, oldest = n > SP_MARKS ? n - SP_MARKS : 0;
    for (uint32_t i = n; i > oldest; i--) {
        const sp_mark *c = &e->marks[(i - 1) & (SP_MARKS - 1)];
        if (c->play != e->seq.play) break;
        if ((double)c->frame <= heard) {
            k = c;
            break;
        }
    }
    if (!k) return; /* before this play started: its first step */
    double m = k->m + (heard - (double)k->frame) * k->tpf;
    double local = fmod(m - k->origin, k->length);
    if (local < 0) local += k->length;
    double u = sp_unwarp(local, k->swing);
    int st = (int)floor(u / SP_STEP + 0.5);
    int t = (int)lround(u - (double)st * SP_STEP);
    if (t > SP_STEP / 2) t = SP_STEP / 2;
    if (t < -SP_STEP / 2) t = -SP_STEP / 2;
    *step = st % (k->length / SP_STEP);
    *tick = t;
}

/* A live hit while playing, stamped where the player heard it: the frame
 * being heard when the call was made (or the MIDI message arrived), else
 * a buffer back. */
static void report_hit(sp_engine *e, int pad, int vel, int note, int64_t t_ns) {
    if (e->pads[pad].slot < 0) return;
    double heard;
    int64_t cf, cns;
    if (sp_get_clock(e, &cf, &cns)) heard = (double)cf + (double)(t_ns - cns) * e->rate / 1e9;
    else heard = (double)(e->frame - atomic_load_explicit(&e->latency_frames, memory_order_relaxed));
    if (heard > (double)e->frame) heard = (double)e->frame;
    int step, tick;
    stamp(e, heard, &step, &tick);
    sp_emit(e, &(sp_event){
                   .type = SP_EV_HIT,
                   .pad = (uint8_t)pad,
                   .vel = (uint8_t)vel,
                   .note = e->pads[pad].mode == SP_KEYS ? (int16_t)note : -1,
                   .a = step,
                   .b = tick,
                   .frame = e->frame,
               });
}

/* The position of the next point of a `grid`-tick straight grid at or
 * after position `m` (swung like the steps). */
static double grid_target(const sp_engine *e, double m, int grid) {
    int32_t L = seq_length(e);
    double rel = m - e->seq.origin;
    double cycle = floor(rel / L) * L;
    double u = sp_unwarp(rel - cycle, e->seq.swing);
    double g = ceil(u / grid - 1e-9) * grid;
    return e->seq.origin + cycle + sp_warp(g, e->seq.swing);
}

/* ── Commands ─────────────────────────────────────────────────────────────── */

static void retire_sample(sp_engine *e, sp_sample *s) {
    if (!s) return;
    for (int i = 0; i < SP_VOICES; i++)
        if (e->voices[i].state != SP_V_FREE && e->voices[i].smp == s) voice_release(e, &e->voices[i], cut_frames(e));
    s->next_retired = e->retired;
    e->retired = s;
}

/* Retired samples no voice plays any more go to the garbage queue. */
static void reclaim(sp_engine *e) {
    sp_sample **link = &e->retired;
    while (*link) {
        sp_sample *s = *link;
        if (s->refs == 0 && sp_discard(e, SP_GB_SAMPLE, s)) *link = s->next_retired;
        else link = &s->next_retired;
    }
}

static void rebase(sp_engine *e) {
    if (e->seq.playing) {
        e->seq.base_m = m_at(e, e->frame);
        e->seq.base_frame = e->frame;
    }
    e->seq.tpf = sp_tpf(e->seq.bpm, e->rate);
}

/* Stopping ends what the sequencer was holding: gated voices fade out
 * (one-shots ring on). Sending clock: Stop goes out, and the clock runs
 * on from the next tick position. */
static void stop_sequencer(sp_engine *e) {
    if (e->seq.playing && e->midi.map && e->midi.map->clock == SP_CLOCK_SEND) {
        midi_push(e, e->at, MO_TRANSPORT, 1, 0xFC, 0, 0);
        double next = (floor(m_at(e, e->at) / 4) + 1) * 4;
        e->midi.free_next = (double)e->seq.base_frame + (next - e->seq.base_m) / e->seq.tpf;
    }
    e->seq.playing = false;
    e->seq.n_pending = 0;
    e->seq.done = 0;
    for (int i = 0; i < SP_VOICES; i++) {
        sp_voice *v = &e->voices[i];
        if (v->state == SP_V_ON && v->gate_at >= 0) voice_release(e, v, v->release_frames);
    }
    if (e->seq.next) {
        sp_pattern *p = e->seq.next;
        e->seq.next = NULL;
        swap_pattern(e, p, 0, e->frame);
    }
    if (e->seq.pending_swing >= 0) e->seq.swing = e->seq.pending_swing;
    e->seq.pending_swing = -1;
    atomic_store_explicit(&e->step_now, -1, memory_order_relaxed);
}

static void live_trigger(sp_engine *e, const sp_cmd *c, bool note_on) {
    int note = note_on ? c->note : -1;
    if (e->seq.playing) report_hit(e, c->pad, c->vel, note >= 0 ? note : e->pads[c->pad].root, c->t_ns);
    if (c->quantize > 0 && e->seq.playing && e->seq.n_pending < SP_PENDING) {
        e->seq.pending[e->seq.n_pending++] = (sp_pending){
            .target = grid_target(e, m_at(e, e->frame), c->quantize),
            .pad = c->pad,
            .vel = c->vel,
            .note = (int16_t)note,
            .note_on = note_on,
        };
        return;
    }
    if (voice_start(e, c->pad, c->vel, note, -1, e->frame, SP_SRC_PLAYED))
        sp_emit(e, &(sp_event){.type = SP_EV_TRIGGERED, .pad = c->pad, .frame = e->frame, .t_ns = c->t_ns});
}

static void cancel_pending(sp_engine *e, int pad) {
    for (int i = 0; i < e->seq.n_pending;) {
        if (e->seq.pending[i].pad == pad) e->seq.pending[i] = e->seq.pending[--e->seq.n_pending];
        else i++;
    }
}

/* The sequencer started at e->at (a play call): sending clock, song
 * position 0 and Start go out on that frame, before its first tick. */
static void seq_started(sp_engine *e) {
    if (e->midi.map && e->midi.map->clock == SP_CLOCK_SEND) {
        midi_push(e, e->at, MO_TRANSPORT, 3, 0xF2, 0, 0);
        midi_push(e, e->at, MO_TRANSPORT, 1, 0xFA, 0, 0);
    }
    e->midi.free_next = -1;
}

/* ── MIDI in (renderer) ───────────────────────────────────────────────────── */

/* Note-on (or off) `c->u.midi.d1`: every pad the kit's table lists for it.
 * A keys pad plays the note chromatically, any other pad triggers; off
 * releases the played (touch, sequencer) voices of gate, loop and keys pads
 * (keys: that note's voice); MIDI's own go by midi_in_release. Voices
 * started here send no MIDI out and remember the input, channel and note
 * that started them. */
static void midi_note(sp_engine *e, const sp_cmd *c, bool on) {
    int note = c->u.midi.d1 & 0x7F, vel = c->u.midi.d2 & 0x7F;
    uint64_t pads = e->midi.map->notes[note];
    while (pads) {
        int pad = __builtin_ctzll(pads);
        pads &= pads - 1;
        uint8_t mode = e->pads[pad].mode;
        bool keys = mode == SP_KEYS;
        if (on) {
            if (e->seq.playing) report_hit(e, pad, vel, keys ? note : e->pads[pad].root, c->t_ns);
            uint64_t before = e->serial;
            if (voice_start(e, pad, vel, keys ? note : -1, -1, e->frame, SP_SRC_MIDI))
                sp_emit(e, &(sp_event){.type = SP_EV_TRIGGERED, .pad = (uint8_t)pad, .b = 1, .frame = e->frame,
                                       .t_ns = c->t_ns});
            for (int i = 0; e->serial != before && i < SP_VOICES; i++) {
                sp_voice *v = &e->voices[i];
                if (v->state != SP_V_ON || v->serial != e->serial) continue;
                v->in_src = c->u.midi.src;
                v->in_ch = c->u.midi.status & 0x0F;
                v->in_note = (uint8_t)note;
                break;
            }
        } else if (mode == SP_GATE || mode == SP_LOOP || keys) {
            for (int i = 0; i < SP_VOICES; i++) {
                sp_voice *v = &e->voices[i];
                if (v->state != SP_V_ON || v->from_midi || v->pad != pad || (keys && v->note != note)) continue;
                voice_release(e, v, v->release_frames);
            }
        }
    }
}

/* Voices MIDI started from input `src` on channel `ch` (< 0: any) with
 * `note` (< 0: any): gate, loop and keys voices fade as on their release;
 * with `sound` every one is cut fast. */
static void midi_release(sp_engine *e, int src, int ch, int note, bool sound) {
    for (int i = 0; i < SP_VOICES; i++) {
        sp_voice *v = &e->voices[i];
        if (v->state != SP_V_ON || !v->from_midi || v->in_src != src) continue;
        if ((ch >= 0 && v->in_ch != ch) || (note >= 0 && v->in_note != note)) continue;
        if (sound) voice_release(e, v, cut_frames(e));
        else if (v->mode == SP_GATE || v->mode == SP_LOOP || v->mode == SP_KEYS) voice_release(e, v, v->release_frames);
    }
}

/* What in an incoming message releases voices MIDI started, by the input,
 * channel and note that started them: not through the table or its channel
 * filter (either may have changed since), and with no stream running too.
 * A note-off: that note's; CC 123 (all notes off): the channel's; CC 120
 * (all sound off): the channel's, fast. */
static void midi_in_release(sp_engine *e, const sp_cmd *c) {
    uint8_t st = c->u.midi.status, kind = st & 0xF0, d1 = c->u.midi.d1 & 0x7F;
    if (st >= 0xF0) return;
    if (kind == 0x80 || (kind == 0x90 && c->u.midi.d2 == 0)) midi_release(e, c->u.midi.src, st & 0x0F, d1, false);
    else if (kind == 0xB0 && (d1 == 120 || d1 == 123)) midi_release(e, c->u.midi.src, st & 0x0F, -1, d1 == 120);
}

/* Input `src` closed (unplugged, its port gone): the voices it started end
 * as on their note-offs. */
static void midi_input_gone(sp_engine *e, int src) { midi_release(e, src, -1, -1, false); }

/* The inputs gone (midi.gone) whose SP_CMD_MIDI_GONE the full command
 * queue refused. Taken before a render's or drain's commands and applied
 * after them: everything an input sent before its bit was set has been
 * applied by then, so a note-on still queued can't outlive the release
 * (one sent after it, by the input's next port, may end early). */
static uint64_t midi_gone_take(sp_engine *e) {
    if (!atomic_load_explicit(&e->midi.gone, memory_order_relaxed)) return 0;
    return atomic_exchange_explicit(&e->midi.gone, 0, memory_order_acq_rel);
}

static void midi_gone_apply(sp_engine *e, uint64_t gone) {
    while (gone) {
        int src = __builtin_ctzll(gone);
        gone &= gone - 1;
        midi_input_gone(e, src);
    }
}

/* One incoming message: first what releases MIDI voices (midi_in_release),
 * then through the kit's table: notes on its channel. Clock, transport and
 * other system messages do nothing (no clock follow yet). */
static void midi_in(sp_engine *e, const sp_cmd *c) {
    const sp_midi_map *m = e->midi.map;
    uint8_t st = c->u.midi.status;
    midi_in_release(e, c);
    if (!m || st >= 0xF0) return;
    if (m->in_channel && (st & 0x0F) != m->in_channel - 1) return;
    switch (st & 0xF0) {
    case 0x90:
        midi_note(e, c, c->u.midi.d2 > 0);
        break;
    case 0x80:
        midi_note(e, c, false);
        break;
    }
}

/* A new table (NULL: MIDI off). Leaving send forgets the free-running
 * clock. */
static void set_midi_map(sp_engine *e, sp_midi_map *map) {
    sp_midi_map *old = e->midi.map;
    e->midi.map = map;
    if (old) sp_discard(e, SP_GB_MIDI_MAP, old);
    if (!map || map->clock != SP_CLOCK_SEND) e->midi.free_next = -1;
}

/* The note-on at `i` in this block's list didn't get into the ring: it was
 * never sent, so neither is its note-off. That is the first for its channel
 * and note later in the list (any other voice's went before the note-on),
 * dropped (len 0); else the voice holding the note has none open. */
static void midi_unsent(sp_engine *e, int i) {
    mo_event *b = e->midi.block;
    uint8_t ch = b[i].b[0] & 0x0F, note = b[i].b[1];
    for (int j = i + 1; j < e->midi.n_block; j++) {
        if (midi_is_note_off(&b[j]) && (b[j].b[0] & 0x0F) == ch && b[j].b[1] == note) {
            b[j].len = 0;
            return;
        }
    }
    for (int v = 0; v < SP_VOICES; v++) {
        sp_voice *w = &e->voices[v];
        if (w->state == SP_V_ON && w->out_note == note && w->out_ch == ch) {
            w->out_note = -1;
            return;
        }
    }
}

/* This block's MIDI out, sorted by frame (stable: a play's Start before its
 * first tick), into the ring; and, sending clock while stopped, the free
 * running ticks at the tempo. */
static void midi_flush(sp_engine *e, int frames) {
    const sp_midi_map *m = e->midi.map;
    if (m && m->clock == SP_CLOCK_SEND && !e->seq.playing && !e->offline) {
        double period = 60.0 * e->rate / (e->seq.bpm * 24.0);
        if (e->midi.free_next < 0) e->midi.free_next = (double)e->frame;
        for (;;) {
            int64_t f = (int64_t)ceil(e->midi.free_next - SP_FRAME_TOL);
            if (f >= e->frame + frames) break;
            midi_push(e, f < e->frame ? e->frame : f, MO_CLOCK, 1, 0xF8, 0, 0);
            e->midi.free_next += period;
        }
    }
    mo_event *b = e->midi.block;
    for (int i = 1; i < e->midi.n_block; i++) {
        mo_event x = b[i];
        int j = i - 1;
        while (j >= 0 && b[j].frame > x.frame) {
            b[j + 1] = b[j];
            j--;
        }
        b[j + 1] = x;
    }
    for (int i = 0; i < e->midi.n_block; i++) {
        if (b[i].len == 0) continue;
        bool off = midi_is_note_off(&b[i]);
        if (!mo_push(&e->midi.out, &b[i], off ? 0 : SP_VOICES) && b[i].kind == MO_NOTE && !off) midi_unsent(e, i);
    }
    e->midi.n_block = 0;
}

/* ── The count-in ─────────────────────────────────────────────────────────── */

#define SP_CLICK_MS 30.0

/* `beats` clicks a beat apart at the tempo: from this block's first frame,
 * or while the sequencer plays from its next bar (so a recording that
 * starts on the downbeat after them lines up with the pattern). Reports
 * the first click and the downbeat after the last. */
static void start_count_in(sp_engine *e, uint32_t id, int beats) {
    double origin, interval;
    if (e->seq.playing) {
        double bar = grid_target(e, m_at(e, e->frame), SP_BAR);
        origin = (double)e->seq.base_frame + (bar - e->seq.base_m) / e->seq.tpf;
        interval = (double)SP_PPQN / e->seq.tpf;
    } else {
        origin = (double)e->frame;
        interval = 60.0 * e->rate / e->seq.bpm;
    }
    e->click.origin = origin;
    e->click.interval = interval;
    e->click.beats = beats;
    e->click.next = 0;
    e->click.next_frame = (int64_t)ceil(origin - SP_FRAME_TOL);
    sp_emit(e, &(sp_event){
                   .type = SP_EV_COUNT_IN,
                   .a = (int32_t)id,
                   .b = beats,
                   .frame = e->click.next_frame,
                   .until = (int64_t)ceil(origin + beats * interval - SP_FRAME_TOL),
               });
}

/* The count-in's clicks into `out`: a short sine blip on each beat, the
 * first higher and louder. */
static void clicks(sp_engine *e, float *out, int frames) {
    if (e->click.t < 0 && e->click.next >= e->click.beats) return;
    const int length = (int)(SP_CLICK_MS * e->rate / 1000.0);
    const double attack = e->rate / 1000.0, decay = 0.006 * e->rate;
    for (int i = 0; i < frames; i++) {
        if (e->click.next < e->click.beats && e->frame + i >= e->click.next_frame) {
            bool accent = e->click.next == 0;
            e->click.t = 0;
            e->click.phase = 0;
            e->click.step = 2.0 * M_PI * (accent ? 1760.0 : 1320.0) / e->rate;
            e->click.gain = accent ? 0.5f : 0.35f;
            e->click.next++;
            e->click.next_frame = (int64_t)ceil(e->click.origin + e->click.next * e->click.interval - SP_FRAME_TOL);
        }
        if (e->click.t < 0) continue;
        double env = (e->click.t < attack ? e->click.t / attack : 1.0) * exp(-e->click.t / decay);
        float x = e->click.gain * (float)(env * sin(e->click.phase));
        out[2 * i] += x;
        out[2 * i + 1] += x;
        e->click.phase += e->click.step;
        if (++e->click.t >= length) e->click.t = -1;
    }
}

static void apply(sp_engine *e, const sp_cmd *c, bool live) {
    switch (c->type) {
    case SP_CMD_TRIGGER:
    case SP_CMD_NOTE_ON:
        if (live && c->pad < SP_PADS) {
            /* note_on drives a keys pad chromatically; any other pad it
             * just triggers. */
            bool keys = c->type == SP_CMD_NOTE_ON && e->pads[c->pad].mode == SP_KEYS;
            live_trigger(e, c, keys);
        }
        break;
    case SP_CMD_RELEASE:
    case SP_CMD_NOTE_OFF:
        if (live && c->pad < SP_PADS) {
            uint8_t mode = e->pads[c->pad].mode;
            if (mode == SP_GATE || mode == SP_LOOP || mode == SP_KEYS) {
                cancel_pending(e, c->pad);
                bool keys_note = c->type == SP_CMD_NOTE_OFF && mode == SP_KEYS;
                release_pad_voices(e, c->pad, keys_note ? c->note : -1);
            }
        }
        break;
    case SP_CMD_PAD:
        if (c->pad < SP_PADS) {
            /* Whatever the old definition was (a toggle's loop, a loop that
             * becomes a one-shot), in the same step as the change. */
            if (c->stop) {
                cancel_pending(e, c->pad);
                release_pad_voices(e, c->pad, -1);
            }
            e->pads[c->pad] = c->u.pad;
        }
        break;
    case SP_CMD_SAMPLE: {
        int slot = c->u.sample.slot;
        if (slot < 0 || slot >= SP_SLOTS) break;
        retire_sample(e, e->slots[slot]);
        e->slots[slot] = c->u.sample.sample;
        if (!c->u.sample.sample)
            for (int i = 0; i < SP_PADS; i++)
                if (e->pads[i].slot == slot) e->pads[i].slot = -1;
        break;
    }
    case SP_CMD_PATTERN:
        if (c->u.pattern.at_bar && e->seq.playing) {
            sp_drop_pattern(e, e->seq.next);
            e->seq.next = c->u.pattern.pattern;
        } else {
            sp_drop_pattern(e, e->seq.next);
            e->seq.next = NULL;
            swap_pattern(e, c->u.pattern.pattern, e->seq.playing ? e->seq.origin : 0, e->frame);
        }
        break;
    case SP_CMD_TEMPO:
        e->seq.bpm = c->u.value;
        rebase(e);
        break;
    case SP_CMD_SWING:
        if (e->seq.playing) e->seq.pending_swing = c->u.value;
        else e->seq.swing = c->u.value;
        break;
    case SP_CMD_PLAY:
        if (!e->seq.playing) {
            e->seq.playing = true;
            e->seq.play++;
            e->seq.base_frame = e->frame;
            e->seq.base_m = 0;
            e->seq.done = -SP_SEG_EPS;
            e->seq.origin = 0;
            e->seq.tpf = sp_tpf(e->seq.bpm, e->rate);
            seq_started(e);
        }
        break;
    case SP_CMD_STOP:
        stop_sequencer(e);
        break;
    case SP_CMD_RATE:
        if (c->u.rate > 0) {
            e->rate = c->u.rate;
            rebase(e);
        }
        break;
    case SP_CMD_RESET:
        stop_sequencer(e);
        for (int i = 0; i < SP_PADS; i++) e->pads[i] = (sp_pad){.slot = -1};
        for (int i = 0; i < SP_SLOTS; i++) {
            retire_sample(e, e->slots[i]);
            e->slots[i] = NULL;
        }
        for (int i = 0; i < SP_VOICES; i++) voice_release(e, &e->voices[i], cut_frames(e));
        sp_drop_pattern(e, e->seq.pattern);
        e->seq.pattern = NULL;
        e->seq.bpm = 120.0;
        e->seq.swing = 0.5;
        e->seq.tpf = sp_tpf(120.0, e->rate);
        e->click.beats = 0;
        set_midi_map(e, NULL);
        break;
    case SP_CMD_COUNT_IN:
        if (live && c->u.count_in.beats > 0) start_count_in(e, c->u.count_in.id, c->u.count_in.beats);
        else e->click.beats = 0;
        break;
    case SP_CMD_MIDI:
        /* With no stream (offline) only releases: a note held across a
         * route change still ends. */
        if (live) {
            atomic_fetch_add_explicit(&e->midi.in_messages, 1, memory_order_relaxed);
            midi_in(e, c);
        } else {
            midi_in_release(e, c);
        }
        break;
    case SP_CMD_MIDI_GONE:
        midi_input_gone(e, c->u.midi.src);
        break;
    case SP_CMD_MIDI_MAP:
        set_midi_map(e, c->u.map);
        break;
    }
    if (c->type == SP_CMD_PATTERN || c->type == SP_CMD_RESET || c->type == SP_CMD_STOP)
        atomic_store_explicit(&e->playing_now, e->seq.playing, memory_order_relaxed);
    if (c->type == SP_CMD_PLAY) atomic_store_explicit(&e->playing_now, true, memory_order_relaxed);
}

static void publish_counts(sp_engine *e) {
    int active = 0;
    for (int i = 0; i < SP_VOICES; i++) active += e->voices[i].state != SP_V_FREE;
    atomic_store_explicit(&e->voices_active, active, memory_order_relaxed);
    if (active > atomic_load_explicit(&e->voices_peak, memory_order_relaxed))
        atomic_store_explicit(&e->voices_peak, active, memory_order_relaxed);
}

/* ── The renderer ─────────────────────────────────────────────────────────── */

/* Render `frames` stereo frames into `out` (interleaved float). Commands
 * queued before the call take effect at its first frame. */
static void sp_render(sp_engine *e, float *out, int frames) {
    memset(out, 0, sizeof(float) * 2 * (size_t)frames);
    e->offline = false;
    e->at = e->frame;
    sp_cmd c;
    uint64_t gone = midi_gone_take(e);
    while (sp_pop_cmd(e, &c)) apply(e, &c, true);
    midi_gone_apply(e, gone);

    sequence(e, frames);
    int cursor = 0;
    for (int i = 0; i < e->n_actions; i++) {
        const sp_action *a = &e->actions[i];
        e->at = e->frame + a->off;
        if (a->off > cursor) {
            mix(e, out, cursor, a->off);
            cursor = a->off;
        }
        if (a->kind == SP_A_GATE) {
            sp_voice *v = &e->voices[a->voice];
            if (v->state == SP_V_ON && v->serial == a->serial) voice_release(e, v, v->release_frames);
        } else if (a->pad >= SP_PADS) {
            continue;
        } else if (a->kind == SP_A_FIRE) {
            voice_start(e, a->pad, a->vel, a->note_on ? a->note : -1, -1, e->frame + a->off, SP_SRC_PLAYED);
        } else {
            /* A step: gate, loop and keys pads sound until the next step. A
             * step that also ends within this block gets its gate now, in
             * order (sequence() only saw the voices there before it). */
            uint8_t mode = e->pads[a->pad].mode;
            bool gated = mode == SP_GATE || mode == SP_LOOP || mode == SP_KEYS;
            uint64_t before = e->serial;
            voice_start(e, a->pad, a->vel, a->note, gated ? a->gate_at : -1, e->frame + a->off, SP_SRC_PLAYED);
            if (gated && e->serial != before && a->gate_at <= e->seq.done) {
                for (int v = 0; v < SP_VOICES; v++) {
                    if (e->voices[v].state != SP_V_ON || e->voices[v].serial != e->serial) continue;
                    insert_action(e, i, (sp_action){
                                            .off = clamp_off(frame_of(e, a->gate_at) - e->frame, frames),
                                            .kind = SP_A_GATE,
                                            .voice = (uint8_t)v,
                                            .serial = e->serial,
                                        });
                    break;
                }
            }
        }
    }
    mix(e, out, cursor, frames);
    clicks(e, out, frames);
    midi_flush(e, frames);

    for (int i = 0; i < 2 * frames; i++) out[i] = sp_soft_clip(out[i]);
    e->frame += frames;
    reclaim(e);
    publish_counts(e);
}

/* The renderer's role while no audio runs (the control path, under its
 * lock): apply what is queued. Live calls are dropped (they would sound
 * late) unless `live`; fades end at once. */
static void sp_drain(sp_engine *e, bool live) {
    e->offline = true;
    e->at = e->frame;
    sp_cmd c;
    uint64_t gone = midi_gone_take(e);
    while (sp_pop_cmd(e, &c)) apply(e, &c, live);
    midi_gone_apply(e, gone);
    reclaim(e);
    publish_counts(e);
}

/* ── Offline resampling and peaks (the loader) ────────────────────────────── */

#define RS_ZC 16   /* zero crossings each side */
#define RS_RES 512 /* table points per zero crossing */

static float rs_table[RS_ZC * RS_RES + 2];
static bool rs_ready = false;

/* Blackman-windowed sinc, from 0 to RS_ZC zero crossings. */
static void rs_init(void) {
    if (rs_ready) return;
    for (int k = 0; k <= RS_ZC * RS_RES + 1; k++) {
        double x = (double)k / RS_RES, u = x / RS_ZC;
        double sinc = k == 0 ? 1.0 : sin(M_PI * x) / (M_PI * x);
        double w = u >= 1.0 ? 0.0 : 0.42 + 0.5 * cos(M_PI * u) + 0.08 * cos(2 * M_PI * u);
        rs_table[k] = (float)(sinc * w);
    }
    rs_ready = true;
}

static double rs_kernel(double x) {
    double p = fabs(x) * RS_RES;
    int i = (int)p;
    if (i >= RS_ZC * RS_RES) return 0.0;
    double f = p - i;
    return rs_table[i] + (rs_table[i + 1] - rs_table[i]) * f;
}

/* `in` (interleaved 16-bit, `frames` at `from` Hz) at `to` Hz, band-limited
 * (windowed sinc, cutoff at the lower Nyquist). NULL when out of memory. */
static int16_t *sp_resample(const int16_t *in, int64_t frames, int channels, int from, int to, int64_t *out_frames) {
    rs_init();
    double ratio = (double)from / to; /* input frames per output frame */
    double fc = ratio > 1.0 ? 1.0 / ratio : 1.0;
    double half = RS_ZC / fc;
    int64_t n = (int64_t)((double)frames * to / from);
    int16_t *out = malloc((size_t)(n > 0 ? n : 1) * (size_t)channels * sizeof(int16_t));
    if (!out) return NULL;
    for (int64_t j = 0; j < n; j++) {
        double x = (double)j * ratio;
        int64_t lo = (int64_t)ceil(x - half), hi = (int64_t)floor(x + half);
        double acc[2] = {0, 0}, wsum = 0;
        for (int64_t i = lo; i <= hi; i++) {
            double w = rs_kernel(((double)i - x) * fc);
            wsum += w;
            if (i < 0 || i >= frames) continue;
            for (int c = 0; c < channels; c++) acc[c] += w * in[i * channels + c];
        }
        for (int c = 0; c < channels; c++) {
            double y = wsum != 0 ? acc[c] / wsum : 0;
            out[j * channels + c] = (int16_t)(y > 32767 ? 32767 : y < -32768 ? -32768 : lrint(y));
        }
    }
    *out_frames = n;
    return out;
}

static int64_t sp_peak_count(int64_t frames) { return (frames + SP_PEAK_FRAMES - 1) / SP_PEAK_FRAMES; }

/* Waveform peaks: one byte per SP_PEAK_FRAMES frames, the largest |sample|
 * over the channels, linear (255 = full scale). `out` holds
 * sp_peak_count(frames) bytes. */
static void sp_peaks(const int16_t *data, int64_t frames, int channels, uint8_t *out) {
    int64_t n = sp_peak_count(frames);
    for (int64_t p = 0; p < n; p++) {
        int peak = 0;
        int64_t end = (p + 1) * SP_PEAK_FRAMES < frames ? (p + 1) * SP_PEAK_FRAMES : frames;
        for (int64_t f = p * SP_PEAK_FRAMES; f < end; f++)
            for (int c = 0; c < channels; c++) {
                int x = abs((int)data[f * channels + c]);
                if (x > peak) peak = x;
            }
        out[p] = (uint8_t)((peak * 255 + 16384) / 32768);
    }
}

/* Interleaved little-endian f32 bytes (`channels` per frame; any
 * alignment, a binary may start anywhere) to 16-bit, keeping the first
 * `keep` channels, clipped to ±1. NULL when out of memory. */
static int16_t *sp_from_f32(const uint8_t *in, int64_t frames, int channels, int keep) {
    int16_t *out = malloc((size_t)(frames > 0 ? frames : 1) * (size_t)keep * sizeof(int16_t));
    if (!out) return NULL;
    for (int64_t f = 0; f < frames; f++)
        for (int c = 0; c < keep; c++) {
            float x;
            memcpy(&x, in + 4 * (f * channels + c), sizeof(x));
            x *= 32767.0f;
            out[f * keep + c] = (int16_t)lrintf(x > 32767.0f ? 32767.0f : x < -32768.0f ? -32768.0f : x);
        }
    return out;
}
