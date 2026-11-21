# mob_audio_io: Agent Instructions

You're in **mob_audio_io**, a Mob capability plugin: low-latency audio
streams (AAudio on Android, RemoteIO + `AVAudioSession` on iOS) with a C ABI
(`priv/include/mob_audio_io.h`) that an app's own static NIF uses to render
audio. Linear: MOB-531. It is a **spike** (0.1.0-dev, not on Hex): output
only. `SPIKE.md` has what was measured and what the full plugin needs.

Also read `~/code/mob/AGENTS.md`, `~/code/mob/MOB_PLUGINS.md` (manifest
schema) and `~/code/mob/guides/upstream_first.md` (the checklist this plugin
follows).

> Keep this file current: API shape, gotchas, host requirements.

## Anatomy

* `priv/include/mob_audio_io.h`: the C ABI (`mai_register`, `mai_post`,
  `mai_processor`, `mai_time`). Thread rules are in its header comment.
* `priv/native/common/mai_core.c`: registry, stream table, render wrapper
  (stream clock, counters), `mai_post` SPSC ring. No erl_nif, no platform
  API: `test/c/mai_core_test.c` runs it on the host.
* `priv/native/common/mai_nif.c`: the NIF functions, owner monitors, the
  event thread (reopen, xrun tuning, posts, messages), the `be_*` backend
  interface.
* `priv/native/android/mob_audio_io_nif.c`: AAudio backend (dlopen).
* `priv/native/ios/mob_audio_io_nif.m`: RemoteIO + AVAudioSession backend.
  Both platform files `#include "../common/mai_nif.c"`, so the ABI's
  symbols are the same code on both.
* `src/mob_audio_io_nif.erl`: the NIF's Erlang module.
* `lib/mob_audio_io.ex` (API, option validation), `native.ex`, `fake.ex`,
  `backend.ex` (behaviour), `log.ex` (logs native events with the
  `[mob_audio_io]` prefix), `self_test.ex`.
* `example/`: a `mob.new` app (`com.genericjam.audioiospike`) whose
  `c_src/spike_dsp.c` registers a click processor and Magpie's sampler
  core. `AudioIoSpike.Probe.run/3` is the measurement, over RPC.
* `scripts/check_abi_symbols.sh`: every header function defined in a
  compiled platform object (CI runs it for both platforms).

## Testing

`mix test` (Fake backend, option validation, self-test classification, and
the C core's host test when `cc` exists). CI also compiles the Android NIF
with the NDK and the iOS NIF with Xcode's clang (`-Werror`) and checks the
ABI symbols.

On a device: `cd example`, zig **0.17.0** first on `PATH`
(`~/.local/share/mise/installs/zig/0.17.0/bin`), lease the device
(`agent-lease`), `mix mob.deploy --native --android --device <serial>` (or
`--ios --device <udid>`), then `mix mob.connect` and
`:rpc.call(node, AudioIoSpike.Probe, :run, ["spike_click", 60], 90_000)`.

## Gotchas

* The example uses the plugin as a path dependency (`{:mob_audio_io, path:
  ".."}`); mob_dev names a path plugin by its directory, so the checkout
  must be called `mob_audio_io`. App C includes the header by a relative
  path (MOB-541): `../../priv/include/mob_audio_io.h` here,
  `../deps/mob_audio_io/priv/include/mob_audio_io.h` with the Hex package.
* `render` must not lock, allocate, log or call `enif_*`. `mai_post` is
  single-producer: call it only from the stream's render.
* `stream_event(STARTED)` runs before the first render on a normal thread:
  that's where a processor learns the rate (Magpie's `SP_CMD_RATE`).
* MOB_RELEASE doesn't reach plugin code (MOB-510): there is no test-only
  native code here; `reset_stats/1` and the built-in processors are safe to
  ship.
