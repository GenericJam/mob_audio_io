%{
  name: :mob_audio_io,
  mob_version: "~> 0.9",
  plugin_spec_version: 1,
  description:
    "Low-latency audio streams (AAudio on Android, RemoteIO + AVAudioSession on iOS) with a C ABI for the app's own DSP",
  # On-device proof for `mix mob.selftest` / mob_ci: opens a 100 ms output
  # stream on the built-in silent processor and checks the callback count
  # rose (see MobAudioIo.SelfTest).
  selftest: MobAudioIo.SelfTest,
  nifs: [
    # One C core (priv/native/common/mai_nif.c) included by both platform
    # files, so the C ABI of priv/include/mob_audio_io.h has the same symbols
    # on both. Android: plain C over AAudio (dlopen'd).
    %{module: :mob_audio_io_nif, native_dir: "priv/native/android", lang: :c, platform: :android},
    # iOS: Objective-C for AVAudioSession; the RemoteIO unit is plain C.
    %{module: :mob_audio_io_nif, native_dir: "priv/native/ios", lang: :objc, platform: :ios}
  ],
  ios: %{
    frameworks: ["AudioToolbox", "AVFoundation"]
  },
  # Manual host-app steps the build can't automate; printed on every
  # `mix mob.deploy --native` of the host.
  host_requirements: [
    "App C that registers a processor includes the header by a relative path " <>
      "(the manifest can't export an include path yet, MOB-541): " <>
      "#include \"../deps/mob_audio_io/priv/include/mob_audio_io.h\" from c_src/.",
    "iOS: to keep playing with the screen locked, Info.plist must declare " <>
      "UIBackgroundModes [audio] (only for apps that really play audio in the background)."
  ]
}
