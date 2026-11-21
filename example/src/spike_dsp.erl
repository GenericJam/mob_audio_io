%% spike_dsp: the example app's own NIF (c_src/spike_dsp.c), which registers
%% processors into mob_audio_io through its C ABI.
-module(spike_dsp).
-export([registered/0, abi/0, count_in/2, stats/0, tone/1, pitch/1]).
-on_load(init/0).

init() ->
    case erlang:load_nif("spike_dsp", 0) of
        ok -> ok;
        {error, _} -> ok
    end.

%% [{Name, Result}]: mai_register's results (0 ok) for every processor
registered() -> erlang:nif_error(nif_not_loaded).

%% the linked plugin's MAI_ABI_VERSION, called from app C
abi() -> erlang:nif_error(nif_not_loaded).

%% Magpie's count-in: Beats metronome clicks at Bpm (a float)
count_in(_Beats, _Bpm) -> erlang:nif_error(nif_not_loaded).

stats() -> erlang:nif_error(nif_not_loaded).

%% tone_loop's rendered frequency in Hz (a float; 0.0 for silence)
tone(_Hz) -> erlang:nif_error(nif_not_loaded).

%% #{hz, clarity, rms_db, samples}: the pitch of tone_loop's last WindowMs of input
pitch(_WindowMs) -> erlang:nif_error(nif_not_loaded).
