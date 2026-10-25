#!/bin/bash
# One-shot check of an engine or timing-model claim (accuracy-harness skill,
# "Testing a review claim or a constant change"): rebuild, unit / blip / song
# tests, the PSG write-cycle diff of every probed test in build/probe, then
# the harness against a baseline.
#
#   test/accuracy/tools/claim_check.sh [<baseline.json>] [<out dir>]
#
# Defaults: the newest file in test/accuracy/baselines/, build/scratch/claim.
# Prints one summary line per stage; details land in <out dir>.  Exit status
# is nonzero when a stage fails, a write moves, or the harness regresses.
#
# The write diff needs the probe logs (build/probe/<name>.txt from
# mgba_probe; not committed) and renders each test for the probe's length:
# micro-tests 1.2 s (70 frames) or 3 s (180), songs 20 s (1200) or 10 s
# (600), SEs 3 s with --player.  tune_psg's 1-cycle DAC rounding is
# expected and ignored.
set -u
cd "$(dirname "$0")/../../.."
BASE=${1:-$(ls -t test/accuracy/baselines/*.json | head -1)}
OUT=${2:-build/scratch/claim}
mkdir -p "$OUT"
status=0
stage() { printf '%-14s %s\n' "$1" "$2"; }

if cmake --build build --target poryaaaa_render poryaaaa_unit_tests poryaaaa_blip_tests poryaaaa_song_tests > "$OUT/build.log" 2>&1; then
    stage build ok
else
    stage build "FAILED (see $OUT/build.log)"; exit 1
fi
u=$(build/poryaaaa_unit_tests 2>&1 | tee "$OUT/unit.log" | tail -1); stage unit "$u"; echo "$u" | grep -q 'passed' || status=1
grep -q 'FAIL' "$OUT/unit.log" && { stage unit "FAILURES (see $OUT/unit.log)"; status=1; }
b=$(build/poryaaaa_blip_tests 2>&1 | tee "$OUT/blip.log" | tail -1); stage blip "$b"; echo "$b" | grep -q ' 0 failures' || status=1
s=$(build/poryaaaa_song_tests build/accuracy/rom 2>&1 | tee "$OUT/song.log" | tail -1); stage songs "$s"; echo "$s" | grep -q 'match' || status=1

# ---- write-cycle diff of every probed test ----
players=""
if [ -f "$HOME/pokeemerald/sound/song_table.inc" ]; then
    players=$(sed -nE 's/^\s*song\s+(se_\w+)\s*,\s*MUSIC_PLAYER_(\w+)\s*,.*/\1 \2/p' "$HOME/pokeemerald/sound/song_table.inc" | tr 'A-Z' 'a-z')
fi
moved=0; checked=0
for f in build/probe/*.txt; do
    n=$(basename "$f" .txt)
    [ -f "$f.frames" ] || continue
    frames=$(grep -c '^frame' "$f.frames")
    args=""
    case $n in
        littleroot) src=songs/mus_littleroot.s;;
        groudon)    src=songs/mus_weather_groudon.s;;
        credits|mt_chimney|rg_vs_trainer|vs_trainer|vs_wild) src=songs/mus_$n.s;;
        mus_*)      src=songs/$n.s;;
        se_*)       src=songs/$n.s
                    p=$(echo "$players" | awk -v n="$n" '$1 == n { print $2 }')
                    [ -n "$p" ] && args="--player $p";;
        *)          src=micro/$n.s;;
    esac
    [ -f "build/accuracy/rom/$src" ] || continue
    if [ "$frames" -gt 1000 ]; then d=20; elif [ "$frames" -gt 500 ]; then d=10; elif [ "$frames" -gt 100 ]; then d=3.0; else d=1.2; fi
    if ! build/poryaaaa_render build/accuracy/project --song "build/accuracy/rom/$src" $args --output "$OUT/$n.wav" \
            --sample-rate 48000 --total-duration-seconds $d --fadeout 0 --tail 0 \
            --psg-log "$OUT/$n.engine.log" > "$OUT/$n.render.log" 2>&1; then
        stage "diff $n" "RENDER FAILED"; status=1; continue
    fi
    line=$(test/accuracy/tools/probe_diff.py "$f" "$OUT/$n.engine.log" 3 0 | tail -1)
    max=$(echo "$line" | sed -nE 's/.*: ([0-9]+) cycles.*/\1/p')
    checked=$((checked + 1))
    if [ -n "$max" ] && [ "$max" != 0 ] && ! { [ "$n" = tune_psg ] && [ "$max" = 1 ]; }; then
        stage "diff $n" "$line"; moved=$((moved + 1))
    fi
done
stage writes "$checked tests diffed, $moved with a moved write"
[ "$moved" = 0 ] || status=1

# ---- harness ----
test/accuracy/tools/accuracy.py > "$OUT/harness.txt" 2>&1
stage harness "$(grep 'pass at' "$OUT/harness.txt")"
test/accuracy/tools/compare_reports.py "$BASE" build/accuracy/report.json > "$OUT/compare.txt" 2>&1
stage compare "$(tail -1 "$OUT/compare.txt") vs $(basename "$BASE"); rows that moved:"
awk -F'|' 'NR > 1 && NF > 2 && $1 ~ /[a-z]/ { split($1, a, " "); if (a[4] != "-" && a[4] != "0.0") print "               " $0 }' "$OUT/compare.txt"
grep -q '^0 regression' "$OUT/compare.txt" || status=1
exit $status
