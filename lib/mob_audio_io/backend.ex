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

  `decode_opts` is `MobAudioIo`'s normalized decode options: `%{mono:
  boolean(), chunk_frames: pos_integer(), window: pos_integer(), max_ms:
  non_neg_integer() (0: no limit), processor: String.t() ("": the owner
  gets the chunks), module: module() | nil}`. A decode sends its owner the
  messages `MobAudioIo`'s "Decoding files" describes.
  """

  @callback open(owner :: pid(), MobAudioIo.direction(), opts :: map()) ::
              {:ok, MobAudioIo.stream()} | {:error, term()}
  @callback close(MobAudioIo.stream()) :: :ok | {:error, :not_open | :unsupported}
  @callback status() :: map() | {:error, :unsupported}
  @callback stream_status(MobAudioIo.stream()) :: map() | {:error, :not_open | :unsupported}
  @callback reset_stats(MobAudioIo.stream()) :: :ok | {:error, :not_open | :unsupported}
  @callback processors() :: [String.t()] | {:error, :unsupported}
  @callback devices(:input | :output) :: [map()] | {:error, term()}
  @callback decode_stream(path :: String.t(), owner :: pid(), decode_opts :: map()) ::
              {:ok, MobAudioIo.decode()} | {:error, term()}
  @callback decode_ack(MobAudioIo.decode(), pos_integer()) ::
              :ok | {:error, :not_running | :unsupported}
  @callback decode_cancel(MobAudioIo.decode()) :: :ok | {:error, :not_running | :unsupported}
end
