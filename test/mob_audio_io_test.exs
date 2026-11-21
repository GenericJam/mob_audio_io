defmodule MobAudioIoTest do
  use ExUnit.Case, async: false

  alias MobAudioIo.Fake

  setup do
    Application.put_env(:mob_audio_io, :backend, Fake)
    Fake.reset()
    on_exit(fn -> Application.delete_env(:mob_audio_io, :backend) end)
  end

  describe "open/3 options" do
    test "defaults: device rate, low latency, shared, the playback session" do
      assert {:ok, %{processor: "mai_sine", rate: 0, frames: 0, sharing: :shared, session: :play}} =
               MobAudioIo.normalize_options(processor: "mai_sine")
    end

    test "explicit rate, buffer frames, sharing and session" do
      assert {:ok, %{rate: 44_100, frames: 256, sharing: :exclusive, session: :play_and_record}} =
               MobAudioIo.normalize_options(
                 processor: "x",
                 rate: 44_100,
                 buffer: {:frames, 256},
                 sharing: :exclusive,
                 session: :play_and_record
               )
    end

    test "each bad option is named" do
      assert {:error, {:invalid_option, :processor}} = MobAudioIo.normalize_options([])
      assert {:error, {:invalid_option, :processor}} = MobAudioIo.normalize_options(processor: "")

      assert {:error, {:invalid_option, :processor}} =
               MobAudioIo.normalize_options(processor: String.duplicate("p", 48))

      assert {:error, {:invalid_option, :rate}} =
               MobAudioIo.normalize_options(processor: "x", rate: 100)

      assert {:error, {:invalid_option, :buffer}} =
               MobAudioIo.normalize_options(processor: "x", buffer: {:frames, 0})

      assert {:error, {:invalid_option, :sharing}} =
               MobAudioIo.normalize_options(processor: "x", sharing: :hog)

      assert {:error, {:invalid_option, :session}} =
               MobAudioIo.normalize_options(processor: "x", session: :record)

      assert {:error, {:invalid_option, :device}} =
               MobAudioIo.normalize_options(processor: "x", device: 3)
    end

    test "an unknown direction is an option error, before the backend" do
      assert {:error, {:invalid_option, :direction}} =
               MobAudioIo.open(self(), :sideways, processor: "mai_sine")
    end
  end

  describe "with the Fake backend" do
    test "open sends :started and status counts callbacks over time" do
      assert {:ok, stream} = MobAudioIo.open(self(), :output, processor: "mai_silence")
      assert_receive {:mob_audio_io, :started, ^stream, %{rate: 48_000, buffer: 384}}

      %{callbacks: before} = MobAudioIo.status(stream)
      Process.sleep(20)
      %{callbacks: later, processor: "mai_silence"} = MobAudioIo.status(stream)
      assert later > before

      assert %{streams: [%{id: ^stream}], processors: ["mai_silence", "mai_sine"]} =
               MobAudioIo.status()
    end

    test "close sends :stopped once; a second close is :not_open" do
      {:ok, stream} = MobAudioIo.open(self(), :output, processor: "mai_sine")
      assert :ok = MobAudioIo.close(stream)
      assert_receive {:mob_audio_io, :stopped, ^stream, %{reason: :closed}}
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

    test "input and duplex are unsupported in this version" do
      assert {:error, :unsupported} = MobAudioIo.open(self(), :input, processor: "mai_silence")
      assert {:error, :unsupported} = MobAudioIo.open(self(), :duplex, processor: "mai_silence")
    end

    test "a queued failure, then the ninth stream, are errors" do
      Fake.fail_next_open(:session)
      assert {:error, :session} = MobAudioIo.open(self(), :output, processor: "mai_silence")

      for _ <- 1..8, do: {:ok, _} = MobAudioIo.open(self(), :output, processor: "mai_silence")
      assert {:error, :busy} = MobAudioIo.open(self(), :output, processor: "mai_silence")
    end

    test "inject delivers what the hardware would" do
      {:ok, stream} = MobAudioIo.open(self(), :output, processor: "mai_silence")
      assert :ok = Fake.inject(stream, :device_lost, %{})
      assert_receive {:mob_audio_io, :device_lost, ^stream, %{}}
      assert {:error, :not_open} = Fake.inject(stream + 100, :xrun, %{})
    end
  end

  describe "the native backend on the host" do
    test "answers :unsupported, never raises" do
      Application.put_env(:mob_audio_io, :backend, MobAudioIo.Native)

      assert {:error, :unsupported} = MobAudioIo.open(self(), :output, processor: "mai_sine")
      assert {:error, :unsupported} = MobAudioIo.status()
      assert {:error, :unsupported} = MobAudioIo.close(1)
      assert {:error, :unsupported} = MobAudioIo.processors()
      refute MobAudioIo.Native.available?()
    end
  end

  describe "the self-test" do
    test "passes only when the callback count rose" do
      assert :pass = MobAudioIo.SelfTest.classify(3, 12)
      assert {:fail, "no audio callback" <> _} = MobAudioIo.SelfTest.classify(5, 5)
      assert {:fail, _} = MobAudioIo.SelfTest.classify({:error, :not_open}, 5)
    end

    test "fails on the host, where the NIF isn't linked" do
      assert {:fail, "mob_audio_io_nif is not linked" <> _} = MobAudioIo.SelfTest.run(%{})
    end
  end
end
