defmodule MobAudioIo.SelfTest do
  @moduledoc """
  The plugin's on-device proof (`Mob.Plugin.SelfTest`), run by
  `mix mob.selftest` and mob_ci.

  1. **Output**: opens a real output stream on the built-in `"mai_silence"`
     processor (nothing audible) through the NIF, waits 150 ms, and reads
     the stream's callback count from `:mob_audio_io_nif.stream_status/1`.
     The platform's audio thread calling the plugin is the answer.
  2. **Input**, if the microphone is permitted: the same with an input
     stream. Without the permission (`{:error, :permission}`) this part is
     left out and the result rests on the output; the runner grants the
     manifest's permissions on emulators and simulators, so CI covers it.

  Outcomes: `:pass` when every stream that opened had callbacks; a failure
  naming the direction when a stream opened but no callback ran, when an
  open answered another error, or when the NIF isn't linked
  (`{:error, :unsupported}`).

  Streams open with `session: :mix`, so music another app plays isn't
  paused. Each is closed before `run/1` returns and its messages are
  flushed.
  """
  @behaviour Mob.Plugin.SelfTest

  @window_ms 150

  @impl true
  def run(_ctx) do
    with :pass <- check(:output),
         result when result in [:pass, :not_permitted] <- check(:input) do
      :pass
    end
  end

  defp check(direction) do
    {:ok, opts} =
      MobAudioIo.normalize_options(direction, processor: "mai_silence", session: :mix)

    case MobAudioIo.Native.open(self(), direction, opts) do
      {:ok, stream} ->
        before = callbacks(stream)
        Process.sleep(@window_ms)
        later = callbacks(stream)
        MobAudioIo.Native.close(stream)
        flush(stream)
        classify(direction, before, later)

      {:error, :permission} when direction == :input ->
        :not_permitted

      {:error, :unsupported} when direction == :output ->
        {:fail, "mob_audio_io_nif is not linked into this build (open answered :unsupported)"}

      {:error, reason} ->
        {:fail, "open(#{inspect(direction)}, \"mai_silence\") failed: #{inspect(reason)}"}
    end
  end

  @doc false
  # The callback counts at the window's start and end.
  @spec classify(MobAudioIo.direction(), term(), term()) :: Mob.Plugin.SelfTest.result()
  def classify(_direction, before, later)
      when is_integer(before) and is_integer(later) and later > before,
      do: :pass

  def classify(direction, before, later) when is_integer(before) and is_integer(later),
    do:
      {:fail,
       "#{direction}: no audio callback in #{@window_ms} ms (callbacks #{before} -> #{later})"}

  def classify(direction, before, later),
    do:
      {:fail, "#{direction}: stream_status/1 gave no callback count: #{inspect({before, later})}"}

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
