# mob_audio_io

Low-latency audio streams for [Mob](https://github.com/GenericJam/mob) apps:
output, input and duplex on AAudio (Android) and RemoteIO / VoiceProcessingIO
with the `AVAudioSession` (iOS), plus device lists and file decoding. The
plugin owns the streams; the app's DSP is its own C, registered through
`priv/include/mob_audio_io.h` and called from the audio thread. Elixir opens
and closes streams, reads `MobAudioIo.status/0`, and gets the streams' events
as messages.

## Install

```elixir
# mix.exs
{:mob_audio_io, github: "GenericJam/mob_audio_io"}  # "~> 0.1" once 0.1.0 is on Hex

# mob.exs
config :mob, :plugins, [:mob_audio_io]
config :mob_dev, static_nifs: [%{module: :my_dsp, archs: [:all]}]
```

The plugin declares `RECORD_AUDIO` (Android) and a microphone usage string
(iOS). Ask for the microphone with `Mob.Permissions.request(socket,
:microphone)` before opening an input; `open/3` answers `{:error,
:permission}` until it's granted.

## Use

```c
/* c_src/my_dsp.c: the app's own NIF */
#include "../deps/mob_audio_io/priv/include/mob_audio_io.h"

static void render(void *ctx, float *out, int32_t frames, const mai_time *t) {
    /* fill frames * t->channels interleaved floats; real-time rules apply.
       t->frame is the stream clock, t->present_ns when out[0] is heard. */
}

static void capture(void *ctx, const float *in, int32_t frames, const mai_time *t) {
    /* the microphone's frames; copy what you need into your own ring */
}

static int load(ErlNifEnv *env, void **priv, ERL_NIF_TERM info) {
    static const mai_processor p = {render, capture, NULL};
    return mai_register("my_synth", &p, NULL) == MAI_OK ? 0 : 1;
}
```

```elixir
{:ok, stream} = MobAudioIo.open(self(), :duplex, processor: "my_synth", module: :my_dsp)
# {:mob_audio_io, :started, stream, %{rate: 48000, burst: 240, buffer: 480, ...}}
MobAudioIo.status(stream)   # rate, cb_frames, buffer, latency_ms, xruns, cb_max_us, gap_max_us, ...
MobAudioIo.devices(:input)  # [%{id: 7, name: "USB-C headset", kind: :usb}, ...]
{:ok, %{rate: r, channels: c, frames: n, data: f32}} = MobAudioIo.decode("/path/song.m4a")
MobAudioIo.close(stream)
```

The moduledoc of `MobAudioIo` has the options, the messages, the errors, the
iOS session policy, background audio and the platform table. In tests,
`config :mob_audio_io, backend: MobAudioIo.Fake`.

`example/` is a Mob app whose own C registers processors (a click, Magpie's
sampler core, a tone loop, a loopback latency probe); `docs/VERIFICATION.md`
has what it measured on phones.

## License

MIT
