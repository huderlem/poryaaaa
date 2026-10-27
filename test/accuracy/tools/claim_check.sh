#!/bin/bash
# One-shot check of an engine or timing-model claim (accuracy-harness skill,
# "Testing a review claim or a constant change"): rebuild, unit / resampler / song
# tests, the PSG write-cycle diff of every probed test in build/probe, then
# the harness against a baseline.
#
#   test/accuracy/tools/claim_check.sh [--mixer vanilla|hq] [--pcm-rate <hz>] [<baseline.json>] [<out dir>]
#
# Defaults: the newest file in test/accuracy/baselines/ of the mixer's family
# (vanilla: not an _hq- file; hq: an _hq- file; never a midi-input one, a
# family of its own), build/scratch/claim[-hq].
# --mixer hq (or MIXER=hq in the environment) checks the HQ-mixer mode
# (docs/HQ-MIXER-ACCURACY.md): the HQ test ROM build/accuracy/rom-hq, the
# probe logs under build/probe-hq, poryaaaa_render --hq-mixer, and
# accuracy.py --mixer hq.  --pcm-rate <hz> (or PCM_RATE=<hz>) checks a ROM
# built at another mix rate (build/accuracy/rom[-hq]-<hz>, probe logs under
# build/probe[-hq]-<hz>, poryaaaa_render --pcm-mix-rate, accuracy.py
# --pcm-rate; baselines *_[hq-]<hz>-*.json).
# Prints one summary line per stage; details land in <out dir>.  Exit status
# is nonzero when a stage fails, a write moves, or the harness regresses.
#
# The write diff needs the probe logs (build/probe/<name>.txt from
# mgba_probe; not committed) and renders each test for the probe's length:
# micro-tests 1.2 s (70 frames) or 3 s (180), songs 20 s (1200) or 10 s
# (600), SEs 3 s with --player, cries 3 s (180 frames) or 4 s (240) with
# --cry (the id and any pitch / pan / volume / length / release / chorus
# from songs.json, and --cry2 / --cry2-start-frame for an id2 / start_frame2).
# An SE / song entry's id2 / start_frame2 (a cry over it) adds the same two
# flags to its line; a probe log named cry_over_* is resolved against that
# entry (its name, then the song it plays) before the cry_* rule.
set -u
cd "$(dirname "$0")/../../.."
MIXER=${MIXER:-vanilla}
PCM_RATE=${PCM_RATE:-13379}
while :; do
    case "${1:-}" in
        --mixer)    MIXER=$2; shift 2;;
        --pcm-rate) PCM_RATE=$2; shift 2;;
        *) break;;
    esac
