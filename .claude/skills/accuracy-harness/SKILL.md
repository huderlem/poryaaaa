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
test/accuracy/tools/accuracy.py --tests micro      # or song, or name,name
test/accuracy/tools/accuracy.py --input midi       # the plugin's MIDI path instead of the .s
test/accuracy/tools/accuracy.py --committed-refs   # no mgba build needed, micro-tests only
build/poryaaaa_song_tests build/accuracy/rom       # .s parser vs ROM, byte for byte
```

`--input song` (default since the sequencer milestone) plays
`build/accuracy/rom/{micro,songs}/<name>.s` through `poryaaaa_render --song`,
so poryaaaa and mgba consume byte-identical song data; `--input midi` plays
the MIDI with the mid2agb options as flags.  The two are not expected to
null each other below a few dB on PCM: a note that starts at a different
phase of the 13379 Hz mix grid gives a different sub-sample waveform.

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
   in between produces a two-test baseline; this happened once):
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
bit-exact tests. For each candidate:

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
mp = open('test/accuracy/rom/build/accuracy_test.map').read()
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
arm-none-eabi-nm test/accuracy/rom/build/accuracy_test.elf > build/probe/syms.nm
cat >> build/probe/syms.nm <<'EOF2'
08001020 t CgbSound_loop
08001036 t CgbSound_active
080010b0 t CgbSound_step2
080010c8 t CgbSound_start
080011a6 t CgbSound_nostart
080011ca t CgbSound_oscoff
08001218 t CgbSound_step
0800122c t CgbSound_modvol
0800130c t CgbSound_stepdone
0800131e t CgbSound_envdone
08001328 t CgbSound_pit
080013a0 t CgbSound_vol
0800141c t CgbSound_done
08001420 t CgbSound_next
EOF2
```
(addresses of the 2026-10-24 ROM; re-derive from `arm-none-eabi-objdump -d`
of CgbSound if m4a.c or the toolchain changes -- the header of mgba_probe.c
names each one).  The map still works for the function entries alone.

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
song file for "groudon"). `d` is engine minus mgba in cycles, per write; a
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
- Cry or SE: the ROM and `mgba_render` already accept `--type cry --id N`
  and SE song ids; the runner needs a test entry type for them (milestone 4).

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
