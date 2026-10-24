# poryaaaa accuracy plan: null against mgba 0.10.5

Agreed 2026-10-24. This document is the source of truth for the accuracy
effort. Decisions here were made deliberately; do not re-open them without
asking.

## Goal

poryaaaa's rendered output nulls against mgba 0.10.5 at 48 kHz for music,
cries and sound effects from vanilla Pokemon Emerald.

- Criterion: perceptual null test, not bit-exactness. Metric is RMS of the
  time-aligned difference relative to the reference, in dB, over the whole
  file and over the worst one-second window.
- Gate starts at -40 dB and tightens toward -60 dB as milestones land.
- mgba 0.10.5 is the ground truth. Real hardware is not a target.
- Comparison happens at 48 kHz, the host rate users hear.

## Reference facts about mgba's audio path

- The sound unit is sampled every 512 CPU cycles (32768 Hz) by default
  (`src/gba/audio.c`, `sampleInterval = GBA_ARM7TDMI_FREQUENCY / 0x8000`),
  governed by the SOUNDBIAS resolution bits.
- DMA FIFO samples are held between timer ticks; PSG runs at the same clock.
- SOUNDBIAS is added and the result clamped to 9 bits.
- Resampling to the host rate uses blip_buf, but `audio.c` redefines
  `blip_add_delta` to `blip_add_delta_fast`: the linear two-tap variant, not
  the sinc kernel.
- PSG behaviour (duty stepping, wave RAM, LFSR noise with coalescing, 64 Hz
  envelope, sweep) is defined by mgba's `src/gb/audio.c`.

## Current state of poryaaaa (why it cannot null today)

- Hand-written integer reimplementation of the m4a mixer, not a port.
- PCM mixed at 13379 Hz, then linearly interpolated to host rate.
- PSG synthesized separately at host rate and summed after the upsample.
- No DMA FIFO, no timer, no SOUNDBIAS / 9-bit clamp, no 8-bit per-frame wrap.
- Notes start at sample-exact MIDI positions, not at frame ticks.
- MODT, TUNE and LFODL unimplemented. No bytecode sequencer.
- No reference comparison tests exist.

## Harness (milestone 1, built before any engine change)

Lives under `test/accuracy/`.

- **Test ROM** in `test/accuracy/rom/`: a standalone ROM built with agbcc
  from the pokeemerald tree's `m4a.c`, `m4a_1.s`, tables and sound data,
  using the game's sound-mode init. It carries the vanilla song table plus
  hand-written micro-test songs (one note per voice type, with and without
  reverb, sweep, compressed sample, fixed-frequency drum, LFO/tuning
  commands). A parameter table at a known offset (type: song/cry/SE, id,
  cry pitch and pan) is patched into the ROM file by the harness before
  loading. One build serves every test.
- **Reference renderer**: C program linked against mgba's
  `libmgba-static.a`, rendering headless at 48 kHz from frame zero.
- **Analysis**: Python with numpy/scipy. Cross-correlation alignment,
  metrics above, pass/fail gates, per-test report.
- **References**: regenerated locally and cached, keyed on ROM hash and
  mgba version. Only micro-test references are committed.
- **Build integration**: optional CMake target, skipped unless the
  pokeemerald and mgba paths are provided.
- First deliverable: a baseline measurement of today's poryaaaa.

## Engine milestones (each gated by the harness, one PR each)

Work happens on a feature branch; main stays releasable.

1. **Sequencer**: port of MPlayMain's command interpreter so the CLI
   renderer and harness play the same compiled song data mgba plays.
   CLI and harness only; no GUI exposure yet.
2. **Output stage**: cycle-based scheduler (280896 cycles per frame, VBlank
   tick and timer-driven DMA reads at real cycle offsets, 512 cycles per
   output sample); DMA FIFO sample-and-hold; PSG generated at 32768 Hz
   modeled on mgba's `gb/audio.c`; SOUNDBIAS add and 9-bit clamp; mgba's
   absolute output scale so no gain normalization is needed; MIT
   reimplementation of blip_buf's fast linear-delta path, unit-tested
   against blip_buf. Remove the optional analog low-pass.
3. **Mixer**: verbatim C translation of SoundMainRAM (224-sample frame
   buffer, 8-bit wrapping sum), the game's reverb, and CgbSound. The
   voicegroup loader emits data in the exact GBA struct layouts (ToneData,
   WaveData) with host pointers, so the ported code is edited as little as
   possible. The PCM rate option becomes the game's sound-mode frequency
   enum (the twelve gPcmSamplesPerVBlankTable rates, default 13379 Hz);
   the "mix at host rate" choice is removed.
