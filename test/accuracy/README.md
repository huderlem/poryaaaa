# poryaaaa accuracy harness

Null-tests poryaaaa against mgba 0.10.5 playing the same compiled song data,
per `docs/ACCURACY_PLAN.md` (milestone 1). Everything here is optional
tooling; the plugin builds without it.

## Layout

| Path | What |
|------|------|
| `rom/` | Standalone GBA test ROM: pokeemerald's `m4a.c`/`m4a_1.s`/`m4a_tables.c` built with agbcc, the full vanilla sound data and song table, the game's sound-mode init (`m4aSoundInit`, VCOUNT-150 `m4aSoundVSync`, VBLANK `m4aSoundMain`), plus the micro-test voicegroup (`accuracy_micro.inc`) and songs (`micro/*.mid`, compiled by mid2agb with the options in `micro/micro.cfg`). A parameter block tagged `PORYACC1` at ROM offset 0xC0 (`struct AccParams`, 28 bytes: type, cry volume, id, cry pitch, cry pan, start frame, cry length, cry release, cry chorus, second cry id, second cry start frame, a padding byte, third cry id, third cry start frame, 1 reserved) selects what to play (song/SE id, cry index, or micro index, plus the cry's `SetPokemonCry*` values, and optionally a second and a third cry at `PlayCryInternal`'s CRY_MODE_NORMAL defaults started on later VBlanks while the first plays -- `mgba_render --id2 N --start-frame2 F`, `--id3 N --start-frame3 F`, 0 = none; with both cry players busy the ROM's `SetPokemonCryTone` replaces the one with the larger clock, plan step 13); the harness patches it, so one build serves every test. `AgbMain` must stay textually identical when the block grows (the second and third cries' waits live in `StartTest`): two extra prologue instructions moved the timer-0 start by 6 cycles and every DirectSound reference with it (plan, probe step 11). |
| `ref/mgba_render.c` | Headless reference renderer linked against `libmgba-static.a`: patches the parameter block, runs N frames from reset, writes 16-bit stereo WAV at the requested rate (48 kHz by default). Deterministic: no user config, idle-loop detection off, unity master volume. The raw DAC stream (`mAVStream.postAudioFrame`, 65536 Hz) is resampled with the engine's own `plugin/m4a_blip.c` so both sides share one bit-exact resampler; `--native-resampler` writes mgba's blip_buf (sinc) output instead. The two modes report different `--version` strings and are cached apart. |
| `tools/metrics.py` | Alignment (energy-envelope cross-correlation, then sample-level refinement) and the null-depth metrics: RMS of the difference relative to the reference in dB over the whole file and the worst 1 s window, raw and after a least-squares gain match, plus per-segment lag drift (tempo mismatch). |
| `tools/accuracy.py` | Runner: builds an overlay project for poryaaaa's voicegroup loader, renders references (cached by ROM hash + mgba version), renders poryaaaa (`--input song`, the default: the compiled `.s` from the ROM build through `poryaaaa_render --song`, byte-identical to what mgba plays; `--input midi`: the MIDI with the mid2agb options mapped to CLI flags), compares, prints a table and writes `report.json`. |
| `tools/compare_reports.py` | Diff two reports; flags regressions over 1 dB. |
| `ref/mgba_probe.c` | libmgba debugger probe (built by hand, see its header): logs every sound/DMA/timer IO write and the entry of the m4a driver functions with the CPU cycle; `<out>.frames` summarizes each frame (driver entry cycles, ply_* counts, track flags, c15, PSG writes) and `<out>.events` mgba's own scheduler lateness. Source of the SoundMain timing model's numbers. |
| `tools/probe_costs.py` | Tabulates probe logs: CgbSound entry by frame work, per-command deltas, write offsets inside CgbSound. |
| `tools/probe_diff.py` | Diffs the PSG register writes of a probe log against `poryaaaa_render --song ... --psg-log <file>`, write by write, in cycles. The check that the timing model reproduces mgba's write cycles. |
| `../test_song_parser.c` (`poryaaaa_song_tests`) | Byte-exact check of the `.s` parser (`plugin/m4a_song.c`) against the ROM: every song under `build/accuracy/rom/{micro,songs}` is parsed, relocated to its address from `accuracy_test.map` and compared with `accuracy_test.gba`. Runs as part of the `accuracy` target. |
| `tools/gen_micro_midis.py` | Generates `rom/micro/*.mid` and `micro.cfg`. Re-run after editing; then rebuild the ROM. |
| `songs.json` | Vanilla corpus (name, type, seconds, note). Songs: PSG + DirectSound (littleroot), pure PSG (weather_groudon), DirectSound-heavy battle (vs_wild), noise (mt_chimney), FRLG drumset (rg_vs_trainer), MEMACC (vs_trainer), long (credits, 60 s). Sound effects (`"type": "se"`, played by `m4aSongNumStart` on the player `sound/song_table.inc` names, which the runner passes to `poryaaaa_render --player`): se_exp and se_faint (SE1, 3 tracks), se_contest_place and se_truck_unload (SE2, 9 tracks), se_low_health and se_rain_stop (SE3, 1 track). Cries (`"type": "cry"`, `"id"` = the gCryTable index; the ROM's `StartCry` with `PlayCryInternal`'s CRY_MODE_NORMAL defaults, `poryaaaa_render --cry N`: the cry song template on cry player 0, `plugin/m4a_cry.c`): cry_pidgey (the shortest sample), cry_bulbasaur, cry_wailord, cry_jynx (the longest; no cry sample loops), and three of `PlayCryInternal`'s other parameter values (`"pitch"` / `"pan"` / `"volume"` keys): cry_jynx_faint (pitch 14440, the sample outliving the 140-tick EOT), cry_bulbasaur_howl (pitch 15800, pan 63), cry_pidgey_quiet (volume 90), and three whole modes with their `"length"` / `"release"` (`SetPokemonCryLength` / `Release`): cry_jynx_faint_full (CRY_MODE_FAINT: pitch 14440, release 200, the EOT releasing the live channel over 18 frames), cry_wailord_doubles (CRY_MODE_DOUBLES: length 20, release 225, the slowest release over 30 frames), cry_pidgey_roar2 (CRY_MODE_ROAR_2: pitch 15616, length 60, release 225, the sample gone before the EOT), and three reversed cries (`gCryTable_Reverse`'s entries, type 0x30 = CMP \| REV, the same DPCM sample played backwards; their `"id"` continues the count past the 388 `cry` lines: 388 + the forward index): cry_bulbasaur_reverse (id 388, the defaults), cry_jynx_growl1 (id 511, CRY_MODE_GROWL_1: pitch 15200, length 15, release 125, a release tail on a reversed channel), cry_pidgey_echo_start (id 403, CRY_MODE_ECHO_START: pitch 15600, volume 90, length 25, release 100, as the one-track form with chorus 0), and three two-track (chorus) modes (`"chorus"` key, `SetPokemonCryChorus`: trackCount 2, the second track's TUNE = (chorus + TUNE) & 0x7F): cry_bulbasaur_encounter (id 0, CRY_MODE_ENCOUNTER: pitch 15600, chorus 20, volume 90, release 225), cry_jynx_echo_end (id 123, CRY_MODE_ECHO_END: pitch 15555, chorus 192, volume 70, release 220), cry_pidgey_echo_start_chorus (id 403, CRY_MODE_ECHO_START complete with its chorus 192, two reversed tracks), and two entries of CRY_MODE_HIGH_PITCH (length 50, release 200, pitch 15800, chorus 20, volume 90) on samples still sounding at tick 50, the EOT releasing a live channel on each of the two chains and the two release tails together: cry_jynx_high_pitch (id 123), cry_wailord_high_pitch (id 381), and two entries with a second cry started while the first plays (`"id2"` / `"start_frame2"` keys: gCryTable index at CRY_MODE_NORMAL's defaults, the VBlank it starts on; `mgba_render --id2 N --start-frame2 F`, `poryaaaa_render --cry2 N --cry2-start-frame F`, cry player 1 chained after cry 0's): cry_second_bulbasaur_pidgey (id 0, id2 15, start_frame2 20, 3 s) and cry_second_jynx_wailord (id 123, id2 381, start_frame2 60, 4 s); the same pair on an `"se"` / `"song"` entry, with `"song"` naming the SE / song it plays, is a cry over it (`poryaaaa_render --song ... --cry2 N --cry2-start-frame F`, cry player 0 chained after the song's player): cry_over_se (se_exp, id2 0, start_frame2 30, 3 s, no steal) and cry_over_bgm (mus_littleroot, id2 0, start_frame2 60, 3 s, the cry stealing a releasing BGM channel on frame 62). Cries are the only DPCM (compressed) material besides the `pcm_cmp` micro-test. |
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
- Cry: append `{"name": "cry_<species>", "type": "cry", "id": N, "seconds": S}`
  with N the gCryTable index (the `cry` lines of `sound/cry_tables.inc`, from
  0; the `cry_reverse` lines after the `gCryTable_Reverse::` label continue
  the count, 388 + the forward cry index in vanilla, since both labels share one block in
  the ROM -- `poryaaaa_render --cry N` prints "reversed" for them and loads
  the voice as type 0x30).  The ROM plays `gCryTable[N]` through `StartCry` (`mgba_render --type
  cry --id N`, `PlayCryInternal`'s CRY_MODE_NORMAL defaults: volume 120, pan
  0, pitch 15360, length 140, priority 10), poryaaaa through `poryaaaa_render
  --cry N` (the cry song template with the cry's DPCM sample as voice 0, on
  cry player 0).  Cries have no MIDI: `--input midi` skips them with a
  message.  `--tests cry` runs them alone.  Optional `"pitch"`, `"pan"`,
  `"volume"`, `"length"`, `"release"`, `"chorus"` keys are `SetPokemonCryPitch` /
  `Panpot` / `Volume` / `Length` / `Release` / `Chorus`'s arguments (the values
  `PlayCryInternal`'s modes use, taken as whole modes: pitches
  14440..15800, volumes 70 / 90 / 120, lengths 10..140, releases 100..225,
  choruses 20 and 192 -- the game's `u32` 192 is the setter's `s8` -64, the
  same byte; a nonzero chorus makes the cry song two tracks, the second's
  TUNE = (chorus + TUNE) & 0x7F); the runner passes them to both renderers
  (`mgba_render --pitch/--pan/--volume/--length/--release/--chorus`,
  `poryaaaa_render --cry-pitch/--cry-pan/--cry-volume/--cry-length/
  --cry-release/--cry-chorus`).  To probe one: `mgba_probe ... 180 1 N
  build/probe/syms.nm gPokemonCryMusicPlayers [pitch [pan [volume [length
  [release [chorus [id2 [startFrame2]]]]]]]]` and `probe_mplay.py --player
  CRY0`; render with `--cry N` (and the same `--cry-*` flags) for the diffs.
  The fifteenth / sixteenth arguments start a second cry (gCryTable index,
  the defaults) on VBlank startFrame2 while the first plays (its start tick
  is on engine frame startFrame2 + 2); it lands on cry player 1
  (`SetPokemonCryTone`'s first free player), whose `.frames` summary the
  eighth argument `gPokemonCryMusicPlayers[1]` selects (`symbol[N]` = N *
  0x40 past the symbol); `probe_mplay.py --player CRY1` prints its run.
  poryaaaa plays the same with `poryaaaa_render --cry N --cry2 M
  --cry2-start-frame F` (slice 8b-i: the second cry's template copy on cry
  player 1, a second sequencer chained after cry 0's on engine tracks
  2..3, its MPlayMain body after cry 0's on the same frame; the first free
  cry player by `SetPokemonCryTone`'s rule, both busy refused); one
  `--seq-log` holds both players' lines and the three diff tools read it
  as is.  In `songs.json` the second cry is the optional `"id2"` /
  `"start_frame2"` pair (both or neither; named as `mgba_render` names
  them): the runner passes `--id2 N --start-frame2 F` to `mgba_render` and
  `--cry2 N --cry2-start-frame F` to `poryaaaa_render`, the reference frame
  count stays `seconds -> frames + 2` (182 for 3 s, 241 for 4 s), and
  `--input midi` skips the entry like any cry.  The two entries are
  cry_second_bulbasaur_pidgey (id 0, id2 15, start_frame2 20, 3 s) and
  cry_second_jynx_wailord (id 123, id2 381, start_frame2 60, 4 s), the two
  probed runs (`build/probe/cry_second_*.txt`, 180 / 240 frames;
  `claim_check.sh` renders a 240-frame probe for 4 s and adds the two
  flags from the keys).
  A third cry: `mgba_render --id3 N --start-frame3 F` and the probe's
  seventeenth / eighteenth arguments (AccParams block offsets 24..26) start
  gCryTable[N] at the defaults on VBlank F while two cries play; neither
  cry player free, `SetPokemonCryTone` replaces the one with the strictly
  larger `clock` (ticks since its own start; a tie takes player 0) and
  `MPlayStart`'s `TrackStop` cuts its chained channels outright (no
  release, no `_081DCFB0` stamp).  The probed run
  `build/probe/cry_third_jynx_wailord_bulbasaur.txt` / `_p1` (Jynx, Wailord
  from VBlank 20, Bulbasaur from VBlank 60; twin
  `cry_third_alone_jynx_wailord.txt`) replaces cry player 0 and restarts it
  on frame 62 taking Jynx's channel 0 free (plan step 13, slice 10a; ground
  truth only -- poryaaaa's `--cry3` is slice 10b).
  `--id2 / --start-frame2` (and the probe's fifteenth / sixteenth
  arguments) also start that cry over a `--type song` run -- an SE or a
  song -- with no ROM change, since `StartTest` runs the second-cry block
  after its type switch: the probe logs `build/probe/cry_over_se.txt`
  (se_exp, Cry_Bulbasaur from VBlank 30) and `cry_over_bgm.txt`
  (mus_littleroot, from VBlank 60) are plan step 12 (slice 9a, ground
  truth only; the cry over music steals a releasing BGM channel on its
  start frame).  poryaaaa plays the same with `poryaaaa_render --song <.s>
  [--player bgm|se1|se2|se3] --cry2 N --cry2-start-frame F` (slice 9b-i: cry
  player 0 chained after the song's player -- BGM -> cry 0 on engine tracks
  10..11, SE1 -> cry 0 on 3..4 -- its body after the song's on the same
  frame, the cross-player steal unlinking the stolen channel from the
  owner's chain at once).  In `songs.json` these are the `"se"` / `"song"`
  entries cry_over_se (`"song": "se_exp"`, id2 0, start_frame2 30, 3 s) and
  cry_over_bgm (`"song": "mus_littleroot"`, id2 0, start_frame2 60, 3 s;
  the cry's EOT on frame 202 is past the row), named after the probe logs,
  with `"song"` naming the SE / song the entry plays (slice 9b-ii: the
  runner adds `--cry2 N --cry2-start-frame F` after `--player`, the
  reference frame count stays 182, `--input midi` skips them, and
  `claim_check.sh` resolves a `cry_over_*` probe log against its entry and
  adds the two flags to that SE / song line).  The probe's `_081DCFB0` breakpoint
  (SoundMainRAM's channel stop, at its IWRAM copy) stamps the frame a
  releasing channel dies on (one stamp per channel: a two-track cry whose
  EOT releases both chains gives two on the same frame); a sample that runs
  out of data does not hit it.
