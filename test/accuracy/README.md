# poryaaaa accuracy harness

Null-tests poryaaaa against mgba 0.10.5 playing the same compiled song data,
per `docs/ACCURACY_PLAN.md` (milestone 1). Everything here is optional
tooling; the plugin builds without it.

## Layout

| Path | What |
|------|------|
| `rom/` | Standalone GBA test ROM: pokeemerald's `m4a.c`/`m4a_1.s`/`m4a_tables.c` built with agbcc, the full vanilla sound data and song table, the game's sound-mode init (`m4aSoundInit`, VCOUNT-150 `m4aSoundVSync`, VBLANK `m4aSoundMain`), plus the micro-test voicegroup (`accuracy_micro.inc`) and songs (`micro/*.mid`, compiled by mid2agb with the options in `micro/micro.cfg`). A 32-byte parameter block tagged `PORYACC1` at ROM offset 0xC0 selects what to play (song/SE id, cry index, or micro index, plus cry pitch/pan/volume and the start frame); the harness patches it, so one build serves every test. |
| `ref/mgba_render.c` | Headless reference renderer linked against `libmgba-static.a`: patches the parameter block, runs N frames from reset, writes 16-bit stereo WAV at the requested rate (48 kHz by default). Deterministic: no user config, idle-loop detection off, unity master volume. |
| `tools/metrics.py` | Alignment (energy-envelope cross-correlation, then sample-level refinement) and the null-depth metrics: RMS of the difference relative to the reference in dB over the whole file and the worst 1 s window, raw and after a least-squares gain match, plus per-segment lag drift (tempo mismatch). |
| `tools/accuracy.py` | Runner: builds an overlay project for poryaaaa's voicegroup loader, renders references (cached by ROM hash + mgba version), renders poryaaaa with the mid2agb options mapped to CLI flags, compares, prints a table and writes `report.json`. |
| `tools/gen_micro_midis.py` | Generates `rom/micro/*.mid` and `micro.cfg`. Re-run after editing; then rebuild the ROM. |
| `songs.json` | Vanilla song corpus (name, seconds). |
| `refs/micro/` | Committed micro-test references (FLAC) and `manifest.json` (ROM hash, mgba version). Lets the micro-tests run without an mgba build. |
| `baselines/` | Saved reports. |

## Requirements

- `~/pokeemerald` (or `POKEEMERALD=`) with `tools/agbcc` installed and its
  tools built (`mid2agb`, `preproc`, `gbafix`, `libagbsyscall`), and
  `arm-none-eabi` binutils.
- An mgba 0.10.5 source tree built with `BUILD_STATIC=ON`
  (`build/libmgba-static.a`).
- Python 3 with numpy and scipy; `soundfile` only to write or read the
  committed FLAC references.

## Running

```sh
cmake -B build -DACCURACY_POKEEMERALD_DIR=$HOME/pokeemerald -DACCURACY_MGBA_DIR=$HOME/mgba
cmake --build build --target accuracy          # ROM + mgba_render + poryaaaa_render + comparison
# or by hand:
make -C test/accuracy/rom POKEEMERALD=$HOME/pokeemerald
test/accuracy/tools/accuracy.py [--tests micro|song|name,...] [--gate -40] [--update-refs] [-- extra poryaaaa_render args]
```

Outputs land in `build/accuracy/`: `refs/<romhash-mgba>/` (reference WAVs),
`out/` (poryaaaa WAVs and logs), `project/` (overlay), `report.json`.

## Adding tests

- Micro-test: add a voice to `rom/accuracy_micro.inc` if needed, add a song in
  `tools/gen_micro_midis.py`, re-run it, rebuild the ROM, run with
  `--update-refs`.
- Vanilla song: append to `songs.json`; the id comes from
  `include/constants/songs.h` and the mid2agb options from `midi.cfg`.
- Cries and SE (milestone 4): the ROM already accepts `--type cry --id N`
  (cry table index, `PlayCryInternal` defaults) and `--type song` with SE ids.