4. **Cries and SE**: add to the test ROM and corpus; fix engine paths as
   the harness shows.
5. **MIDI quantization**: latch MIDI events (note-on, note-off, CC, pitch
   bend) to the next frame tick, because MPlayMain runs once per VBlank and
   the game can only start a note on a frame edge. This adds up to 16.7 ms
   of latency (8.4 ms average) and, more noticeably, 0 to 16.7 ms of jitter,
   which makes a MIDI keyboard feel rubbery. So:
   - Default to **unquantized when the host transport is stopped** (live
     playing) and **quantized when the transport is playing** (honest
     preview of recorded parts). CLAP exposes transport state, so this
     needs no user-facing toggle; still provide a manual override.
   - The engine's own tick (envelopes, LFOs, tempo) advances per frame in
     both modes. The modes differ only in when a note may start.

## Corpus

Vanilla Emerald, five to eight songs chosen to isolate features (pure PSG,
DirectSound-heavy with reverb, compressed samples, noise channel, etc.),
plus one or two of the user's own tracks as a practical check. Cries and SE
added in milestone 4.

## Explicitly deferred

- ipatix's HQ mixer (later effort).
- GUI song loader, cry and SE browsing in the plugin.
- Hardware comparisons.

## Status and handoff (updated 2026-10-24)

### Done: harness (branch `accuracy-harness`, commit "Add accuracy harness")

`test/accuracy/README.md` documents layout, requirements and commands. The
short version: `cmake -B build -DACCURACY_POKEEMERALD_DIR=~/pokeemerald
-DACCURACY_MGBA_DIR=~/mgba && cmake --build build --target accuracy`, or
`make -C test/accuracy/rom` then `test/accuracy/tools/accuracy.py`. A full
run (31 micro-tests + mus_littleroot) takes a few seconds. The `/accuracy`
skill in `.claude/skills/accuracy-harness` walks an agent through it.

Facts an agent needs that are not obvious from the code:

- The ROM mirrors Emerald's interrupt layout: `m4aSoundVSync` from the
  VCOUNT-150 interrupt, `m4aSoundMain` from VBLANK. Keep it that way; the
  output-stage milestone models exactly this timing.
- The reference always lags poryaaaa by 50 to 120 ms (ROM starts the song on
  frame 1; the first DMA buffer plays a few frames later; PCM DMA period is
  7 frames). Alignment therefore searches [-20, 200] ms. Once the output
  stage is cycle-based, poryaaaa should reproduce this offset itself and
  the measured lag should become a constant.
- References are cached by ROM hash + mgba version. Any change to
  `rom/micro/*.mid`, `accuracy_micro.inc` or the ROM sources changes the hash
  and re-renders. Run `--update-refs` only then, never to absorb an engine
  regression. Committed micro refs are FLAC (needs python `soundfile`).
- `test/accuracy/baselines/` holds one report per milestone. Save a new one
  after each engine milestone (`cp build/accuracy/report.json ...`) and
  compare with `tools/compare_reports.py`.

### Baseline of main (f57f5a4): 0/32 pass at -40 dB

Every test's null depth is within a few dB of 0; poryaaaa does not null
anywhere yet. Three findings from the baseline that the engine milestones
must address:

1. **Tempo drift.** mus_littleroot drifts ~3.9 ms per second against mgba
   (the harness reports it in the `drift` column). Until this is zero,
   whole-file metrics on songs are meaningless. m4a's tempo is a per-frame
   accumulator (`tempoC += tempoI` ... `>= 150` in MPlayMain) at 59.7275
   frames per second; port that arithmetic verbatim rather than deriving
   ticks from BPM.
2. **Level.** poryaaaa is about 3.5 dB quieter than mgba on nearly every
   voice type (median -3.47 dB). Not to be fixed by a gain knob: the
   output stage adopts mgba's absolute scale and the mixer port carries
   the game's volume arithmetic, after which this gap should vanish.
3. **Sweep.** The `sq1_sweep` micro-test (square 1, sweep byte 0x3A) renders
   total silence in poryaaaa; mgba plays it normally. Likely the current
   sweep unit stops the channel on a downward sweep. It falls to the
   mixer/CgbSound port, but if it is a one-line bug it can go earlier.

### Done: engine milestone 1, the sequencer (branch `accuracy-harness`)

What landed (commits after 9ccb294):