done
RSUF=""; RATEFLAGS=""; RATEPAT=""
if [ "$PCM_RATE" != 13379 ]; then RSUF="-$PCM_RATE"; RATEFLAGS="--pcm-mix-rate $PCM_RATE"; RATEPAT="${PCM_RATE}-"; fi
case $MIXER in
    vanilla) SUF=""; HQFLAG="";
             if [ -n "$RSUF" ]; then newest=$(ls -t test/accuracy/baselines/*_${RATEPAT}*.json 2>/dev/null | grep -v '_hq-\|midi' | head -1)
             else newest=$(ls -t test/accuracy/baselines/*.json | grep -v '_hq-\|midi\|_[0-9]\{4,5\}-' | head -1); fi;;
    hq)      SUF="-hq"; HQFLAG="--hq-mixer";
             if [ -n "$RSUF" ]; then newest=$(ls -t test/accuracy/baselines/*_hq-${RATEPAT}*.json 2>/dev/null | grep -v midi | head -1)
             else newest=$(ls -t test/accuracy/baselines/*_hq-*.json 2>/dev/null | grep -v 'midi\|_hq-[0-9]\{4,5\}-' | head -1); fi;;
    *)       echo "MIXER must be vanilla or hq" >&2; exit 2;;
esac
SUF=$SUF$RSUF
ROMDIR=build/accuracy/rom$SUF
BASE=${1:-$newest}
OUT=${2:-build/scratch/claim$SUF}
mkdir -p "$OUT"
status=0
stage() { printf '%-14s %s\n' "$1" "$2"; }

if cmake --build build --target poryaaaa_render poryaaaa_unit_tests poryaaaa_resampler_tests poryaaaa_song_tests > "$OUT/build.log" 2>&1; then
    stage build ok
else
    stage build "FAILED (see $OUT/build.log)"; exit 1
fi
u=$(build/poryaaaa_unit_tests 2>&1 | tee "$OUT/unit.log" | tail -1); stage unit "$u"; echo "$u" | grep -q 'passed' || status=1
grep -q 'FAIL' "$OUT/unit.log" && { stage unit "FAILURES (see $OUT/unit.log)"; status=1; }
b=$(build/poryaaaa_resampler_tests 2>&1 | tee "$OUT/resampler.log" | tail -1); stage resampler "$b"; echo "$b" | grep -q ' 0 failures' || status=1
s=$(build/poryaaaa_song_tests $ROMDIR 2>&1 | tee "$OUT/song.log" | tail -1); stage songs "$s"; echo "$s" | grep -q 'match' || status=1

# ---- write-cycle diff of every probed test ----
players=""
if [ -f "$HOME/pokeemerald/sound/song_table.inc" ]; then
    players=$(sed -nE 's/^\s*song\s+(se_\w+)\s*,\s*MUSIC_PLAYER_(\w+)\s*,.*/\1 \2/p' "$HOME/pokeemerald/sound/song_table.inc" | tr 'A-Z' 'a-z')
fi
cries=$(python3 -c '
import json
for s in json.load(open("test/accuracy/songs.json")):
    if s.get("type") == "cry" and "id" in s:
        extra = " ".join("--cry-%s %d" % (k, s[k]) for k in ("pitch", "pan", "volume", "length", "release", "chorus") if k in s)
        if "id2" in s:
            extra += " --cry2 %d --cry2-start-frame %d" % (s["id2"], s["start_frame2"])
        print(s["name"], "--cry %d %s" % (s["id"], extra))')
# SE / song entries with a cry over them: name, the song it plays, the two flags.
overs=$(python3 -c '
import json
for s in json.load(open("test/accuracy/songs.json")):
    if s.get("type", "song") in ("se", "song") and "id2" in s and "start_frame2" in s:
        print(s["name"], s.get("song", s["name"]), "--cry2 %d --cry2-start-frame %d" % (s["id2"], s["start_frame2"]))')
moved=0; checked=0
for f in build/probe$SUF/*.txt; do
    n=$(basename "$f" .txt)
    [ -f "$f.frames" ] || continue
    frames=$(grep -c '^frame' "$f.frames")
    args=""
    case $n in
        cry_over_*) over=$(echo "$overs" | awk -v n="$n" '$1 == n { print }')
                    [ -n "$over" ] || continue
                    song=$(echo "$over" | awk '{ print $2 }')
                    src=songs/$song.s
                    args=$(echo "$over" | awk '{ $1 = ""; $2 = ""; print }')
                    p=$(echo "$players" | awk -v n="$song" '$1 == n { print $2 }')
                    [ -n "$p" ] && args="--player $p $args";;
        cry_*)      src=""
                    args=$(echo "$cries" | awk -v n="$n" '$1 == n { $1 = ""; print }')
                    [ -n "$args" ] || continue;;
        littleroot) src=songs/mus_littleroot.s;;
        groudon)    src=songs/mus_weather_groudon.s;;
        credits|mt_chimney|rg_vs_trainer|vs_trainer|vs_wild) src=songs/mus_$n.s;;
        mus_*)      src=songs/$n.s;;
        se_*)       src=songs/$n.s
                    p=$(echo "$players" | awk -v n="$n" '$1 == n { print $2 }')
                    [ -n "$p" ] && args="--player $p";;
        *)          src=micro/$n.s;;
    esac
    if [ -n "$src" ]; then
        [ -f "$ROMDIR/$src" ] || continue
        src="--song $ROMDIR/$src"
    fi
    if [ "$frames" -gt 1000 ]; then d=20; elif [ "$frames" -gt 500 ]; then d=10; elif [ "$frames" -gt 200 ]; then d=4.0; elif [ "$frames" -gt 100 ]; then d=3.0; else d=1.2; fi
    if ! build/poryaaaa_render build/accuracy/project $src $args $HQFLAG $RATEFLAGS --output "$OUT/$n.wav" \
            --sample-rate 48000 --total-duration-seconds $d --fadeout 0 --tail 0 \
            --psg-log "$OUT/$n.engine.log" > "$OUT/$n.render.log" 2>&1; then
        stage "diff $n" "RENDER FAILED"; status=1; continue
    fi
    line=$(test/accuracy/tools/probe_diff.py "$f" "$OUT/$n.engine.log" 3 0 | tail -1)
    max=$(echo "$line" | sed -nE 's/.*: ([0-9]+) cycles.*/\1/p')
    checked=$((checked + 1))
    if [ -n "$max" ] && [ "$max" != 0 ]; then
        stage "diff $n" "$line"; moved=$((moved + 1))
    fi
done
stage writes "$checked tests diffed, $moved with a moved write"
[ "$moved" = 0 ] || status=1

# ---- harness ----
test/accuracy/tools/accuracy.py --mixer $MIXER --pcm-rate $PCM_RATE > "$OUT/harness.txt" 2>&1
stage harness "$(grep 'pass at' "$OUT/harness.txt")"
test/accuracy/tools/compare_reports.py "$BASE" build/accuracy/report$SUF.json > "$OUT/compare.txt" 2>&1
stage compare "$(tail -1 "$OUT/compare.txt") vs $(basename "$BASE"); rows that moved:"
awk -F'|' 'NR > 1 && NF > 2 && $1 ~ /[a-z]/ { split($1, a, " "); if (a[4] != "-" && a[4] != "0.0") print "               " $0 }' "$OUT/compare.txt"
grep -q '^0 regression' "$OUT/compare.txt" || status=1
exit $status
