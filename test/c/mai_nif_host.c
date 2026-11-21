/*
 * A macOS host build of the NIF: priv/native/common/mai_nif.c with the iOS
 * backend's plain-C ExtAudioFile decode (priv/native/ios/mai_apple.c) and
 * no streams (every be_* but be_decode answers "unsupported"). It makes
 * the real decode_stream/3, decode_ack/2 and decode_cancel/1 (threads,
 * messages, acks, cancel, owner monitors) run in a host BEAM:
 * test/host_nif_smoke.exs loads it.
 *
 * clang -std=c11 -Wall -Wextra -Werror -Wno-unused-function -O2 -dynamiclib -undefined dynamic_lookup \
 *   -I"$ERTS_INCLUDE" test/c/mai_nif_host.c -o mob_audio_io_nif.so \
 *   -framework AudioToolbox -framework CoreFoundation
 */
#include "../../priv/native/common/mai_nif.c"
#include "../../priv/native/ios/mai_apple.c"

static const char *be_platform(void) { return "host"; }

static const char *be_load(void) { return "unsupported"; }

static const char *be_open(mai_stream *s) {
    (void)s;
    return "unsupported";
}

static const char *be_start(mai_stream *s) {
    (void)s;
    return "unsupported";
}

static void be_close(mai_stream *s) { s->be = s->be2 = NULL; }

static int32_t be_xruns(mai_stream *s) {
    (void)s;
    return 0;
}

static void be_tune(mai_stream *s) { (void)s; }

static void be_poll(mai_stream *s) { (void)s; }

static void be_latency(mai_stream *s, double *out_ms, double *in_ms) {
    (void)s;
    *out_ms = *in_ms = -1;
}

static void be_idle(void) {}

static ERL_NIF_TERM be_session_term(ErlNifEnv *env) { return atom(env, "nil"); }

static ERL_NIF_TERM be_devices(ErlNifEnv *env, int dir) {
    (void)dir;
    return error_atom(env, "unsupported");
}

static const char *be_decode(const char *path, mai_decode *d, char *detail, size_t detail_n) {
    return mai_apple_decode(path, d, detail, detail_n);
}
