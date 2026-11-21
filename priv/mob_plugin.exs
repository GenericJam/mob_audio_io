%{
  name: :mob_audio_io,
  mob_version: "~> 0.9",
  plugin_spec_version: 1,
  description:
    "Low-latency audio streams (output, input, duplex; AAudio on Android, RemoteIO + AVAudioSession on iOS), " <>
      "device lists and file decoding, with a C ABI for the app's own DSP",
  # On-device proof for `mix mob.selftest` / mob_ci: opens 150 ms output
  # (and, with the microphone permitted, input) streams on the built-in
  # silent processor and checks their callback counts rose (see
  # MobAudioIo.SelfTest).
  selftest: MobAudioIo.SelfTest,
  nifs: [
    # One C core (priv/native/common/mai_nif.c) included by both platform
    # files, so the C ABI of priv/include/mob_audio_io.h has the same symbols
    # on both. Android: plain C over AAudio and AMediaCodec (both dlopen'd)
    # and JNI (devices, the microphone permission).
    %{module: :mob_audio_io_nif, native_dir: "priv/native/android", lang: :c, platform: :android},
    # iOS: Objective-C for AVAudioSession; the audio units and ExtAudioFile
    # decoding are plain C.
    %{module: :mob_audio_io_nif, native_dir: "priv/native/ios", lang: :objc, platform: :ios}
  ],
  android: %{
    # Input and duplex streams. Declared by the plugin because the NIF asks
    # for it; the app still requests it at runtime
    # (Mob.Permissions.request(socket, :microphone)), and open/3 answers
    # {:error, :permission} until it is granted.
    permissions: ["android.permission.RECORD_AUDIO"]
  },
  ios: %{
    frameworks: ["AudioToolbox", "AVFoundation"],
    # The binary references the recording APIs, so App Store validation
    # wants the purpose string even in an app that only plays. Usage
    # descriptions combine with other plugins'; an app's own Info.plist
    # string wins.
    plist_keys: %{
      NSMicrophoneUsageDescription: "Audio input for this app's sound features."
    }
  },
  # Manual host-app steps the build can't automate; printed on every
  # `mix mob.deploy --native` of the host.
  host_requirements: [
    "App C that registers a processor includes the header by a relative path " <>
      "(the manifest can't export an include path yet, MOB-541): " <>
      "#include \"../deps/mob_audio_io/priv/include/mob_audio_io.h\" from c_src/.",
    "iOS: to keep audio running with the screen locked or the app in the background, " <>
      "the app's own Info.plist must declare UIBackgroundModes [audio] (only for apps " <>
      "with a real audio feature: App Store review rejects it otherwise).",
    "Android: to keep the microphone open in the background, run a foreground service " <>
      "of type microphone (mob_background >= 0.3.1 provides one)."
  ]
}
