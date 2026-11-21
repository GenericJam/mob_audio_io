defmodule MobAudioIo.Fake.Wav do
  @moduledoc false
  # The Fake's decoder: RIFF/WAVE with 16-, 24- or 32-bit integer PCM or
  # 32-bit float data, to the same result `MobAudioIo.decode/2` gives on a
  # device (interleaved native-endian f32, the size cap, mono folding).

  @pcm 1
  @float 3
  @extensible 0xFFFE

  @spec parse(binary()) :: {:ok, map(), binary()} | {:error, atom()}
  def parse(<<"RIFF", _size::little-32, "WAVE", chunks::binary>>), do: chunks(chunks, nil)
  def parse(_), do: {:error, :unsupported}

  defp chunks(<<"fmt ", n::little-32, body::binary-size(n), rest::binary>>, _fmt) do
    case fmt(body) do
      {:ok, fmt} -> chunks(pad(rest, n), fmt)
      error -> error
    end
  end

  defp chunks(<<"data", n::little-32, rest::binary>>, fmt) when is_map(fmt) do
    {:ok, fmt, binary_part(rest, 0, min(n, byte_size(rest)))}
  end

  defp chunks(<<"data", _::binary>>, nil), do: {:error, :decode_error}

  defp chunks(<<_id::binary-4, n::little-32, rest::binary>>, fmt) when byte_size(rest) >= n,
    do: chunks(pad(binary_part(rest, n, byte_size(rest) - n), n), fmt)

  defp chunks(_, _), do: {:error, :no_audio}

  # chunks are word-aligned
  defp pad(<<_, rest::binary>>, n) when rem(n, 2) == 1, do: rest
  defp pad(rest, _n), do: rest

  defp fmt(
         <<tag::little-16, channels::little-16, rate::little-32, _::binary-6, bits::little-16,
           ext::binary>>
       )
       when channels > 0 and rate > 0 do
    tag = if tag == @extensible, do: sub_format(ext), else: tag

    case {tag, bits} do
      {@pcm, b} when b in [16, 24, 32] ->
        {:ok, %{kind: :int, bits: b, channels: channels, rate: rate}}

      {@float, 32} ->
        {:ok, %{kind: :float, bits: 32, channels: channels, rate: rate}}

      _ ->
        {:error, :unsupported}
    end
  end

  defp fmt(_), do: {:error, :decode_error}

  defp sub_format(
         <<_cb::little-16, _valid::little-16, _mask::little-32, tag::little-16, _::binary>>
       ),
       do: tag

  defp sub_format(_), do: 0

  @spec to_f32(map(), binary(), pos_integer(), boolean()) :: {:ok, map()} | {:error, atom()}
  def to_f32(%{bits: bits, channels: ch, rate: rate} = fmt, data, max_bytes, mono) do
    frame_bytes = div(bits, 8) * ch
    frames = div(byte_size(data), frame_bytes)
    out_ch = if mono, do: 1, else: ch

    cond do
      frames == 0 ->
        {:error, :no_audio}

      frames * out_ch * 4 > max_bytes ->
        {:error, :too_large}

      true ->
        samples = samples(fmt, binary_part(data, 0, frames * frame_bytes))
        samples = if mono, do: fold(samples, ch), else: samples
        out = for x <- samples, into: <<>>, do: <<x::float-32-native>>
        {:ok, %{rate: rate, channels: out_ch, frames: frames, data: out}}
    end
  end

  defp samples(%{kind: :float}, data), do: for(<<x::float-32-little <- data>>, do: x)
  defp samples(%{bits: 16}, data), do: for(<<x::signed-little-16 <- data>>, do: x / 32_768)
  defp samples(%{bits: 24}, data), do: for(<<x::signed-little-24 <- data>>, do: x / 8_388_608)
  defp samples(%{bits: 32}, data), do: for(<<x::signed-little-32 <- data>>, do: x / 2_147_483_648)

  defp fold(samples, ch) do
    samples |> Enum.chunk_every(ch) |> Enum.map(&(Enum.sum(&1) / ch))
  end
end
