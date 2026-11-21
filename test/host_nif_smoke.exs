# The real NIF's file decoding in a host BEAM, on macOS: test/c/mai_nif_host.c
# (priv/native/common/mai_nif.c with the iOS backend's ExtAudioFile decode)
# built as mob_audio_io_nif.so and loaded under the plugin's own Elixir, so
# decode_stream/3, decode_ack/2, decode_cancel/1 and decode/2 run end to end:
# the decode thread, its messages, the window, cancel, the owner's monitor.
# Files: 5-minute 44.1 kHz stereo WAV, AAC .m4a (afconvert) and MP3 (lame or
# ffmpeg, skipped without one). Not part of `mix test` (no NIF on the host
# there); CI's macos-coreaudio job runs it:
#
#   elixir test/host_nif_smoke.exs <dir holding mob_audio_io_nif.so> [workdir]

[so_dir | rest] = System.argv()
root = Path.expand("..", __DIR__)
work = List.first(rest) || Path.join(System.tmp_dir!(), "mai_host_nif_#{System.os_time()}")
File.mkdir_p!(work)

{:ok, :mob_audio_io_nif, beam} =
  :compile.file(String.to_charlist(Path.join(root, "src/mob_audio_io_nif.erl")), [:binary])

# on_load loads "mob_audio_io_nif" relative to the working directory
File.cd!(so_dir, fn ->
  {:module, _} = :code.load_binary(:mob_audio_io_nif, ~c"mob_audio_io_nif.erl", beam)
end)

for f <- ~w(backend log native), do: Code.require_file("lib/mob_audio_io/#{f}.ex", root)
Code.require_file("lib/mob_audio_io.ex", root)
true = MobAudioIo.Native.available?()

defmodule HostNifSmoke.Files do
  @rate 44_100
  @seconds 300

  def rate, do: @rate
  def frames, do: @rate * @seconds

  # `seconds` of 44.1 kHz stereo 16-bit: 1 kHz at 0.5 left, 0.25 right.
  def wav(path, seconds) do
    second =
      for i <- 0..(@rate - 1), into: <<>> do
        x = :math.sin(2 * :math.pi() * 1000 * i / @rate)
        <<round(0.5 * x * 32_767)::little-signed-16, round(0.25 * x * 32_767)::little-signed-16>>
      end

    data = :binary.copy(second, seconds)

    File.write!(path, [
      "RIFF",
      <<36 + byte_size(data)::little-32>>,
      "WAVEfmt ",
      <<16::little-32, 1::little-16, 2::little-16, @rate::little-32, @rate * 4::little-32,
        4::little-16, 16::little-16>>,
      "data",
      <<byte_size(data)::little-32>>,
      data
    ])

    path
  end

  def make(work) do
    wav = wav(Path.join(work, "long.wav"), @seconds)
    m4a = Path.join(work, "long.m4a")
    {_, 0} = System.cmd("afconvert", ~w(-f m4af -d aac -b 128000) ++ [wav, m4a])
    mp3 = Path.join(work, "long.mp3")

    mp3 =
      cond do
        System.find_executable("lame") ->
          {_, 0} = System.cmd("lame", ["--quiet", "-b", "192", wav, mp3])
          mp3

        System.find_executable("ffmpeg") ->
          {_, 0} =
            System.cmd(
              "ffmpeg",
              ~w(-loglevel error -y -i) ++ [wav] ++ ~w(-codec:a libmp3lame -b:a 192k) ++ [mp3]
            )

          mp3

        true ->
          IO.puts("SKIP mp3: no MP3 encoder here")
          nil
      end

    %{
      short: wav(Path.join(work, "short.wav"), 1),
      wav: wav,
      m4a: m4a,
      mp3: mp3,
      text:
        Path.join(work, "text.wav") |> tap(&File.write!(&1, String.duplicate("not audio ", 20)))
    }
  end
end

Application.put_env(:host_nif_smoke, :files, HostNifSmoke.Files.make(work))
ExUnit.start(autorun: false, capture_log: true)

