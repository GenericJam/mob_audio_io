defmodule MobAudioIo do
  @moduledoc """
  Low-latency audio streams for Mob apps, with the DSP in the app's own C.

  The plugin owns the platform's audio streams: AAudio on Android, a RemoteIO
  (or VoiceProcessingIO) unit and the `AVAudioSession` on iOS. An app's
  static NIF registers a processor through the C interface in
  `priv/include/mob_audio_io.h` (`mai_register/3`), and the plugin calls it
  from the audio thread: `render` for output, `capture` for input, both (in
  that order, in one callback) for duplex. Audio never passes through the
  BEAM; Elixir opens and closes streams, reads `status/0`, lists devices,
  decodes files and receives the stream's messages.

  ## Platforms

  | | Android (API 28+) | iOS (15+) | Host (`mix test`, dev) |
  |--|--|--|--|
  | `:output` | AAudio, low latency, shared or exclusive | RemoteIO | `{:error, :unsupported}` |
  | `:input` | AAudio with an input preset | RemoteIO input (VoiceProcessingIO for `preset: :voice`) | `{:error, :unsupported}` |
  | `:duplex` | AAudio output, the input read in its callback | one RemoteIO unit, input pulled in the render | `{:error, :unsupported}` |
  | `devices/1` | `AudioManager.getDevices` (JNI) | `AVAudioSession` inputs, the current route's outputs | `{:error, :unsupported}` |
  | `decode/2` | `AMediaExtractor` + `AMediaCodec` | `ExtAudioFile` | `{:error, :unsupported}` |
  | `device:` (output) | an AAudio device id | `{:error, :unsupported}` (iOS routes output itself) | |
  | Presentation time | AAudio timestamps on 12+; an estimate from the buffer on 8-11 | `AudioTimeStamp.mHostTime` + the session's latency | |

  `MobAudioIo.Fake` runs all of it on the host for tests.

  ## Processors

  Two are built in: `"mai_silence"` (plays nothing and drops its input,
  what `MobAudioIo.SelfTest` opens) and `"mai_sine"` (440 Hz at -18 dBFS,
  for a listening check). Everything else comes from the app's C, by name:
  `processors/0` lists what's registered. A processor exists once the app's
  NIF module has loaded, which Mob does lazily: pass `module: :my_dsp` to
  `open/3` and it is loaded first.

  ## Messages

  The owner process receives, as `{:mob_audio_io, kind, stream, payload}`:

    * `:started`, `%{rate:, channels:, input_channels:, burst:, buffer:,
      sharing:, direction:, reopened:}`: the stream runs (again, after a
      reopen, with `reopened: true`).
    * `:stopped`, `%{reason: :closed | :device_lost | :interrupted |
      :owner_exit, posts_dropped: n}`. Posts the processor queued before a
      close reach the owner first; `posts_dropped` counts those that could
      not (the owner had exited).
    * `:device_lost`, `%{}`: the device went away (headphones, USB, a
      Bluetooth headset). The plugin reopens on the new route; `:started`
      follows.
    * `:interrupted`, `%{}` and `:resumed`, `%{}`: iOS session
      interruptions (a call, Siri, another app). After `:resumed` the stream
      reopens and `:started` follows. An interruption that never sends its
      end (the app was suspended) resumes when the app is back in the
      foreground.
    * `:route_changed`, `%{reason: :new_route | :new_rate | :media_reset}`:
      a new route. The stream is rebuilt for a new rate or a media-services
      reset.
    * `:xrun`, `%{xruns:, buffer:}`: underruns (output) and overruns
      (input) since open; on Android the buffer grows by a burst (up to its
      capacity) unless a fixed size was asked for.
    * `:error`, `%{reason:, detail:, while:}`: a reopen failed; it is tried
      again after 0.5 s, 1 s, 2 s ... up to every 8 s.
    * `:post`, `binary`: a message the processor queued with `mai_post`.

  A screen keeps these in assigns; `Mob.Test.send_message/2` stands in for
  them in tests. Every one but `:post` is also logged with a
  `[mob_audio_io]` prefix (`MobAudioIo.Log`), so they reach Sloppy Joe's
  `logs`.

  ## Errors

  `open/3` answers `{:error, reason}`:

    * `:unsupported`: this platform (the host) or this combination (an
      output `device:` on iOS).
    * `:permission`: an `:input` or `:duplex` stream without the
      microphone permission. Ask with `Mob.Permissions.request(socket,
      :microphone)` first; iOS answers `:permission` until the user chose.
    * `:unknown_processor`: nothing registered under that name.
    * `:busy`: 8 streams are open.
    * `{:module, reason}`: the `:module` didn't load.
    * `{:invalid_option, key}`: a bad option (checked before any native
      call).
    * `{:unavailable, detail}`: the platform refused the stream; `detail`
      is its own words (`"open: AAUDIO_ERROR_... (-889)"`,
      `"AudioUnitInitialize: -10875"`).
    * `{:session, detail}`: iOS couldn't configure or activate the audio
      session (another app holds it, a call is active).
    * `{:invalid_device, detail}`: no such `:device` / `:input_device`.

  ## Sessions (iOS)

  One `AVAudioSession` serves every stream, so it is configured from all
  the open ones:

    * `:play` (output default): the playback category; the silent switch
      doesn't mute it and other apps' audio stops.
    * `:mix`: plays beside other apps' audio (`mixWithOthers`, FMutate's).
    * `:play_and_record` (input and duplex default): PlayAndRecord with the
      speaker (not the earpiece) as the default output and Bluetooth A2DP
      allowed (Choir Coach's).

  Any open input makes the session PlayAndRecord; it mixes only if every
  open stream asked for `:mix`. The `:buffer` is the preferred IO buffer
  (5 ms for `:low_latency`); iOS rounds it to what the hardware does
  (`status().session.io_buffer_ms`). The input `:preset` picks the mode:
  `:unprocessed` is Measurement, `:voice` is VoiceChat with the
  VoiceProcessingIO unit (echo cancellation).

  ## Background audio

  A stream keeps running with the screen locked only if the app may run in
  the background:

    * iOS: the app's `Info.plist` declares `UIBackgroundModes` `[audio]`
      (this plugin doesn't add it: Apple rejects the mode for apps without
      an audio feature). Recording keeps going in the background once
      started; iOS doesn't let the microphone start from the background.
    * Android: output keeps running; the microphone needs a foreground
      service of type `microphone` while in the background, which
      `mob_background` (0.3.1+) provides.

  ## Fake

  `config :mob_audio_io, backend: MobAudioIo.Fake` makes this module run on
  the host: see `MobAudioIo.Fake`.
  """

  require Logger

  @typedoc "A stream id, as `open/3` returned it."
  @type stream :: pos_integer()
  @type direction :: :output | :input | :duplex
  @typedoc "An Android AAudio device id or an iOS port UID, as `devices/1` lists them."
  @type device_id :: integer() | String.t()
  @type preset :: :default | :generic | :performance | :unprocessed | :voice
  @type option ::
          {:processor, String.t()}
          | {:module, module()}
          | {:rate, pos_integer() | nil}
          | {:buffer, :low_latency | {:frames, pos_integer()}}
          | {:channels, 1 | 2}
          | {:input_channels, 1 | 2}
          | {:sharing, :shared | :exclusive}
          | {:session, :play | :mix | :play_and_record}
          | {:preset, preset()}
          | {:device, device_id() | nil}
          | {:input_device, device_id() | nil}

  @known_options [
    :processor,
    :module,
    :rate,
    :buffer,
    :channels,
    :input_channels,
    :sharing,
    :session,
    :preset,
    :device,
    :input_device
  ]
  @presets [:default, :generic, :performance, :unprocessed, :voice]

  @default_max_bytes 64 * 1024 * 1024
  @max_max_bytes 1024 * 1024 * 1024

  @doc """
  Open a stream for `owner` and start it.

  Options:

    * `:processor` (required): a registered processor's name.
    * `:module`: the app's NIF module that registers the processor; loaded
      (`Code.ensure_loaded/1`) before the stream opens.
    * `:rate`: frames per second; `nil` (default) for the device's own,
      which avoids resampling.
    * `:buffer`: `:low_latency` (default: two bursts on Android, a 5 ms IO
      buffer on iOS) or `{:frames, n}`.
    * `:channels`: output channels, 1 or 2 (default 2).
    * `:input_channels`: input channels, 1 (default) or 2.
    * `:sharing`: Android `:shared` (default) or `:exclusive` (falls back to
      shared when refused).
    * `:session`: iOS `:play`, `:mix` or `:play_and_record` (see
      "Sessions"); defaults to `:play` for output and `:play_and_record`
      with input. `:play` with input is an option error.
    * `:preset` (input and duplex): `:default` (Android's own,
      VOICE_RECOGNITION), `:generic`, `:performance` (Android
      VOICE_PERFORMANCE, low-latency singing), `:unprocessed` (no AGC or
      noise suppression where the device allows; iOS Measurement mode) or
      `:voice` (echo cancellation for calls: Android VOICE_COMMUNICATION, iOS
      VoiceChat + VoiceProcessingIO).
    * `:device`: a device from `devices/1` (the input's for `:input`, the
      output's otherwise); `nil` (default) for the system's choice.
    * `:input_device` (duplex): the input's device.

  The stream closes when `owner` exits.
  """
  @spec open(pid(), direction(), [option()]) :: {:ok, stream()} | {:error, term()}
  def open(owner, direction, opts) when is_pid(owner) do
    with :ok <- check_direction(direction),
         {:ok, opts} <- normalize_options(direction, opts),
         :ok <- load_module(opts.module),
         {:ok, stream} <- backend().open(owner, direction, opts) do
      Logger.info(
        "[mob_audio_io] opened stream #{stream}: #{inspect(direction)} #{opts.processor}"
      )

      {:ok, stream}
    else
      {:error, reason} = error ->
        Logger.warning("[mob_audio_io] open #{inspect(direction)} failed: #{inspect(reason)}")
        error
    end
  end

  @doc """
  Stop and close a stream. Posts still queued reach the owner, then
  `:stopped`. `{:error, :not_open}` for an unknown or closed id.
  """
  @spec close(stream()) :: :ok | {:error, :not_open | :unsupported}
  def close(stream) when is_integer(stream) do
    case backend().close(stream) do
      :ok ->
        Logger.info("[mob_audio_io] closed stream #{stream}")
        :ok

      error ->
        error
    end
  end

  @doc """
  Everything the plugin knows, as a plain map (for RPC and agents):

      %{platform: :android | :ios | :fake, abi: 1, processors: ["mai_silence", ...],
        streams: [stream_status], session: nil | %{...}}

  Each stream:

    * `id, processor, direction, running, interrupted, session, sharing`.
    * `rate`, `channels` (output), `input_channels`.
    * `burst`: the platform's own block (AAudio's burst, iOS's IO buffer);
      `cb_frames`: the frames the last callback actually had, with
      `cb_frames_min` / `cb_frames_max` since `reset_stats/1` (Android's
      legacy path and the iOS simulator call with other sizes than the
      burst).
    * `buffer` and `capacity` (frames).
    * `latency_ms`: output. iOS: the session's output latency plus one IO
      buffer. Android 12+: the frames written but not yet presented (AAudio
      timestamps). Android 8-11: only the frames written but not yet read by
      the device, because `AAudioStream_getTimestamp` can abort the process
      there (see `docs/VERIFICATION.md`). `input_latency_ms`: iOS's input
      latency plus one IO buffer; `nil` elsewhere.
    * `present`: where `mai_time.present_ns` comes from: `:timestamp` or
      `:estimate`.
    * `xruns` (underruns and overruns since open), `input_underflows`
      (duplex callbacks whose input came short and was zero-filled).
    * `callbacks`, `frames`, `cb_max_us` (the worst time in one callback:
      the processor and the plugin's bookkeeping), `cb_avg_us`,
      `gap_max_us` (the worst time between two callbacks' starts).
    * `in_peak_db` (since `reset_stats/1`) and `in_rms_db` (the last
      callback's block), measured by the plugin on every input; `nil` for
      output.
    * `preset`, `preset_granted` (Android's `getInputPreset`), `device_id`,
      `input_device_id`.
    * `reopens`, `posts_dropped`, `last_error`.

  `session` (iOS): `active, category, mode, rate, io_buffer_ms,
  preferred_io_buffer_ms, output_latency_ms, input_latency_ms,
  input_available, outputs, inputs`.
  """
  @spec status() :: map() | {:error, :unsupported}
  def status, do: backend().status()

  @doc "One stream's map from `status/0`."
  @spec status(stream()) :: map() | {:error, :not_open | :unsupported}
  def status(stream) when is_integer(stream), do: backend().stream_status(stream)

  @doc "Zero a stream's worst-case counters (`cb_max_us`, `gap_max_us`, `cb_frames_min/max`, `in_peak_db`), e.g. after its start."
  @spec reset_stats(stream()) :: :ok | {:error, :not_open | :unsupported}
  def reset_stats(stream) when is_integer(stream), do: backend().reset_stats(stream)

  @doc "Names of the registered processors."
  @spec processors() :: [String.t()] | {:error, :unsupported}
  def processors, do: backend().processors()

  @doc """
  The devices of a direction, in the platform's order:
  `[%{id: device_id, name: String.t(), kind: atom}]`.

  `kind`: `:builtin_mic`, `:builtin_speaker`, `:builtin_earpiece`,
  `:wired_headset`, `:wired_headphones`, `:bluetooth`, `:bluetooth_a2dp`,
  `:usb`, `:hdmi`, `:airplay`, `:car` or `:other`. Android lists every
  input or output `AudioManager` knows (ids for `:device`); iOS lists the
  session's available inputs (port UIDs for `:device`) and the current
  route's outputs (informational: iOS picks the output itself).
  """
  @spec devices(:input | :output) :: [map()] | {:error, :unsupported | term()}
  def devices(direction) when direction in [:input, :output] do
    case backend().devices(direction) do
      {:error, reason} = error ->
        Logger.warning("[mob_audio_io] devices #{inspect(direction)} failed: #{inspect(reason)}")
        error

      list ->
        list
    end
  end

  @doc """
  Decode an audio file to PCM: `{:ok, %{rate:, channels:, frames:, data:}}`,
  `data` being `frames * channels` interleaved 32-bit floats in the
  device's byte order (little-endian on every supported phone).

  WAV, MP3, AAC/M4A and whatever else the platform's decoders take. Runs on
  a dirty scheduler, so a long file doesn't hold up the BEAM's schedulers.

  Options:

    * `:max_bytes`: the most PCM to produce (default 64 MiB, about 2.9
      minutes of 48 kHz stereo; at most 1 GiB). A longer file answers
      `{:error, :too_large}` rather than a partial result.
    * `:mono`: fold every frame to one channel (default `false`).

  Errors: `:not_found`, `:unsupported` (the host, or a format the platform
  can't decode), `:no_audio`, `:too_large`, `:no_memory`, `:decode_error`,
  each possibly as `{reason, detail}` with the platform's words, and
  `{:invalid_option, key}`.
  """
  @spec decode(Path.t(), keyword()) :: {:ok, map()} | {:error, term()}
  def decode(path, opts \\ []) when is_binary(path) and is_list(opts) do
    with {:ok, max_bytes, mono} <- decode_options(opts),
         {:ok, pcm} <- backend().decode(Path.expand(path), max_bytes, mono) do
      Logger.info(
        "[mob_audio_io] decoded #{Path.basename(path)}: #{pcm.frames} frames, #{pcm.channels} ch at #{pcm.rate} Hz"
      )

      {:ok, pcm}
    else
      {:error, reason} = error ->
        Logger.warning("[mob_audio_io] decode #{Path.basename(path)} failed: #{inspect(reason)}")
        error
    end
  end

  @doc false
  @spec backend() :: module()
  def backend, do: Application.get_env(:mob_audio_io, :backend, MobAudioIo.Native)

  defp check_direction(direction) when direction in [:output, :input, :duplex], do: :ok
  defp check_direction(_), do: {:error, {:invalid_option, :direction}}

  defp load_module(nil), do: :ok

  defp load_module(module) do
    case Code.ensure_loaded(module) do
      {:module, _} -> :ok
      {:error, reason} -> {:error, {:module, reason}}
    end
  end

  @doc false
  # The options as the backends take them: %{processor, module, rate (0: the
  # device's), frames (0: low latency), channels, input_channels, sharing,
  # session, preset, device ("": the default), input_device}.
  @spec normalize_options(direction(), keyword()) ::
          {:ok, map()} | {:error, {:invalid_option, atom()}}
  def normalize_options(direction, opts) when is_list(opts) do
    input? = direction in [:input, :duplex]

    with :ok <- check_keys(opts),
         {:ok, processor} <- processor(opts[:processor]),
         {:ok, module} <- module(Keyword.get(opts, :module)),
         {:ok, rate} <- rate(Keyword.get(opts, :rate)),
         {:ok, frames} <- frames(Keyword.get(opts, :buffer, :low_latency)),
         {:ok, channels} <- one_of(:channels, Keyword.get(opts, :channels, 2), [1, 2]),
         {:ok, in_channels} <-
           one_of(:input_channels, Keyword.get(opts, :input_channels, 1), [1, 2]),
         {:ok, sharing} <-
           one_of(:sharing, Keyword.get(opts, :sharing, :shared), [:shared, :exclusive]),
         {:ok, session} <- session(Keyword.get(opts, :session), input?),
         {:ok, preset} <- preset(Keyword.get(opts, :preset), input?),
         {:ok, device} <- device(:device, Keyword.get(opts, :device)),
         {:ok, input_device} <- input_device(Keyword.get(opts, :input_device), direction) do
      {:ok,
       %{
         processor: processor,
         module: module,
         rate: rate,
         frames: frames,
         channels: channels,
         input_channels: in_channels,
         sharing: sharing,
         session: session,
         preset: preset,
         device: device,
         input_device: input_device
       }}
    end
  end

  def normalize_options(_direction, _), do: {:error, {:invalid_option, :opts}}

  defp check_keys(opts) do
    case Enum.find(opts, fn
           {key, _} -> key not in @known_options
           _ -> true
         end) do
      nil -> :ok
      {key, _} -> {:error, {:invalid_option, key}}
      _ -> {:error, {:invalid_option, :opts}}
    end
  end

  defp processor(name) when is_binary(name) and byte_size(name) in 1..47, do: {:ok, name}
  defp processor(_), do: {:error, {:invalid_option, :processor}}

  defp module(nil), do: {:ok, nil}
  defp module(module) when is_atom(module), do: {:ok, module}
  defp module(_), do: {:error, {:invalid_option, :module}}

  defp rate(nil), do: {:ok, 0}
  defp rate(rate) when is_integer(rate) and rate in 8_000..192_000, do: {:ok, rate}
  defp rate(_), do: {:error, {:invalid_option, :rate}}

  defp frames(:low_latency), do: {:ok, 0}
  defp frames({:frames, n}) when is_integer(n) and n in 16..16_384, do: {:ok, n}
  defp frames(_), do: {:error, {:invalid_option, :buffer}}

  defp session(nil, input?), do: {:ok, if(input?, do: :play_and_record, else: :play)}
  defp session(:play, true), do: {:error, {:invalid_option, :session}}
  defp session(session, _input?), do: one_of(:session, session, [:play, :mix, :play_and_record])

  defp preset(nil, _input?), do: {:ok, :default}
  defp preset(_preset, false), do: {:error, {:invalid_option, :preset}}
  defp preset(preset, true), do: one_of(:preset, preset, @presets)

  defp device(_key, nil), do: {:ok, ""}
  defp device(_key, id) when is_integer(id) and id > 0, do: {:ok, Integer.to_string(id)}
  defp device(_key, id) when is_binary(id) and byte_size(id) in 1..127, do: {:ok, id}
  defp device(key, _), do: {:error, {:invalid_option, key}}

  defp input_device(nil, _direction), do: {:ok, ""}
  defp input_device(id, :duplex), do: device(:input_device, id)
  defp input_device(_id, _direction), do: {:error, {:invalid_option, :input_device}}

  defp one_of(key, value, allowed) do
    if value in allowed, do: {:ok, value}, else: {:error, {:invalid_option, key}}
  end

  defp decode_options(opts) do
    case Enum.find(opts, fn {key, _} -> key not in [:max_bytes, :mono] end) do
      nil ->
        max_bytes = Keyword.get(opts, :max_bytes, @default_max_bytes)
        mono = Keyword.get(opts, :mono, false)

        cond do
          not (is_integer(max_bytes) and max_bytes in 1..@max_max_bytes) ->
            {:error, {:invalid_option, :max_bytes}}

          not is_boolean(mono) ->
            {:error, {:invalid_option, :mono}}

          true ->
            {:ok, max_bytes, mono}
        end

      {key, _} ->
        {:error, {:invalid_option, key}}
    end
  end
end
