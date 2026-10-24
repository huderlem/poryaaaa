---
name: accuracy-harness
description: Run and interpret the poryaaaa-vs-mgba null-test harness (test/accuracy). Use before and after any engine change, when asked about accuracy numbers, baselines, references, or to add a micro-test or corpus song.
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
test/accuracy/tools/accuracy.py                    # all tests
test/accuracy/tools/accuracy.py --tests micro      # or song, or name,name
test/accuracy/tools/accuracy.py --committed-refs   # no mgba build needed, micro-tests only
```

Output: a table on stdout and `build/accuracy/report.json`. poryaaaa WAVs and
logs are in `build/accuracy/out/`, references in `build/accuracy/refs/<romhash-mgba>/`.

## Read the table

- `null` / `worst1s`: 20·log10(RMS(ref − test) / RMS(ref)) over the file and
  the worst 1 s window, after time alignment, **no gain correction**. These
  gate PASS/FAIL (default −40 dB). 0 dB means "as different as the signal
  itself"; a correct render reads −40 or lower.
- `gm null` / `gm wrst`: same after a single least-squares gain. If `gm` is
  much better than raw, the error is a level offset, not a waveform error.
- `lag ms`: alignment applied to poryaaaa's render. Expected 50–120 ms
  (the ROM starts on frame 1 plus DMA latency). Values near 0 or near the
  window edge mean alignment failed; look at the waveforms, do not trust
  that row.
- `drift`: lag slope in ms per second over 4 s segments (songs only). Any
  nonzero value is a tempo mismatch and makes `null` meaningless for that test.
- `ref` / `test`: RMS level in dBFS. A constant offset across all tests is
  the output-scale problem, see the plan.

## Workflow for an engine change

1. Run the harness **before** touching code; confirm it matches the latest
   file in `test/accuracy/baselines/`.
2. Make the change, rebuild `poryaaaa_render`, run again.
3. `test/accuracy/tools/compare_reports.py test/accuracy/baselines/<latest>.json build/accuracy/report.json`
   flags regressions over 1 dB.
4. When a milestone lands, copy `build/accuracy/report.json` to
   `test/accuracy/baselines/<date>_<milestone>.json` and note the numbers in
   the plan's status section.

Never run `--update-refs` to make numbers better. References change only
when the ROM changes (micro MIDIs, `accuracy_micro.inc`, ROM sources), and
the ROM hash in the refs path tells you when that happened.

## Add a test

- Micro-test: add a voice to `test/accuracy/rom/accuracy_micro.inc` if
  needed, add a song in `tools/gen_micro_midis.py`, run it, rebuild the ROM
  (`cmake --build build --target accuracy_rom`), run the harness with
  `--update-refs`, commit the new FLAC in `test/accuracy/refs/micro/`.
- Vanilla song: append `{"name": "mus_xxx", "seconds": N}` to
  `test/accuracy/songs.json`. Id and mid2agb options are looked up
  automatically.
- Cry or SE: the ROM and `mgba_render` already accept `--type cry --id N`
  and SE song ids; the runner needs a test entry type for them (milestone 4).

## Debugging a single test

```sh
build/mgba_render build/accuracy/rom/accuracy_test.gba /tmp/ref.wav --frames 240 --type micro --id 0
python3 -c "import sys; sys.path.insert(0,'test/accuracy/tools'); import metrics; print(metrics.compare_files('/tmp/ref.wav','build/accuracy/out/sq1.wav'))"
```
Micro-test ids are line numbers (from 0) in `test/accuracy/rom/micro/micro.cfg`.
Song ids come from pokeemerald `include/constants/songs.h`.
