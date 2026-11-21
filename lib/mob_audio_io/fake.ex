defmodule MobAudioIo.Fake do
  @moduledoc """
  A host backend for tests: `config :mob_audio_io, backend: MobAudioIo.Fake`.

  It keeps streams in an Agent and answers like the device does: `open/3`
  sends the owner `{:mob_audio_io, :started, stream, info}`, `close/1` sends
  `:stopped`, and `status/0` reports a 48 kHz stream with 192-frame bursts
  whose `callbacks` and `frames` grow with wall time, so
  `MobAudioIo.SelfTest`-style checks work. Its processors are the built-ins
  plus whatever `register_processor/1` added.

  To drive a screen through what the hardware would do, `inject/3` sends
  the stream's owner any message (`:device_lost`, `:interrupted`, `:xrun`,
  `:post`, ...), as `Mob.Test.send_message/2` would.

  State is global (one Agent): use it from `async: false` tests and call
  `reset/0` in `setup`.
  """
  @behaviour MobAudioIo.Backend

  use Agent

  @rate 48_000
  @burst 192
  @builtins ["mai_silence", "mai_sine"]

  defp initial, do: %{streams: %{}, next: 1, processors: @builtins, fail_next: nil}

  @doc "Back to no streams, the built-in processors and no queued failure."
  @spec reset() :: :ok
  def reset, do: update(fn _ -> initial() end)

  @doc "Make `name` openable, as an app's `mai_register` would."
  @spec register_processor(String.t()) :: :ok
  def register_processor(name) when is_binary(name) do
    update(fn s -> %{s | processors: Enum.uniq(s.processors ++ [name])} end)
  end

  @doc "The next `open/3` answers `{:error, reason}`."
  @spec fail_next_open(term()) :: :ok
  def fail_next_open(reason), do: update(&%{&1 | fail_next: reason})

  @doc "Send the stream's owner `{:mob_audio_io, kind, stream, payload}`."
  @spec inject(MobAudioIo.stream(), atom(), term()) :: :ok | {:error, :not_open}
  def inject(stream, kind, payload) when is_atom(kind) do
    case get(&Map.get(&1.streams, stream)) do
      nil ->
        {:error, :not_open}

      %{owner: owner} ->
        send(owner, {:mob_audio_io, kind, stream, payload})
        :ok
    end
  end

  @impl true
  def open(owner, direction, opts) do
    ensure_started()

    Agent.get_and_update(__MODULE__, fn s ->
      cond do
        s.fail_next != nil -> {{:error, s.fail_next}, %{s | fail_next: nil}}
        direction != :output -> {{:error, :unsupported}, s}
        opts.processor not in s.processors -> {{:error, :unknown_processor}, s}
        map_size(s.streams) >= 8 -> {{:error, :busy}, s}
        true -> add_stream(s, owner, opts)
      end
    end)
    |> tap(fn
      {:ok, stream} -> send(owner, {:mob_audio_io, :started, stream, started_info(opts)})
      _ -> :ok
    end)
  end

  defp add_stream(s, owner, opts) do
    stream = s.next
    entry = %{owner: owner, opts: opts, opened_at: System.monotonic_time(:microsecond)}
    {{:ok, stream}, %{s | streams: Map.put(s.streams, stream, entry), next: stream + 1}}
  end

  defp started_info(opts) do
    %{
      rate: rate(opts),
      channels: 2,
      burst: @burst,
      buffer: buffer(opts),
      sharing: :shared,
      reopened: false
    }
  end

  @impl true
  def close(stream) do
    ensure_started()

    Agent.get_and_update(__MODULE__, fn s ->
      case Map.pop(s.streams, stream) do
        {nil, _} -> {{:error, :not_open}, s}
        {entry, rest} -> {{:ok, entry}, %{s | streams: rest}}
      end
    end)
    |> case do
      {:ok, %{owner: owner}} ->
        send(owner, {:mob_audio_io, :stopped, stream, %{reason: :closed}})
        :ok

      error ->
        error
    end
  end

  @impl true
  def status do
    s = get(& &1)
    streams = s.streams |> Enum.sort() |> Enum.map(fn {id, e} -> stream_map(id, e) end)
    %{platform: :fake, abi: 1, processors: s.processors, streams: streams, session: nil}
  end

  @impl true
  def stream_status(stream) do
    case get(&Map.get(&1.streams, stream)) do
      nil -> {:error, :not_open}
      entry -> stream_map(stream, entry)
    end
  end

  @impl true
  def reset_stats(stream) do
    if get(&Map.has_key?(&1.streams, stream)), do: :ok, else: {:error, :not_open}
  end

  @impl true
  def processors, do: get(& &1.processors)

  defp stream_map(id, %{opts: opts, opened_at: t0}) do
    elapsed_us = System.monotonic_time(:microsecond) - t0
    rate = rate(opts)
    callbacks = div(elapsed_us * rate, @burst * 1_000_000)

    %{
      id: id,
      processor: opts.processor,
      direction: :output,
      running: true,
      interrupted: false,
      rate: rate,
      channels: 2,
      burst: @burst,
      buffer: buffer(opts),
      capacity: 4096,
      sharing: :shared,
      device_id: 0,
      latency_ms: 8.0,
      xruns: 0,
      callbacks: callbacks,
      frames: callbacks * @burst,
      cb_max_us: 0.0,
      cb_avg_us: 0.0,
      gap_max_us: @burst * 1_000_000 / rate,
      cb_frames: {@burst, @burst},
      reopens: 0,
      posts_dropped: 0,
      last_error: nil
    }
  end

  defp rate(%{rate: 0}), do: @rate
  defp rate(%{rate: rate}), do: rate

  defp buffer(%{frames: 0}), do: 2 * @burst
  defp buffer(%{frames: n}), do: n

  defp get(fun) do
    ensure_started()
    Agent.get(__MODULE__, fun)
  end

  defp update(fun) do
    ensure_started()
    Agent.update(__MODULE__, fun)
  end

  defp ensure_started do
    case Agent.start(&initial/0, name: __MODULE__) do
      {:ok, _} -> :ok
      {:error, {:already_started, _}} -> :ok
    end
  end
end