defmodule HostNifSmoke do
  use ExUnit.Case, async: false

  alias HostNifSmoke.Files

  @window 4
  @chunk 4096

  defp files, do: Application.fetch_env!(:host_nif_smoke, :files)

  defp rss_kb do
    {out, 0} = System.cmd("ps", ["-o", "rss=", "-p", System.pid()])
    out |> String.trim() |> String.to_integer()
  end

  # The owner: takes each chunk, reads it (an analysis would), acks it;
  # never more than the window - 1 other chunks waiting in the mailbox (the
  # end may follow the last ones without an ack). Cancels once
  # `cancel_at` frames arrived (nil: never).
  defp stream(path, opts, cancel_at \\ nil) do
    :erlang.garbage_collect()
    base_bin = :erlang.memory(:binary)
    base_rss = rss_kb()
    t0 = System.monotonic_time(:microsecond)

    {:ok, d} =
      MobAudioIo.decode_stream(path, self(), [window: @window, chunk_frames: @chunk] ++ opts)

    acc = %{
      d: d,
      frames: 0,
      chunks: 0,
      peak_bin: 0,
      peak_rss: 0,
      waiting_max: 0,
      cancel_at: cancel_at,
      t_cancel: nil
    }

    {result, acc} = loop(acc)
    elapsed_ms = (System.monotonic_time(:microsecond) - t0) / 1000

    Map.merge(acc, %{
      result: result,
      elapsed_ms: elapsed_ms,
      bin_growth: acc.peak_bin - base_bin,
      rss_growth_kb: acc.peak_rss - base_rss
    })
  end

  defp loop(%{d: d} = acc) do
    receive do
      {:mob_audio_io, :decode_started, ^d, info} ->
        loop(Map.put(acc, :started, info))

      {:mob_audio_io, :decoded, ^d, chunk} ->
        {:messages, mailbox} = Process.info(self(), :messages)
        waiting = Enum.count(mailbox, &match?({:mob_audio_io, :decoded, ^d, _}, &1))
        _sum = for <<x::float-32-native <- chunk>>, reduce: 0.0, do: (s -> s + x * x)

        acc = %{
          acc
          | frames: acc.frames + div(byte_size(chunk), 4 * acc.started.channels),
            chunks: acc.chunks + 1
        }

        acc = %{acc | waiting_max: max(acc.waiting_max, waiting)}
        # (erlang:memory/1 and ps cost: sampled)
        acc =
          if rem(acc.chunks, 16) == 0,
            do: %{acc | peak_bin: max(acc.peak_bin, :erlang.memory(:binary))},
            else: acc

        acc =
          if rem(acc.chunks, 256) == 0,
            do: %{acc | peak_rss: max(acc.peak_rss, rss_kb())},
            else: acc

        _ = MobAudioIo.decode_ack(d)

        if acc.cancel_at && is_nil(acc.t_cancel) && acc.frames >= acc.cancel_at do
          t = System.monotonic_time(:microsecond)
          assert :ok = MobAudioIo.decode_cancel(d)
          loop(Map.put(%{acc | t_cancel: t}, :frames_at_cancel, acc.frames))
        else
          loop(acc)
        end

      {:mob_audio_io, kind, ^d, info}
      when kind in [:decode_done, :decode_cancelled, :decode_error] ->
        acc = Map.put(acc, :t_end, System.monotonic_time(:microsecond))
        refute_receive {:mob_audio_io, _, ^d, _}, 100
        {{kind, info}, acc}
    end
  end

  for {name, mono, slack} <- [{:wav, false, 0}, {:m4a, true, 2048}, {:mp3, false, 3 * 1152}] do
    test "#{name}: 5 minutes streamed whole, then cancelled a third of the way in" do
      path = files()[unquote(name)]

      if path do
        mono = unquote(mono)
        ch = if mono, do: 1, else: 2
        s = stream(path, mono: mono)
        pcm_mb = Files.frames() * ch * 4 / 1_048_576

        IO.puts(
          "#{unquote(name)}: #{s.frames} frames in #{round(s.elapsed_ms)} ms, #{s.chunks} chunks, " <>
            "at most #{s.waiting_max} more waiting, binary memory +#{Float.round(s.bin_growth / 1_048_576, 1)} MB, " <>
            "RSS +#{Float.round(s.rss_growth_kb / 1024, 1)} MB (the whole PCM: #{Float.round(pcm_mb, 1)} MB)"
        )

        assert {:decode_done, %{frames: frames, rate: 44_100, channels: ^ch}} = s.result
        assert s.frames == frames
        assert abs(frames - Files.frames()) <= unquote(slack)
        assert s.started.rate == 44_100 and s.started.channels == ch
        assert s.waiting_max <= @window - 1
        assert s.bin_growth < 16 * 1_048_576
        assert s.rss_growth_kb < 48 * 1024

        c = stream(path, [mono: mono], div(Files.frames(), 3))
        latency_ms = (c.t_end - c.t_cancel) / 1000

        IO.puts(
          "#{unquote(name)} cancelled at #{c.frames_at_cancel} frames: :decode_cancelled #{Float.round(latency_ms, 2)} ms " <>
            "after the cancel, #{c.frames - c.frames_at_cancel} frames arrived after it (sent before it)"
        )

        assert {:decode_cancelled, %{frames: sent}} = c.result
        assert sent == c.frames
        assert c.frames - c.frames_at_cancel <= @window * @chunk
        assert latency_ms < 100
      end
    end
  end

  test "decode/2 on decode_stream: the same PCM; its size cap refuses at once" do
    %{short: short, wav: long} = files()
    {:ok, whole} = MobAudioIo.decode(short)
    assert %{rate: 44_100, channels: 2, frames: 44_100} = whole

    {:ok, d} = MobAudioIo.decode_stream(short, self())
    chunks = collect(d, [])
    assert IO.iodata_to_binary(chunks) == whole.data

    assert {:ok, %{channels: 1, frames: 44_100}} = MobAudioIo.decode(short, mono: true)
    {t, result} = :timer.tc(fn -> MobAudioIo.decode(long, max_bytes: 1_000_000) end)
    assert result == {:error, :too_large}
    assert t < 500_000
  end

  defp collect(d, acc) do
    receive do
      {:mob_audio_io, :decoded, ^d, c} ->
        MobAudioIo.decode_ack(d)
        collect(d, [acc | c])

      {:mob_audio_io, :decode_done, ^d, _} ->
        acc

      {:mob_audio_io, :decode_started, ^d, _} ->
        collect(d, acc)
    end
  end

  test "max_ms refuses the 5-minute file from its stated length" do
    {:ok, d} = MobAudioIo.decode_stream(files().m4a, self(), max_ms: 60_000)
    assert_receive {:mob_audio_io, :decode_started, ^d, %{frames: stated}}
    assert_receive {:mob_audio_io, :decode_error, ^d, %{reason: :too_long, frames: 0}}, 1000
    assert abs(stated - Files.frames()) < 4096
  end

  test "the owner's exit cancels; a processor takes the chunks" do
    path = files().wav
    test = self()

    owner =
      spawn(fn ->
        send(test, MobAudioIo.decode_stream(path, self(), window: 1))
        Process.sleep(:infinity)
      end)

    assert_receive {:ok, d}
    Process.exit(owner, :kill)

    assert Enum.any?(1..100, fn _ ->
             Process.sleep(10)
             MobAudioIo.decode_cancel(d) == {:error, :not_running}
           end)

    {:ok, d} =
      MobAudioIo.decode_stream(files().short, self(), processor: "mai_silence", window: 1)

    assert_receive {:mob_audio_io, :decode_started, ^d, %{rate: 44_100}}
    assert_receive {:mob_audio_io, :decode_done, ^d, %{frames: 44_100}}
    refute_received {:mob_audio_io, :decoded, ^d, _}
  end

  test "errors, busy, cancel of a waiting decode" do
    {:ok, d} = MobAudioIo.decode_stream("/nonexistent/x.wav", self())

    assert_receive {:mob_audio_io, :decode_error, ^d,
                    %{reason: :not_found, detail: "no such file" <> _}}

    {:ok, d} = MobAudioIo.decode_stream(files().text, self())
    assert_receive {:mob_audio_io, :decode_error, ^d, %{reason: :unsupported}}
    assert {:error, :unsupported} = MobAudioIo.decode(files().text)
    assert {:error, {:not_found, _}} = MobAudioIo.decode("/nonexistent/x.wav")

    assert {:error, :unknown_processor} =
             MobAudioIo.decode_stream(files().short, self(), processor: "nope")

    ids =
      for _ <- 1..4 do
        {:ok, d} = MobAudioIo.decode_stream(files().wav, self(), window: 1)
        assert_receive {:mob_audio_io, :decode_started, ^d, _}
        assert_receive {:mob_audio_io, :decoded, ^d, _}
        d
      end

    assert {:error, :busy} = MobAudioIo.decode_stream(files().short, self())

    for d <- ids do
      assert :ok = MobAudioIo.decode_cancel(d)
      assert_receive {:mob_audio_io, :decode_cancelled, ^d, %{frames: 4096}}
    end

    refute_receive {:mob_audio_io, _, _, _}, 100
    assert {:error, :not_running} = MobAudioIo.decode_ack(hd(ids))
  end
end

%{failures: failures} = ExUnit.run()
System.halt(if failures == 0, do: 0, else: 1)
