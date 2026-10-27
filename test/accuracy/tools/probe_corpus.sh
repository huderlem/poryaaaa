#!/bin/bash
# Run mgba_probe over the probed corpus (the 67 logs of build/probe/) for one
# test ROM: every micro-test, song, SE and cry the harness has a probe log for,
# with the frame counts, players and cry parameters those logs were made with.
#
#   test/accuracy/tools/probe_corpus.sh <rom.gba> <syms.nm> <out dir> [name ...]
#
# e.g. the vanilla ROM into build/probe (the canonical logs) or the HQ-mixer ROM
# into build/probe-hq (docs/HQ-MIXER-ACCURACY.md, step 2).  With names, only
# those logs.  The probe binary is build/probe/mgba_probe (accuracy-harness
# skill: how to build it and the CgbSound_* lines to append to the nm listing).
set -u
cd "$(dirname "$0")/../../.."
ROM=$1; SYMS=$2; OUT=$3; shift 3
mkdir -p "$OUT"
D="15360 0 120 140 0 0"          # StartCry's defaults: pitch pan volume length release chorus
CRY=gPokemonCryMusicPlayers
# the player and cry arguments come after the syms argument in mgba_probe's
# argv, so run2 reorders: name frames type id -- player [cry args]
run2() {
    local n=$1 frames=$2 type=$3 id=$4; shift 4
    if [ -n "$NAMES" ] && ! printf '%s\n' "$NAMES" | grep -qx "$n"; then return; fi
    build/probe/mgba_probe "$ROM" "$OUT/$n.txt" "$frames" "$type" "$id" "$SYMS" "$@" > "$OUT/$n.stdout" 2>&1 || echo "FAILED $n"
    echo "$n"
}
NAMES=$(printf '%s\n' "$@")
# micro-tests: type 2, id = line in micro.cfg from 0 (70 frames = 1.2 s, 180 = 3 s)
i=0
for m in sq1 sq1_env sq1_sweep sq1_alt sq2 sq2_alt wave wave_alt noise noise7 pcm pcm_hi pcm_adsr pcm_drum pcm_cmp pcm_reverb pcm_chord pcm_bass pan vol vel bend lfo_vib lfo_trem lfo_pan lfo_delay lfo_psg tune tune_psg psg_mix pcm_psg pcm_ratio_1x pcm_short_loop pcm_fix_loop pcm_reverb_max pcm_rev pcm_cmp_hi synth_pulse synth_pulse_lfo synth_saw synth_tri; do
    case $m in
        pcm|pcm_cmp|wave) f=180;;
        pcm_hi|pcm_adsr|pcm_reverb|pcm_bass|pan|vol|tune|pcm_ratio_1x|pcm_short_loop|pcm_fix_loop|pcm_reverb_max|pcm_rev|pcm_cmp_hi|synth_pulse|synth_pulse_lfo|synth_saw|synth_tri) f=0;;   # not in the probed corpus
        *) f=70;;
    esac
    [ $f = 0 ] || run2 $m $f 2 $i
    [ $m = wave ] && run2 wave180 180 2 $i
    i=$((i + 1))
done
# songs: type 0, 1200 frames = 20 s (credits 600 = 10 s)
run2 littleroot 1200 0 405; run2 groudon 1200 0 444; run2 vs_wild 1200 0 474; run2 mt_chimney 1200 0 406
run2 rg_vs_trainer 1200 0 509; run2 vs_trainer 1200 0 476; run2 credits 600 0 455
# sound effects: the player song_table.inc names
run2 se_exp 180 0 33 gMPlayInfo_SE1; run2 se_faint 180 0 16 gMPlayInfo_SE1
run2 se_contest_place 180 0 24 gMPlayInfo_SE2; run2 se_truck_unload 180 0 51 gMPlayInfo_SE2
run2 se_low_health 180 0 90 gMPlayInfo_SE3; run2 se_rain_stop 180 0 86 gMPlayInfo_SE3
# cries: type 1, gCryTable index, PlayCryInternal's mode values (songs.json)
run2 cry_pidgey 180 1 15 $CRY; run2 cry_bulbasaur 180 1 0 $CRY; run2 cry_wailord 180 1 381 $CRY; run2 cry_jynx 180 1 123 $CRY
run2 cry_jynx_faint 180 1 123 $CRY 14440 0 120 140 0 0
run2 cry_bulbasaur_howl 180 1 0 $CRY 15800 63 120 140 0 0
run2 cry_pidgey_quiet 180 1 15 $CRY 15360 0 90 140 0 0
run2 cry_jynx_faint_full 180 1 123 $CRY 14440 0 120 140 200 0
run2 cry_wailord_doubles 180 1 381 $CRY 15360 0 120 20 225 0
run2 cry_pidgey_roar2 180 1 15 $CRY 15616 0 120 60 225 0
run2 cry_bulbasaur_reverse 180 1 388 $CRY
run2 cry_jynx_growl1 180 1 511 $CRY 15200 0 120 15 125 0
run2 cry_pidgey_echo_start 180 1 403 $CRY 15600 0 90 25 100 0
run2 cry_bulbasaur_encounter 180 1 0 $CRY 15600 0 90 140 225 20
run2 cry_jynx_echo_end 180 1 123 $CRY 15555 0 70 140 220 192
run2 cry_pidgey_echo_start_chorus 180 1 403 $CRY 15600 0 90 25 100 192
run2 cry_jynx_high_pitch 180 1 123 $CRY 15800 0 90 50 200 20
run2 cry_wailord_high_pitch 180 1 381 $CRY 15800 0 90 50 200 20
# two cries (the second at the defaults on VBlank F), read on cry player 0 and 1
run2 cry_second_bulbasaur_pidgey 180 1 0 $CRY $D 15 20;      run2 cry_second_bulbasaur_pidgey_p1 180 1 0 "$CRY[1]" $D 15 20
run2 cry_second_jynx_wailord 240 1 123 $CRY $D 381 60;       run2 cry_second_jynx_wailord_p1 240 1 123 "$CRY[1]" $D 381 60
# a third cry with both cry players busy (and its twin without it)
run2 cry_third_alone_jynx_wailord 180 1 123 $CRY $D 381 20;         run2 cry_third_alone_jynx_wailord_p1 180 1 123 "$CRY[1]" $D 381 20
run2 cry_third_jynx_wailord_bulbasaur 180 1 123 $CRY $D 381 20 0 60; run2 cry_third_jynx_wailord_bulbasaur_p1 180 1 123 "$CRY[1]" $D 381 20 0 60
# a cry over an SE / a song, read on cry player 0 and on the song's player
run2 cry_over_se 180 0 33 $CRY $D 0 30;        run2 cry_over_se_se 180 0 33 gMPlayInfo_SE1 $D 0 30
run2 cry_over_bgm 180 0 405 $CRY $D 0 60;      run2 cry_over_bgm_bgm 180 0 405 gMPlayInfo_BGM $D 0 60
