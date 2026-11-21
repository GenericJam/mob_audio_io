# mob_audio_io

Low-latency audio streams for [Mob](https://github.com/GenericJam/mob) apps:
AAudio on Android, a RemoteIO unit and the `AVAudioSession` on iOS. The
plugin owns the stream; the app's DSP is its own C, registered through
`priv/include/mob_audio_io.h` and called from the audio thread. Elixir opens
and closes streams, reads `MobAudioIo.status/0`, and gets the stream's events
as messages.

**Status: spike (MOB-531), not on Hex.** Output streams only. See
[SPIKE.md](SPIKE.md) for the measurements and what the full plugin needs.

## Use

```elixir
# mix.exs
{:mob_audio_io, github: "GenericJam/mob_audio_io"}

# mob.exs
config :mob, :plugins, [:mob_audio_io]
config :mob_dev, static_nifs: [%{module: :my_dsp, archs: [:all]}]
```

```c
/* c_src/my_dsp.c: the app's own NIF */
#include "../deps/mob_audio_io/priv/include/mob_audio_io.h"

static void render(void *ctx, float *out, int32_t frames, const mai_time *t) {
    /* fill frames * t->channels interleaved floats; real-time rules apply */
}

static int load(ErlNifEnv *env, void **priv, ERL_NIF_TERM info) {
    static const mai_processor p = {render, NULL, NULL};
    return mai_register("my_synth", &p, NULL) == MAI_OK ? 0 : 1;
}
```

```elixir
{:ok, stream} = MobAudioIo.open(self(), :output, processor: "my_synth")
# {:mob_audio_io, :started, stream, %{rate: 48000, burst: 96, buffer: 192, ...}}
MobAudioIo.status(stream)
MobAudioIo.close(stream)
```

The moduledoc of `MobAudioIo` has the options, the messages, the errors and
the platform table. In tests, `config :mob_audio_io, backend: MobAudioIo.Fake`.

`example/` is a Mob app that registers a click and Magpie's sampler core from
its own C (see `AGENTS.md`).

## License

MIT
