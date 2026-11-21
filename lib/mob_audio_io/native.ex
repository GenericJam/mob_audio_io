defmodule MobAudioIo.Native do
  @moduledoc """
  The device backend: `:mob_audio_io_nif`, statically linked into the app.
  On the host, where it isn't linked, every call answers
  `{:error, :unsupported}`.
  """
  @behaviour MobAudioIo.Backend

  @native_keys [
    :rate,
    :frames,
    :channels,
    :input_channels,
    :sharing,
    :session,
    :preset,
    :device,
    :input_device
  ]

  @impl true
  def open(owner, direction, opts) do
    native(fn ->
      MobAudioIo.Log.attach()
      :mob_audio_io_nif.open(owner, direction, opts.processor, Map.take(opts, @native_keys))
    end)
  end

  @impl true
  def close(stream), do: native(fn -> :mob_audio_io_nif.close(stream) end)

  @impl true
  def status, do: native(&:mob_audio_io_nif.status/0)

  @impl true
  def stream_status(stream), do: native(fn -> :mob_audio_io_nif.stream_status(stream) end)

  @impl true
  def reset_stats(stream), do: native(fn -> :mob_audio_io_nif.reset_stats(stream) end)

  @impl true
  def processors, do: native(&:mob_audio_io_nif.processors/0)

  @impl true
  def devices(direction), do: native(fn -> :mob_audio_io_nif.devices(direction) end)

  @impl true
  def decode_stream(path, owner, opts) do
    native(fn ->
      MobAudioIo.Log.attach()
      :mob_audio_io_nif.decode_stream(path, owner, Map.delete(opts, :module))
    end)
  end

  @impl true
  def decode_ack(decode, n), do: native(fn -> :mob_audio_io_nif.decode_ack(decode, n) end)

  @impl true
  def decode_cancel(decode), do: native(fn -> :mob_audio_io_nif.decode_cancel(decode) end)

  @doc "Whether the NIF is linked into this build."
  @spec available?() :: boolean()
  def available? do
    is_integer(:mob_audio_io_nif.abi_version())
  catch
    :error, :nif_not_loaded -> false
  end

  defp native(fun) do
    fun.()
  catch
    :error, :nif_not_loaded -> {:error, :unsupported}
  end
end
