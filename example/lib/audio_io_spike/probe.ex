defmodule AudioIoSpike.Probe do
  @moduledoc """
  The spike's measurements, run over RPC (`mix mob.connect`, then
  `:rpc.call(node, AudioIoSpike.Probe, :run, [...], timeout)`). The calling
  process owns the stream, so it gets the stream's messages.
  """

  @doc """
  Open `processor`, let it settle for a second, zero the worst-case counters,
  run for `seconds`, read `MobAudioIo.status/1`, close. Returns the status,
  the platform session, the owner messages by kind and, for the app's own
  processors, what the app C counted.
  """
  @spec run(String.t(), pos_integer(), keyword()) :: map()
  def run(processor, seconds, opts \\ []) do
    {:ok, stream} = MobAudioIo.open(self(), :output, [processor: processor] ++ opts)
    Process.sleep(1_000)
    :ok = MobAudioIo.reset_stats(stream)
    start = MobAudioIo.status(stream)
    if processor == "magpie_sampler", do: :spike_dsp.count_in(4 * seconds, 240.0)
    Process.sleep(seconds * 1_000)
    status = MobAudioIo.status(stream)
    session = MobAudioIo.status().session
    :ok = MobAudioIo.close(stream)
    messages = drain(stream, [])

    %{
      status: status,
      callbacks_in_window: status.callbacks - start.callbacks,
      session: session,
      messages: Enum.frequencies_by(messages, &elem(&1, 1)),
      first_posts: messages |> Enum.filter(&match?({:mob_audio_io, :post, _, _}, &1)) |> Enum.take(3),
      app: app_stats()
    }
  end

  @doc "What the app's C sees of the plugin: registration results and the ABI it linked."
  @spec app_view() :: map()
  def app_view do
    %{registered: :spike_dsp.registered(), abi: :spike_dsp.abi(), processors: MobAudioIo.processors()}
  end

  @doc """
  On boot, if `Documents/mai_probe_request` exists: run the whole spike
  measurement and write it to `Documents/mai_probe_result.txt` (pulled with
  `xcrun devicectl device copy from`). For a phone whose Erlang distribution
  the Mac can't reach (see SPIKE.md).
  """
  @spec maybe_run_from_file() :: :ok
  def maybe_run_from_file do
    # (no HOME in the iOS BEAM's environment: Documents is above the beams dir)
    docs = (System.get_env("MOB_BEAMS_DIR") || "") |> String.split("/Documents/") |> hd() |> Kernel.<>("/Documents")
    request = Path.join(docs, "mai_probe_request")

    if File.exists?(request) do
      File.rm(request)

      spawn(fn ->
        Process.sleep(3_000)
        result = run_all()
        File.write!(Path.join(docs, "mai_probe_result.txt"), inspect(result, pretty: true, limit: :infinity))
      end)
    end

    :ok
  end

  @doc "The self-test, the app's view of the plugin, the play-and-record latency probe and two 60 s runs."
  @spec run_all() :: map()
  def run_all do
    selftest = MobAudioIo.SelfTest.run(%{})
    view = app_view()
    probe = run("mai_silence", 5, session: :play_and_record)
    click = run("spike_click", 60)
    sampler = run("magpie_sampler", 60)
    %{selftest: selftest, app_view: view, play_and_record: probe, spike_click: click, magpie_sampler: sampler}
  end

  defp app_stats, do: :spike_dsp.stats()

  defp drain(stream, acc) do
    receive do
      {:mob_audio_io, kind, ^stream, payload} -> drain(stream, [{:mob_audio_io, kind, stream, payload} | acc])
    after
      200 -> Enum.reverse(acc)
    end
  end
end
