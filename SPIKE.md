# mob_audio_io spike (MOB-531, phase 1)

The question: can one Mob plugin own the audio streams on Android and iOS
while each app keeps its DSP in its own C, linked against the plugin's C
interface? The assessment marked the iOS half of that as unverified.

**Verdict: go.** App C registered processors into plugin C and rendered
through it on Android (moto g power 2021), the iOS simulator and the iPhone
SE, with Magpie's real sampler core (`sp_render`) among them. The adjustments
below go into the full plugin.

## What was built

- `priv/include/mob_audio_io.h`: the C ABI. `mai_register(name, processor,
  ctx)`, `mai_unregister`, `mai_post(stream, msg, n)` (real-time safe, single
  producer), `mai_abi_version`; `mai_processor` = `render`, `capture`,
  `stream_event`; `mai_time` = stream frame, host ns, rate, channels.
- One C core (`priv/native/common/`) for both platforms: the registry, the
  stream table, the render wrapper (stream clock, callback counters), the
  `mai_post` ring, the event thread (reopen, buffer tuning, posts, owner
  messages), owner monitors. The Android file adds AAudio, the iOS file
  RemoteIO and the `AVAudioSession`; both `#include` the core, so the ABI is
  the same code on both and CI checks every header symbol is defined in both
  platforms' objects.
