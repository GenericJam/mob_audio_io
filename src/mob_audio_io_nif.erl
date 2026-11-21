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
    decode/3,
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

%% decode(Path, MaxBytes, Mono) -> {ok, #{rate, channels, frames, data}}
%%   | {error, Reason} | {error, {Reason, Detail}}
decode(_Path, _MaxBytes, _Mono) ->
    erlang:nif_error(nif_not_loaded).

set_logger(_PidOrNil) ->
    erlang:nif_error(nif_not_loaded).

abi_version() ->
    erlang:nif_error(nif_not_loaded).
