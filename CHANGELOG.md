# Changelog

## 0.1.0

- C ABI `priv/include/mob_audio_io.h` (ABI 1): `mai_register`,
  `mai_unregister`, `mai_post`, `mai_abi_version`, `mai_now_ns`;
  `mai_processor` (render, capture, `stream_event(ctx, stream, event,
  time)`) and `mai_time` (stream frame, host ns, presentation ns, rate,
  channels). iOS times are on mach_absolute_time's clock (the clock of
  `AudioTimeStamp.mHostTime` and mob_midi), Android's on CLOCK_MONOTONIC.
- Presentation time per block: iOS from `mHostTime` plus the session's
  latency; Android 12+ from AAudio timestamps (polled off the audio thread),
  Android 8-11 estimated from the buffer, never calling
  `AAudioStream_getTimestamp`, which aborts there.
- `MobAudioIo.open/3` takes `:module` (the app's NIF module, loaded first),
  `:channels`, `:input_channels`, `:preset`, `:device`, `:input_device`; a
  failed open returns the platform's detail (`{:error, {:unavailable,
  detail}}`).
- `status/0`: `cb_frames` (the last callback's), `cb_frames_min/max`,
  `input_latency_ms`, `present`, `input_underflows`, `in_peak_db`,
  `in_rms_db`.
- Posts still queued at close reach the owner before `:stopped`, or are
  counted in `:stopped`'s `posts_dropped` when the owner has exited.
- The event thread sleeps while no stream is open; failed reopens back off
  from 0.5 s to 8 s.
- iOS: an interruption that began while the app was suspended, or whose end
  never came, is recovered when the app returns to the foreground.
- `MobAudioIo.devices/1`, `MobAudioIo.decode/2`; the Fake decodes WAV.
- `MobAudioIo.SelfTest` checks output and, when the microphone is
  permitted, input.
* Android: AAudio input with presets (default, generic, performance,
  unprocessed, voice) and device selection, duplex (input read as 16-bit in
  the output callback through a ring), `RECORD_AUDIO` check, `devices/1`
  through `AudioManager`, `decode/2` through `AMediaCodec`.
* iOS: RemoteIO input (VoiceProcessingIO for `:voice`), duplex in one unit,
  the session configured from all open streams, preferred input by port UID,
  microphone permission check, `devices/1`, `decode/2` through ExtAudioFile.
* A stream lost within 1 s of each start backs off (0.5 s to 8 s) instead
  of reopening at once; an explicit Android device that disappears falls
  back to the system's route on reopen.
* Manifest: `RECORD_AUDIO` and `NSMicrophoneUsageDescription`.
* CI: the C core under ASan+UBSan and TSan; the Apple C parts against real
  CoreAudio on macOS.
