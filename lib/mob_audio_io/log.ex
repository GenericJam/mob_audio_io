defmodule MobAudioIo.Log do
  @moduledoc """
  Logs what the native side reports, with a `[mob_audio_io]` prefix.

  Native code can't reach Logger (and so not Sloppy Joe's `logs`), so the
  NIF sends this process a copy of every stream message but `:post`, and
  of every decode message but the `:decoded` chunks. Started, unsupervised
  and unlinked, by the first `MobAudioIo.Native.open/3` or
  `decode_stream/3`; it lives for the app's life.
  """
  use GenServer

  require Logger

  @doc "Start the logger if needed and point the NIF at it."
  @spec attach() :: :ok
  def attach do
    pid =
      case GenServer.start(__MODULE__, nil, name: __MODULE__) do
        {:ok, pid} -> pid
        {:error, {:already_started, pid}} -> pid
      end

    :mob_audio_io_nif.set_logger(pid)
  end

  @impl true
  def init(nil), do: {:ok, nil}

  @impl true
  def handle_info({:mob_audio_io, kind, id, info}, state) do
    Logger.log(
      level(kind, info),
      "[mob_audio_io] #{subject(kind)} #{id} #{kind} #{inspect(info)}"
    )

    {:noreply, state}
  end

  def handle_info(_other, state), do: {:noreply, state}

  @doc false
  @spec level(atom(), term()) :: Logger.level()
  def level(kind, _info) when kind in [:error, :device_lost, :xrun, :interrupted, :decode_error],
    do: :warning

  def level(:stopped, %{posts_dropped: n}) when n > 0, do: :warning
  def level(_kind, _info), do: :info

  defp subject(kind)
       when kind in [:decode_started, :decode_done, :decode_cancelled, :decode_error],
       do: "decode"

  defp subject(_kind), do: "stream"
end
