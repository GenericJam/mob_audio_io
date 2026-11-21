# mob_audio_io: device verification

What the example app (`example/`, `com.genericjam.audioiospike`) measured
with the full plugin, through `AudioIoSpike.Probe.run_all/0` over RPC. A
full run: the self-test, the devices and processors, 60 s of output
(`spike_click`, posting a message per click), 60 s of input (`tone_loop`,
the plugin's meter and a pitch estimate once a second), 10 s of a 1 kHz tone
played through the speaker and heard back through the microphone (duplex
`tone_loop`), 15 s of a burst a second whose acoustic round trip the capture
times (duplex `loop_latency`), and decoding a WAV, an MP3 and an M4A shipped
in `priv/audio`. Worst-case counters are zeroed 1 s after each open.

The moto e, iPhone SE and Android 15 emulator columns are full runs of the
final build (main faf1de8, with `decode_stream/3`) on 2026-11-21, after
device audio resumed (MOB-546). The moto g power column is the earlier final
run (main 8f1905e, PRs #1-#3); the simulator column ran the final iOS code.

## Results

| | moto g power 2021 (Android 11, API 30) | moto e (Android 10, API 29, armv7a) | iPhone SE 3rd gen (iOS 27.2.1) | Android 15 emulator (API 35, arm64) | iOS simulator (iPhone Air, iOS 27.0) |
|--|--|--|--|--|--|
| build | main 8f1905e | main faf1de8 | main faf1de8 | main faf1de8 (`run_all(10)`) | final iOS code (e74cd0e) |
| self-test (output + input) | pass | pass | pass | pass | pass |
| output 60 s, `spike_click` | 13,535 callbacks, 0 xruns, cb_frames 240 (16-240), cb_max 740 µs, gap_max 10.6 ms, latency 10.0 ms, present :estimate, 123/123 posts | 13,313 callbacks, 0 xruns, cb_frames 240 (16-240), cb_max 512 µs, gap_max 10.4 ms, latency 10.0 ms, present :estimate, 123/123 posts | 12,001 callbacks, 0 xruns, cb_frames 240 (240-240), cb_max 58 µs, gap_max 5.10 ms, latency 16.5 ms, present :timestamp, 122/122 posts | 10 s: 0 xruns, latency 115.5 ms, **present :timestamp** | 5,254 callbacks, 0 xruns, cb_frames 557-558, cb_max 8 µs, gap_max 11.7 ms, 122 posts |
| input 60 s | 16,620 callbacks, 0 xruns, cb_frames 192 (64-192), cb_max 243 µs, gap_max 5.7 ms, input latency 4.0 ms; ambient RMS median -74.3 dBFS, peak -57.8 | 13,505 callbacks, 0 xruns, cb_max 214 µs, gap_max 6.7 ms, input latency 5.0 ms; ambient RMS median -72.4 dBFS, peak -34.6 | 12,532 callbacks, 0 xruns, cb_frames 240, cb_max 1.25 ms, gap_max 5.33 ms, input latency 7.4 ms; ambient RMS median -60.3 dBFS, peak -26.2 | 10 s: 0 xruns, input latency 11.2 ms, **present :timestamp** | 5,328 callbacks, 0 xruns, RMS median -60.0 dBFS, peak -38.3 |
| tone 1 kHz, speaker → own mic | 1000.0 Hz median (999.98-1000.02) in 20/20 windows, -9.6 dBFS | not heard: see below | 1000.0 Hz in 20/20 windows (1000.0-1000.0), RMS median -21.4 dBFS | emulator mic is a synthetic 220 Hz tone, not the Mac's | not heard (the Mac's speaker and mic; RMS -59.6) |
| tone from another source | | the iPhone's 1 kHz: 1000.1-1000.3 Hz, clarity ≥ 0.99, `:default` and `:unprocessed` | the Mac's 1 kHz: 1000.1-1000.2 Hz, clarity ≥ 0.99, with `:default`, `:unprocessed` and `:voice` (VoiceProcessingIO, input only) | | |
| duplex | 3,550 callbacks, 0 xruns, 0 input underflows, cb_max 321 µs, input latency 12 ms | 10 s: 0 xruns, 0 underflows, cb_max 381 µs | 10 s: 0 xruns, 0 underflows, cb_max 56 µs | 0 xruns, present :timestamp | 0 xruns, 19 underflows in 15 s (the simulator alternates 557/558-frame blocks) |
| loopback round trip | median 108.6 ms (8 of 16 bursts heard; spread 5-452 ms: the detector is unreliable against this phone's AGC) | 0 of 16 heard (see below) | **median 28.2 ms (27.1-29.0), 16 of 16 heard**; presentation-time gap 14.4 ms | not meaningful (synthetic mic) | median 37.5 ms (37.5-38.1, 15 of 16 heard); presentation-time gap 37.3 ms |
| decode WAV 44.1 kHz stereo | 88,200 frames, 2 ch, -24.1 dBFS | 88,200 frames, -24.1 dBFS | 88,200 frames, 17 ms | 88,200 frames | 88,200 frames, 17 ms |
| decode MP3 48 kHz mono | 96,000 frames (exact) | 96,000 (exact) | 97,920 (ExtAudioFile keeps the encoder padding) | 96,000 (exact) | 97,920 |
| decode M4A 48 kHz stereo | 96,256 frames | 96,256 | 96,000 (exact) | 96,256 | 96,000 |
| `decode_stream/3` (1024-frame chunks, window 2) | | same PCM as `decode/2` for all three; cancel after the first chunk: `:decode_cancelled` in 24 ms, nothing after it; `processor:`, `max_ms: 100` (`:too_long`), missing file all end correctly | same PCM for all three; cancel 0.24 ms, nothing after it; the other ends as on the moto e | same PCM for all three; cancel 3.4 ms; the other ends correct | |
| decode errors | missing: `{:not_found, ...}`; 100 kB cap: `:too_large` | same | same | same | same |
| devices | in: 14, 15 builtin_mic; out: 2 speaker, 3 earpiece | in: 13, 14 builtin_mic; out: 2 earpiece, 3 speaker | in: "iPhone Microphone"; out: Speaker | | MicrophoneBuiltIn; Speaker |
| presets | default 6, generic 1, performance 10, unprocessed 9, voice 7 all open (agent run); `:voice` heard another phone's tone at -36 dBFS (AEC/NS gate the phone's own tone and a quiet room to zeros) | all 5 open with 0 xruns (granted 6, 1, 10, 9, 7) | `:default`, `:unprocessed` (Measurement), `:voice` (VoiceChat + VoiceProcessingIO) open with 0 xruns | | `:voice` VoiceChat + VoiceProcessingIO, `:unprocessed` Measurement (agent run) |
| permission | revoked: input and duplex `{:error, :permission}`, output runs | granted (`pm grant`) | allowed; input opens | granted (`pm grant`) | revoked: `:permission`; granted: opens |

The moto e's own speaker could not be heard. The plugin's signal reached the
speaker: `dumpsys media.audio_flinger` showed the HAL output on
`AUDIO_DEVICE_OUT_SPEAKER` at -15.1 dBFS for the whole tone (the tone's own
RMS, 0.25 amplitude), media volume 15/15, master mute off. Yet neither the
moto e's microphone nor the iPhone SE beside it heard it, while the moto e's
microphone heard the iPhone's tone cleanly. The fault is past the HAL (the
phone's speaker or how it lay), not in the plugin; the moto g power 2021
heard its own tone 20/20 through the same Android path.

The first moto e run overlapped the iPhone's run: its input and tone numbers
from that run heard the iPhone, so the moto e acoustic rows above are from
runs with the other phone silent.

Other findings:

* Android's legacy `AudioStreamRecord::read` drops data when a non-blocking
  float read of a FAST input asks for more than has arrived (android-11, -12
  and main): duplex reads its input as 16-bit and converts (31 underflows/s
  before, 0 after).
* An explicit Android device the audio policy won't route a media stream to
  (the second mic, the earpiece) is disconnected ~60 ms after each start; the
  core backs such quick losses off (0.5 s → 8 s) instead of reopening ~7
  times a second.
* Both motos are below API 31, so their presentation times are estimates
  (`present: :estimate`). The AAudio timestamp path (API 31+) ran on the
  Android 15 emulator: output, input and duplex all `present: :timestamp`,
  no abort. Not yet on an Android 12+ phone.
* Route change: no headphones were attached. The route and rate-change paths
  are covered by the macOS CoreAudio CI job and the simulator's session-union
  run (`:mix` output, then an input stream: Playback → PlayAndRecord with the
  output running, back to Playback 400 ms after the input closed), not by a
  physical route change (MOB-546).
* iPhone RPC: the Mac can't reach the phone's Wi-Fi-named node
  (EHOSTUNREACH, macOS local-network privacy); `scripts/rpc.exs --iphone`
  launches it named for 127.0.0.1 and goes through `iproxy` to port 9101
  (FMutate's recipe).

## Streaming decode (`decode_stream/3`)

Measured on the host first, then on the phones (the `decode_stream/3` row
above). `decode/2` runs on it.

Measured on the Mac (M4 Pro, macOS 27.0.1), 5-minute 44.1 kHz stereo
files (a 1 kHz tone), chunks of 4096 frames, a window of 4:

| | WAV | M4A (AAC 128k, `mono: true`) | MP3 (lame 192k) |
|--|--|--|--|
| C, `test/c/mai_apple_test.c` (-O2): whole file | 13,230,000 frames, 52 ms; footprint +0.2 MB (the whole PCM: 100.9 MB) | 13,230,000 frames, 476 ms; +0.2 MB (50.5 MB) | 13,230,000 frames, 804 ms; +0.1 MB (100.9 MB) |
| C: chunk buffers alive at once | 5 (the window + the one filling) | 5 | 5 |
| C: cancel a third of the way in, until the codec is released | 0.04-1.5 ms | 0.07-0.18 ms | 0.04-10 ms |
| BEAM, `test/host_nif_smoke.exs` (the real NIF, an owner reading and acking each chunk): whole file | 2,245 ms; binary memory +0.4 MB, RSS +0.8 MB | 2,023 ms; +0.3 MB, +0.8 MB | 2,595 ms; +0.5 MB, +2.5 MB |
| BEAM: `decode_cancel/1` to `:decode_cancelled` in the owner's mailbox | 0.48 ms | 1.11 ms | 0.34 ms |

Under ASan the footprint grows with the decode (its quarantine keeps the
freed chunks), so CI checks the footprint bound (< 24 MB) on an -O2 build.
In the core's host test (`test/c/mai_core_test.c`, also under TSan) a
cancel ends a decode waiting for an ack in ~0.02 ms, and one busy decoding
at its next push.

On the phones (`AudioIoSpike.Probe.decode_stream_all/0`, part of
`run_all/0`), the Android `AMediaCodec` loop and the iOS build of the same
ExtAudioFile code returned the same PCM as `decode/2`, and cancel ended the
decode with nothing after `:decode_cancelled` (table above). Long files were
measured on the host only; Android's HE-AAC doubled rate is not measured yet.

# The spike (MOB-531, phase 1)

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

## Review

Reviewer subagent (Codex rate-limited until 2026-11-21 01:09): **SHIP** as
a spike, no blockers. Fixed in this repo: AAudio `setUsage`/`setContentType`
are optional (API 28; API 26/27 would otherwise refuse every stream),
`set_logger/1` runs on a dirty scheduler (it takes the plugin's lock),
`MAI_EVENT_ROUTE_CHANGED` reaches the processor only while the stream is
stopped (the header promises it never runs beside render), the monitor
resource type is taken over on a NIF upgrade, and the logger's message env is
cleared after each send. These need a device run again (the numbers above
predate them).

Left for the full plugin:
- an iOS interruption *began* for an app that was suspended has no *ended*
  (`AVAudioSessionInterruptionReasonAppWasSuspended`): rebuild the unit
  right away instead of waiting;
- return the platform's detail for a failed open (`{:error, {reason, detail}}`);
- park the event thread on a condition variable while no stream is open
  (it wakes every 5 ms for the app's life now);
- drain the `mai_post` ring before a closed stream's slot is freed.

## How to reproduce

See `AGENTS.md` (Testing): deploy `example/` and run
`AudioIoSpike.Probe.run_all()` (or `output/3`, `input/2`, `tone/3`,
`loopback/2`, `decode_all/0`, `decode_stream_all/0`) through
`scripts/rpc.exs`; on an iPhone with `--iphone <udid>`. (The spike's
file-triggered probe described above is gone.) The streaming decode's
host numbers: the `macos-coreaudio` steps of `.github/workflows/test.yml`
(`mai_apple_test.c` at -O2 with `MAI_APPLE_TEST_NO_HARDWARE=1`, and
`test/host_nif_smoke.exs`).
