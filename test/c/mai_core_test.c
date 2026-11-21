/*
 * Host test of priv/native/common/mai_core.c: the processor registry, the
 * render wrapper's clock and counters, and the mai_post queue. Run by
 * test/mob_audio_io_c_core_test.exs (cc on the PATH).
 */
#include <stdio.h>
#include <stdlib.h>

#include "../../priv/native/common/mai_core.c"

static int failures = 0;

#define CHECK(cond)                                                       \
    do {                                                                  \
        if (!(cond)) {                                                    \
            fprintf(stderr, "%s:%d: CHECK failed: %s\n", __FILE__, __LINE__, #cond); \
            failures++;                                                   \
        }                                                                 \
    } while (0)

typedef struct {
    int renders;
    int64_t last_frame;
    int32_t last_rate, last_channels;
    int zeroed;
    int events[8];
    int n_events;
    int32_t stream;
} probe;

static void probe_render(void *ctx, float *out, int32_t frames, const mai_time *t) {
    probe *p = ctx;
    p->zeroed = 1;
    for (int32_t i = 0; i < frames * t->channels; i++)
        if (out[i] != 0.0f) p->zeroed = 0;
    for (int32_t i = 0; i < frames * t->channels; i++) out[i] = 0.5f;
    p->renders++;
    p->last_frame = t->frame;
    p->last_rate = t->rate;
    p->last_channels = t->channels;
    uint8_t msg[4] = {1, 2, 3, (uint8_t)p->renders};
    mai_post(p->stream, msg, sizeof msg);
}

static void probe_event(void *ctx, int32_t stream, int32_t event, const mai_time *t) {
    probe *p = ctx;
    (void)stream; (void)t;
    if (p->n_events < 8) p->events[p->n_events++] = event;
}

static void test_registry(void) {
    mai_processor proc = {probe_render, NULL, NULL};
    probe p = {0};
    char long_name[MAI_NAME_MAX + 4];
    memset(long_name, 'x', sizeof long_name - 1);
    long_name[sizeof long_name - 1] = 0;

    CHECK(mai_abi_version() == MAI_ABI_VERSION);
    CHECK(mai_register(NULL, &proc, &p) == MAI_ERR_INVALID);
    CHECK(mai_register("", &proc, &p) == MAI_ERR_INVALID);
    CHECK(mai_register(long_name, &proc, &p) == MAI_ERR_INVALID);
    CHECK(mai_register("x", NULL, &p) == MAI_ERR_INVALID);

    mai__register_builtins();
    char names[MAI_MAX_PROCESSORS][MAI_NAME_MAX];
    CHECK(mai__processor_names(names, MAI_MAX_PROCESSORS) == 2);

    /* fill the table: 2 built-ins + 14 */
    char name[16];
    for (int i = 0; i < MAI_MAX_PROCESSORS - 2; i++) {
        snprintf(name, sizeof name, "p%d", i);
        CHECK(mai_register(name, &proc, &p) == MAI_OK);
    }
    CHECK(mai_register("one_too_many", &proc, &p) == MAI_ERR_FULL);
    /* replacing an existing name needs no free slot */
    CHECK(mai_register("p0", &proc, &p) == MAI_OK);
    CHECK(mai_unregister("nope") == MAI_ERR_NOT_FOUND);
    for (int i = 0; i < MAI_MAX_PROCESSORS - 2; i++) {
        snprintf(name, sizeof name, "p%d", i);
        CHECK(mai_unregister(name) == MAI_OK);
    }
    CHECK(mai__processor_names(names, MAI_MAX_PROCESSORS) == 2);
}

static void test_render_and_posts(void) {
    probe p = {0};
    mai_processor proc = {probe_render, NULL, probe_event};
    CHECK(mai_register("probe", &proc, &p) == MAI_OK);

    mai_stream *s = mai__alloc_stream();
    CHECK(s != NULL);
    CHECK(mai__acquire(s, "missing") == MAI_ERR_NOT_FOUND);
    CHECK(mai__acquire(s, "probe") == MAI_OK);
    /* pinned: can't be replaced or removed while a stream uses it */
    CHECK(mai_register("probe", &proc, &p) == MAI_ERR_BUSY);
    CHECK(mai_unregister("probe") == MAI_ERR_BUSY);

    s->rate = 48000;
    s->channels = 2;
    p.stream = s->id;
    CHECK(mai_post(s->id, "x", 1) == MAI_ERR_NOT_OPEN); /* not open yet */
    atomic_store(&s->open, true);
    mai__event(s, MAI_EVENT_STARTED);
    CHECK(p.n_events == 1 && p.events[0] == MAI_EVENT_STARTED);

    float buf[2 * 256];
    for (int i = 0; i < 2 * 256; i++) buf[i] = 9.0f; /* garbage the wrapper must clear */
    mai__render(s, buf, 192);
    CHECK(p.renders == 1);
    CHECK(p.zeroed == 1);
    CHECK(p.last_frame == 0);
    CHECK(p.last_rate == 48000 && p.last_channels == 2);
    mai__render(s, buf, 96);
    CHECK(p.last_frame == 192);
    CHECK(atomic_load(&s->callbacks) == 2);
    CHECK(atomic_load(&s->frames_total) == 288);
    CHECK(atomic_load(&s->cb_frames_min) == 96 && atomic_load(&s->cb_frames_max) == 192);

    /* last_error keeps what fits, NUL-terminated */
    char long_err[MAI_ERRMAX * 2];
    memset(long_err, 'e', sizeof long_err - 1);
    long_err[sizeof long_err - 1] = 0;
    mai__set_error(s, long_err);
    CHECK(strlen(s->last_error) == MAI_ERRMAX - 1);

    /* a reopen keeps the stream clock going */
    mai__reset_callback_state(s);
    mai__render(s, buf, 10);
    CHECK(p.last_frame == 288);

    /* posts: the three renders posted one each, in order */
    mai_msg m;
    for (int i = 1; i <= 3; i++) {
        CHECK(mai__pop_post(s, &m));
        CHECK(m.len == 4 && m.data[0] == 1 && m.data[3] == (uint8_t)i);
    }
    CHECK(!mai__pop_post(s, &m));

    /* full after MAI_POSTS, counted as dropped */
    for (int i = 0; i < MAI_POSTS; i++) CHECK(mai_post(s->id, "y", 1) == MAI_OK);
    CHECK(mai_post(s->id, "y", 1) == MAI_ERR_FULL);
    CHECK(atomic_load(&s->posts_dropped) == 1);
    uint8_t big[MAI_POST_MAX + 1] = {0};
    CHECK(mai_post(s->id, big, sizeof big) == MAI_ERR_TOO_LARGE);
    CHECK(mai_post(s->id, NULL, 3) == MAI_ERR_INVALID);
    while (mai__pop_post(s, &m)) {}
    CHECK(mai_post(s->id, NULL, 0) == MAI_OK); /* an empty message is fine */
    CHECK(mai__pop_post(s, &m) && m.len == 0);

    /* a stale id never reaches the slot's next stream */
    int32_t old = s->id;
    atomic_store(&s->open, false);
    mai__release(s);
    CHECK(mai_unregister("probe") == MAI_OK);
    mai_stream *s2 = mai__alloc_stream();
    CHECK(s2 == s);
    CHECK(s2->id != old);
    atomic_store(&s2->open, true);
    CHECK(mai_post(old, "z", 1) == MAI_ERR_NOT_OPEN);
    CHECK(mai__stream(old) == NULL);
    CHECK(mai__stream(s2->id) == s2);
    CHECK(mai__stream(0) == NULL && mai__stream(-5) == NULL);
    atomic_store(&s2->open, false);
}

static void test_sine(void) {
    mai__register_builtins();
    mai_stream *s = mai__alloc_stream();
    CHECK(mai__acquire(s, "mai_sine") == MAI_OK);
    s->rate = 48000;
    s->channels = 2;
    atomic_store(&s->open, true);
    float buf[2 * 480];
    mai__render(s, buf, 480);
    float peak = 0;
    for (int i = 0; i < 2 * 480; i++) peak = fabsf(buf[i]) > peak ? fabsf(buf[i]) : peak;
    CHECK(peak > 0.12f && peak <= 0.126f);
    CHECK(buf[2 * 10] == buf[2 * 10 + 1]); /* both channels */
    atomic_store(&s->open, false);
    mai__release(s);
}

int main(void) {
    test_registry();
    test_render_and_posts();
    test_sine();
    if (failures) {
        fprintf(stderr, "%d failure(s)\n", failures);
        return 1;
    }
    printf("mai_core: all checks passed\n");
    return 0;
}
