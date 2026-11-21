/*
 * c_src/midi.c — the plain-C pieces of Magpie's MIDI: the byte-stream
 * parser and the out queue between the renderer and the thread that
 * sends. sampler.c includes it (the renderer maps notes and sends clock;
 * see "MIDI" there), so it builds on the Mac for the host tests too.
 *
 * Time: incoming messages carry CLOCK_MONOTONIC nanoseconds (Android's
 * MIDI timestamps are System.nanoTime); outgoing ones carry the engine
 * frame they belong to, which the sending thread turns into nanoseconds
 * through the output timestamp.
 */

#include <stdatomic.h>
#include <stdbool.h>
#include <stdint.h>
#include <string.h>

/* Not every includer uses every helper (the host tests, the NIF). */
#define MI_UNUSED __attribute__((unused))

/* ── Parser ───────────────────────────────────────────────────────────────── */

/* One complete message: `n` bytes (1..3). */
typedef struct {
    uint8_t status, d1, d2, n;
} mi_msg;

/* Per source: a stream of bytes may split a message anywhere. */
typedef struct {
    uint8_t status;  /* the message being assembled (running status for channel messages); 0 none */
    uint8_t data[2];
    uint8_t have, need;
    bool sysex;
} mi_parser;

static int mi_data_len(uint8_t status) {
    if (status < 0xF0) {
        uint8_t type = status & 0xF0;
        return type == 0xC0 || type == 0xD0 ? 1 : 2;
    }
    if (status == 0xF1 || status == 0xF3) return 1;
    if (status == 0xF2) return 2;
    return 0;
}

/* Feed `n` bytes; each complete message goes to `emit`. Real-time bytes
 * (0xF8–0xFF) pass through wherever they fall, even inside another message,
 * without disturbing it (active sensing and the undefined ones are
 * dropped). Channel messages keep running status; system common messages
 * cancel it. SysEx is skipped up to its end (or the next status byte).
 * Data bytes with no status are dropped. */
MI_UNUSED static void mi_parse(mi_parser *p, const uint8_t *in, size_t n, void (*emit)(void *ctx, mi_msg m),
                               void *ctx) {
    for (size_t i = 0; i < n; i++) {
        uint8_t b = in[i];
        if (b >= 0xF8) {
            if (b != 0xF9 && b != 0xFD && b != 0xFE) emit(ctx, (mi_msg){b, 0, 0, 1});
            continue;
        }
        if (b == 0xF0) {
            p->sysex = true;
            p->status = 0;
            continue;
        }
        if (b == 0xF7) {
            p->sysex = false;
            continue;
        }
        if (b & 0x80) {
            p->sysex = false;
            p->have = 0;
            p->need = (uint8_t)mi_data_len(b);
            p->status = b;
            if (p->need == 0) {
                if (b != 0xF4 && b != 0xF5) emit(ctx, (mi_msg){b, 0, 0, 1});
                p->status = 0;
            }
            continue;
        }
        if (p->sysex || !p->status) continue;
        p->data[p->have++] = b;
        if (p->have < p->need) continue;
        emit(ctx, (mi_msg){p->status, p->data[0], p->need > 1 ? p->data[1] : 0, (uint8_t)(1 + p->need)});
        p->have = 0;
        if (p->status >= 0xF0) p->status = 0;
    }
}

/* ── The out queue (renderer → sending thread) ────────────────────────────── */

#define MO_RING 1024 /* power of two */

enum { MO_NOTE, MO_CLOCK, MO_TRANSPORT };

/* A message for engine frame `frame`. */
typedef struct {
    int64_t frame;
    uint8_t len, kind;
    uint8_t b[3];
} mo_event;

typedef struct {
    mo_event ev[MO_RING];
    _Atomic uint32_t head, tail;
    _Atomic uint64_t dropped;
} mo_ring;

/* Queue `ev` unless fewer than `reserve` + 1 places are free (the renderer
 * keeps the last places for note-offs). */
static bool mo_push(mo_ring *r, const mo_event *ev, uint32_t reserve) {
    uint32_t tail = atomic_load_explicit(&r->tail, memory_order_relaxed);
    uint32_t head = atomic_load_explicit(&r->head, memory_order_acquire);
    if (tail - head >= MO_RING - reserve) {
        atomic_fetch_add_explicit(&r->dropped, 1, memory_order_relaxed);
        return false;
    }
    r->ev[tail & (MO_RING - 1)] = *ev;
    atomic_store_explicit(&r->tail, tail + 1, memory_order_release);
    return true;
}

/* The oldest message without taking it; false when empty. */
MI_UNUSED static bool mo_peek(mo_ring *r, mo_event *ev) {
    uint32_t head = atomic_load_explicit(&r->head, memory_order_relaxed);
    uint32_t tail = atomic_load_explicit(&r->tail, memory_order_acquire);
    if (head == tail) return false;
    *ev = r->ev[head & (MO_RING - 1)];
    return true;
}

MI_UNUSED static void mo_drop(mo_ring *r) {
    uint32_t head = atomic_load_explicit(&r->head, memory_order_relaxed);
    atomic_store_explicit(&r->head, head + 1, memory_order_release);
}

MI_UNUSED static bool mo_pop(mo_ring *r, mo_event *ev) {
    if (!mo_peek(r, ev)) return false;
    mo_drop(r);
    return true;
}

/* What an earlier stream left in the queue, taken into `out` (MO_RING
 * places): the notes whose other half went out. A note-off whose note-on
 * is still here was never sounding for the receiver: neither goes. A
 * note-on with no note-off after it belongs to a voice still holding the
 * note: it goes, late, so the voice's note-off has its note-on. A
 * note-off whose note-on went out goes. Clock and transport go stale.
 * Returns how many. */
MI_UNUSED static int mo_take_stale(mo_ring *r, mo_event *out) {
    int n = 0;
    mo_event ev;
    while (mo_pop(r, &ev)) {
        if (ev.kind != MO_NOTE) continue;
        bool off = (ev.b[0] & 0xF0) == 0x80;
        int last = n - 1;
        while (last >= 0 && !(((out[last].b[0] ^ ev.b[0]) & 0x0F) == 0 && out[last].b[1] == ev.b[1])) last--;
        if (off && last >= 0 && (out[last].b[0] & 0xF0) != 0x80) {
            memmove(&out[last], &out[last + 1], (size_t)(n - last - 1) * sizeof(mo_event));
            n--;
        } else {
            out[n++] = ev;
        }
    }
    return n;
}
