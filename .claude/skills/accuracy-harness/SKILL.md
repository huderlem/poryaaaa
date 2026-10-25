---
name: accuracy-harness
description: Run and interpret the poryaaaa-vs-mgba null-test harness (test/accuracy), save baselines, attribute a sub--40 dB residual (sample diff, mgba_probe lateness), check loader data against the ROM, and debug PSG write timing with mgba_probe / --psg-log / probe_diff.py. Use before and after any engine or loader change, when asked about accuracy numbers, baselines, references, PSG timing, or to add a micro-test or corpus song.
---

# Accuracy harness

Ground truth is mgba 0.10.5 playing a test ROM built from pokeemerald's m4a
engine; poryaaaa renders the same material and `test/accuracy/tools/accuracy.py`
reports the null depth. Plan and decisions: `docs/ACCURACY_PLAN.md` (treat
as settled). Details: `test/accuracy/README.md`.

## Run

```sh
# one-time configure (paths are cached)
cmake -B build -DACCURACY_POKEEMERALD_DIR=$HOME/pokeemerald -DACCURACY_MGBA_DIR=$HOME/mgba
# ROM + mgba_render + poryaaaa_render + comparison
cmake --build build --target accuracy
# re-run only the comparison (after rebuilding poryaaaa_render yourself)
test/accuracy/tools/accuracy.py                    # all tests, compiled-song input (default)
test/accuracy/tools/accuracy.py --tests micro      # or song, se, or name,name
test/accuracy/tools/accuracy.py --input midi       # the plugin's MIDI path instead of the .s
test/accuracy/tools/accuracy.py --input midi --midi-quantize   # ... with poryaaaa_render --quantize (the frame latch)
test/accuracy/tools/accuracy.py --input midi --no-gba-start-delay  # ... with the first MIDI note at output time 0
test/accuracy/tools/accuracy.py --input midi --gba-tempo-grid  # ... with each MIDI tick on the ROM's tempoC / 150 frame
test/accuracy/tools/accuracy.py --committed-refs   # no mgba build needed, micro-tests only
build/poryaaaa_song_tests build/accuracy/rom       # .s parser vs ROM, byte for byte
```