- Android output: AAudio by `dlopen`, low latency, shared (exclusive on
  request, falling back), float stereo at the device rate, two bursts of
  buffer, a burst more after an underrun (Magpie's `tune_buffer`), reopen
  after a disconnect from the event thread (AAudio's error callback only
  flags it). Lifted from FMutate's `fm1audio.c` and Magpie's
  `magpie_audio.c`.
- iOS output: a RemoteIO unit at the hardware rate, float stereo; the
  session as `:play` (Playback), `:mix` (+ mixWithOthers) or
  `:play_and_record` (Choir Coach's options), a 5 ms preferred IO buffer;
  interruptions, route changes (a new rate rebuilds the unit) and
  media-services resets, flagged by the notification and handled on the
  event thread. Lifted from `fm1audio.c`, `FMutateAudioSession.swift` and
  `choir_audio_ios.m`.
- Elixir: `MobAudioIo.open/3`, `close/1`, `status/0,1`, `reset_stats/1`,
  `processors/0`; owner messages `{:mob_audio_io, kind, stream, payload}`;
  native events logged from Elixir with `[mob_audio_io]` (`MobAudioIo.Log`);
  `MobAudioIo.Fake` behind `MobAudioIo.Backend`; `MobAudioIo.SelfTest`
  (opens 100 ms of `"mai_silence"`, passes if the callback count rose).
- `example/`: a `mob.new` app (`com.genericjam.audioiospike`) whose own NIF
  `c_src/spike_dsp.c` registers `"spike_click"` (a blip every 0.5 s that
  posts `{frame, host_ns}` per click) and `"magpie_sampler"` (Magpie's
  `sampler.c` copied in, `sp_render` as the render, the engine's events
  posted to the owner).

## App C → plugin C

| | Android (moto g power 2021, Android 11, arm64) | iOS simulator (iPhone Air, iOS 27.0) | iPhone SE 3rd gen (iOS 27.2.1) |
|--|--|--|--|
| App + plugin link into one binary | yes (`libaudio_io_spike.so`) | yes | yes (one executable) |
| `:spike_dsp.registered()` | `[spike_click: 0, magpie_sampler: 0]` | same | `[spike_click: 0, magpie_sampler: 0]` |
| `:spike_dsp.abi()` (app C calling `mai_abi_version`) | 1 | 1 | 1 |
| `MobAudioIo.processors()` | built-ins + both app processors | same | built-ins + both app processors |
| Clicks rendered by app C / posts delivered to the owner (60 s) | 123 / 123 | 123 / 123 | 123 / 123 |
| `sp_render` frames over the 61 s run | 2,928,240 (= the stream's frames) | not run | 2,928,480 |
| Magpie engine event through `mai_post` | `SP_EV_COUNT_IN` (type 6, 240 beats, frame 48000) | n/a | `SP_EV_COUNT_IN` (same payload) |

So the [INFERENCE] in the assessment holds on both platforms: a static NIF
of the app calls the plugin's functions directly, and the plugin calls back
into the app's C from the audio thread. Nothing in mob_dev had to change.

## Numbers

60 s after a 1 s settle and `reset_stats/1` (`AudioIoSpike.Probe.run/3`).
`cb_max_us`/`cb_avg_us`: time in one callback (the processor plus the
plugin's bookkeeping). `gap_max_us`: worst time between two callbacks'
starts.

| | Android, `spike_click` | Android, `magpie_sampler` | iOS sim, `spike_click` | iPhone SE, `spike_click` | iPhone SE, `magpie_sampler` |
|--|--|--|--|--|--|
| rate | 48,000 | 48,000 | 48,000 | 48,000 | 48,000 |
| burst / buffer (frames) | 240 / 480 (cap 1920) | 240 / 480 | 128 / 128 (IO buffer) | 240 / 240 (5 ms IO buffer) | 240 / 240 |
| frames per callback (min, max) | 16, 240 | 16, 240 | 557, 558 | 240, 240 | 240, 240 |
| callbacks in 60 s | 13,313 | 13,312 | 5,168 | 12,000 | 12,001 |
| reported latency (ms) | 10.0 (queued frames only, see below) | 10.0 | 2.77 | 16.5 (output 11.5 + IO 5.0) | 16.5 |
| xruns | 0 | 0 | 0 | 0 | 0 |
| cb_max_us / cb_avg_us | 289.6 / 12.0 | 756.4 / 39.3 | 8.0 / 2.8 | 57 / 4.2 | 75 / 8.0 |
| gap_max_us | 11,183 | 11,784 | 11,725 | 5,125 | 5,129 |
| posts dropped | 0 | 0 | 0 | 0 | 0 |
| self-test | pass | | pass | pass | |

Android notes: the stream is shared and on Android 11 (the legacy
AudioTrack path, not MMAP): 5 ms bursts, double-buffered to 10 ms; some
callbacks are 16 frames, which is why there are 222 callbacks/s rather than
200. The worst gap (11 ms) is about two bursts, inside the 10 ms buffer plus
the burst being played, and no underrun was counted. Magpie's full sampler
with the count-in clicking costs 39 µs per 5 ms callback on average, 0.76 ms
at worst.

iOS simulator: the host's audio runs 557-frame (11.6 ms) callbacks whatever
the session's 2.67 ms IO buffer says, so the simulator's `burst` is
misleading; `cb_frames` is what ran.

### iOS latency probe (half day in the assessment)

`MobAudioIo.status().session` with `session: :play_and_record` and the
5 ms preferred IO buffer:

| | iOS simulator | iPhone SE |
|--|--|--|
| category | PlayAndRecord | PlayAndRecord |
| preferred IO buffer | 5.0 ms | 5.0 ms |
| IOBufferDuration granted | 2.67 ms | 5.0 ms (240 frames) |
| outputLatency | 0.10 ms | 11.5 ms |
| inputLatency | 0.10 ms | 2.4 ms |
| route | Speaker | Speaker |

On the iPhone SE iOS grants the 5 ms asked for (240 frames at 48 kHz) and
calls back with exactly 240 frames, 200 times a second; the worst gap
between callbacks over a minute was 5.13 ms and the worst callback 75 µs
with Magpie's sampler. What a rendered frame waits before it reaches the
speaker is outputLatency + IOBufferDuration = 11.5 + 5.0 = 16.5 ms; the
built-in mic adds 2.4 ms of inputLatency, so an acoustic round trip
through RemoteIO would be about 16.5 + 2.4 + 5.0 (the input buffer) ≈ 24 ms
[INFERENCE: not measured acoustically]. outputLatency is the route's (the
speaker); headphones and Bluetooth will differ. Choir Coach's 10 ms buffer
would add 5 ms each way for nothing on this phone.

The simulator's numbers (0.1 ms latencies, 557-frame callbacks) are the
Mac's audio and say nothing about a phone.

## Gaps found

1. **No header export in the manifest (MOB-541).** App C reaches the header
   by a relative path: `#include "../deps/mob_audio_io/priv/include/mob_audio_io.h"`
   for a Hex dependency; a path dependency isn't under `deps/`, so the
   example uses `../../priv/include/...`. mob_dev passes only
   `-Dproject_c_nifs` to the app's `ios/build.zig` (`native_build.ex:3841`)
   and adds no include path for plugins. Fix: a manifest key (e.g.
   `c_abi: %{include: "priv/include"}`) that adds `-I` for the app's NIFs on
   both platforms. Recorded in the manifest's `host_requirements` until then.
2. **No cross-platform symbol check (MOB-542).** A header function missing
   on one platform shows up as a link error in the app. This repo's CI does
   it for itself (`scripts/check_abi_symbols.sh` on the NDK and Xcode
   objects); mob_dev's `validate_plugin` should do it for every plugin with
   a C ABI.
3. **No release flag for plugin code (MOB-510).** `MOB_RELEASE` doesn't
   reach plugin sources, so the plugin has no test-only native code: the
   built-in processors and `reset_stats/1` are safe to ship, and the fake
   and `send_message/2` are Elixir.
4. **Android `<service>`: not a gap.** mob_dev 0.6.19+ has
   `android.manifest_application_snippets` and list-valued `bridge_kt`
   (MOB-39); mob_background 0.3.1 already declares a `dataSync|microphone`
   foreground service that way. Background mic leans on
   mob_background >= 0.3.1.
5. **Registration is lazy.** A processor exists once the app's NIF module is
   loaded, which Mob does on first use: right after boot `processors/0`
   listed only the built-ins until something called `:spike_dsp`. Apps must
   load their DSP module before `open/3` (`Code.ensure_loaded/1` in
   `on_start`), or the full plugin's `open/3` takes a `:module` to load.
6. **`AAudioStream_getTimestamp` can abort the process on Android 11.** The
   moto g power's crash buffer has SIGABRTs in
   `AudioStreamLegacy::getBestTimestamp` from both FMutate (11-20 12:24) and
   Magpie (11-20 04:29), each from its stats/clock thread. The plugin calls
   it only on API 31+; below that `latency_ms` is the frames written but not
   yet read (10 ms here), which leaves out the HAL's part. Magpie's clock
   (`refresh_clock`) uses the same call and needs the same guard when it
   moves onto the plugin.
7. **iPhone dist from this Mac (not the plugin).** `mix mob.connect` timed
   out against the iPhone: the app named its node after its Wi-Fi address
   (`10.0.0.120`, from its `Documents/mob_diag_host_ip.txt`), a network the
   Mac (`192.168.50.1`) isn't on, and the BEAM and Python on this Mac got
   EHOSTUNREACH to the phone's USB link-local address while `/usr/bin/nc`
   connected [INFERENCE: macOS local-network privacy for processes this
   agent harness starts]. `mix mob.deploy --ios` (BEAM push) also hung
   silently twice before succeeding after ~140 s. The iPhone numbers were
   taken without dist: `AudioIoSpike.Probe.maybe_run_from_file/0` runs the
   probe on boot when `Documents/mai_probe_request` exists and writes the
   result back for `devicectl device copy from` (the iOS BEAM has no `HOME`;
   Documents comes from `MOB_BEAMS_DIR`).
8. **Example quirk:** changing only `applicationId` (not the Kotlin
   `namespace`) breaks mob_dev's app launch (`<bundle>/.MainActivity`); the
   example is started with `am start -n <id>/com.example.audio_io_spike.MainActivity`.

## Adjustments for the full plugin

- `stream_event` gets the stream id and a `mai_time` (rate, channels, the
  stream clock): the assessment's `(ctx, event)` can't tell a processor its
  rate before the first render or which stream to `mai_post` to. Magpie
  needs both (its `SP_CMD_RATE` at open, its events to the owner).
- `host_ns` is the callback's start. The full plugin should add the
  presentation time (AAudio timestamps on API 31+, `AudioTimeStamp.mHostTime`
  on iOS) for Magpie's sample-accurate clock, behind the Android 11 guard.
- Report per-callback frames (`cb_frames`), not only `burst`: Android's
  legacy path and the iOS simulator call with other sizes than the burst.
- Input and duplex, the device list, input presets and file decoding, as in
  the assessment; the core's stream table, event thread and post ring carry
  over unchanged.
- Processors load lazily (gap 5): `open/3` should take the app's NIF module
  and load it.
- Magpie on iPhone also needs mob_midi's C interface on CoreMIDI
  (MOB-543): its engine sends and receives MIDI through `mob_midi.h`, which
  answers `MOB_MIDI_ERR_UNSUPPORTED` on iOS today. Independent of this
  plugin.

## How to reproduce

See `AGENTS.md` (Testing). In short: `cd example`, zig 0.17.0 on `PATH`,
lease the device, `mix mob.deploy --native --android --device <serial>`,
`mix mob.connect --no-iex`, then
`mix run --no-start scripts/rpc.exs <node> 'AudioIoSpike.Probe.run("spike_click", 60)' 90000`.
For the iPhone: copy an empty `Documents/mai_probe_request` into the app's
container with `xcrun devicectl device copy to`, relaunch, and copy
`Documents/mai_probe_result.txt` back two and a half minutes later.
