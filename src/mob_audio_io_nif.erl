%% mob_audio_io_nif: the Erlang module of the mob_audio_io NIF.
%%
%% The native side is one C core (priv/native/common/mai_nif.c) built per
%% platform: priv/native/android/mob_audio_io_nif.c (AAudio, JNI,
%% AMediaCodec) and priv/native/ios/mob_audio_io_nif.m (RemoteIO +
%% AVAudioSession, ExtAudioFile). On a device the NIF is statically linked
%% into the app; on the host it isn't, so on_load tolerates the failure and
%% the stubs raise nif_not_loaded, which MobAudioIo.Native turns into
%% {error, unsupported}.
-module(mob_audio_io_nif).
-export([
    open/4,
    close/1,
    status/0,
    stream_status/1,
    reset_stats/1,
    processors/0,
    devices/1,
    decode_stream/3,
    decode_ack/2,
    decode_cancel/1,
    set_logger/1,
    abi_version/0
]).
-on_load(init/0).

init() ->
    case erlang:load_nif("mob_audio_io_nif", 0) of
        ok -> ok;
        {error, _} -> ok
    end.

%% open(Owner, Direction, Processor, Opts)
%%   Direction: output | input | duplex
%%   Opts: #{rate (0: the device's), frames (0: low latency), channels,
%%           input_channels, sharing, session, preset, device, input_device
%%           (binaries, <<>> for the default)}
%% -> {ok, Id} | {error, Reason} | {error, {Reason, Detail}}
open(_Owner, _Direction, _Processor, _Opts) ->
    erlang:nif_error(nif_not_loaded).

close(_Id) ->
    erlang:nif_error(nif_not_loaded).

status() ->
    erlang:nif_error(nif_not_loaded).

stream_status(_Id) ->
    erlang:nif_error(nif_not_loaded).

reset_stats(_Id) ->
    erlang:nif_error(nif_not_loaded).

processors() ->
    erlang:nif_error(nif_not_loaded).

%% devices(input | output) -> [#{id, name, kind}] | {error, Reason}
devices(_Direction) ->
    erlang:nif_error(nif_not_loaded).

%% decode_stream(Path, Owner, Opts) -> {ok, Id} | {error, busy | unknown_processor | no_memory}
%%   Opts: #{mono, chunk_frames, window, max_ms (0: none),
%%           processor (<<>>: the owner gets the chunks)}
%% The owner gets {mob_audio_io, decode_started | decoded | decode_done |
%% decode_cancelled | decode_error, Id, Payload}.
decode_stream(_Path, _Owner, _Opts) ->
    erlang:nif_error(nif_not_loaded).

%% decode_ack(Id, N) -> ok | {error, not_running}
decode_ack(_Id, _N) ->
    erlang:nif_error(nif_not_loaded).

%% decode_cancel(Id) -> ok | {error, not_running}
decode_cancel(_Id) ->
    erlang:nif_error(nif_not_loaded).

set_logger(_PidOrNil) ->
    erlang:nif_error(nif_not_loaded).

abi_version() ->
    erlang:nif_error(nif_not_loaded).