`--input song` (default since the sequencer milestone) plays
`build/accuracy/rom/{micro,songs}/<name>.s` through `poryaaaa_render --song`,
so poryaaaa and mgba consume byte-identical song data; `--input midi` plays
the MIDI with the mid2agb options as flags (cries and the cry-over rows are
skipped: no MIDI).  `--midi-quantize` adds `poryaaaa_render --quantize`,
the MIDI event latch (milestone 5 slice 1: note on / off, CC, program,
bend, XCMD applied at the top of the next VBlank tick, in arrival order,
after the gate walk; a note-off latched into the same frame as its note-on
gives the GBA's one-tick note).  Every midi run passes `poryaaaa_render
--gba-start-delay` (slice 2a; the renderer's own default is off, and
`--no-gba-start-delay` runs the harness without it): MIDI time 0 where
the ROM starts the song, frame 3's SoundMain (883882 cycles), every event dispatched 2522 samples
later at 48 kHz -- the last sample a blip frame (2048 cycles) before that
SoundMain, since the machine sits up to a blip frame past a sample's cycle
when the sample is dispatched; the nominal 2529 would start every first
note on frame 4.  **The midi-input reports are their own baseline family**
(`baselines/<date>_midi-path-*.json`, the report's `"input"` field says
`midi`, `"midi_quantize"` and `"gba_start_delay"` say which form; the two
2026-10-25 baselines without the delay are `midi-path-start` / `-latch`,
with it `midi-path-aligned` / `-aligned-latch`, with it and
`--gba-tempo-grid` `midi-path-tempo-grid` / `-tempo-grid-latch`, the
report's `"gba_tempo_grid"` true): never
compare one against a song-input baseline with `compare_reports.py`, and
never save a midi run as the song-input baseline.  What separates the two
paths today is not the mix grid (both paths mix the same SoundMainRAM
frame, bit-identical when a note lands on the same frame) but the MIDI
path's time origin (with `--no-gba-start-delay`, ROM cycle 0
where the song's tick 0 is frame 3's SoundMain: an event after the first
frame 3 frames early on ticks that are multiples of 4, 4 frames on
n = 1 mod 4 from 21, 2 mod 4 from 66, 3 mod 4 from 110; the delay
removes it) and its tempo grid (exact BPM where the ROM ticks on tempoC /
150: with the delay, tick n on frame 3 + ceil(1.2443 n)
against the ROM's 3 + ceil(1.25 n), one frame early on bend, the lfo
rows, psg_mix and vol's ramp ticks 110, 114, ..., 130, and the engine's
LFO one frame late on one tick in four, n = 0 mod 4; `--gba-tempo-grid`
puts every tick on the ROM's frame: bend -inf, vol -47.2 (the VOL on its
FINE tick, which ply_fine's unlink keeps from the releasing note), six
songs' drift 0.0 (groudon, uncorrelated, -0.5), the LFO grid left to
slice 3b).  Aligned, the eleven PCM rows without such an
event are bit-identical to their `--song` renders (-inf, pcm_cmp /
pcm_chord at their -55 floor); the PSG rows keep the fixed CgbSound entry
(same writes, -2667 on frame 3, -464 .. -472 on tick frames, -1388 ..
-1444 on note frames), and two SEs show the per-tick
command batching (a VOL before a note in one tick reaches only the new
note on the GBA, every channel on the MIDI path).  To take a midi row
apart, diff it against its own `--song` render, not the reference; plan,
"Milestone 5 slice 1", "slice 2a" and "slice 3a".

Output: a table on stdout and `build/accuracy/report.json`. poryaaaa WAVs and
logs are in `build/accuracy/out/`, references in `build/accuracy/refs/<romhash-mgba>/`.

## Read the table

- `null` / `worst1s`: 20·log10(RMS(ref − test) / RMS(ref)) over the file and
  the worst 1 s window, after time alignment, **no gain correction**. These
  gate PASS/FAIL (default −40 dB). 0 dB means "as different as the signal
  itself"; a correct render reads −40 or lower.
- `gm null` / `gm wrst`: same after a single least-squares gain. If `gm` is
  much better than raw, the error is a level offset, not a waveform error.
- `lag ms`: alignment applied to poryaaaa's render, chosen as the lag with
  the deepest raw null among the envelope-correlation estimate and the top
  waveform cross-correlation peaks. Since the output stage (milestone 2)
  it is 0.0 ms on every test and song: the engine reproduces the ROM's
  start-up timing itself. A nonzero lag now means the waveforms do not
  correlate (e.g. a pure-PSG song whose CgbSound write timing is off);
  look at them, do not trust that row.
- `drift`: lag slope in ms per second over 4 s segments (songs only). Any
  nonzero value is a tempo mismatch and makes `null` meaningless for that
  test -- unless the song does not correlate at all (null around 0 dB, e.g.
  pure-PSG mus_weather_groudon today), in which case the drift is noise.
- `ref` / `test`: RMS level in dBFS. A constant offset across all tests is
  the output-scale problem, see the plan.

## Workflow for an engine change

1. Run the harness **before** touching code; confirm it matches the latest
   file in `test/accuracy/baselines/`.
2. Make the change, rebuild `poryaaaa_render`, run again.
3. `test/accuracy/tools/compare_reports.py test/accuracy/baselines/<latest>.json build/accuracy/report.json`
   flags regressions over 1 dB.
4. When a milestone lands (and again after its review fixes, so the next
   agent's "reproduces exactly" check holds), save both the report and the
   table, then record the numbers in the plan's status section.
   `report.json` is whatever the **last** run wrote, so run the default
   harness immediately before copying (a `--tests` or `--input midi` run
   in between produces a two-test or a midi-input baseline; this happened
   once):
   ```sh
   test/accuracy/tools/accuracy.py > /tmp/full.txt
   cp build/accuracy/report.json test/accuracy/baselines/<date>_<milestone>.json
   grep -vE '^columns|^  null|^  gm|^report' /tmp/full.txt \
       > test/accuracy/baselines/<date>_<milestone>.txt
   test/accuracy/tools/compare_reports.py test/accuracy/baselines/<previous>.json \
       test/accuracy/baselines/<date>_<milestone>.json   # sanity: all 38 rows present
   ```

## Testing a review claim or a constant change

One change at a time, never a batch: in this project three "verified"
constant changes applied together once hid that one of them broke four
bit-exact tests. `test/accuracy/tools/claim_check.sh [<baseline.json>]
[<out dir>]` does steps 1-3 below in one run (build, the three test
binaries, the write-cycle diff of every probed test in `build/probe/`,
the harness and `compare_reports.py`; about five minutes). For each
candidate:

1. Apply it alone; rebuild `poryaaaa_render` and `poryaaaa_unit_tests`.
2. `accuracy.py`, then `compare_reports.py <latest baseline> build/accuracy/report.json`.
   Any test that was `-inf` must still be `-inf`.
3. For PSG timing changes also run the write-cycle diff below on the
   affected tests; the harness is insensitive to writes that repeat a
   register's value, the diff is not.
4. Regression: revert and record the claim as "rejected by measurement" in
   the plan with the numbers. Neutral but source-verified (e.g. an ordering
   rule read from mgba): adopt and say it was harness-neutral.
5. A claim that improves most tests but costs one `-inf` is **wrong**, not
   "mostly right": the -inf tests are the ones whose data you can check
   byte for byte.  The mixer milestone's look-ahead byte did exactly this
   (12 tests gained 6 dB, pcm_adsr and pcm_bass lost -inf) because the rule
   was reasoned from the converter's source instead of read from the ROM;
   the ROM gave the right rule in one step (next section).

## Checking loader data against the ROM

Any claim about what a sample, voice or header byte "is" gets settled by
reading the test ROM, not the converter's source or the plugin's loader.
Every symbol's address is in the map, and the ROM is the file mgba plays:

```python
import struct, re
rom = open('build/accuracy/rom/accuracy_test.gba', 'rb').read()
mp = open('build/accuracy/rom/accuracy_test.map').read()
addr = int(re.search(r'0x([0-9a-f]{16})\s+DirectSoundWaveData_sc88pro_flute\b', mp).group(1), 16) - 0x08000000
typ, status, freq, loopStart, size = struct.unpack_from('<HHIII', rom, addr)
data = addr + 16
print(rom[data + size: data + size + 4].hex())      # the bytes after the data
# compare with what the loader reads:
# open('~/pokeemerald/sound/direct_sound_samples/sc88pro_flute.bin','rb').read()[16:]
```

Micro-voice symbols are in `test/accuracy/rom/accuracy_micro.inc`
(`DirectSoundWaveData_*`, `Cry_*`, `ProgrammableWaveData_*`).  Do this
*before* editing the loader; it is six lines and replaces a harness
round-trip.  What the ROM holds for the look-ahead byte, the DPCM block
tails and the headers is recorded in docs/ACCURACY_PLAN.md ("Done: engine
milestone 3").


## PSG write timing: probe, log, diff

The method for anything that moves wave/noise/square tests or PSG songs.
Wave and noise phase run on the CPU clock, so a trigger a few cycles off
costs 20-40 dB; squares only need DAC-slot (256-cycle) accuracy.

**Build the probe** (not a CMake target; keep binaries and logs in
`build/probe/`):
```sh
mkdir -p build/probe
gcc -O1 -o build/probe/mgba_probe test/accuracy/ref/mgba_probe.c \
    -I$HOME/mgba/include -I$HOME/mgba/build/include \
    $HOME/mgba/build/libmgba-static.a -lz -lpng -lm -lpthread -lrt -ldl \
    /usr/lib/x86_64-linux-gnu/libEGL.so /usr/lib/x86_64-linux-gnu/libGL.so
```
**Symbols**: pass the ELF's `nm` listing, not the map, so the probe also
breaks on the local labels inside MPlayMain / ply_note (`_081DD*`) and on
the `CgbSound_*` addresses appended for the compiled CgbSound:
```sh
arm-none-eabi-nm build/accuracy/rom/accuracy_test.elf > build/probe/syms.nm
cat >> build/probe/syms.nm <<'EOF2'
080010dc t CgbSound_loop
080010f2 t CgbSound_active
0800116c t CgbSound_step2
08001184 t CgbSound_start
08001262 t CgbSound_nostart
08001286 t CgbSound_oscoff
080012d4 t CgbSound_step
080012e8 t CgbSound_modvol
080013c8 t CgbSound_stepdone
080013da t CgbSound_envdone
080013e4 t CgbSound_pit
0800145c t CgbSound_vol
080014d8 t CgbSound_done
080014dc t CgbSound_next
EOF2
```
(addresses of the 2026-10-25 ROM with the third-cry parameters, CgbSound
at 0x80010ac: slice 10a's StartTest grew main.c by 0x30 bytes and every ROM
symbol from AgbMain on moved by +0x30 with the bodies unchanged -- the
2026-10-25 ROM with the second-cry parameters had CgbSound at 0x800107c and
each label 0x30 lower, the one with the cry chorus parameter 0x800103c, the
2026-10-24 ROM with the length / release parameters 0x8001020, the one
before that 0x8000ff0.
Re-derive from `arm-none-eabi-objdump -d` of CgbSound if m4a.c, main.c or
the toolchain changes -- the header of mgba_probe.c names each one.
**Changing the ROM**: keep `AgbMain`'s boot path and per-frame loop
instruction-identical (the second-cry wait lives in `StartTest` for that
reason); two extra prologue instructions once moved the timer-0 start by 6
cycles and, through mgba's ordering of a timer overflow against the DAC
slot it coincides with, every DirectSound reference -- while the probe's
`.frames` and lateness stayed put.  Before the harness, render pcm,
pcm_cmp and cry_bulbasaur with `mgba_render` on the new ROM and `cmp` them
against the current references (plan, probe step 11).  The
ELF and map the CMake target writes are `build/accuracy/rom/accuracy_test.elf`
/ `.map`; `test/accuracy/rom/build/` is only a stale hand build if it
exists, do not read symbols from it.)  The map still works for the function
entries alone.

**Run it**: `build/probe/mgba_probe build/accuracy/rom/accuracy_test.gba
build/probe/<name>.txt <frames> <type> <id> build/probe/syms.nm`.
Type 2 = micro-test, id = its line number (from 0) in
`test/accuracy/rom/micro/micro.cfg`; type 0 = song, id from
`~/pokeemerald/include/constants/songs.h` (mus_littleroot 405,
mus_weather_groudon 444). 70 frames cover 1.2 s; a 3 s micro-test needs 180.
Outputs: `<name>.txt` raw log (breakpoints `B`, IO writes `W`, lateness
`L`), `<name>.txt.frames` one line per engine frame, `<name>.txt.events`
mgba scheduler lateness.

**Frame numbering**: engine frame k >= 1 starts at cycle
124773 + (k-1)*280896; VBlank line 160 starts at +197120 within it. All
`.frames` cycles are relative to that line-160 start. mgba's own
`frameCounter` (the `fN` in the raw log) is one higher.

**Log the engine's writes and diff them**: render from the harness's
overlay project `build/accuracy/project` (not `~/pokeemerald`: the micro
voicegroup only exists in the overlay):
```sh
build/poryaaaa_render build/accuracy/project --song build/accuracy/rom/micro/<name>.s \
    --output /tmp/x.wav --sample-rate 48000 --total-duration-seconds 3.0 \
    --fadeout 0 --tail 0 --psg-log build/probe/<name>.engine.log
test/accuracy/tools/probe_diff.py build/probe/<name>.txt build/probe/<name>.engine.log [max_lines] [threshold]
```
(songs: `build/accuracy/rom/songs/<name>.s`, `--total-duration-seconds 1.2`
for a 70-frame probe, 20 for a 1200-frame one; mus_weather_groudon is the
song file for "groudon"; an SE adds `--player se1|se2|se3`; a cry replaces
`--song <file>` with `--cry <id>`). `d` is engine minus mgba in cycles, per write; a
threshold of 0 lists every nonzero write. Lines marked `<<< differs` on
registers 90/94/98/9C are only the 32-bit vs byte logging of wave RAM.
Reading a uniform offset: the whole frame shifted = a sequencer cost
(`M4A_CYC_*` in `plugin/m4a_engine.h`, charged in `m4a_sequencer.c`); a
shift from one write onward within a channel = a driver offset in
`m4a_cgb_channel_tick` (`plugin/m4a_channel.c`); later channels shifted =
the earlier channel's duration (quiet cost, envelope pass, tail).

**Where a residual comes from** (the step-3 tools; each prints the first
divergence per frame):
```sh
# the sequencer: per-frame CgbSound entry, per-command and per-track positions
build/poryaaaa_render build/accuracy/project --song build/accuracy/rom/songs/mus_<x>.s \
    --output /tmp/x.wav --sample-rate 48000 --total-duration-seconds 20 --fadeout 0 --tail 0 \
    --seq-log build/probe/<x>.seq.log --psg-log build/probe/<x>.engine.log
test/accuracy/tools/entry_diff.py build/probe/<x>.txt.frames build/probe/<x>.seq.log
test/accuracy/tools/cmd_diff.py   build/probe/<x>.txt build/probe/<x>.seq.log   # ply_* entries
test/accuracy/tools/track_diff.py build/probe/<x>.txt build/probe/<x>.seq.log   # _081DD998 per track
# the driver: CgbSound pass costs by path/state, and every segment inside a pass
test/accuracy/tools/probe_cgb.py build/probe/<x>.txt [-v]
test/accuracy/tools/probe_cgb.py --segments build/probe/*.txt
# MPlayMain segments between the internal labels, keyed on the deciding state
test/accuracy/tools/probe_mplay.py build/probe/*.txt [frame ...]
```
A frame whose entry matches but whose writes are off is the driver
(`CGB_CYC_*` in `plugin/m4a_channel.c`); an entry that is off is the
sequencer (`M4A_CYC_*` in `plugin/m4a_engine.h`, charged in
`m4a_sequencer.c`), and cmd_diff / track_diff say at which command or
track.  Compare a stale `.engine.log` against nothing: a failed render
leaves the previous log in place (the 2026-10-24 "groudon stops at 1.2 s"
was a wrong song name in a helper script).  The FIFO refill DMA stalls the
CPU 18 cycles at frame cycle 15091 (and every 20064 after); both models
charge it (`m4a_gba_fifo_dma_stall`), so a +-18 on a long frame is a
missing or doubled stall, not a code-path cost.

**Deriving a cost from the probe** (`tools/probe_costs.py build/probe/*.txt`
tabulates it): the "CgbSound entry by frame work" table gives whole-frame
costs; compare like with like (no-tick frame vs tick frame: tempoC at entry
plus tempoI reaching 150; a steady second note vs the song-start frame,
which adds track start and setup commands). A ply_* breakpoint-to-next
delta is that handler plus MPlayMain's dispatch of the next command, so it
is the right per-command cost; the last delta before CgbSound also holds
MPlayMain's tail. Write offsets inside CgbSound are read from a frame where
the channel is the first active one (subtract the 120 prologue and 41 per
inactive channel before it). Record every new number in the plan's probe
section before using it, and never fit a constant to the harness.

Never run `--update-refs` to make numbers better. References change only
when the ROM changes (micro MIDIs, `accuracy_micro.inc`, ROM sources) or
the reference definition does (the refs path is keyed on the ROM hash and
`mgba_render --version`, which names the resampler: `mGBA 0.10.5+m4a_blip`
is mgba's DAC stream through the engine's own m4a_blip, the reference since
2026-10-24; `--native-resampler` gives mgba's sinc output for listening).

## Add a test

- Micro-test: add a voice to `test/accuracy/rom/accuracy_micro.inc` if
  needed, add a song in `tools/gen_micro_midis.py`, run it, rebuild the ROM
  (`cmake --build build --target accuracy_rom`), run the harness with
  `--update-refs`, commit the new FLAC in `test/accuracy/refs/micro/`.
- Vanilla song: append `{"name": "mus_xxx", "seconds": N}` to
  `test/accuracy/songs.json`. Id and mid2agb options are looked up
  automatically; the compiled `.s` is already in `build/accuracy/rom/songs/`.
- Sound effect: `{"name": "se_xxx", "type": "se", "seconds": N}` (2-4 s).
  The runner reads its player from `sound/song_table.inc` and passes
  `--player se1|se2|se3` to poryaaaa_render (3 / 9 / 1 tracks and the
  player's MPlayMain chain position, `m4a_sequencer_set_player`).  To probe
  one: `mgba_probe ... 180 0 <id> build/probe/syms.nm gMPlayInfo_SE1` (the
  eighth argument picks the player the `.frames` summary reads) and
  `probe_mplay.py --player SE1 ...`; render with `--player se1` for the
  write diff.
- Cry: `{"name": "cry_<species>", "type": "cry", "id": N, "seconds": S}`,
  N the gCryTable index (the `cry` lines of `sound/cry_tables.inc` from
  0, then the `cry_reverse` lines after `gCryTable_Reverse::` continuing
  the count: 388 + the forward cry index is the reversed cry, type 0x30 = CMP | REV,
  the same sample played backwards; the ROM's one block holds both
  labels, `voicegroup_cry_table_entry` reads it the same way and
  `poryaaaa_render --cry N` prints "reversed").  The ROM's `StartCry` plays `gCryTable[N]` with `PlayCryInternal`'s
  CRY_MODE_NORMAL defaults (`mgba_render --type cry --id N`); the runner
  passes `--cry N` to poryaaaa_render, which plays the cry song template
  (`plugin/m4a_cry.c`: gPokemonCrySongTemplate with the SetPokemonCry*
  setters applied, the data charged as IWRAM) with the cry's DPCM sample
  as voice 0 on cry player 0 (`--player cry0`, the one SetPokemonCryTone
  takes from silence).  No MIDI: `--input midi` skips cries.  To probe
  one: `mgba_probe ... 180 1 N build/probe/syms.nm gPokemonCryMusicPlayers`
  (type 1 = cry) and `probe_mplay.py --player CRY0 ...`; render with
  `--cry N` for the entry / cmd / track diffs (cries have no PSG writes).
  A second cry while the first plays: `mgba_render --id2 N
  --start-frame2 F` / the probe's fifteenth and sixteenth arguments after
  the six cry parameters (`... 15360 0 120 140 0 0 N F`) start gCryTable[N]
  at the defaults on VBlank F (start tick on engine frame F + 2); it takes
  cry player 1 (`SetPokemonCryTone`'s first free player) and the next free
  channel in index order; `gPokemonCryMusicPlayers[1]` as the eighth
  argument (`symbol[N]` = N * 0x40 past the symbol) makes the `.frames`
  summary read that player, `probe_mplay.py --player CRY1` prints its run.
  Frame with both playing = 1007 + (79 + body) per playing player, cry 1's
  body after cry 0's (plan, step 11).  poryaaaa: `poryaaaa_render --cry N
  --cry2 M --cry2-start-frame F` (slice 8b-i: a second sequencer chained
  after cry 0's, on engine tracks 2..3, its own template copy and
  voicegroup, the first free cry player, both busy refused); one
  `--seq-log` carries both players and `entry_diff` / `cmd_diff` /
  `track_diff` read it unchanged (the `c` / `t` lines' track number is the
  engine index, 2 / 3 for cry 1).  The two probed runs are `--cry 0
  --cry2 15 --cry2-start-frame 20` (3 s, `cry_second_bulbasaur_pidgey`)
  and `--cry 123 --cry2 381 --cry2-start-frame 60` (4 s,
  `cry_second_jynx_wailord`), the corpus entries of the same names
  (slice 8b-ii): a cry entry's optional `"id2"` / `"start_frame2"` pair
  (both or neither, as `mgba_render` names them) is the second cry's
  gCryTable index at the defaults and the VBlank it starts on; the runner
  passes `--id2 N --start-frame2 F` to `mgba_render` and `--cry2 N
  --cry2-start-frame F` to `poryaaaa_render`, the reference frame count
  stays `seconds -> frames + 2` (182 / 241), `--input midi` skips it like
  any cry, and `claim_check.sh` adds the two flags to its cry line and
  renders a 240-frame probe for 4 s.  Their residual is the one-cry
  rows' floor (-60 / -45 dB, every run a one-sample slot crossing).
  The same `--id2 / --start-frame2` (probe arguments 15 / 16) start a cry
  over a `--type song` run too -- an SE or a song, no ROM change, `StartTest`
  running the second-cry block after its type switch: `build/probe/
  cry_over_se.txt` (se_exp + Cry_Bulbasaur from VBlank 30, no steal) and
  `cry_over_bgm.txt` (mus_littleroot + Cry_Bulbasaur from VBlank 60: the
  cry steals a releasing BGM channel on frame 62), plan step 12.  poryaaaa
  plays them with `poryaaaa_render --song build/accuracy/rom/songs/se_exp.s
  --player se1 --cry2 0 --cry2-start-frame 30` / `--song .../mus_littleroot.s
  --cry2 0 --cry2-start-frame 60` (slice 9b-i: cry player 0 chained after
  the song's player, BGM -> cry 0 on engine tracks 10..11 / SE1 -> cry 0 on
  3..4, the steal unlinking the owner's chain at once; `--seq-log` /
  `--psg-log` and the diff tools as for two cries).  Their `songs.json`
  rows are `cry_over_se` / `cry_over_bgm` (slice 9b-ii): an `"se"` /
  `"song"` entry carrying the same `"id2"` / `"start_frame2"` pair plus
  `"song"` naming the SE / song it plays (`"se_exp"` / `"mus_littleroot"`;
  the entry is named after its probe log), both 3 s; the runner appends
  `--cry2 N --cry2-start-frame F` after `--player`, `--input midi` skips
  them, and `claim_check.sh` resolves a `cry_over_*` log against its entry
  and adds the two flags to that line.
  A third cry while both cry players are busy: `mgba_render --id3 N
  --start-frame3 F` / the probe's seventeenth and eighteenth arguments
  (`... 15360 0 120 140 0 0 <id2> <F2> <id3> <F3>`, AccParams block offsets
  24..26) start gCryTable[N] at the defaults on VBlank F3.  With neither
  cry player free, `SetPokemonCryTone` replaces the one with the strictly
  larger `clock` (ticks since its own MPlayStart; a tie takes player 0),
  and `MPlayStart`'s `TrackStop` cuts that player's chained channels
  outright between two SoundMains (`statusFlags := 0` in ROM code: no
  release, no `_081DCFB0` stamp; the `TrackStop_Loop` / `TrackStop_1`
  breakpoints under the `MPlayStart` of that VBlank show it).  The probed
  run is `build/probe/cry_third_jynx_wailord_bulbasaur.txt` / `_p1` (Jynx
  on cry 0, Wailord on cry 1 from VBlank 20, Bulbasaur from VBlank 60, 180
  frames; the twin without the third cry is
  `cry_third_alone_jynx_wailord.txt` / `_p1`): cry player 0 is replaced,
  Jynx cut on VBlank 60, Bulbasaur's start tick on frame 62 takes Jynx's
  channel 0 free (`SCAN_FREE` 14, body 2991 as a lone cry's, the frame
  4666 = 1007 + (79 + 2991) + (79 + 510)); plan step 13.  poryaaaa does
  not play it yet (`--cry2` while both are busy is refused; `--cry3` is
  slice 10b).  No `songs.json` row; `claim_check.sh` skips the
  `cry_third_*` logs by the `cry_` rule.
  Optional `"pitch"`, `"pan"`, `"volume"`, `"length"`, `"release"`,
  `"chorus"` keys (SetPokemonCryPitch / Panpot / Volume / Length / Release
  / Chorus's arguments; take the values from `PlayCryInternal`'s modes in
  `src/sound.c` as whole modes, never invented ones; a nonzero chorus
  makes the cry song two tracks on the cry player -- the second track's
  TUNE = (chorus + TUNE) & 0x7F, the game's u32 192 is the s8 -64 -- and
  the frame skeleton is the two-track one of plan step 9: start 6792,
  1932 with both channels alive, +93 on each unlink tick and -20 per dead
  track, the EOT 2360 on two empty chains and 2596 with a live channel on
  each chain, step 10) go to both renderers
  (`mgba_render --pitch/--pan/--volume/--length/--release/--chorus`,
  `poryaaaa_render --cry-pitch/--cry-pan/--cry-volume/--cry-length/
  --cry-release/--cry-chorus`) and to the probe as its ninth to fourteenth
  arguments after the player symbol.  A releasing channel's death frame is the
  probe's `_081DCFB0` stamp in the raw log (SoundMainRAM's `statusFlags =
  0`, broken at its IWRAM copy; raw `fN` = engine frame N-1; one stamp per
  channel, so a two-chain release gives two on one frame); the envelope
  is `env = env * release >> 8` per pass from the EOT frame, the stop on
  the pass whose product is 0.  A sample that runs out of data ends through
  the count path and gives no stamp: its death is the tick's unlink frame
  (1689) while the track lives.
  Cry samples are 10512 Hz DPCM, none loops, the longest (Jynx) ends
  before the 140-tick EOT; the residual of a cry is the PCM
  scheduler-lateness floor (one-sample runs at the `.events` slot
  crossings), large per sample because DPCM cry waveforms jump.  A
  reversed cry that runs out of data ends on the forward sample's first
  sample, whose look-behind is "block -1": the GBA reads open bus there
  (mgba: the IWRAM Unk2 prefetch word, decoding to 38) and the port a
  zero block, so a one-sample run at a reversed cry's end is that
  look-behind, not the decode (plan, probe step 8).

## Debugging a single test

```sh
build/mgba_render build/accuracy/rom/accuracy_test.gba /tmp/ref.wav --frames 240 --type micro --id 0
python3 -c "import sys; sys.path.insert(0,'test/accuracy/tools'); import metrics; print(metrics.compare_files('/tmp/ref.wav','build/accuracy/out/sq1.wav'))"
```
Micro-test ids are line numbers (from 0) in `test/accuracy/rom/micro/micro.cfg`
(`grep -n '' test/accuracy/rom/micro/micro.cfg`; sq1 0, pcm 10, pcm_cmp
14, pcm_chord 16, pcm_psg 30).  Song ids come from pokeemerald
`include/constants/songs.h`.

## Attributing a residual below -40 dB

A PCM test that nulls at -45..-60 instead of -inf differs in a few
samples.  Find them before suspecting the mixer: the reference is
`build/accuracy/refs/<romhash-mgba>/<test>.wav`, the render
`build/accuracy/out/<test>.wav`.

```python
import numpy as np, wave
def rd(p):
    w = wave.open(p); d = np.frombuffer(w.readframes(w.getnframes()), np.int16)
    return d.reshape(-1, w.getnchannels()).astype(np.int64), w.getframerate()
r, sr = rd('build/accuracy/refs/<romhash-mgba>/pcm_cmp.wav'); t, _ = rd('build/accuracy/out/pcm_cmp.wav')
n = min(len(r), len(t)); d = r[:n, 0] - t[:n, 0]
idx = np.nonzero(np.abs(d) > 20)[0]           # ignore the resampler's +-1 ringing
runs, a = [], idx[0]
for p, q in zip(idx, idx[1:]):
    if q - p > 40: runs.append((a, p)); a = q
runs.append((a, idx[-1]))
for a, b in runs: print(f"{a/sr:.4f}s len {(b-a+1)/sr*13379:.0f} pcm samples maxdiff {abs(d[a:b+1]).max()}")
```

Reading the runs (48 kHz output; one 13379 Hz PCM sample spans about 3.6
output samples and the band-limited resampler rings for about 5 ms; one
PCM LSB is about 192 in the int16 output):

- **A run of one PCM sample, a few per second, at no particular note
  event**: mgba scheduler lateness, not the engine.  Confirm with the
  probe: `build/probe/mgba_probe <rom> build/probe/<test>.txt 180 2 <id>
  <map>` and read the `timer0 ... slot_crossed=N (first at cycle C)` line
  of `build/probe/<test>.txt.events`; `C / 16777216` is the time of the
  first error run.  pcm_cmp has 9 such crossings, pcm_chord 6, pcm 0 (and
  pcm is -inf).  This floor is about -55 dB on the micro-tests and is the
  deferred item 1c in the plan; nothing in the engine fixes it.
- **A run of about 64 PCM samples at a note start**: one DPCM block, i.e.
  the decoder or the block data.
- **A run of one or two samples at the end of a sample or at every loop
  pass**: the look-ahead byte after the data (check the ROM, section
  above).
- **Whole frames (224 samples) off**: envelope or volume arithmetic.
- **Constant level offset (`gm null` much deeper than `null`)**: a scale
  constant.

## Hand-built channel tests

`test/test_engine.c` drives the mixer frame by frame with a hand-built
`M4APCMChannel` through `m4a_mixer_mix_channel(&e, &ch, frame)`, where
`frame` is `int8_t[2 * GBA_PCM_DMA_BUF_SIZE]` (right side at 0, left at
`+GBA_PCM_DMA_BUF_SIZE`).  Set `e.pcmDivFreq = 1` so `ch.frequency` is the
23-bit per-sample step (0x800000 = one source sample per output); the
engine's real divFreq is 627 at 13379 Hz, and a Golden Sun synth's phase
step is `(divFreq * frequency) << 3`.  Expected bytes at envelope volume
128 are `(sample * 128) >> 8`; see `test_mixer_arithmetic` for the
patterns (ror-8 packing, FIX frame, interpolating frame, loop wrap, lane
wrap, reverb rounding).
