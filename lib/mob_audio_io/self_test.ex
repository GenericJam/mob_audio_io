defmodule MobAudioIo.SelfTest do
  @moduledoc """
  The plugin's on-device proof (`Mob.Plugin.SelfTest`), run by
  `mix mob.selftest` and mob_ci.

  Opens a real output stream on the built-in `"mai_silence"` processor
  (nothing audible) through the NIF, waits 100 ms, reads the stream's
  callback count from `:mob_audio_io_nif.stream_status/1`, and closes it.
  The platform's audio thread calling the plugin is the answer:

    * the count rose: `:pass`.
    * it didn't: a failure (the stream opened but no callback ran).
    * `open` answered `{:error, reason}` (no AAudio, the session refused):
      a failure naming the reason.
    * the NIF isn't linked (`{:error, :unsupported}`): a failure.

  It opens with `session: :mix`, so music another app plays isn't paused.
  The stream is closed before `run/1` returns and its messages are flushed.
  """
  @behaviour Mob.Plugin.SelfTest

  @window_ms 100

  @impl true
  def run(_ctx) do
    opts = %{processor: "mai_silence", rate: 0, frames: 0, sharing: :shared, session: :mix}

    case MobAudioIo.Native.open(self(), :output, opts) do
      {:ok, stream} ->
        before = callbacks(stream)
        Process.sleep(@window_ms)
        later = callbacks(stream)
        MobAudioIo.Native.close(stream)
        flush(stream)
        classify(before, later)

      {:error, :unsupported} ->
        {:fail, "mob_audio_io_nif is not linked into this build (open answered :unsupported)"}

      {:error, reason} ->
        {:fail, "open(:output, \"mai_silence\") failed: #{inspect(reason)}"}
    end
  end

  @doc false
  # The callback counts at the window's start and end.
  @spec classify(term(), term()) :: Mob.Plugin.SelfTest.result()
  def classify(before, later) when is_integer(before) and is_integer(later) and later > before,
    do: :pass

  def classify(before, later) when is_integer(before) and is_integer(later),
    do: {:fail, "no audio callback in #{@window_ms} ms (callbacks #{before} -> #{later})"}

  def classify(before, later),
    do: {:fail, "stream_status/1 gave no callback count: #{inspect({before, later})}"}

  defp callbacks(stream) do
    case MobAudioIo.Native.stream_status(stream) do
      %{callbacks: n} -> n
      other -> other
    end
  end

  defp flush(stream) do
    receive do
      {:mob_audio_io, _kind, ^stream, _} -> flush(stream)
    after
      0 -> :ok
    end
  end
end
