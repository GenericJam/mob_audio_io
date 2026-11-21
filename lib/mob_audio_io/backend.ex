defmodule MobAudioIo.Backend do
  @moduledoc """
  What `MobAudioIo` calls: `MobAudioIo.Native` on a device, `MobAudioIo.Fake`
  in host tests (`config :mob_audio_io, backend: MobAudioIo.Fake`).

  `opts` is `MobAudioIo`'s normalized options: `%{processor: String.t(),
  rate: non_neg_integer() (0: the device's), frames: non_neg_integer() (0:
  low latency), sharing: :shared | :exclusive, session: :play | :mix |
  :play_and_record}`.
  """

  @callback open(owner :: pid(), MobAudioIo.direction(), opts :: map()) ::
              {:ok, MobAudioIo.stream()} | {:error, term()}
  @callback close(MobAudioIo.stream()) :: :ok | {:error, :not_open | :unsupported}
  @callback status() :: map() | {:error, :unsupported}
  @callback stream_status(MobAudioIo.stream()) :: map() | {:error, :not_open | :unsupported}
  @callback reset_stats(MobAudioIo.stream()) :: :ok | {:error, :not_open | :unsupported}
  @callback processors() :: [String.t()] | {:error, :unsupported}
end
