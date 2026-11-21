defmodule MobAudioIo.Native do
  @moduledoc """
  The device backend: `:mob_audio_io_nif`, statically linked into the app.
  On the host, where it isn't linked, every call answers
  `{:error, :unsupported}`.
  """
  @behaviour MobAudioIo.Backend

  @impl true
  def open(owner, direction, opts) do
    native(fn ->
      MobAudioIo.Log.attach()

      :mob_audio_io_nif.open(
        owner,
        direction,
        opts.processor,
        opts.rate,
        opts.frames,
        opts.sharing,
        opts.session
      )
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
