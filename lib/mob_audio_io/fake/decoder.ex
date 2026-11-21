defmodule MobAudioIo.Fake.Decoder do
  @moduledoc false
  # One Fake decode (MobAudioIo.Fake.decode_stream/3): a process that sends
  # its owner what a device's decode thread sends (MobAudioIo's "Decoding
  # files"): :decode_started, :decoded chunks while fewer than `window` are
  # unacknowledged, then one end. It answers {:cancel, from, ref} with
  # {ref, :ok} and sends nothing after but :decode_cancelled; the owner's
  # exit stops it. `ended` runs last, after the end went out.

  alias MobAudioIo.Fake.Wav

  @spec run(pos_integer(), String.t(), pid(), map(), (pos_integer() -> any())) :: any()
  def run(decode, path, owner, opts, ended) do
    st = %{
      decode: decode,
      owner: owner,
      monitor: Process.monitor(owner),
      opts: opts,
      inflight: 0,
      frames: 0,
      channels: 0
    }

    result =
      with {:ok, bin} <- read(path),
           {:ok, fmt, data} <- Wav.parse(bin),
           {:ok, pcm} <- Wav.to_f32(fmt, data, opts.mono) do
        start(st, pcm)
      else
        {:error, reason} -> {{:error, reason}, st}
      end

    finish(result)
    ended.(decode)
  end

  defp read(path) do
    case File.read(path) do
      {:ok, bin} -> {:ok, bin}
      {:error, _} -> {:error, :not_found}
    end
  end

  defp start(st, %{rate: rate, channels: channels, frames: frames, data: data}) do
    send_owner(st, :decode_started, %{rate: rate, channels: channels, frames: frames})
    st = Map.merge(st, %{rate: rate, channels: channels})

    if st.opts.max_ms > 0 and frames > div(st.opts.max_ms * rate, 1000) do
      {{:error, :too_long}, st}
    else
      pump(st, chunks(data, st.opts.chunk_frames * channels * 4))
    end
  end

  defp chunks(<<>>, _size), do: []

  defp chunks(data, size) when byte_size(data) <= size, do: [data]

  defp chunks(data, size) do
    <<chunk::binary-size(^size), rest::binary>> = data
    [chunk | chunks(rest, size)]
  end

  # Send the next chunk when the window allows (a processor takes them
  # all), else wait for an ack; a cancel or the owner's exit ends it.
  defp pump(st, chunks) do
    timeout = if ready?(st, chunks), do: 0, else: :infinity

    receive do
      {:ack, n} ->
        pump(%{st | inflight: max(st.inflight - n, 0)}, chunks)

      {:cancel, from, ref} ->
        send(from, {ref, :ok})
        {:cancelled, st}

      {:DOWN, ref, :process, _, _} when ref == st.monitor ->
        {:cancelled, st}
    after
      timeout -> next(st, chunks)
    end
  end

  defp ready?(_st, []), do: true
  defp ready?(%{opts: %{processor: ""}} = st, _chunks), do: st.inflight < st.opts.window
  defp ready?(_st, _chunks), do: true

  defp next(st, []), do: {:done, st}

  defp next(st, [chunk | rest]) do
    frames = div(byte_size(chunk), st.channels * 4)

    if st.opts.processor == "" do
      send_owner(st, :decoded, chunk)
      pump(%{st | inflight: st.inflight + 1, frames: st.frames + frames}, rest)
    else
      pump(%{st | frames: st.frames + frames}, rest)
    end
  end

  defp finish({:done, st}),
    do: send_owner(st, :decode_done, %{frames: st.frames, rate: st.rate, channels: st.channels})

  defp finish({:cancelled, st}), do: send_owner(st, :decode_cancelled, %{frames: st.frames})

  defp finish({{:error, reason}, st}),
    do: send_owner(st, :decode_error, %{reason: reason, detail: nil, frames: st.frames})

  defp send_owner(st, kind, payload),
    do: send(st.owner, {:mob_audio_io, kind, st.decode, payload})
end
