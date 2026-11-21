# Changelog

## Unreleased (0.1.0-dev, spike for MOB-531)

- C ABI `priv/include/mob_audio_io.h`: `mai_register`, `mai_unregister`,
  `mai_post`, `mai_abi_version`; `mai_processor` (render, capture,
  stream_event) and `mai_time` (stream frame, host ns, rate, channels).
- Output streams: AAudio (Android, low latency, shared or exclusive, burst
  tuning after underruns, reopen on disconnect) and RemoteIO +
  AVAudioSession (iOS, interruptions, route changes, media-services resets).
- `MobAudioIo.open/3`, `close/1`, `status/0,1`, `reset_stats/1`,
  `processors/0`; owner messages; `[mob_audio_io]` logging of native events;
  `MobAudioIo.Fake`; `MobAudioIo.SelfTest`.
- Built-in processors `"mai_silence"` and `"mai_sine"`.
