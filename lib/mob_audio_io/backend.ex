defmodule MobAudioIo.Backend do
  @moduledoc """
  What `MobAudioIo` calls: `MobAudioIo.Native` on a device, `MobAudioIo.Fake`
  in host tests (`config :mob_audio_io, backend: MobAudioIo.Fake`).

  `opts` is `MobAudioIo`'s normalized options: `%{processor: String.t(),
  module: module() | nil, rate: non_neg_integer() (0: the device's), frames:
  non_neg_integer() (0: low latency), channels: 1 | 2, input_channels: 1 |
  2, sharing: :shared | :exclusive, session: :play | :mix |
  :play_and_record, preset: MobAudioIo.preset(), device: String.t() ("":
  the default), input_device: String.t()}`.
  """

  @callback open(owner :: pid(), MobAudioIo.direction(), opts :: map()) ::
              {:ok, MobAudioIo.stream()} | {:error, term()}
  @callback close(MobAudioIo.stream()) :: :ok | {:error, :not_open | :unsupported}
  @callback status() :: map() | {:error, :unsupported}
  @callback stream_status(MobAudioIo.stream()) :: map() | {:error, :not_open | :unsupported}
  @callback reset_stats(MobAudioIo.stream()) :: :ok | {:error, :not_open | :unsupported}
  @callback processors() :: [String.t()] | {:error, :unsupported}
  @callback devices(:input | :output) :: [map()] | {:error, term()}
  @callback decode(path :: String.t(), max_bytes :: pos_integer(), mono :: boolean()) ::
              {:ok, map()} | {:error, term()}
end