- `plugin/m4a_song.{h,c}`: parser for mid2agb `.s` output (the GNU as
  subset it uses plus the MPlayDef.s vocabulary built in; no binutils at run
  time). Output is the song's `.rodata` image with offsets in place of
  addresses and a relocation list. `test/test_song_parser.c`
  (`poryaaaa_song_tests`, run by the `accuracy` target) relocates every song
  to its address from `accuracy_test.map` and compares with the ROM: all 561
  songs in the ROM (31 micro-tests, every vanilla song, SE and phoneme)
  match byte for byte.
- `plugin/m4a_sequencer.{h,c}`: port of MPlayMain and the ply_* handlers,
  structured like m4a_1.s. Runs from a new engine frame hook at the top of
  `m4a_engine_tick` (where MPlayMain sits in SoundMain), with the engine's
  own gate-time / tempo / LFO work disabled while attached. Tempo is the
  per-frame accumulator verbatim. Track parameters are written to the engine
  track and applied at frame end through `m4a_engine_track_apply` (the
  VOLCHG/PITCHG pass), including the quirk that ply_note clears the pending
  flags. Notes go through `m4a_engine_note_on`, which now returns the
  channel it started so the sequencer can set the gate time in ticks and
  pick the newest channel for EOT. Engine fidelity fixes that came with it:
  ply_note's LFO-delay restart (`lfoDelayC = lfoDelay`, clear_modM) at note
  start, and the MODT (CC 0x16), TUNE (CC 0x18) and LFODL (CC 0x1A)
  commands, which were unimplemented on the MIDI path too.
- `poryaaaa_render --song <file.s>` (voicegroup from the header, master
  volume and reverb baked in; `--song-volume`/`--reverb` are ignored with a
  note). Without `--total-duration-seconds` the song's FINE ends the render
  and `--loop-count` counts passes through its GOTO.
- `accuracy.py --input song|midi`, default `song`. `metrics.find_lag` now
  picks, among the envelope-correlation estimate and the top waveform
  cross-correlation peaks, the lag with the deepest raw null: the old
  two-stage aligner missed the true lag by 4 to 30 ms on a third of the PCM
  tests once the renders got close, and reported it as lag scatter.
- Corpus: `songs.json` has 7 songs (littleroot, weather_groudon = pure PSG
  with pseudo-echo, vs_wild = DirectSound-heavy battle, mt_chimney = noise,
  rg_vs_trainer = FRLG drumset, vs_trainer = the one vanilla MEMACC,
  credits = 60 s). No vanilla music uses compressed samples (cries only), so
  that category is the `pcm_cmp` micro-test until milestone 4.

Reports: `baselines/2026-10-24_sequencer.json` (song input) and
`baselines/2026-10-24_sequencer_midi-path.json` (the same build on `--input
midi`, scored with the new aligner, for an apples-to-apples "before").

| test | before null | after null | before drift | after drift | lag before -> after |
|------|------------:|-----------:|-------------:|------------:|--------------------|
| mus_littleroot | +1.4 | -4.8 | 3.9 | 0.0 | 118.9 -> 53.6 |
| mus_vs_wild (new) | +1.6* | -5.3 | 2.6* | 0.0 | 53.6 |
| mus_mt_chimney (new) | +1.7* | -2.1 | 4.2* | 0.0 | 53.6 |
| mus_rg_vs_trainer (new) | +1.5* | -1.9 | 4.2* | 0.0 | 53.6 |
| mus_vs_trainer (new) | +1.6* | -4.8 | 2.6* | 0.0 | 53.6 |
| mus_credits (new, 60 s) | +1.5* | -4.0 | 5.5* | 0.0 | 53.6 |
| mus_weather_groudon (new) | +1.2* | +1.2 | n/a | n/a | uncorrelated |
| pcm micro-tests (16) | -3.6 .. +0.6 | -7.4 .. -9.4 | | | all 53.6 (pcm_hi 28.8) |
| wave / wave_alt | -2.6 | -6.9 / -6.2 | | | 36.7 / 36.6 |
| square / noise micro-tests | -4.3 .. +1.2 | -2.4 .. +0.3 | | | period-ambiguous |

(*) measured with `--input midi` on the sequencer build. Gate: still 0/38
at -40 dB, as expected. `compare_reports.py` against the f57f5a4 baseline
flags three square tests (sq1_env, sq1_alt, sq2_alt) as 1 to 2 dB worse;
see finding 3.

Findings for the next milestones:

