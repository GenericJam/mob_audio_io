defmodule MobAudioIo do
  @moduledoc """
  Low-latency audio streams for Mob apps, with the DSP in the app's own C.

  The plugin owns the platform's audio stream: AAudio on Android, a RemoteIO
  unit and the `AVAudioSession` on iOS. An app's static NIF registers a
  processor through the C interface in `priv/include/mob_audio_io.h`
  (`mai_register/3`), and the plugin calls it from the audio thread. Audio
  never passes through the BEAM; Elixir opens and closes streams, reads
  `status/0` and receives the stream's messages.

  **Spike (0.1, unreleased):** output streams only. `:input` and `:duplex`
  answer `{:error, :unsupported}`; the device list, input presets and file
  decoding of the full plugin aren't here yet. See `SPIKE.md`.

  ## Platforms

  | Platform | Output | Input | Notes |
  |--|--|--|--|
  | Android (API 26+) | AAudio, low latency, shared (or exclusive) | not yet | `libaaudio.so` loaded at runtime; below API 26 `open/3` answers `{:error, :unavailable}` |
  | iOS | RemoteIO + `AVAudioSession` | not yet | interruptions, route changes and media-services resets handled |
  | Host (`mix test`, dev) | `{:error, :unsupported}` | `{:error, :unsupported}` | use `MobAudioIo.Fake` |

  ## Processors

  Two are built in: `"mai_silence"` (what `MobAudioIo.SelfTest` opens) and
  `"mai_sine"` (440 Hz at -18 dBFS, for a listening check). Everything else
  comes from the app's C, by name: `processors/0` lists what's registered.

  ## Messages

  The owner process receives, as `{:mob_audio_io, kind, stream, payload}`:

    * `:started`, `%{rate:, channels:, burst:, buffer:, sharing:, reopened:}`:
      the stream runs (again, after a reopen, with `reopened: true`).
    * `:stopped`, `%{reason: :closed | :device_lost | :interrupted}`.
    * `:device_lost`, `%{}`: Android, the output went away (headphones,
      USB). The plugin reopens on the new route; `:started` follows.
    * `:interrupted`, `%{}` and `:resumed`, `%{}`: iOS session
      interruptions (a call, Siri, another app). After `:resumed` the stream
      reopens and `:started` follows.
    * `:route_changed`, `%{reason: :new_route | :new_rate | :media_reset}`:
      a new output route. The stream is rebuilt for a new rate or a
      media-services reset.
    * `:xrun`, `%{xruns:, buffer:}`: underruns since open; on Android the
      buffer grows by a burst (up to its capacity) unless a fixed size was
      asked for.
    * `:error`, `%{reason:, detail:, while:}`: a reopen failed; it is tried
      again every 500 ms.
    * `:post`, `binary`: a message the processor queued with `mai_post`.

  A screen can keep these in assigns; `Mob.Test.send_message/2` stands in
  for them in tests. Every one but `:post` is also logged with a
  `[mob_audio_io]` prefix.

  ## Errors

  `open/3` answers `{:error, reason}` with `:unsupported` (this platform or
  direction), `:unknown_processor`, `:busy` (8 streams open),
  `:unavailable` (the platform refused the stream; the spike doesn't
  return the platform's detail for a failed open yet),
  `:session` (iOS couldn't activate the audio session), or
  `{:invalid_option, key}`.

  ## Fake

  `config :mob_audio_io, backend: MobAudioIo.Fake` makes this module run on
  the host: see `MobAudioIo.Fake`.
  """

  require Logger

  @typedoc "A stream id, as `open/3` returned it."
  @type stream :: pos_integer()
  @type direction :: :output | :input | :duplex
  @type option ::
          {:processor, String.t()}
          | {:rate, pos_integer() | nil}
          | {:buffer, :low_latency | {:frames, pos_integer()}}
          | {:sharing, :shared | :exclusive}
          | {:session, :play | :mix | :play_and_record}

  @known_options [:processor, :rate, :buffer, :sharing, :session]

  @doc """
  Open a stream for `owner` and start it.

  Options:

    * `:processor` (required): a registered processor's name.
    * `:rate`: frames per second; `nil` (default) for the device's own,
      which avoids resampling.
    * `:buffer`: `:low_latency` (default: two bursts on Android, a 5 ms IO
      buffer on iOS) or `{:frames, n}`.
    * `:sharing`: Android `:shared` (default) or `:exclusive` (falls back to
      shared when refused).
    * `:session`: iOS `:play` (default; the silent switch doesn't mute it),
      `:mix` (plays beside other apps) or `:play_and_record`.

  The stream closes when `owner` exits.
  """
  @spec open(pid(), direction(), [option()]) :: {:ok, stream()} | {:error, term()}
  def open(owner, direction, opts) when is_pid(owner) do
    with :ok <- check_direction(direction),
         {:ok, opts} <- normalize_options(opts),
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

  @doc "Stop and close a stream. `{:error, :not_open}` for an unknown or closed id."
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

      %{platform: :android | :ios, abi: 1, processors: ["mai_silence", ...],
        streams: [stream_status], session: nil | %{...}}

  Each stream: `id, processor, direction, running, interrupted, rate,
  channels, burst, buffer, capacity, sharing, device_id, latency_ms, xruns,
  callbacks, frames, cb_max_us, cb_avg_us, gap_max_us, cb_frames ({min,
  max}), reopens, posts_dropped, last_error`. `cb_max_us` is the worst time
  in one callback (the processor's render and the plugin's bookkeeping),
  `gap_max_us` the worst time between two callbacks' starts. `latency_ms`:
  iOS, the session's output latency plus one IO buffer; Android 12+, the
  frames written but not yet presented (AAudio timestamps); Android 8-11,
  only the frames written but not yet read by the device, because
  `AAudioStream_getTimestamp` can abort the process there (see `SPIKE.md`).

  `session` (iOS): `active, category, rate, io_buffer_ms,
  preferred_io_buffer_ms, output_latency_ms, input_latency_ms,
  input_available, outputs`.
  """
  @spec status() :: map() | {:error, :unsupported}
  def status, do: backend().status()

  @doc "One stream's map from `status/0`."
  @spec status(stream()) :: map() | {:error, :not_open | :unsupported}
  def status(stream) when is_integer(stream), do: backend().stream_status(stream)

  @doc "Zero a stream's worst-case counters (`cb_max_us`, `gap_max_us`, `cb_frames`), e.g. after its start."
  @spec reset_stats(stream()) :: :ok | {:error, :not_open | :unsupported}
  def reset_stats(stream) when is_integer(stream), do: backend().reset_stats(stream)

  @doc "Names of the registered processors."
  @spec processors() :: [String.t()] | {:error, :unsupported}
  def processors, do: backend().processors()

  @doc false
  @spec backend() :: module()
  def backend, do: Application.get_env(:mob_audio_io, :backend, MobAudioIo.Native)

  defp check_direction(direction) when direction in [:output, :input, :duplex], do: :ok
  defp check_direction(_), do: {:error, {:invalid_option, :direction}}

  @doc false
  # The options as the backends take them: %{processor, rate (0: device),
  # frames (0: low latency), sharing, session}.
  @spec normalize_options(keyword()) :: {:ok, map()} | {:error, {:invalid_option, atom()}}
  def normalize_options(opts) when is_list(opts) do
    with :ok <- check_keys(opts),
         {:ok, processor} <- processor(opts[:processor]),
         {:ok, rate} <- rate(Keyword.get(opts, :rate)),
         {:ok, frames} <- frames(Keyword.get(opts, :buffer, :low_latency)),
         {:ok, sharing} <-
           one_of(:sharing, Keyword.get(opts, :sharing, :shared), [:shared, :exclusive]),
         {:ok, session} <-
           one_of(:session, Keyword.get(opts, :session, :play), [:play, :mix, :play_and_record]) do
      {:ok,
       %{processor: processor, rate: rate, frames: frames, sharing: sharing, session: session}}
    end
  end

  def normalize_options(_), do: {:error, {:invalid_option, :opts}}

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

  defp rate(nil), do: {:ok, 0}
  defp rate(rate) when is_integer(rate) and rate in 8_000..192_000, do: {:ok, rate}
  defp rate(_), do: {:error, {:invalid_option, :rate}}

  defp frames(:low_latency), do: {:ok, 0}
  defp frames({:frames, n}) when is_integer(n) and n in 16..16_384, do: {:ok, n}
  defp frames(_), do: {:error, {:invalid_option, :buffer}}

  defp one_of(key, value, allowed) do
    if value in allowed, do: {:ok, value}, else: {:error, {:invalid_option, key}}
  end
end
