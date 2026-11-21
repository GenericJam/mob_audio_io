# mob_audio_io: Agent Instructions

You're in **mob_audio_io**, a Mob capability plugin: low-latency audio
streams (output, input, duplex; AAudio on Android, RemoteIO /
VoiceProcessingIO + `AVAudioSession` on iOS), device lists and file decoding,
with a C ABI (`priv/include/mob_audio_io.h`) that an app's own static NIF
uses to render and capture audio. Linear: MOB-531. `docs/VERIFICATION.md`
has what was measured on phones (and the spike that preceded the plugin).

Also read `~/code/mob/AGENTS.md`, `~/code/mob/MOB_PLUGINS.md` (manifest
schema) and `~/code/mob/guides/upstream_first.md` (the checklist this plugin
follows).

> Keep this file current: API shape, gotchas, host requirements.

## Anatomy

* `priv/include/mob_audio_io.h`: the C ABI (`mai_register`, `mai_post`,
  `mai_now_ns`, `mai_processor`, `mai_time`). Thread rules are in its header
  comment. ABI 1 is frozen with 0.1.0: bump `MAI_ABI_VERSION` on any
  incompatible change.
* `priv/native/common/mai_core.c`: registry, stream table, `mai__process`
  (capture then render, stream clock, input meter, counters), the
  presentation-time anchor (seqlock) and timing math, the `mai_post` SPSC
  ring, the PCM buffer decoders fill. No erl_nif, no platform API:
  `test/c/mai_core_test.c` runs it on the host.
* `priv/native/common/mai_nif.c`: the NIF functions, owner monitors, the
  event thread (reopen with backoff, xrun tuning, the anchor poll, posts,
  messages; parked on a condvar while no stream is open), and the `be_*`
  backend interface the platform files implement.
* `priv/native/android/mob_audio_io_nif.c`: AAudio (dlopen), JNI (devices,
  RECORD_AUDIO), AMediaCodec decoding (dlopen).
* `priv/native/ios/mob_audio_io_nif.m`: the `AVAudioSession` policy across
  streams, permissions, notifications; it includes the plain-C Apple parts
  (audio unit callbacks, ExtAudioFile decoding) that the macOS CI job also
  runs against real CoreAudio.
* Both platform files `#include "../common/mai_nif.c"`, so the ABI's symbols
  are the same code on both; `scripts/check_abi_symbols.sh` checks it in CI.
* `src/mob_audio_io_nif.erl`: the NIF's Erlang module.
* `lib/mob_audio_io.ex` (API, option validation, the moduledoc is the
  spec), `native.ex`, `fake.ex` + `fake/wav.ex`, `backend.ex` (behaviour),
  `log.ex` (logs native events with the `[mob_audio_io]` prefix),
  `self_test.ex`.
* `example/`: a `mob.new` app (`com.genericjam.audioiospike`) whose
  `c_src/spike_dsp.c` (+ `verify_dsp.c`) registers a click, Magpie's
  sampler core, `"tone_loop"` and `"loop_latency"`. `AudioIoSpike.Probe` is
  the verification, over RPC.

## Testing

`mix test` (Fake backend, option validation, WAV decoding, self-test
classification, and the C core's host test when `cc` exists). CI also runs
the C core under ASan+UBSan and TSan, compiles the Android NIF with the NDK
and the iOS NIF with Xcode's clang (`-Werror`), checks the ABI symbols, and
runs the Apple C parts against CoreAudio on macOS.

On a device: `cd example`, zig **0.17.0** first on `PATH`
(`~/.local/share/mise/installs/zig/0.17.0/bin`), lease the device
(`agent-lease`), `mix mob.deploy --native --android --device <serial>` (or
`--ios --device <udid>`), then:

* Android / simulator: `mix mob.connect --no-iex`, then
  `mix run --no-start scripts/rpc.exs <node> 'AudioIoSpike.Probe.run_all()' 600000`.
* iPhone: the Mac can't reach the phone's Wi-Fi-named node (EHOSTUNREACH,
  macOS local-network privacy). Launch it named for 127.0.0.1 and go
  through usbmuxd: `xcrun devicectl device process launch --device <udid>
  --terminate-existing --environment-variables '{"MOB_NODE_HOST":"127.0.0.1"}'
  com.genericjam.audioiospike`, then
  `mix run --no-start scripts/rpc.exs --iphone <udid> '<elixir>' <timeout>`
  (it runs `iproxy` to the phone's port 9101).

## Releases

Bumping `version` in `mix.exs` on `main` publishes to Hex through
`.github/workflows/release.yml` (added with the 0.1.0 bump; it needs the
`HEX_API_KEY` and `MOB_PLUGIN_SIGN_KEY` repo secrets). Bump only when the
latest `tests` run on `main` is green; say so with `@all` on Muster `#mob`
first; then watch the `release` and `tests` runs and confirm with
`mix hex.info mob_audio_io`.

## Gotchas

* The example uses the plugin as a path dependency (`{:mob_audio_io, path:
  ".."}`); mob_dev names a path plugin by its directory, so the checkout
  must be called `mob_audio_io` (worktrees: `<dir>/mob_audio_io`). App C
  includes the header by a relative path (MOB-541):
  `../../priv/include/mob_audio_io.h` here,
  `../deps/mob_audio_io/priv/include/mob_audio_io.h` with the Hex package.
* `render`/`capture` must not lock, allocate, log or call `enif_*`.
  `mai_post` is single-producer: call it only from the stream's callbacks.
* `stream_event(STARTED)` runs before the first callback on a normal
  thread: that's where a processor learns the rate (Magpie's `SP_CMD_RATE`).
* Processors exist once the app's NIF module is loaded: pass `module:` to
  `open/3`.
* Never call `AAudioStream_getTimestamp` below API 31: it aborts the process
  on Android 11's legacy path. The core's anchor poll guards it.
* iOS times are on mach_absolute_time's clock (`CLOCK_UPTIME_RAW`), the
  clock of `AudioTimeStamp.mHostTime` and mob_midi, not Darwin's
  `CLOCK_MONOTONIC`.
* MOB_RELEASE doesn't reach plugin code (MOB-510): there is no test-only
  native code here; `reset_stats/1` and the built-in processors are safe to
  ship.
