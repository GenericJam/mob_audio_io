defmodule MobAudioIoTest do
  use ExUnit.Case, async: false

  alias MobAudioIo.Fake

  @moduletag :tmp_dir
  @moduletag :capture_log

  setup do
    Application.put_env(:mob_audio_io, :backend, Fake)
    Fake.reset()
    on_exit(fn -> Application.delete_env(:mob_audio_io, :backend) end)
  end

  defp norm(direction, opts), do: MobAudioIo.normalize_options(direction, opts)

  describe "open/3 options" do
    test "output defaults: device rate, low latency, stereo, shared, the playback session" do
      assert {:ok,
              %{
                processor: "mai_sine",
                module: nil,
                rate: 0,
                frames: 0,
                channels: 2,
                input_channels: 1,
                sharing: :shared,
                session: :play,
                preset: :default,
                device: "",
                input_device: ""
              }} = norm(:output, processor: "mai_sine")
    end

    test "input and duplex default to play-and-record; :play can't record" do
      assert {:ok, %{session: :play_and_record}} = norm(:input, processor: "x")
      assert {:ok, %{session: :mix}} = norm(:duplex, processor: "x", session: :mix)
      assert {:error, {:invalid_option, :session}} = norm(:input, processor: "x", session: :play)
    end

    test "explicit values pass through; device ids become strings" do
      assert {:ok,
              %{
                rate: 44_100,
                frames: 256,
                channels: 1,
                input_channels: 2,
                sharing: :exclusive,
                preset: :unprocessed,
                device: "7",
                input_device: "BuiltInMicrophoneDevice"
              }} =
               norm(:duplex,
                 processor: "x",
                 rate: 44_100,
                 buffer: {:frames, 256},
                 channels: 1,
                 input_channels: 2,
                 sharing: :exclusive,
                 preset: :unprocessed,
                 device: 7,
                 input_device: "BuiltInMicrophoneDevice"
               )
    end

    test "each bad option is named" do
      bad = [
        {:output, [], :processor},
        {:output, [processor: ""], :processor},
        {:output, [processor: String.duplicate("p", 48)], :processor},
        {:output, [processor: "x", rate: 100], :rate},
        {:output, [processor: "x", buffer: {:frames, 0}], :buffer},
        {:output, [processor: "x", channels: 6], :channels},
        {:input, [processor: "x", input_channels: 0], :input_channels},
        {:output, [processor: "x", sharing: :hog], :sharing},
        {:output, [processor: "x", session: :record], :session},
        {:input, [processor: "x", preset: :karaoke], :preset},
        {:output, [processor: "x", preset: :voice], :preset},
        {:output, [processor: "x", device: -3], :device},
        {:input, [processor: "x", input_device: 3], :input_device},
        {:output, [processor: "x", module: "my_dsp"], :module},
        {:output, [processor: "x", volume: 3], :volume}
      ]

      for {direction, opts, key} <- bad do
        assert {:error, {:invalid_option, ^key}} = norm(direction, opts),
               "#{inspect(direction)} #{inspect(opts)}"
      end
    end

    test "an unknown direction is an option error, before the backend" do
      assert {:error, {:invalid_option, :direction}} =
               MobAudioIo.open(self(), :sideways, processor: "mai_sine")
    end

    test ":module is loaded before the open; one that can't load is an error" do
      assert {:ok, _} = MobAudioIo.open(self(), :output, processor: "mai_sine", module: :lists)

      assert {:error, {:module, :nofile}} =
               MobAudioIo.open(self(), :output, processor: "mai_sine", module: :no_such_dsp)
    end
  end

  describe "with the Fake backend" do
    test "open sends :started and status counts callbacks over time" do
      assert {:ok, stream} = MobAudioIo.open(self(), :output, processor: "mai_silence")

      assert_receive {:mob_audio_io, :started, ^stream,
                      %{rate: 48_000, buffer: 384, direction: :output, input_channels: 0}}

      %{callbacks: before} = MobAudioIo.status(stream)
      Process.sleep(20)
      %{callbacks: later, processor: "mai_silence", in_peak_db: nil} = MobAudioIo.status(stream)
      assert later > before

      assert %{streams: [%{id: ^stream}], processors: ["mai_silence", "mai_sine"]} =
               MobAudioIo.status()
    end

    test "close sends :stopped once; a second close is :not_open" do
      {:ok, stream} = MobAudioIo.open(self(), :output, processor: "mai_sine")
      assert :ok = MobAudioIo.close(stream)
      assert_receive {:mob_audio_io, :stopped, ^stream, %{reason: :closed, posts_dropped: 0}}
      assert {:error, :not_open} = MobAudioIo.close(stream)
      assert {:error, :not_open} = MobAudioIo.status(stream)
    end

    test "an app processor must be registered first" do
      assert {:error, :unknown_processor} =
               MobAudioIo.open(self(), :output, processor: "magpie_sampler")

      Fake.register_processor("magpie_sampler")
      assert {:ok, _} = MobAudioIo.open(self(), :output, processor: "magpie_sampler")
      assert "magpie_sampler" in MobAudioIo.processors()
    end

    test "input and duplex need the microphone permission" do
      assert {:ok, input} = MobAudioIo.open(self(), :input, processor: "mai_silence")
      Fake.set_input_level(input, -23.5)

      assert %{direction: :input, channels: 0, input_channels: 1, in_rms_db: -23.5} =
               MobAudioIo.status(input)

      Fake.deny_permission()
      assert {:error, :permission} = MobAudioIo.open(self(), :duplex, processor: "mai_silence")
      assert {:ok, _} = MobAudioIo.open(self(), :output, processor: "mai_silence")
    end

    test "a queued failure, then the ninth stream, are errors" do
      Fake.fail_next_open({:unavailable, "open: AAUDIO_ERROR_UNAVAILABLE (-889)"})

      assert {:error, {:unavailable, "open: " <> _}} =
               MobAudioIo.open(self(), :output, processor: "mai_silence")

      for _ <- 1..8, do: {:ok, _} = MobAudioIo.open(self(), :output, processor: "mai_silence")
      assert {:error, :busy} = MobAudioIo.open(self(), :output, processor: "mai_silence")
    end

    test "inject delivers what the hardware would" do
      {:ok, stream} = MobAudioIo.open(self(), :output, processor: "mai_silence")
      assert :ok = Fake.inject(stream, :device_lost, %{})
      assert_receive {:mob_audio_io, :device_lost, ^stream, %{}}
      assert {:error, :not_open} = Fake.inject(stream + 100, :xrun, %{})
    end

    test "devices lists what was set" do
      assert [%{kind: :builtin_mic}] = MobAudioIo.devices(:input)
      Fake.set_devices(:output, [%{id: 9, name: "USB DAC", kind: :usb}])
      assert [%{id: 9, kind: :usb}] = MobAudioIo.devices(:output)
    end
  end

  describe "decode/2 with the Fake" do
    defp wav(tag, bits, channels, rate, data) do
      block = div(bits, 8) * channels

      fmt =
        <<tag::little-16, channels::little-16, rate::little-32, rate * block::little-32,
          block::little-16, bits::little-16>>

      body =
        "WAVE" <>
          "fmt " <>
          <<byte_size(fmt)::little-32>> <>
          fmt <>
          "LIST" <> <<3::little-32, "abc", 0>> <> "data" <> <<byte_size(data)::little-32>> <> data

      "RIFF" <> <<byte_size(body)::little-32>> <> body
    end

    defp floats(bin), do: for(<<x::float-32-native <- bin>>, do: Float.round(x, 4))

    test "16-bit stereo PCM to interleaved floats, past an odd-sized chunk", %{tmp_dir: dir} do
      path = Path.join(dir, "a.wav")

      File.write!(
        path,
        wav(1, 16, 2, 44_100, <<16_384::little-16, -32_768::little-16, 0::16, 8_192::little-16>>)
      )

      assert {:ok, %{rate: 44_100, channels: 2, frames: 2, data: data}} = MobAudioIo.decode(path)
      assert floats(data) == [0.5, -1.0, 0.0, 0.25]
    end

    test "mono folds channels; float and 24-bit data decode", %{tmp_dir: dir} do
      path = Path.join(dir, "f.wav")
      File.write!(path, wav(3, 32, 2, 48_000, <<0.5::float-32-little, 0.25::float-32-little>>))
      assert {:ok, %{channels: 1, frames: 1, data: data}} = MobAudioIo.decode(path, mono: true)
      assert floats(data) == [0.375]

      path = Path.join(dir, "i24.wav")
      File.write!(path, wav(1, 24, 1, 8_000, <<-4_194_304::little-24>>))
      assert {:ok, %{data: data}} = MobAudioIo.decode(path)
      assert floats(data) == [-0.5]
    end

    test "the size cap, a missing file, a non-WAV and bad options are errors", %{tmp_dir: dir} do
      path = Path.join(dir, "big.wav")
      File.write!(path, wav(1, 16, 1, 8_000, :binary.copy(<<1::16>>, 100)))
      assert {:error, :too_large} = MobAudioIo.decode(path, max_bytes: 399)
      assert {:ok, %{frames: 100}} = MobAudioIo.decode(path, max_bytes: 400)

      assert {:error, :not_found} = MobAudioIo.decode(Path.join(dir, "none.wav"))
      File.write!(Path.join(dir, "x.mp3"), "ID3...")
      assert {:error, :unsupported} = MobAudioIo.decode(Path.join(dir, "x.mp3"))
      assert {:error, {:invalid_option, :max_bytes}} = MobAudioIo.decode(path, max_bytes: 0)
      assert {:error, {:invalid_option, :mono}} = MobAudioIo.decode(path, mono: 1)
    end
  end

  describe "the native backend on the host" do
    test "answers :unsupported, never raises" do
      Application.put_env(:mob_audio_io, :backend, MobAudioIo.Native)

      assert {:error, :unsupported} = MobAudioIo.open(self(), :output, processor: "mai_sine")
      assert {:error, :unsupported} = MobAudioIo.status()
      assert {:error, :unsupported} = MobAudioIo.close(1)
      assert {:error, :unsupported} = MobAudioIo.processors()
      assert {:error, :unsupported} = MobAudioIo.devices(:input)
      assert {:error, :unsupported} = MobAudioIo.decode("/tmp/none.wav")
      refute MobAudioIo.Native.available?()
    end
  end

  describe "the self-test" do
    test "passes only when the callback count rose" do
      assert :pass = MobAudioIo.SelfTest.classify(:output, 3, 12)
      assert {:fail, "input: no audio callback" <> _} = MobAudioIo.SelfTest.classify(:input, 5, 5)
      assert {:fail, _} = MobAudioIo.SelfTest.classify(:output, {:error, :not_open}, 5)
    end

    test "fails on the host, where the NIF isn't linked" do
      assert {:fail, "mob_audio_io_nif is not linked" <> _} = MobAudioIo.SelfTest.run(%{})
    end
  end

  describe "the logger" do
    test "warns on failures and on posts a close couldn't deliver" do
      assert MobAudioIo.Log.level(:xrun, %{}) == :warning
      assert MobAudioIo.Log.level(:stopped, %{reason: :owner_exit, posts_dropped: 3}) == :warning
      assert MobAudioIo.Log.level(:stopped, %{reason: :closed, posts_dropped: 0}) == :info
    end
  end
end
