defmodule MobAudioIo.Fake do
  @moduledoc """
  A host backend for tests: `config :mob_audio_io, backend: MobAudioIo.Fake`.

  It keeps streams in an Agent and answers like a device does:

    * `open/3` sends the owner `{:mob_audio_io, :started, stream, info}` and
      `close/1` sends `:stopped`. Output, input and duplex all open; input
      and duplex answer `{:error, :permission}` after `deny_permission/0`.
    * `status/0` reports 48 kHz streams with 192-frame callbacks whose
      `callbacks` and `frames` grow with wall time, so
      `MobAudioIo.SelfTest`-style checks work; inputs report the level set
      with `set_input_level/2`.
    * `devices/1` lists a built-in mic and speaker unless `set_devices/2`
      replaced them.
    * `decode/3` decodes WAV files (16-, 24- and 32-bit PCM, 32-bit float)
      in Elixir, with the device's size cap, mono folding and errors.
    * Processors are the built-ins plus whatever `register_processor/1`
      added.

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
  @default_devices %{
    input: [%{id: 1, name: "Built-in microphone", kind: :builtin_mic}],
    output: [%{id: 2, name: "Built-in speaker", kind: :builtin_speaker}]
  }

  defp initial do
    %{
      streams: %{},
      next: 1,
      processors: @builtins,
      fail_next: nil,
      permission: true,
      devices: @default_devices,
      levels: %{}
    }
  end

  @doc "Back to no streams, the built-in processors and devices, the permission granted, no queued failure."
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

  @doc "Input and duplex opens answer `{:error, :permission}` until `reset/0`."
  @spec deny_permission() :: :ok
  def deny_permission, do: update(&%{&1 | permission: false})

  @doc "What `devices(direction)` lists."
  @spec set_devices(:input | :output, [map()]) :: :ok
  def set_devices(direction, list) when direction in [:input, :output] and is_list(list) do
    update(fn s -> %{s | devices: Map.put(s.devices, direction, list)} end)
  end

  @doc "The input level (dBFS) an input or duplex stream's status reports."
  @spec set_input_level(MobAudioIo.stream(), float()) :: :ok
  def set_input_level(stream, db) when is_number(db) do
    update(fn s -> %{s | levels: Map.put(s.levels, stream, db / 1)} end)
  end

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
        direction != :output and not s.permission -> {{:error, :permission}, s}
        opts.processor not in s.processors -> {{:error, :unknown_processor}, s}
        map_size(s.streams) >= 8 -> {{:error, :busy}, s}
        true -> add_stream(s, owner, direction, opts)
      end
    end)
    |> tap(fn
      {:ok, stream} ->
        send(owner, {:mob_audio_io, :started, stream, started_info(direction, opts)})

      _ ->
        :ok
    end)
  end

  defp add_stream(s, owner, direction, opts) do
    stream = s.next

    entry = %{
      owner: owner,
      direction: direction,
      opts: opts,
      opened_at: System.monotonic_time(:microsecond)
    }

    {{:ok, stream}, %{s | streams: Map.put(s.streams, stream, entry), next: stream + 1}}
  end

  defp started_info(direction, opts) do
    %{
      rate: rate(opts),
      channels: if(direction == :input, do: 0, else: opts.channels),
      input_channels: if(direction == :output, do: 0, else: opts.input_channels),
      burst: @burst,
      buffer: buffer(opts),
      sharing: :shared,
      direction: direction,
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
        send(owner, {:mob_audio_io, :stopped, stream, %{reason: :closed, posts_dropped: 0}})
        :ok

      error ->
        error
    end
  end

  @impl true
  def status do
    s = get(& &1)

    streams =
      s.streams |> Enum.sort() |> Enum.map(fn {id, e} -> stream_map(id, e, s.levels[id]) end)

    %{platform: :fake, abi: 1, processors: s.processors, streams: streams, session: nil}
  end

  @impl true
  def stream_status(stream) do
    case get(&{Map.get(&1.streams, stream), &1.levels[stream]}) do
      {nil, _} -> {:error, :not_open}
      {entry, level} -> stream_map(stream, entry, level)
    end
  end

  @impl true
  def reset_stats(stream) do
    if get(&Map.has_key?(&1.streams, stream)), do: :ok, else: {:error, :not_open}
  end

  @impl true
  def processors, do: get(& &1.processors)

  @impl true
  def devices(direction), do: get(&Map.fetch!(&1.devices, direction))

  @impl true
  def decode(path, max_bytes, mono) do
    with {:ok, bin} <- read(path),
         {:ok, fmt, data} <- MobAudioIo.Fake.Wav.parse(bin) do
      MobAudioIo.Fake.Wav.to_f32(fmt, data, max_bytes, mono)
    end
  end

  defp read(path) do
    case File.read(path) do
      {:ok, bin} -> {:ok, bin}
      {:error, _} -> {:error, :not_found}
    end
  end

  defp stream_map(id, %{direction: direction, opts: opts, opened_at: t0}, level) do
    elapsed_us = System.monotonic_time(:microsecond) - t0
    rate = rate(opts)
    callbacks = div(elapsed_us * rate, @burst * 1_000_000)
    input? = direction != :output
    output? = direction != :input
    db = level || -200.0

    %{
      id: id,
      processor: opts.processor,
      direction: direction,
      running: true,
      interrupted: false,
      rate: rate,
      channels: if(output?, do: opts.channels, else: 0),
      input_channels: if(input?, do: opts.input_channels, else: 0),
      burst: @burst,
      buffer: buffer(opts),
      capacity: 4096,
      sharing: :shared,
      session: opts.session,
      preset: if(input?, do: opts.preset),
      preset_granted: if(input?, do: 0),
      device_id: 0,
      input_device_id: 0,
      latency_ms: if(output?, do: 8.0),
      input_latency_ms: if(input?, do: 4.0),
      present: :estimate,
      xruns: 0,
      input_underflows: 0,
      callbacks: callbacks,
      frames: callbacks * @burst,
      cb_frames: @burst,
      cb_frames_min: @burst,
      cb_frames_max: @burst,
      cb_max_us: 0.0,
      cb_avg_us: 0.0,
      gap_max_us: @burst * 1_000_000 / rate,
      in_peak_db: if(input?, do: db),
      in_rms_db: if(input?, do: db),
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
