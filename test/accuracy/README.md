# poryaaaa accuracy harness

Null-tests poryaaaa against mgba 0.10.5 playing the same compiled song data,
per `docs/ACCURACY_PLAN.md` (milestone 1). Everything here is optional
tooling; the plugin builds without it.

## Layout

| Path | What |
|------|------|
| `rom/` | Standalone GBA test ROM: pokeemerald's `m4a.c`/`m4a_1.s`/`m4a_tables.c` built with agbcc, the full vanilla sound data and song table, the game's sound-mode init (`m4aSoundInit`, VCOUNT-150 `m4aSoundVSync`, VBLANK `m4aSoundMain`), plus the micro-test voicegroup (`accuracy_micro.inc`) and songs (`micro/*.mid`, compiled by mid2agb with the options in `micro/micro.cfg`). A 32-byte parameter block tagged `PORYACC1` at ROM offset 0xC0 selects what to play (song/SE id, cry index, or micro index, plus cry pitch/pan/volume and the start frame); the harness patches it, so one build serves every test. |
| `ref/mgba_render.c` | Headless reference renderer linked against `libmgba-static.a`: patches the parameter block, runs N frames from reset, writes 16-bit stereo WAV at the requested rate (48 kHz by default). Deterministic: no user config, idle-loop detection off, unity master volume. The raw DAC stream (`mAVStream.postAudioFrame`, 65536 Hz) is resampled with the engine's own `plugin/m4a_blip.c` so both sides share one bit-exact resampler; `--native-resampler` writes mgba's blip_buf (sinc) output instead. The two modes report different `--version` strings and are cached apart. |
| `tools/metrics.py` | Alignment (energy-envelope cross-correlation, then sample-level refinement) and the null-depth metrics: RMS of the difference relative to the reference in dB over the whole file and the worst 1 s window, raw and after a least-squares gain match, plus per-segment lag drift (tempo mismatch). |
| `tools/accuracy.py` | Runner: builds an overlay project for poryaaaa's voicegroup loader, renders references (cached by ROM hash + mgba version), renders poryaaaa (`--input song`, the default: the compiled `.s` from the ROM build through `poryaaaa_render --song`, byte-identical to what mgba plays; `--input midi`: the MIDI with the mid2agb options mapped to CLI flags), compares, prints a table and writes `report.json`. |
| `tools/compare_reports.py` | Diff two reports; flags regressions over 1 dB. |
| `ref/mgba_probe.c` | libmgba debugger probe (built by hand, see its header): logs every sound/DMA/timer IO write and the entry of the m4a driver functions with the CPU cycle; `<out>.frames` summarizes each frame (driver entry cycles, ply_* counts, track flags, c15, PSG writes) and `<out>.events` mgba's own scheduler lateness. Source of the SoundMain timing model's numbers. |
| `tools/probe_costs.py` | Tabulates probe logs: CgbSound entry by frame work, per-command deltas, write offsets inside CgbSound. |
| `tools/probe_diff.py` | Diffs the PSG register writes of a probe log against `poryaaaa_render --song ... --psg-log <file>`, write by write, in cycles. The check that the timing model reproduces mgba's write cycles. |
| `../test_song_parser.c` (`poryaaaa_song_tests`) | Byte-exact check of the `.s` parser (`plugin/m4a_song.c`) against the ROM: every song under `build/accuracy/rom/{micro,songs}` is parsed, relocated to its address from `accuracy_test.map` and compared with `accuracy_test.gba`. Runs as part of the `accuracy` target. |
| `tools/gen_micro_midis.py` | Generates `rom/micro/*.mid` and `micro.cfg`. Re-run after editing; then rebuild the ROM. |
| `songs.json` | Vanilla corpus (name, type, seconds, note). Songs: PSG + DirectSound (littleroot), pure PSG (weather_groudon), DirectSound-heavy battle (vs_wild), noise (mt_chimney), FRLG drumset (rg_vs_trainer), MEMACC (vs_trainer), long (credits, 60 s). Sound effects (`"type": "se"`, played by `m4aSongNumStart` on the player `sound/song_table.inc` names, which the runner passes to `poryaaaa_render --player`): se_exp and se_faint (SE1, 3 tracks), se_contest_place and se_truck_unload (SE2, 9 tracks), se_low_health and se_rain_stop (SE3, 1 track). No vanilla music uses compressed samples; the `pcm_cmp` micro-test covers DPCM until cries arrive. |
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
test/accuracy/tools/accuracy.py [--input song|midi] [--tests micro|song|name,...] [--gate -40] [--update-refs] [-- extra poryaaaa_render args]
build/poryaaaa_song_tests build/accuracy/rom        # parser vs ROM, byte for byte
```

The sequencer (`plugin/m4a_sequencer.c`, a port of MPlayMain) plays the
compiled song inside poryaaaa's VBlank tick, so with `--input song` the
only differences left between the two renders are in synthesis: the mixer
and output stage (milestones 2 and 3).  `--input midi` keeps the plugin's
MIDI path measurable (milestone 5 compares the two).

Outputs land in `build/accuracy/`: `refs/<romhash-mgba>/` (reference WAVs),
`out/` (poryaaaa WAVs and logs), `project/` (overlay), `report.json`.

## Adding tests

- Micro-test: add a voice to `rom/accuracy_micro.inc` if needed, add a song in
  `tools/gen_micro_midis.py`, re-run it, rebuild the ROM, run with
  `--update-refs`.
- Vanilla song: append to `songs.json`; the id comes from
  `include/constants/songs.h` and the mid2agb options from `midi.cfg`.
- Sound effect: append `{"name": "se_xxx", "type": "se", "seconds": N}`;
  the music player (SE1 / SE2 / SE3, 3 / 9 / 1 tracks) comes from
  `sound/song_table.inc`, the compiled `.s` is already under
  `build/accuracy/rom/songs/`.  `--tests se` runs them alone.
- Cries (milestone 4, next slice): the ROM already accepts `--type cry --id N`
  (cry table index, `PlayCryInternal` defaults); the runner has no entry
  type for them yet.
