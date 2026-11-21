defmodule AudioIoSpike.Probe do
  @moduledoc """
  The measurements behind `docs/VERIFICATION.md`, run over RPC
  (`scripts/rpc.exs`). The calling process owns each stream, so it gets the
  stream's messages. Every function opens, measures, closes and returns a
  plain map.
  """

  @doc """
  Output: open `processor`, settle 1 s, zero the worst-case counters, run
  `seconds`, read `MobAudioIo.status/1`, close.
  """
  @spec output(String.t(), pos_integer(), keyword()) :: map()
  def output(processor, seconds, opts \\ []) do
    with_stream(:output, [processor: processor, module: :spike_dsp] ++ opts, fn stream ->
      settle(stream)
      start = MobAudioIo.status(stream)
      if processor == "magpie_sampler", do: :spike_dsp.count_in(4 * seconds, 240.0)
      Process.sleep(seconds * 1_000)
      status = MobAudioIo.status(stream)
      %{status: status, callbacks_in_window: status.callbacks - start.callbacks}
    end)
  end

  @doc """
  Input: listen on `tone_loop` for `seconds`, sampling the plugin's meter
  and the input's pitch once a second.
  """
  @spec input(pos_integer(), keyword()) :: map()
  def input(seconds, opts \\ []) do
    with_stream(:input, [processor: "tone_loop", module: :spike_dsp] ++ opts, fn stream ->
      settle(stream)

      series =
        for _ <- 1..seconds do
          Process.sleep(1_000)
          s = MobAudioIo.status(stream)
          p = :spike_dsp.pitch(250)
          %{rms_db: r(s.in_rms_db), peak_db: r(s.in_peak_db), hz: r(p.hz), clarity: r(p.clarity)}
        end

      %{status: MobAudioIo.status(stream), levels: summarize(series)}
    end)
  end

  @doc """
  A known tone through the speaker and back through the microphone (duplex
  `tone_loop` at `hz`): the pitch heard, twice a second after a 1 s settle.
  """
  @spec tone(pos_integer(), number(), keyword()) :: map()
  def tone(seconds, hz \\ 1_000.0, opts \\ []) do
    :spike_dsp.tone(hz / 1)

    with_stream(:duplex, [processor: "tone_loop", module: :spike_dsp] ++ opts, fn stream ->
      settle(stream)

      heard =
        for _ <- 1..(seconds * 2) do
          Process.sleep(500)
          :spike_dsp.pitch(250)
        end

      found = Enum.filter(heard, &(&1.hz > 0 and &1.clarity > 0.8))
      hzs = Enum.map(found, & &1.hz)

      %{
        played_hz: hz,
        heard_hz_median: median(hzs),
        heard_hz_min: hzs |> Enum.min(fn -> nil end) |> r(),
        heard_hz_max: hzs |> Enum.max(fn -> nil end) |> r(),
        windows: length(heard),
        windows_with_pitch: length(found),
        rms_db_median: heard |> Enum.map(& &1.rms_db) |> median(),
        status: MobAudioIo.status(stream)
      }
    end)
  end

  @doc """
  The acoustic round trip (duplex `loop_latency`): a burst a second, its
  arrival in the input on the stream clock, for `seconds`.
  """
  @spec loopback(pos_integer(), keyword()) :: map()
  def loopback(seconds, opts \\ []) do
    with_stream(:duplex, [processor: "loop_latency", module: :spike_dsp] ++ opts, fn stream ->
      settle(stream)
      Process.sleep(seconds * 1_000)
      status = MobAudioIo.status(stream)
      rate = status.rate
      posts = for {:mob_audio_io, :post, ^stream, bin} <- drain(stream), do: decode_post(bin)
      heard = Enum.filter(posts, &(&1.heard > 0))
      frames = Enum.map(heard, &(&1.heard - &1.click))
      by_present = for p <- heard, p.click_ns > 0 and p.heard_ns > 0, do: (p.heard_ns - p.click_ns) / 1.0e6

      %{
        bursts: length(posts),
        heard: length(heard),
        round_trip_ms_median: median(Enum.map(frames, &(&1 * 1_000 / rate))),
        round_trip_ms_min: frames |> Enum.min(fn -> nil end) |> ms(rate),
        round_trip_ms_max: frames |> Enum.max(fn -> nil end) |> ms(rate),
        present_gap_ms_median: median(by_present),
        status: status
      }
    end)
  end

  @doc "Decode the bundled files (priv/audio) and a missing one."
  @spec decode_all() :: map()
  def decode_all do
    dir = audio_dir()

    files =
      for name <- ["a440_stereo.wav", "tone1k.mp3", "tone660.m4a"], into: %{} do
        path = Path.join(dir, name)
        {t, result} = :timer.tc(fn -> MobAudioIo.decode(path) end)

        summary =
          case result do
            {:ok, pcm} ->
              %{
                rate: pcm.rate,
                channels: pcm.channels,
                frames: pcm.frames,
                seconds: r(pcm.frames / pcm.rate),
                rms_db: rms_db(pcm.data),
                ms: div(t, 1_000)
              }

            error ->
              error
          end

        {name, summary}
      end

    Map.merge(files, %{
      "mono a440" => mono_summary(Path.join(dir, "a440_stereo.wav")),
      "missing" => MobAudioIo.decode(Path.join(dir, "none.wav")),
      "capped" => MobAudioIo.decode(Path.join(dir, "a440_stereo.wav"), max_bytes: 100_000)
    })
  end

  @doc "What the plugin and the app's C see: devices, processors, registrations, the ABI."
  @spec view() :: map()
  def view do
    %{
      registered: :spike_dsp.registered(),
      abi: :spike_dsp.abi(),
      processors: MobAudioIo.processors(),
      inputs: MobAudioIo.devices(:input),
      outputs: MobAudioIo.devices(:output),
      session: MobAudioIo.status().session
    }
  end

  @doc "The whole verification: self-test, view, 60 s output and input, tone, loopback, decode."
  @spec run_all(pos_integer()) :: map()
  def run_all(seconds \\ 60) do
    %{
      selftest: MobAudioIo.SelfTest.run(%{}),
      view: view(),
      output: output("spike_click", seconds),
      input: input(seconds),
      tone: tone(10),
      loopback: loopback(15),
      decode: decode_all()
    }
  end

  defp with_stream(direction, opts, fun) do
    case MobAudioIo.open(self(), direction, opts) do
      {:ok, stream} ->
        try do
          fun.(stream)
          |> Map.put(:messages, stream |> drain() |> Enum.frequencies_by(&elem(&1, 1)))
        after
          MobAudioIo.close(stream)
        end

      error ->
        %{error: error}
    end
  end

  defp settle(stream) do
    Process.sleep(1_000)
    :ok = MobAudioIo.reset_stats(stream)
  end

  defp drain(stream, acc \\ []) do
    receive do
      {:mob_audio_io, kind, ^stream, payload} ->
        drain(stream, [{:mob_audio_io, kind, stream, payload} | acc])
    after
      0 -> Enum.reverse(acc)
    end
  end

  defp decode_post(<<click::signed-native-64, heard::signed-native-64, click_ns::signed-native-64,
                     heard_ns::signed-native-64>>),
       do: %{click: click, heard: heard, click_ns: click_ns, heard_ns: heard_ns}

  defp summarize(series) do
    %{
      rms_db_median: series |> Enum.map(& &1.rms_db) |> median(),
      rms_db_max: series |> Enum.map(& &1.rms_db) |> Enum.max(fn -> nil end),
      peak_db_max: series |> Enum.map(& &1.peak_db) |> Enum.max(fn -> nil end),
      seconds: length(series),
      first: Enum.take(series, 3)
    }
  end

  defp mono_summary(path) do
    case MobAudioIo.decode(path, mono: true) do
      {:ok, pcm} -> %{channels: pcm.channels, frames: pcm.frames, rms_db: rms_db(pcm.data)}
      error -> error
    end
  end

  defp rms_db(data) do
    {sum, n} = for <<x::float-32-native <- data>>, reduce: {0.0, 0}, do: ({s, n} -> {s + x * x, n + 1})
    if n > 0 and sum > 0, do: r(10 * :math.log10(sum / n)), else: nil
  end

  defp audio_dir do
    case System.get_env("MOB_BEAMS_DIR") do
      nil -> Path.expand("priv/audio")
      beams -> Path.join([beams, "priv", "audio"])
    end
  end

  defp median([]), do: nil

  defp median(list) do
    sorted = Enum.sort(list)
    sorted |> Enum.at(div(length(sorted), 2)) |> r()
  end

  defp ms(nil, _rate), do: nil
  defp ms(frames, rate), do: r(frames * 1_000 / rate)

  defp r(nil), do: nil
  defp r(x) when is_float(x), do: Float.round(x, 2)
  defp r(x), do: x
end