1. **Exit criterion met for PCM and songs.** Drift is 0.0 on every song
   that correlates, and the lag is 53.6 ms (2575 samples at 48 kHz) on
   every PCM micro-test and every song. This is the constant the output
   stage must reproduce: ROM start on frame 1 plus DMA latency. Wave and
   noise tests agree on ~36.5 ms, 17 ms (one frame) less than PCM, which
   is the PSG-vs-DMA path difference the cycle-based scheduler should
   produce by itself.
2. **Two renders only null down to the PCM step phase.** A note that starts
   at a different phase of the 13379 Hz mix grid gives a different
   sub-sample waveform; the same engine playing the same note from the MIDI
   and song paths is bit-identical at host mix rate but only -3 dB at 13379
   Hz. The null depth against mgba is bounded the same way until the output
   stage models the DMA sample-and-hold at the real cycle offsets.
3. **Square-wave tests are in the uncorrelated regime** (null -2 .. +3 dB)
   and move by +-2 dB with the oscillator phase and the free-running 64 Hz
   hardware-envelope phase at note start, both of which the engine does not
   reset on retrigger and which the frame-quantized note start shifted. The
   three flagged "regressions" are this noise; nothing to chase before the
   PSG port. Pure-PSG mus_weather_groudon likewise does not correlate at
   all, so its drift column is noise.
4. **pcm_hi** prefers a lag of 28.8 ms: its high-key aliasing differs enough
   that the best null is off the true onset. Mixer milestone.
5. The MIDI path numbers on the same build show MODT/TUNE/LFODL and the
   LFO-delay restart working there too (lfo_trem -7.5, lfo_pan -7.4, tune
   -2.3, lfo_delay -2.9 dB, from +0.7, +0.8, +0.5, -0.7).

### Next: engine milestone 2, the output stage

The plan section above stands.  The sequencer's frame hook is where the
cycle-based scheduler should call MPlayMain; the per-frame tick already
runs the sequencer before the envelopes, as SoundMain does.

### Reference: milestone 1 decisions as agreed

Goal restated: `poryaaaa_render` and the harness play the **compiled** song
data (mid2agb output) through a port of MPlayMain, so the input to both
poryaaaa and mgba is byte-identical. Decisions for this milestone:

- **Input format: parse mid2agb's `.s` output directly**, in C, in the
  renderer/engine. The vocabulary is fixed and small (`sound/MPlayDef.s`:
  W00..W96, N01..N96, TIE, EOT, VOICE, VOL, PAN, BEND, BENDR, LFOS, LFODL,
  MOD, MODT, TUNE, XCMD, TEMPO, KEYSH, PRIO, GOTO, PATT, PEND, REPT, FINE,
  MEMACC, plus `.byte`/`.word` with labels). Do not require binutils at run
  time. Verify the parser byte-for-byte against the ROM build: the ELF map
  (`build/accuracy/rom/accuracy_test.map`) gives every song's address, so a
  unit test can compare parsed bytes to the bytes in `accuracy_test.gba`.
- **Voicegroup** comes from the song header's `_grp` symbol, resolved
  through the existing loader (strip the `voicegroup_` prefix).
- **Renderer CLI**: add `--song <file.s>` as an alternative to `--midi`
  (`-V` master volume and reverb are already baked into the `.s`, so
  `--song-volume`/`--reverb` must not be applied on this path). Keep the
  MIDI path; milestone 5 compares the two.
- **Harness hook**: `accuracy.py` gets a `--input song|midi` switch (default
  `song` once it exists). For micro-tests the `.s` files are
  `build/accuracy/rom/micro/<name>.s`; for vanilla songs
  `build/accuracy/rom/songs/<name>.s`. Both are produced by the ROM build.
- **Timing**: the sequencer runs on the frame tick (one MPlayMain call per
  VBlank, 59.7275 Hz). Until the output stage lands, the per-frame tick
  can keep driving the existing sample-based synthesis; the point of this
  milestone is identical event streams and identical tempo.
- **Exit criterion** (the -40 dB gate will not be met yet): `drift` is 0.0
  for the song corpus, the lag is the same constant for every micro-test of
  a given voice class, and the per-test numbers do not regress against
  `baselines/2026-10-24_main-f57f5a4.json`. Save the new report as
  `baselines/<date>_sequencer.json`.

### Corpus growth

`test/accuracy/songs.json` has one song. Add the rest of the five-to-eight
song corpus during the sequencer milestone (pure PSG, DirectSound with
reverb, compressed samples, noise, a long one); each entry is a name from
`midi.cfg` plus a duration. Cries and SE wait for milestone 4; the ROM and
renderer already accept `--type cry --id <cry table index>` and SE ids.
