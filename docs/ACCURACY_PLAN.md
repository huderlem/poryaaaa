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

Code review (three independent reads: sequencer vs m4a_1.s/m4a.c, the
engine diff on the MIDI path, parser robustness) found and fixed, commits
a80b920..: the LFODL note-start leaking pending changes to older notes,
fixed-rate voices following bends, the key-127 clamp (now saturating at
178/166 like the GBA), the sequencer playing 16 tracks where the BGM player
has 10, a legato-inherit MO_VOL re-arm, and 13 parser hardening items. The
MIDI path was A/B-verified byte-identical to the parent commit. Known,
documented gaps: hand-written-only XCMDs (xwave, xcmd_0D sample start
offset) are consumed but not applied; memacc indices are masked to the
16-byte area where the GBA reads out of bounds.

### Done: engine milestone 2, the output stage (branch `accuracy-harness`)

Ordered so each step is independently verifiable and committed; the harness
gates each.

1. **Done (03f0b41)** `plugin/m4a_blip.{h,c}`: MIT reimplementation of
   blip_buf's fast linear-delta path.  `test/test_blip.c`
   (`poryaaaa_blip_tests`, built with `ACCURACY_MGBA_DIR`) checks it against
   mgba's blip_buf: 49112 checks, 0 failures, bit-exact.
2. **Done (ff805df)** `plugin/m4a_gba.{h,c}` + scheduler in `m4a_engine.c`:
   cycle clock, timer-0 FIFO sample-and-hold, DMA ring, SOUNDBIAS stage,
   boot replay, `m4a_engine_boot`.  Harness after this step: every PCM
   micro-test at lag **0.0 ms**, level gap **0.0 dB**, null -36..-42 dB
   (pcm_bass passes -40; pcm_hi -22.7 and pcm_reverb -10.4 are mixer-port
   items); all songs lag 0.0, drift 0.0.  PSG tests unchanged (old
   oscillators still attached, stepped on the DAC grid): noise7, psg_mix,
   mus_weather_groudon read 1.2-1.4 dB worse than the sequencer baseline,
   which step 3 is for.
3. **3a done**: `mgba_render` captures mgba's DAC stream through
   `mAVStream.postAudioFrame` and resamples it with `plugin/m4a_blip.c`
   (see the blip note below); `--native-resampler` keeps mgba's own output.
   Its `--version` reports `mGBA 0.10.5+m4a_blip`, so the harness caches
   these references apart from the native ones and the committed micro
   FLACs were regenerated once (**reference-definition change**, ROM hash
   unchanged, 7b2e822f7a23).  Harness after 3a: **14/38 pass**.  Every PCM
   micro-test deepened: pcm -37.8 -> -46.3, pcm_hi -22.7 -> -47.2 (its
   "high-key aliasing" was the sinc-vs-linear kernel, not the mixer),
   pcm_adsr -36.1 -> -64.0, pcm_drum -27.4 -> -69.1, pcm_cmp -28.7 ->
   -54.6, pcm_bass -41.8 -> -66.5, pan/vol/vel/bend/lfo_*/tune -36..-39 ->
   -42..-58.  Still failing on the PCM side: pcm_reverb -10.4 and
   pcm_chord -28.6 (unchanged; mixer-port items), vel's worst second -38.6.
   PSG tests and songs unchanged (the PSG model has not landed yet).
   **3b done (ee2ceec)**: `plugin/m4a_psg.{h,c}`, the PSG ported from mgba
   `src/gb/audio.c` in GBA style with GBAIOWrite8/GBAIOWrite register
   semantics and stored-bit images; `plugin/m4a_channel.c` is now the
   CgbSound register driver (transliterated, labels kept) on top of it, two
   PSG instances (real + poly-debug shadow), writes stamped at VBlank line
   start + 1740 after the DAC catch-up, the frame sequencer hooked into
   `m4a_gba_run` ahead of a same-cycle sample event, MPlayExtender's boot
   writes replayed.  CGB notes start at the next SoundMain (CHN_START).
   The old oscillator fields and tests are gone; `test_engine.c` has PSG
   unit tests against hand-computed mgba behavior and driver
   register-sequence tests.  The `sq1_sweep` silence was not the sweep
   unit: `voicegroup_loader.c` parsed voice arguments with `%d`, so the
   micro voice's `0x3A` sweep byte failed to parse and the voice fell to a
   sample-less PCM voice (now `%i`).
4. **Done (6c1ff6d)**: analog low-pass removed (flag, `--analog-filter`,
   GUI checkbox, `analog_filter` config key now ignored, CLAP state byte
   still written as 0 and read back so old saves load).

### Milestone 2 results (`baselines/2026-10-24_output-stage.json`)

**15/38 pass at -40 dB** (sequencer baseline: 0/38).  Before =
`baselines/2026-10-24_sequencer.json`, after = output stage; raw null in
dB, lag 0.0 ms and drift 0.0 everywhere.

| test | before | after | | test | before | after |
|------|-------:|------:|-|------|-------:|------:|
| sq1 | -2.4 | -36.7 | | pcm_chord | -9.0 | -28.6 |
| sq1_env | -2.2 | -3.5 | | pcm_bass | -9.0 | **-inf** |
| sq1_sweep | 0.0 (silent) | **-43.1** | | pan | -9.3 | **-48.6** |
| sq1_alt | -1.6 | -35.1 | | vol | -9.4 | **-56.7** |
| sq2 | -1.3 | **-inf** (bit-exact) | | vel | -9.4 | -56.7 (worst 1 s -38.7) |
| sq2_alt | -0.7 | -34.1 | | bend | -9.4 | **-43.1** |
| wave | -6.9 | -11.9 | | lfo_vib | -9.4 | **-42.6** |
| wave_alt | -6.2 | -10.6 | | lfo_trem | -9.4 | **-42.0** |
| noise | -1.0 | -5.3 | | lfo_pan | -9.4 | **-42.1** |
| noise7 | -2.4 | -6.2 | | lfo_delay | -9.4 | **-42.3** |
| pcm | -9.2 | **-46.3** | | lfo_psg | -1.1 | -20.7 |
| pcm_hi | -6.3 | **-47.2** | | tune | -9.3 | **-58.6** |
| pcm_adsr | -7.4 | **-inf** | | tune_psg | -3.2 | -32.8 |
| pcm_drum | -8.2 | **-inf** | | psg_mix | -0.1 | -2.4 |
| pcm_cmp | -8.2 | **-55.0** | | pcm_psg | -2.5 | -17.0 |
| pcm_reverb | -7.2 | -10.4 | | | | |

| song | before | after | | song | before | after |
|------|-------:|------:|-|------|-------:|------:|
| mus_littleroot | -4.8 | -7.4 | | mus_mt_chimney | -2.1 | -3.0 |
| mus_weather_groudon | +1.2 | +1.3 | | mus_rg_vs_trainer | -1.9 | -3.3 |
| mus_vs_wild | -5.3 | -6.9 | | mus_vs_trainer | -4.8 | -7.8 |
| mus_credits | -4.0 | -6.5 | | | | |

Findings:

1. **PCM path is done to the resampler's precision.**  Level gap 0.0 dB,
   lag 0.0 ms, 14 of the 17 PCM micro-tests pass; vel misses on its worst
   second (-38.6) only.  `pcm_reverb` (-10.4) and `pcm_chord` (-28.6) are
   the mixer port: the game's reverb and the 8-bit wrapping sum.
2. **PSG: the model is right, the write timing is the residual.**  sq2
   nulls to -73 dB (LSB-level), so the square model, its scale and the
   DAC/resampler chain are exact.  What is left is *when* CgbSound's
   register writes land.  Measured from the renders against the +1740
   constant: on a frame that starts notes mgba's writes are later by
   ~2700 cycles for one note (wave/noise micro-tests: our onset 8-11
   samples early), ~7000 for mus_weather_groudon's first frame (20
   samples), ~11000 for psg_mix's four simultaneous notes (32 samples);
   on steady frames the jitter is a few hundred cycles (sq1's third note
   trigger: ~1400).  MPlayMain's per-frame work (ply_note setup) runs
   before CgbSound in SoundMain, so its cost sets the write time.
   Consequences: a square's duty phase is free-running and unaffected, but
   its volume-step instants are off by that jitter (the -35 dB floor on
   50 %-duty sq1 / the `_alt` tests; 12.5 %-duty sq2 happens to step on
   duty edges), while the wave RAM rotation and the LFSR start at the
   trigger, so wave and noise are phase-shifted for the rest of the song
   (wave -12, noise -5, psg_mix -2.4, mus_weather_groudon uncorrelated).
   Next lever: model SoundMain's variable entry (base + per-note-on cost,
   per-channel offsets inside CgbSound), which needs `mgba_probe`
   measurements of CgbSound entry vs. notes started per frame.
3. **gCgb3Vol over-range** (wave center-pan envelopeGoal 16..31): the GBA
   reads into `gClockTable`, whose bytes are < 0x20, so NR32's volume
   field is 0 = silence; modeled that way (was `& 0x0F`).
4. Songs with PSG content gained 1-3 dB; their PCM components null at
   -40..-70, so song nulls are now bounded by PSG write timing and the
   mixer items above.  mus_weather_groudon's drift column is 0 now (it was
   noise on an uncorrelated signal).

Code review (two independent reads: scheduler/FIFO/blip/mgba_render
capture against mgba `src/gba/{audio,timer,dma}.c`, `core/timing.c`,
blip_buf; PSG + driver against `src/gb/audio.c`, `src/gba/io.c` and
`m4a.c` CgbSound/CgbOscOff/CgbModVol/MPlayExtender).  Everything else in
both reads was verified line for line.  Fixed (the review-fixes commit):

- `poryaaaa_render` wrote `(int)(x * 32767)`, truncating every sample by
  up to 1 LSB; now `lrintf(x * 32768)`, exact for the engine's int16/32768
  output.  sq2, pcm_adsr, pcm_drum and pcm_bass now null to **-inf**
  (bit-identical to mgba through the shared resampler).
- Timer 0 period is the game's integer reload (`TM0CNT_L = -(280896 /
  spv)`, 1254 cycles at 224) instead of an exact rational; identical at
  13379 Hz, matters for the other sound-mode rates.
- CPU register writes at a cycle now land before the hardware events due at
  that cycle (mgba completes the store inside the instruction).  No tie
  occurs in the corpus (the SoundMain and VSync stamps are 1 and 63 mod 64
  while events are multiples of 64), so the numbers did not move.
- SOUNDBIAS boot write at 5760 as measured (was 5750; same output).
- CgbModVol is called only where CgbSound calls it (note start and
  envelopeCounter == 0); MPlayMain's VOLCHG pass is ChnVolSetAsm + MO_VOL.
  Neutral on the corpus.

Review claims **rejected by measurement** (both make lfo_psg worse against
mgba, so the reference decides):

- "c15 should start at 0 (SoundInit zero-fills SoundInfo), so the first
  CgbSound reloads 14."  With 0 lfo_psg goes -20.7 -> -10.8 dB; 14 (the
  engine's first tick, frame 2's SoundMain, then sees 13) matches mgba.
  Reading: the ROM's frame-1 VBlank IRQ is pending when IME is set at
  cycle 323299 (just after that VBlank), so one SoundMain/CgbSound runs
  before frame 2's; the engine's boot does not replay it (PCM is unaffected
  since that frame's ring slot is rewritten before the DMA reaches it).  A
  probe of `SoundInfo.c15` after the first CgbSound would confirm; the c15
  phase is right either way.
- "`envelopeGoal = (u32)(left + right)` on a u8 field wraps the sum at 256
  before `/= 16`."  The wrapped form nulls 12 dB worse (-8.4); agbcc keeps
  the sum in a register and stores `(left + right) / 16`, so the goal can
  reach 31 and the `gCgb3Vol` over-read (finding 3) is real.

Deferred, documented: (a) mgba processes timer/frame events at the
*late* time of the instruction that crossed them (`cyclesLate` corrects
the rescheduling, not GBAAudioSampleFIFO's `until` or `_updateFrame`'s
sample time), so a FIFO sample can land one 65536 Hz slot later in mgba
while the CPU is busy; a plausible -40..-50 dB floor on PCM that needs a
probe to quantify before chasing mixer items below it.  (b) The FIFO DMA
moves 4 words instantly instead of over ~40 cycles (only observable with a
VSync restart inside that window; never at 224 samples per VBlank).
(c) `m4a_engine_set_pcm_mix_rate` after boot does not replay
SampleFreqSet (plugin path only).  (d) Sound-mode bit 0 (Emerald's
default "Mono" option forces center pan on PSG) is not modeled; the test
ROM never sets it.

Facts measured in mgba 0.10.5 with `test/accuracy/ref/mgba_probe.c` (a
libmgba debugger probe; the plan's earlier assumptions were wrong on the
first two):

- **The DAC runs at 65536 Hz, not 32768.** Emerald's `SOUND_MODE_DA_BIT_8`
  writes SOUNDBIAS_H = 0x40 (resolution 1): mgba samples every 256 cycles,
  4 samples per 1024-cycle sample event, blip frames of 2048 cycles.
- **Desktop mgba uses blip_buf's sinc kernel**, not the fast path: the
  `#define blip_add_delta blip_add_delta_fast` in audio.c is `#ifdef __3DS__`
  only.  The linear path was implemented as decided; the kernel difference
  would bound PSG/PCM nulls somewhere around -20..-30 dB in the top octave.
  **Decision (2026-10-24): the reference adopts the engine's resampler.**
  `mgba_render` captures mgba's raw DAC stream through
  `mAVStream.postAudioFrame` (one int16 pair per 65536 Hz sample) and
  resamples it with `plugin/m4a_blip.c` using the same clock (16777216),
  frame (2048 cycles) and rate the engine uses, so both sides go through
  one bit-exact resampler and the harness measures only the GBA-side
  model.  A `--native-resampler` flag keeps mgba's own sinc output for
  listening checks.  This changes what a reference is, so the committed
  micro refs and the cached corpus refs are regenerated once with
  `--update-refs` (the ROM is unchanged; record it as a reference-definition
  change in the status section).  The bl_step table itself stays out of the
  tree (LGPL data).
- mgba's GBA-style noise is **point-sampled** (`ch4.sample << 3`), not
  coalesced; squares/wave/noise are unipolar 0..15 x 8 x (1 + NR50 volume),
  >> 2 (SOUNDCNT_H volume 2), PCM is int8 << 2, then bias 0x200, clamp
  0..0x3FF, x48 -> int16.
- Reset: video starts at line 126; frame k >= 1 starts at
  124773 + (k-1) x 280896 cycles.  m4aSoundInit: SOUNDCNT_X 0x8F @5720,
  SOUNDCNT_H 0xA90E @5737, SOUNDBIAS_H 0x40 @5760, DMA on @15873, timer on
  @39794 (line 159 of frame 0), MPlayExtender PSG writes @39875-39935
  (NR52 0x8F, NR50/51 0, NR12/22/42 = 8, NR14/24/44 = 0x80, NR30 = 0,
  NR50 = 0x77), DMA off @41238, timer off @47452, DMA on @47893, timer on
  @320682 (line 159 of frame 1).  IME is set at 323299, after frame 1's
  VBlank, so the first interrupts the ROM catches are frame 2's: VSync
  handler entry line 150 + 164 (DMA restart ~+250), SoundMain entry line
  160 + 197.  The test starts after that SoundMain; the song's first
  MPlayMain is frame 3's VBlank.  The FIFO carries 2 stale words + 3
  internal bytes across the second init pass (SOUNDCNT_H is written once),
  so ring byte k plays at timer tick 12 + k after the final timer enable.
- CgbSound entry: VBlank line start + 1390 in a steady frame, + ~4070 on
  the song-start frame (MPlayMain's note setup); register writes follow
  within ~200-900 cycles, channels 1..4 in order.  The engine uses one
  constant (+1740) for all writes; the start-frame extra is not modeled.
- Register write sequences (byte writes; mgba handles NR11-14/21-24/31-34/
  41-44 bytes individually, NR10/NR30/NR50/NR51/NR52 through the merged
  16-bit path which ignores writes while NR52 is off): square start NR10,
  NRx1 = duty<<6|length, then MO_PIT NRx3/NRx4 (no trigger: n4 = 0), then
  MO_VOL NR51, NRx2, NRx4|0x80 (plus a second identical NR14 write when
  NR10 bit 3 is clear).  Wave: NR30 = 0x40, WAVE_RAM x4, NR30 = 0, NR31,
  NR33, NR34 = 0x80|hi (n4 = 0x80 triggers at MO_PIT), NR51, NR32, NR30 =
  0x80, NR34 = n4, then n4 &= 0x7F so later frames never retrigger.  Noise:
  NR41, NR43 = period<<3, NR43 = (old & 8)|freq, NR44 = 0, NR51, NR42,
  NR44 = 0x80.  CgbOscOff: NRx2 = 8, NRx4 = 0x80 (wave: NR30 = 0).
- m4aSoundVSync's DMA restart is a disable/enable pair: the START_NOW
  write happens while the DMA is already enabled, so mgba does not
  transfer; only the source address reloads.  FIFO contents are kept.

### SoundMain timing probe (2026-10-24, step 1 of the timing model)

`test/accuracy/ref/mgba_probe.c` was extended (per-frame summary in
`<out>.frames`, scheduler-lateness hooks in `<out>.events`) and run for 70
frames on sq1, wave, noise, psg_mix, lfo_psg, mus_weather_groudon (id 444)
and mus_littleroot (id 405); `test/accuracy/tools/probe_costs.py` tabulates
the raw logs.  All cycles below are relative to the start of VBlank line
160 of the engine's frame k (frame k >= 1 starts at 124773 + (k-1) x
280896).  Measured facts:

- **Fixed entries**: m4aSoundMain +185, SoundMain +197, first MPlayMain
  +291 (six players are chained; the five idle ones cost 55 cycles each),
  m4aSoundVSync at line 150 + 164 (its DMA restart write +250, as modeled).
- **CgbSound entry depends on MPlayMain's work**, not on a constant:
  - BGM player paused (before MPlayStart): **1007**.
  - Playing, no tempo tick this frame: **1396 + 6 x E** (E = tracks with
    MPT_FLG_EXIST): 1402 / 1414 / 1420 / 1450 for E = 1 / 3 / 4 / 9, exact.
  - A tempo tick (tempoC + tempoI >= 150) with no commands adds **464** for
    E = 1 and, per song, 588-656 (E = 3), 728-760 (E = 4), 1006-1171 (E =
    9): about 404 + 60 x E, the spread being per-track state (channels in
    the chain, LFO).  An active LFO (lfo_psg, mod != 0) with its PITCHG
    pass costs 496 on top (2362 vs 1866).
  - Commands, measured as the delta from one ply_* entry to the next
    (handler plus dispatch): keysh 182, tempo 184, voice 277, vol 182,
    pan 184, bend 168, bendr 182, lfos 162, mod 146-202, modt 184, xcmd
    61 + xiecv 142; the track-start path (Clear64byte, SoundMainBTM) about
    260 (244-327 solved from the start frames of psg_mix, groudon,
    littleroot).
  - **A CGB note start costs 924** (square and wave; noise 832): sq1/wave
    frame 48 = 2790 vs the 1866 steady tick frame; groudon's two-note
    frames 27/50 = +1848/+1882.  A PCM note start costs 1005-1423
    (channel allocation varies; littleroot frames 20/37/62).
  - Start frames: sq1 4069 (tempo, keysh, voice, vol, pan, note), noise
    3977, lfo_psg 5075, psg_mix 10304 (4 tracks), groudon 10703 (3 tracks,
    35 commands), littleroot 22695 (9 tracks, 84 commands).  These are the
    "~2700 per note" of finding 2: the actual composition is the command
    list above.
- **Register writes inside CgbSound are deterministic** (offsets from the
  CgbSound entry for channel 1 as the first active channel; the channel
  prologue is ~120 cycles and an inactive channel ~24, an active channel
  without writes 203, a channel ends ~99 after its last write):
  - square start: NR10 +351, NR11 +361, NR13 +563, NR14 +595, NR51 +628,
    NR12 +656, NR14 +666; the next channel's first write 330 later.
  - wave start (ch 3 first active, so +48 for ch 1-2): NR30=40 +431..463,
    WAVE RAM +26/+41/+56/+71 after it, NR30=00 +77, NR31 +81, NR33 +239,
    NR34 +271, NR51 +304, NR32 +336, NR30=80 +352, NR34 +358.
  - noise start (ch 4 first active): NR41 +489, NR43 +8, NR43 +170, NR44
    +194, NR51 +227, NR42 +255, NR44 +265 after it.
  - steady MO_PIT: NRx3 +283, NRx4 +32 after; steady MO_VOL: NR51 +327
    (or 33 after the NRx4 of a MO_PIT), NRx2 +28, NRx4 +10 after; wave
    MO_VOL: NR51 then NR32 +32; CgbOscOff: NRx2=08 +323, NRx4=80 +14 (sq)
    / +6 (noise); wave off NR30=00.
  - psg_mix start frame, all four channels: NR10 +351 ... NR14 +666, NR21
    +996, ..., NR24 +1301, NR30 +1649, ..., NR34 +2007, NR41 +2349, ...,
    NR44 +2614; SoundMainRAM entry +2729.
- **The engine's single stamp (+1740) is therefore 340 early on a 1-track
  no-tick frame's first write, 120 late on a tick frame, 1000-2700 early
  on note-start frames and up to 21000 early on a song's first frame.**
- **1b: there is no frame-1 SoundMain.**  DISPSTAT (VBlank/VCOUNT IRQ
  enables, 0x9628) is written at cycle 323278, IE at 323293 and IME at
  323299, all *after* frame 1's VBlank (321893), so no IRQ was pending;
  the first VCountIntr is frame 2's (590621) and the first VBlankIntr
  frame 2's (602962).  The first CgbSound the ROM runs is frame 2's, with
  **c15 0 -> 14**; MPlayStart follows at 604892 (after that SoundMain) and
  the song's first MPlayMain is frame 3's, which runs CgbSound with **c15
  14 -> 13**.  The envelope double-step (prevC15 == 0) therefore falls on
  frames 17, 32, 47, ... (lfo_psg: frames 16/31/46 in the micro-test's
  own numbering shift by its note start).  The engine starts c15 at 14
  and decrements on its frame-2 tick, so its channels see 13 on frame 2
  and 12 on frame 3: **one c15 step ahead of mgba**.  The earlier rejection
  of "c15 should start at 0" was a measurement on the pre-output-stage
  build; step 2 re-tests it with the write timing in place.
- **1c: mgba scheduler lateness** (70 frames each): timer-0 overflows
  15238, max cyclesLate 6 (micro-tests) / 7 (littleroot) / 10 (groudon);
  the FIFO sample landed one 65536 Hz slot late (`until` computed at the
  processing time) **0 times in every micro-test, 5 times in littleroot,
  6 in groudon** (groudon: every fourth frame at line 161 + 65, during
  SoundMainRAM).  The sample event's time is corrected (`now -
  cyclesLate`); the PSG frame event (593) was never late across a slot
  boundary.  Bound: 5-6 one-slot sample-and-hold errors per 1.17 s, each
  256 cycles wide, puts this floor around -50 dB on songs and nowhere on
  the micro-tests; not worth modeling before the mixer.

### SoundMain timing probe, step 3: inside MPlayMain (2026-10-24)

`mgba_probe` now takes the ELF's `nm` listing as its symbol file
(`arm-none-eabi-nm test/accuracy/rom/build/accuracy_test.elf >
build/probe/syms.nm`; the map still works for the function entries) and
sets breakpoints on the `_081DD*` local labels of m4a_1.s inside
MPlayMain's tick loop, its VOLCHG/PITCHG pass, ply_note and TrackStop,
logging r0/r3/r4/r5/r7 and, where a branch depends on memory, the deciding
bytes (track flags, wait/LFO fields, the CGB channel's status, the
channel's track/prev/next pointers, the track's chain pointer).  The
breakpoints do not change the emulation: the `.frames` files of groudon
and littleroot are identical to the earlier runs' column for column.
`tools/probe_mplay.py <logs...> [frame...]` prints the breakpoint
sequence of the BGM player's MPlayMain for the frames named and, over all
frames and logs, the delta between consecutive breakpoints keyed on the
deciding state.  Runs: the seven corpus songs for 300 frames each (5 s)
and sq1, sq1_env, noise, pcm_drum, pcm_chord, vel, bend, lfo_vib,
lfo_trem, lfo_pan, lfo_delay, lfo_psg, psg_mix, pcm_psg for 70 (about
25,000 breakpoint deltas).  Every number below is a measured delta with a
single value unless marked *est.* (a path the corpus never took; the
estimate is the measured neighbouring path plus or minus the instruction
difference, with the rule, seen throughout the data, that a taken Thumb
branch in ROM costs about 4 more than a not-taken one).  Sporadic +18
(ply_vol, keysh, xiecv, MidiKeyToCgbFreq once each) are the FIFO DMA
stalling the CPU, not code paths; the model ignores them.

**Frame skeleton.**  MPlayMain calls the next player in the chain before
its own body, so the BGM player (last of six) does its work first: its
entry is at +566 (first MPlayMain +291, five idle entries 55 apart), then
FadeOutBody at +96, `_081DD9BC` (tempoC test) +54 later.  Paused player:
entry to `_081DDA6C` 71.  Exit: `_081DDA6C` five more times 63 apart (the
idle players unwinding), CgbSound 55 after the last.  So 1007 paused and,
playing, 566 + 150 + (body) + 370.

**No-tick path** (`_081DD9BC` -> `_081DD9C4` 12 -> `_081DD9C8` 4), then
the VOLCHG/PITCHG loop over *all* `MusicPlayerInfo.trackCount` tracks
(10 for the BGM player, not only the song's): a track without
MPT_FLG_EXIST 14 (`_081DD9C8` -> `_081DDA62`), with EXIST and no flags 20,
next track `_081DDA62` -> `_081DD9C8` 16, after the last `_081DDA62` ->
`_081DDA6C` 10.  (1396 + 6 E of step 1 is 566 + 150 + 16 + 10 x 30 - 6 +
370 and + 6 per existing track: confirmed as a sum.)

**Tick** (tempoC >= 150): `_081DD9BC` -> `_081DD874` 14 -> `_081DD87C` 8;
per track of trackCount: no EXIST 16 (`_081DD87C` -> `_081DD998`); EXIST
14 to `_081DD886`, then 12 to the chain walk (`_081DD892`) or 18 straight
to `_081DD8BA` with an empty chain.  Chain walk per channel: ON with gate
0: 20; ON, gate decremented to nonzero: 26; ON, gate reaching 0 (SF_STOP
set): 34; dead (SF_ON clear): 14 to `_081DD8AE`, 12 to ClearChain, 33 to
RealClearChain, then to `_081DD8B4` 54 (prev 0, next 0), 50 (prev 0, next
set), 56 (prev set, next 0), *est.* 52 (both set); from `_081DD8B4` 12 to
the next channel or 6 to `_081DD8BA` at the end.  `_081DD8BA` ->
`_081DD938` 14 without MPT_FLG_START; with it 20 to Clear64byte, 33 to
SoundMainBTM, 68 to `_081DD938` (121, replaces the 264 "track start" of
step 2, which also held the start frame's command dispatches).  At
`_081DD938` with wait != 0: 28 to `_081DD994` when lfoSpeed != 0 and mod
== 0 (every track after start; lfoSpeed == 0 *est.* 23), 40 when the LFO
is delayed (lfoDelayC != 0, decremented), LFO step: 34 to `_081DD95A`, 26
to `_081DD972` when lfoSpeedC - 0x40 is negative (lsls/asrs/b) or 24
when it is not (the taken bpl), then 20 when modM is unchanged, 38 when it changes with
modT 0 (PITCHG), 36 with modT 1/2 (VOLCHG).  `_081DD994` -> `_081DD998` 4;
`_081DD998` -> `_081DD87C` 18 (next track) or -> `_081DD9A4` 10 (last);
`_081DD9A4` -> `_081DD9BC` 22; a second tick in the same frame repeats
from the 14.

**Command dispatch** (wait == 0): `_081DD938` -> `_081DD8E0` 12; an
explicit command byte 21 to `_081DD8EC` then 14 to `_081DD8F6` (byte <
0xBD: the `bcc` is taken) or 10 (>= 0xBD, running status stored);
running status (data byte first) 25 straight to `_081DD8F6`.  Note (>=
0xCF): 34 to ply_note.  Other commands: 10 to `_081DD90C`, then 40 to the
handler (table call), or a W## wait: 10 to `_081DD92E` and 26 back to
`_081DD938` (93 for an explicit wait byte; step 2 charged 0 and had
absorbed it elsewhere).
Handler entry to `_081DD938` (handler, return, the flags test): tempo 91,
keysh 89, voice 184, vol 89, pan 91, bend 91, bendr 89, lfos 69, lfodl
81, mod 69 (value != 0) or 51 + clear_modM 58 = 109 (value 0), modt 91
(unchanged) / 95 (changed), xcmd 61 to the sub-handler + xiecv/xiecl 49,
goto 110 (2 + 108), patt 30 + goto 140, pend 18 + 22 = 40 (12 + 22 at
pattern level 0, where the `beq` skips the pop).  Not executed by the
corpus in 5 s (*est.* 90, the median of the measured one-byte handlers):
tune, rept, memacc, prio, endtie, fine, port.  The xcmd sub-commands
other than xiecv / xiecl are charged the measured 110 of those two
(*est.*).  ply_note exit `_081DDCEA` -> `_081DD938` 30.

**ply_note.**  Entry to `_081DDB46`: 78 with no data bytes (running
key/velocity), 97 with a key byte, 114 with key and velocity, *est.* 129
with a gate-extension byte too (none in the corpus).  Voice resolution to
`_081DDB9C`: plain voice 26 (22 + 4); keysplit 92 (45 + 35 + 12: the
table and sub-voice are read from ROM); drum (rhythm) 111 (30 + 2 + 35 +
27 + 17).  Then 48 to `_081DDBC4` (CGB) or 40 to `_081DDBEC` (DirectSound;
47 when the voice came from a keysplit/drum table in ROM, which also adds
25 to TrkVolPitSet below for the type/wav/attack reads).  CGB channel
test `_081DDBC4` -> `_081DDC40`: 20 when the channel is free (status 0),
26 when it has SF_STOP; a sounding channel taken by priority *est.* 32,
by equal priority and track order 48 (mt_chimney, one instance); a
rejected note *est.* 34 (priority) / 42 (track order) to `_081DDCEA`.  DirectSound scan:
`_081DDBEC` -> `_081DDBFA` 14, then per channel: free -> `_081DDC40` 14
(scan ends); ON+STOP as the first stopping candidate 32 + 2; ON+STOP with
a stopping candidate already 24 to `_081DDC18`; ON without STOP 20 to
`_081DDC14`, then 10 to `_081DDC34` when a stopping candidate exists or 4
to `_081DDC18`; `_081DDC18` -> `_081DDC24` 12 (priority >= best; strictly
lower priority *est.* 16 straight to `_081DDC32`, never taken); `_081DDC24`
-> `_081DDC32` 18 (equal priority, later track) / 16 (equal priority,
same track: the taken bls and the bcc that falls through), -> `_081DDC34`
22 (equal priority, earlier track), higher priority *est.* 8; `_081DDC32` -> `_081DDC34` 2; `_081DDC34` -> `_081DDBFA`
12 (next), -> `_081DDC40` 12 (candidate chosen after the last channel),
*est.* 14 to `_081DDCEA` with none.  Channel setup: `_081DDC40` ->
ClearChain 12, 33 to RealClearChain, RealClearChain -> `_081DDC54` by the
channel's links: track 0 (already dropped from a chain) 44; in a chain
with prev 0/next 0 68, prev 0/next set 64, prev set/next 0 70, both set
66; **+4 to each when the new track's chain is empty after the unlink**
(the taken `beq _081DDC54`; this is the 44/48 and 68/72 split).
`_081DDC54` -> `_081DDC66` 18 (lfoDelay 0) or 24 + clear_modM 36 = 60;
`_081DDC66` -> TrkVolPitSet 14; TrkVolPitSet -> ChnVolSetAsm 100 + 82
with VOLSET (+4 when modT == 1) + 50 with PITSET (-2 when modT != 0) + 25
for a keysplit/drum voice; ChnVolSetAsm -> `_081DDCA0` 78; CGB: 28 to
`_081DDCBC` (32 with a real sweep value, pan bit clear and time bits set:
sq1_sweep; *est.* +16 for a drum/keysplit CGB voice, length and sweep
from ROM), 30 to MidiKeyToCgbFreq, 190 to `_081DDCDC` (noise 98);
DirectSound: 12 to `_081DDCCE`, 20 to MidiKeyToFreq, 235 to `_081DDCDC`;
`_081DDCDC` -> `_081DDCEA` 14.  A note on a free DirectSound channel 0
with a plain voice and no flags therefore costs 77 + 114 + 26 + 40 + 14 +
14 + 12 + 33 + 44 + 18 + 14 + 100 + 78 + 12 + 20 + 235 + 14 + 30 = 895
from `_081DD938` back to `_081DD938`.

**VOLCHG/PITCHG pass**, a flagged existing track: `_081DD9C8` ->
TrkVolPitSet 30; TrkVolPitSet -> `_081DD9E6` 62 + 82 with VOLSET (+4 when
modT == 1) + 50 with PITSET (144 / 112 / 194 measured for flags 83 / 8C /
8F), or 6 more to `_081DDA58` when the chain is empty (150 / 118 / 200).
Per chained channel: `_081DD9E6` -> `_081DD9F6` 14 (ON); with VOLCHG 24
to ChnVolSetAsm and 72 (DirectSound) / 74 (CGB, MO_VOL set) to
`_081DDA14`, without 20; with PITCHG 24 to `_081DDA28`, then CGB 32 to
MidiKeyToCgbFreq and 200 to `_081DDA52` (noise *est.* 108), DirectSound
10 + 16 to MidiKeyToFreq and 237; without PITCHG 14 to `_081DDA52`;
`_081DDA52` -> `_081DDA58` 6 after the last channel (*est.* 10 back to
`_081DD9E6` for the next: no flagged track in the corpus had two channels
sounding); a dead channel in this pass *est.* 14 + 12 + 33 + 54 + 6 (not
observed: the tick's walk drops it first).  `_081DDA58` -> `_081DDA62`
10.  Step 2's "376 per channel" was 14 + 24 + 74 + 24 + 32 + 200 + 6 = 374
for a CGB channel with both flags.

**TrackStop.**  Only MPlayStart calls it in the corpus, before any
channel sounds, so no write is placed by it; its loop was instrumented
(TrackStop_Loop/_1/_2/_3) but never ran with a chained channel.  Left as
the SoundMain-stamped writes of step 2.

**m4aSoundMain's other work**: the CgbSound entry also moves with
`SoundMainBTM`'s 68 only on a track's start tick, nothing else in the
frame varies with song state before CgbSound.

**Inside CgbSound** (compiled C: breakpoints on addresses read from the
disassembly, appended to the nm listing as `CgbSound_*`; per-pass table
`tools/probe_cgb.py`, segment table `probe_cgb.py --segments`, over the
corpus for 1200 frames and the PSG micro-tests).  The cursor model of
`m4a_channel.c` is these segments (`CGB_CYC_*`), named after the labels;
cycles between consecutive breakpoints / writes:
- CgbSound -> channel-1 loop head 53 (51 when c15 was 0 and reloads 14);
  an inactive channel: loop head -> next 28 -> next loop head 16 (44);
  after channel 4: next -> SoundMainRAM 99 / 95.
- Active pass: loop -> active 26; the register switch active -> step2 69
  (square 1), 61 (square 2), 75 (wave), 69 (noise); step2 -> start 24 or
  -> nostart 30.
- Start block: start -> CgbModVol 44; CgbModVol -> first write: NR10 135,
  NR21 131, NR30 147 (= 0x40, before a wave RAM upload) / 153 (= 0, no
  upload), NR41 141, each + **CgbPan's path**: right >= left and not hard
  right 0, hard right (right/2 >= left) +4, left > right and not hard left
  +6, hard left +10 (the four branches of the comparison; CgbModVol's
  other arithmetic is constant).  NR10 -> NR11 10; NR30 -> WAVE 26, WAVE
  -> WAVE 15, WAVE -> NR30 6, NR30 -> NR31 4; NR41 -> NR43 8.  Last start
  write -> envelope_step_complete: attack != 0: 70 (square) / 62 (wave,
  noise); attack 0 -> envelope_decay_start +40; decay 0 too ->
  envelope_sustain_start +76 (sq1: 146 + 18 + 10 + 28 = the 202 of step 2).
- nostart -> envelope_step_repeat 38 (46 when SF_STOP is set with the
  envelope already in release); SF_STOP with an envelope phase (release
  start): -> envelope_step_complete 84 (wave 78) when release != 0, else
  the pseudo-echo start: -> envelope_complete 114 with an echo, -> 
  oscillator_off 94 without; SF_IEC: -> envelope_complete 42, -> 
  oscillator_off 40 when the length expires.
- The step (envelopeCounter 0): step -> modvol 18 (wave 20, it sets MO_VOL
  first) -> CgbModVol 10; CgbModVol -> envelope_step_complete, + the pan
  class: sustain 147, release 155, attack 173, decay 179, attack -> decay
  211, decay -> sustain 219 (wave 213), decay reaching a sustain of 0 ->
  oscillator_off 211 (echo *est.* 237), release ending -> pseudo-echo
  start -> envelope_complete 191 / oscillator_off 165; counter != 0: 12.
  envelope_step_complete -> envelope_complete 18, or -> the step again 24
  (prevC15 == 0).
- envelope_complete -> pitch block 10 (NRx3 28 later, +40 TONEDATA_TYPE_FIX,
  noise NR43 32; NRx3 -> NRx4 32, noise 24; NRx4 -> volume block 12 or ->
  channel_complete 18) / -> volume block 26 / -> channel_complete 32;
  volume block: NR51 21, NR51 -> NRx2 28 (wave NR32 32), NRx2 -> NRx4 10,
  wave NR32 -> NR30 = 0x80 16 -> NR34 6 -> channel_complete 18 (NR32 ->
  channel_complete 20 without the retrigger); NRx4 -> channel_complete 24
  (square 1: the sweep-direction test, bit set), 12 (square 2 / noise);
  square 1 with NR10 bit 3 clear writes NR14 again (*est.* 14, then 6: the 24 path without its taken branch).
- oscillator_off -> CgbOscOff 14; CgbOscOff -> NR12 37 (-> NR14 14), NR22
  37 (-> NR24 6), NR30 51, NR42 45 (-> NR44 6); -> channel_complete 22;
  channel_complete -> next 4 -> next loop head 16.
- MidiKeyToCgbFreq: 190 (square / wave) in ply_note and 200 in the pitch
  pass, **180 with a key at or below 35** (the `key = 0` branch; groudon's
  low wave notes); noise 98, **94 when the key is clamped** (key > 80; key
  <= 20 *est.* the same).  ply_note with a real sweep value (pan bit clear,
  time bits set): `_081DDCA0` -> `_081DDCBC` 32 instead of 28.

**The FIFO refill DMA.**  The two sound FIFOs are refilled by DMA 1 / DMA 2
with four words each at the same timer-0 overflow, once every 16 samples
= 20064 cycles; the eight FIFO writes sit at frame cycle 15091..15105
(relative to VBlank line 160; the previous refill is at -4973, the next at
35155) and stall the CPU for **18 cycles**: every segment of MPlayMain or
CgbSound that spans 15091 measured 18 long (ply_vol 58, keysh 107, xiecv
67, MidiKeyToCgbFreq 208, TrkVolPitSet 125/207/257 ...), nowhere else.
`m4a_gba_fifo_dma_stall` charges it in both models from the engine's own
refill schedule (`fifoDmaCycle` + k x 16 timer periods).

**Corrections to the step-3 numbers above found by the comparison tools**
(each verified by the write-cycle diff going to 0 on the test named):
`_081DD8EC` -> `_081DD8F6` is 14 for a byte below 0xBD and 10 at or above
(the taken `bcc`; sq1 start frame); an explicit W## costs 93 (sq1); the
LFO half costs 26 for a negative lfoSpeedC - 0x40 and 24 otherwise
(mt_chimney, lfo_*); the scan's equal-priority compare costs 16 for the
same track and 18 for a later one (pcm_psg chord); pend is 40 with a
pattern level and 34 at level 0 (vs_trainer / vs_wild); endtie 95 with a
key byte, 89 without (rg_vs_trainer, one each); a CGB channel taken by
track order at equal priority 48 (mt_chimney, one instance); the pitch
pass's TrkVolPitSet keeps the full 50 for PITSET with modT != 0 (the -2 is
ply_note's only); `_081DDA52` -> `_081DD9E6` 12.

### SoundMain timing probe, step 4: the SE players (2026-10-24, milestone 4 slice 1)

`mgba_probe` takes an eighth argument, the `gMPlayInfo_*` symbol whose
track flags and tempoC the `.frames` summary reads (default
`gMPlayInfo_BGM`; the breakpoint log always covers every player, r7 at
MPlayMain's labels being the MusicPlayerInfo), and `probe_mplay.py
--player SE1|SE2|SE3|<addr>` selects the run it prints.  Runs: the six
SEs of the corpus for 180 frames each (type 0, ids 33 se_exp, 16 se_faint,
24 se_contest_place, 51 se_truck_unload, 90 se_low_health, 86
se_rain_stop), 3 s; the numbers below hold on every one of their 177
complete frames.  Cycles from VBlank line 160.

**The chain.**  `MPlayOpen` prepends each player to SoundInfo's list
(BGM, SE1, SE2, SE3, then the two cry players, `m4aSoundInit`), and
MPlayMain calls the previously opened player before its own body
(`_081DD82E`: `bl call_r3` on `o_MusicPlayerInfo_func`), so SoundMain
enters cry 1 at 291, cry 0 at 346, SE3 401, SE2 456, SE1 511, BGM 566, and
the bodies run BGM first.  A paused player costs 71 from its entry to
`_081DDA6C` only when it is the BGM player (ident check, func test, push,
status test); every other paused player resumes at the deeper player's
`_081DDA6C` and reaches its own `_081DDA6C` **63** later (the deeper
player's epilogue, `pop {r0}`, the register pushes, the status test).  A
playing SE player resumes the same way and reaches FadeOutBody **88**
after the deeper `_081DDA6C` and `_081DD9BC` **54** after that (142; the
BGM player's 96 + 54 = 150 includes its ident and func tests).  After its
`_081DDA6C` the outer paused players unwind 63 each and CgbSound follows
the last by 55.  Measured positions, all frames:

| player | gMPlayTable | tracks | body starts at | FadeOutBody | `_081DD9BC` | `_081DDA6C` -> CgbSound |
|---|---|---|---|---|---|---|
| BGM | 0 | 10 | 566 (entry) | 662 | 716 | 370 = 5 x 63 + 55 |
| SE1 | 1 | 3 | 637 (BGM's `_081DDA6C`) | 725 | 779 | 307 = 4 x 63 + 55 |
| SE2 | 2 | 9 | 700 | 788 | 842 | 244 |
| SE3 | 3 | 1 | 763 | 851 | 905 | 181 |

With every player paused CgbSound is at 1007 whichever player the test
selected (frame 2 of every run, and every frame after the SE's FINE:
637 + 63 + 307 = 700 + 63 + 244 = 763 + 63 + 181 = 1007).  The engine's
`m4a_sequencer_set_player` derives entry / head / paused / exit from
`M4A_CYC_MP_BGM_ENTRY` 566, `M4A_CYC_MP_PAUSED` 71, `M4A_CYC_MP_IDLE` 63,
`M4A_CYC_MP_SE_HEAD` 142 and `M4A_CYC_MP_TAIL` 55, and both track loops
run over the player's track count (3 / 9 / 1), each non-existing track
costing the 16 / 14 of step 3 (se_truck_unload: 8 empty SE2 tracks per
tick, 16 + 18 each; se_contest_place: 5 empty tracks in the apply pass).
Track counts and the `ms` field are read from
`sound/music_player_table.inc` and `sound/song_table.inc`; the plan's
earlier "+291 / +346 / ... give the others" is the entry order above.

**Paths the SEs took that the songs never did** (each verified by the
write diff going to 0 on the test named):
- **ply_fine**, measured for the first time: entry -> `ply_fine_done` 16
  with an empty chain (se_faint, se_contest_place's tracks 2-4), or entry
  -> `ply_fine_loop` 10, loop -> `ply_fine_ok` 14 (SF_ON tested, SF_STOP
  set), ok -> RealClearChain 12, RealClearChain -> `ply_fine_done` 50
  (prev 0 / next 0; the walk's 54 less the loop's taken `bne`), and
  `ply_fine_done` -> `_081DD994` 28: the handler returns to the flags test
  at `_081DD926`, which finds the track gone and takes the `beq
  _081DD994`, so FINE's cost ends at `_081DD994`, not `_081DD938` (44 empty,
  114 with one channel: se_exp, se_truck_unload, se_rain_stop,
  se_contest_place's track 1).  Step 3 charged the 90 default plus 14 + 54
  per channel: +46 / +44 on the FINE frame.  A chain of two or more
  channels at FINE is still unmeasured (est.: the walk's
  `M4A_CYC_WALK_UNLINK_*` for the continuing iterations).
- **The tick with no track left** (the frame after FINE, status ->
  PAUSE): `_081DD9A4` -> `_081DDA6C` **24** (clock increment, the `cmp r4`,
  the status store, the branch), se_truck_unload frame 31 = 842 + 22 + 8 x
  34 + 16 + 10 + 24 + 244 = 1430; `M4A_CYC_TICK_ALL_DONE`.  Step 3 charged
  the exit straight from `_081DD9A4` (R1#14 in its review, "the frame where
  every track has finished", unmeasured then).
- **Square 1's second NR14 write** (sweep register bit 3 clear: se_faint's
  voice 90, sweep 0x17): NR14 -> NR14 **24**, then -> channel_complete
  **2** (`CGB_CYC_NR14_NR14` / `CGB_CYC_NR14_DONE`, step 3's *est.* 14 + 6;
  se_faint's start frame was -10 / -6 on the writes after it).

**Write-cycle residual** (engine minus mgba, every PSG write of the 180
frames, `probe_diff.py` threshold 0) after the three corrections: se_exp
0 on 284 writes, se_faint 0 on 38, se_contest_place 0 on 51,
se_truck_unload 0 on 22, se_low_health 0 on 100 (se_rain_stop has no PSG
writes); `entry_diff.py` / `cmd_diff.py` / `track_diff.py`: 0 divergences
on all 177 frames of each.  The three corrections moved nothing else: the
write diff of every probed micro-test and song in `build/probe/` (14 PSG
micro-tests, pcm / pcm_cmp / wave over 180 frames, the six songs over 1200)
stayed at 0, and the harness read the timing-3 baseline to the decimal
(38/38, every delta 0.0).

### SoundMain timing probe, step 5: the cry players (2026-10-24, milestone 4 slice 2)

Runs: four cries, probe type 1 (`ACC_TYPE_CRY`, the ROM's `StartCry` with
mgba_render's defaults pan 0, volume 120, pitch 15360 = key 60 at C_V,
length 140, priority 10), ids 15 Cry_Pidgey, 0 Cry_Bulbasaur, 381
Cry_Wailord, 123 Cry_Jynx, 180 frames each, the eighth argument
`gPokemonCryMusicPlayers` (in the ELF's nm listing; cry 1 is +0x40, so
`probe_mplay.py --player 03001f88` / `03001fc8` by address).  Cycles from
VBlank line 160; every number below holds on all 177 complete frames of
each of the four runs unless a frame is named.

**Which player.**  `SetPokemonCryTone` from silence takes cry 0
(`gPokemonCryMusicPlayers[0]` 0x03001F88, tracks `gPokemonCryTracks[0..1]`
0x03002258 / 0x030022A8): r7 at every MPlayMain label of the playing run
is 0x3001F88 on all four cries; cry 1 (0x3001FC8) only ever shows its
paused `_081DDA6C`.

**The chain** (step 4's rule, measured on the cry players): SE3's
`_081DDA6C` at **826** (= 566 + 71 + 3 x 63) every frame; the playing cry
0 reaches FadeOutBody **88** later (914) and `_081DD9BC` 54 after that
(**968** = 826 + 142, the SE head); after its own `_081DDA6C` the paused
cry 1 unwinds **63** and CgbSound follows **55** later: exit **118**.  With
every player paused (frame 2 and from frame 145 on) CgbSound is at
**1007**, as for the SE runs.  The player's second track (the song has
one) costs the step-3 16 in the tick loop and 14 in the apply pass.  Cry 1
playing is not measured (no run starts two cries): the chain rule gives
entry 889 / exit 55, est.

| player | gMPlayTable | tracks | body starts at | FadeOutBody | `_081DD9BC` | `_081DDA6C` -> CgbSound |
|---|---|---|---|---|---|---|
| cry 0 | (4th after BGM) | 2 | 826 (SE3's `_081DDA6C`) | 914 | 968 | 118 = 63 + 55 |
| cry 1 | (5th) | 2 | 889 est. | 977 est. | 1031 est. | 55 est. |

**The song data is in IWRAM.**  `gPokemonCrySongs` (0x03001F20) holds the
patched template, so every song-data byte MPlayMain reads costs less than
from ROM (WS0 3/1 with prefetch).  Measured pairs, ROM song (step 3) vs
cry (same labels, same state): command fetch `_081DD8E0 -> _081DD8EC` 21
vs **14**; ply_vol 89 vs **82**; ply_pan 91 vs **84**; ply_tune (step 3's
default 90) **84**; ply_voice 184 vs **177**; ply_goto 110 vs **82**
(`ply_goto` -> `ply_goto_1` 2 -> `_081DD938` 80; four bytes); ply_xcmd ->
its sub-handler 61 vs **54**; ply_note -> `_081DDB46` with key and
velocity bytes 114 vs **93** (three reads: the key, the velocity and the
gate test that finds the next command byte).  Every pair differs by **7
per song-data byte read** (`M4A_CYC_ROM_BYTE`): 7, 7, 7, 7, 28, 7, 21.
The rest of the frame -- the chain walk, Clear64byte 20 + 33, SoundMainBTM
68, the wait path 28, the empty-track 16 / 14, ply_note after
`_081DDB46` (22 / 4 / 40 / 14 / 14 / 12 / 33 / 48 / 18 / 14 / 232 / 78 /
12 / 20 / 235 / 14 / 30), ply_fine 16 + 28 -- is the same as on a ROM
song, as it reads RAM structures only.

**Handlers measured for the first time** (RAM song data; the ROM values
are est. as RAM + 7 per byte read, no vanilla song uses them): `ply_xcmd_0D`
-> `_081DD938` **120** (four bytes; ROM est. 148); `ply_xrele` -> `_081DD938`
**48** (ROM est. 55); `ply_xwait` -> `_081DD938` **108** while timer <
length (ROM est. 122) and **102** on the tick it expires (frame 143; ROM
est. 116); `ply_endtie` -> `_081DD938` **54** with no key byte and an empty
chain (the sample had ended; step 3's ROM 89 / 95 on rg_vs_trainer are
kept as they were measured, their chain state unrecorded).

**The frames of a cry** (CgbSound entry): 3 (start tick: TUNE, GOTO,
VOICE, VOL, XCMD 0D, XCMD RELE, PAN, TIE, XCMD WAIT) **4077**; the steady
xwait tick **1596** (fetch, xcmd 54 + xwait 108, the wait path, one live
channel in the walk); the tick after the sample ended **1689** (the walk
unlinks the dead channel: Pidgey frame 14, Bulbasaur 50, Wailord 122, Jynx
137) then **1576**; 143 (xwait expires, EOT, FINE) **1810**; 144 (the
finished tick, `M4A_CYC_TICK_ALL_DONE`) **1192**; 145 on **1007**.

**The cry data in the ROM** (`gCryTable`, 388 entries, every ToneData
`20 3C 00 00 <wav> FF 00 FF 00`: `cry` = `voice_directsound_compressed
60, 0, sample, 255, 0, 255, 0`).  Every cry sample header is type 1
(DPCM), status 0 (**no cry loops**), freq 10764288 (10512 Hz); sizes run
from 1902 (Pidgey, 0.18 s) to 23524 (Jynx, 2.24 s), so at pitch 15360 no
cry reaches the 140-tick EOT with its sample still playing; the EOT finds
an empty chain on every cry.

### SoundMain timing probe, step 6: cry variants (2026-10-24, milestone 4 slice 3)

Runs: three cries with `PlayCryInternal`'s non-default pitch / pan /
volume values through `StartCry`, probe type 1 for 180 frames each with
`gPokemonCryMusicPlayers`, the probe's new ninth / tenth / eleventh
arguments (pitch, pan, volume; `patchParams` wrote 0x3C00 / 0 / 120 before):
`cry_jynx_faint` (123, pitch 14440 = CRY_MODE_FAINT's, pan 0, volume 120),
`cry_bulbasaur_howl` (0, pitch 15800 = CRY_MODE_HIGH_PITCH's, pan 63, volume
120), `cry_pidgey_quiet` (15, pitch 15360, pan 0, volume 90 =
CRY_MODE_ENCOUNTER / HIGH_PITCH's).  Cycles from VBlank line 160, as in
step 5.

**The template bytes** (`SetPokemonCryPitch`: b = pitch + 0x80, key =
(b >> 8) & 0x7F, tune = (b >> 1) & 0x7F, tune2 = (16 + tune) & 0x7F from the
template's 0x50 - 0x40; `SetPokemonCryPanpot`: (pan + 0x40) & 0x7F;
`SetPokemonCryVolume`: volume & 0x7F), worked by hand before the first
render and confirmed by the probe (`_081DD938`'s r3 after the EOT is the
track key: 0x38 / 0x3E / 0x3C):

| variant | pitch | b | TIE key (0x2A) | TUNE (0x12) | TUNE2 (0x19) | PAN (0x28) | VOL (0x1D) |
|---|---|---|---|---|---|---|---|
| cry_jynx_faint | 14440 | 0x38E8 | 0x38 (56) | 0x74 (c_v + 52) | 0x04 | 0x40 | 0x78 |
| cry_bulbasaur_howl | 15800 | 0x3E38 | 0x3E (62) | 0x1C (c_v - 36) | 0x2C | 0x7F (63 + 64) | 0x78 |
| cry_pidgey_quiet | 15360 | 0x3C80 | 0x3C (60) | 0x40 (c_v) | 0x50 | 0x40 | 0x5A (90) |

Every other byte is the defaults' (`test_cry_song`).  14440 is
CRY_MODE_FAINT's pitch (the task's "CRY_MODE_WEAK" uses 15000); 90 is a game
value (ENCOUNTER / HIGH_PITCH; ECHO_END uses 70); pan 63 is the value
`battle_anim_throw.c` gives the opponent side (an SE pan there; the cry
callers pass -25 / 25 / 63 / -63).

**ply_endtie with a chained channel holding the key** (the first
measurement; step 5 derived `M4A_CYC_ENDTIE_MATCH` 28 as the ROM 89 minus
the RAM 61).  At pitch 14440 Cry_Jynx's 23524 samples run 2.75 s, past the
140-tick EOT at frame 143 (2.38 s), so the EOT's walk finds the channel:
`ply_endtie` -> `_081DD938` **82** on cry_jynx_faint's frame 143 against
**54** on the other three variants and all four defaults (empty chain, no
key byte): the match costs **28**, exactly the derived value.  The rest of
that frame: `ply_fine` -> `ply_fine_loop` 10 -> `ply_fine_ok` 14 ->
`RealClearChain` 12 -> `ply_fine_done` 50 -> `_081DD994` 28 (one live
channel unlinked, step 3's FINE costs), against `ply_fine` -> `_done` 16
with an empty chain; and the tick's walk over the live channel is the
steady +20 (1596 vs 1576).  Frame 143's CgbSound entry is **1928** = 1810
+ 20 + 28 + 70; 144 is 1192 and 145 on 1007 as on every cry (the FINE
cleared the track, so 144 has no walk).  `_SKIP` 18 (a chained channel
passed over) stays est.: no run has two channels on the cry track.

**Everything else is the defaults'**: the start tick 4077 (pitch, pan and
volume change bytes, not paths: ply_tune / ply_pan / ply_vol cost the same
whatever the operand), the steady xwait tick 1596 while the channel lives
and 1576 after it dies (the unlink tick 1689: cry_bulbasaur_howl frame 46,
four frames before cry_bulbasaur's 50, the sample being faster at 15800;
cry_pidgey_quiet frame 14 as cry_pidgey; cry_jynx_faint never), the 2-cycle DAC rounding on SoundMainRAM_Buffer's
entry on frames 137 / 141 as before.  `entry_diff.py` 0 on all 177
complete frames, `cmd_diff.py` / `track_diff.py` 0 on each of the three
with the port's existing constants: the harness needed no new number.
The release-0 cut at the EOT (the channel's SF_STOP on frame 143, gone on
144) is the first audible one in the corpus: no sample run differs there.

### SoundMain timing probe, step 7: cry length / release (2026-10-24, milestone 4 slice 4)

**The ROM change.**  `struct AccParams` (test/accuracy/rom/main.c, the
`PORYACC1` block in crt0.s) gains `u16 length` and `u8 release` from its
reserved bytes (offsets 16..18 of the block; the earlier fields keep their
offsets, the block stays 28 bytes before the code that follows it -- the
"32" in the patchers is only their bounds check), defaulting to 140 / 0 in
the ROM's data so a block patched by an old renderer plays the old values;
`StartCry` passes them to `SetPokemonCryLength` / `SetPokemonCryRelease`
in `PlayCryInternal`'s order.  `mgba_render --length / --release` and
`mgba_probe`'s twelfth / thirteenth arguments patch them.  The longer
`StartCry` moved every ROM symbol after main.o by **+0x30** (`CgbSound`
0x8000ff0 -> 0x8001020, `MPlayMain` 0x80025b4 -> 0x80025e4, `gCryTable`
0x0802af7c -> 0x0802afac; IWRAM is unchanged, `gPokemonCrySongs` 0x03001f20):
the compiled `CgbSound` is byte-identical at the new address (537
instructions compared address-normalised; the one differing word is a
literal-pool ROM pointer, +0x30), so the `CgbSound_*` labels in the skill
and the probe header are the old ones +0x30 (loop 0x8001050 .. next
0x8001450).  The references' directory moved to
`build/accuracy/refs/e6551cf3bb92-mGBA_0.10.5+m4a_blip/`.  **Neutrality**:
the full default harness and `claim_check.sh` against
`2026-10-24_cry-variants.json` with the new ROM and the unchanged engine
give every one of the 51 rows identical in every field of report.json
(null, worst, gm, lag, RMS, peak, drift; 32 rows still -inf), 46/51, 44
diffed with 0 moved writes, unit 1221/1221, blip 0, songs 561/561; the
cry_jynx_faint probe re-run under the new ROM gives a `.frames` file
identical to the old ROM's.  (The ELF and map the CMake target writes are
`build/accuracy/rom/accuracy_test.elf` / `.map`; `test/accuracy/rom/build/`
was a stale hand build from the morning whose `nm` showed no change --
the skill now names the CMake paths.)

**The probe's channel-death breakpoint.**  No MPlayMain tick can show
when a releasing channel dies once `ply_fine` has unlinked it (the track
is gone, so there is no 1689-type unlink frame), so the probe now also
breaks on m4a_1.s `_081DCFB0`, SoundMainRAM's `statusFlags = 0` store (a
release or pseudo-echo tail reaching 0, or SF_START with SF_STOP) -- at
its **IWRAM** address: SoundMainRAM runs from `SoundMainRAM_Buffer`
(0x03000770), so the ROM label (0x08001d70) never executes and the probe
relocates it to 0x03000878.  A sample that runs out of data ends through
the count path instead and does not hit it (cry_pidgey, cry_pidgey_roar2).

Runs: three cries with `PlayCryInternal`'s whole modes (pitch, length and
release together; chorus stays 0 where the mode sets it -- none of these
three does), probe type 1 for 180 frames with `gPokemonCryMusicPlayers`:
`cry_jynx_faint_full` (123; CRY_MODE_FAINT complete: pitch 14440, length
140, release 200), `cry_wailord_doubles` (381; CRY_MODE_DOUBLES: pitch
15360, length 20, release 225), `cry_pidgey_roar2` (15; CRY_MODE_ROAR_2:
pitch 15616, length 60, release 225).  Cycles from VBlank line 160, as
before.

**The template bytes**, worked by hand before the first render
(`SetPokemonCryPitch` as step 6; `SetPokemonCryLength` the xWAIT u16 at
0x2E..0x2F little-endian; `SetPokemonCryRelease` the xRELE byte at 0x26)
and confirmed by the probe's track key after the EOT (`_081DD938`'s r3:
0x38 / 0x3C / 0x3D):

| entry | pitch | b | TIE key (0x2A) | TUNE (0x12) | TUNE2 (0x19) | xWAIT (0x2E,0x2F) | xRELE (0x26) |
|---|---|---|---|---|---|---|---|
| cry_jynx_faint_full | 14440 | 0x38E8 | 0x38 (56) | 0x74 | 0x04 | 0x8C 0x00 (140) | 0xC8 (200) |
| cry_wailord_doubles | 15360 | 0x3C80 | 0x3C (60) | 0x40 | 0x50 | 0x14 0x00 (20) | 0xE1 (225) |
| cry_pidgey_roar2 | 15616 | 0x3D80 | 0x3D (61) | 0x40 | 0x50 | 0x3C 0x00 (60) | 0xE1 (225) |

PAN 0x40 and VOL 0x78 (the defaults) on all three.  Cry_Pidgey's 1902
samples at key 61 (10512 * 2^(1/12) = 11137 Hz) run 0.171 s, so the
sample is long gone at tick 60 and the EOT finds an empty chain, as the
task's (c) requires; Cry_Wailord's 20846 samples (1.98 s) are cut at tick
20; Cry_Jynx's 23524 at pitch 14440 (2.75 s) are cut at tick 140 as in
step 6.

**The sequencer frames** are the known ones: start tick 4077 on frame 3;
xwait ticks 1596 with the channel alive and 1576 after it dies (the
unlink tick 1689 on cry_pidgey_roar2's frame 14, as cry_pidgey's); the
EOT frame **1928** on cry_jynx_faint_full's 143 and cry_wailord_doubles'
23 (`ply_endtie` -> `_081DD938` **82**, the chained channel holding the
key, then `ply_fine` -> `_loop` 10 -> `_ok` 14 -> `RealClearChain` 12 ->
`_done` 50 -> `_081DD994` 28, the live channel unlinked), **1810** on
cry_pidgey_roar2's 63 (`ply_endtie` -> `_081DD938` **54**, empty chain;
`ply_fine` -> `_done` 16); 1192 on the frame after, 1007 from then on.
The xRELE byte changes nothing here: `ply_xrele` costs the same for any
operand and the release value only reaches SoundMainRAM.  `ply_fine`
with a releasing channel takes the same path as with the release-0
channel of step 6 (SF_STOP already set by `ply_endtie`; `ply_fine_ok`
clears the channel's track pointer, flg F0 = ON | STOP | ... as logged).

**The channel's death** (the `_081DCFB0` stamps, raw `fN` = frame N-1):
cry_jynx_faint_full **frame 161**, cry_wailord_doubles **frame 53**,
cry_jynx_faint (release 0, re-probed) frame 143.  This is the GBA's
release arithmetic exactly: `envelopeVolume = envelopeVolume * release
>> 8` once per SoundMainRAM pass starting on the EOT frame itself
(MPlayMain's `ply_endtie` sets SF_STOP before that frame's SoundMainRAM),
and `_081DCFB6`'s `cmp r5, pseudoEchoVolume; bhi` with echo 0 stops the
channel on the pass whose product is 0 (that pass mixes nothing).  From
255 (attack 255 reaches it on the first pass; decay 0 / sustain 255 hold
it): release 200 gives 199, 155, 121, 94, 73, 57, 44, 34, 26, 20, 15,
11, 8, 6, 4, 3, 2, 1, 0 -- 18 mixed release passes on frames 143..160
and the stop on 161; release 225 gives 224, 196, 172, 151, 132, 116,
101, 88, 77, 67, 58, 50, 43, 37, 32, 28, 24, 21, 18, 15, 13, 11, 9, 7,
6, 5, 4, 3, 2, 1, 0 -- 30 mixed passes on frames 23..52 and the stop on
53.  (With masterVolume 12 the per-channel volume is `(13 * env) >> 4`,
0 at env 1, so the last pass that can move the output is env 2, one frame
before the stop.)  `_SKIP` 18 stays est.: no run here puts two channels
on the cry track either.  The scheduler-lateness floor of each run
(`.events`): cry_jynx_faint_full 4 timer-0 slot crossings (first at
cycle 28146944), cry_wailord_doubles 6 (first 3428099),
cry_pidgey_roar2 1 (2304512).

### SoundMain timing probe, step 8: the reverse cries (2026-10-25, milestone 4 slice 5)

**No ROM change.**  `gCryTable_Reverse` is not a second table but a label
inside the block `sound/cry_tables.inc` assembles (`gCryTable::`, 388 `cry`
lines, `gCryTable_Reverse::`, 388 `cry_reverse` lines), so the test ROM's
`StartCry(index)` with `&gCryTable[index]` reaches the reversed entries at
388 + the forward index (Bulbasaur's forward cry is index 0, its reverse 388): the map has `gCryTable` 0x0802afac and `gCryTable_Reverse`
0x0802c1dc, 0x1230 = 388 * 12 apart.  The runner's `"id"` stays the linear
index.  The 12-byte entries read from the ROM (type, key, _, pan, wav,
attack, decay, sustain, release):

| id | entry | ROM bytes | type | wav (= the forward entry's) |
|---|---|---|---|---|
| 0 | cry Cry_Bulbasaur | `20 3c 00 00 bc 39 0d 08 ff 00 ff 00` | 0x20 CMP | 0x080d39bc |
| 388 | cry_reverse Cry_Bulbasaur | `30 3c 00 00 bc 39 0d 08 ff 00 ff 00` | 0x30 CMP \| REV | 0x080d39bc |
| 15 | cry Cry_Pidgey | `20 3c 00 00 f0 30 0e 08 ff 00 ff 00` | 0x20 | 0x080e30f0 |
| 403 | cry_reverse Cry_Pidgey | `30 3c 00 00 f0 30 0e 08 ff 00 ff 00` | 0x30 | 0x080e30f0 |
| 123 | cry Cry_Jynx | `20 3c 00 00 e8 77 15 08 ff 00 ff 00` | 0x20 | 0x081577e8 |
| 511 | cry_reverse Cry_Jynx | `30 3c 00 00 e8 77 15 08 ff 00 ff 00` | 0x30 | 0x081577e8 |

Key 60, pan 0, envelope 255 / 0 / 255 / 0 on all six, as
`voice_directsound_compressed_reverse 60, 0, \sample, 255, 0, 255, 0`
(the `cry_reverse` macro, `.byte 0x30` then `_voice_directsound`).  The
samples are the forward ones byte for byte: 16-byte headers
`01 00 00 00 00 40 a4 00 00 00 00 00 <size LE>` (type 1 = DPCM, freq
10764288 = 10512 Hz, loopStart 0; sizes 8183 / 1902 / 23524), the data
`size / 64` full blocks of 0x21 bytes and a partial last block holding only
the nibbles the remaining samples need (the next sample's header follows 4
/ 6 / 12 bytes before a full block's end); the loader allocates
`size / 64 + 1` full blocks zero-filled, and since Unk2 always decodes 33
bytes the GBA's decode of the last block reads those few header bytes
where the port reads zeros -- for sample indices >= size only, which
neither direction outputs (the review below).

**The loader.**  `voicegroup_cry_table_symbol` stopped at the
`gCryTable_Reverse::` label, so no index past 387 resolved and every cry
loaded as type 0x20: the first fix of the slice.
`voicegroup_cry_table_entry` (plugin/voicegroup_loader.c) now counts the
`cry` / `cry_reverse` (/ `cry_uncomp` / `cry_reverse_uncomp`) lines through
that one label and returns the macro's type byte; `voicegroup_load_cry_typed`
gives the engine a voice of that type (`voicegroup_load_cry` is the 0x20
form); `poryaaaa_render --cry 388` prints "Cry_Bulbasaur (reversed:
gCryTable_Reverse's entry)" and "played in reverse".  Gate:
`test_cry_table_index` (a miniature table with both labels in a temporary
root, and the vanilla file's 388 / 403 / 511 / 123 / 776 when ~/pokeemerald
is present).  The eventide `cry_reverse_custom` / `_reverse` voicegroup
macros were already parsed by `parse_voicegroup_file`; this is only the
by-index path.

**What the reversed path reads at the sample's edges** (read before the
first render, so a run at the start or the end can be attributed).
SoundMainRAM_Unk1's first-mix setup for a REV channel (m4a_1.s
`_081DD29C`'s predecessor: `r3 = size + 2 * wav + 0x20 - r3`, i.e. data +
size with r3 at data) flips `currentPointer` to the end, and the CMP
branch then stores it back as the index `size`; the reversed CMP walk
(`_081DD3C0`) decodes index `size - 1` as the first output and `size - 2`
as its look-behind, both inside the `size / 64 + 1` allocated blocks
(Bulbasaur 8182 in block 127 of 128, Pidgey 1901 in 29 of 30, Jynx 23523
in 367 of 368) -- so **nothing before the data is read at the start**; the
byte before each sample's data is the header's last byte, `size >> 24` =
0x00 for all three (that byte is what the *uncompressed* REV path's
`ldrsb r1, [r3, -1]` would read at the end; the port's `REV_RD(-1)` = 0
matches it).  At the **end**, the walk reaches sample 0 with `r2 = 1`
still to consume: it decodes "block -1" as sample 0's look-behind
(`SoundMainRAM_Unk2` with r3 = -1: block 0x3FFFFFF, 0x21 * 0x3FFFFFF +
wav + 0x10 = `wav + 0x83FFFFEF` mod 2^32 = 0x8C0D39AB / 0x8C0E30DF /
0x8C1577D7), an address outside the cartridge space that mgba's
`GBALoad8` default case serves as open bus (`GBALoadBad`: `prefetch[1]`,
the IWRAM Unk2 instruction word two ahead of each `ldrb`, byte lane
`address & 3`; all three addresses are lane 3, so the 33 bytes are
`e4 01 00 96 e1 d0 ...` from the words 0xe4d21001 / 0xe19600d0 and the
delta decode gives **38** at block offset 63).  The port's `unk2` decodes
a zero block for any index past the allocation and reads **0** there.
The difference reaches the one or two outputs between the last two
crossings (`r0 = d[0]`, `r1 = lookbehind - d[0]`, `(r9 * r1) >> 23` with
r9 the fraction below one sample): one output of up to `38 * frac`
sample units at the forward sample's first sample, i.e. a **one-sample
run at the reversed cry's end** if it shows at all; a release that stops
the channel before sample 0 ((b) below) never gets there.  Recorded here
so that run, if it appears, is attributed to the open-bus look-behind and
not to the decode; emulating mgba's prefetch there is a mixer change and
is not made in this slice.

**The template bytes**, worked by hand before the first render
(`SetPokemonCryPitch`: b = pitch + 0x80, TIE key = b >> 8, TUNE = (b >> 1)
& 0x7F, TUNE2 = (16 + TUNE) & 0x7F with chorus 0; `SetPokemonCryLength` the
xWAIT u16 at 0x2E..0x2F; `SetPokemonCryRelease` the xRELE byte at 0x26;
`SetPokemonCryVolume` the VOL byte at 0x1D) and confirmed by the probe's
track key after each EOT (`_081DD938`'s r3 on the line after `ply_endtie`:
0x3C / 0x3B / 0x3D).  The mode's `reverse` flag is not a template byte: it
selects the table (`GET_CRY(index, table, reversed)`), hence the id.

| entry | id | mode | pitch | b | TIE key (0x2A) | TUNE (0x12) | TUNE2 (0x19) | VOL (0x1D) | xWAIT (0x2E,0x2F) | xRELE (0x26) |
|---|---|---|---|---|---|---|---|---|---|---|
| cry_bulbasaur_reverse | 388 | CRY_MODE_NORMAL defaults | 15360 | 0x3C80 | 0x3C (60) | 0x40 | 0x50 | 0x78 (120) | 0x8C 0x00 (140) | 0x00 |
| cry_jynx_growl1 | 511 | CRY_MODE_GROWL_1: reverse, length 15, release 125, pitch 15200 | 15200 | 0x3BE0 | 0x3B (59) | 0x70 | 0x00 | 0x78 | 0x0F 0x00 (15) | 0x7D (125) |
| cry_pidgey_echo_start | 403 | CRY_MODE_ECHO_START: reverse, length 25, release 100, pitch 15600, volume 90, chorus 192 | 15600 | 0x3D70 | 0x3D (61) | 0x38 | 0x48 | 0x5A (90) | 0x19 0x00 (25) | 0x64 (100) |

PAN 0x40 on all three.  CRY_MODE_ECHO_START's chorus 192 is not patchable
(the ROM's `StartCry` sets no chorus), so (c) is one track with chorus 0
and its note says so; its volume 90 is part of the mode and is patched
(`--volume 90` / `--cry-volume 90`).  Length against pitch, as slice 4
checked: Cry_Pidgey's 1902 samples at key 61 + TUNE 56 (about 11.0 kHz)
run 0.17 s, gone by frame 14, long before tick 25 on frame 28, so (c)'s
EOT finds an empty chain and the xRELE byte reaches no channel;
Cry_Jynx's 23524 at key 59 + TUNE 112 (about 10.36 kHz, 2.27 s) are cut
at tick 15 on frame 18 with 0.3 s played -- the *end* of the sample, since
it plays backwards; Cry_Bulbasaur's 8183 at 10512 Hz (0.78 s) run out on
frame 50 through the count path.

**The probe runs** (type 1, 180 frames, `gPokemonCryMusicPlayers`, the
parameters as the ninth to thirteenth arguments; cycles from VBlank line
160): the frame skeleton is slice 4's to the cycle.  cry_bulbasaur_reverse:
start tick 4077 on frame 3, 1596 while the channel lives, the unlink tick
**1689 on frame 50** (the sample ran out, as cry_bulbasaur's forward play
does on the same frame: the same 8183 samples at the same rate), 1576
after, the EOT **1810 on frame 143** (`ply_endtie` -> `_081DD938` 54, the
empty chain; `ply_fine` -> `_done`), 1192 on 144, 1007 from 145.
cry_jynx_growl1: 4077 on 3, 1596 to 17, the EOT **1928 on frame 18**
(`_081DD938` 82, the chained channel holding key 0x3B; `ply_fine` ->
`_loop` -> RealClearChain, the live channel unlinked), 1192 on 19, 1007
from 20; the channel's death at the `_081DCFB0` stamp **raw f25 = frame
24**: release 125 from 255 gives 124, 60, 29, 14, 6, 2, 0 -- six mixed
release passes on frames 18..23 and the stop on 24 (`env * 125 >> 8`,
the stop on the pass whose product is 0, exactly as the 200 / 225 chains
of step 7).  cry_pidgey_echo_start: 4077 on 3, 1596 to 13, the unlink
**1689 on frame 14** (as cry_pidgey / cry_pidgey_roar2), 1576 after, the
EOT **1810 on frame 28**, 1192 on 29, 1007 from 30; no death stamp (the
count path).  The reversed walk costs SoundMainRAM, not MPlayMain, so no
sequencer constant changes.  Scheduler-lateness floor (`.events`):
cry_bulbasaur_reverse **0** timer-0 slot crossings, cry_pidgey_echo_start
**0**, cry_jynx_growl1 4 (cycles 1180928, 2304515, 3428098, 5675266 =
0.0704, 0.1374, 0.2043, 0.3383 s; the last two inside the release tail).

### SoundMain timing probe, step 9: cry chorus, the two-track modes (2026-10-25, milestone 4 slice 6)

**The ROM change.**  `struct AccParams` gains `s8 chorus` from its reserved
bytes (offset 19 of the `PORYACC1` block, default 0 in the ROM data, the
block stays 28 bytes), `StartCry` passes it to `SetPokemonCryChorus` in
`PlayCryInternal`'s order (after Release, before Priority); `mgba_render
--chorus` and `mgba_probe`'s fourteenth argument patch byte 11 of the
block.  `StartCry` / `StartTest` grew main.o by 0x1c, so every ROM symbol
after it moved **+0x1c** (`CgbSound` 0x8001020 -> 0x800103c, `MPlayMain`
0x80025e4 -> 0x8002600, `ply_note` 0x80028c0 -> 0x80028dc, `gCryTable`
0x0802afac -> 0x0802afc8, `_081DCFB0` 0x8001d70 -> 0x8001d8c; `StartTest`
itself +0x14; IWRAM unchanged, `gPokemonCrySongs` 0x03001f20); the compiled
`CgbSound` is byte-identical at the new address (505 Thumb instructions
and 19 literal-pool words compared address-normalised -- the two objdump
listings of 541 lines include the header and a tail past CgbSound's end;
the three `bl CgbModVol` / `CgbOscOff` encodings are the same relative
offsets, the one differing word is the literal-pool ROM pointer, +0x1c), so the `CgbSound_*` labels in the skill and the probe
header are step 7's +0x1c (loop 0x800106c .. next 0x800146c, each checked
to land on the same instruction).  The references' directory moved to
`build/accuracy/refs/d28b3dc2950d-mGBA_0.10.5+m4a_blip/`.  **Neutrality**:
the full default harness with the new ROM and the unchanged engine gives
every one of the 57 rows identical to `2026-10-25_cry-reverse.json` in
every field of report.json but the ref path (50/57, 32 rows still -inf);
the cry_jynx_faint probe re-run under the new ROM gives a `.frames` file
identical to the old ROM's.

**The modes** (src/sound.c `PlayCryInternal`, whole modes; `chorus` is a
`u32` there and `SetPokemonCryChorus` takes `s8`, so 192 is the s8 -64 --
the same byte 0xC0, and `(val + tuneValue) & 0x7F` is the same for both):
(a) CRY_MODE_ENCOUNTER on Cry_Bulbasaur (id 0): release 225, pitch 15600,
chorus 20, volume 90, length the default 140; (b) CRY_MODE_ECHO_END on
Cry_Jynx (id 123): release 220, pitch 15555, chorus 192, volume 70, length
140; (c) CRY_MODE_ECHO_START complete on cry_reverse Cry_Pidgey (id 403):
reverse, length 25, release 100, pitch 15600, chorus 192, volume 90 --
slice 5's cry_pidgey_echo_start with its chorus restored (that entry stays
as the one-track form).

**The template bytes**, worked by hand before the first render
(`SetPokemonCryPitch` as steps 6-8: b = pitch + 0x80, TIE key = b >> 8,
TUNE = (b >> 1) & 0x7F, TUNE2 = (16 + TUNE) & 0x7F; then
`SetPokemonCryChorus` with a nonzero value sets trackCount (0x00) to 2 and
**overwrites TUNE2 (0x19) with (chorus + TUNE) & 0x7F**, so the pitch
setter's `a = tuneValue2 - tuneValue` carry is discarded; `SetPokemonCryVolume`
the VOL byte at 0x1D, `SetPokemonCryLength` the xWAIT u16 at 0x2E..0x2F,
`SetPokemonCryRelease` the xRELE byte at 0x26):

| entry | id | mode | pitch | b | trackCount (0x00) | TIE key (0x2A) | TUNE (0x12) | TUNE2 (0x19) | VOL (0x1D) | xWAIT (0x2E,0x2F) | xRELE (0x26) |
|---|---|---|---|---|---|---|---|---|---|---|---|
| cry_bulbasaur_encounter | 0 | CRY_MODE_ENCOUNTER: release 225, pitch 15600, chorus 20, volume 90 | 15600 | 0x3D70 | 2 | 0x3D (61) | 0x38 (56) | (20 + 56) & 0x7F = 0x4C (76) | 0x5A (90) | 0x8C 0x00 (140) | 0xE1 (225) |
| cry_jynx_echo_end | 123 | CRY_MODE_ECHO_END: release 220, pitch 15555, chorus 192, volume 70 | 15555 | 0x3D43 | 2 | 0x3D (61) | 0x21 (33) | (192 + 33) & 0x7F = 0x61 (97) | 0x46 (70) | 0x8C 0x00 (140) | 0xDC (220) |
| cry_pidgey_echo_start_chorus | 403 | CRY_MODE_ECHO_START: reverse, length 25, release 100, pitch 15600, chorus 192, volume 90 | 15600 | 0x3D70 | 2 | 0x3D (61) | 0x38 (56) | (192 + 56) & 0x7F = 0x78 (120) | 0x5A (90) | 0x19 0x00 (25) | 0x64 (100) |

PAN 0x40 on all three.  Both tracks share the body from `cont` (VOICE 0,
VOL, XCMD 0x0D, xRELE, PAN, TIE key, xWAIT, EOT, FINE): track 0 starts at
`part0` (TUNE then GOTO cont), track 1 at `part1` (TUNE2, falling into
cont), so the two channels carry the same key and differ in `tune` only.
Length against pitch, as slices 4 and 5 checked: Cry_Bulbasaur's 8183
samples at key 61 (about 11.0 / 11.1 kHz on the two tracks) run 0.74 s and
end through the count path around frame 50, long before tick 140 -- so
(a)'s EOT finds two empty chains and the mode's release 225 reaches no
channel (the task's expectation that the EOT releases both live channels
does not hold for this sample; the mode is kept whole, not altered);
Cry_Jynx's 23524 at key 61 + TUNE 33 / 97 (about 10.83 / 11.47 kHz: 2.17
/ 2.05 s, i.e. about 130 / 123 frames from frame 3) end through the count
path around frames 133 / 126, **before** tick 140 on frame 143 -- unlike
cry_jynx_faint's pitch 14440, which outlives the EOT -- so (b)'s EOT also
finds two empty chains and the mode's release 220 reaches no channel (the
task's expectation of a two-channel release at the EOT holds for neither
(a) nor (b); the modes are kept whole); Cry_Pidgey's 1902 backwards are
gone by frame 14, (c)'s EOT on frame 28 finds empty chains as slice 5's
entry did.  The probe confirms the TIE key on **both** tracks after each
EOT: `_081DD938`'s r3 = 0x3D on the line after each of the two
`ply_endtie`s (r5 = 0x03002258 for track 0, 0x030022A8 for track 1).

**The probe runs** (type 1, 180 frames, `gPokemonCryMusicPlayers`, the six
parameters as the ninth to fourteenth arguments; cycles from VBlank line
160).  The two-track frame skeleton, identical on all three entries where
both tracks live: the start tick **6792** on frame 3 (`ply_note` twice, the
setup commands of both tracks: ply_tune 2, ply_goto 1, ply_voice 2, ply_vol
2, ply_xcmd 6 (0x0D 2, xRELE 2, xWAIT 2), ply_pan 2; the one-track start was
4077), **1932** on every frame with both channels alive (the one-track
1596), the unlink tick of the first channel to run out **2025** (= 1932 +
93, the one-track unlink was 1596 + 93 = 1689), the next frame with one
dead track and the other's unlink **2005** (= 1932 - 20 + 93: the dead
track costs 20 less, as the one-track 1576), **1892** with both dead (1932
- 40), the EOT on two empty chains **2360** (two `ply_endtie` -> `_081DD938`
54 and two `ply_fine` -> `_done`; the one-track empty-chain EOT was 1810),
1192 on the frame after, 1007 from then on.  Per entry: cry_bulbasaur_encounter
unlinks on frames **47** (track 1, TUNE 76, 11.14 kHz) and **48** (track
0, TUNE 56, 11.0 kHz), the EOT on 143; cry_jynx_echo_end unlinks on **126**
(track 1, TUNE 97) and **133** (track 0, TUNE 33), the EOT on 143;
cry_pidgey_echo_start_chorus unlinks on **13** (track 1, TUNE 120) and
**14** (track 0, TUNE 56), the EOT on 28.  **No `_081DCFB0` stamp in any of
the three** (every channel ends through the count path; no release tail
ever runs), so there are no death stamps to compare: the deaths are the
unlink frames, and the engine's channels lose CHN_ON on the frame before
each unlink (46 / 47 on (a), `test_sequencer_cycles_cry_encounter_chorus`).

**No new timing constant.**  The engine's existing per-track and
per-command model (the RAM command costs of step 5, `ply_note` on the cry,
the per-track tick with its unlink and dead-track terms, the EOT path)
composes to the two-track skeleton without any change: `entry_diff.py` 0
divergences on all 177 complete frames of each of the three entries,
`cmd_diff.py` 0, `track_diff.py` 0, before any engine edit.  Nothing was
derived or fitted.  **`_SKIP` 18 stays est.**: the two tracks each own a
one-channel chain (`track->chan` per track), so `ply_endtie`'s walk still
never passes over a channel; no run in this slice puts two channels on
one track's chain either.

Scheduler-lateness floor (`.events`, `L timer0` lines): cry_bulbasaur_encounter
5 timer-0 slot crossings (cycles 2872577, 3428103, 9046018, 10737665,
11293191 = 0.1712, 0.2043, 0.5392, 0.6400, 0.6731 s), cry_jynx_echo_end 10
(1180928, 4551683, 7922432, 7934976, 15787520, 15800064, 18034689,
20281856, 21405441, 31517699 = 0.0704, 0.2713, 0.4722, 0.4730, 0.9410,
0.9418, 1.0750, 1.2089, 1.2759, 1.8786 s; two pairs 12544 cycles apart),
cry_pidgey_echo_start_chorus 3 (1748994, 2872578, 3428096 = 0.1042,
0.1712, 0.2043 s).

### SoundMain timing probe, step 10: the two-channel release, CRY_MODE_HIGH_PITCH (2026-10-25, milestone 4 slice 7)

The one branch slice 6 left unexercised: `ply_endtie` and `ply_fine` with
a live channel on **each** of the cry player's two chains, i.e. the EOT
releasing two chained channels at once and their two release tails.  No
ROM, CLI or runner change: every parameter is patchable since slice 6.

**The mode** (src/sound.c `PlayCryInternal`, whole): CRY_MODE_HIGH_PITCH
= length 50, release 200, pitch 15800, chorus 20, volume 90 (no reverse).
On two samples that are still sounding at tick 50: Cry_Jynx (gCryTable 123,
23524 samples) and Cry_Wailord (381, 20846 samples).

**The template bytes**, worked by hand before the first render
(`SetPokemonCryPitch`: b = 15800 + 0x80 = 0x3E38, TIE key = b >> 8 = 0x3E
(62), TUNE = (b >> 1) & 0x7F = 0x1F1C & 0x7F = 0x1C (28), the pitch
setter's TUNE2 (16 + 28) & 0x7F = 0x2C -- cry_bulbasaur_howl's bytes --
then `SetPokemonCryChorus(20)` sets trackCount 2 and overwrites TUNE2 with
(20 + 28) & 0x7F = 0x30 (48); VOL 90 = 0x5A; xWAIT 50 = 0x32 0x00; xRELE
200 = 0xC8; PAN 0x40):

| entry | id | trackCount (0x00) | TIE key (0x2A) | TUNE (0x12) | TUNE2 (0x19) | VOL (0x1D) | xWAIT (0x2E,0x2F) | xRELE (0x26) |
|---|---|---|---|---|---|---|---|---|
| cry_jynx_high_pitch | 123 | 2 | 0x3E (62) | 0x1C (28) | 0x30 (48) | 0x5A (90) | 0x32 0x00 (50) | 0xC8 (200) |
| cry_wailord_high_pitch | 381 | 2 | 0x3E (62) | 0x1C (28) | 0x30 (48) | 0x5A (90) | 0x32 0x00 (50) | 0xC8 (200) |

**Length against pitch** (as slices 4-6): key 62 with TUNE 28 is 2 - 36/64
semitones above the 10512 Hz base, 11422 Hz; TUNE2 48 is 2 - 16/64, 11630
Hz.  Cry_Jynx's 23524 samples run 2.06 / 2.02 s (about 123 / 121 frames
from the start tick on frame 3, i.e. the count path would end them around
frames 126 / 124); Cry_Wailord's 20846 run 1.83 / 1.79 s (about 109 / 107
frames, ending around 112 / 110).  The EOT at tick 50 is on frame **53**,
0.84 s after the start tick, so **both channels of both entries are alive
on the EOT frame** and the branch is exercised on each: two `ply_endtie`
walks each finding its chain's one channel holding the key, two `ply_fine`
unlinks of a live channel.  Release 200 from 255 (attack 255, sustain 255)
gives 199, 155, 121, 94, 73, 57, 44, 34, 26, 20, 15, 11, 8, 6, 4, 3, 2, 1
on the 18 passes of frames 53..70 and the stop (product 0) on frame **71**
= EOT + 18 for both channels, unless the two chains differ -- the sample
data outlives the tail on both (the release ends at 1.14 s of a 1.8-2.1 s
sample), so neither channel can end through the count path first.

**Expected from the engine's model before the probe** (the existing
constants composed, not a prediction fitted to anything): the EOT frame
with one live chained channel measured 1928 against the empty chain's
1810 (step 7: `ply_endtie` -> `_081DD938` 82 instead of 54, `ply_fine`
-> `_loop` 10 -> `_ok` 14 -> `RealClearChain` 12 -> `_done` 50 instead
of `_done` 16); the two-empty-chain EOT measured 2360 (step 9).  If the
per-track terms simply add, two live chains give 2360 + 2 * (1928 - 1810)
= **2596**.  The probe decides; measured values are recorded below before
they are used.

**The probe runs** (type 1, 180 frames, `gPokemonCryMusicPlayers`, the
six parameters 15800 0 90 50 200 20 as the ninth to fourteenth arguments;
cycles from VBlank line 160; `build/probe/cry_jynx_high_pitch.txt` and
`cry_wailord_high_pitch.txt`).  The two `.frames` files are identical in
every sequencer column: the start tick **6792** on frame 3 (`ply_note` 2,
the setup commands of both tracks as step 9), **1932** on frames 4..52
with both channels alive (no unlink tick ever: neither sample runs out),
the EOT on frame **53**: **2596** -- `ply_endtie` 2, `ply_fine` 2,
`ply_fine_loop` 2, `ply_fine_ok` 2, `ply_fine_done` 2 -- then 1192 on 54
and 1007 from 55 (both tracks gone).  The raw walk on frame 53 (raw f54),
per track and identical for the two: `ply_endtie` -> `_081DD938` **82**
(the chain's one channel holding the key 0x3E -- `_081DD938`'s r3 = 0x3E
on both tracks, r5 = 0x03002258 then 0x030022A8), `ply_fine` -> `_loop`
10 -> `_ok` 14 (flg F0) -> `RealClearChain` 12 (r0 = 0x03000FC0 for
track 0, 0x03001000 for track 1; prev 0, next 0) -> `_done` 50 -> `_081DD994`
28: the same per-track costs step 7 measured with one live chain (1928
against the empty chain's 1810), so the two-chain EOT is **2360 + 2 * 118
= 2596**, the sum written above before the probe.  `cmd_diff.py` and
`track_diff.py` 0 divergences on both entries, `entry_diff.py` 0 on every
complete frame (its one listed line is the probe's unfinished frame 179,
mgba -999999, as on every 180-frame cry probe), all **before any engine
edit and with no new constant** -- nothing to derive.

**`_SKIP` 18 stays est.**  This run does **not** produce a `ply_endtie`
pass-over: each track's chain holds exactly one channel (`RealClearChain`'s
prev 0 / next 0 on both), and each walk ends at its first channel with the
match.  Two channels on one track's chain would need a second cry while
one plays, a later slice.

**The death stamps** (`_081DCFB0` at its IWRAM copy 0x03000878): two per
entry, both on raw **f72 = frame 71**, 69 cycles apart in the same
SoundMainRAM pass (cycles 19986289 and 19986358 on both entries; r4 =
0x03000FC0 then 0x03001000, the two channels in order), i.e. the EOT
frame + 18 for both channels of both entries, as the hand sequence above
says: the 18 mixed release passes on frames 53..70 and the stop on 71.
The chains do not differ.

Scheduler-lateness floor (`.events` `L timer0` lines; 8 crossings on
each entry, at the same cycles since the two share every frame's work):
1180928, 2317059, 6798848, 7922434, 10182147, 15232005, 18034688,
19158272 = 0.0704, 0.1381, 0.4052, 0.4722, 0.6069, 0.9079, 1.0750,
1.1419 s.

### SoundMain timing probe, step 11: a second cry while one plays (2026-10-25, milestone 4 slice 8a)

Ground truth only: the ROM, the two patchers and the probe runs.  No engine,
`poryaaaa_render`, runner or `songs.json` change (the engine runs one
sequencer per render; two concurrent players are slice 8b, which starts
from the numbers below).

**The ROM change.**  `struct AccParams` gains `u16 id2` (block offset 20)
and `u8 startFrame2` (offset 22) from its reserved bytes (5 remain, 23..27;
the block stays 28 bytes, every existing field at its offset, both new
bytes 0 in the ROM data so a block patched by an older renderer plays as
before).  `startFrame2` is the VBlank count on which `StartCry(id2, 0, 120,
15360, 140, 0, 0)` runs -- `PlayCryInternal`'s CRY_MODE_NORMAL defaults
through the same `StartCry` / `SetPokemonCry*` / `SetPokemonCryTone` path as
the first cry; no per-cry mode fields.  `mgba_render --id2 / --start-frame2`
and `mgba_probe`'s fifteenth / sixteenth arguments patch block bytes 12..14;
`mgba_probe`'s eighth argument accepts `symbol[N]` (N * 0x40, one
MusicPlayerInfo, past the symbol) so the `.frames` summary can read
`gPokemonCryMusicPlayers[1]`.

**Where the second start lives, and why** (found by measurement, kept as a
rule): the first version put a second per-frame check in `AgbMain`'s loop
(`started2`, `StartCry` called from the loop).  agbcc then emitted two more
prologue instructions (`sub sp, #12` for `StartCry`'s stack arguments and a
register init), so every boot-time register write -- `m4aSoundInit`'s, the
timer-0 start included -- landed **6 cycles later** (probe `W` lines 213
against 207, 15438 against 15432), and with the timer's phase against the
65536 Hz DAC slot grid moved, mgba's ordering of a timer-0 overflow against
the slot it coincides with flipped on the coincidences: **every**
DirectSound reference changed (pcm: 555 one-sample events in 3 s at the
128-PCM-sample beat of the 1254-cycle timer period against the 256-cycle
slot, the PCM step landing one slot later; every -inf PCM row fell to -40 dB;
PSG rows unchanged).  Not scheduler lateness: the probe's `.events` on the
same ROM reads 4 crossings, and the `.frames` of cry_jynx_faint_full are
identical, since those are VBlank-relative.  Bisection: the +0x2c symbol
shift alone (a padded crt0, the old loop) gives references byte-identical
to the baseline; the loop change alone moves them.  Two further attempts
(a state variable with a non-inlined `StartTest(state, frame)`; the same
with the block pointer in a register) each changed the prologue by one
instruction (`push {r4, r5, lr}` instead of `{r4, r5, r6, lr}`; the pointer
load hoisted before `RegisterRamReset`) and still moved pcm.  The version
kept: `AgbMain` is **textually identical** to the slice-7 ROM's (agbcc emits
the same code, the one difference the `bl StartTest` offset), and
`StartTest`, after starting the test, waits for `startFrame2` itself
(`VBlankIntrWait` per frame from `startFrame`) and starts the second cry
before returning to the loop.  Boot path and steady-state loop are the old
instruction stream; the only extra execution is inside `StartTest` on the
start frame (silence: a late FIFO sample of silence is silence) and, when a
second cry is requested, the per-frame wait until it starts.  Rule for the
next ROM change: the boot path up to `m4aSoundInit` and the per-frame loop
must stay instruction-identical; check pcm / pcm_cmp / cry_bulbasaur
renders against the baseline references byte for byte before the harness.

**The symbol shift.**  `StartTest` grew by 0x40 (0x78 -> 0xb8 bytes, `nm -S`), so
every ROM symbol from `AgbMain` on moved **+0x40** (nm tables before /
after: 14530 names, the same set; 16 ROM symbols before `AgbMain` at 0x0,
9461 at +0x40, 41 IWRAM + 8 EWRAM / 233770 other at 0; `AgbMain` 0x8000300 ->
0x8000340, `CgbSound` 0x800103c -> **0x800107c**, `MPlayMain` 0x8002600 ->
0x8002640, `ply_note` 0x80028dc -> 0x800291c, `_081DCFB0` 0x8001d8c ->
0x8001dcc, `gCryTable` 0x0802afc8 -> 0x0802b008; `StartCry` 0x80001e8 and
`StartTest` 0x8000288 unchanged; IWRAM unchanged: `gPokemonCrySongs`
0x03001f20, `gPokemonCryMusicPlayers` 0x03001f88, `gPokemonCryTracks`
0x03002258, `gSoundInfo` 0x03000f70, `SoundMainRAM_Buffer` 0x03000770).  The
compiled `CgbSound` compared address-normalised over its 586 objdump lines
(the body, `m4aMPlayTempoControl` and the start of `m4aMPlayVolumeControl`
follow in the range): every offset and every instruction byte equal but the
one literal-pool ROM pointer (0x08002f28 -> 0x08002f68, +0x40), so the
`CgbSound_*` labels are step 9's +0x40 (loop 0x80010ac, active 0x80010c2,
step2 0x800113c, start 0x8001154, nostart 0x8001232, oscoff 0x8001256,
step 0x80012a4, modvol 0x80012b8, stepdone 0x8001398, envdone 0x80013aa,
pit 0x80013b4, vol 0x800142c, done 0x80014a8, next 0x80014ac), each checked
to land on the same instruction as before.

**Gates.**  The cry_jynx_faint_full probe (type 1, 123, 14440 0 120 140 200
0) re-run under the new ROM gives a `.frames` file identical to the old
ROM's (its `.events` differ from the 2026-10-24 log of the slice-4 ROM, which
is not a gate; the references decide).  **Harness neutrality**: the full
default harness with the new ROM and the unchanged engine gives every one
of the 62 rows identical to `2026-10-25_cry-two-chain-release.json` in
every field of report.json but the ref path (53/62, the same nine FAILs,
`compare_reports.py` 0 regressions, every row 0.0); the references'
directory moved to `build/accuracy/refs/56b288f4b49f-mGBA_0.10.5+m4a_blip/`.
pcm, pcm_cmp, cry_bulbasaur and sq1 rendered on the new ROM are
byte-identical to the baseline's reference WAVs (`cmp`).

**Before the first probe: which player the second cry takes, and why**
(`~/pokeemerald/src/m4a.c` `SetPokemonCryTone`, lines 1660-1699).  The
function loops i = 0, 1 over `gPokemonCryTracks[i * 2]` (each cry player's
track 0): a player is **free** when that track's `flags` are 0 and either
its `chan` is 0 or `chan->track != track` (a channel it no longer owns);
the first free player is taken (`goto start_song`).  If neither is free,
the player with the **largest `clock`** wins (`maxClock <
gPokemonCryMusicPlayers[i].clock`, strict, so a tie takes index 0): the cry
that has been playing longest is replaced.  Then `ident++`, the template
`gPokemonCrySong` is copied to `gPokemonCrySongs[i]` (0x03001f20 + 0x34 * i:
52 bytes each), its `tone`, `part[0..1]` and `gotoTarget` are pointed into
that copy, `ident = ID_NUMBER`, `MPlayStart(&gPokemonCryMusicPlayers[i],
...)`.  **The priority test**: `MPlayStart` (m4a.c 611) starts when
`!unk_B || ((no songHeader || !START on track 0) && (no TRACK status ||
PAUSE)) || mplayInfo->priority <= songHeader->priority`; the cry players'
`unk_B` is 0 (`MPlayOpen`'s `Clear64byte`, never set -- only the
`gMPlayTable` players get `unk_A`), so the first disjunct holds and a cry
always restarts the player SetPokemonCryTone chose; the priority byte
(`SetPokemonCryPriority(10)`, CRY_PRIORITY_NORMAL, into the song header's
`priority`) reaches `mplayInfo->priority` and the tracks, and matters only
in `ply_note`'s channel steal when no channel is free (m4a_1.s 1690-1740:
the lowest-priority ON channel, ties to the one with the higher `track`
pointer).  **Expectation for both runs**: on the second start frame cry
player 0's track 0 has MPT_FLG_EXIST (the first cry is playing), so player
0 is not free; player 1's track 0 has flags 0 and `chan` 0 (m4aSoundInit's
`track->chan = 0`, nothing ever played on it), so the second cry lands on
**cry player 1** (`gPokemonCryMusicPlayers[1]` 0x03001fc8, tracks
0x030022f8 / 0x03002348, song copy 0x03001f54), whose MPlayMain entry is
the **first** of the six in SoundMain's order (step 4: 291; the body runs
after cry 0's `_081DDA6C`, step 5's est. 889 / 977 / 1031 / exit 55).
`ply_note`'s channel scan (m4a_1.s `_081DDBFA`: the first channel without
SF_ON in index order) finds channel 0 (0x03000fc0) held by the first cry
and takes **channel 1 (0x03001000)**.  Each cry has its own track (one
track per cry song at chorus 0), so **no track's chain ever holds two
channels** in these runs and `_SKIP` 18 stays est.  The second cry's own
frame costs: its start tick on its frame, the steady tick, the whole-frame
CgbSound entry with both playing, the death / unlink frames and any
`_081DCFB0` stamps are read from the probe below, not predicted.

**The probe runs** (type 1, the new ROM's `syms.nm`; each run twice, with
`gPokemonCryMusicPlayers` and with `gPokemonCryMusicPlayers[1]` as the
eighth argument -- the two raw logs are identical in every cycle, only the
MPlayMain-entry annotation of the selected player differs, so the `_p1`
`.frames` files carry cry player 1's track flags and tempoC;
`build/probe/cry_second_bulbasaur_pidgey.txt` / `_p1` and
`cry_second_jynx_wailord.txt` / `_p1`; the `cry_` prefix without a songs.json
entry makes `claim_check.sh` skip them).  (a) Cry_Bulbasaur (gCryTable 0) at
the defaults from VBlank 1 and Cry_Pidgey (15) at the defaults from VBlank
**20**, 180 frames (`... 15360 0 120 140 0 0 15 20`); (b) Cry_Jynx (123) then
Cry_Wailord (381) from VBlank **60**, 240 frames (`... 381 60`).  Cycles
from VBlank line 160.  **Frame convention**: `startFrame2` N gives the
second cry's start tick on engine frame **N + 2** (22 / 62), as `startFrame`
1 gives the first's on frame 3.

**Which player**: cry player **1**, as stated above.  r7 at `_081DD9BC` is
0x03001FC8 on 283 frames of each run (0x03001F88 on the other 283); the
second cry's `ply_note` runs with r5 = 0x030022F8 (cry 1's track 0) and
takes **channel 1**, r4 = 0x03001000 at `_081DDC40` / `_081DDC54` (frame 22,
cycle 6224222 in (a); frame 62, 17460062 in (b)); the first cry's channel is
0x03000FC0 on track 0x03002258 (frame 3, cycle 886476, both runs).

**Entry positions with both players live** (the same on every such frame of
both runs, by state): the six MPlayMain entries at 291 / 346 / 401 / 456 /
511 / 566 every frame, as always; the paused BGM / SE1 / SE2 / SE3 at
`_081DDA6C` 637 / 700 / 763 / 826; cry 0 playing: FadeOutBody **914**,
`_081DD9BC` **968**, its `_081DDA6C` at 1478 on a steady tick (body 510);
cry 1 playing resumes **88** after cry 0's `_081DDA6C` (FadeOutBody 1566),
`_081DD9BC` **54** later (1620), its body **510** on a steady tick (the same
as cry 0's), its own `_081DDA6C` 2130 and CgbSound **55** after it (2185).
Cry 1 alone (cry 0 paused, (a) frames 145..163, (b) 145..203): cry 0's paused
`_081DDA6C` at **889** (= 826 + 63), FadeOutBody **977**, `_081DD9BC`
**1031**, exit **55** -- step 5's est. row to the cycle.  So a playing cry
player costs the frame 79 + its body whichever of the two it is (the 142
head against the paused 63, the 55 tail), and the frame is **1007 + the sum
over playing players of (79 + body)**: 1596 with one, 2185 with two.

| player | state | body starts at | FadeOutBody | `_081DD9BC` | `_081DDA6C` -> CgbSound |
|---|---|---|---|---|---|
| cry 0 | playing | 826 | 914 | 968 | 118 (cry 1 paused) or cry 1's 142 + body + 55 |
| cry 1 | playing, cry 0 paused | 889 | 977 | 1031 | 55 |
| cry 1 | playing, cry 0 playing | cry 0's `_081DDA6C` (1478 steady) | +88 | +142 | 55 |

**The frame skeleton** (CgbSound entry).  (a): 3 **4077** (the first start
tick, as alone), 4..21 **1596**, 22 **4736** (the second cry's start tick =
1596 + 79 + 3061), 23..32 **2185** (both alive), 33 **2278** (Pidgey's
unlink tick on cry 1: + 93, `RealClearChain` under 0x03001FC8 at 1727),
34..49 **2165** (cry 1's dead track - 20), 50 **2258** (Bulbasaur's unlink on
cry 0, + 93, `RealClearChain` at 1075 -- the same frame as alone), 51..142
**2145**, 143 **2379** (cry 0's EOT on an empty chain: `ply_endtie` ->
`_081DD938` 54, `ply_fine` -> `_done` 16 -> `_081DD994` 28; + 234 as the
one-cry 1810 = 1576 + 234), 144 **1761** (cry 0's finished tick 185 on cry
1's 1576), 145..161 **1576** (cry 1 alone, dead track), 162 **1810** (cry 1's
EOT, 22 + 140), 163 **1192**, 164 on **1007**.  (b): 3 4077, 4..61 1596, 62
**4736**, 63..136 **2185**, 137 **2278** (Jynx's unlink on cry 0, as alone),
138..142 **2165**, 143 **2399** (cry 0's EOT with cry 1's channel still alive:
2165 + 234), 144 **1781** (= 1596 + 185), 145..180 **1596**, 181 **1689**
(Wailord's unlink on cry 1: 62 + 119, the 59 frames after its start tick that
it takes alone, 3 -> 122), 182..201 **1576**, 202 **1810** (cry 1's EOT, 62 +
140), 203 1192, 204 on 1007.  The second player's steady tick is cry 0's
to the cycle on every label (`_081DD9BC` -> `_081DD892` 48 -> `_081DD8B4` 20
-> `_081DD8BA` 6 -> `_081DD938` 14, xcmd 54 + xwait 108, the wait path,
`_081DD9A4` -> `_081DD9BC` 22, -> `_081DDA6C` 76); its EOT walk on 162 /
202 is the one-track empty-chain walk (54, 16, 28).

**The second start tick**: body **3061** against the first cry's 2991 (frame
3 of cry_bulbasaur), **+70**, all of it in `ply_note`'s channel scan passing
over channel 0: `_081DDBFA` -> `_081DDC14` **20** (ON, no STOP) ->
`_081DDC18` **4** (no stopping candidate) -> `_081DDC24` **12** (equal
priority, r0 = 0x0A against the note's 10) -> `_081DDC34` **22** (channel 0's
track 0x03002258 below the note's 0x030022F8: the taken `bls`, the taken
`bcc`, no candidate recorded) -> `_081DDBFA` **12** -> `_081DDC40` **14**
(channel 1 free), then the first cry's sequence from `ClearChain` on, label
for label (the RAM template's command costs of step 5).  The engine's
existing constants compose it exactly: `M4A_CYC_SCAN_ON` 20 + `SCAN_ON_CMP` 4
+ `SCAN_CMP` 12 + `SCAN_EARLIER_TRACK` 22 + `SCAN_NEXT` 12 = 70, then
`SCAN_FREE` 14 -- the pass-over the songs measured; nothing new.

**Deaths and unlinks**: every channel of both runs ends through the count
path -- **no `_081DCFB0` stamp** in either log, and every EOT finds an empty
chain (`_081DD938` r3 = 0x3C after each `ply_endtie`, the track's `chan` 0;
`ply_fine` -> `_done` 16).  (a) Pidgey (cry 1, channel 1) unlinks on frame
**33** = 22 + 11 (alone 3 -> 14), Bulbasaur (cry 0, channel 0) on **50** as
alone; (b) Jynx (cry 0) on **137** as alone, Wailord (cry 1) on **181** = 62 +
119.  Length against pitch at the defaults (key 60, TUNE 0, 10512 Hz):
Jynx's 23524 samples run 2.238 s, about 134 frames from frame 3, and
Wailord's 20846 run 1.983 s, 118.4 frames from frame 62, so both end
before their EOTs (143 / 202); the release is 0 anyway.  The task's
expectation of stamps on (b)'s EOT frames does not hold at the defaults;
nothing is cut.  Deaths are the unlink frames.

**Two channels on one track's chain: never.**  Each cry song is one track
(chorus 0), each track's `chan` holds its one channel (`RealClearChain` prev
0 / next 0 at all four unlinks), and the two live channels sit on different
tracks of different players.  `_SKIP` 18 stays est.

**Scheduler-lateness floor** (`.events` `L timer0`): (a) 3 crossings at
3428099, 4551683, 5675267 = 0.2043, 0.2713, 0.3383 s; (b) 24 at 3428099,
4551683, 5675267, 6798851, 7922435, 9046017, 11293187, 12416771, 13540355,
14663939, 15787523, 16911107, 25899776, 27035906, 28159491, 29283075,
30406659, 31530243, 32653827, 34900995, 36024579, 37148163, 39382790,
41629952 = 0.2043, 0.2713, 0.3383, 0.4052, 0.4722, 0.5392, 0.6731, 0.7401,
0.8071, 0.8740, 0.9410, 1.0080, 1.5437, 1.6115, 1.6784, 1.7454, 1.8124,
1.8793, 1.9463, 2.0803, 2.1472, 2.2142, 2.3474, 2.4813 s.

**Decomposition**: every measured frame of both runs is the sum of the
existing per-player terms (the 1007 idle frame, 79 + body per playing
player in SoundMain's order with cry 1's body after cry 0's) and the
existing per-track and per-command terms inside each body (step 5's RAM
template costs, the unlink + 93 and dead-track - 20, the empty-chain EOT +
234, the finished tick 185, the scan pass-over 70).  Nothing in the two logs
falls outside that model; no constant is derived, fitted or changed here.
What the engine lacks is structural: one sequencer per render (one
frameHook), so it cannot run cry player 1's body after cry player 0's on
the same frame -- slice 8b.


### SoundMain timing probe, step 12: a cry over an SE and over music (2026-10-25, milestone 4 slice 9a)

Ground truth only: no engine, plugin, `cmd/`, `test_engine.c`, `songs.json`,
runner, ROM or baseline change.  Before anything, `claim_check.sh` against
`2026-10-25_two-cry-rows.json` reproduced the baseline (57 diffed, 0 moved,
55/64, 0 regressions, every row 0.0; unit 1471/1471, blip 49112/0, songs
561/561).

**The premise held: no ROM change.**  Read, not assumed: `rom/main.c`'s
`StartTest` runs its `startFrame2` block *after* the type switch, for every
type, so `--type song --id <se or song id> --id2 N --start-frame2 F` starts
`gCryTable[N]` at CRY_MODE_NORMAL's defaults on VBlank F over the playing SE
or song through the same `StartCry` path; `mgba_render.c`'s parser sets
`p.id2` / `p.startFrame2` and `patchParams` writes block bytes 12..14 with no
test of `p.type`, and `mgba_probe.c`'s fifteenth / sixteenth arguments go
through its `patchParams` the same way.  The ROM, `syms.nm` (CgbSound
0x800107c) and every reference are the slice-8a ones.

**The runs** (type 0, 180 frames, `... 15360 0 120 140 0 0 <id2> <F>`;
each twice, the raw logs identical in every cycle, only the `.frames`
summary's player and the six `MPlayMain trackCount` annotation lines of
raw f3 differing): `build/probe/cry_over_se.txt` (se_exp, id 33, SE1, 3 tracks,
Cry_Bulbasaur = gCryTable[0] from VBlank **30**: start tick on frame **32**;
eighth argument `gPokemonCryMusicPlayers`) and `cry_over_se_se.txt`
(`gMPlayInfo_SE1`); `cry_over_bgm.txt` (mus_littleroot, id 405, BGM, 9 of
10 tracks, Cry_Bulbasaur from VBlank **60**: frame **62**) and
`cry_over_bgm_bgm.txt` (`gMPlayInfo_BGM`).  The `cry_` prefix without a
`songs.json` entry makes `claim_check.sh` skip them.  For like-for-like
comparison the two songs were re-probed alone on the current ROM into
`build/scratch/littleroot_alone.txt` / `se_exp_alone.txt` (their `.frames`
cycles identical to the step-3 / step-4 logs `littleroot.txt` /
`se_exp.txt` on every complete frame; the alone logs below are those), and
a 240-frame `build/scratch/cry_over_bgm_240.txt` covers the cry's EOT over
music (frame 202, past the 180-frame run).  `mgba_render --type song --id
33 --id2 0 --start-frame2 30 --frames 182` and `--id 405 ... --start-frame2
60` rendered `build/scratch/cry_over_se.wav` / `cry_over_bgm.wav` (582348
bytes each) for slice 9b; not listened to, not compared.  Cycles from
VBlank line 160; "alone" = the song's own probe without the cry.

**Reading method.**  Per frame and per player the raw log gives the
MPlayMain entry, `FadeOutBody`, `_081DD9BC` and `_081DDA6C` (r7 = the
MusicPlayerInfo), so each playing player's body (`_081DD9BC` ->
`_081DDA6C`) is measured directly; `bodies.py`-style sums check step 11's
rule **CgbSound = 1007 + sum over playing players of (79 + body)** on every
frame, with the paused players' terms (BGM 71, the others 63, the tail 55)
exactly as step 4 gives them.  The BGM's / SE1's body with the cry is
compared with its body alone on the same frame, and where they differ the
two label sequences are diffed segment by segment.

**Run (a), cry over se_exp.**  The SE's note (frame 3, `ply_note` vt = 01:
a square-1 voice) takes a CGB channel through the `_081DDBC4` path, so no
DirectSound channel is ON when the cry starts: its scan is `_081DDBFA` ->
`_081DDC40` **14** (`SCAN_FREE`, channel 0, 0x03000FC0), then the lone
cry's sequence label for label (`ClearChain` 12, `RealClearChain` 33,
`_081DDC54` 48 = `NOTE_UNLINK_NONE` 44 + `CHAIN_EMPTY` 4).  The priority
compare is never reached; SE1's `priority` is 5 (`se_exp_pri`, no PRIO
command), the cry's 10.  No steal.  SE1's body is **identical to its body
alone on every frame** (3..102: alone CGB - 1086, 0 mismatches); the
cry's body takes only step 5 / 11 values: start **2991**, steady **510**,
unlink tick **603** (+93), dead track **490**, EOT **724**, finished tick
**106**.  The whole run composes from step 4's chain rule, step 5's cry
costs and step 11's 1007 + (79 + body) -- no exception:

| frames | state | CgbSound | = alone + | alone |
|---|---|---|---|---|
| 2..31 | SE alone | the alone values | 0 | 1007, 3987, 2117, ... |
| 32 | the cry's start tick | **5672** | 79 + 2991 = 3070 | 2602 |
| 33..78 | both playing | alone + **589** | 79 + 510 | 1412..2950 |
| 79 | Bulbasaur's unlink (32 + 47, as alone 3 -> 50) | **2793** | 79 + 603 = 682 | 2111 |
| 80..100 | SE playing, cry's track dead | alone + **569** | 79 + 490 | |
| 101 / 102 | the SE's FINE / finished tick | **2340** / **1795** | 569 | 1771 / 1226 |
| 103..171 | cry alone, dead track | **1576** | 569 | 1007 |
| 172 / 173 | the cry's EOT (32 + 140) / finished tick | **1810** / **1192** | 803 / 185 | 1007 |
| 174 on | all paused | **1007** | 0 | 1007 |

The cry's death is the count path (no `_081DCFB0` stamp in the log; none
in the SE's either), the unlink `RealClearChain` under 0x03001F88 on frame
79 with prev 0 / next 0; the EOT finds the chain empty (`ply_endtie` ->
`_081DD938` 54, `ply_fine` -> `_done` 16).  The SE's 284 PSG writes are the
alone run's (frame, register, value) sequence exactly; each write's cycle
moves by the frame's delta above.  `.events`: 6 `timer0` slot crossings
(first at cycle 9046022 = 0.539 s; se_exp alone has 0: the crossings come
with the PCM channel's mixer work).

**Run (b), cry over mus_littleroot.**  Frames 2..61 are the alone frames;
the BGM's body on frame 62 is its alone body (2734 = 3820 - 1086: the
`MPlayStart` between SoundMains touched only the cry player).  **The
cross-player steal of slice 8b-i's review is exercised.**  On frame 62 all
five DirectSound channels are ON when the cry's `ply_note` scans
(`_081DDBFA` r3 = 5 = `maxChans`, loaded after the `_081DDBEC` label): channel 0 (0x03000FC0) ON | STOP on BGM
track 1 (0x03000090), channel 1 (0x03001000) ON | STOP on the same track,
channels 2 / 3 (0x03001040 / 0x03001080) ON, channel 4 (0x030010C0) ON
with the BGM's note of this very frame.  The walk, label for label against
m4a_1.s: channel 0 `_081DDBFA` -> `_081DDC32` 32 -> `_081DDC34` 2
(`SCAN_FIRST_STOP` 34: ON | STOP with r2 = 0, `adds r2, 1`, r6 := its
priority 0, r7 := its track -- the note's own priority 10 in r6 is
**overwritten**, a stopping candidate wins regardless of priority) ->
`_081DDBFA` 12; channel 1 -> `_081DDC18` 24 (`SCAN_STOP`) -> `_081DDC24` 12
(`SCAN_CMP`: priority 0 against r6 0, the taken `bcs`) -> `_081DDC32` 16 ->
`_081DDC34` 2 (`SCAN_SAME_TRACK` 18: `ldr r0, track` = 0x03000090 = r7, the
taken `bls`, the not-taken `bcc`, so the later channel of the same track
is recorded) -> 12; channels 2, 3, 4 -> `_081DDC14` 20 -> `_081DDC34` 10
(`SCAN_ON` + `SCAN_ON_SKIP`: a stopping candidate exists) -> 12 each (the
last `_081DDC34` -> `_081DDC40` 12, `SCAN_DONE`); the scan is **238**
against the lone cry's 14.  Then `ClearChain` 12, `RealClearChain` 33 and
`RealClearChain` -> `_081DDC54` **70** on the stolen channel 0x03001000
(trk = 0x03000090, prev = 0x03001080, next = 0x03000FC0, the cry's own
chain empty) = `NOTE_UNLINK_11` 66 + `NOTE_CHAIN_EMPTY` 4 -- the GBA
unlinks the channel from BGM track 1's chain at once, through
`channel->track`, inside the cry's `ply_note`.  The start body is **3237** =
2991 + 224 (scan) + 22 (unlink), every term an existing constant; the frame
7136 = 3820 + 79 + 3237.  The stolen note: BGM track 1 took channel 1 on
frame 53 (`_081DDC40` r4 = 0x03001000, raw f54), its gate expired on frame
58 (`_081DD8B4` r0 = 0x40), and alone its envelope reached zero on frame 68
(`_081DCFB0` stamp raw f69, r3 = 0x0808C970) with the walk dropping it on
69 (`RealClearChain` under 0x03002008, prev 0x03001080, next 0, 127 cycles:
`_081DD8AE` 14 + `ClearChain` 12 + 33 + `WALK_UNLINK_10` 56 + `WALK_NEXT`
12); with the cry it is cut on frame 62, four frames after its STOP, and
neither the stamp nor the drop occurs (the stamp list with the cry, 12 against alone's 15, lacks alone's f69,
f101, f113 and f135 and has an f127 alone lacks: the later reallocation of
channels, several shared stamps falling on other channels).
The cry's channel priority 10 (`mplayInfo->priority` 10 + track priority 0)
against the BGM's 0 (`mus_littleroot_pri` 0, no PRIO command) is compared
only later, by BGM notes scanning past the cry's live channel 1 without a
stopping candidate: `_081DDC18` -> `_081DDC24` 12 (0xA >= 0) -> `_081DDC34`
**8** (the taken `bhi`): six times (frames 87, 95, 103, twice each, r0 =
0x0A, r4 = 0x03001000) -- `M4A_CYC_SCAN_HIGHER`'s *est.* 8 **measured for
the first time** (no other log in `build/probe/` has the segment), equal to
the estimate.  No BGM note was dropped: 52 `ply_note` entries in frames
<= 178 in both logs, 52 took a channel.  Every frame obeys the chain rule
(1007 + (79 + body) per playing player, both players' bodies measured; 0
exceptions on 2..178 and on the 240-frame run's 2..238), and the cry's body
takes only its known values: 3237 (frame 62), 510 (63..108), 603 (109, the
unlink, 62 + 47), 490 (110..201), 724 (202, EOT, 62 + 140), 106 (203):

| frames | state | CgbSound | alone | delta = 79 + cry body + BGM delta |
|---|---|---|---|---|
| 2..61 | BGM alone | the alone values | | 0 |
| 62 | the cry's start tick (steal) | **7136** | 3820 | 79 + 3237, BGM 0 |
| 63..108 | both playing | alone + 589 + BGM delta | | BGM delta 0 on no-tick frames; **-32** on tick frames 63, 65..67, 88, 90..92, 94, 97..99, 105, 106, 108 (one channel fewer in track 1's walk: `WALK_GATE0` 20 + `WALK_NEXT` 12); -127 on 69 and 101 (the drops that no longer happen, five segments each); +126 on 78, +84 on 87, -28 on 95 (= -32 + 4), +172 on 103 (BGM notes' scans and unlinks over a different channel landscape); 0 on 70..77, 79..86 |
| 109 | Bulbasaur's unlink (cry player 0's walk, `RealClearChain` raw f110) | **3196** | 2546 | 79 + 603 - 32 |
| 110..178 | BGM playing, cry's track dead | alone + 569 + BGM delta | | -32 on 110, 112 (-274 total: a free take instead of a steal), 113 -95, +32 on 115, 116, 117, 119 (a channel more in a walk), 120 +10, 127 +95, 128 -276, 135 -95, 137 +324, 145 -30, 153 -78, 162 +88, 170 +42, 178 -56; 0 elsewhere |
| 202 / 203 (240-frame run) | the cry's EOT / finished tick | **3818** / **4693** | 3015 / 4438 | 79 + 724 / 79 + 106 + 70 |
| 204..238 | BGM alone again | alone on 31 frames; +4 / +58 / +70 / +80 on four note frames | | 0 |

Every BGM-body difference is **state, not cost**: the segment multiset diff
(with minus alone) of every differing frame on 2..178 contains only
`_081DD892`+12 / `_081DD8B4`+20 (`WALK_NEXT` / `WALK_GATE0`), `_081DD8AE`+14
/ `ClearChain`+12 / `RealClearChain`+33 / `_081DD8B4`+56 (the walk's dead
drop, `WALK_UNLINK_10`), and the scan / take segments `_081DDBFA`+12,
`_081DDC14`+20, `_081DDC34`+10, `_081DDC18`+4 / +24, `_081DDC24`+12,
`_081DDC34`+8 / +22, `_081DDC32`+32 / +18 / +16 with `_081DDC34`+2,
`_081DDC40`+12 / +14 and `_081DDC54`+44 / +48 / +64 / +72 / +74 / +70 --
each the engine's `SCAN_*` / `NOTE_UNLINK_*` (+ `CHAIN_EMPTY`) constant for
that state, and every (segment, state) -> value pair but the two named
above (70 on the cross-player steal, 8 on `SCAN_HIGHER`) occurs in the
alone song log or the lone cry log with the same value.  The value 70
itself is not new: alone frame 58's same-player steal (prev set / next 0,
`NOTE_UNLINK_10` 70) reads 70 too, so the steal's 66 + 4 and the 70 + 0
reading are not told apart by this measurement; only the state (both
links set, the taking chain empty) is new.  **No new
constant is derived, fitted or changed.**  The BGM's 156 PSG writes are the
alone run's (frame, register, value) sequence exactly (the stolen channel
is PCM); `.events`: 20 crossings (first 3996161 = 0.238 s; alone 14; the
240-frame run 21).  The cry's death is the count path (its channel's
`_081DCFB0` never appears; the 12 stamps in the log are BGM channels'), the
EOT (frame 202) finds its chain empty.

**Deaths and unlinks summary**: (a) cry on frame 79 (count path), SE FINE
101; (b) the stolen BGM note cut on 62 (no death of its own), the cry on
109 (count path), EOT 202.  The `.frames` of the song players (`_se` /
`_bgm` logs) show the cry only through the CgbSound entry, the shifted
write cycles and the `note` / `ply` command counts (which count every
player's handlers): the stolen note is audible in the PCM mix alone (the
`build/scratch` renders, for 9b).

**What slice 9b needs -- structural statement only, nothing implemented.**
(Historical since slice 9b-i, 2026-10-25: its "today" sentences about
`init_chained`, `set_player` and `chainOwner[]` describe the code before
that slice; see "Done: milestone 4 slice 9b-i".  Item (4)'s "today"
sentences about the runner's loader and `poryaaaa_render`'s `--cry2`
requiring `--cry` are historical since 9b-i / 9b-ii likewise.)
(1) `MAX_TRACKS` 16 in `plugin/m4a_engine.h` holds both chains: BGM (10) ->
cry 0 (2) with trackBase 10 (engine tracks 10..11), SE1 (3) -> cry 0 with
trackBase 3 (tracks 3..4); a BGM -> SE1 -> cry 0 chain is 10 + 3 + 2 = 15
tracks, but today's `init_chained` would first select SE2 (9 tracks) for
the third member at trackBase 13 and refuse it (13 + 9 > 16), so that
chain needs the player passed into `init_chained` or selected before the
range check.
(2) `m4a_sequencer_init_chained(outer, inner, trackBase)` today refuses an
inner with an outer, a trackBase below `inner->trackBase + inner->maxTracks`
or at `MAX_TRACKS`, and an inner that is cry player 1; it then calls
`m4a_sequencer_set_player(outer, inner->player + 1)` -- the **next** player
-- so a BGM -> cry 0 chain (players 0 -> 4) or SE1 -> cry 0 (1 -> 4) needs
the caller to re-select cry 0 with `set_player` afterwards, which its
checks allow (`player <= inner->player` refused, any higher player and a
fitting track range accepted).  The chain's frame code already charges the
paused players between two members, `(o->player - last->player - 1) *
M4A_CYC_MP_IDLE` (SE1 / SE2 / SE3's 63 each for BGM -> cry 0, SE2 / SE3's
for SE1 -> cry 0), as step 11's 1007 + (79 + body) requires and run (a) /
(b) confirm (SE1 -> cry 0 on frame 32: 637 + 142 + body + 63 + 63 + 142 +
body + 63 + 55).  (3) **The cross-player steal's cycle model, now measured**: the taking
player's `ply_note` must find the stolen channel's current owner across the
whole chain (the GBA's `channel->track`; today each sequencer's
`chainOwner[]` knows only its own tracks, so `m4a_sequencer.c`'s note path
would charge `NOTE_UNLINK_NONE` 44 instead of the measured `NOTE_UNLINK_11`
66 -- -22 on frame 62 -- and leave the channel in BGM track 1's
`chainOwner`; the BGM's next tick walk then finds `chan_in_chain` false
(the channel's `trackIndex` is the cry's engine track from the steal) and
takes the dead branch, `WALK_DEAD` 59 + the unlink cost, a drop the GBA
never performs -- one spurious ~115-cycle frame, then -32 as the GBA),
unlink it from that owner's chain at once with the owner's prev / next
(`unlink_cost` run against the owner sequencer), and clear the owner's
`chainOwner[h]`; the owner's walk then sees one channel fewer (the -32).
The reverse direction (a BGM note over the cry's channel) is the existing
`SCAN_HIGHER` path; the engine's allocation (`m4a_engine_note_on`'s shared
scan) already picks channel 1 since it follows the GBA's scan over the
shared channels.  (4) `songs.json`: an `"se"` or `"song"` entry carrying
the same `"id2"` / `"start_frame2"` pair (both or neither) -- today the
runner's loader builds the `cry2` / `cry2_start_frame` keys only in its
`kind == "cry"` branch (an `"id2"` on an SE / song entry is silently
ignored, no warning, no range check), so the song / SE branch needs the
keys with the same both-or-neither and range checks; the `mgba_render`
line then appends `--id2 / --start-frame2` as it already does for any
entry with `cry2`, while the `poryaaaa_render` line does so only inside
the `t.get("cry")` branch and needs the `--cry2 N --cry2-start-frame F`
pair in the song / SE branch too, and `poryaaaa_render` itself today requires
`--cry` with `--cry2` and chains cry 1 after cry 0; a cry over a song needs
cry 0 chained after the song's player (BGM or SE1) with the second cry's
sample as its voicegroup and `m4a_cry_choose_player` from silence (cry 0).
Suggested rows, named after the logs: `cry_over_se` (se_exp, id2 0,
start_frame2 30, 3 s) and `cry_over_bgm` (mus_littleroot, id2 0,
start_frame2 60, 3 s or 4 s to cover the EOT 202).  `claim_check.sh`'s SE /
song argument lines would need the two flags as its cry line has them.

**Code review of slice 9a (2026-10-25).**  Two independent read-only
reviews of the section above (reviewer 1: every number of both frame
tables, the per-player bodies and the chain rule recomputed from the raw
logs on 2..178 / 2..238 of every run, 0 exceptions; the frame-32 / frame-62
walks label for label with registers, the asm reading of the
stopping-candidate rule, the priority bytes, the six `SCAN_HIGHER`
measurements, the stolen note's history, the stamp lists, the write
sequences and their per-frame shifts, the event counts, the WAV sizes, the
240-frame run; reviewer 2: every composition against steps 4, 5, 11 and the
header's constants by name and value, the segment-to-constant map, the
premise against `main.c` / `mgba_render.c` / `mgba_probe.c` and the symbol
file, the structural statement against `m4a_sequencer.c` (init_chained,
set_player, the chain's gap charging, the walk's `chan_in_chain` test,
`allocate_pcm_channel`), `poryaaaa_render.c`, `accuracy.py`'s loader and
command lines, `claim_check.sh`, the README / skill text).  **Every
measured number and every composition confirmed; no constant derived or
changed.**  Corrected in the text (no number in a table cell changed but
the two listed below): the -32 frame list (107 is 0, 118 is 0; frame 101's
-127 added to the row), the stamp diff (alone's f113 and f135 also absent),
"six frames into its release" -> four frames after its STOP, r3 = 5 read
at `_081DDBFA` not `_081DDBEC`, the twin logs identical in every cycle but
six annotation lines, the `.frames` `note` / `ply` counts including the
cry's handlers, the value 70 also occurring alone as `NOTE_UNLINK_10`
(the 66 + 4 reading is by state, not told apart by measurement); in the
structural statement: today's engine would not count the stolen channel
live but drop it as dead on the owner's next walk (`chan_in_chain` tests
the channel's track), the runner's loader never carries `"id2"` on an SE /
song entry today, and a three-member BGM -> SE1 -> cry 0 chain is refused
by `init_chained`'s next-player selection despite its 15 tracks fitting.
**Not acted on, recorded**: the 240-frame run's frames 179..201 are not
tabulated (BGM-state deltas -114 / -95 / +296 / +238 / +127 on 187 / 194 /
195 / 199 / 201, the chain rule exact there); the shared later stamps fall
on other channels than alone (the reallocation).  No code changed in the
slice, so the gate is the reproduction check: `claim_check.sh` against
`2026-10-25_two-cry-rows.json` after the probe logs were added -- 57
diffed, 0 moved, 55/64, 0 regressions, every row 0.0, unit 1471/1471, blip
49112/0, songs 561/561 (the `cry_over_*` logs skipped by the `cry_` rule).

### SoundMain timing probe, step 13: a third cry while both cry players are busy (2026-10-25, milestone 4 slice 10a)

Ground truth only: the ROM's parameter block, the two patchers, the probe
runs and this section.  No engine, plugin, `cmd/`, `test_engine.c`,
`songs.json`, runner, `claim_check.sh`, baseline or new corpus row (slice
8b-i's refusal of a `--cry2` while both cry players are busy is the path
this step measures; the engine change is slice 10b).  Before any change,
`claim_check.sh` against `2026-10-25_cry-over-song-rows.json` reproduced the
baseline (59 diffed, 0 moved, 57/66, 0 regressions, every row 0.0; unit
1521/1521, blip 49112/0, songs 561/561).

**The rule, read from the source before the probe** (a prediction to
test; `~/pokeemerald/src/m4a.c` `SetPokemonCryTone` 1660-1699, `MPlayStart`
611-666, `src/m4a_1.s` `TrackStop` 1468-1506 and `_081DD9A4` 1341-1344;
`src/sound.c` `PlayCryInternal` 369-: `StartCry` is its CRY_MODE_NORMAL
defaults, so the third cry is `StartCry(id3, 0, 120, 15360, 140, 0, 0)`
exactly as the second).  `SetPokemonCryTone` loops i = 0, 1 over
`gPokemonCryTracks[i * 2]`: a player whose track 0 has `flags` 0 and
either `chan` 0 or `chan->track != track` is free and taken at once
(`goto start_song`); otherwise `if (maxClock < gPokemonCryMusicPlayers[i].clock)`
records it (strict, `maxClock` starting at 0: a tie, or two clocks of 0,
takes **player 0**), and after the loop `i = maxClockIndex`.  `clock` is
set to 0 by `MPlayStart` (m4a.c 633) and incremented by one at
`_081DD9A4` on every tick of that player (after its track loop, before the
tempoC subtraction), so it counts the **ticks since the player's own
start**: with both cry players at the template's tempo, the cry that
started earlier has the larger clock and **is replaced**.  Then
`ident++`, `gPokemonCrySongs[i] = gPokemonCrySong` (the 52-byte template
copy, the same IWRAM copy as the replaced cry used), the pointer rewrites,
`ident = ID_NUMBER`, `MPlayStart(&gPokemonCryMusicPlayers[i], ...)`.
`MPlayStart` always starts a cry player (`unk_B` 0, step 11): it zeroes
`status`, `clock`, `tempoC`, `fadeOI`, sets tempoD / tempoI 150 and tempoU
0x100, and for each of the player's two tracks calls **`TrackStop`** then
sets `flags = EXIST | START`, `chan = 0`, `cmdPtr = part[i]` (track 0) or
`flags = 0` (track 1 at chorus 0).  `TrackStop` (m4a_1.s 1468): if the
track has `MPT_FLG_EXIST` and a `chan`, it walks the chain from `chan`
through `nextChannelPointer`: for each channel with `statusFlags != 0`, a
CGB type calls `CgbOscOff`, every type then **stores `statusFlags = 0`**
(`TrackStop_1`), and every channel gets `track = 0` (`TrackStop_2`); then
`track->chan = 0` (`TrackStop_3`).  So **the replaced cry's PCM channel is
cut outright**, between two SoundMains, in ROM code: no release, no
SoundMainRAM envelope pass sees it again (statusFlags 0 is skipped), hence
**no `_081DCFB0` stamp** (that store is SoundMainRAM's, at the IWRAM copy)
and no unlink walk on the replaced player (its `chan` is 0 and its track is
restarted); the `TrackStop` / `TrackStop_Loop` / `TrackStop_1` / `_2` /
`_3` / `_Done` breakpoints in the raw log, under the `MPlayStart` of the
third cry's VBlank, are where the cut shows.  The template's `reverb` is 0,
so `m4aSoundMode` is not called.  **Prediction for the planned run** (Jynx
on cry 0 from VBlank 1, Wailord on cry 1 from VBlank 20, Bulbasaur from
VBlank 60): on VBlank 60 both track 0s have `MPT_FLG_EXIST` (Jynx's 23524
samples run about 134 frames from frame 3, Wailord's 20846 about 118 from
frame 22, both EOTs later still), neither is free; cry 0's `clock` is the
larger (59 ticks, frames 3..61, against cry 1's 40, frames 22..61), so
**cry player 0 is replaced**: Jynx is cut on VBlank 60 (channel 0,
0x03000FC0, statusFlags := 0 by `TrackStop_1`), cry 0's track 0 restarts
with START, and on engine frame **62** cry 0's body is a **start tick**
whose `ply_note` scan finds channel 0 free (statusFlags 0, the first in
index order: `_081DDBFA` -> `_081DDC40` 14, `SCAN_FREE`) -- so Bulbasaur
takes **channel 0**, Jynx's old channel, and the start body is the lone
cry's **2991** (the same RAM template copy, the same free take), the frame
1007 + (79 + 2991) + (79 + 510) = **4666** (cry 1's steady body after it).
Wailord continues untouched on channel 1: its unlink tick on frame 22 + 119
= **141**, its EOT 22 + 140 = **162**; Bulbasaur's unlink on 62 + 47 =
**109** (as alone 3 -> 50), its EOT 62 + 140 = **202** (past a 180-frame
run).  Jynx's unlink on 137 and cry 0's EOT on 143 **do not happen**.  Frame
62's other members: cry 1's body 510 steady.  Nothing of this was measured
when the paragraph was written; the probe below decides.

**The ROM change.**  `struct AccParams` gains `u16 id3` at block offset
**24** (block bytes `b[16..17]` as the patchers count from the magic) and
`u8 startFrame3` at **26** (`b[18]`), after a padding byte at 23 (`id3`'s
alignment; `reserved0`) and before one reserved byte at 27; the block stays
28 bytes, every existing field at its offset, both new fields 0 in
`crt0.s` so a block patched by an older renderer plays as before.
`StartTest` hoists its `frame` initialisation out of the second-cry block
and adds a third block of the same shape after it: wait `VBlankIntrWait`
per frame until `startFrame3`, then `StartCry(id3, 0, 120, 15360, 140, 0,
0)` -- the second cry's defaults, `PlayCryInternal`'s CRY_MODE_NORMAL.
`mgba_render --id3 N --start-frame3 F` and `mgba_probe`'s seventeenth /
eighteenth arguments patch `b[16..18]`; both usage texts name them.  The
compiled `StartTest` loads `startFrame3` from `[gAccParams + 26]` and `id3`
from `[+ 24]` (objdump), and passes 0 / 120 / 15360 / 140 / 0 / 0 to
`StartCry`.  Two corners the ROM has but no run measured: with both start
frames nonzero and `startFrame3 <= startFrame2` the third block's wait
condition is false at once, so the third `StartCry` runs on the second
cry's VBlank immediately after it (not skipped, no wait); with
`startFrame2` 0 and `startFrame3` nonzero the third block alone waits from
`startFrame` and the cry it starts is a second cry (one player free).  Both
patchers truncate a start frame above 255 to its low byte, as for
`startFrame2`.
**Boot-path rule, checked**: `AgbMain`'s disassembly before and after is
identical in every instruction but the relative `bl StartTest` encoding
(`f7ff ff82` -> `f7ff ff6a`: the callee stayed at 0x8000288 while the call
site moved), the slice-8a result again.  `StartTest` grew 0xb8 -> **0xe8**
(`nm -S`, +0x30), so every ROM symbol from `AgbMain` on moved **+0x30**
(nm tables before / after: 14530 names, the same set; 16 ROM symbols before
`AgbMain` at 0 -- `Init`, `IntrMain`, `StartCry`, `StartTest`, `gAccParams`,
`VBlankIntr`, `VCountIntr`, the header labels, the compiler's
`.gcc2_compiled.` --, **9457** at +0x30, no ROM symbol moved by any other
amount, no IWRAM / EWRAM or absolute symbol moved): `AgbMain` 0x8000340 -> 0x8000370, `MPlayStart`
0x8000c68 -> 0x8000c98, `CgbSound` 0x800107c -> **0x80010ac**, `_081DCFB0`
0x8001dcc -> 0x8001dfc, `MPlayMain` 0x8002640 -> 0x8002670, `TrackStop`
0x80028a8 -> 0x80028d8, `ply_note` 0x800291c -> 0x800294c, `gCryTable`
0x802b008 -> 0x802b038; IWRAM unchanged (`gPokemonCrySongs` 0x03001f20,
`gPokemonCryMusicPlayers` 0x03001f88, `gPokemonCryTracks` 0x03002258).
`CgbSound`'s 566 objdump lines compared address-normalised: every
instruction equal, the one literal-pool ROM pointer 0x08002f68 ->
0x08002f98 (+0x30), so the `CgbSound_*` labels are step 11's **+0x30**
(loop 0x80010dc, active 0x80010f2, step2 0x800116c, start 0x8001184,
nostart 0x8001262, oscoff 0x8001286, step 0x80012d4, modvol 0x80012e8,
stepdone 0x80013c8, envdone 0x80013da, pit 0x80013e4, vol 0x800145c, done
0x80014d8, next 0x80014dc), each checked to land on the same instruction
text as before; `build/probe/syms.nm` rebuilt from the new ELF with them
appended.  **The three renders**: pcm, pcm_cmp and cry_bulbasaur (and sq1)
rendered by the new `mgba_render` on the new ROM are **byte-identical**
(`cmp`) to the current references in
`build/accuracy/refs/56b288f4b49f-mGBA_0.10.5+m4a_blip/`; the ROM's sha1
5484b140... -> b3d22c13..., so the harness keys a new refs directory.

**The probe runs** (type 1, Jynx 123, the new `syms.nm`; each twice, with
`gPokemonCryMusicPlayers` and `gPokemonCryMusicPlayers[1]` as the eighth
argument; the two raw logs of each run have **identical cycle columns on
every line** (0 differences with the annotation stripped), only the
`MPlayMain trackCount / exist / start / tempoC` annotation of the selected
player differs, 222 / 240 lines per log): `build/probe/cry_third_jynx_wailord_bulbasaur.txt`
/ `_p1` -- `... 15360 0 120 140 0 0 381 20 0 60`, 180 frames: Wailord
(381) on cry 1 from VBlank **20** (start tick frame 22), Bulbasaur (0) from
VBlank **60** (frame 62) -- and the "alone" twin
`cry_third_alone_jynx_wailord.txt` / `_p1` (`... 381 20`, no third cry);
plus `build/scratch/cry_third_jynx_wailord_bulbasaur_240.txt` (240 frames,
for Bulbasaur's EOT past 180).  The `cry_` prefix without a `songs.json`
entry makes `claim_check.sh` skip them.  Cycles from VBlank line 160; raw
`fN` = engine frame N - 1.

**Which player, measured: cry player 0, as predicted.**  Raw f62 (engine
frame 61, VBlank line 191: 38694 cycles after line 160's start, after that
frame's SoundMain ended at CgbSound 2185 + its RAM pass, before frame 62's)
holds the third cry's `MPlayStart` at cycle 17214347, then `TrackStop`
+107 whose registers name the player: **r0 = 0x03001F88**
(`gPokemonCryMusicPlayers[0]`), the song copy **r7 = 0x03001F20**
(`gPokemonCrySongs[0]`), r4 = 0x03002258 (cry 0's track 0); **`TrackStop_Loop`** +20 with **r4 =
0x03000FC0** (Jynx's channel 0, the chain head), **`TrackStop_1`** +20 (r3
= 7: `type & TONEDATA_TYPE_CGB` 0, the PCM path -- `statusFlags := 0`),
`TrackStop_2` +2 (`track := 0`), `TrackStop_3` +8 (r4 = 0: no
`nextChannelPointer`; `track->chan := 0`), `TrackStop_Done` +2; then
`TrackStop` for track 1 (r4 = 0x030022A8, flags 0) -> `TrackStop_Done` 18.
Jynx is **cut outright** 59 frames into its 134, mid-sample, between two
SoundMains, in ROM code: no release, no envelope pass, **no `_081DCFB0`
stamp** (none in any of the three logs).  Neither cry player was free:
both track 0s had MPT_FLG_EXIST (the `.frames` `trk` columns read 1/0/2 on
frame 61 for both players), and cry 0's clock was the larger.  The
`TrackStop` cycles above (20 / 20 / 2 / 8 / 2; 18 for an empty track) run in
the main loop outside SoundMain and cost the engine nothing.

**Frame 62, the restart tick.**  Cry 0's body is **2991** -- the lone cry's
first start tick to the cycle -- and its 94-line breakpoint sequence from
`_081DD9BC` to its `_081DDA6C` is **identical label for label and delta
for delta** to raw f4's (Jynx's own start on frame 3); the only differences
in the two frames' logs are cry 1's live body following cry 0's instead of
its paused 63, and CgbSound's first-channel segment (53 / 51).  The scan:
`_081DDBFA` (r3 = 5 = maxChans) -> **`_081DDC40` 14** with **r4 =
0x03000FC0, chan = 0**: channel 0, Jynx's old channel, `statusFlags` 0
since the cut, the first in index order -- `SCAN_FREE` 14, no pass-over;
`ClearChain` 12, `RealClearChain` 33, `_081DDC54` **48** (`NOTE_UNLINK_NONE`
44 + `CHAIN_EMPTY` 4: cry 0's own chain was emptied by `TrackStop`).  So
**Bulbasaur takes channel 0** and the frame is **4666** = 1007 + (79 +
2991) + (79 + 510), the prediction's number.  In the `.frames` summary
cry 0 reads `note 1 ply 15 trk 1/1/2` on frame 62 (START seen at
MPlayMain entry), cry 1 (`_p1`) `trk 1/0/2` on 23..162 (1/1/2 on its own
start tick 22).

**Frame skeleton** (CgbSound entry).  Third-cry run: 2 1007, 3 **4077**, 4..21
1596, 22 **4736** (Wailord's start on cry 1: the pass-over 70 as step 11),
23..61 **2185**, 62 **4666**, 63..108 2185, 109 **2278** (Bulbasaur's unlink
on cry 0, 62 + 47 as alone 3 -> 50; `RealClearChain` raw f110 under
0x03001F88, prev 0 / next 0), 110..140 **2165**, 141 **2258** (Wailord's
unlink on cry 1, 22 + 119, the twin's frame; raw f142 under 0x03001FC8),
142..161 **2145**, 162 **2379** (cry 1's EOT, 22 + 140, with cry 0's track
dead: 2145 + 234; `ply_endtie` -> `ply_fine` -> `ply_fine_done` raw f163),
163 **1761** (cry 1's finished tick 185 on 1576), 164..201 **1576** (cry 0
playing, dead track), 202 **1810** (Bulbasaur's EOT, 62 + 140, on an empty
chain; the 240-frame run, raw f203), 203 **1192**, 204 on **1007**.  Twin:
2 1007, 3 4077, 4..21 1596, 22 4736, 23..136 2185, 137 **2278** (Jynx's
unlink on cry 0, as alone; raw f138), 138..140 2165, 141 2258 (Wailord's),
142 2145, 143 **2379** (cry 0's EOT with cry 1's track dead; raw f144),
144 1761, 145..161 1576, 162 **1810** (cry 1's EOT, raw f163), 163 1192,
164 on 1007.  Against the twin the third-cry run differs on 62 (+2481),
109 (+93), 110..136 (-20), 137 (-113), 143 (-234), 144 (+384: 2145
against the twin's finished tick 1761), 145..161 (+569) and from 162 on
(+569: cry 0 alive with a dead track where the twin has cry 1 alone, then
paused), nothing else.

| player | body | where |
|---|---|---|
| cry 0 | 2991 | frames 3 and 62 (start ticks, free take of channel 0) |
| cry 0 / cry 1 | 510 | steady tick, live channel |
| cry 0 / cry 1 | 603 | the unlink tick (109 / 141; 137 in the twin) |
| cry 0 / cry 1 | 490 | steady tick, dead track |
| cry 1 | 3061 | frame 22 (start tick, pass-over 70 + free take) |
| cry 0 / cry 1 | 724 | the EOT on an empty chain (202 / 162; 143 in the twin) |
| cry 0 / cry 1 | 106 | the finished tick |

**The chain rule holds on every frame**: CgbSound = 1007 + sum over playing
players of (79 + body), body = `_081DD9BC` -> `_081DDA6C` measured per
player, **177 / 177** frames (2..178) of the third-cry run and 177 / 177 of
the twin (`build/scratch/bodies.py`), each player's body one of the seven
values above.  Every number is an existing step 5 / 11 term (the 1007 idle
frame, 79 + body per playing player in SoundMain's order, the RAM
template's command costs, `SCAN_FREE` 14, the unlink + 93, the dead track
- 20, the EOT + 234, the finished tick 185); **no constant is derived,
fitted or changed**, and no segment in the three logs falls outside the
existing `SCAN_*` / `NOTE_UNLINK_*` / `WALK_*` / `M4A_CYC_*` set.

**Deaths and unlinks**: Jynx has no death of its own -- cut by `TrackStop`
on VBlank 60 (raw f62) with about 75 frames of sample left, its unlink
frame 137 and cry 0's EOT 143 gone; Bulbasaur (channel 0) ends through the
count path, unlink 109, EOT 202; Wailord (channel 1) count path, unlink
141, EOT 162 -- no `_081DCFB0` stamp in any log, every EOT on an empty
chain (`_081DD938` r3 = 0x3C after `ply_endtie`, `ply_fine` -> `_done`).
One channel per track's chain throughout.

**Scheduler-lateness floor** (`.events` `L timer0`): third-cry run **7**
crossings at 3428099, 4551683, 5675267, 25899776, 30394112, 31517702,
33764869 = 0.2043, 0.2713, 0.3383, 1.5437, 1.8116, 1.8786, 2.0125 s (the
240-frame run the same 7); twin **10** at 3428099, 4551683, 5675267,
25899776, 27035906, 28159491, 29283075, 30406659, 31530243, 33777409 =
0.2043, 0.2713, 0.3383, 1.5437, 1.6115, 1.6784, 1.7454, 1.8124, 1.8793,
2.0133 s.

**The reference for slice 10b**: `mgba_render --type cry --id 123 --id2 381
--start-frame2 20 --id3 0 --start-frame3 60 --frames 182` ->
`build/scratch/cry_third_jynx_wailord_bulbasaur.wav` (582348 bytes, 145576
samples at 48 kHz); not listened to, not compared.

**What slice 10b needs -- structural statement only, nothing implemented**
(against `plugin/m4a_sequencer.c`, `plugin/m4a_cry.c` and
`cmd/poryaaaa_render.c` as they stand at this commit).
(1) **The player choice**: `m4a_cry_choose_player(players[2])`
(`m4a_cry.c`) returns the first player for which
`m4a_sequencer_player_free` holds and **-1 when neither is free**; it must
instead apply `SetPokemonCryTone`'s fallback over the non-NULL players --
`maxClock = 0, index = 0; if (maxClock < players[i]->clock) { maxClock =
...; index = i; }` strictly, so a tie or two zero clocks give **player 0**
-- and return that index.  The sequencer already keeps the GBA's `clock`:
`M4ASequencer.clock` is zeroed by `m4a_sequencer_start` (`m4a_sequencer.c`
1328, MPlayStart's `clock = 0`) and incremented once per tick after the
track loop (1136, `_081DD9A4`), so on VBlank 60 it reads 59 for cry 0 and
40 for cry 1 as the ROM's do.  In the over-song form (`--song --cry2`,
`players[1]` NULL: cry player 1 is never opened) the GBA would find cry 1
free and take it, so a third cry there needs cry 1 opened after cry 0 in
the chain (BGM -> cry 0 -> cry 1 is 14 tracks, SE1 -> cry 0 -> cry 1 is 7;
`init_chained_player` accepts both) -- or stays refused; the rule must not
fall back to replacing cry 0 with one player.
(2) **The replacement itself** is already `m4a_sequencer_start` on the
busy sequencer: it runs `track_stop` on every track (the GBA's `TrackStop`
through `MPlayStart`: every channel the track's `chainOwner` holds has
`chainOwner[h]` cleared -- `track->chan = 0`, `channel->track = 0` -- and
`m4a_engine_kill_channel` sets a PCM channel's `status` to 0 with no cycle
charge and no write, CgbOscOff for a CGB one), then sets each song track's
flags to EXIST | START with its `cmdPtr`, zeroes `clock`, `tempoC`,
`status`, resets the tempo -- MPlayStart's fields, plus the engine's own
bookkeeping (`chanSeq`, `seqCounter`, `noteCount`, the loop state; it has
no `fadeOI`).  One difference to note for 10b: `TrackStop_2` also stores
`channel->track = 0` on the GBA, while `m4a_engine_kill_channel` leaves the
engine channel's track field as it was; `m4a_sequencer_player_free` reads
that field only for a channel the chain still holds, so after `track_stop`
(chain cleared) it does not matter for the free test -- but any new test
that consults a killed channel's track field would differ from the GBA.
Issued at the top of the hook of engine frame `startFrame3 + 2` as
`cry2_hook` issues the second cry (between the two SoundMains, before
either body), the replaced sequencer's next body is a start tick whose
`ply_note` scan finds the killed channel's status 0 first in index order:
the engine's shared scan charges `SCAN_FREE` 14 and `NOTE_UNLINK_NONE` 44
+ `CHAIN_EMPTY` 4 on the emptied chain -- frame 62's 2991 and 4666 from the
existing constants, if the sequencer's start-tick path is unchanged by the
restart (the gate says).  The voicegroup: `m4a_sequencer_set_voicegroup(players[0],
voices3)` before the start, as `cry2_hook` does for the second cry (cry
0's first cry used the engine's `voiceGroup` from `--cry`; the per-sequencer
override of slice 8b-i is consulted by `ply_voice` only, and the dead
channel carries its voice by copy).
(3) **`poryaaaa_render --cry3 <id> --cry3-start-frame <1..255>`** in the
`--cry --cry2` form (both cry players opened): `sCry2` becomes a list of
pending starts (song, voices, startFrame, startedOn), each issued by the
hook on its frame through the chooser of (1); the `done` test covers every
pending start and both sequencers; the report names the player the third
cry took and the cry it replaced ("Third cry on cry player 0, replacing the
first cry at tick 59"); `--cry3` without `--cry2`, a `--cry3-start-frame`
not above `--cry2-start-frame`, and `--cry3` in the `--song` form are
refused with messages (the GBA's behaviour there is a different state than
this probe measured).  `--seq-log`'s `c` / `t` lines keep the engine track
index (0 / 1 for cry 0, 2 / 3 for cry 1), so `entry_diff` / `cmd_diff` /
`track_diff` read a three-cry log unchanged; the restart's `MPlayStart`
leaves no trace in the probe's per-frame tools.
(4) **Gates**: (a) `claim_check.sh` against the slice-10a baseline after
the engine change -- the counts unchanged, every row 0.0, `-inf` rows
`-inf`; (b) `entry_diff` / `cmd_diff` / `track_diff` 0 on the probed run
(`--cry 123 --cry2 381 --cry2-start-frame 20 --cry3 0 --cry3-start-frame
60`, 3 s) with frame 62's 4666 and the free take of channel 0 reproduced;
(c) the null against `build/scratch/cry_third_jynx_wailord_bulbasaur.wav`
at the one-cry rows' floor, every run above 20 LSB at one of the seven
crossings above; (d) a unit test on the frame skeleton 3..178 with the kill
of channel 0 on frame 62 (status 0, cry 0's `chainOwner` cleared, Wailord's
channel 1 untouched), the free take, the deaths 109 / 141, the EOT 162 on
an empty chain, the refusals of (3), and `m4a_cry_choose_player`'s
fallback (both busy -> the larger clock; a tie -> 0; one free -> it).
(5) **The corpus row** (10b-ii): `cry_third_jynx_wailord_bulbasaur` (cry,
id 123, id2 381, start_frame2 20, **`"id3"` 0, `"start_frame3"` 60**, 3 s;
the keys as `mgba_render` names them, both or neither, 0..65535 / 1..255,
start_frame3 > start_frame2), the runner's `--id3 N --start-frame3 F` /
`--cry3 N --cry3-start-frame F` lines, the reference frame count 182, the
`claim_check.sh` cry line with the two new flags, `--input midi` skipping
it like any cry.

### Done: SoundMain timing model (step 2, 2026-10-24)

**27/38 pass at -40 dB** (output-stage baseline: 15/38); `compare_reports.py`
against `baselines/2026-10-24_output-stage.json`: 0 regressions, 18
improvements.  Every single-track PSG micro-test and psg_mix are now
**bit-exact** (sq1, sq1_env, sq1_sweep, sq1_alt, sq2_alt, wave, wave_alt,
noise, noise7, tune_psg, psg_mix: -inf), lfo_psg -59.9; pcm_psg -17.0 ->
-29.8; mus_weather_groudon +1.3 -> -5.0, the other songs 0-3 dB better.
Still failing: pcm_reverb -10.4, pcm_chord -28.6, vel's worst second
-38.7 (mixer items), pcm_psg -29.8 (loud PCM+PSG; the mixer's 8-bit sum),
and the seven songs (-4..-10).

The model (constants `M4A_CYC_*` in `plugin/m4a_engine.h`, every number a
probe measurement, none fitted to the harness):

- `m4aSoundMain` is stamped at VBlank line 160 + 197 (SoundMain's entry;
  was +1740 for every write).  The sequencer (`m4a_sequencer.c`) sums
  MPlayMain's work in cycles while it runs: CgbSound entry = 1007 when the
  player is paused, else 1396 + 6 per existing track, + per tempo tick 404
  + 60 for the first track and 84 for each further one, + per command its
  measured handler cost (keysh 182, tempo 184, voice 277, vol 182, pan
  184, bend 168, bendr 182, lfos 162, mod 164, modt 184, tune 184, xcmd
  203, others 180), + 264 per track start, + 924 per CGB note (noise 832,
  +50 when the CGB channel was still sounding, +6 for an explicit N##
  byte), + 810 + 130 x busy channels scanned per DirectSound note, + 120
  per active LFO step, + 87 per dead channel dropped from a chain, + 376
  per sounding channel in a flagged track's VOLCHG/PITCHG pass.  It is
  published as `engine->cgbEntryCycles`; without a sequencer (the plugin's
  MIDI path) the steady one-track value applies, so those writes land at
  +1753, where they used to land at +1740.
- The CgbSound driver (`m4a_channel.c`) carries a cycle cursor through the
  four channels: prologue 120, inactive channel 41, active channel without
  writes 244 (+163 per envelope pass with envelopeCounter 0, +36 for the
  c15 double step), a channel ends 93 after its last write.  Write offsets
  inside a channel: start sequence first write at 231 (sq1) / 239 (sq2) /
  255 (wave with RAM upload, 261 without) / 246 (noise), the gaps of the
  probe table (NR10->NR11 10, NR30->WAVE 26, WAVE 15, NR41->NR43 8, ...),
  start block -> NRx3 166 (sq) / 158 (wave) / 162 (noise), +36 when the
  start goes straight to sustain (decay 0), +40 for TONEDATA_TYPE_FIX;
  NRx3->NRx4 32 (noise 24); NR51 33 after NRx4 or, when nothing was
  written before, at 207 (decay start) / 215 (release start) / 380 (sq) /
  416 (wave) / 417 (noise) (decay->sustain) / 350 (wave sustain step) /
  376 (pseudo-echo start); NR51->NRx2 28 (wave 32), NRx2->NRx4 10, wave
  NR32->NR30 16, NR30->NR34 6; CgbOscOff at 211 / 237 / 228 (release 0)
  or 149 / 175 / 166 (echo expired), NRx2->NRx4 14 (noise 6).  Each write
  runs the hardware up to its cycle (`m4a_gba_run`) first, so DAC slots,
  timer ticks and the PSG frame sequencer interleave with the writes as in
  mgba.
- `c15` starts at 0 (the ROM's SoundInit value; the first CgbSound, frame
  2's, reloads 14, frame 3's sees 13 as measured).  The harness cannot
  tell 0 from 14 (bit-identical on all 38 tests: every micro-test
  envelope is in sustain when the double step lands), so the probe decides.
  The frame-1 SoundMain of the earlier reading does not exist (1b), so
  nothing is replayed in `m4a_engine_boot`.

Verification: `poryaaaa_render --song ... --psg-log <file>` logs every PSG
write with its cycle and `tools/probe_diff.py` diffs it against the probe.
**Residual** (engine minus mgba, cycles): sq1, sq1_env, wave (180
frames), noise: 0 on every write (wave 180: one write +3); lfo_psg <= 4;
psg_mix <= 36 (one wave sustain step +12); pcm_psg -18 on the 3-track
start frame, -98 on a note frame after four PCM notes ended; groudon
-10 on the start frame, -12..-88 on steady frames (per-track tick cost
varies with track state: 61-84 per track measured); littleroot (9 tracks,
PCM-heavy) -1049 on the start frame and -460..+320 on note frames.  What
the data does not resolve: the DirectSound note cost beyond the channel
scan (voice type, keysplit/drum lookup, ClearChain of a dead channel it
takes: 806-1423 measured), the per-track tick cost's dependence on track
state, and the VOLCHG/PITCHG pass's per-track share (376 was measured on
one track with one channel).  These bound the songs' PSG timing to about
+-400 cycles (1.5 DAC slots) on PCM-note frames; a probe with breakpoints
inside MPlayMain's track loop and ply_note's channel search would settle
them.  The square tests are insensitive to this (duty phase free-running,
-inf already); wave and noise are not (their phase runs on the CPU
clock), which is why the songs with wave content remain at -5..-9 dB
until those costs are measured.

Not a timing item but found on the way: `voicegroup_loader.c` had made
`sq1_sweep` silent with `%d` (fixed in milestone 2); nothing comparable
this time.  The MIDI path (`--input midi`) still renders (lag 35-50 ms
against the ROM as before; it is not expected to null).

Code review of the timing model (two independent reads: the model and
driver against m4a.c CgbSound / m4a_1.s MPlayMain and the probe frames;
the probe and the scheduler interaction against mgba `core/timing.c`,
`gba/{timer,audio}.c`, `gb/audio.c`).  Verified and fixed (commit after
9751614):

- Event ordering at a write cycle: mgba's run loop processes every event
  due by an instruction's start cycle before executing it, so a store at T
  lands *after* the events due at T; the engine did the opposite
  (`m4a_gba_run(cursor - 1)`).  Now `m4a_gba_run(cursor)` / `m4a_gba_run(next)`.
  Only a PSG frame step coinciding with a write could differ; the harness
  is unchanged (adopted on the source reading, verified neutral).
- The CgbSound entry is based on the SoundMain stamp
  (`engine->soundMainStamp`), not on `gba.now`, which a TrackStop's
  CgbOscOff writes had already moved by 6-14 cycles.
- Constants that disagreed with the probe frames: the gate-expiry branch
  of MPlayMain's chain walk costs 8 per expiring channel (sq1/sq1_env/noise
  frame 33: entry 1874 vs 1866; psg_mix frame 63: +32 for four) and is
  charged in the sequencer; the oscillator-off and release-start offsets
  lose the 8 they had absorbed (203/220/229 and 207).  A plain decay or
  release step before a write costs 195, a sustain step 163 (sq1_env
  frames 9/20/31).  A quiet active channel costs 238 (psg_mix frames
  23/30/36/43/49/56: the wave's NR51 at 338 after two 163-pass squares).
  Square-2 start 237 and the PWM duty write positioned like a pitch write;
  envelope passes after a start block count toward the MO_PIT write.
- Probe: timer-0 slot crossings now use the DAC-grid test (the `until`
  form misses crossings at a 1024-cycle window end); IWRAM symbol
  addresses come from the map; a square-phase counter for the frame step
  (GBAudioUpdateFrame runs the PSG to the late time).  Re-measured over
  the same six runs: identical counts (0 crossings in the micro-tests,
  5 / 6 in littleroot / groudon, `until` form and grid form agree) and
  **0 square duty steps between a frame event's due and processing time**
  in any run, so 1c's conclusion stands.

Review claims **rejected by measurement**: "the tick's per-track cost is
34 + 26 per live channel in the chain" (sq1 frames 36-47 read 438 instead
of 464 once the note has died).  Charging it per chained channel breaks
the start frames (sq1/wave/noise -26, psg_mix -104; sq1_alt, wave,
wave_alt, psg_mix lose -inf) because the start frame costs the full 60
with an empty chain; the 438 belongs to the waiting-track state and is
not modeled.  Noted, not acted on (unmeasured): a channel idling in
pseudo-echo is charged the 238 quiet cost although its CgbSound pass is
shorter; ply_fine's ClearChain; ply_note's ClearChain of a dead chained
channel; TrackStop's writes stamped at the SoundMain entry rather than
inside MPlayMain.  Write-cycle residual after the review: sq1, sq1_env,
wave 0; noise 0 (start +3 in psg_mix's noise-after-wave position); lfo_psg
<= 4; psg_mix <= 29; songs unchanged from the figures above.  Harness:
27/38, 0 regressions against the timing-model baseline.

### Engine milestone 3 handoff (2026-10-24, as given; done below)

Start from the tip of branch `accuracy-harness` (the review-fixes commit and this baseline refresh).  Before any change
the harness must reproduce `baselines/2026-10-24_timing-model.json`
(27/38; `compare_reports.py`: 0 regressions, 0 improvements) and
`build/poryaaaa_unit_tests` 973/973, `poryaaaa_blip_tests`,
`poryaaaa_song_tests build/accuracy/rom` must pass.

Scope, as defined under "Engine milestones" item 3: verbatim C translation
of SoundMainRAM (`~/pokeemerald/src/m4a_1.s` lines 88-600: the reverb
pass over the previous DMA buffer, the 224-sample frame, the 8-bit
wrapping `add r6, r1, r6, ror 8` sum into packed 4-sample words, the
fixed-frequency (`TONEDATA_TYPE_FIX`) and interpolating (`fw` / `divFreq`)
loops, `SoundMainRAM_Unk1/Unk2` for CMP/REV samples), the game's reverb
(`SoundMainRAM_Reverb`: `(a + b + c + d) * reverb >> 9`, rounded toward
zero on the sign bit), with `voicegroup_loader.c` emitting exact GBA
struct layouts (`ToneData` 12 bytes, `WaveData` header 16 bytes + data)
with host pointers.  The PCM rate option becomes the sound-mode frequency
enum (`gPcmSamplesPerVBlankTable`, `m4a_tables.c:102`, twelve rates,
default `SOUND_MODE_FREQ_13379`); "mix at host rate" (pcmMixRate 0) is
removed from the engine, CLI (`--pcm-mix-rate`), GUI combo, config key
and CLAP state (keep loading old saves).

Targets: pcm_reverb -10.4, pcm_chord -28.6, vel worst second -38.7, and
pcm_psg -29.8 (loud PCM + PSG: likely the 8-bit sum).  Nothing in the
timing-model baseline may regress; songs should gain on their PCM side.

Constraints to carry: the plugin's MIDI path keeps rendering (`--input
midi`); the opt-in flags keep behaving as documented in the plan and the
memory file (respectBaseMidiKey, portamento, pwm, Golden Sun synths,
pcmOverflowMode).  Where the verbatim port makes one meaningless (the
8-bit wrap is now stock behaviour, so `pcmOverflowMode` "wrap" is the
default path and "off"/"clip" become the deviations), say so here rather
than dropping it silently.  Unit tests against hand-computed SoundMainRAM
arithmetic (one channel, one frame; the ror-8 packing; the reverb
rounding; a wrap case).  One commit per sub-step, harness after each.

Known from this session, relevant to the port: the engine's PCM path
today is `m4a_channel.c` (`m4a_pcm_channel_start/tick/render`, int32 mix
with a DPCM block cache and the Golden Sun synth types), `m4a_reverb.c`,
and `mix_pcm_frame` in `m4a_engine.c` writing int16 into the ring the DMA
model reads (`M4AFifoWord.s[4]` is int16 so the ring can carry unwrapped
values; the hardware is 8-bit).  PCM is already bit-exact on single-note
tests (pcm_adsr, pcm_drum, pcm_bass -inf), so the envelope arithmetic and
the sample-and-hold are right; the port must not move them.

### Done: engine milestone 3, the mixer (2026-10-24)

**31/38 pass at -40 dB** (timing-model baseline: 27/38);
`compare_reports.py baselines/2026-10-24_timing-model.json
baselines/2026-10-24_mixer.json`: 0 regressions, 20 improvements, every
test that was -inf is still -inf.  Three commits on `accuracy-harness`
after cf9ac17, harness after each:

1. **9d2ac2e, the SoundMainRAM port** (`plugin/m4a_mixer.c`, with
   `m4a_mixer.h`): m4a_1.s lines 88-709 translated with the register names
   kept as variable names.  The reverb pass over the ring (both taps, the
   `asr 9` / `tst 0x80; addne 1` rounding, mono write to both sides, the
   counter-2 wrap to the buffer start), the no-reverb zero fill, the
   packed-word `mul / bic 0xFF0000 / add ror 8` accumulate
   (`m4a_mixer_acc`) with the 8-bit lane wrap and the mid-word stop
   rotation, the fixed-frequency block/single loops with their r8/r9
   bookkeeping and loop restart, the interpolating loop with `bic r9,
   0x3F800000`, the `lr - 1 == 0 -> r0 += r1` advance and the loop-end
   overshoot, and Unk1/Unk2: the lazy SF_SPECIAL setup (REV pointer flip
   `data + size - offset`, CMP index `ptr - data`), xpi = 0xFF000000, FIX
   step 0x800000, the three Unk1 loops, the stop path that does store
   fw/count/pointer (unlike the main path's), the nibble order of the
   decoder and **one engine-wide decoding buffer** (sDecodingBuffer) with
   the per-channel xpi tag, so two compressed channels clobber each other's
   block exactly as on the GBA.  A CMP-typed voice on uncompressed data
   returns without mixing, as `_081DD468` does (the old engine played it).
   The DMA ring is `int8_t pcmBuffer[2 * 1584]` in the engine
   (SoundInfo.pcmBuffer's layout; m4a_gba reads int8), so the FIFO sees the
   wrapped bytes.  SoundChannel.frequency is MidiKeyToFreq's result again
   and divFreq is applied in the mixer, as `mul r4, r12, r1` does.  The PCM
   envelope step (`m4a_pcm_channel_tick`, unchanged arithmetic) now runs
   inside SoundMainRAM per channel, before its mix, instead of in the tick.
   `m4a_reverb.c` (a separate delay line) is gone; the ring is the reverb
   memory, `M4AReverb` keeps only the amount.  The host-pointer WaveData
   (header fields + `int8_t *data`) stands in for the inline GBA layout:
   the only two address computations that depended on it are the REV flip
   and the CMP index, both rewritten relative to `data`; the reverse path's
   look-behind before the first sample reads 0 (the GBA reads the header's
   last byte, `size >> 24`).  Harness: 27 -> 30/38; pcm_reverb -10.4 ->
   -46.1, pcm_chord -28.6 -> -44.7, pcm_psg -29.8 -> -44.7, mus_littleroot
   -9.1 -> -27.4, mus_vs_trainer -9.7 -> -26.9, mus_credits -7.2 -> -18.6,
   mus_vs_wild -9.7 -> -19.7.  Unit tests (`test_mixer_arithmetic`, and the
   reverse / DPCM / synth tests rewritten to drive frames through
   `m4a_mixer_mix_channel`): the ror-8 packing against hand-computed words
   (0xEC0FF605 for 10,-20,30,-40 at volume 128; lane add onto a loaded
   word; 126 + 126 wrapping to -4; the bic dropping a sub-256 product), a
   FIX frame on an 8-sample loop, the interpolating frame with its
   look-ahead and mid-word stop, a loop-end overshoot, two channels wrapping
   a lane, the reverb rounding cases (-40*64>>9 = -5 -> -4; -64>>9 = -1 ->
   0), the counter-2 tap, the zero fill, the shared decoding buffer.
2. **f0d1ebc, the look-ahead byte** (`voicegroup_loader.c`): the mixer
   interpolates the last sample toward `data[size]`.  The loader had
   duplicated the last sample there.  The claim "the converter pads with 0"
   (from wav2agb's `convert_uncompressed_bin`) was tested alone and
   **rejected by measurement**: pcm_adsr (-inf -> -58.4) and pcm_bass (-inf
   -> -50.0) regressed while the unlooped and most looped tests improved.
   Reading the test ROM settled it: the .bin files pokeemerald ships carry
   one extra sample after the data, the loop-start sample for a looped
   sound (sc88pro_flute 0xF8, pizzicato 0x24, fretless_bass 0x0E) and 0 for
   an unlooped one (sd90_solo_snare), followed by alignment padding.  The
   .bin path now reads that byte from the file; the .wav/.aif paths derive
   it.  Harness: 30 -> 31/38; pcm, pcm_hi, pcm_adsr, pcm_reverb, pcm_bass,
   pan, vol, vel, bend, lfo_vib/trem/pan/delay and tune are **bit-exact**;
   pcm_chord -55.7, pcm_psg -55.0.
3. **26f691b, the sound-mode frequency enum**: `engine->pcmFreqMode`
   (SOUND_MODE_FREQ_xxxxx >> 16, 1..12 into gPcmSamplesPerVBlankTable,
   default 4 = 13379 Hz), `m4a_engine_set_pcm_freq_mode` running
   SampleFreqSet's arithmetic and reprogramming the timer.  "Mix at host
   rate" is gone from the engine, `--pcm-mix-rate` (snaps to the nearest
   table rate and says so), the GUI combo (the twelve rates), the config
   key and the CLAP state; the state still stores the rate in Hz as a float
   at the same offset, so older saves load (a stored 0 maps to 13379).
   Harness unchanged (31/38, the default mode is the game's).

Per-test null, before (timing model) / after (mixer):
sq1..noise7, pcm_adsr, pcm_drum, pcm_bass, tune_psg, psg_mix -inf / -inf;
pcm -46.3 / -inf; pcm_hi -47.2 / -inf; pcm_cmp -55.0 / -55.0; pcm_reverb
-10.4 / -inf; pcm_chord -28.6 / -55.7; pan -48.6 / -inf; vol -56.7 / -inf;
vel -56.7 (worst 1 s -38.7) / -inf; bend -43.1 / -inf; lfo_vib -42.6 /
-inf; lfo_trem -42.0 / -inf; lfo_pan -42.1 / -inf; lfo_delay -42.3 /
-inf; lfo_psg -59.9 / -59.9; tune -58.6 / -inf; pcm_psg -29.8 / -55.0;
mus_littleroot -9.1 / -27.4; mus_weather_groudon -5.0 / -5.0; mus_vs_wild
-9.7 / -19.7; mus_mt_chimney -5.5 / -7.4; mus_rg_vs_trainer -6.4 / -8.3;
mus_vs_trainer -9.7 / -26.9; mus_credits -7.2 / -18.6.  `build/poryaaaa_unit_tests`
1016/1016, `poryaaaa_blip_tests` 49112/0, `poryaaaa_song_tests` 561/561.

What is left on the PCM side is **mgba's scheduler lateness (1c), not the
mixer**.  pcm_cmp -55.0 is seven single-sample events (0.0706, 0.7403,
1.7118, 1.7449, 1.8118, 1.9458, 2.0128 s), each one PCM sample held one
65536 Hz DAC slot late; the Cry_Bulbasaur bytes the loader holds are
identical to the ROM's (4220 data bytes checked), the DPCM decode is
exact in the unit tests, and `mgba_probe` on pcm_cmp (type 2, id 14, 180
frames) reports **9 timer-0 overflows crossing a DAC slot** (max late 9
cycles, first at cycle 1180930 = 0.0704 s, the first error event) against
0 on pcm (bit-exact) and 6 on pcm_chord: the compressed decoder and the
multi-channel mix are the long ARM instruction streams a timer event
lands behind.  pcm_chord -55.7 and pcm_psg -55.0 are the same floor.  This
is the item deferred in 1c ("5-6 one-slot sample-and-hold errors per 1.17
s ... around -50 dB"); modeling it means modeling instruction timing, not
the driver.  The songs are bounded by the PSG write timing of the
timing-model section (mus_weather_groudon, mt_chimney, rg_vs_trainer:
wave/noise-heavy, unchanged by the mixer) plus this floor.

Opt-in flags after the port: respectBaseMidiKey (key math in
`m4a_engine_note_on`, untouched), portamento (CC 5; the legato inherit now
also copies CHN_SPECIAL and xpi so an inherited reverse/compressed
position is not set up twice) and pwm (CGB only) behave as documented.
Golden Sun synths (zero-size samples) keep generating through the same
packed accumulate (`mix_synth` in m4a_mixer.c, an extension marked as
such); on stock hardware a zero-size sample is a one-sample click, so this
remains the one deliberate deviation in the mixer.  **`pcmOverflowMode`
does not exist in the tree**: `git log --all -S pcmOverflowMode` finds it
only in this plan's handoff text (commit 9751614), never in code, on any
branch; the memory note describing it records work that was never
committed.  With the port the 8-bit wrap is simply stock behaviour; an
"off"/"clip" deviation would need a wider lane accumulator and is not
implemented.

Code review of the mixer (two independent reads, given the sources and
the diff: the mixer/reverb/envelope against m4a_1.s SoundMainRAM, Unk1,
Unk2 and m4a.c SoundMain/SampleFreqSet; the loader's layouts, the
look-ahead byte, the rate plumbing and the plugin state against
m4a_internal.h, wav2agb, music_voice.inc and m4a.c).  Both found **no
divergence in the stock mixing paths** (every loop, exit path, the reverb,
the slot selection and SampleFreqSet's arithmetic confirmed instruction by
instruction; the second review also verified the .wav loader byte-exact
against all 105 vanilla .bin files).  Each claim below was applied alone and
measured; every one was harness-neutral (31/38, 0 regressions, 0
improvements against the mixer baseline), as expected for changes outside
the stock paths, and the unit tests gate them (1018/1018):

- The Golden Sun synth pitch was wrong after the port: `mix_synth` advanced
  the phase by `frequency << 3` while `frequency` had become the raw
  MidiKeyToFreq value (the step is `divFreq * frequency`); and the START
  envelope step, now run right before the first mix, wiped the triangle's
  90-degree start phase.  Found through the review's "first frame starts at
  the negative peak" claim; fixed (the step is scaled, the tick keeps a
  synth's primed phase), and the unit tests now check the first mixed frame.
- A `cry` / `voice_directsound_compressed` voice whose .bin was missing or
  stale fell back to the uncompressed source, which the verbatim Unk1 then
  mixes as silence (`_081DD468: tst REV; beq _081DD4F0`); the loader now
  clears the voice's CMP bit on that fallback so it plays as a plain voice.
- The .wav path's look-ahead byte is now the source's own extra sample
  (`sample[size]` when the header size is the `agbl` override below the
  written count, else 0, as wav2agb's `.bin` output has it) rather than the
  derived `data[loopStart]`; identical for every vanilla file, right for a
  user .wav without `agbl`.  The .aif path reads its last sample the same
  way.
- `m4a_engine_set_pcm_freq_mode` now does what m4aSoundMode does around
  SampleFreqSet: DMA off, ring cleared, timer off, reload, DMA on, timer on
  one period later (the VCOUNT 159 wait is not modeled; call while stopped).
- Unk1's stop path stores count 0 (`_081DD4F4: mov r2, 0`), the Unk2 block
  pointer is only formed inside the data, a stale ring comment.

Review notes not acted on: `masterVolume` starts at 12 (m4aSoundInit's
value; the memory note saying 15 "must stay" predates the PSG model and the
bit-exact harness, and is now wrong); `_081DCFC8` keeps the decay bits
under IEC where the port clears them (no observable effect); a loop with
loopStart >= size is disabled by the loader where the GBA would never end
(malformed data); the DPCM reverse look-behind at index -1 and the
look-ahead past the last block decode zeros where the GBA reads unrelated
memory.

### Done: timing model step 3, the songs (2026-10-24)

**38/38 pass at -40 dB** (mixer baseline: 31/38); `compare_reports.py
baselines/2026-10-24_mixer.json baselines/2026-10-24_timing-3.json`: 0
regressions, 8 improvements, every -inf still -inf.  **mus_weather_groudon
is bit-exact over its 20 s (-inf)** and lfo_psg too; the six other songs
sit at -49 .. -53 dB, the mgba scheduler-lateness floor of the PCM mix
(item 1c), with their PSG writes landing on mgba's cycle.

Per-test null, before (mixer) / after (timing 3): every micro-test that
was -inf is -inf; pcm_cmp -55.0 / -55.0; pcm_chord -55.7 / -55.7; lfo_psg
-59.9 / **-inf**; pcm_psg -55.0 / -55.0; mus_littleroot -27.4 / -49.6
(worst 1 s -22.4 / -44.2); mus_weather_groudon -5.0 / **-inf**; mus_vs_wild
-19.7 / -49.4 (-12.0 / -40.1); mus_mt_chimney -7.4 / -49.6 (-2.8 / -44.3);
mus_rg_vs_trainer -8.3 / -49.3 (-2.9 / -42.9); mus_vs_trainer -26.9 /
-52.7 (-16.2 / -45.2); mus_credits -18.6 / -51.3 (-12.9 / -40.7).
`poryaaaa_unit_tests` 1032/1032 (14 new: the sq1 probe's CgbSound entry on
frames 3..52, the FIFO stall arithmetic, the inactive-channel skeleton),
`poryaaaa_blip_tests` 49112/0, `poryaaaa_song_tests` 561/561; `--input midi`
still renders.

**Write-cycle residual** (engine minus mgba, every PSG write of the 1200
frames the probe ran, `probe_diff.py` with threshold 0), before / after:
littleroot start frame -1051, note frames -460..+320, envelope writes
-188 and +101 / **0 on all 1178 writes**; groudon -12 start, -12..-88
steady / **0 on 2815**; mt_chimney, vs_wild, vs_trainer, rg_vs_trainer
(not measured before) / **0 on 4237, 1118, 927 and 4262**; psg_mix <= 29
/ 0; lfo_psg <= 4 / 0; pcm_psg -18 / 0; tune_psg 1 (unchanged; one write,
the DAC-slot rounding of a pitch update).  The modeled CgbSound entry
matches the probe's on every frame of all six songs (`track_diff.py`:
0 divergences) and every ply_* handler entry lands on its breakpoint
(`cmd_diff.py`).

The work, in order (one build and harness per step; commits on
`accuracy-harness` after 4332497):

1. **The probe inside MPlayMain** (75ce797): breakpoints on the local
   labels from the ELF's nm listing, registers and the deciding memory
   bytes at each, `probe_mplay.py`; the numbers of the probe section above.
2. **The sequencer charged segment by segment** (`m4a_sequencer.c`,
   `M4A_CYC_*`): the aggregates of step 2 (tick base, per-track, note 924 /
   832 / 810 + 130 x scan, apply 376, start 264, LFO 120) are replaced by
   MPlayMain's control flow with the measured cost of every segment:
   the frame skeleton (BGM player last of six, 566 / 150 / 370), both loops
   over the player's 10 tracks (not the song's), the chain walk with the
   gate paths and RealClearChain's four link cases (`chainOwner` /
   `chanSeq` reproduce the chain's membership and order, including the
   neighbours changing as dead channels drop out), the START init, the
   wait / LFO path, the dispatch (explicit byte vs running status, below /
   above 0xBD), each handler, ply_note's byte parsing, voice resolution
   (plain / keysplit / rhythm, ROM sub-voice reads), the CGB channel test,
   the DirectSound scan replicated channel by channel, the unlink with the
   chain-empty store, TrkVolPitSet by flags and modT, ChnVolSetAsm, the
   frequency routines by channel and key, and the VOLCHG/PITCHG pass per
   track and per chained channel.  Harness after this step alone: 32/38,
   credits -51, vs_trainer -47, mt_chimney -21.5; littleroot regressed to
   -25.6 (worst second -12.5) because the exact entry exposed the driver's
   multi-channel write offsets, which the old overcharged entry had been
   compensating -- the step below.
3. **The CgbSound driver charged segment by segment** (`m4a_channel.c`,
   `CGB_CYC_*`): the per-write offsets measured on single-channel
   micro-tests (prologue 120, inactive 41, 207 / 380 / 416 ...) are
   replaced by the pass structure in the probe section: loop head,
   switch, start / nostart, the step with CgbModVol's pan class, the
   transitions, the double step, the pitch and volume blocks, CgbOscOff,
   and the channel's exit, so a channel's writes follow the channels
   before it exactly.  The engine's channel loop starts at the loop head
   (53 / 51) and an inactive channel costs 44.  Harness: 35/38, littleroot
   -49.6, groudon -40.2.
4. **The FIFO DMA stall** in both models (18 cycles when a charged span
   crosses a refill), and the corrections the three comparison tools
   found (the probe section's last paragraph: the dispatch byte costs,
   the LFO halves, the scan compare, pend, endtie, the CGB takeover, the
   low-key and clamped-key frequency paths, NR34 after a retrigger, the
   apply pass's PITSET and next-channel step).  Each was a one-constant
   change verified by the diff tools before the harness; none moved a
   bit-exact test.  Harness: 36/38 after the frequency-key paths (groudon
   -inf), 38/38 after the wave retrigger tail (rg_vs_trainer -49.3,
   mt_chimney -49.6).

Tools added (`test/accuracy/tools/`): `probe_mplay.py` (MPlayMain
segments keyed on state), `probe_cgb.py` (CgbSound passes and segments),
`entry_diff.py` / `cmd_diff.py` / `track_diff.py` (the engine's
`--seq-log` -- per frame the CgbSound entry, per command the handler entry,
per track the `_081DD998` position -- against the probe), and
`poryaaaa_render --seq-log`.  The accuracy-harness skill documents them.

What the model still estimates (paths the corpus never takes; each marked
*est.* in the headers): a ply_note rejected by priority or taking a CGB
channel from a lower priority, a note with no free DirectSound channel, a
scan step meeting a strictly lower or higher priority, a gate-extension
note byte, a keysplit sub-voice that is itself split, the CGB start of a
keysplit/drum voice (+16 for its ROM reads), the decay -> pseudo-echo
start with an echo (237), square 1's second NR14 write with an upward
sweep, a dead channel met in the pitch pass, `lfoSpeed == 0` in the wait
path, pattern level 0 on pend, the START | STOP oscillator-off, and the
unmeasured one-byte handlers (tune, rept, memacc, prio, fine, port at
90).  A future song that uses one of them will show up in cmd_diff /
track_diff as a constant offset on that command.  Not modeled: the
scheduler lateness of the PCM mix (1c, -50 dB floor), the DMA stall's
instruction-boundary alignment (mgba's first FIFO write varies 15091..
15096 with the instruction in flight; the model charges at 15091), and
the one-cycle DAC rounding tune_psg shows on a pitch update.

**Code review of the timing model step 3 (2026-10-24).**  Two
independent reviews (probe + cost model against m4a_1.s / m4a.c and the
probe frames; sequencer charging against m4a_1.s and the recorded
numbers), every claim applied alone and run through the unit tests, the
write-cycle diff of the 14 PSG micro-tests and the six probed songs, and
the harness against the timing-3 baseline.  None moved a write or a test:
every adopted claim was harness-neutral (38/38, every -inf kept, all six
songs at 0 cycles on every write), none was rejected by measurement.
Adopted, source-verified: the FIFO stall counted on CgbSound's entry ->
loop-head segment too (R2#21); the apply pass charging
`cgb_freq_key_cost` after TrkVolPitSet, on the new keyM (R2#12); LFOS 0
charged the 109 of MOD 0, its clear_modM path (R2#3; unit test
`test_sequencer_cycles_lfos_zero`); the LFO step's rising half computed
on the untruncated `lfoSpeedC + lfoSpeed` sum, as `_081DD95A`'s `subs`
does, in both the sequencer and the engine's own LFO (R2#7; unit test
`test_sequencer_lfo_untruncated_sum`, which fails on the truncated sum
at the fourth step of LFOS 0x50); an attack step that reaches its goal
with decay 0 charged once, `CGB_CYC_STEP_ATTACK_SUSTAIN` 229 (*est.*
219 + 10) instead of attack->decay plus decay->sustain (R1#24 / R2#19;
unit test in `test_cgb_driver_square`, NR51 at CgbSound entry + 574);
with sustain 0 as well, `STEP_ATTACK_OFF` 221 / `STEP_ATTACK_ECHO` 247
(*est.*); a start frame with attack, decay and sustain all 0 keeping its
start-block cost (`pendStart + START_SUSTAIN`, *est.*) before the
pseudo-echo decision (R1#23); noise keys at or below 20 on their own
*est.* constant, 90 (R1#19); square 1's second NR14 write *est.* 14 then
6 (the measured 24 path less its taken branch; R1#26); ply_fine's loop
charged per chained channel, *est.* 14 + RealClearChain's measured
`WALK_UNLINK_*` cost, newest first (R1#16); a note the GBA rejected
charged its rejection only when the polyphony-debug invert mode plays it
on a shadow channel (R2#22, non-stock; unit test
`test_sequencer_cycles_poly_invert`); TrackStop killing only the channels
still chained to the track, as its `track->chan` walk does (R2#23; unit
test `test_sequencer_track_stop_chain`; no stock path can observe the
difference, since every unchaining either frees the channel or clears
the track's EXIST flag first).  Tooling and plumbing: the probe's `n4`
read from CgbChannel + 0x1A, not the frequency's high byte at + 0x21
(R1#1); the FIFO writes sit at frame cycle 15091..15105, as the logs
say, not 15092.. (R1#29); cmd_diff.py ignores the handlers' local labels
(`ply_fine_loop` ...) that the nm listing turns into breakpoints (R1#5);
`m4a_gba_set_pcm_rate` restarts the refill cadence (`fifoDmaCycle` = -1,
R2#27); poryaaaa_render closes its --seq-log / --psg-log files (R2#32).
Not acted on (unmeasured paths the corpus never takes, kept at their
*est.* values and listed here so the next review does not re-raise them):
the sweep test's pan-bit branch and the RHY path on CGB drum voices
(R1#18a / 17a, *est.* +-4); TrkVolPitSet's y clamp (R1#18b); CgbModVol's
clamp branches (R1#7, +-2); the measured -2 inNote asymmetry of TrkVolPitSet
(R1#18d, left as measured); the frame where every track has finished
(R1#14); the key > 130 clamp in MidiKeyToCgbFreq (R1#15); the `frameCount`
comment and the modulo in the FIFO stall test (R2#26, R2#31); the plan's
"other xcmd sub-commands at 90" wording, corrected above to the 110 the
code charges (R2#2); ply_endtie's walk over more than one chained channel
(R1#16, measured 95 / 89 with one).

### Done: milestone 4 slice 1, sound effects on the SE players (2026-10-24)

Six vanilla sound effects join the corpus as test type `se`
(`songs.json`, `"type": "se"`; `accuracy.py --tests se`): the ROM plays
them by `m4aSongNumStart` on the music player the song table names, and
the runner passes that player to `poryaaaa_render --player se1|se2|se3`
(the sequencer's `m4a_sequencer_set_player`: 3 / 9 / 1 tracks and the
player's MPlayMain chain position, probe section above).  Baseline
`baselines/2026-10-24_se.json` / `.txt`: **43/44 pass at -40 dB**; the 38
previous tests are unchanged to the decimal (`compare_reports.py
2026-10-24_timing-3.json 2026-10-24_se.json`: 0 regressions, 0
improvements, every -inf still -inf).  `poryaaaa_unit_tests` 1109/1109 (38
new), `poryaaaa_blip_tests` 49112/0, `poryaaaa_song_tests` 561/561 (the SE
`.s` files were already in it); `--input midi` still renders (the MIDI
path has no music player; `--player` prints a note and is ignored there).

| test | player | content | null | worst 1 s | ref dBFS |
|---|---|---|---|---|---|
| se_exp | SE1 | square 1, a TIE through pitch bends, EOT | **-inf** | -inf | -24.8 |
| se_faint | SE1 (all 3 tracks) | DirectSound snare, square 1 with sweep 0x17, noise | -53.3 | -42.9 | -29.9 |
| se_contest_place | SE2 (4 of 9) | DirectSound timpani, strings keysplit, square 1 + 2 | -53.7 | -44.7 | -26.9 |
| se_truck_unload | SE2 | noise only, FINE with a chained channel | **-inf** | -inf | -39.2 |
| se_low_health | SE3 | square 1, GOTO loop, XCMD pseudo-echo, priority 3 | **-inf** | -inf | -24.0 |
| se_rain_stop | SE3 | DirectSound only, BEND / BENDR, priority 2 | -41.8 | -23.4 (FAIL) | -41.3 |

The three pure-PSG SEs are bit-exact over their 2-3 s, one on each SE
player.  The two mixed SEs sit at the PCM scheduler-lateness floor like the
songs.  **se_rain_stop fails the worst-second gate on one PCM sample**: the
render equals the reference everywhere except a single 13379 Hz sample at
0.5394 s (2 LSB), and the probe's `.events` puts mgba's second timer-0
slot crossing at cycle 9046019 = 0.5392 s (`se_rain_stop.txt.events`,
`slot_crossed=2`; the first, at 0.0704 s, falls in the silence before the
note).  That is item 1c, the deferred floor, landing in a tail 46 dB down
where one sample is -23 dB of the window's energy; the full-file null
(-41.8) passes.  The test stays in the corpus as the PCM-only SE: swapping
it for one that happens to dodge a lateness crossing would hide the floor,
not fix it.  Nothing in the engine changes it (the plan's 1c).

**Measured SE-player constants** (probe section, step 4): a paused
non-BGM player 63 (`M4A_CYC_MP_IDLE`), a playing SE player's head 142
(`M4A_CYC_MP_SE_HEAD` = FadeOutBody 88 + 54), the last `_081DDA6C` ->
CgbSound 55 (`M4A_CYC_MP_TAIL`); `_081DD9BC` at 716 / 779 / 842 / 905 and
exit 370 / 307 / 244 / 181 for BGM / SE1 / SE2 / SE3; paused 1007 for all.
Three paths measured for the first time and adopted: ply_fine 44 with an
empty chain / 114 with one channel (`M4A_CYC_FINE_*`), the finished tick's
`_081DD9A4` -> `_081DDA6C` 24 (`M4A_CYC_TICK_ALL_DONE`), square 1's second
NR14 write 24 + 2 (`CGB_CYC_NR14_NR14` / `_DONE`, were *est.* 14 + 6).
Each was checked by the write diff before the harness; together they moved
no write of the previously probed corpus (the sweep in the probe section)
and no harness number.

**Per-SE write-cycle residual** (engine minus mgba, `probe_diff.py`
threshold 0, 180 frames): se_exp 0 on 284 writes, se_faint 0 on 38,
se_contest_place 0 on 51, se_truck_unload 0 on 22, se_low_health 0 on 100,
se_rain_stop no PSG writes; `entry_diff.py`, `cmd_diff.py` and
`track_diff.py` 0 divergences on every frame of all six.  Before the three
corrections: +44 / +46 on each FINE frame (ply_fine) and -10 / -6 on
se_faint's start frame (the NR14 path); the player positions themselves
matched on every other frame from the first render.

The work, in order (commits on `accuracy-harness` after 2fad88a):

1. **Probe**: the player argument for the `.frames` summary and
   `probe_mplay.py --player`; the six SEs probed; the numbers of the probe
   section (step 4) recorded before use.
2. **Sequencer**: `m4a_sequencer_set_player` (track count and chain
   position derived from the measured constants; init selects BGM, start
   clamps the song's tracks to the player's as MPlayStart does), the
   measured ply_fine / finished-tick / NR14 costs, `poryaaaa_render
   --player`; unit tests `test_sequencer_players` (the four players'
   positions and counts, the paused 1007, the SE1 / SE2 no-tick frames of
   se_faint / se_contest_place, a 4-track song on SE1 playing 3) and
   `test_sequencer_cycles_se2` (se_truck_unload's 30 frames on SE2 against
   its probe, FINE and the finished tick included).
3. **Corpus and runner**: `songs.json` type `se`, the player from
   `song_table.inc`, `--tests se`; README and skill.
4. **Baseline and this section.**

What the SE slice did not measure: a FINE with two or more chained
channels (est. the walk's unlink costs for the continuing iterations), a
SE on the BGM player or two players sounding at once (the probe's chain
holds for any combination of paused players; a second *playing* player
would add its body between the entries and is not modeled: the sequencer
is one player), MPlayStart's priority / `unk_B` gating against an already
playing song (the ROM starts one SE from silence), and the cry players
(next slice).

**Code review of the SE slice (2026-10-24).**  Two independent reviews
(player selection and MPlayMain position against m4a.c `m4aSongNumStart` /
`MPlayOpen` / `MPlayStart` / `CgbSound` and m4a_1.s, decoding the raw probe
logs themselves; runner, songs.json plumbing and the SE `.s` files against
the ROM bytes), then `tools/claim_check.sh` (new: build, the three test
binaries, the write diff of all 37 probed tests, the harness and
`compare_reports.py` in one run) on the adopted changes.  Neither found a
divergence.  Byte checks: gSongTable's `ms` for the six ids equals the
player the runner reads from song_table.inc, the header pointers equal the
map's labels, track count / priority / reverb bytes equal the `.s`,
gMPlayTable's numTracks 10 / 3 / 9 / 1 equal `m4a_sequencer_set_player`.
Confirmed from the logs: the chain positions of all four players on every
frame (item 2), both loops over the player's trackCount (3), ply_fine's
segments on all four FINE frames (4), the finished tick's 24 on the SE
frames **and on the BGM player** (wave.txt frame 156, `_081DD9A4` with r4
0 -> `_081DDA6C` = 24: the earlier base had charged the exit alone there),
the NR14 path reading IO NR10 as the C does (6), the unit tests' numbers
(8).  Adopted, harness-neutral (claim_check: 43/44, 0 moved writes, every
row 0.0): ply_fine charging a flat 50 per unlink (R1#4: the previous
`(unlink - WALK_UNLINK_00)` term was always 0 since every chained channel
at FINE has prev 0, and a chain of two or more is unmeasured -- the prev
0 / next set unlink is 4 cheaper than prev 0 / next 0 and the loop's
taken bne about 4 more, so est. 50 for those too); `--player` under a
"Compiled-song options" usage heading (R2#7); `probe_mplay.py --player`
anywhere in the arguments and its docstrings (R2#6); a test comment
(R1#8).  Not acted on: `m4a_sequencer_start`'s second loop clearing to
16 tracks instead of the player's count (R1#1; track_stop is a no-op on
a non-existing track and the frame loops visit maxTracks only -- the
wider clear is what a host switching players in-process needs); the
unk_B / priority gate of MPlayStart and `tone` (R1#1, unobservable from
one start); an out-of-range player selecting BGM without an error (R1#2;
the cry players are the next slice, which will extend
`M4A_PLAYER_COUNT`); the STOP flags of ply_fine set by track index rather
than by chain membership (R1#4, identical for every reachable state); a
misspelled songs.json entry being skipped with a warning like a missing
song (R2#2); `M4A_SEQ_DEFAULT_MAX_TRACKS` kept as a definition (R1#7 /
R2#9).  Baseline refreshed from the claim_check run after the fixes:
identical to the one above.

**Next slice: cries.**  `StartCry` in `test/accuracy/rom/main.c` plays
`gCryTable[id]` through `SetPokemonCryTone` on one of the two cry players
(`gPokemonCryMusicPlayers`, 2 tracks each, chain entries 291 and 346 and
bodies after SE3's: a playing cry player resumes 63 x 3 after the BGM's
`_081DDA6C` by the step-4 chain, to be measured, not assumed), with the
cry song template's pan / volume / pitch and the compressed (DPCM) cry
samples the micro-test pcm_cmp covers.  The runner needs a `cry` entry
type (`--type cry --id N --pitch --pan --volume`), the engine a way to
play a cry (the template song, `gPokemonCrySong`, with the tone pointer
patched), and the probe's player argument already takes
`gPokemonCryMusicPlayers`.  Before any change the harness must reproduce
`baselines/2026-10-24_se.json` (43/44; 0 regressions, 0 improvements) and
the three test binaries must pass as above.

### Done: milestone 4 slice 2, cries at the defaults (2026-10-24)

Four cries join the corpus as test type `cry` (`songs.json`, `"type":
"cry"`, `"id"` = the gCryTable index; `accuracy.py --tests cry`): the ROM
plays `gCryTable[id]` through `StartCry` with `PlayCryInternal`'s
CRY_MODE_NORMAL values (volume 120, pan 0, pitch 15360 = key 60 at c_v,
length 140, release 0, progress 0, chorus 0, priority 10), and the runner
passes `--cry id` to `poryaaaa_render`, which plays the cry song template
(`plugin/m4a_cry.c`: `gPokemonCrySongTemplate` with the `SetPokemonCry*`
setters applied in PlayCryInternal's order, the pointer words as
`SetPokemonCryTone` patches them, `dataInRam` set) with the cry's DPCM
sample as voice 0 (`voicegroup_load_cry`: the `cry` macro's voice, the
built .bin preferred as in a voicegroup) on cry player 0
(`M4A_PLAYER_CRY0`, the one `SetPokemonCryTone` takes from silence).
`--input midi` skips cries with a message.  Baseline
`baselines/2026-10-24_cry.json` / `.txt`: **45/48 pass at -40 dB**; the 44
previous tests are unchanged to the decimal (`compare_reports.py
2026-10-24_se.json 2026-10-24_cry.json`: 0 regressions, 0 improvements,
every -inf still -inf; se_rain_stop's documented FAIL as before).
`poryaaaa_unit_tests` 1170/1170 (61 new), `poryaaaa_blip_tests` 49112/0,
`poryaaaa_song_tests` 561/561.  `claim_check.sh`: 41 probed tests, 0 moved
writes -- tune_psg's "expected 1-cycle DAC rounding" exception is gone: it
was the TUNE cost (below), now 0 on its 16 writes.

| test | gCryTable | sample | null | worst 1 s | ref dBFS |
|---|---|---|---|---|---|
| cry_pidgey | 15 | 1902 samples, 0.18 s (the shortest) | -43.0 | -38.4 (FAIL) | -33.4 |
| cry_bulbasaur | 0 | 8183 samples, 0.78 s | -54.9 | -51.9 | -28.1 |
| cry_wailord | 381 | 20846 samples, 1.98 s | -48.1 | -46.9 | -22.4 |
| cry_jynx | 123 | 23524 samples, 2.24 s (the longest) | -41.1 | -38.2 (FAIL) | -26.2 |

**Residual attribution** (skill, "Attributing a residual below -40 dB"):
the sequencer is exact on every frame of all four (below), so the whole
residual is in the samples.  Every run of differing output samples above
20 int16 LSB (0.1 PCM LSB) starts within 1.5 ms of a timer-0 slot crossing
the probe's raw log records (`L timer0 ... FIFO sample lands one DAC slot
later`; the `.events` count): pidgey 1 crossing, 1 run; bulbasaur 9
crossings, 5 runs (the other four crossings differ by less); wailord 19
crossings, 16 runs; jynx 19 crossings, 16 runs; no run anywhere else.  The
first crossing is at cycle 3428099 (0.2043 s) in every run, as on
pcm_cmp.  That is item 1c, mgba's scheduler lateness, and nothing in the
engine moves it.  The cries show it harder than the songs and SEs because
a DPCM cry waveform jumps by several LSB per sample, so one sample landing
a DAC slot late differs by up to 8 (pidgey, wailord) or 12 (jynx) PCM LSB
where a song's smooth mix differs by 1 or 2: cry_pidgey's single crossing
falls inside its 0.18 s of sound and alone puts the worst second at -38.4;
cry_jynx's 12-LSB crossing at 1.343 s does the same.  Both fail the
worst-second gate on the floor, like se_rain_stop; the full-file nulls
pass.  They stay in the corpus for the same reason (swapping them for
cries that dodge a crossing would hide the floor, not fix it).

**Measured cry-player constants** (probe section, step 5): cry 0 starts
its body at SE3's `_081DDA6C` **826** (= 566 + 71 + 3 x 63), FadeOutBody
88 later, `_081DD9BC` at **968** (the SE head 142), exit **118** (cry 1's
idle 63 + the tail 55); every paused frame 1007; `SetPokemonCryTone` from
silence takes cry 0 on all four.  `m4a_sequencer_set_player` derives them
from the step-4 constants with `M4A_PLAYER_CRY0` / `_CRY1` (track count 2
each, `MPlayOpen`'s argument in `m4aSoundInit`) and now returns -1 for an
out-of-range player instead of selecting BGM (`poryaaaa_render` errors).
Cry 1 playing (entry 889, exit 55) is unmeasured: no run starts two cries.

**The song data is in IWRAM**: `gPokemonCrySongs` is RAM, so MPlayMain's
song-data reads are cheaper than a ROM song's by **7 cycles per byte read**
(`M4A_CYC_ROM_BYTE`, seven pairs in the probe section, including
ply_goto's 28 for four bytes and TIE's 21 for three reads).  `M4ASong.dataInRam`
(set by `m4a_cry_song_new`, 0 for every parsed .s) makes the sequencer take
it off the ROM-measured handler costs (`ram_bytes()` in `cmd_cycles`,
`ply_note` and the command fetch).  Handlers costed for the first time:
`ply_xcmd_0D` 120, `ply_xrele` 48, `ply_xwait` 108 / 102 (expiry) with the
sub-command dispatch 54 (RAM; ROM est. + 7 per byte, unreachable by any
vanilla song), `ply_endtie` 54 with no key byte and an empty chain -- which
splits step 3's 89 / 95 into `M4A_CYC_ENDTIE` 61 (ROM est. 54 + 7) + 28 for
the chained channel that holds the key (`_MATCH`) + 6 for a key byte
(`_KEY`), with 18 est. for a chained channel passed over (`_SKIP`); the walk
is now costed from the chain state as m4a_1.s walks it, and rg_vs_trainer's
4262 writes stayed at 0.  **ply_tune** is 84 from RAM, so its ROM cost is
**91** (est. 84 + 7) where step 3 used the 90 default: tune_psg's 1-cycle
write residual, excused since the timing model landed, is 0.

**Per-cry sequencer residual**: `entry_diff.py` 0 on all 177 frames of
each of the four (the probe's incomplete frame 179 excepted), `cmd_diff.py`
0 command-position divergences (the xcmd sub-handlers added to its filter
list), `track_diff.py` 0, on each; cries have no PSG writes, so no write
diff.  Before the TUNE correction the start frame was -1 from GOTO on;
the player position matched on every frame from the first render.

**Verified against the ROM**: `gPokemonCrySongTemplate` at 0x08002F20
(`01 00 FF 00 | 8C 2F 00 08 | 0 x 8 | 00 | C8 40 | B2 0 | C8 50 | BD 00 |
BE 7F | CD 0D 0 | CD 07 00 | BF 40 | CF 3C 7F | CD 0C 3C 00 | CE B1`) equals
`gM4ACrySongTemplate` byte for byte (unit test, the voicegroup_dummy word
zeroed); the setters applied by hand give `test_cry_song`'s expected
bytes (priority 0A, VOL 78, PAN 40, TIE 3C 7F with TUNE 40 / 50, xWAIT 8C
00), and `m4a_song_relocate` at 0x03001F20 with gCryTable[0] 0x0802AF7C
gives the pointer words `SetPokemonCryTone` stores.  Every gCryTable entry
is `20 3C 00 00 <wav> FF 00 FF 00`; every cry sample is type 1, status 0
(none loops), 10512 Hz, 1902..23524 samples: no cry is cut by the 140-tick
EOT at pitch 15360 (the task's "one cut by the length" does not exist in
vanilla Emerald; a pitch variant would make one).

The work, in order (commits on `accuracy-harness` after 884be8e): the
probe section (step 5); the engine (`m4a_cry.c`, the players, the RAM
costs, `voicegroup_load_cry` / `voicegroup_cry_table_symbol`, `--cry`,
unit tests `test_cry_song`, the players test's six players and error
return, `test_sequencer_cycles_cry`: cry_pidgey's frames 3..145 through
the real mixer with a stand-in DPCM sample of its length and rate, the
channel dying on frame 14 as in mgba); the corpus, runner, `claim_check.sh`
cry case, README and skill; this section and the baseline.

**Code review of the cry slice (2026-10-24).**  Two independent reviews
(the template, setters, player selection and chain position against m4a.c
`SetPokemonCryTone` / `SetPokemonCry*` / `m4aSoundInit`, m4a_tables.c and
m4a_1.s, decoding the raw cry probe logs themselves; the runner, songs.json
plumbing, `--cry`, `claim_check.sh`, the loader and the template / ToneData
/ sample bytes against the ROM), then `tools/claim_check.sh` on each
adopted change alone.  Neither found a divergence at the defaults.  Byte
checks: gCryTable[15 / 0 / 381 / 123] are `20 3C 00 00 <wav> FF 00 FF 00`
with the map's Cry_Pidgey / Bulbasaur / Wailord / Jynx pointers; the four
`cries/*.bin` files are byte-identical to the ROM at those pointers (header
and data), the .bin newer than the .wav so the loader takes it and keeps
the CMP bit; `gPokemonCrySongTemplate` at 0x08002F20 equals
`gM4ACrySongTemplate` but for the voicegroup_dummy word; the setters
hand-applied with mgba_render's defaults give `test_cry_song`'s bytes;
`voicegroup_cry_table_symbol` walks the 388 `cry` lines in the ROM's order
and stops at `gCryTable_Reverse::`.  Confirmed from the logs: SE3's
`_081DDA6C` 826, FadeOutBody +88, `_081DD9BC` 968, exit 63 + 55 on every
frame; frames 3 / 4 / 14 / 143 / 144 / 145 segment by segment against the
port's charges (item 7 of review 1); the byte-read counts of every handler
(`ldrb` / `READ_XCMD_BYTE`) behind `ram_bytes()`, ply_note's 1 / 2 / 3
test reads, gXcmdTable's indices, ply_endtie's walk conditions.
**Adopted** (R1#6, latent divergence, harness-neutral by construction --
no vanilla song or default cry uses it; gated by `test_sequencer_xrele`):
`XCMD xRELE` now stores the track tone's release as `ply_xrele` does (the
port skipped the byte, so a cry with `SetPokemonCryRelease` != 0, the
game's other cry modes, or a song using xRELE would have cut the note at
EOT instead of releasing it); the parser learned the MPlayDef.s xcmd
names (`xWAVE` .. `xWAIT`).  claim_check: 0 moved writes, every row 0.0.
**Adopted, cosmetic** (one claim_check for the batch, every row 0.0):
`--cry` rejects non-numeric ids (was atoi: `abc` played Bulbasaur, R2#6);
the FILE leak on a negative index in `voicegroup_cry_table_symbol` (both);
`ram_bytes` charged on the `_081DD8E0` segment that holds the ldrb
(`M4A_CYC_CMD_BYTE` / `_RUNNING`) rather than the fetch before it (R1#5a;
same sum); xcmd sub-commands 0x00 / 0x03 (`ply_xxx` = ply_fine) no longer
charged XIEC on top of ply_fine's own cost (R1#5d, unreachable); the
142-tick comment in `test_sequencer_cycles_cry` (R1#7).  **Not acted on**:
`XCMD 0x0D` (`unk_3C`, the sample start offset `ply_note` copies into the
channel's count: R1#6) -- the port skips it, so `M4ACryParams.progress`
!= 0 would start the sample at 0; every `PlayCryInternal` mode passes 0
and the test ROM too, so it is harness-neutral today and belongs to the
variants slice with the mixer's SF_START offset; the game's persistent
`gPokemonCrySong` (a second cry after a chorus cry sees a different
`tuneValue2 - tuneValue`; unused bytes at trackCount 1, and the harness
starts one cry from reset: R1#2, R2#2); the reference render done before
the `--input midi` skip and the skipped test absent from the report (R2#4);
the probes' `.frames` files hold 179 `frame` lines for a 180-frame run
(the docs say 180 frames, the probe's argument: R2#5, #8); the 2 s windows
of cry_pidgey / cry_bulbasaur end before the EOT tick (the EOT finds an
empty chain on every cry; wailord and jynx run 3 s and cover it: R2#3).
Baseline refreshed from the cosmetic batch's claim_check run: identical
to the one above.

Not in this slice (next): the pitch / pan / volume / length variants
(`PlayCryInternal`'s other modes, `--pitch` etc. on both sides),
`CRY_MODE_*` with chorus (two tracks) and the reverse cries
(`gCryTable_Reverse`, `TONEDATA_TYPE_REV` on DPCM), a cry-sample DPCM
audit against the ROM bytes (the four corpus .bin files already match the
ROM byte for byte), the `XCMD 0x0D` start offset, a second cry while one plays (cry 1's
position, `SetPokemonCryTone`'s clock rule), and a cry over music.

### Done: milestone 4 slice 3, cry variants (2026-10-24)

Three cries with `PlayCryInternal`'s non-default pitch / pan / volume join
the corpus (`songs.json` cry entries take optional `"pitch"`, `"pan"`,
`"volume"` keys, `SetPokemonCryPitch` / `Panpot` / `Volume`'s arguments;
the runner passes them to `mgba_render --pitch/--pan/--volume` and to
`poryaaaa_render --cry-pitch/--cry-pan/--cry-volume`, `claim_check.sh`
likewise; `mgba_probe` takes them as its ninth to eleventh arguments).
The values are the game's (`src/sound.c`): 14440 is CRY_MODE_FAINT's
pitch, 15800 CRY_MODE_HIGH_PITCH's, 90 CRY_MODE_ENCOUNTER / HIGH_PITCH's
volume; pan 63 is the opponent-side value `battle_anim_throw.c` uses (for
an SE; the cry callers pass -25 / 25 / 63 / -63).  Chorus, length, release
and the reverse cries stay out (the next slices), so each variant takes one
parameter path the defaults never took.  Baseline
`baselines/2026-10-24_cry-variants.json` / `.txt`: **46/51 pass at -40 dB**;
the 48 previous tests are unchanged to the decimal (`compare_reports.py
2026-10-24_cry.json 2026-10-24_cry-variants.json`: 0 regressions, 0
improvements, every -inf still -inf, every row 0.0; cry_pidgey, cry_jynx
and se_rain_stop FAIL on the documented floor as before).
`poryaaaa_unit_tests` 1221/1221 (46 new), `poryaaaa_blip_tests` 49112/0,
`poryaaaa_song_tests` 561/561.  `claim_check.sh`: 44 probed tests (the
three variants added), 0 moved writes.

| test | gCryTable | pitch / pan / volume | sample | null | worst 1 s | ref dBFS |
|---|---|---|---|---|---|---|
| cry_jynx_faint | 123 | 14440 / 0 / 120 | 23524 samples, 2.75 s at this pitch: cut by the EOT at 2.38 s | -61.2 | -56.1 | -25.7 |
| cry_bulbasaur_howl | 0 | 15800 / 63 / 120 | 8183 samples, 0.72 s; PAN 0x7F, the left side exactly silent | -47.9 | -39.4 (FAIL) | -25.4 |
| cry_pidgey_quiet | 15 | 15360 / 0 / 90 | 1902 samples, 0.18 s; VOL 0x5A | -43.1 | -38.5 (FAIL) | -36.0 |

**Measured constants** (probe section, step 6): `ply_endtie` with the
first chained channel holding the key is **82** from RAM against the empty
chain's 54, so `M4A_CYC_ENDTIE_MATCH` is **28 measured** where slice 2 had
derived it as the ROM 89 minus the RAM 61; the harness needed no new
number (the port reproduced cry_jynx_faint's frame 143 at 1928 = 1810 + 20
+ 28 + 70 from its existing constants, the +70 being step 3's ply_fine
with one live channel).  `_SKIP` 18 stays est. (no run puts two channels on
a cry track; `test_sequencer_endtie_walk` pins the model's relative
charges).  The template bytes of each variant (the step-6 table) were
worked by hand from the setters before the first render and are confirmed
by the probe's track key after the EOT (0x38 / 0x3E / 0x3C) and by
`test_cry_song`.

**Per-test sequencer residual**: `entry_diff.py` 0 on all 177 complete
frames of each of the three, `cmd_diff.py` 0, `track_diff.py` 0 (the
probe's incomplete frame 179 excepted), including cry_jynx_faint's EOT
with the chained channel and its FINE unlinking a live channel.

**Residual attribution** (skill, "Attributing a residual below -40 dB";
every run of differing output samples above 20 int16 LSB against the
probe's `L timer0 ... slot` crossings): cry_jynx_faint 1 crossing (cycle
28146944, 1.6777 s), 1 run at 1.6779 s, one output sample, 2.9 PCM LSB,
on both sides; cry_pidgey_quiet 1 crossing (3428099, 0.2043 s, as
cry_pidgey), 1 run at 0.2045 s, 5.9 LSB; cry_bulbasaur_howl 6 crossings
(0.3383 .. 0.7401 s), 6 runs on the right side each 0.2 ms after a
crossing (2.9 / 1.8 / 1.0 / 1.6 / 15.4 / 0.6 LSB; the 15.4-LSB one at
0.6733 s alone puts the worst second at -39.4) and none on the left,
whose reference and render are both exactly zero (PAN 0x7F gives the left
volume 0).  No run anywhere else: the release-0 cut at cry_jynx_faint's
EOT (SF_STOP on frame 143, the channel gone on 144) differs by nothing,
the pitch and pan arithmetic by nothing.  The two FAILs are item 1c's
scheduler-lateness floor, as cry_pidgey's and cry_jynx's; they stay in the
corpus for the same reason.  cry_jynx_faint's -61 is the deepest cry: the
lower pitch halves the per-output-sample jumps a late slot exposes, and
its one crossing falls on a quiet stretch.

**XCMD 0x0D** (`unk_3C`, the sample start offset; slice 2's review item
R1#6) is ported: `ply_xcmd_0D` stores the little-endian word in the
track (`M4ATrack.sampleStart`), `ply_note`'s DirectSound path hands it to
the channel as its count (`_081DDCCE`) and SoundMainRAM's SF_START pass
turns it into `currentPointer = data + count`, `count = size - count`
(`m4a_pcm_channel_start_at`, applied at note-on since nothing on the GBA
reads either field before that frame); the compressed path reads the
pointer back as the sample index, so an offset inside a DPCM block decodes
that block and starts mid-way; the track's start tick clears it
(Clear64byte covers 0x3C).  Harness-neutral by construction (every
`PlayCryInternal` mode and the test ROM pass progress 0): gated by
`test_pcm_start_offset` (an uncompressed FIX ramp at offset 3; DPCM at a
block-aligned 64 and a non-aligned 37 against `make_dpcm_wave`'s decoded
array, the channel stopping after size - offset samples) and
`test_sequencer_xcmd_0D` (the word through the parser and the sequencer
into the channel, persistence across notes, the reset at the track's
start).  `--cry-progress` reaches `M4ACryParams.progress` from the CLI
(four lines; no reference can check it since the ROM's StartCry passes 0).

**Code review of the variants slice (2026-10-24).**  Two independent
reviews (the setters' arithmetic for the three value sets, ply_endtie's
walk conditions and cost, the 0x0D port against m4a_1.s ply_note /
SoundMainRAM / Unk1 / Unk2 and m4a.c, decoding the probe logs themselves;
the probe and renderer argument plumbing against struct AccParams,
songs.json's keys through the runner and claim_check, the template bytes
against the ROM at 0x08002F20 for each variant), then `claim_check.sh` on
the adopted batch.  Neither found a divergence: both confirm the five
bytes per variant from the ROM template, the 54 / 82 endtie stamps and
the +70 FINE / +20 walk that make 1928, the LE word and cmdPtr += 4 of
ply_xcmd_0D, the SF_START conversion for uncompressed / DPCM / reversed
samples (Unk1 reading the pointer back as the index, so an in-block offset
decodes its block), that nothing on the GBA reads `count` between ply_note
and the pass, that `unk_3C` is inside Clear64byte's 0x00..0x3F and written
nowhere else, that `midiKey` is the raw track key, and that the probe logs
show the patched values (`vol=5A`, `pan=3F`, MidiKeyToFreq 8746 / 11426 /
10512).  **Adopted, cosmetic** (one claim_check for the batch): an offset
past the sample is clamped to its end in `m4a_pcm_channel_start_at` (the
GBA reads past the data before its signed count check stops it; the port
would have read host memory; `--cry-progress` is the only way to reach it,
unit-tested); `claim_check.sh` skips a cry entry without an `"id"` as the
runner does instead of dropping every cry; `poryaaaa_render` notes when a
`--cry-*` flag is given without `--cry`; the runner's inline comment.
**Not acted on**: `_SKIP` 18 stays est. (a hand count of the pass-over
path gives 18..24; only a two-note cry probe can settle it, and no game
cry has two channels on one track); the persistent `gPokemonCrySong`'s
`a = tuneValue2 - tuneValue` after a chorus cry (slice 2's R1#2; it can
never reach a one-track cry's output); the probe's raw `fN` stamps are one
ahead of the `.frames` rows and the plan's frame numbers (the EOT frame
143 is `f144` in the raw log); the summary line prints the raw
`--cry-volume` before the setter's `& 0x7F`.  Baseline refreshed from the
batch's claim_check run.

The work, in order (commits on `accuracy-harness` after d2a7086): the
probe arguments and step 6; the engine (0x0D, the CLI flags, the unit
tests, `test_sequencer_cycles_cry_faint`: Cry_Jynx's length and rate at
pitch 14440 through the real mixer, frames 3..145 against the probe with
the channel alive at the EOT); the corpus, runner, `claim_check.sh`,
README and skill; this section and the baseline.

### Done: milestone 4 slice 4, cry length / release modes (2026-10-24)

Three cries with `PlayCryInternal`'s whole modes join the corpus: pitch,
length and release together as `src/sound.c` sets them (`songs.json` cry
entries take optional `"length"` / `"release"` keys, `SetPokemonCryLength`
/ `SetPokemonCryRelease`'s arguments, forwarded like pitch / pan / volume:
`mgba_render --length/--release`, `poryaaaa_render --cry-length/
--cry-release`, `mgba_probe`'s twelfth / thirteenth arguments,
`claim_check.sh` likewise).  None of the three modes sets chorus, so no
value is left out; chorus (two tracks), the reverse cries, a second cry
while one plays and cries over music stay for later slices.  The test
ROM's `StartCry` takes length and release from `struct AccParams` (the
probe section, step 7: the block's reserved bytes, defaults 140 / 0 in the
ROM data, the +0x30 symbol shift, the 51 existing rows identical under the
rebuilt ROM before any engine change).  No engine code changed in this
slice: the setters (`m4a_cry.c`), `ply_xrele`, the PCM release step and
`ply_fine`'s unlink of a releasing channel were already ported; the slice
exercises them against the ROM for the first time and gates them.
Baseline `baselines/2026-10-24_cry-modes.json` / `.txt`: **49/54 pass at
-40 dB**; the 51 previous rows are unchanged to the decimal
(`compare_reports.py 2026-10-24_cry-variants.json 2026-10-24_cry-modes.json`:
0 regressions, 0 improvements, every -inf still -inf, every row 0.0;
cry_pidgey, cry_jynx, se_rain_stop, cry_bulbasaur_howl and cry_pidgey_quiet
FAIL on the documented floor as before).  `poryaaaa_unit_tests`
1270/1270 (49 new, the review's gate test included), `poryaaaa_blip_tests` 49112/0, `poryaaaa_song_tests`
561/561.  `claim_check.sh`: 47 probed tests (the three new ones added), 0
moved writes.

| test | gCryTable | mode (pitch / length / release) | sample | null | worst 1 s | ref dBFS |
|---|---|---|---|---|---|---|
| cry_jynx_faint_full | 123 | CRY_MODE_FAINT: 14440 / 140 / 200 | 23524 samples, 2.75 s at this pitch: released by the EOT at frame 143, 18 release passes, the channel stopped on 161 | -61.0 | -56.1 | -25.7 |
| cry_wailord_doubles | 381 | CRY_MODE_DOUBLES: 15360 / 20 / 225 | 20846 samples, 1.98 s: released by the EOT at frame 23, 30 release passes, stopped on 53 | -47.3 | -45.9 | -29.4 |
| cry_pidgey_roar2 | 15 | CRY_MODE_ROAR_2: 15616 / 60 / 225 | 1902 samples at key 61, 0.17 s: gone at frame 14, the EOT at 63 finds an empty chain | -50.1 | -49.8 | -33.6 |

**The channel's death frames, probe against engine**: the probe's new
`_081DCFB0` stamp (SoundMainRAM's `statusFlags = 0`, broken at the IWRAM
copy since the ROM label never executes) puts cry_jynx_faint_full's death
on **frame 161** and cry_wailord_doubles' on **frame 53**; the engine's
channel loses CHN_ON on the same frames (`test_sequencer_cycles_cry_faint_full`
asserts 161 with the envelope 199, 155, 121, 94, 73, 57, 44, 34, 26, 20,
15, 11, 8, 6, 4, 3, 2, 1 on frames 143..160; the Wailord run's 53 follows
from the same arithmetic at release 225, 224 .. 1 on 23..52, which
`test_pcm_release_envelope` pins on a hand-built channel).  The last
non-zero output sample of reference and render agrees to the sample on
all three (cry_jynx_faint_full 2.7159 s, cry_wailord_doubles 0.9110 s,
cry_pidgey_roar2 0.2930 s).  The `ply_endtie`-with-match frame reads
**82** from the probe on both (a) and (b), as step 6 measured; (c)'s
empty chain reads 54.  No run put two channels on the cry track, so
`_SKIP` 18 stays est.  The "1689-type unlink frame" the task names does
not exist for (a) and (b): `ply_fine` unlinks the releasing channel on the
EOT frame itself (the track is gone), and the death is SoundMainRAM's
alone -- hence the new breakpoint.

**Per-test sequencer residual**: `entry_diff.py` 0 on all 177 complete
frames of each of the three (the probe's incomplete frame 179 excepted),
`cmd_diff.py` 0, `track_diff.py` 0, with no new constant: the EOT frames
are 1928 (a, b) and 1810 (c), the xwait ticks 1596 / 1576, the start
tick 4077, all from the existing model.

**Residual attribution** (skill, "Attributing a residual below -40 dB";
every run of differing output samples above 20 int16 LSB, both sides,
against the probe's `L timer0` slot crossings, raw `fN` = frame N-1):
cry_jynx_faint_full 4 crossings (1.6777, 2.4813, 2.5483, 2.6823 s), 2
runs: 1.6779 s (frame 100, 0.3 PCM samples, 2.9 LSB) and 2.4815 s (frame
148, in the release tail at env 44, 0.6 LSB), each 0.2 ms after a
crossing; the crossings at 2.5483 / 2.6823 s fall at env 11 and 2 and
produce no run above the threshold.  cry_wailord_doubles 6 crossings
(0.2043, 0.2713, 0.3383, 0.4052, 0.5392, 0.6062 s), 4 runs: 0.2045 (7.3
LSB), 0.2715 (2.0), 0.3385 (4.4 right / 3.7 left), 0.4054 s (2.4, frame
24, the first release pass at env 224), each 0.2 ms after a crossing; the
two later crossings fall at env 15 and 5 and produce none.  cry_pidgey_roar2
1 crossing (0.1374 s), 1 run at 0.1376 s (3.2 LSB).  **No run anywhere
else**: nothing at a release pass that is not at a slot crossing, nothing
at either channel's death, nothing at (c)'s EOT -- the release arithmetic
differs by nothing; all three residuals are item 1c's scheduler-lateness
floor, large per sample because the DPCM cry waveforms jump.  (a) is as
deep as cry_jynx_faint (-61.2) because its one loud crossing is the same
one; (b) and (c) pass where cry_pidgey / cry_jynx fail because their
crossings land on quieter stretches (cry_pidgey_roar2's single crossing is
at 0.137 s against cry_pidgey's at 0.204 s: the key-61 sample has moved on
by then).

**Code review of the length / release slice (2026-10-24).**  Two
independent reviews (reviewer 1: SoundMainRAM's SF_STOP release step and
stop pass against `m4a_pcm_channel_tick` / `m4a_sound_main_ram`, the
volume products' multiplier (SoundInfo byte 7 = masterVolume 12 via the
`o_SoundChannel_release` offset alias), `ply_endtie`'s conditions,
`ply_fine` / TrackStop on a releasing channel, the tick walk's ON test,
the setters and `ply_xrele` / `ply_xwait`, `ply_note`'s release copy, the
unit tests' sequences against the source arithmetic and the probe stamps;
reviewer 2: `struct AccParams` against crt0's block, both patchers and
the ROM bytes (`StartTest`'s `ldrh [r5,#16]` / `ldrb [r5,#18]`), the
rebuild's byte-level effect (every ROM symbol from AgbMain on +0x30,
StartTest +0x24, 17943 differing bytes all ROM pointers +0x30 plus the one
`bl StartTest` offset; IWRAM / EWRAM identical; the cry samples and the
template byte-identical), the 14 CgbSound labels each on the instruction
its name describes, the `_081DCFB0` relocation, the runner / claim_check
keys, the template bytes of the three entries from the ROM template).
Both confirm everything the slice relies on; the release envelope and the
stop pass match to the instruction.  **One divergence, adopted,
harness-neutral** (`claim_check.sh` against `2026-10-24_cry-modes.json`
with the fix alone: the 54 rows identical in every field, 47 diffed with 0
moved writes, unit 1270/1270): `ply_fine`'s RealClearChain nulls
`channel->track` (m4a_1.s `str r0, [r4, o_SoundChannel_track]` with r0 =
0) while the port kept `trackIndex`, so a channel still releasing after
FINE stayed selectable by the same track index's later VOLCHG / PITCHG
pass (`apply_track_volumes` / `refresh_channel_pitches`), by a later
`ply_fine` on that index, and kept its track rank in `ply_note`'s
equal-priority tie-break where the GBA's null pointer ranks lowest.  The
game case is a cry with a release tail followed by another cry on the same
player, or any song started after a FINE with a tail; no corpus test
restarts a track inside a tail, hence neutral.  Fixed in `ply_fine`
(`chan_clear_track` -> `M4A_NO_TRACK` = -1 on each chained channel, the
SF_STOP flagging folded into the same chain walk as `ply_fine_loop`),
gated by `test_sequencer_fine_orphan` (a releasing square after FINE has
no track; a VOL 10 / PAN song restarted on the same index leaves its
volumes alone; the test fails on the old code in three places).
**Cosmetic, adopted**: crt0.s's "32 bytes" comment (the block is and was
28 bytes; the patchers' 32 is a bounds check).  **Not acted on**:
`mgba_render` / `mgba_probe` take the cry arguments with `atoi` and no
range check (pre-existing style, they feed the ROM's u16 / u8 directly);
the port's XCMD xTYPE / xATTA / xDECA / xSUST / xLENG / xSWEE sub-commands
skip their operand where the source writes `track->tone` fields, and
sub-commands >= 0x0E map to `ply_fine` where the GBA calls past the table
-- no vanilla song or cry template uses them (a slice of its own if one
turns up); the GUI's channel label prints `trackIndex + 1`, so an orphan
reads "T0" (and a poly event recorded from one 255 -> guarded by `<
MAX_TRACKS`); the plan's step-6 "ninth / tenth / eleventh" wording stays
as history.  Baseline refreshed from the fix's claim_check run (identical
to the first).

The work, in order (commits on `accuracy-harness` after 3a7f8b3): the ROM
/ probe / mgba_render and the neutrality check (step 7); the CLI flags,
runner, corpus, unit tests, README and skill; the baseline and this
section; the review's fix, its gate test, the paragraph above and the
refreshed baseline.

### Done: milestone 4 slice 5, reverse cries (2026-10-25)

The reversed cry table entries join the corpus: `gCryTable_Reverse`'s
`cry_reverse` voices (ToneData type 0x30 = CMP | REV, the same DPCM sample
played backwards through SoundMainRAM_Unk1's `_081DD3C0` walk), one track
each.  Chorus (two tracks), a second cry while one plays and cries over
music stay for later slices.  **No ROM change** and **no mixer change**:
the slice fixes the loader (the by-index lookup stopped at the
`gCryTable_Reverse::` label, so no id past 387 resolved; probe step 8
above) and gates the already-ported reversed DPCM walk against the ROM for
the first time.  Baseline `baselines/2026-10-25_cry-reverse.json` / `.txt`:
**50/57 pass at -40 dB**; the 54 previous rows are unchanged to the decimal
(`compare_reports.py 2026-10-24_cry-modes.json 2026-10-25_cry-reverse.json`:
0 regressions, 0 improvements, every -inf still -inf, every row 0.0; the
five documented floor FAILs as before).  `poryaaaa_unit_tests` 1328/1328
(58 new), `poryaaaa_blip_tests` 49112/0, `poryaaaa_song_tests` 561/561.
`claim_check.sh`: 50 probed tests (the three new ones added), 0 moved
writes.

| test | gCryTable | mode | sample | null | worst 1 s | ref dBFS | |
|---|---|---|---|---|---|---|---|
| cry_bulbasaur_reverse | 388 (= gCryTable_Reverse[0]) | CRY_MODE_NORMAL: 15360 / 140 / 0 | 8183 samples backwards from block 127, 0.78 s: run out through the count path, the unlink tick on frame 50, the EOT at 143 on an empty chain | -46.5 | -33.6 | -28.1 | FAIL |
| cry_jynx_growl1 | 511 (= gCryTable_Reverse[123]) | CRY_MODE_GROWL_1: 15200 / 15 / 125 | 23524 samples backwards from block 367 (about 10.36 kHz): the EOT at frame 18 releases the live reversed channel, 6 release passes 124 .. 2, the channel stopped on 24 | -62.4 | -62.4 | -36.8 | PASS |
| cry_pidgey_echo_start | 403 (= gCryTable_Reverse[15]) | CRY_MODE_ECHO_START: 15600 / 25 / 100, volume 90 (chorus 192 not patchable, 0) | 1902 samples backwards, 0.17 s: gone by frame 14, the EOT at 28 finds an empty chain | -34.9 | -26.4 | -36.2 | FAIL |

**The three gCryTable indices and their ROM bytes** (probe step 8's table):
388 `30 3c 00 00 bc 39 0d 08 ff 00 ff 00`, 403 `30 3c 00 00 f0 30 0e 08 ff
00 ff 00`, 511 `30 3c 00 00 e8 77 15 08 ff 00 ff 00` -- type 0x30, key 60,
pan 0, the forward entries' wav pointers (0x080d39bc / 0x080e30f0 /
0x081577e8), envelope 255 / 0 / 255 / 0; `voicegroup_cry_table_entry`
returns the same type for them and `poryaaaa_render --cry` prints them as
reversed (`test_cry_table_index` pins the vanilla 388 / 403 / 511 / 123 and
the 776-entry end).

**The channel's death, probe against engine**: cry_jynx_growl1's `_081DCFB0`
stamp is raw f25 = **frame 24**; the engine's channel loses CHN_ON on frame
24 (`test_sequencer_cycles_cry_growl1_reverse`: the envelope 124, 60, 29,
14, 6, 2 on frames 18..23 from `255 * 125 >> 8` per pass, the stop on the
pass whose product is 0, the channel of type 0x30 with CHN_SPECIAL set and
its sample index walking down from 23524).  The two count-path cries have
no stamp: their unlink ticks (1689) are on frames 50 and 14 in the probe,
and the engine's last non-zero output agrees with the reference's to the
sample on all three (0.9254 / 0.4548 / 0.3150 s against 0.9255 / 0.4548 /
0.3153 s; the differences at the end of (a) and (c) are the run below,
not a length).

**Per-test sequencer residual**: `entry_diff.py` 0 on all 177 complete
frames of each of the three, `cmd_diff.py` 0, `track_diff.py` 0, no new
constant: the frames are slice 4's (4077, 1596, 1689 / 1576, 1928 or 1810
at the EOT, 1192, 1007).  The reversed walk is SoundMainRAM's cost, not
MPlayMain's.

**Residual attribution** (skill, "Attributing a residual below -40 dB";
every run of differing output samples above 20 int16 LSB, both sides):
cry_jynx_growl1 (4 slot crossings at 0.0704, 0.1374, 0.2043, 0.3383 s): one
run, 0.1376 s, 0.6 PCM samples, 104 LSB (0.5 int8), 0.2 ms after the second
crossing; the other three crossings produce nothing above the threshold
(the first is before the sample is loud, the last two in the release tail
at env 14 and 2) -- the floor, item 1c.  **cry_bulbasaur_reverse and
cry_pidgey_echo_start have 0 slot crossings and exactly one run each**:
0.8488 s / 0.2424 s, 1.4 PCM samples long (one PCM sample plus the
resampler's ringing), 959 / 1535 LSB (5 / 8 int8 units), at the **end of
the reversed play** -- the forward sample's first sample -- and nothing at
the start (no 64-sample run anywhere: the reversed decode and the block
walk differ by nothing).  This is the look-behind of sample 0 recorded in
probe step 8 before the first render: the GBA decodes "block -1" from open
bus (mgba `GBALoadBad`, the IWRAM Unk2 prefetch word; the 33 bytes `e4 01
00 96 e1 d0 ...` decode to **38** at block offset 63) where the port's
`unk2` decodes a zero block (0); the one output between the last two
crossings interpolates `d[0] + ((r9 * (lookbehind - d[0])) >> 23)` toward
38 on the GBA and toward 0 here.  **Measured, not only derived**: a
throwaway build whose out-of-range block in `unk2` holds those 33 bytes
renders cry_bulbasaur_reverse and cry_pidgey_echo_start at **-inf** (bit
exact) with cry_bulbasaur and cry_pidgey unchanged; the build was reverted
and the mixer is not changed in this slice (the task's rule: a reversed
cry above the floor is attributed and reported before any mixer change;
emulating the open-bus look-behind is a 33-byte constant keyed to the
sample pointer's byte lane -- all three cry samples are lane 3, other lanes
give other bytes -- and the GBA's prefetch model, i.e. its own change with
its own review).  It is why (a) and (c) FAIL: their worst 1 s window holds
that single sample on a quiet file; (b)'s release stops the channel long
before sample 0 and never reaches it.  Against the forward twins: cry_bulbasaur (-54.9) and
cry_pidgey (-43.0) carry only slot-crossing runs and no end run, because
the forward walk's look-ahead at index `size` reads the extra block the
converter emits (milestone 3), which exists in the ROM; the reversed
walk's symmetric read at index -1 has no such block.

The work, in order (commits on `accuracy-harness` after aeb01d6): the
loader fix, its unit test and probe step 8; the corpus, the unit tests
(the variants rows, `test_pcm_dpcm_reverse_walk`,
`test_sequencer_cycles_cry_growl1_reverse`), README and skill; the
baseline and this section; then the review paragraph below.

**Code review of the reverse-cry slice (2026-10-25).**  Two independent
reviews (reviewer 1: SoundMainRAM_Unk1's SF_SPECIAL flip and CMP index
conversion, the `_081DD3C0` reversed walk crossing by crossing (lr == 1 and
lr > 1, the `bic`, the signed `ble`, the word cadence, the `add r3, 2`
exit, the `_081DD4F4` partial-word stop), Unk2's decoder and block address,
the shared sDecodingBuffer across channel orders, the FIX step, the
release / IEC / stop path's type-independence, ply_note's fields and the
SF_START placement, every unit-test expectation re-derived from the
source arithmetic; reviewer 2: the table lookup line by line against the
781-line cry_tables.inc, the macros' bytes, the six ROM entries read
again byte for byte plus the last pair (387 / 775) and the data after
776, StartCry's `ldrh` index and `idx * 12` from the disassembly, the
AccParams layout against crt0 and both patchers' bytes (id 511 as a
u16), the three entries' template bytes by hand and the modes' fields
against sound.c, the runner / claim_check plumbing, a grep for stale
type-0x20 or index-below-388 assumptions, the docs against the sources).
Everything the slice relies on is confirmed.  **One divergence, both
reviews independently, not adopted here**: the look-behind of sample 0
at the end of a completed reverse walk (Unk2 with r3 = -1, block
0x3FFFFFF at `wav + 0x83FFFFEF`, open bus) decodes to a non-zero value on
the GBA and to 0 in the port -- the run attributed and measured above
(-inf with mgba's 33 bytes).  **Decided 2026-10-25: not wanted** -- the
port keeps the zero block; cry_bulbasaur_reverse and cry_pidgey_echo_start
stay FAIL on this documented single sample, like the five floor FAILs, and
no later review should re-raise it.  **Pre-existing, unreached**: the
start-offset clamp for an offset past the sample end (slice 3's
host-memory guard; the GBA mixes bytes past the sample to the first
crossing, the port the extra block's; `SetPokemonCryProgress(0)` is the
only caller).  **Cosmetic, left**: the port presets `xpi` at note-on
where ply_note does not (Unk1 resets it on every call before any read);
the loader's `cry_uncomp` / `cry_reverse_uncomp` spellings are not in
vanilla music_voice.inc (eventide's; harmless).  **Cosmetic, adopted**:
"388 + species" read "388 + the forward index" (Bulbasaur's forward cry is
index 0, `SpeciesToCryId(species - 1)`) in the help text, README, skill
and plan; the step-8 sentence that said the ROM holds `size / 64 + 1`
full blocks (that is the loader's allocation; the ROM's last block is
partial, see above).  No code changed after the baseline, so
`2026-10-25_cry-reverse.json` stands as saved (its own `claim_check.sh`
run: 50/57, 50 diffed, 0 moved writes, 1328/1328, blip 0, songs 561/561).

### Done: milestone 4 slice 6, cry chorus, the two-track modes (2026-10-25)

`PlayCryInternal`'s chorus modes join the corpus: `SetPokemonCryChorus`
with a nonzero value makes the cry song **two tracks** (part0: TUNE, GOTO
cont; part1: TUNE2 = (chorus + TUNE) & 0x7F, falling into cont), and the
cry player's MPlayMain runs two tracks with `ply_note` twice on the start
tick for the first time.  A second cry while one plays and cries over
music stay for later slices.  **ROM change** (probe step 9 above): `struct
AccParams` gains `s8 chorus` at block offset 19, `StartCry` passes it to
`SetPokemonCryChorus` in `PlayCryInternal`'s order, both patchers write
byte 11 of the block, every ROM symbol after main.o moved +0x1c, the
CgbSound body unchanged, the 57 previous rows identical under the new ROM
before any engine change.  **No engine change**: the port's `m4a_cry.c`
already built the two-track song and the sequencer's per-track model
already reproduced the two-track frames; the slice plumbs the value
(`poryaaaa_render --cry-chorus`, `M4ACryParams.chorus`; songs.json
`"chorus"` through accuracy.py and claim_check.sh) and gates it against
the ROM.  Baseline `baselines/2026-10-25_cry-chorus.json` / `.txt`:
**52/60 pass at -40 dB**; the 57 previous rows are unchanged in every
report field but the ref path (`compare_reports.py 2026-10-25_cry-reverse.json
2026-10-25_cry-chorus.json`: 0 regressions, 0 improvements, every -inf
still -inf, every row 0.0; the seven documented FAILs as before).
`poryaaaa_unit_tests` 1373/1373 (45 new), `poryaaaa_blip_tests` 49112/0,
`poryaaaa_song_tests` 561/561.  `claim_check.sh`: 53 probed tests (the
three new ones added), 0 moved writes.

| test | gCryTable | mode | tracks / samples | null | worst 1 s | ref dBFS | |
|---|---|---|---|---|---|---|---|
| cry_bulbasaur_encounter | 0 | CRY_MODE_ENCOUNTER: pitch 15600, chorus 20, volume 90, release 225 (length 140) | two channels on Cry_Bulbasaur's 8183 samples at TUNE 56 / 76 (about 11.0 / 11.1 kHz): both run out through the count path (unlink ticks on 47 / 48), the EOT at 143 finds two empty chains | -46.5 | -43.6 | -28.8 | PASS |
| cry_jynx_echo_end | 123 | CRY_MODE_ECHO_END: pitch 15555, chorus 192, volume 70, release 220 (length 140) | two channels on Cry_Jynx's 23524 samples at TUNE 33 / 97 (the wrap: (192 + 33) & 0x7F; about 10.8 / 11.5 kHz, 2.2 / 2.0 s): both run out through the count path (unlinks on 126 / 133) before the EOT at 143 | -43.9 | -40.9 | -28.4 | PASS |
| cry_pidgey_echo_start_chorus | 403 (= gCryTable_Reverse[15]) | CRY_MODE_ECHO_START complete: reverse, length 25, release 100, pitch 15600, chorus 192, volume 90 | two reversed channels on Cry_Pidgey's 1902 samples at TUNE 56 / 120: gone on 13 / 14, the EOT at 28 finds two empty chains | -31.3 | -28.9 | -33.2 | FAIL |

**The template bytes** (probe step 9's table, worked by hand before the
first render and pinned by `test_cry_song`'s variants rows 10-12): trackCount
2 on all three; TIE key 0x3D, TUNE 0x38, TUNE2 0x4C, VOL 0x5A, xWAIT 0x8C
0x00, xRELE 0xE1 for (a); 0x3D / 0x21 / 0x61 / 0x46 / 0x8C 0x00 / 0xDC for
(b); 0x3D / 0x38 / 0x78 / 0x5A / 0x19 0x00 / 0x64 for (c).  The probe
confirms the TIE key on both tracks at each EOT (`_081DD938` r3 = 0x3D
after each of the two `ply_endtie`s, tracks 0x03002258 and 0x030022A8).

**Death frames, probe against engine.**  None of the three produces a
`_081DCFB0` stamp: every channel ends through the count path before its
track's EOT (Cry_Bulbasaur at 11 kHz lasts 0.74 s, Cry_Jynx at pitch 15555
2.0-2.2 s against the 2.33 s to tick 140, Cry_Pidgey 0.17 s), so the modes'
release values 225 / 220 / 100 reach no channel and there is no two-channel
release tail to compare -- the task's expectation for (a) and (b) does not
hold for these samples at these pitches, and the modes were kept whole
rather than altered.  The deaths are the unlink ticks: probe frames 47 / 48
(a), 126 / 133 (b), 13 / 14 (c), the faster (TUNE2) track first; the
engine's channels lose CHN_ON on the frame before each unlink (46 / 47 on
(a), asserted by `test_sequencer_cycles_cry_encounter_chorus`) and its
unlink ticks fall on the probe's frames (the 2025 / 2005 entries).  The
last non-zero output sample agrees with the reference's on all three
(0.8719 / 2.2991 / 0.3187 s against 0.8719 / 2.2991 / 0.3189 s; (c)'s 0.2
ms is the end run below).

**Per-test sequencer residual**: `entry_diff.py` 0 on all 177 complete
frames of each of the three, `cmd_diff.py` 0, `track_diff.py` 0, with no
engine edit and no new constant; the two-track skeleton (6792, 1932, 2025
/ 2005 / 1892, 2360, 1192, 1007) is what the existing per-track model
gives.  `_SKIP` 18 stays est. (each track owns a one-channel chain).

**Residual attribution** (skill, "Attributing a residual below -40 dB";
runs of output samples differing by more than 20 int16 LSB; the crossing
times from the probes' `L timer0` lines): cry_bulbasaur_encounter (5
crossings) has four runs, at 0.1714 / 0.2045 / 0.5394 / 0.6733 s, each
0.3-1.1 PCM samples long, 0.2 ms after the crossings at 0.1712 / 0.2043 /
0.5392 / 0.6731 s (the 0.6400 one produces nothing above the threshold) --
the floor.  cry_jynx_echo_end (10 crossings) has seven runs at 0.0706 /
0.2715 / 0.4724 / 0.9412 / 1.0751 / 1.2091 / 1.2761 s; the two that read
as 11 PCM samples long are each **two** single-sample runs 0.75 ms apart
merged by the run grouping (the crossing pairs 7922432 / 7934976 and
15787520 / 15800064, 12544 cycles apart: diff samples at 0.47242, 0.47244
and then 0.47317-0.47323 s) -- the floor; the 1.8786 crossing produces
nothing.  cry_pidgey_echo_start_chorus (3 crossings) has the three
crossing runs (0.1045 / 0.1714 / 0.2045 s, the first 2875 LSB because two
DPCM channels sum there) and **two end runs at 0.2327 s and 0.2424 s**, 1.4
PCM samples each (192 / 1534 LSB): the ends of the two reversed plays --
track 1 at TUNE 120 reaches sample 0 first, track 0 at TUNE 56 at the same
0.2424 s as slice 5's one-track entry -- i.e. the open-bus look-behind of
sample 0 decided not wanted in slice 5's review, now once per track.  It
is why (c) FAILs, as cry_pidgey_echo_start does; it stays documented, no
mixer change.

The work, in order (commits on `accuracy-harness` after 385f4c7): the
ROM / probe / mgba_render change with the symbol shift and neutrality; the
CLI, runner, corpus, unit tests, README, skill and probe step 9; the
baseline and this section; then the review paragraph below.

**Code review of the cry-chorus slice (2026-10-25).**  Two independent
reviews (reviewer 1: `SetPokemonCryChorus` against m4a_cry.c -- trackCount,
the TUNE2 overwrite, the s8 arithmetic for 192 and the `& 0x7F`
equivalence of 192 and -64, the order after `SetPokemonCryPitch` and its
discarded carry, the pointer words and relocations; `MPlayStart`'s
`min(songHeader->trackCount, mplayInfo->trackCount)` with the cry players'
MPlayOpen(2) and the clearing loop; MPlayMain's loop over two tracks with
two `ply_note`s on the start tick (track 1 taking chans[1] because the
search skips track 0's channel, as the port's first-free allocation does),
`ply_tune`'s signed tune and its path through TrkVolPitSet / MidiKeyToFreq
on both tracks, the per-track dead-channel unlink on consecutive frames,
`ply_endtie` / `ply_fine` per track; the two-part template layout and the
GOTO; every unit-test expectation re-derived; the chorus-0 path untouched.
Reviewer 2: the AccParams layout offset by offset with the ABI alignment
(no padding, 28 bytes, chorus at 19), the ROM's bytes at 0xC0 by xxd, both
patchers' byte 11 and argument indices, StartCry's order and the u32 ->
s8 conversion against PlayCryInternal, the full before / after symbol
tables (243296 lines each; +0x14 for StartTest, +0x1c for every ROM
symbol from AgbMain on, 0 for all 24 IWRAM and both EWRAM symbols), each
of the 14 `CgbSound_*` labels on its instruction, the template bytes by
hand, the modes against sound.c, the ids against cry_tables.inc, the
runner / claim_check keys, the CLI range and cast, the effect on chorus-0
inputs).  **No divergence** in either.  **Cosmetic, adopted** (comments
and notes only, no code or input changed; `claim_check.sh` re-run after
them: 53 diffed, 0 moved writes, the harness identical to the baseline):
the kept one-track entry's note and the variants-table comment no longer
say chorus "is not patchable"; `m4a_cry.h`'s field comment said "TUNE c_v
+ chorus" where the second track's TUNE is (chorus + the pitch setter's
TUNE) & 0x7F; the step-9 instruction count was the objdump line count
(505 instructions + 19 literal words, not 541).  **Not acted on**: on the
GBA `gPokemonCrySong` persists across cries, so a chorus-0 cry after a
chorus cry carries the previous chorus in tuneValue2 (the pitch setter's
`a`), which only track 1 reads and track 1 does not exist at chorus 0 --
no audio differs, and the port builds from the pristine template (a later
"second cry" slice may revisit); `MPlayStart`'s unk_B / status / priority
precondition is not ported (true from silence on cry player 0, the only
case the harness plays); `poryaaaa_render` prints "chorus -64" for
`--cry-chorus 192` (the s8 the game passes).  **Untested branch, noted for
a later slice**: `ply_endtie` / `ply_fine` with a live channel on each of
two chains -- none of the three modes keeps a channel alive to its EOT at
these pitches, so the two-channel release tail (and `_SKIP`) remain
unmeasured.  No code changed after the baseline, so
`2026-10-25_cry-chorus.json` stands as saved.

### Done: milestone 4 slice 7, the two-channel release (2026-10-25)

The branch slice 6 left unexercised: `ply_endtie` and `ply_fine` with a
live channel on **each** of the cry player's two chains, the EOT releasing
two chained channels at once and their two release tails.  CRY_MODE_HIGH_PITCH
whole (src/sound.c `PlayCryInternal`: length 50, release 200, pitch
15800, chorus 20, volume 90) on the two samples still sounding at tick
50, Cry_Jynx (gCryTable 123) and Cry_Wailord (381).  **No ROM, CLI,
runner or engine change**: every parameter was patchable since slice 6
and the sequencer's per-track model composes the two-live-chain EOT
without a new constant (probe step 10 above).  Baseline
`baselines/2026-10-25_cry-two-chain-release.json` / `.txt`: **53/62 pass
at -40 dB**; the 60 previous rows are identical to `2026-10-25_cry-chorus.json`
in **every** field of report.json, the ref path included (the ROM did not
change; `compare_reports.py`: 0 regressions, 0 improvements, every -inf
still -inf, every row 0.0; the eight documented FAILs as before).
`poryaaaa_unit_tests` 1394/1394 (21 new), `poryaaaa_blip_tests` 49112/0,
`poryaaaa_song_tests` 561/561.  `claim_check.sh`: 55 probed tests (the two
new ones added), 0 moved writes.

| test | gCryTable | mode | tracks / samples | null | worst 1 s | ref dBFS | |
|---|---|---|---|---|---|---|---|
| cry_jynx_high_pitch | 123 | CRY_MODE_HIGH_PITCH: length 50, release 200, pitch 15800, chorus 20, volume 90 | two channels on Cry_Jynx's 23524 samples at TUNE 28 / 48 (11422 / 11630 Hz, 2.06 / 2.02 s), both alive at the EOT on frame 53, both released over 18 frames and stopped on 71 | -42.8 | -33.4 | -29.0 | FAIL |
| cry_wailord_high_pitch | 381 | the same mode | two channels on Cry_Wailord's 20846 samples (1.83 / 1.79 s), the same EOT and tails | -51.3 | -51.2 | -25.4 | PASS |

**The template bytes** (step 10's table, by hand before the first render,
pinned by `test_cry_song`'s variants rows 13-14): trackCount 2, TIE key
0x3E, TUNE 0x1C, TUNE2 0x30, VOL 0x5A, xWAIT 0x32 0x00, xRELE 0xC8 on
both (the sample is the tone word, the template does not change with it).
The probe confirms the key on both tracks at the EOT (`_081DD938` r3 =
0x3E after each `ply_endtie`).

**The EOT cost with two live chains, probe against engine**: **2596** on
frame 53 of both entries, the engine's `frameCycles` 2596 on the same frame
(`test_sequencer_cycles_cry_high_pitch_two_chain`, `entry_diff.py` 0 on
every complete frame of both), composed from the existing per-track terms
(2360 for two empty chains + 118 per live chain, step 7's three terms:
the tick's walk over a live channel 20, `ply_endtie`'s match 82 instead
of 54, and `ply_fine`'s `_loop` / `_ok` / `RealClearChain` / `_done` 86
instead of 16).  Nothing derived, nothing fitted.  **`_SKIP` 18 stays
est.**: this run does not produce a `ply_endtie` pass-over (one channel per
chain, `RealClearChain`'s prev 0 / next 0 on both; each walk ends at its
match).  It needs two channels on one track's chain, i.e. a second cry
while one plays -- a later slice.

**Death frames, probe against engine, both channels of both entries**:
the probe's two `_081DCFB0` stamps on raw f72 = frame **71** per entry
(channels 0x03000FC0 and 0x03001000, 69 cycles apart in one SoundMainRAM
pass; cycles 19986289 / 19986358 on both entries); the engine's two
channels lose CHN_ON on frame **71** on both tracks (the unit test asserts
71 / 71 against the probe's stamps), after the 18 passes 199 .. 1 on
frames 53..70 that the test asserts on both channels.  The chains do not
differ.  The last non-zero output sample agrees with the reference's on
both (1.2206 / 1.2217 s).

**Per-test sequencer residual**: `entry_diff.py` 0 on every complete frame
of both entries (the listed line is the probe's unfinished frame 179, as on
every 180-frame cry probe), `cmd_diff.py` 0, `track_diff.py` 0, with no
engine edit.

**Residual attribution** (skill, "Attributing a residual below -40 dB";
runs of output samples differing by more than 20 int16 LSB; the crossing
times from the probes' `L timer0` lines, 8 per entry at the same cycles:
0.0704, 0.1381, 0.4052, 0.4722, 0.6069, 0.9079, 1.0750, 1.1419 s):
cry_jynx_high_pitch has seven single-PCM-sample runs at 0.0706 / 0.1383 /
0.4054 / 0.4724 / 0.6071 / 0.9081 / 1.0751 s, each 0.2 ms after a crossing
(126 .. 1668 LSB; the 1.1419 crossing produces nothing above the threshold),
cry_wailord_high_pitch five, at 0.1383 / 0.4054 / 0.4724 / 0.6071 / 0.9081 s
(113 .. 1629 LSB; the 0.0704, 1.0750 and 1.1419 crossings nothing) -- the
scheduler-lateness floor on both, no run anywhere else.  **Why Jynx FAILs
and Wailord passes**: the gate is the worst 1 s window, and Jynx's worst
window starts at 1.0 s, where the reference holds only the release tail
(ref -52.9 dBFS over the window; -44 / -50 dBFS per 0.1 s at 1.0 / 1.1 s,
silence from 1.22 s): the one-LSB run at 1.0751 s (192 LSB, one PCM LSB)
against that quiet tail reads -33.4 dB, while the same crossing produces
no run on Wailord and its 1.0 s window nulls to -inf.  Over the whole file
Jynx is -42.8 and over its 0.0 / 0.1 s windows -43.0 / -42.7.  The FAIL is
the floor against a near-silent window, the ninth documented floor FAIL;
nothing in the engine addresses it (deferred item 1c), leave it.

The work, in order (commits on `accuracy-harness` after 5a2c611): the
corpus, probe step 10, unit tests and docs; the baseline and this section;
then the review paragraph below.

**Code review of the two-channel-release slice (2026-10-25).**  Two
independent reviews (reviewer 1: `ply_endtie`'s walk in m4a_1.s against
the port -- the key-byte branches, the newest-first walk over `track->chan`
with the `0x83` / SF_STOP / key tests, SF_STOP the only store, the first
match ending the walk, 61 - 7 + 28 = 82 for the RAM template; `ply_fine`'s
`_loop` / `_ok` / `RealClearChain` / `_done` with a live channel -- the
`0xC7` SF_ON test, the pointer stores and `channel->track = 0`, the unlinked
channel's own `next` followed, `track->flags = 0`, every chained channel ON
at FINE because the tick's walk already cleared the dead ones and
SoundMainRAM runs after MPlayMain, 10 + 14 + 12 + 50 + 28 per one-channel
chain; the release arithmetic at `_081DCFB6` for two channels on one frame
-- the same `muls` / `lsrs` / unsigned compare against pseudoEchoVolume 0,
the first product applied on the EOT frame itself in both, the channel
loop's per-channel state on the stack so one stop does not touch the other,
the attack / decay path leaving both at 255 from frame 5; the per-track EOT
ordering -- track 0's ENDTIE and FINE complete before track 1's walk, the
EXIST flag off from the next tick, SoundMainRAM walking channels by index so
track 0's lower-index channel stores its 0 first within the same pass.
Reviewer 2: the mode against sound.c:403-409 and the setter order at
458-465, every template byte re-derived from m4a.c's setters and the struct
offsets, the ids counted in cry_tables.inc and the sample counts from the
.bin headers (0x5BE4 / 0x516E, freq word 0x00A44000), the tune unit (ply_tune
`byte - C_V`, TrkVolPitSet's * 4 = 1/64 semitone) and the frequencies /
durations / 0.84 s to the EOT with the cry player's tempo 150 one tick per
frame, every step-10 number against the two raw logs and `.frames` /
`.events` files -- no value outside {6792, 1932, 2596, 1192, 1007} in
either, the two walks' cycles and registers, exactly two stamps and eight
crossings per log -- the envelope ladder, the runner keys in accuracy.py
and claim_check.sh, the README / skill text).  **No divergence** on any
path this slice exercises.  **Adopted** (unit-test pins and a comment, no
code or input changed; `claim_check.sh` re-run after them): the test now
asserts track 0 on channel 0 and track 1 on channel 1 (ply_note's
first-free scan in index order, the probe's 0x03000FC0 / 0x03001000 and
the within-pass stamp order), and ON | STOP without IEC on both channels
over frames 53..70 (the release path, not pseudo-echo); the test's comment
reconciles the 82 as 61 - 7 (RAM byte) + 28; the Done section's breakdown
of the 118 per live chain names step 7's three terms (the tick's walk over
a live channel 20, the match 28, ply_fine's 70) instead of two.  **Not
acted on, recorded**: `_SKIP` 18 is one est. value where the source has
three pass-over paths of different lengths (the `0x83` test failing, SF_STOP
set, the key mismatch: m4a_1.s 1833-1850), a latent timing difference for a
chain of two or more whose newest channel does not match -- not reachable
on the cry player until a second cry while one plays, and the constant is
labelled est.; a channel in pseudo-echo is passed over by both (the `0x83`
mask excludes SF_IEC), a confirmation.  No engine code changed after the
baseline, so `2026-10-25_cry-two-chain-release.json` stands as saved.

### Done: milestone 4 slice 8a, a second cry while one plays -- the ground truth (2026-10-25)

The ROM, the two patchers, the probe and the measurements only; **no
engine, `poryaaaa_render`, runner or `songs.json` change and no new corpus
test** (probe step 11 above has every number and the ROM story).  Baseline
`baselines/2026-10-25_second-cry-rom.json` / `.txt`: **53/62 pass at -40
dB**, every one of the 62 rows identical to `2026-10-25_cry-two-chain-release.json`
in every field of report.json but the ref path (the ROM changed; refs
`56b288f4b49f-mGBA_0.10.5+m4a_blip`; `compare_reports.py` 0 regressions, 0
improvements, every row 0.0, the nine documented floor / open-bus FAILs as
before).  `claim_check.sh` against the previous baseline: 55 probed tests
diffed, 0 moved writes; `poryaaaa_unit_tests` 1395/1395, `poryaaaa_blip_tests`
49112/0, `poryaaaa_song_tests` 561/561.  The gates of slices 4 and 6: the
cry_jynx_faint_full probe re-run under the new ROM gives a `.frames` file
identical to the old ROM's, and pcm / pcm_cmp / cry_bulbasaur / sq1 render
byte-identical to the baseline references.

**The ROM** (`test/accuracy/rom/main.c`, `crt0.s`): `AccParams` + `u16 id2`
(block offset 20) + `u8 startFrame2` (22), 5 reserved bytes left, 28 bytes;
`StartCry(id2, 0, 120, 15360, 140, 0, 0)` on VBlank `startFrame2` through
the game's `SetPokemonCry*` / `SetPokemonCryTone` path; `mgba_render --id2 /
--start-frame2`; `mgba_probe` arguments 15 / 16 and `symbol[N]` as the
eighth.  Symbol shift **+0x40** from `AgbMain` on (`CgbSound` 0x800107c, the
`CgbSound_*` labels +0x40 in the skill and the probe header, IWRAM
unchanged).  **The rule learnt**: `AgbMain`'s boot path and per-frame loop
must stay instruction-identical across ROM changes -- a two-instruction
prologue change moved the timer-0 start 6 cycles and every DirectSound
reference with it, invisible to the `.frames` gate; the second cry's wait
therefore lives in `StartTest`.  Check pcm / pcm_cmp / cry_bulbasaur
byte for byte before the harness after any ROM change.

**What slice 8b needs** (all from step 11; nothing derived or fitted):
- **Player**: the second cry lands on **cry player 1** (`SetPokemonCryTone`'s
  first free player: player 0's track 0 has MPT_FLG_EXIST, player 1's has
  flags 0 and `chan` 0; when neither is free the larger `clock` is replaced);
  `MPlayStart` always starts it (the cry players' `unk_B` is 0).  Its song
  copy is `gPokemonCrySongs[1]` (IWRAM, the same 7-per-byte costs), its
  tracks 0x030022F8 / 0x03002348.
- **Entry position**: cry 1 is the first MPlayMain entry of the frame (291)
  and its body runs **last**, after cry 0's: FadeOutBody **88** after cry
  0's `_081DDA6C`, `_081DD9BC` **54** later (the 142 head), its own body,
  then CgbSound **55** after its `_081DDA6C`.  Alone: 889 / 977 / 1031 / 55.
  A playing cry player costs the frame **79 + its body** whichever it is;
  frame = 1007 + Σ (79 + body) over playing players, cry 0's body before cry
  1's.
- **Skeleton**: 1596 one playing, **2185** both playing steadily (bodies
  510 each), the second's start tick **4736** (= 1596 + 79 + 3061, the 3061 =
  the first's 2991 + the scan pass-over 70), unlink ticks + 93 (2278 / 2258 /
  1689), dead tracks - 20 (2165 / 2145 / 1576), the EOT + 234 on an empty
  chain (2379 with cry 1's track dead, 2399 with its channel alive, 1810
  alone), the finished tick + 185 (1761 / 1781 / 1192).
- **Channels**: the first cry on channel 0 (0x03000FC0), the second on
  **channel 1** (0x03001000): `ply_note`'s index-order scan passes over the
  live channel 0 at **70** cycles (`M4A_CYC_SCAN_ON` 20 + `SCAN_ON_CMP` 4 +
  `SCAN_CMP` 12 + `SCAN_EARLIER_TRACK` 22 + `SCAN_NEXT` 12 -- equal priority
  10, the earlier track pointer) and takes channel 1 at `SCAN_FREE` 14.
- **Deaths**: all through the count path, no `_081DCFB0` stamps: Pidgey (cry
  1) frame 33 = 22 + 11, Bulbasaur (cry 0) 50; Jynx (cry 0) 137, Wailord
  (cry 1) 181 = 62 + 119; every EOT (143 / 162; 143 / 202) on an empty chain.
  `startFrame2` N starts the second's tick on frame **N + 2**.
- **Crossings**: (a) 3 at 0.2043 / 0.2713 / 0.3383 s; (b) 24, listed in
  step 11 (`build/probe/cry_second_*.txt.events`).
- **Chains**: one channel per track always; `_SKIP` 18 stays est.

**The handoff**: the engine runs **one sequencer per render** (one
`frameHook`, `m4a_sequencer_set_player` picking one player's chain
position).  Slice 8b makes two players run concurrently on one frame -- cry
player 0's body then cry player 1's, each with its own tracks, template copy
and chain position, sharing the twelve channels' index-order scan and
SoundMainRAM -- then adds `poryaaaa_render --cry2 N --cry2-start F`, a
`songs.json` key, the runner, and corpus entries for the two runs above
(expected: the skeleton and deaths of step 11, both EOTs on empty chains, the
floor residual at the listed crossings).  Cries over music stay later still.

**Code review of the second-cry ROM slice (2026-10-25).**  Two independent
read-only reviews (reviewer 1: `SetPokemonCryTone`'s free test, `maxClock`
fallback and tie, the song copy and pointer rewrites against the ROM's
`StartCry` path and the logs' r7 / r5 / r4; `MPlayStart`'s boolean with the
cry players' `unk_B` 0 and the priority never consulted, the nine setters in
`PlayCryInternal`'s order and the second call's values; `MPlayOpen`'s prepend
and `MPlayMain`'s call-before-body giving bodies BGM, SE1, SE2, SE3, cry 0,
cry 1 with exactly six entries on every frame of both logs; `ply_note`'s
scan from `_081DDBFA` walked in the source for an ON channel at equal
priority with a lower track pointer -- `_081DDC14` / `_081DDC18` /
`_081DDC24` / `bls` / `bcc` / `_081DDC34`, not recorded as a steal candidate
-- against the log's label order and the 20 / 4 / 12 / 22 / 12 deltas, 1003
against 933 for the two `ply_note` spans; `AgbMain`'s single `StartTest`
call, the wait loop's count, exactly one `MPlayStart` per cry, the tick on
count + 2; the block layout against crt0.s, agbcc hazards.  Reviewer 2: the
28-byte layout against the ROM bytes at 0xC0 and both patchers' b[12..14],
the `symbol[N]` parse and sizeof MusicPlayerInfo 0x40, the nm tables
re-counted (9461 at +0x40, 16 at 0, IWRAM / EWRAM 0, no other shift), the
CgbSound disassemblies (586 lines, one literal word +0x40) and each label's
instruction, the README / skill addresses, every step-11 number against the
raw logs and `.frames` / `.events` files, the sample headers from the ROM
(freq 10764288, sizes 8183 / 1902 / 23524 / 20846), the `_p1` files, the Done
section against step 11, and the two baselines field by field).  **No
divergence** in anything either review could test; every number confirmed.
**Adopted** (cosmetic, no engine or ROM code changed): the probe's `.frames`
header now prints the eighth argument as given (`gPokemonCryMusicPlayers[1]`,
it had printed the truncated symbol; the `_p1` files re-run, only their
header line changed); `StartTest`'s sizes 0x78 -> 0xb8 (`nm -S`; growth
+0x40 as stated), "41 IWRAM + 8 EWRAM" for the 49 RAM symbols, Wailord
118.4 frames instead of "about 119".  **Not acted on, recorded**: the
`maxClock` fallback (both cry players busy: the larger `clock` is replaced,
a tie takes player 0) is never exercised in these logs; `startFrame2` <=
`startFrame` starts both cries on the same count (cry 0's START processed
first), never before or twice; `SetPokemonCryPitch` reads `tuneValue2 -
tuneValue` from the global template left by the previous cry, so a chorus-0
cry after a chorused one inherits its TUNE2 -- inaudible, since chorus 0
sets trackCount 1 and part1 never runs, and a nonzero chorus overwrites
TUNE2; the one-cry comparison log `cry_bulbasaur.txt` is from the
2026-10-24 ROM (0x8c lower from `AgbMain` on), its frame-4 cycles identical
to the new logs'; the first cry's `MPlayStart` is 85 cycles later in the
main loop because `StartTest` grew (no SoundMain effect, the first tick's
cycles are identical); the cry players' equal priority 10 protects cry 0's
channel from a cry 1 steal by the lower-track tiebreak while cry 1's would
be stealable by a later cry 0 note if no channel were free -- not reached
(maxChans 5, channel 1 free).  `claim_check.sh` after the edits: 55 diffed, 0 moved, unit
1395/1395, blip 49112/0, songs 561/561, harness identical to
`2026-10-25_second-cry-rom.json` (0 regressions, every row 0.0); no engine
or ROM code changed after the baseline, so it stands as saved.

### Engine: two cry players (slice 8b-i, 2026-10-25) -- the structural statement

Written before any code change, from probe step 11 and the slice-8a
handoff; nothing here is measured anew.  **What the engine does today**:
one `M4ASequencer` per render, attached to the engine's single
`frameHook`; `m4a_sequencer_frame` starts its cycle count at the player's
`mpEntry` (the deeper players' paused costs folded in: cry 0's 826 = 566 +
71 + 3 x 63), charges `mpHead` and the body, and ends with `mpExit` (the
outer players' idle 63 each plus the tail 55: cry 0's 118) before
publishing the CgbSound entry.  Its tracks are engine tracks 0..n-1 by
the same index (`engine->tracks[i]`, `m4a_engine_note_on(e, i, ...)`,
the channel's `trackIndex` compared with `i` in `ply_note`'s scan and the
CGB steal test); its per-track chain bookkeeping (`chainOwner`, `chanSeq`)
is its own; the voicegroup is the engine's one `voiceGroup`, which `--cry`
loads with the cry's ToneData as voice 0.

**What has to change**, in the order SoundMain composes it:

1. **A second sequencer instance** for cry player 1: its own `M4ASeqTrack`s,
   its own `M4ASong` (the template copy `gPokemonCrySongs[1]`, 52 bytes in
   IWRAM: `dataInRam` set, so the same 7-per-byte rule), its own
   `chainOwner` / `chanSeq`, its own tempo accumulator and clock.  Its
   engine tracks are a **disjoint range** (`trackBase` 2: engine tracks
   2..3, above cry 0's 0..1 as `gPokemonCryTracks[2..3]` 0x030022F8 lies
   above `[0..1]` 0x03002258), so `ply_note`'s scan and the engine's
   allocation compare the second cry's track against the first's channel
   as the GBA compares the track pointers: channel 0's track is the
   *earlier* one -> `M4A_CYC_SCAN_EARLIER_TRACK`, not a steal candidate.
   Its VOICE 0 must resolve to the **second** cry's ToneData: a
   per-sequencer voicegroup (NULL = the engine's), consulted by `ply_voice`
   only; the engine's channels carry the voice by copy as before.
2. **Channels** stay the shared twelve: both sequencers call the same
   `m4a_engine_note_on`, whose allocation is `ply_note`'s index-order scan
   over `pcmChannels[0..maxPcmChannels)`.  With cry 0 on channel 0 (ON,
   priority 10, lower track) the second cry's note passes it over and takes
   channel 1: `SCAN_ON` 20 + `SCAN_ON_CMP` 4 + `SCAN_CMP` 12 +
   `SCAN_EARLIER_TRACK` 22 + `SCAN_NEXT` 12 = 70, then `SCAN_FREE` 14 --
   the existing constants, as step 11 found.  Nothing new.
3. **The frame order**: `MPlayMain` is entered for cry 1 first (291) but
   its body runs **last**, after cry 0's, because each player calls the
   previously opened one before its own body.  So the second sequencer is
   chained *after* the first: the engine's hook runs cry 0's body to its
   `_081DDA6C`, then cry 1 continues from that cycle count (not from its
   own `mpEntry`), then the last one charges the exit.  Cost composition:
   - cry 0 playing, cry 1 paused: 826 + 142 + body0 + **63** (cry 1's
     `mpPaused`) + **55** (cry 1's `mpExit`) = the one-cry 1596 (body 510):
     the 118 cry 0 charged alone is exactly 63 + 55, so a single-player
     render is unchanged by construction -- and the single-player code path
     is kept literally (no outer sequencer: `mpExit` as before).
   - both playing: 826 + 142 + body0 (`_081DDA6C` 1478 steady) + **142**
     (cry 1's `mpHead`: FadeOutBody 88 + 54) + body1 + **55** = 2185 with
     both bodies 510; the second start tick 1596 + 79 + 3061 = 4736 (79 =
     142 - 63: a playing player costs the frame its head less the paused
     idle, plus its body).
   - cry 1 playing, cry 0 paused: 826 + **63** (cry 0's `mpPaused`) + 142 +
     body1 + 55: `_081DDA6C` 889, FadeOutBody 977, `_081DD9BC` 1031, exit
     55 -- step 5's est. row, measured in step 11.
   - the tick-all-done frame (`M4A_CYC_TICK_ALL_DONE` 24) and the apply pass
     end at the player's `_081DDA6C` as before; only the exit moves to the
     chain's last member.  Splitting one `cyc()` of 118 into 63 + 55 charges
     the FIFO DMA stall identically (the stall count is over the half-open
     cycle range, additive over a split).
   - the frame line of `--seq-log` is written once, by the last member,
     after both players' per-command / per-track lines, so `entry_diff` /
     `cmd_diff` / `track_diff` read a two-player log unchanged (the probe's
     ply_* and `_081DD998` breakpoints are in the same order: cry 0's then
     cry 1's); the track number on the `c` / `t` lines becomes the engine
     track index (2 / 3 for cry 1) so the two players' lines are telling
     apart.  No tool change expected; if one is needed it is recorded in
     the Done section.
4. **The second start**: `StartCry(id2, 0, 120, 15360, 140, 0, 0)` on VBlank
   `startFrame2` = `MPlayStart` between two SoundMains, the start tick on
   engine frame `startFrame2 + 2` (as cry 0's `startFrame` 1 gives frame 3).
   In the engine a frame hook's entry is engine frame `frameCount + 2`
   (boot runs frame 2's hook), so the start is issued at the top of the
   hook of frame `startFrame2 + 2`, before either body -- `MPlayStart`
   touches only the player's own tracks, so the point between the previous
   SoundMain and this one is the same state.  `SetPokemonCryTone`'s
   player choice is taken at that moment from the two players' track 0
   (flags 0 and `chan` 0 or `chan->track != track` = free; the first free
   wins -- cry 1 in both runs, cry 0 having MPT_FLG_EXIST); neither free
   (the `maxClock` replacement) is refused with a message, out of scope.
   The second cry plays at CRY_MODE_NORMAL's defaults only.
5. **No new timing constant**: every number above is one of `M4A_CYC_MP_*`,
   the per-player `mpHead` / `mpPaused` / `mpExit` `m4a_sequencer_set_player`
   already derives, and the per-track / per-command costs the one-cry
   renders use.  If a frame of the two runs does not compose from them the
   gate records it and stops; nothing is fitted.

### Done: milestone 4 slice 8b-i, two cry players in one render -- the engine and poryaaaa_render (2026-10-25)

**The structural change** (the statement above, carried out as written; no
runner, `songs.json`, corpus or baseline change, the 62 rows identical).
`M4ASequencer` gained `trackBase` (its tracks are engine tracks
`[trackBase, trackBase + maxTracks)`; every engine-track use -- the
`engine->tracks[i]` fields, `m4a_engine_note_on` / `_xcmd` /
`_track_apply`, `ply_note`'s scan and CGB steal comparisons, the chain
membership test -- goes through it, 0 for a lone player), a per-player
voicegroup for VOICE (`m4a_sequencer_set_voicegroup`;
`m4a_engine_program_change_from`, NULL = the engine's), and SoundMain's
chain: `m4a_sequencer_init_chained(outer, inner, trackBase)` attaches a
higher player whose body runs after `inner`'s from `inner`'s `_081DDA6C`
cycle count (the second player installs no hook; `inner`'s runs the
chain; `set_player` refuses a chain-order or track-range violation).
`m4a_sequencer_frame` became `frame_body` (to the player's `_081DDA6C`:
the paused 63, or the head 142 + the body, or the tick-all-done 24) run
for each member in MPlayOpen order, then the last member's `mpExit`; the
last member publishes `cgbEntryCycles` and writes the `--seq-log` frame
line after both players' `c` / `t` lines (whose track numbers are now the
engine index, 2 / 3 for cry 1; `entry_diff` / `cmd_diff` / `track_diff`
read a two-player log **unchanged** -- the probe's ply_* and `_081DD998`
breakpoints are in the same order, cry 0's then cry 1's).  A lone player
runs the old path literally, the one difference the 118 exit charged as
63 + 55 in two `cyc()` calls instead of one -- additive over the FIFO
stall's half-open cycle range, and gate a below confirms it.
`m4a_sequencer_player_free` is `SetPokemonCryTone`'s track-0 test (flags 0
and no chained channel of its own); `m4a_cry_choose_player` its first-free
rule over the two cry players (neither free -> -1: the `maxClock`
replacement stays unimplemented, refused).  `poryaaaa_render --cry2 <id>
--cry2-start-frame <1..255>` (needs `--cry` on cry0): the second cry's
sample and its own template copy at CRY_MODE_NORMAL's defaults, cry
player 1 chained on engine tracks 2..3, a frame-hook wrapper issuing the
`MPlayStart` at the top of frame F + 2's hook (`frameCount == F`, before
either body) on the first free cry player -- cry 1 while the first plays,
cry 0 if the first has finished -- or refusing the render with a message
when neither is free.  **No new timing constant**; every frame of both
runs composed from the existing ones.

**Gate a** (`claim_check.sh` against `2026-10-25_second-cry-rom.json` after
the change): 55 probed tests diffed, **0 moved writes**; harness 53/62,
0 regressions, every one of the 62 rows 0.0 in null and worst (the -inf
rows -inf); unit 1395/1395 (before the new tests), blip 49112/0, songs
561/561.  A single-player render did not move by a cycle.

**Gate b** (the two step-11 runs with the new flags against the probe
logs; the `_p1` `.frames` files give the same result, the raw logs being
identical): (a) `--cry 0 --cry2 15 --cry2-start-frame 20`, 3.0 s:
`entry_diff` 0 on all 177 complete frames (the 180-frame probe's last
frame has no CgbSound entry), `cmd_diff` 0 of 178, `track_diff` 0 of 178;
(b) `--cry 123 --cry2 381 --cry2-start-frame 60`, 4.0 s: `entry_diff` 0
on all 236, `cmd_diff` 0 of 237, `track_diff` 0 of 237.  The skeleton to
the cycle: (a) 4077 / 1596 / 4736 / 2185 / 2278 / 2165 / 2258 / 2145 /
2379 / 1761 / 1576 / 1810 / 1192 / 1007 on frames 3 / 4..21 / 22 / 23..32
/ 33 / 34..49 / 50 / 51..142 / 143 / 144 / 145..161 / 162 / 163 / 164 on;
(b) 4077 / 1596 / 4736 / 2185 / 2278 / 2165 / 2399 / 1781 / 1596 / 1689 /
1576 / 1810 / 1192 / 1007 on 3 / 4..61 / 62 / 63..136 / 137 / 138..142 /
143 / 144 / 145..180 / 181 / 182..201 / 202 / 203 / 204 on.  The second
cry on cry player 1 and channel 1 through the existing scan constants
(the 70-cycle pass-over of channel 0 inside the 4736).

**Gate c** (`mgba_render --type cry --id 0 --id2 15 --start-frame2 20
--frames 182` / `--id 123 --id2 381 --start-frame2 60 --frames 242`
against the renders, `metrics.compare_files`; not a baseline row):
(a) null **-60.4 dB**, worst 1 s **-59.9** (at 0.2 s), lag 0.0 ms, gain
1.00001; (b) null **-45.3 dB**, worst **-42.6** (at 0.1 s), lag 0.0, gain
0.99998 -- the levels of the one-cry rows (cry_pidgey / cry_bulbasaur
about -60, cry_jynx / cry_wailord at the floor's -45..-42, the DPCM
waveforms jumping).  Every run above the 20-LSB threshold attributed
(the skill's run finder, left and right channels): (a) 3 runs, each one
PCM sample (0.3..1.4 samples at 48 kHz), at 0.2045 / 0.2715 / 0.3385 s --
the three `.events` crossings 0.2043 / 0.2713 / 0.3383 s, each 0.2 ms
after its crossing (the slot's half-period); nothing else.  (b) 21 runs
on the left channel and 22 on the right, each one PCM sample, every one
within 0.3 ms of one of the 24 crossings; the crossings at 0.4052 and
2.1472 s give |d| <= 1 on both channels (the slipped sample equals its
neighbour there) and 0.7401 s a run on the right channel only (|d| 113,
left 1).  No run at a note event, no 64-sample block, no whole frame: the
PCM scheduler-lateness floor and nothing else.

**Gate d** (`test/test_engine.c`, `test_sequencer_cycles_two_cries`:
unit 1395 -> **1471/1471**): the frame skeleton of both runs on frames
3..180 / 3..238 against the probe values, played with stand-in DPCM
samples of the cries' lengths (8183 / 1902, 23524 / 20846 at 10512 Hz);
the second cry on cry player 1 (`m4a_cry_choose_player` 0 before anything
plays, 1 with cry 0's track EXIST | START, 0 again when both have
finished) and on channel 1 with cry 1's track 0 as engine track 2; both
channels ending through the count path (CHN_ON cleared after the mix of
frames 32 / 49 and 136 / 180, the unlink ticks 33 / 50 and 137 / 181 in the
skeleton; never ON | STOP, so no `_081DCFB0` stamp); both EOTs (143 / 162;
143 / 202) finding the chain empty; the chain's last member publishing the
entry, both players counting the same frames; a single cry with the second
player attached and inactive giving the one-cry skeleton of
`test_sequencer_cycles_cry` unchanged; chain-order refusal and detach.

**Tool changes**: none; `entry_diff` / `cmd_diff` / `track_diff` read the
merged log as is (one frame line, the c / t lines of both players in
SoundMain's order).  **What slice 8b-ii needs**: a `songs.json` key for
the second cry (its id and start frame, e.g. `"id2"` / `"start_frame2"`
as `mgba_render` names them), the runner passing `--id2 / --start-frame2`
to `mgba_render` and `--cry2 / --cry2-start-frame` to `poryaaaa_render`
(and `claim_check.sh`'s cry argument line the same), two corpus entries
for the runs above (expected: the gate-c nulls, the floor at the listed
crossings), and the baseline with the two new rows.  Cries over music,
and a second cry while both players are busy (the `maxClock`
replacement), stay later.

**Code review of slice 8b-i (2026-10-25).**  Two independent read-only
reviews (reviewer 1: the chain against SoundMain's `o_SoundInfo_MPlayMainHead`
call and MPlayMain's `_081DD82E` call-before-body, `MPlayOpen`'s prepend
and `m4aSoundInit`'s order giving bodies BGM, SE1, SE2, SE3, cry 0, cry 1;
the paused 63 / head 142 / tail 55 composition against steps 4, 5 and 11;
the lone player's path operand by operand against the old code and the FIFO
stall's half-open range (additive over the 63 + 55 split in every reachable
case); `SetPokemonCryTone`'s free test, loop and `goto start_song` against
`m4a_sequencer_player_free` / `m4a_cry_choose_player`; `MPlayStart`'s
precondition (never failing for a cry player, `unk_B` 0, ident set) and its
own-tracks-only init against `m4a_sequencer_start`; `ply_note`'s scan walked
label for label for the second cry's note over the first's live channel
(`_081DDBFA` / `_081DDC14` / `_081DDC18` / `_081DDC24` / the taken `bls`
and `bcc` / `_081DDC34` / `_081DDC40`, 20 + 4 + 12 + 22 + 12 + 14) and the
reverse case (a later-track candidate recorded, the free channel still
taken), the CGB steal compare, the address order `gPokemonCryTracks[0]` <
`[2]` as engine index 0 < 2; every engine-track access routed through
`trackBase`; the second start's window against `StartTest`'s main-loop
call between two SoundMains.  Reviewer 2: the four frame states derived
from the code against the `.frames` files and step 11's table, the idle
term 0 for the cry pair and step 4's rule for a sparser chain; every
piece-wise value of the three expected-value functions checked frame by
frame against the probe `.frames` files (0 mismatches on 3..178 / 3..238 /
3..145), the sample sizes against the ROM's WaveData headers, the deaths
and unlinks against the raw logs' `RealClearChain` cycles (no `_081DCFB0`
in either log), the start frames against cry 1's `ply_note` cycles; the
three diff tools on the merged log (one frame line, both players' lines in
SoundMain's order, nothing dropped, 0 / 0 / 0 on both runs); the flags'
ranges against the ROM's `u16 id2` / `u8 startFrame2` and `mgba_render`,
the checks and messages, every exit path's frees, the dynamic-end
condition, the shared trace file, `claim_check.sh` skipping the
`cry_second_*` logs, the README / skill text, the callers of the struct.)
**No divergence** on any path the two runs exercise; every number
confirmed.  **Adopted** (none can touch a stock render; one `claim_check.sh`
over the batch: 55 diffed, 0 moved, 53/62, 0 regressions, every row 0.0,
unit 1471/1471, blip 49112/0, songs 561/561): `m4a_sequencer_init_chained`
refuses an inner that is cry player 1 (nothing is opened after it) and
fails when `set_player` fails instead of linking a player-0 / zero-track
chain; `set_player` also refuses a track count that would run into the
outer's `trackBase`; the unused `sCry2.user` field dropped and the
"second cry on cry player 0" summary line reworded (the counts printed are
the restart's); run (a)'s unit-test bound 180 -> 178, the 180-frame probe's
last complete frame; `entry_diff.py` skips the probe's -999999 sentinel row
(the incomplete last frame), so the summary line reads 0 differ on a
probe-length render -- the one tool change of the slice; a comment at
`ply_note`'s ClearChain charge for the latent case below.  **Not acted on,
recorded**: a cross-player channel steal (player A's note taking a channel
player B's track still chains) would charge `UNLINK_NONE` and leave the
channel in B's `chainOwner` until B's next walk, where the GBA's
`RealClearChain` drops it at once through `channel->track` -- cycle model
only, unreachable with two cry players (at most four channels of five) and
noted for a BGM + cry chain; the `maxClock` replacement when both cry
players are busy is refused, not played (by design, fails loudly);
`track_diff.py`'s "track i % 10" label is the breakpoint position, which on
a cry-1-alone frame reads 0 / 1 for engine tracks 2 / 3 (diagnostic text
only); `m4a_sequencer_start`'s second `track_stop` loop runs to
`M4A_SONG_MAX_TRACKS` rather than the player's count (never-EXIST tracks
return at once); a future `songs.json` key for the second cry needs
`claim_check.sh`'s cry argument line (the `extra = ...` python) extended and
a 4.0 s rung for the 240-frame probe; step 11's heading sentence "the
engine runs one sequencer per render" is historical.

### Done: milestone 4 slice 8b-ii, the two-cry corpus rows (2026-10-25)

The runner, `songs.json`, `claim_check.sh` and the baseline only: no engine,
plugin, `poryaaaa_render`, ROM or `test_engine.c` change; the two runs of
slice 8b-i gate c become corpus rows.

**The keys.**  A cry entry may carry `"id2"` and `"start_frame2"` (both or
neither; the runner skips an entry with one of them with a warning), named
as `mgba_render` names them: the second cry's gCryTable index, played at
CRY_MODE_NORMAL's defaults, and the VBlank (1..255, the ROM's `u8
startFrame2`) its `StartCry` runs on -- its start tick on engine frame
`start_frame2 + 2`.  **The runner lines**: `mgba_render ... --id2 N
--start-frame2 F` (AccParams bytes 12..14 through the existing patcher) and
`poryaaaa_render --cry N ... --cry2 N --cry2-start-frame F` (after the
`--cry-*` parameters; cry player 1 chained after cry 0's, slice 8b-i);
`--input midi` skips the entry with the cry message as before.  The
reference frame count is the runner's existing `seconds -> ceil(seconds *
cpu / frame) + 2`: 182 for 3.0 s (gate c's count) and **241 for 4.0 s where
gate c used 242**; the comparison truncates to the shorter file, the 4.0 s
poryaaaa render (192000 samples against the 241-frame reference's 192996),
so the 241- and 242-frame references give the same null to full precision
(-45.3057 dB on both, checked with `metrics.compare_files`; the rule stays).
`claim_check.sh`'s cry line adds `--cry2 / --cry2-start-frame` from the
keys and a 4.0 s rung for a probe over 200 frame lines (the 240-frame
jynx_wailord probe has 239), so the two `build/probe/cry_second_*.txt` logs
are diffed: 0 mgba writes against 0 engine writes each (cries write no PSG
register), counted among the diffed tests.

**The two rows** (`songs.json`, named after the probe logs):
`cry_second_bulbasaur_pidgey` (id 0, id2 15, start_frame2 20, 3.0 s) and
`cry_second_jynx_wailord` (id 123, id2 381, start_frame2 60, 4.0 s).
**Gate a**: (a) null **-60.4 dB**, worst 1 s **-59.9** (at 0.2 s), lag 0.0,
gain 1.00001, ref / test -28.8 dBFS; (b) null **-45.3 dB**, worst **-42.6**
(at 0.1 s), lag 0.0, gain 0.99998, -22.1 dBFS -- gate c's numbers to the
digit, both PASS at -40; the residual the PCM scheduler-lateness floor at
the `.events` crossings (3 / 24 `timer0 slot_crossed`, attributed run by
run in slice 8b-i's gate c).  The 62 existing rows identical to
`2026-10-25_second-cry-rom.json` in every `report.json` field
(`compare_reports.py` 0 regressions, every row 0.0, the two new rows "only
in after").  **Gate b**: `claim_check.sh` against the previous baseline --
57 tests diffed (55 + the two new logs), 0 moved; harness 55/64; unit
1471/1471 (unchanged), blip 49112/0, songs 561/561.  **Gate c, the
baseline**: `test/accuracy/baselines/2026-10-25_two-cry-rows.json` / `.txt`
from the full default run, 64 rows, **55/64** at -40 dB (the nine
documented FAILs unchanged: se_rain_stop, cry_pidgey, cry_jynx,
cry_bulbasaur_howl, cry_pidgey_quiet, cry_bulbasaur_reverse,
cry_pidgey_echo_start, cry_pidgey_echo_start_chorus, cry_jynx_high_pitch).

**What comes next**: the next ground-truth probe is a cry over music (BGM
and a cry player in one chain, the latent cross-player steal of slice
8b-i's review reachable) or a cry while an SE plays -- a probe step and a
ROM parameter first, not an engine change; a second cry while both cry
players are busy (the `maxClock` replacement) stays refused.

**Code review of slice 8b-ii (2026-10-25).**  Two independent read-only
reviews (reviewer 1: the runner's `--id2 / --start-frame2` line against
`mgba_render.c`'s flat argv parser and its AccParams patch -- `b = rom + off
+ 8`, so `b[12..14]` are block offsets 20..22, `u16 id2` / `u8 startFrame2`
of `rom/main.c`, `StartTest`'s wait and `StartCry(id2, 0, 120, 15360, 140,
0, 0)`; the poryaaaa line against `--cry2`'s 0..0xFFFF, `--cry2-start-frame`'s
1..255, the "go together" / "needs --cry" / cry-player-0 checks and the
cry0 default without `--player`; `frames_for` 122 / 182 / 241 from the
file's constants and the same rule as every one-cry row; 4.0 s = 238.9
frames covering the start tick 62, the unlink 181 and both EOTs 143 / 202;
the `--input midi` skip through the same `cry` branch, the both-or-neither
check making the `int()` safe, no other consumer of the keys; `claim_check`'s
argument lines rebuilt by awk to `--cry 0 --cry2 15 --cry2-start-frame 20`
/ `--cry 123 --cry2 381 --cry2-start-frame 60`, the `_p1` logs skipped by
the empty args, the rungs 179 -> 3.0 and 239 -> 4.0 with every existing
probe at 69 / 179 / 599 / 1199 lines unchanged; the notes against gate b
and d.  Reviewer 2: every field of the two rows against gate c of 8b-i,
`.events` `slot_crossed=3` / `24`, the 62 shared rows of the baseline
field by field with python (0 differences, headers equal, the .txt diff
only the two appended rows and the summary), the nine FAILs, the README /
skill text and a grep for the stale "no corpus entry" sentences.)  **No
divergence.**  **Adopted** (one `claim_check.sh` against
`2026-10-25_two-cry-rows.json`: 57 diffed, 0 moved, 55/64, 0 regressions,
every row 0.0, unit 1471/1471, blip 49112/0, songs 561/561 -- unexercised
by the corpus, neutral by construction): the runner refuses an `"id2"`
outside 0..65535 or a `"start_frame2"` outside 1..255 with a warning and
a skip, since `mgba_render` would take a 0 as "no second cry" while
`poryaaaa_render` refuses it.  **Not acted on, recorded**: the
`songs.json` notes and this section say "180 / 240 frames" for the probes
(the plan's nominal lengths) while `grep -c '^frame'` counts 179 / 239,
the rung thresholds being set on the latter; `claim_check`'s printed cry
line carries a double space before `--cry2` (awk rebuilds it); its python
block runs without `-I` (pre-existing, the repo's own file); 241 x
280896 / 16777216 x 48000 is 193680, not the 192996 samples `mgba_render`
reports for 241 frames (the blip's own rounding; the plan quotes the
reported count); step 11's "one sequencer per render" sentence stays
historical.

### Done: milestone 4 slice 9b-i, a cry over a sound effect and over music -- the engine and poryaaaa_render (2026-10-25)

**The structural change** (step 12's statement, carried out as written; no
`songs.json`, runner, `claim_check.sh`, ROM, reference or baseline change;
`plugin/m4a_sequencer.c/.h`, `cmd/poryaaaa_render.c`, `test/test_engine.c`
only).  Before anything, `claim_check.sh` against
`2026-10-25_two-cry-rows.json` reproduced the baseline (57 diffed, 0 moved,
55/64, 0 regressions, every row 0.0; unit 1471/1471, blip 49112/0, songs
561/561).
(1) `m4a_sequencer_init_chained_player(outer, inner, trackBase, player)`
names the chained player; `m4a_sequencer_init_chained` is now the wrapper
selecting `inner->player + 1` (cry 1 after cry 0, unchanged).  BGM (10
tracks) -> cry 0 on engine tracks 10..11 and SE1 (3) -> cry 0 on 3..4 are
accepted (SE2 -> cry 0 on 9..10, SE3 -> cry 0 on 1..2 too), and a BGM ->
SE1 -> cry 0 chain of 15 tracks, which the next-player form refused.  The
gap players' idle 63 each were already charged by the chain's frame code
(`(o->player - last->player - 1) * M4A_CYC_MP_IDLE`: SE1 / SE2 / SE3's 189
for BGM -> cry 0, SE2 / SE3's 126 for SE1 -> cry 0) -- verified, not
re-added: frame 32 of run (a) = 637 + 142 + 1516 + 126 + 142 + 2991 + 118
= 5672.  (2) **The cross-player steal**: when `ply_note`'s note-on takes a
channel this player's own chains do not hold, `chain_owner_of` walks the
chain (to its first member, then along `outer`) for the member whose
`chainOwner[h]` still holds it -- the GBA's `channel->track` -- and the
taking note charges `NOTE_TAKE` + `unlink_cost` run against *that* owner
(its prev / next: `NOTE_UNLINK_00/01/10/11`) + `CHAIN_EMPTY` when the
taker's chain is empty, clearing the owner's `chainOwner[h]` at once; the
owner's next walk sees one channel fewer (the -32) and never meets the
channel as dead.  No owner found: `NOTE_UNLINK_NONE` 44 as before.  The
same-player path is literally the one before (a channel of this player's
own track goes through the unchanged branch).  (3) `poryaaaa_render --song
<se or song .s> [--player bgm|se1|se2|se3] --cry2 <id> --cry2-start-frame
<F>` plays gCryTable[id] at CRY_MODE_NORMAL's defaults over the song: cry
player 0 chained after the song's player with the cry's ToneData as its own
voicegroup (`load_cry2`, the loading shared with the `--cry --cry2` form),
the `MPlayStart` at the top of frame F + 2's hook by `m4a_cry_choose_player`
over `{cry 0, NULL}` (cry 0 from silence).  `--cry ... --cry2` (cry 1 after
cry 0) is unchanged.  Refused with messages: `--cry2` without `--cry` or
`--song` (the MIDI path), and `--player cry0 / cry1` for the song in this
form (`main`'s check and `render_sequenced`'s, the latter also covering a
chain the sequencer refuses).  **No new timing constant**: every frame of
both runs composed from the existing `M4A_CYC_*` values at the first
render; nothing was fitted.

**Gate a** (`claim_check.sh` against `2026-10-25_two-cry-rows.json` after
the engine change, before the new tests): 57 tests diffed, **0 moved
writes**; harness 55/64 (the nine documented FAILs), 0 regressions, every
one of the 64 rows 0.0 in null and worst (the -inf rows -inf: compare's
"rows that moved" empty); unit 1471/1471, blip 49112/0, songs 561/561.  A
lone player and the cry 0 -> cry 1 chain did not move by a cycle.

**Gate b** (the two step-12 runs with `--seq-log` / `--psg-log` against
`build/probe/cry_over_se.txt` / `cry_over_bgm.txt`; 3.0 s each): (a)
`--song build/accuracy/rom/songs/se_exp.s --player se1 --cry2 0
--cry2-start-frame 30`: `entry_diff` 176 frames compared, 0 differ, max
|d| 0 (the 180-frame probe's frames 3..178); `cmd_diff` 0 of 178;
`track_diff` 0 of 178; `probe_diff` threshold 0: mgba 284 writes, engine
284, max |d| 0 cycles.  (b) `mus_littleroot.s --cry2 0 --cry2-start-frame
60`: `entry_diff` 176 / 0 / 0; `cmd_diff` 0 of 178; `track_diff` 0 of 178;
156 writes against 156, max |d| 0.  Frame 62's 7136 = 3820 + 79 + 3237 with
the steal's 70 (`NOTE_UNLINK_11` 66 + `CHAIN_EMPTY` 4) and the BGM's -32
tick frames after it reproduced; with `chain_owner_of` disabled (a
negative check of gate d's test, reverted) the engine reads 7114 on frame
62 (-22) and 3232 on 63 (+123: the spurious dead drop, `WALK_DEAD` 59 +
`WALK_UNLINK_*` + the walk, instead of the GBA's -32) -- step 12's
prediction to the cycle.

**Gate c** (`metrics.compare_files` of each render against
`build/scratch/cry_over_se.wav` / `cry_over_bgm.wav`, the `mgba_render
--type song --id 33 --id2 0 --start-frame2 30 --frames 182` / `--id 405
... --start-frame2 60` references; not baseline rows): (a) null **-55.0
dB**, worst 1 s **-52.7** (at 0.0 s), lag 0.0 ms, gain 0.999999; (b) null
**-49.6 dB**, worst **-46.4** (at 1.6 s), lag 0.0, gain 0.99997.  Every run
above 20 LSB attributed (the skill's run finder, both channels, against
the raw logs' `L timer0` lines = the `.events` slot crossings): (a) 4 runs
per channel, each one PCM sample (0.3..1.4 samples at 48 kHz), at 0.7403 /
0.7742 / 0.8073 / 1.0751 s -- four of the 6 crossings (0.7401 / 0.7740 /
0.8071 / 1.0750), each 0.19..0.21 ms after its crossing (the slot's
half-period); the crossings 0.5392 / 0.6062 give |d| <= 20 on both
channels (the slipped sample equals its neighbour: the SE's first 0.54 s
are PSG only, the cry not yet started).  (b) 17 runs per channel, each one
PCM sample, every one 0.19..0.21 ms after one of the 20 crossings; the
crossings 0.2382 / 0.3052 give |d| <= 20 on both channels and 1.8116 s a
run on the right channel only (left 2.0797 s only).  No run at a note
event (the steal on frame 62 = 1.04 s has none), no 64-sample block, no
whole frame: the PCM scheduler-lateness floor and nothing else.  The
levels sit between the one-cry rows' (-60 / -45): a PCM song's floor with
the cry's DPCM jumps added.

**Gate d** (`test/test_engine.c`, `test_sequencer_cycles_cry_over_song`:
unit 1471 -> **1521/1521**): the frame skeleton of both runs on frames
3..178 against the probes' CgbSound entries (two 176-value tables), played
from the ROM's compiled `build/accuracy/rom/songs/se_exp.s` /
`mus_littleroot.s` with their vanilla voicegroups (`rs_sfx_1` /
`littleroot` from `~/pokeemerald`; the runs are skipped with a message
when either is absent) and a stand-in DPCM sample of Cry_Bulbasaur's 8183
samples at 10512 Hz; cry 0 chained after SE1 at trackBase 3 / after BGM at
10 with one hook (the song's); `m4a_cry_choose_player` over `{cry 0, NULL}`
0; the state `ply_note` scans on the start tick (run (a): no DirectSound
channel ON, the free take of channel 0; run (b): five ON, channel 1 ON |
STOP on BGM track 1 with `chainOwner[1] == 2`), then after it the cry's
channel with `trackIndex` = cry 0's engine track, cry 0's `chainOwner` 1,
the BGM's `chainOwner[1]` **0 at once** and the same track's other
releasing channel 0 still chained; the cry's channel ending through the
count path after frame 78 / 108 (the unlink ticks 79 / 109; never ON |
STOP); run (a)'s EOT 172 finding the chain empty; the last member
publishing the entry, both players counting the same frames; one TIE on
cry 0; detaching cry 0 leaves the song's hook.  And the chain
constructors: the next-player form after cry 0 still selects cry 1; the
explicit form refuses a player not above its inner's, a trackBase inside
the inner's tracks, SE2's 9 tracks at 10 (past 16) and an inner that
already has an outer, and accepts BGM -> SE1 (10..12) -> cry 0 (13..14).

**Tool changes**: none (`entry_diff` / `cmd_diff` / `track_diff` /
`probe_diff` read the chained log as for two cries).  **What slice 9b-ii
needs** (historical since slice 9b-ii, 2026-10-25: done as written below,
see "Done: milestone 4 slice 9b-ii"; the "today" sentences describe the
runner before it): `songs.json` keys on `se` / `song` entries -- the same `"id2"` /
`"start_frame2"` pair (both or neither, the runner's range checks 0..65535
/ 1..255) in the song / SE branch of the runner's loader, which today
builds `cry2` / `cry2_start_frame` only for `kind == "cry"`; the runner's
two lines -- `mgba_render ... --id2 N --start-frame2 F` (already emitted
for any entry carrying `cry2`) and `poryaaaa_render --song <.s> [--player
seN] --cry2 N --cry2-start-frame F` in the song / SE branch (today only in
the `t.get("cry")` branch), `--input midi` skipping such an entry as it
skips cries; `claim_check.sh`'s SE / song argument lines gaining the two
flags from the keys as its cry line has them; two rows, `cry_over_se`
(se_exp, id2 0, start_frame2 30, 3 s; expected the gate-c -55.0 / -52.7)
and `cry_over_bgm` (mus_littleroot, id2 0, start_frame2 60, 3 s, or 4 s to
cover the EOT 202; -49.6 / -46.4 at 3 s); the baseline with the two new
rows, the 64 existing identical; and `claim_check.sh`'s probe-name rule
(`case $n in cry_*)` resolves a `cry_*` probe log against `type == "cry"`
entries only), so rows named `cry_over_*` of type se / song need that case
or other names.

**Code review of slice 9b-i (2026-10-25).**  Two independent read-only
reviews (reviewer 1: MPlayOpen's order and MPlayMain's call-before-body
against `m4a_sequencer_frame`'s gap term, each paused player charged once
for BGM -> cry 0 and SE1 -> cry 0 and the lone / cry 0 -> cry 1 paths
unchanged, the constructors operand by operand against the pre-diff
`init_chained`; `_081DDC40`'s ClearChain / RealClearChain stores in the
four prev / next cases against `chain_owner_of` + `unlink_cost` on the
owner, the clearing moment, the raw f63 steal line for line (prev
0x03001080 / next 0x03000FC0 -> 66 + the taker's empty chain 4 = 70), the
CGB path reaching the same take, the uniqueness of a channel's holder
(every store of a nonzero `chainOwner` first clears any holder), every
consumer of the former owner's state (walk, apply, fine, stop, endtie,
`player_free`), the track-address order of `gMPlayTrack_BGM` 0x03000040 <
SE1 0x03000360 < SE2 < SE3 < `gPokemonCryTracks` 0x03002258 against the
engine index order of every accepted chain and the scan's `bls` / `bcc` /
`bcs`, `MAX_TRACKS` and `set_player`'s range check, the hook's frameCount
arithmetic, `m4a_cry_choose_player` over {cry 0, NULL} against
`SetPokemonCryTone`, the refusals, the `--cry --cry2` path, writers,
`finished`, `sCry2`'s lifetime, detach order and frees; reviewer 2: the
composition of every frame kind for both chains from `set_player`'s
formulas against steps 4 / 5 / 11 / 12, no constant or `cyc()` added, the
two 176-value tables recomputed from the `.frames` files with 0
mismatches and the row comments, the steal's registers and
(0x03000090 - 0x03000040) / 0x50 = track 1 from the map, the 50 new
assertions, the CLI checks and messages against `print_usage`, the error
paths' frees, every number of the plan section, the "what 9b-ii needs"
paragraph against `accuracy.py`'s loader branches and `claim_check.sh`,
the README / skill text, stale sentences).  **No divergence; every
measured number confirmed; no constant derived or changed.**  Acted on
(docs and one header comment, no code behaviour change; `claim_check`
after: 57 diffed, 0 moved, 55/64, 0 regressions, every row 0.0, unit
1521/1521, blip 49112/0, songs 561/561): the `init_chained` header comment
(it selects inner's + 1 itself and refuses cry player 1 as inner), the
README's player list (bgm included), a historical note on step 12's
"today" sentences, `claim_check.sh`'s `cry_*` name rule in the 9b-ii
paragraph above.  Recorded, not acted on: a dead channel (status 0,
`SCAN_FREE`) still held by another player's chain is now unlinked from
that owner at once with the measured unlink cost, as the GBA's
RealClearChain does through `channel->track` (before 9b-i it would have
been charged `UNLINK_NONE` and dropped by the owner's walk) -- the GBA
behaviour, unexercised by the two runs; `init_chained_player`'s `trackBase
>= MAX_TRACKS` test and `render_sequenced`'s `player > SE3` test are
redundant with `set_player`'s range check and `main`'s check; the
two-cry form's "Second cry: StartCry ..." line now prints before the
sCry2 stores (stdout order only); the unit test's `on == 5` loop is the
engine's default `maxPcmChannels` 5 hard-coded; the test skips both runs
with a message, not a counted skip, when `build/accuracy/rom/songs` or
`~/pokeemerald` is absent (the existing pattern; the constructor
assertions still run), and reads the song path relative to the repo root
as `claim_check.sh` runs it; `CryOverRun.eot` uses 0 for "past the run"
where `stolenFromTrack` uses -1; `m4a_sequencer_frame`'s head comment
cites the two-cry statement only; `track_apply`'s dead branch against the
GBA's `_081DD9C8` pass was not re-checked by this review (pre-existing).

### Done: milestone 4 slice 9b-ii, the cry-over-song corpus rows (2026-10-25)

`songs.json`, the runner, `claim_check.sh`, the baseline and docs only: no
engine, plugin, `cmd/`, ROM, reference-definition or `test_engine.c`
change; the two runs of slice 9b-i gate c become corpus rows.  Before any
change, `claim_check.sh` against `2026-10-25_two-cry-rows.json` reproduced
the baseline (57 diffed, 0 moved, 55/64, 0 regressions, every row 0.0;
unit 1521/1521, blip 49112/0, songs 561/561), and the probe logs
`build/probe/cry_over_se.txt` / `cry_over_bgm.txt` (+ `.frames`, 179
`frame` lines each) were in place from slice 9a.

**The keys.**  An `"se"` or `"song"` entry may carry the same `"id2"` /
`"start_frame2"` pair a cry entry may (both or neither; 0..65535 /
1..255, the ROM's `u16 id2` / `u8 startFrame2`; a violation is a warning
and a skip, the checks now shared by the two loader branches as
`second_cry_ok`): gCryTable[id2] at CRY_MODE_NORMAL's defaults, its
`StartCry` on VBlank start_frame2 (start tick on engine frame + 2), over
the SE / song.  Since the loader has always taken the entry's name as the
song it plays (`songs.h`, `midi.cfg`, `song_table.inc`, the `.s` path) and
the two rows are named after their probe logs, such an entry also carries
`"song"`: the name of the SE / song it plays (default: the entry's name,
so every existing row is unchanged).  **The runner lines**: `mgba_render
... --type song --id <song> --id2 N --start-frame2 F` (the line already
appended the two flags for any entry with `cry2`; verified, not
duplicated -- `StartTest` runs the second-cry block after its type switch,
slice 9a's premise) and `poryaaaa_render --song <.s> ... [--player seN]
--cry2 N --cry2-start-frame F` (after `--player`, the song / SE branch
only: a cry entry's own `--cry2` line is untouched); `--input midi` skips
such an entry with a message ("a cry over the song has no MIDI; skipped
with --input midi"), as it skips cries.  The reference frame count is the existing `seconds ->
frames + 2`: 182 for both rows.  `claim_check.sh`: a probe log named
`cry_over_*` is resolved against the SE / song entry of that name (its
`"song"` for the `.s`, `--player` from `song_table.inc` for an SE, then
the two flags from the keys) before the `cry_*` rule, which looks only
among `type == "cry"` entries and would have skipped both logs; the
`cry_over_se_se` / `cry_over_bgm_bgm` twin logs (the same runs read from
the other player's symbol) match no entry and stay skipped as before.

**The two rows** (`songs.json`): `cry_over_se` (type se, song se_exp, id2
0, start_frame2 30, 3.0 s) and `cry_over_bgm` (type song, song
mus_littleroot, id2 0, start_frame2 60, 3.0 s).  Both kept at 3 s so the
existing 180-frame probes cover them; the BGM cry's EOT on frame 202 is
past the row (its chain is empty from the count-path death on frame 108,
so 4 s would add only the tick's xWAIT; the note says so).  **Gate a**:
(a) null **-55.0 dB**, worst 1 s **-52.7**, lag 0.0, gain -0.0 dB, ref /
test -23.5 dBFS; (b) null **-49.6 dB**, worst **-46.4**, lag 0.0, -23.1
dBFS -- slice 9b-i's gate-c numbers to the digit, both PASS at -40; the
two new references are byte-identical (`cmp`) to gate c's
`build/scratch/cry_over_se.wav` / `cry_over_bgm.wav`, the `mgba_render
--type song --id 33 / 405 --id2 0 --start-frame2 30 / 60 --frames 182`
lines in the refs' `.log`s; the residual the PCM scheduler-lateness floor
attributed run by run in 9b-i (6 / 20 `timer0 slot_crossed` in the
`.events`).  `compare_reports.py` against `2026-10-25_two-cry-rows.json`:
0 regressions, every one of the 64 existing rows 0.0 in null and worst
and identical in every `report.json` field, the `ref` / `test` paths
included (checked with python), the two new rows "only in after".  **Gate b**:
`claim_check.sh` against the previous baseline -- **59 tests diffed**
(57 + the two new logs: the SE's 284 and the BGM's 156 mgba writes, max
|d| 0 cycles each, the engine lines rebuilt as `--song
build/accuracy/rom/songs/se_exp.s --player se1 --cry2 0
--cry2-start-frame 30` / `--song .../mus_littleroot.s --cry2 0
--cry2-start-frame 60`), **0 moved**; harness **57/66**; unit 1521/1521
(unchanged, no test change), blip 49112/0, songs 561/561.  `--input midi
--tests cry_over_se,cry_over_bgm,se_exp` skips the two with the message
and runs se_exp.  **Gate c, the baseline**:
`test/accuracy/baselines/2026-10-25_cry-over-song-rows.json` / `.txt`
from the full default run (gate b's harness stage, the last run before the
copy), 66 rows, **57/66** at -40 dB, the nine documented FAILs unchanged
(se_rain_stop, cry_pidgey, cry_jynx, cry_bulbasaur_howl,
cry_pidgey_quiet, cry_bulbasaur_reverse, cry_pidgey_echo_start,
cry_pidgey_echo_start_chorus, cry_jynx_high_pitch); the `.txt` differs
from the previous one by the two appended rows and the summary line only.
Reproduced once with `claim_check.sh` against it after the commit: 59
diffed, 0 moved, 57/66, 0 regressions, every row 0.0; unit 1521/1521,
blip 49112/0, songs 561/561.

**Code review of slice 9b-ii (2026-10-25).**  Two independent read-only
reviews (reviewer 1: the runner's `mgba_render` line against
`mgba_render.c`'s type-independent `--id2` / `--start-frame2` parse and
AccParams patch `b[12..14]` and `main.c`'s `StartTest` running the
second-cry block after its type switch with `StartCry(id2, 0, 120, 15360,
140, 0, 0)`; the `poryaaaa_render` line against the order-independent argv
loop, the 0..65535 / 1..255 ranges, the "go together" / "needs --cry or
--song" / player <= SE3 checks, the BGM default without `--player`,
`render_song` -> `load_cry2` -> `render_sequenced`'s `song2` branch;
`second_cry_ok` against the previous inline checks (same conditions and
messages, both call sites `continue`), the `"song"` key reaching every
lookup (songs.h, midi.cfg, song_table.inc, the .mid / .s paths) while
`name` stays the row, no existing entry changed, a `"song"` on a cry entry
ignored; `frames_for(3.0)` = 182 against both refs' `.log`s; the midi skip
before any command; `claim_check.sh`'s `overs` block and awk re-run on the
real files (`--player se1   --cry2 0 --cry2-start-frame 30` / `--cry2 0
--cry2-start-frame 60`), the `cry_over_*` case before `cry_*` (bash takes
the first match; after it the two logs would skip silently), the
`_se` / `_bgm` twin logs and the `_p1` logs skipped as before, every other
probe log's line unchanged, 179 lines -> the 3.0 s rung; reviewer 2: every
field of the two rows against the `.frames` / raw logs / `.events` (ids
33 / 405, SE1 / BGM, gCryTable[0], the start ticks 32 / 62 at 5672 / 7136,
the free take of channel 0 from `_081DDC40 chan=0` on raw f33, the steal
of 0x03001000 from track 0x03000090 on raw f63, the -32 / -127 / +126 /
+84 / -28 / +172 table recomputed against the alone run, no `_081DCFB0`
stamp for either cry, the SE's EOT 172 / 173 and the BGM cry's 202 past
the log, 6 / 20 crossings); the baseline against the previous one with
python (headers equal, the 64 shared rows identical in every field, two
new rows, the `.txt` diff two rows and the summary), the rows' numbers
against 9b-i gate c and the refs' `.log`s, the refs byte-identical to the
scratch WAVs, the nine FAILs, the README / skill / plan text and a grep for
stale phrases).  **No divergence; no wrong fact or number.**  Acted on
(docs only; `claim_check.sh` after: 59 diffed, 0 moved, 57/66, 0
regressions, every row 0.0, unit 1521/1521, blip 49112/0, songs 561/561):
the cry_over_bgm note's "BGM (10 tracks)" made "9 of the BGM player's 10
tracks" (the song has 9, the player 10); this section's skip message
quoted in full and its "but the ref / test paths" sentence corrected (the
paths are identical too); historical markers on 9b-i's "What slice 9b-ii
needs" paragraph and on step 12's item (4).  Recorded, not acted on:
step 12's "Deaths and unlinks summary" names the cry's unlink tick (79 /
109) as its death frame where 9b-i and the rows name the count-path frame
before it (78 / 108), a pre-existing wording difference inside the plan;
the rows say "180 frames" (the probe's nominal length) where `grep -c
'^frame'` counts 179, both stated correctly where they stand; the runner
still renders and caches the mgba reference of a row it then skips under
`--input midi`, as it always has for cries; `second_cry_ok`'s docstring
repeats the former inline comment; for an SE / song entry the pair check
now runs after the songs.h / type / player checks (a malformed pair on a
row with a bad song name reports the song first).

**What comes next**: the next ground-truth probe is a second cry while
both cry players are busy (the game's `SetPokemonCryTone` replacing the
player with the smaller `maxClock` -- the `--cry2` refusal of slice 8b-i
becomes a measured path; a probe step and a ROM parameter first, no
engine change until the frames are read) or an SE over music (BGM + SE1
in one chain, the SE's PSG writes after the BGM's on the same frame; the
`StartTest` block needs a second song id, so a ROM change under the
boot-path rule).  Either is a probe step first, as 9a was for 9b.

### Done: milestone 4 slice 10a, a third cry while both cry players are busy -- the ground truth (2026-10-25)

The ROM's parameter block, the two patchers, the probe and the
measurements only; **no engine, plugin, `cmd/`, `test_engine.c`,
`songs.json`, runner, `claim_check.sh` or baseline change and no new corpus
row** (probe step 13 above has every number, the rule as read and as
measured, and slice 10b's structural statement).  Before any change,
`claim_check.sh` against `2026-10-25_cry-over-song-rows.json` reproduced
the baseline (59 diffed, 0 moved, 57/66, 0 regressions, every row 0.0;
unit 1521/1521, blip 49112/0, songs 561/561).

**The ROM** (`test/accuracy/rom/main.c`, `crt0.s`): `AccParams` + `u16 id3`
(block offset 24) + `u8 startFrame3` (26) after a padding byte at 23, one
reserved byte left, 28 bytes; `StartTest` waits for `startFrame3` after the
second cry's block and runs `StartCry(id3, 0, 120, 15360, 140, 0, 0)`;
`mgba_render --id3 / --start-frame3`; `mgba_probe` arguments 17 / 18.
`AgbMain` instruction-identical (the relative `bl StartTest` encoding
only); symbol shift **+0x30** from `AgbMain` on (`CgbSound` 0x80010ac, the
`CgbSound_*` labels +0x30 in the skill and the probe header, IWRAM
unchanged); pcm, pcm_cmp, cry_bulbasaur and sq1 rendered on the new ROM
**byte-identical** to the current references (ROM sha1 b3d22c13...; the
harness keys a new refs directory).

**The rule and the measurement**: `SetPokemonCryTone` replaces the busy
cry player with the strictly larger `clock` (ticks since its own
`MPlayStart`; a tie -> player 0), and `MPlayStart`'s `TrackStop` cuts that
player's chained channels outright (`statusFlags := 0` in ROM code, no
release, no `_081DCFB0` stamp).  Measured on Jynx (cry 0, VBlank 1) +
Wailord (cry 1, VBlank 20) + Bulbasaur (VBlank 60): **cry player 0**
replaced (`MPlayStart` r0 0x03001F88 on raw f62, `TrackStop_Loop` /
`TrackStop_1` on channel 0x03000FC0), Bulbasaur's start tick on frame
**62** taking **channel 0** free (`SCAN_FREE` 14), body **2991** label for
label the lone cry's, the frame **4666** = 1007 + (79 + 2991) + (79 + 510);
Bulbasaur's unlink 109 / EOT 202, Wailord's 141 / 162, Jynx's 137 / 143
gone.  Chain rule 177 / 177 frames on the run and on its twin; **no new
constant**.  Crossings 7 / 10.  Reference for 10b:
`build/scratch/cry_third_jynx_wailord_bulbasaur.wav`.

**Gate** (no engine change: the reproduction check after the ROM change):
`claim_check.sh` against `2026-10-25_cry-over-song-rows.json` -- **59 tests
diffed, 0 moved** (the engine's write cycles against the existing probe
logs), **57/66** at -40 dB, `compare_reports.py` **0 regressions**, every
row 0.0, and every one of the 66 rows identical to the baseline in every
`report.json` field but `ref` (the references re-rendered into
`build/accuracy/refs/382db6b7de17-mGBA_0.10.5+m4a_blip/`, all **66 WAVs
byte-identical** to the `56b288f4b49f` directory's by `cmp`); unit
**1521/1521**, blip 49112/0, songs 561/561.  The baseline file stands as
saved (no engine change; only the ref path moved, as in slice 8a).

**Code review of slice 10a (2026-10-25).**  Two independent read-only
reviews (reviewer 1: `AgbMain`'s disassembly before / after instruction by
instruction, the +0x30 shift recomputed from both nm tables, the
`CgbSound` body and each of the 14 labels, the AccParams layout against
`crt0.s` and the ROM bytes at 0xC0, the compiled `StartTest`'s load
offsets 26 / 24 and `StartCry`'s arguments, the wait-loop corners, both
patchers' `b[16..18]` and argv indices, `SetPokemonCryTone` / `MPlayStart`
/ `TrackStop` / `_081DD9A4` read in the source, every frame table, body and
register recomputed from the raw logs (177/177, 237/237), the crossings,
the WAV, the renders against both refs directories; reviewer 2: every
composed cycle value against the step 5 / 11 terms and the header's named
constants, the addresses against both nm tables and the four places the
labels appear, the structural statement line by line against
`m4a_cry.c`, `m4a_sequencer.c` (`player_free`, `start`, `track_stop`,
`clock` at 1328 / 1136, the chain capacities 14 / 7), `m4a_engine.c`
(`kill_channel`), `poryaaaa_render.c` (`sCry2`, `cry2_hook`, the refusal,
the done test, the seq-log track index), the README / skill / probe header
/ Done section, the `_p1` cycle columns, the stale-phrase grep).  **No
divergence in code or in any measured number.**  Corrected in the text
(docs and comments only; no number in a table cell changed): the
twin-vs-third delta on frame 144 is **+384** (2145 against 1761), not -185;
the restart tick's identical span is **94** lines (123 was the whole
frame's breakpoint count); **16** ROM symbols before `AgbMain` (the
compiler's `.gcc2_compiled.` local counted) and the IWRAM / other counts
dropped for the statement that nothing else moved; the `_p1` diff is 222 /
240 lines per log (444 / 480 counted both sides); the r0 / r7 identifying
the replaced player are on the `TrackStop` line, not `MPlayStart`'s (which
prints no registers); `MPlayStart` 611-666 and `TrackStop` 1468-1506; the
`_p1` `trk 1/0/2` reading limited to 23..162; "block bytes 24..26" ->
"block offsets 24..26" in `mgba_render.c`, the skill and the README (the
plan's `b[16..18]` is the patcher index); the structural statement's
"field for field" qualified (the engine's own bookkeeping, no `fadeOI`)
and the `TrackStop_2` / `kill_channel` track-field difference added as a
10b note; the ROM corners (`startFrame3 <= startFrame2` runs the third
`StartCry` on the second's VBlank right after it; `startFrame2` 0 with
`startFrame3` nonzero is a second cry) and the compiled load offsets
recorded.  Recorded, not acted on: the argument numbering counts the
program name as the first argument (consistent with "eighth" / "ninth" /
"fifteenth" throughout); the engine's `m4a_sequencer_start` runs its
second `track_stop` loop to `M4A_SONG_MAX_TRACKS` over its own tracks where
`MPlayStart` loops to the player's `trackCount` (harmless); the hoisted
`frame` computation runs even with both start frames 0 (a few cycles inside
`StartTest` outside SoundMain).  `claim_check.sh` after the edits (a
comment line in `mgba_render.c` the only code-file change): 59 diffed, 0
moved, 57/66, 0 regressions, every row 0.0; unit 1521/1521, blip 49112/0,
songs 561/561.

### Milestone 4 closed (2026-10-25)

Three consecutive ground-truth probes -- 8a (a second cry while one plays),
9a (a cry over a sound effect and over music) and 10a (a third cry while
both cry players are busy) -- were predicted from the source before the
run and matched mgba to the cycle with no new constant (the chain rule
1007 + sum (79 + body), the steal, the player choice).  The engine ports
of 8b and 9b then reproduced their frames and the renders null at the
one-cry rows' floor.  The remaining slices are therefore **deferred as a
decision, not an oversight**:

- **10b, the third cry while both cry players are busy**: poryaaaa refuses
  it today (`--cry2` while both are busy, `m4a_cry_choose_player` -1 with
  neither free); the game replaces the longer-playing player
  (`SetPokemonCryTone`'s strictly-larger `clock`, a tie -> player 0) and
  `MPlayStart`'s `TrackStop` cuts its channels outright.  Everything
  needed is in step 13's structural statement (the chooser's fallback,
  `m4a_sequencer_start` on the busy sequencer, `--cry3` /
  `--cry3-start-frame`, the gates, the corpus row) and the reference WAV is
  `build/scratch/cry_third_jynx_wailord_bulbasaur.wav`.
- **The SE-over-music probe**: BGM and SE1 PSG writes on one frame (the SE's
  after the BGM's in the MPlayMain chain); the `StartTest` block needs a
  second song id, so a ROM change under the boot-path rule (`AgbMain`
  instruction-identical, pcm / pcm_cmp / cry_bulbasaur byte-identical
  before the harness).
- **A note to carry**: `m4a_engine_kill_channel` leaves the engine
  channel's track field where the GBA's `TrackStop_2` zeroes it
  (`channel->track = 0`).  Harmless today -- `m4a_sequencer_player_free`
  reads that field only for a channel the chain still holds -- but any new
  test that consults a killed channel's track field would differ from the
  GBA.

Milestone 5 (MIDI quantization: the design in "Engine milestones" item 5)
follows; its first slice measures the MIDI path against the same references
before changing it.

### Milestone 5 slice 1: the MIDI path's frame latch (2026-10-25)

Scope, small on purpose: the MIDI path's first measurement against the
references and the frame latch alone (an engine flag, default off,
`poryaaaa_render --quantize`).  Plugin, GUI, config and CLAP transport
wiring are slice 2; any mid2agb-equivalent conversion the measurement
points at is slice 3.  **The sequencer path is not touched**: before any
change `claim_check.sh` against `2026-10-25_cry-over-song-rows.json`
reproduced the baseline (59 diffed, 0 moved, 57/66, 0 regressions, every
row 0.0; unit 1521/1521, blip 49112/0, songs 561/561), and that gate must
read the same after.

**Step 1, the MIDI path as it stands** (`accuracy.py --input midi`, the
default test set: 44 rows -- 31 micro, 7 songs, 6 SEs; the 20 cry rows and
the 2 cry-over rows of the 66-row song-input set are skipped since they
have no MIDI):
`baselines/2026-10-25_midi-path-start.json` / `.txt`, **0/44** at -40 dB.
A separate baseline family: the report's `input` field says `midi`; a
midi-input report is never compared against a song-input one.  Lag about
50 ms on most rows (sq1 49.9, sq1_env 50.2, wave 50.1, pcm_adsr 50.2,
pcm_drum 50.2, pcm_reverb 50.2, pcm_bass 50.2, pan 50.2, tune 50.1,
tune_psg 49.7; pcm 34.8, vol 30.8, vel 19.2; pcm_hi 160.5, lfo_trem 110.6,
lfo_psg 196.3; sq1_sweep, noise, pcm_cmp, psg_mix 0.0; sq2 -19.1,
pcm_chord -11.2, lfo_vib -4.2, lfo_delay -3.5, pcm_psg -15.3); on the micro
rows gain from -0.1 (tune_psg) to -8.4 dB (noise), null -12.2 (tune_psg) ..
+0.9 (noise), over all 44 rows gain down to -19.4 (se_truck_unload) and
null up to +2.3 (the songs);
every song drifts 4.2 .. 5.1 ms/s; the `ref` / `test` RMS columns agree
within 0.0 .. 0.4 dB on every micro row and within 1.0 dB on every row
(sq1 -16.9 / -17.0, noise -20.0 / -20.0,
vel -26.6 / -27.0, se_contest_place -27.0 / -28.0), so the gain column is
the least-squares gain of two misaligned signals, not a level offset.

**Step 2, where the two paths differ -- read from the code, predictions
before any change.**  The `--midi` path: `cmd/poryaaaa_render.c`
`parse_midi` (454-620) converts every event's tick to an output sample
with `tick_to_sample` (397-416, the exact tempo map, 500000 us per beat by
default), the render loop (1919-1931) renders up to each event's sample
and calls `dispatch_event` (1279-1310: note on / off, CC, program, bend
straight into `m4a_engine_note_on` / `_off` / `_cc` / `_program_change` /
`_pitch_bend`, the MIDI tempo into `m4a_engine_set_tempo_bpm`), after
`m4a_engine_boot(&engine, true)` (1898) kept the boot's output so sample 0
is ROM cycle 0.  micro.cfg's options are mapped by the runner: `-V127` ->
`--song-volume 127` (`m4a_engine_set_song_volume`), `-R50` -> `--reverb
50` (`m4a_reverb_set_amount`), `-G` -> the voicegroup positional, `-E`
has no flag (the renderer plays the MIDI's own gates).  The `--song` path:
`m4a_sequencer.c` through the frame hook (MPlayMain's body at every
SoundMain), the ROM's `MPlayStart` during frame 2 and the first tick on
engine frame 3 (`publish_cycles`, plan steps 1-3), commands applied at the
tick, mid2agb's conversions baked into the `.s`
(`~/pokeemerald/tools/mid2agb`).  The differences, each with what it
should do to lag, gain and null:

1. **The time origin (the start offset).**  The MIDI's tick 0 is output
   sample 0 = ROM cycle 0; the ROM's song tick 0 is frame 3's SoundMain
   (124773 + 2 x 280896 + 160 x 1232 + 197 = 883882 cycles, **52.68 ms**).
   An event at MIDI time t < the boot's end (602986 cycles, 35.94 ms) is
   dispatched when the loop first renders, i.e. after frame 2's
   SoundMain, and is mixed in frame 3's -- the same frame as the ROM's
   first tick, by the boot's construction.  Every later event is
   dispatched at t from cycle 0 instead of t from frame 3, i.e. mixed
   **3 frames (842688 cycles, 50.23 ms, 2411 samples) early**.  Predicted:
   the lag column reads about 50 ms on every row whose energy is in the
   notes after the first; the first note cannot be aligned at the same
   time, so the null stays shallow whatever lag is chosen; the gain column
   is noise.  Measured before the latch, pcm's onsets (ref / song / midi):
   note 1 at 70.54 / 70.54 / 70.54 ms, note 2 at 824.0 / 824.0 / 773.7,
   note 3 at 1577.4 / 1577.4 / 1527.1.  The latch cannot change it: an
   event latched to the next tick is applied in the same frame it was
   mixed in before (below, 12).  The alignment choice (MIDI time 0 at frame
   3's SoundMain, as the ROM's `MPlayStart` places it) is slice 2's.
2. **The tempo grid.**  mid2agb writes `TEMPO 120*tbs/2` (agb.cpp 506:
   round(60e6 / us-per-beat) = the BPM; `tbs` = the clock multiplier on 24 clocks per beat, 1 without
   -X), `ply_tempo` doubles the byte (m4a_sequencer.c 399-403, m4a_1.s
   ply_tempo): tempoD = 120, tempoI = tempoD x tempoU >> 8 = 120; a tick
   runs when tempoC += tempoI reaches 150 -> **0.8 ticks per frame, 47.78
   ticks/s** at 59.7275 Hz.  The MIDI path places events by
   `tick_to_sample` at the exact 500000 us per beat (24 ticks per beat ->
   **48.00 ticks/s**) and uses tempoI = 120 only for the LFO
   (`m4a_engine_set_tempo_bpm`).  The GBA is 0.456 % slower (48 / 47.782 - 1 = 0.4562 %): predicted
   **drift 4.56 ms/s** on every song (measured 4.2 .. 5.1 before the latch:
   the aligner's 4 s segments resolve it coarsely), the second micro note
   (tick 36) 3.4 ms early on top of (1) and the third (tick 72) 6.8 ms in
   continuous time -- which the frame quantization absorbs for these two
   ticks (both land exactly 3 frames early, measured 50.3 ms each) -- so
   the lag estimates scatter by that much between tests (49.7 .. 50.2 on
   the aligned rows) and later notes decorrelate; a 24-tick note spans 30
   frames on the ROM and 29.86 frames of MIDI time (29 or 30 SoundMains),
   so the test RMS reads 0.0 .. 0.4 dB under the ref.  Not the latch's
   (slice 3: place MIDI ticks on the tempoC / 150 grid as the ROM does).
3. **Velocity.**  mid2agb maps the MIDI velocity through
   `g_noteVelocityLUT` (midi.cpp 639; tables.cpp 124-253: 1..4 -> 4, 5..8
   -> 8, ..., 121..124 -> 124, 125..127 -> 127); the MIDI path passes the
   raw byte.  The micro tests use 127 and (`vel`) 64 and 24, all fixed
   points of the table -> **no effect on any micro row**; on the songs up
   +3 per note: 0.1 .. 0.8 dB for velocity 33 and up, more on quieter
   notes (2.5 dB at 9 -> 12, 12 dB at 1 -> 4); the songs' velocities not
   checked.  Slice 3.
4. **VOL / PAN / BEND / the controllers: identical.**  `VOL v*mvl/mxv`
   (agb.cpp 357) with `-V127` is v x 127 / 127 = v; the engine's CC 7 is
   v x songMasterVolume / 127 = v with `--song-volume 127`
   (m4a_engine.c 1407-1410).  The VOL mid2agb inserts before the first
   note when the track has none (agb.cpp 434-442) is the engine's default
   127.  `PAN c_v+(v-64)` (360) = `(int8_t)(v - 64)` (1412-1413); `BEND
   c_v+(MSB-64)` (513, param2 = the bend's MSB, midi.cpp 507) =
   `(int16_t)(((MSB << 7) | LSB) - 8192) >> 7` = MSB - 64 (the arithmetic
   shift floors; the LSB never carries); BENDR 0x14, MOD 1, LFOS 0x15,
   MODT 0x16, TUNE 0x18 `c_v+(v-64)`, LFODL 0x1A, VOICE: the same numbers
   on both sides (agb.cpp 351-396 / 510, m4a_engine_cc).  Reverb: `-R50`
   -> `pcm_reverb_rev = reverb_set+50` applied by `MPlayStart`
   (m4a.c 661) = `--reverb 50`.  Predicted: no gain or null effect; the
   `ref` / `test` RMS columns agree, which the start baseline shows.
5. **Note length and the one-tick note.**  With `-E` the duration in
   clocks is the MIDI ticks (24 clocks per beat, 24 ticks per beat; midi.cpp
   641), a zero-length note becomes 1 clock (643-644); the gate counts
   down in MPlayMain's chain walk at the top of the track's tick
   (m4a_1.s 1192-1204), before that tick's commands.  The MIDI path
   dispatches the note-off at its own sample, mid-frame; the release
   starts at the next SoundMainRAM -- the same frame as the ROM's when the
   tick grid matches (it does not: 1 and 2).  A note shorter than the gap
   to the next SoundMain leaves the channel START | STOP, which
   SoundMainRAM kills silently (m4a_1.s `_081DCF6A` -> `_081DCFB0`,
   statusFlags := 0; m4a_channel.c 118-122; on the CGB side CgbSound's
   oscillator_off, m4a.c 987-1004, m4a_channel.c 639) where the
   GBA plays the one-clock note mid2agb wrote.  No micro note is that
   short (the shortest are pcm_drum's and pcm_adsr's 6-tick notes, 7.5
   frames) -> no harness effect; the latch's
   same-frame rule (a latched note-off against a channel still START sets
   gateTime 1, so the next tick's gate walk stops it, as the GBA's N01)
   restores the one-tick note for the plugin.
6. **The order of a frame's work.**  MPlayMain per track: gate walk,
   commands, LFO step, then the VOLCHG / PITCHG pass, then CgbSound.  The
   engine's own tick (m4a_engine.c `m4a_engine_tick`): hook, c15, PCM
   gates, the CGB gates interleaved with the CgbSound passes, then the
   tempo accumulator and `m4a_lfo_tick`, portamento, PWM.  So on the MIDI
   path the LFO value of frame k reaches the PSG registers at frame
   k + 1's CgbSound, one frame later than the GBA's; PCM channels read the
   LFO'd frequency at SoundMainRAM time (the same frame).  Predicted: the
   CGB LFO row (lfo_psg) stays uncorrelated whatever the latch does; the
   PCM LFO rows are bounded by 1 and 2 only.  An engine-order change for
   slice 3, under the sequencer-untouched rule (the `!seq` branches only).
7. **The CgbSound entry.**  Without a sequencer `cgbEntryCycles` is the
   steady one-track value (`m4a_engine_init`: `M4A_CYC_CGB_ENTRY_BASE +
   M4A_CYC_PER_TRACK - M4A_CYC_SOUNDMAIN_ENTRY`) on every frame; the
   ROM's varies with MPlayMain's work (the micro probes' `.frames`, cycles
   from line 160: frame 3's start tick 4069 sq1 / 4088 pcm / 5125 lfo_vib /
   10304 psg_mix, a steady frame 1402 without a tick and 1866 with one; the
   MIDI path's fixed 1205 after SoundMain is 1402 from line 160, the ROM's
   no-tick value, 464 cycles early on every tick frame).  The
   PSG writes land hundreds to thousands of cycles off on note frames.
   Squares are insensitive (duty phase free-running), so their shallow
   nulls are 1 and 2; wave and noise phase runs on the CPU clock, so wave
   / wave_alt (-8.4 / -7.7) and noise (+0.9) would not null even with 1
   and 2 fixed.  Not the latch's.
8. **Track index.**  A type-1 MIDI's SMF track number is the engine track
   (`dispatch_event` 1284; the conductor track is 0, the music track 1)
   where the sequencer's first track is 0.  It only enters the CGB steal
   tie-break between tracks (the same relative order) and the track count
   (16 on the MIDI path, 10 on the BGM player).  No micro or song effect.
9. **The 13379 Hz grid phase** (the skill's statement that the two paths
   never null each other on PCM) predates milestone 3: both paths now mix
   the same SoundMainRAM frame from the same channel state, so a note
   assigned to the same frame on both is bit-identical.  Measured: the
   MIDI and song renders of pcm are **identical sample for sample from 0
   to 25079** (0.5225 s, the MIDI path's note-off of note 1, three frames
   before the ROM's); the statement is retired below once the measurement
   confirms it.
10. **Program change** is a tick command (VOICE) on both sides; the latch
    includes it so a VOICE and the note after it stay in order.
11. **Tempo** (`m4a_engine_set_tempo_bpm`) is read by the accumulator at the
    tick, so an unlatched tempo set between two ticks is used by the next
    tick either way: not latched (also the plugin's transport tempo).
12. **The latch itself** is predicted **harness-neutral on the renderer**:
    an event dispatched between SoundMain k and k + 1 is applied before
    SoundMain k + 1's CgbSound and SoundMainRAM whether it is applied at
    once or at the top of tick k + 1, and no MIDI-path entry point writes
    hardware at call time (`preserve_stolen_cgb` runs only in the
    polyphony-debug invert mode; `m4a_engine_kill_channel`'s CgbOscOff is
    reached from CC 120 only (CC 123 is `all_notes_off`, STOP only), which
    no micro or corpus MIDI carries).
    Predicted: every row of the latch baseline equal to the start baseline
    and every WAV byte-identical.  What the latch buys is the plugin's
    sample-accurate CLAP event loop (`m4a_plugin.c` `plugin_process`,
    397-437: events applied between sub-block renders, i.e. mid-frame) --
    the GBA's frame-edge note start, the order guarantee within a frame
    and the one-tick note (5).

**Step 3, the latch** (`plugin/m4a_engine.h` / `.c`; default off; no
sequencer-path change).  `engine->quantizeEvents`,
`m4a_engine_set_quantize_events(engine, bool)`: with it on, the event entry
points -- `m4a_engine_note_on` (returns `M4A_CHANNEL_QUEUED`, -2),
`_note_off`, `_cc`, `_program_change`, `_pitch_bend`, `_xcmd` -- called
outside a tick push an `M4AQueuedEvent` onto `eventQueue` (1024 entries;
a full queue is applied at once, in order, before the event queues, counted
in `eventQueueOverflow` -- the review fix below) instead of acting.  `m4a_engine_tick` sets `inTick`
for its duration (an entry point called from the frame hook, the
sequencer's `ply_*`, always applies at once), runs the hook, c15, the PCM
gate walk and the CGB gate walk (the latter moved out of the CgbSound
pass loop into its own loop just before -- each channel's countdown still
precedes its own pass, and the `!seq` guard leaves the song path
untouched), then **drains the queue in arrival order**, then the CgbSound
passes, the tempo accumulator / LFO, portamento and PWM as before: the
events land where MPlayMain's commands do, after the chain walk and before
CgbSound.  The same-frame rule: a latched note-off that finds its channel
still `START` sets `gateTime = 1` instead of `STOP`, so the next tick's
gate walk stops it -- the GBA's one-tick note (mid2agb's one-clock
minimum), where `START | STOP` would be killed silently by SoundMainRAM
(`_081DCF6A` -> `_081DCFB0`; m4a_channel.c the same).  With the latch off
that case still yields `START | STOP` (unchanged; slice 2 decides whether
the live mode wants the one-tick note too).  `m4a_engine_all_sound_off`
outside a tick drops the queued events (a panic must not release queued
note-ons next frame); inside a drain (CC 120) it only silences, so the
events after it keep their order.  `m4a_engine_set_quantize_events(...,
false)` flushes the queue in order (`m4a_engine_flush_events`).  Tempo
(`m4a_engine_set_tempo_bpm`) and `program_change_from` (the sequencer's)
are not latched.  `poryaaaa_render --quantize` (the `--midi` path only;
a note with `--song` / `--cry`); `accuracy.py --midi-quantize` passes it
through and the report carries `"midi_quantize"`.  Unit tests
(`test_event_latch`, 57 assertions): a note-on mid-frame starts at the
next tick with the flag and at once without; a note-off before the tick
waits for it; order preserved (program 0, note 60, program 1, note 62:
each note takes the program queued before it); the same-frame note-on /
note-off yields START without STOP and gate 1 at its tick and STOP at the
next, on a PCM and on a CGB channel (the latter's own tick's CgbSound pass
takes it START -> attack, the next tick's gate walk STOP -> release); CC
7, bend, BENDR and XCMD latched the same way and the bend reaching the
sounding channel at the tick; all-sound-off dropping the queue; disabling
the latch flushing in order; the 1025th event applying at once and
counted (since the review: a note-on queued first and its note-off
overflowing, the note-off still reaching its note); a note-on from the
frame hook applying at once with the latch on.  Unit **1578/1578** (1521 + 57).

**Step 4, the measurement** (`accuracy.py --input midi` without and with
`--midi-quantize` on the slice's build, against the same references;
`baselines/2026-10-25_midi-path-start.json` is the pre-change run,
`2026-10-25_midi-path-latch.json` the latched one).  Prediction 12 held
exactly: the latch-off run of the new build equals the start baseline in
every field of every row (`compare_reports.py` 0 regressions, 0 rows
moved; a field-by-field diff of lag, gain, null, worst, gm null, test RMS
and drift: 0 differences on 44 rows), the latched run equals it the same
way, and the 44 WAVs of the two runs are **byte-identical** (`cmp`).  So
the table below is both the before and the after (the gate, 0/44, as
expected):

| test | lag ms | gain dB | null | worst1s | ref / test dBFS | drift |
|------|-------:|--------:|-----:|--------:|----------------:|------:|
| sq1 |   49.9 |   -2.4 |   -3.0 |   -1.3 |  -16.9 /  -17.0 | - |
| sq1_env |   50.2 |   -1.6 |   -4.8 |   -1.2 |  -18.4 /  -18.4 | - |
| sq1_sweep |    0.0 |   -5.4 |   -0.3 |    2.7 |  -17.0 /  -17.1 | - |
| sq1_alt |   49.8 |   -5.0 |   -0.6 |    1.7 |  -16.9 /  -17.0 | - |
| sq2 |  -19.1 |   -3.1 |   -2.2 |   -0.9 |  -18.1 /  -18.2 | - |
| sq2_alt |   47.9 |   -5.7 |   -0.2 |    2.6 |  -18.2 /  -18.3 | - |
| wave |   50.1 |   -0.5 |   -8.4 |   -6.2 |  -19.8 /  -19.9 | - |
| wave_alt |   49.8 |   -0.7 |   -7.7 |   -7.5 |  -19.8 /  -20.0 | - |
| noise |    0.0 |   -8.4 |    0.9 |    4.1 |  -20.0 /  -20.0 | - |
| noise7 |   67.0 |   -5.6 |   -0.2 |    2.4 |  -18.5 /  -18.8 | - |
| pcm |   34.8 |   -0.9 |   -6.4 |   -2.0 |  -21.4 /  -21.6 | - |
| pcm_hi |  160.5 |   -2.0 |   -3.6 |   -1.1 |  -20.9 /  -21.0 | - |
| pcm_adsr |   50.2 |   -2.8 |   -2.5 |    5.5 |  -29.9 /  -30.0 | - |
| pcm_drum |   50.2 |   -0.9 |   -6.6 |   -3.6 |  -22.0 /  -22.1 | - |
| pcm_cmp |    0.0 |   -4.5 |   -0.9 |    3.4 |  -27.1 /  -27.1 | - |
| pcm_reverb |   50.2 |   -0.8 |   -7.1 |   -1.4 |  -21.0 /  -21.1 | - |
| pcm_chord |  -11.2 |   -6.0 |    0.0 |    2.3 |  -14.2 /  -14.3 | - |
| pcm_bass |   50.2 |   -2.2 |   -3.4 |   -0.9 |  -25.2 /  -25.3 | - |
| pan |   50.2 |   -1.3 |   -5.2 |   -0.9 |  -19.6 /  -19.7 | - |
| vol |   30.8 |   -1.4 |   -4.6 |   -2.3 |  -25.3 /  -25.6 | - |
| vel |   19.2 |   -0.8 |   -5.9 |   -0.8 |  -26.6 /  -27.0 | - |
| bend |   55.7 |   -7.7 |    0.7 |    3.3 |  -20.2 /  -20.3 | - |
| lfo_vib |   -4.2 |   -4.2 |   -1.2 |   -0.5 |  -20.1 /  -20.2 | - |
| lfo_trem |  110.6 |   -1.4 |   -5.2 |   -0.7 |  -20.2 /  -20.3 | - |
| lfo_pan |   64.8 |   -1.4 |   -5.1 |   -0.7 |  -20.2 /  -20.3 | - |
| lfo_delay |   -3.5 |   -3.0 |   -2.3 |   -1.0 |  -21.0 /  -21.1 | - |
| lfo_psg |  196.3 |   -5.7 |   -0.2 |    2.6 |  -17.2 /  -17.5 | - |
| tune |   50.1 |   -1.0 |   -6.3 |   -2.1 |  -21.6 /  -21.8 | - |
| tune_psg |   49.7 |   -0.1 |  -12.2 |  -10.2 |  -18.1 /  -18.2 | - |
| psg_mix |    0.0 |   -5.3 |   -0.3 |    2.9 |  -15.2 /  -15.4 | - |
| pcm_psg |  -15.3 |   -1.7 |   -4.3 |   -2.8 |  -13.3 /  -13.4 | - |
| mus_littleroot |   67.0 |  -14.8 |    2.1 |    4.0 |  -23.7 /  -23.7 |    4.6 |
| mus_weather_groudon |  105.9 |   -5.2 |   -0.5 |    4.5 |  -31.1 /  -31.1 |    4.2 |
| mus_vs_wild |  117.2 |  -14.1 |    2.1 |    3.6 |  -20.9 /  -20.9 |    4.2 |
| mus_mt_chimney |   67.0 |  -13.2 |    1.9 |    3.5 |  -23.5 /  -23.5 |    4.2 |
| mus_rg_vs_trainer |  100.5 |  -16.5 |    2.3 |    3.7 |  -19.6 /  -19.6 |    4.2 |
| mus_vs_trainer |  117.2 |  -13.2 |    1.9 |    3.7 |  -20.4 /  -20.4 |    4.2 |
| mus_credits |  155.4 |  -16.2 |    2.3 |    4.8 |  -23.0 /  -23.0 |    5.1 |
| se_exp |   57.1 |  -14.3 |    1.9 |    2.2 |  -24.8 /  -25.3 | - |
| se_faint |    0.0 |   -3.9 |   -1.5 |    2.4 |  -29.9 /  -29.6 | - |
| se_contest_place |   50.2 |   -4.3 |   -0.8 |   -0.8 |  -27.0 /  -28.0 | - |
| se_truck_unload |   12.3 |  -19.4 |    2.2 |    2.4 |  -39.2 /  -39.9 | - |
| se_low_health |   50.1 |   -2.1 |   -3.6 |   -2.5 |  -24.0 /  -24.1 | - |
| se_rain_stop |    0.0 |   -0.7 |   -4.9 |    1.8 |  -41.3 /  -42.3 | - |

Attribution of the residual, per step 2's list, with the numbers:

- **The time origin (1) is the lag.**  The ROM runs tick n on frame
  3 + ceil(1.25 n) (`MPlayStart`'s tempoC 0 and tempoI 150 give the start
  tick on frame 3, then TEMPO 120 -> 0.8 ticks per frame); the MIDI path's
  tick n is dispatched at n x 349525.33 cycles from cycle 0 and mixed on
  frame 3 + ceil(1.2443 n - 3.147).  The difference is **3 frames for
  every tick that is a multiple of 4 up to about tick 150** -- pcm's 0, 24,
  36, 60, 72, 120, and the micro set's usual 24 / 36 / 48 / 72 / 96 / 120
  -- **and 4 frames on other ticks from tick 21 on** (n = 1 mod 4 from 21,
  2 mod 4 from 66, 3 mod 4 from 110; review of slice 1, recomputed from
  the 31 MIDIs): bend (21, 33, 45, 57, 66, 69, 78, 81, 90, 93, 102, 126),
  pcm_adsr's note-off at 78, psg_mix (78, 90, 102, 114), lfo_vib /
  lfo_trem / lfo_pan (102, 126), vol's ramp (110 .. 130).  pcm's measured
  onsets (note 2 at tick 36: 773.7 vs 824.0 ms, note 3 at tick 72: 1527.1
  vs 1577.4: both 50.3 ms = 3 frames of 16.743 ms early) and the lag
  column (49.7 .. 50.2 on sq1, sq1_env, sq1_alt, wave, wave_alt,
  pcm_adsr, pcm_drum, pcm_reverb, pcm_bass, pan, tune, tune_psg,
  se_contest_place, se_low_health: 14 rows; sq2_alt 47.9) say so.  **The
  lag is the ROM's start offset: the first tick on frame 3 (883882
  cycles, 52.68 ms, which the boot's construction already hands the first
  frame's events), while MIDI time 0 sits at ROM cycle 0.**  The first
  note cannot be aligned at the same time as the rest (it is on time),
  which bounds the null at a few dB whatever lag the aligner picks; the
  rows with another lag (pcm 34.8, vol 30.8, vel 19.2, noise7 67.0, bend
  55.7, lfo_pan 64.8, pcm_hi 160.5, lfo_trem 110.6, lfo_psg 196.3, and the
  0.0 / negative ones) are the aligner choosing among equally shallow
  candidates, not a different offset (their null is -6.4 .. +0.9 dB).
  The formulas take an event as applied at the first SoundMain after its
  exact cycle; `m4a_engine_process` runs the machine in 1024-cycle steps
  until the resampler holds the samples, so an event is dispatched up to a
  few thousand cycles late, which moves an event only when it falls that
  close before a SoundMain.  The alignment choice -- MIDI time 0 at frame
  3's SoundMain, as `MPlayStart` on frame 2 places the song -- is slice
  2's; with it, tick n lands on frame 3 + ceil(1.2443 n), **equal to the
  ROM's frame for every micro event except** bend (45, 57, 69, 81, 90, 93,
  102, 126), lfo_vib / lfo_trem / lfo_pan (102, 126), psg_mix (90, 102,
  114) and vol (110 .. 130): one frame early there, the tempo grid (2)
  showing through within the window.
- **The tempo grid (2) is the drift**: 4.2 .. 5.1 ms/s on the seven songs
  against the predicted 4.56 per MIDI second (0.4562 %; 4.54 against ref
  time; the aligner's 4 s segments resolve
  it to the frame), and the test RMS 0.0 .. 0.4 dB under the ref on the
  micro rows (a 24-tick note spans 30 ROM frames and 29 or 30 MIDI-path
  frames).  Slice 3: place MIDI ticks on the tempoC / 150 grid.
- **Velocity (3)**: no micro effect (127, 64, 24 are fixed points of the
  table), as predicted; the songs carry it (slice 3).
- **VOL / PAN / BEND / controllers / reverb (4)**: identical, as the
  `ref` / `test` RMS agreement on every row shows (sq1 -16.9 / -17.0,
  noise -20.0 / -20.0, pcm_reverb -21.0 / -21.1).
- **The one-tick note (5)**: no micro note is shorter than a frame; the
  rule is exercised by the unit test only.
- **The LFO order (6) and the CgbSound entry (7)** keep the PSG rows
  (lfo_psg; wave / wave_alt -8.4 / -7.7; noise +0.9; the squares' duty
  phase excepted) from nulling even once 1 and 2 are fixed; slice 3's
  engine-order item and, for the entry, the question whether the MIDI
  path should charge MPlayMain's costs at all.
- **The mix grid (9) is retired**: the MIDI and song renders of pcm are
  identical sample for sample from 0 to 25079 (0.5225 s, the MIDI path's
  first note-off, three frames before the ROM's); the skill's sentence is
  replaced by the time-origin / tempo-grid statement.
- **The latch (12)**: harness-neutral on the renderer, as predicted, to the
  byte.  Its value is for the plugin (slice 2): the CLAP loop applies
  events between sub-block renders, mid-frame; with the latch they start
  on the frame edge the GBA can start a note on, in order, and a note
  shorter than a frame plays one tick instead of being killed.

**Step 5, the gates.**  `claim_check.sh` against
`2026-10-25_cry-over-song-rows.json` after the change: **59 tests diffed,
0 moved, 57/66, 0 regressions, every row 0.0** -- the song-input gate
unchanged (the unit line of that run read 1575/1578 with three failures
in the new test's own assertions, fixed before the commit: the CGB
channel's status after its own tick's CgbSound pass and the hook test's
missing program change; unit **1578/1578** after); blip 49112/0, songs
561/561.  The two midi baselines saved as above.

**What slice 2 needs.**  (a) The transport-driven default in
`m4a_plugin.c`: `m4a_engine_set_quantize_events(&engine, playing)` from
`CLAP_TRANSPORT_IS_PLAYING` at the top of `plugin_process` (unquantized
while the transport is stopped -- live playing -- and quantized while it
plays; the flip to off flushes what is queued, the flip to on queues from
the next event), plus the manual override (GUI Options menu item, a
`poryaaaa.cfg` key, CLAP state appended bytes, back-compatible like the
other toggles: off / on / transport); the plugin's `all_sound_off` on
reset already drops the queue.  (b) The start alignment for the renderer's
`--midi` path and the harness: MIDI time 0 at frame 3's SoundMain (883882
cycles into the ROM, i.e. dispatch an event at MIDI time t at output time
t + 52.68 ms, or equivalently start the MIDI clock when the boot ends
plus one frame), chosen so that tick n lands on the ROM's frame -- the
measurement above says every micro event then matches up to tick 120 with
the exact tempo still in place; the `lag` column should read 0.0 on the
PCM rows and the PCM rows should null as the song path's do until the
tempo grid (slice 3) separates them.  (c) The sub-frame decision for the
live mode: whether an unlatched same-frame note-on / note-off should play
one tick as well (today it is killed, as the GBA kills START | STOP).
Slice 3 is then the tempo grid (tempoC / 150 per frame, the GBA's
rounding), the velocity table and the LFO / CgbSound order for the MIDI
path, each measured alone with `--input midi` against the latch baseline.

**Code review of slice 1 (2026-10-25).**  Two independent read-only
reviews (reviewer 1: the latch against MPlayMain's frame order in m4a_1.s
-- the chain walk, the track start, the command loop, the LFO step, the
VOLCHG / PITCHG pass, CgbSound and SoundMainRAM -- SoundMainRAM's and
CgbSound's START | STOP handling, the one-clock note, the CGB countdown
move on the song path and both MIDI modes, the shadow pool, the queue's
order, re-entrancy, panic and flush paths, every caller of
`m4a_engine_note_on`'s return value, the plugin's CLAP loop, the renderer
wiring, the unit test assertion by assertion; reviewer 2: step 2 against
mid2agb's main.cpp / midi.cpp / tables.cpp / agb.cpp, MPlayDef.s,
MPlayStart and the ply_* handlers, the renderer's option handling and
conversions, the boot constants and every cycle / ms / frame number
recomputed, the micro MIDIs' event ticks, the runner switch, the baselines
field by field, the docs).  The first launch of both reviews stopped on an
API rate limit before reporting; both were relaunched with the same briefs.

*Acted on, each tested alone with `claim_check.sh`:*

- **The overflow broke arrival order** (reviewer 1): a full queue applied
  the new event at once, ahead of the 1024 queued -- a note-off
  overflowing past its own queued note-on found no channel and left the
  note stuck.  Fixed: a full queue is applied in order first, then the
  event queues.  Harness-neutral by construction (no micro or corpus run
  fills the queue): `claim_check.sh` 59 diffed, 0 moved, 57/66, 0
  regressions, every row 0.0; the latched midi run 0 field differences on
  44 rows against `2026-10-25_midi-path-latch.json`.  Its gate is the unit
  test (the overflow block of `test_event_latch` now queues a note-on,
  fills the queue and checks the overflowing note-off reaches the note).
  Unit **1580/1580**; blip 49112/0, songs 561/561.
- **Drain only on frames with a song tick** (reviewer 1: MPlayMain runs
  its track loop only while tempoC >= 150, one frame in five skipped at
  tempoI 120; the latch drains every frame).  Applied alone (`if (seq ||
  tempoC + tempoI >= 150)` before the drain): **rejected by measurement**
  -- `accuracy.py --input midi --midi-quantize` against the latch
  baseline, **16 regressions** over 1 dB (wave +6.0, wave_alt +6.3, vol
  +6.2, pan +5.1, tune +5.2, vel +5.0, sq1_env +4.0, pcm_adsr +2.0,
  tune_psg +2.1, sq2 +1.9, se_faint +1.8, pcm_psg +1.6, psg_mix +1.5,
  pcm_bass +1.2, mus_weather_groudon +1.0, ...) against 5 improvements
  (lfo_trem / lfo_pan -2.5, pcm_chord -2.2, pcm_hi -1.7, pcm_reverb
  -1.1).  The engine's accumulator on the MIDI path is not the ROM's
  (its tempoC phase starts at boot, tempoI is the MIDI tempo and MIDI time
  0 is not frame 3), so gating on it moves events off the ROM's frames.
  Reverted; the question belongs with the tempo grid of slice 3, where the
  MIDI clock is put on tempoC / 150 and the gating would follow from it.
- **The one-tick note counts frames, the GBA's gate counts song ticks**
  (reviewer 1): the header comment now says so (equal when one frame is
  one tick; at tempoI < 150 the GBA's STOP can come two frames after the
  start, at tempoI > 150 a frame with two ticks gives START | STOP and
  drops the note).  No code change: it is the same tick-grid question.
- **Plan text** (reviewer 2, every item recomputed here before the edit):
  the "exactly 3 frames early" statement holds for ticks that are
  multiples of 4 only -- other ticks are 4 frames early from tick 21 (bend,
  pcm_adsr's note-off at 78, psg_mix, the three lfo rows at 102 / 126,
  vol's ramp), recomputed from the 31 MIDIs' event ticks; the slice-2
  alignment matches the ROM's frame on every micro event except bend, the
  lfo rows, psg_mix and vol (one frame early there), corrected in the
  attribution, the slice-2 paragraph's claim and the skill; 20 cry rows,
  not 10; the RMS agreement 0.0 .. 0.4 dB on the micro rows and within 1.0
  dB on all (se_contest_place 0.96, se_rain_stop 0.99); 14 rows in 49.7 ..
  50.2 (sq2_alt 47.9); the other-lag rows' null -6.4 .. +0.9; the step-1
  gain / null ranges are the micro rows'; item 2's 3.4 / 6.8 ms absorbed
  by the frame quantization; item 3's velocity effect larger below 33; the
  shortest micro notes 6 ticks (pcm_drum, pcm_adsr); item 7's ROM entries
  from the micro probes (4069 / 4088 / 5125 / 10304 on frame 3, 1402 / 1866
  steady) and the MIDI path's 1402 equal to the no-tick value; CC 123 does
  not reach `kill_channel`; line references (midi.cpp 507, PAN 1412-1413,
  agb.cpp 510); 0.4562 %, 4.54 ms/s against ref time; `tbs` the
  multiplier; the CGB START | STOP via CgbSound's oscillator_off; the
  dispatch caveat (up to a few thousand cycles after the event's cycle);
  README: "after the gate walk" and the start baseline's missing
  `midi_quantize` field.

*Recorded, not acted on:* the step-2 line numbers are the pre-change
file's (accurate at 857f74a; m4a_engine.c moved by about 80 lines and
poryaaaa_render.c by 5 / 10 at the slice commit); the order of commands
within one tick is per track on the GBA and by arrival in the queue, which
matters only when a gate expiring on one track changes another track's
channel scan in the same tick (on the MIDI path gates exist only through
the one-tick rule); the one-tick rule also applies to a note-off applied
by a flush, and CC 123 (`all_notes_off`) inside a drain sets STOP on a
START channel with no one-tick treatment; c15 is decremented before the
drain where CgbSound decrements it at its head (nothing in the drain
reads it); an overflowed CC 0x1D / 0x1F counts two overflows; mid2agb
applies the velocity table twice (idempotent); the accuracy.py docstring's
one long line.  **For slice 2** (the plugin, from reviewer 1):
`m4a_engine_reset_portamento` on the transport edge does not touch the
queue, so note-ons queued before a stop record portamento history after
it -- the transport-edge handler should flush or drop the queue first; a
CLAP NOTE_CHOKE goes through `note_off` and is therefore deferred to the
tick (and in the note's own frame becomes a one-tick note rather than a
cut) -- a choke should probably bypass the latch; events in the first
block drain during the quiet boot, as they apply there today; the
1024-event capacity spans every block until the next SoundMain (dense
automation reaches the in-order overflow, no longer a correctness issue).

### Milestone 5 slice 2a: the MIDI start alignment (2026-10-25)

*Naming, after the decision at the end of this section:* the flag below
was `poryaaaa_render --midi-start zero|rom` / `accuracy.py --midi-start`
with a report field `"midi_start"`.  It is now `poryaaaa_render
--gba-start-delay` (off by default), `accuracy.py` passes it on every
`--input midi` run (`--no-gba-start-delay` to leave it out), and the field
is `"gba_start_delay"` (true / false).  "rom" in the text below means that
flag on, "zero" off.

Scope, small on purpose: the start alignment of `poryaaaa_render`'s
`--midi` path alone, behind a flag (`--midi-start rom|zero`, default
`zero`, so today's output stays byte-identical).  Plugin, GUI, config and
CLAP transport wiring are slice 2b; the tempo grid, the velocity table and
the LFO / CgbSound order are slice 3.  **The sequencer path is not
touched.**  Before any change all three baselines reproduced:
`claim_check.sh` against `2026-10-25_cry-over-song-rows.json` 59 diffed,
0 moved, 57/66, 0 regressions, every row 0.0, unit 1580/1580, blip
49112/0, songs 561/561; `accuracy.py --input midi --midi-quantize` 0
field differences on 44 rows against `2026-10-25_midi-path-latch.json`;
`accuracy.py --input midi` 0 field differences against
`2026-10-25_midi-path-start.json`.

**Step 1, predictions (written before the code).**

*The render loop as it stands* (`cmd/poryaaaa_render.c`, the `--midi`
branch of `main`).  `parse_midi` places every event at
`tick_to_sample(tick)` (exact tempo map, `(uint64_t)(samples + 0.5)`; the
micro MIDIs are all 24 ticks per beat at 500000 us per beat, so one tick
is **exactly 1000 output samples at 48 kHz**).  The loop renders up to an
event's sample with `render_frames` and then calls `dispatch_event`.
`m4a_engine_boot(&engine, true)` has already run the machine to frame 2's
SoundMain (602986 cycles) and kept its output, so output sample s is ROM
time s x 2^24 / rate, and today MIDI time 0 is output sample 0.
`m4a_engine_process` reads the blip until it holds the samples asked for
and runs `run_until(now + 1024)` while it does not; the blip publishes
samples per 2048-cycle frame (`GBA_BLIP_FRAME`), so after s samples the
machine sits at the first 2048-cycle frame end that makes sample s
readable.  **Measured** (a scratch program stepping a booted engine one
sample at a time, 20 s at 48 kHz): after the boot's 1722 samples, the
machine cycle at the dispatch of sample s is c(s) - 150 .. c(s) + 1893,
c(s) = s x 2^24 / 48000, periodic in 375 samples (131072 cycles).
Independent of how the loop chunks the render: the machine only runs
when the blip is short.

*The ROM's start.*  `MPlayStart` on frame 2 (tempoC 0, tempoI 150), the
first MPlayMain on frame 3's SoundMain at 124773 + 2 x 280896 + 160 x 1232
+ 197 = **883882 cycles (52.68 ms, 2528.81 samples at 48 kHz)**, and the
song's TEMPO 120 from that tick on: tick n on frame **3 + ceil(1.25 n)**
(frames 3, 5, 6, 7, 8, 10, ...).

*How to offset: shift every event by a whole number of samples, O,
taken from a cycle stamp.*  The samples are the renderer's only clock
(the engine API takes sample counts, there is no run-to-cycle), so the
offset is an integer sample shift applied at dispatch: an event at MIDI
sample p is dispatched after `p + O` output samples.  `totalSamples` and
`fadeStartSample` move by O only where they are MIDI time (the natural
length's tail and the loop-count fade); with `--total-duration-seconds`
(the harness) they are output time and stay.  The loop expansion and the
gate-carry run in MIDI time as today, so their arithmetic is untouched;
an event they produce in MIDI time that lands at or past `totalSamples`
after the shift is dropped by the loop's existing `break`.

*The boundary.*  An event dispatched at machine cycle d is applied
before SoundMain k iff d < S_k (`run_until` runs a SoundMain due at the
`until` cycle), with or without the latch: without it the entry point
acts at once and SoundMain k mixes it; with it the event is queued and
drained at the top of tick k.  The ROM's tick 0 is applied *at* S_3, so
MIDI time 0 needs d < S_3 strictly.  The nominal shift round(2528.81) =
2529 fails that: the dispatch of sample 2529 is at cycle **884586, 704
cycles after S_3** (samples 2526 .. 2531 all wait for the blip frame
ending at 884586), so every row's first note would start on frame 4, one
frame late.  The last sample dispatched before S_3 is 2525 (882538).  So
the origin is the last sample at least one blip frame before frame 3's
SoundMain: **O = floor((883882 - 2048) x rate / 2^24)**, 2522 at 48 kHz.
Its dispatch is at 882538, 1344 cycles before S_3, at every rate checked
(22050 / 32768 / 44100 / 48000 / 88200 / 96000 / 192000: O = 1158 / 1722 /
2317 / 2522 / 4635 / 5045 / 10091, the last pre-S_3 sample 1160 / 1724 /
2320 / 2525 / 4640 / 5050 / 10101).  The rule this gives: an event at MIDI
time t is applied at the first SoundMain at or after 883882 + t x 2^24
cycles, except that an event 661 .. 2027 cycles after a SoundMain
(measured over ticks 0 .. 139: dispatch minus ideal cycle) would be taken
by that SoundMain, one frame early.

*Dispatch lateness against the micro events.*  With O = 2522 every micro
event's dispatch is at least **1344 cycles before** its SoundMain (tick 0)
and at least **14720 cycles after** the previous one (bend's tick 78):
no micro event crosses a SoundMain through the dispatch lateness or the
guard, so the frames are the exact-time ones, **3 + ceil(1.2443 n)**
(n x 349525.33 cycles from S_3).

*Per micro MIDI, which events land on the ROM's frame* (each event's
frame computed from the measured dispatch cycles, the 31 MIDIs; the same
computation at O = 0 reproduces slice 1's statement exactly -- 3 frames
early on multiples of 4, 4 frames on n = 1 mod 4 from 21, 2 mod 4 from 66
and 3 mod 4 from 110, 3 frames on the others; review-corrected wording).  All of
them, except these, **one frame early** (ceil(1.2443 n) < ceil(1.25 n)):
bend 45, 57, 69, 81, 90, 93, 102, 126 (8 of its 35 event ticks); lfo_vib,
lfo_trem, lfo_pan 102, 126; psg_mix 90, 102, 114; vol 110, 114, 118, 122,
126, 130 (the ticks = 2 mod 4 of its ramp; 108, 112, ..., 132 land on the
ROM's frame -- the slice-1 review's "vol 110 .. 130" was the range, not
the set).  pcm_adsr's note-off at 78, an exception at O = 0, lands on the
ROM's frame.  At O = 2529 the same list plus tick 0 one frame late on
every row.

*A second difference the alignment does not touch: the engine's LFO
grid.*  The MIDI path's accumulator (`m4a_engine_tick`, `!seq`) starts at
tempoI 150 and is consumed by the boot's frame-2 tick; the MIDI's tempo
event (MIDI time 0) is dispatched between S_2 and S_3 at both origins, so
tempoI 120 from frame 3 and LFO ticks on frames 4, 5, 6, 7, 9, ... where
the ROM's song ticks (and its LFO steps) fall on 3, 5, 6, 7, 8, 10, ...:
**one frame early on every fifth tick**, and the engine's tick count since
the note differs from the ROM's by one on those frames.  Part of the
tempo grid (step 2 of slice 1; slice 3).

*Predictions per row* (`--input midi --midi-start rom`, the same with
`--midi-quantize`):

- **Lag 0.0 on every PCM row**, and on every micro row whose events
  correlate at all (the PSG rows too: their note frames now match).
- **The PCM rows with no exception events and no LFO null as the song
  path does**: pcm, pcm_hi, pcm_adsr, pcm_drum, pcm_reverb, pcm_bass, pan,
  vel, tune **-inf** (bit-identical to the reference); pcm_cmp and
  pcm_chord at their song-path floor (-55.0 / -55.7, the scheduler
  lateness crossings).  **pcm is identical sample for sample to the
  `--song` render over the whole file** (no exception events).  The
  `ref` / `test` RMS of these rows equal.
- **vol**: six volume steps one frame early -> a residual of six
  one-frame volume differences of one ramp step each, shallow but deep
  under today's -4.6: predicted -25 .. -40 dB.  **bend**: eight bend
  steps one frame early, the same kind, predicted -20 .. -35.
- **lfo_vib / lfo_trem / lfo_pan / lfo_delay**: not null -- the LFO grid
  above (lfo_delay has no exception event and still does not null: the
  test of the grid statement), plus two one-frame-early events on the
  first three.  lfo_psg adds slice 1's item 6 (the LFO reaches the PSG
  one CgbSound later).
- **The PSG rows** (sq1 .. noise7, tune_psg, psg_mix; the PSG half of
  pcm_psg): bounded by slice 1's item 7, the fixed CgbSound entry (1402
  from line 160 every frame against the ROM's 1866 on tick frames and
  4069 .. 10304 on frame 3): the squares (DAC-slot accuracy, free-running
  duty) improve the most but do not reach -inf (464 cycles is about 1.8
  DAC slots of 256 on every tick frame); wave / wave_alt / noise / noise7 stay
  shallow (phase on the CPU clock); psg_mix also carries three
  exception events; pcm_psg nulls its PCM half and keeps the PSG half's
  residual.
- **The songs**: drift unchanged (4.2 .. 5.1 ms/s, the tempo grid), null
  still about 0 .. +2 dB (decorrelated after a few seconds); the lag
  column about 50 ms smaller where the aligner tracked the start.  **The
  SEs**: lag about 0 where the SE's events land on ROM frames; their
  tempo grids are their own (not predicted per row).
- **The latch**: aligned and aligned-latched runs **byte-identical**
  (slice 1's prediction 12, unchanged by a shift: an event between S_{k-1}
  and S_k is applied before S_k either way).
- **Neither flag** equals the start baseline in every field; the latch
  alone equals the latch baseline.

**Step 2, the offset** (`cmd/poryaaaa_render.c`; no engine change).
`--midi-start zero|rom` (default `zero`; the `--midi` path only, a note
with `--song` / `--cry`): `rom` sets `midiOrigin =
midi_rom_start_samples(rate)` = floor((`ROM_FIRST_TICK_CYCLE` -
`GBA_BLIP_FRAME`) x rate / `GBA_CPU_HZ`), `ROM_FIRST_TICK_CYCLE` = 124773
+ 2 x 280896 + 160 x 1232 + 197 = 883882; the render loop dispatches
every event after `samplePos + midiOrigin` output samples, and the natural
length's `totalSamples` / the loop-count `fadeStartSample` move by the same
amount (an explicit `--total-duration-seconds` does not).  The renderer
prints "MIDI time 0 at output sample N".  `accuracy.py --midi-start
zero|rom` passes it through (only `rom` adds the flag) and the midi report
carries `"midi_start"` next to `"midi_quantize"`.  Unit test
`test_midi_start_origin` (33 assertions): frame 3's SoundMain at 883882;
at 22050 / 44100 / 48000 / 96000 Hz, latched and not, after the kept boot
and O samples the machine is past frame 2's SoundMain and short of frame
3's (`nextSoundMain` still 883882), and a note-on dispatched there has
been through SoundMainRAM (START taken) once frame 3's SoundMain has run.
Checked against the nominal origin (floor(883882 x rate / 2^24)): 16 of its
assertions fail.  Unit **1613/1613** (1580 + 33).

**Step 3, the measurement** (`accuracy.py --input midi`, the four
configurations on the slice's build, the same references):

- neither flag: **0 field differences on 44 rows** against
  `2026-10-25_midi-path-start.json`;
- `--midi-quantize` alone: **0 field differences on 44 rows** against
  `2026-10-25_midi-path-latch.json`;
- `--midi-start rom`: `baselines/2026-10-25_midi-path-aligned.json` /
  `.txt`, **12/44** at -40 dB (start baseline 0/44);
- `--midi-start rom --midi-quantize`:
  `baselines/2026-10-25_midi-path-aligned-latch.json` / `.txt`, 0 field
  differences against the aligned run and the 44 WAVs **byte-identical**
  (`cmp`) to it; every one of the 44 differs from the unaligned render.

`compare_reports.py 2026-10-25_midi-path-start.json
2026-10-25_midi-path-aligned.json` (within the midi family): 31 improved,
2 over 1 dB the other way (sq1_env -4.8 -> -3.5, se_low_health -3.6 ->
-1.9: two PSG rows at noise level before and after, attributed below).
The aligned run against the start baseline:

| test | lag ms | gain dB | null | worst1s | ref / test dBFS | drift | start lag / null / worst1s |
|------|-------:|--------:|-----:|--------:|----------------:|------:|---------------------------:|
| sq1 | 0.0 | -0.0 | -37.2 | -32.9 | -16.8 / -16.8 | - | 49.9 / -3.0 / -1.3 |
| sq1_env | 0.2 | -2.2 | -3.5 | -1.2 | -18.3 / -18.3 | - | 50.2 / -4.8 / -1.2 |
| sq1_sweep | 0.0 | -0.0 | -43.1 | -37.1 | -17.0 / -17.0 | - | 0.0 / -0.3 / 2.7 |
| sq1_alt | 0.0 | -0.0 | -35.5 | -30.6 | -16.8 / -16.8 | - | 49.8 / -0.6 / 1.7 |
| sq2 | 0.0 | 0.0 | -inf | -inf | -18.1 / -18.1 | - | -19.1 / -2.2 / -0.9 |
| sq2_alt | 0.0 | -0.0 | -34.5 | -29.7 | -18.1 / -18.1 | - | 47.9 / -0.2 / 2.6 |
| wave | 0.2 | -0.3 | -11.8 | -9.4 | -19.8 / -19.8 | - | 50.1 / -8.4 / -6.2 |
| wave_alt | 0.1 | -0.4 | -10.6 | -8.2 | -19.8 / -19.8 | - | 49.8 / -7.7 / -7.5 |
| noise | 0.0 | -1.5 | -5.1 | -3.1 | -20.0 / -20.0 | - | 0.0 / 0.9 / 4.1 |
| noise7 | 0.0 | -1.2 | -6.0 | -2.9 | -18.5 / -18.5 | - | 67.0 / -0.2 / 2.4 |
| pcm | 0.0 | 0.0 | -inf | -inf | -21.4 / -21.4 | - | 34.8 / -6.4 / -2.0 |
| pcm_hi | 0.0 | 0.0 | -inf | -inf | -20.9 / -20.9 | - | 160.5 / -3.6 / -1.1 |
| pcm_adsr | 0.0 | 0.0 | -inf | -inf | -29.9 / -29.9 | - | 50.2 / -2.5 / 5.5 |
| pcm_drum | 0.0 | 0.0 | -inf | -inf | -21.9 / -21.9 | - | 50.2 / -6.6 / -3.6 |
| pcm_cmp | 0.0 | 0.0 | -55.0 | -50.2 | -27.1 / -27.1 | - | 0.0 / -0.9 / 3.4 |
| pcm_reverb | 0.0 | 0.0 | -inf | -inf | -21.0 / -21.0 | - | 50.2 / -7.1 / -1.4 |
| pcm_chord | 0.0 | -0.0 | -55.7 | -51.9 | -14.2 / -14.2 | - | -11.2 / 0.0 / 2.3 |
| pcm_bass | 0.0 | 0.0 | -inf | -inf | -25.2 / -25.2 | - | 50.2 / -3.4 / -0.9 |
| pan | 0.0 | 0.0 | -inf | -inf | -19.5 / -19.5 | - | 50.2 / -5.2 / -0.9 |
| vol | 0.0 | -0.1 | -31.2 | -20.3 | -25.3 / -25.2 | - | 30.8 / -4.6 / -2.3 |
| vel | 0.0 | 0.0 | -inf | -inf | -26.5 / -26.5 | - | 19.2 / -5.9 / -0.8 |
| bend | 5.0 | -8.5 | 1.0 | 3.4 | -20.2 / -20.2 | - | 55.7 / 0.7 / 3.3 |
| lfo_vib | -3.8 | -3.6 | -1.7 | -0.6 | -20.1 / -20.1 | - | -4.2 / -1.2 / -0.5 |
| lfo_trem | 3.9 | -0.2 | -13.4 | -8.6 | -20.1 / -20.1 | - | 110.6 / -5.2 / -0.7 |
| lfo_pan | 3.9 | -0.2 | -13.4 | -8.6 | -20.1 / -20.1 | - | 64.8 / -5.1 / -0.7 |
| lfo_delay | -3.5 | -3.0 | -2.3 | -1.2 | -21.0 / -21.0 | - | -3.5 / -2.3 / -1.0 |
| lfo_psg | -18.8 | -6.5 | 0.2 | 1.0 | -17.1 / -17.9 | - | 196.3 / -0.2 / 2.6 |
| tune | 0.0 | 0.0 | -inf | -inf | -21.6 / -21.6 | - | 50.1 / -6.3 / -2.1 |
| tune_psg | 0.0 | -0.0 | -33.5 | -26.5 | -18.0 / -18.0 | - | 49.7 / -12.2 / -10.2 |
| psg_mix | 0.0 | -3.2 | -2.1 | 1.9 | -15.2 / -15.1 | - | 0.0 / -0.3 / 2.9 |
| pcm_psg | 0.0 | -0.1 | -17.3 | -14.4 | -13.3 / -13.3 | - | -15.3 / -4.3 / -2.8 |
| mus_littleroot | 16.7 | -14.8 | 2.1 | 4.0 | -23.7 / -23.7 | 5.0 | 67.0 / 2.1 / 4.0 |
| mus_weather_groudon | 55.7 | -2.9 | -2.4 | 2.1 | -31.1 / -31.1 | 4.7 | 105.9 / -0.5 / 4.5 |
| mus_vs_wild | 67.0 | -14.8 | 2.1 | 3.7 | -20.9 / -20.9 | 4.2 | 117.2 / 2.1 / 3.6 |
| mus_mt_chimney | 16.7 | -11.9 | 1.7 | 3.7 | -23.5 / -23.5 | 4.6 | 67.0 / 1.9 / 3.5 |
| mus_rg_vs_trainer | 33.5 | -16.2 | 2.3 | 3.9 | -19.6 / -19.6 | 4.2 | 100.5 / 2.3 / 3.7 |
| mus_vs_trainer | 67.0 | -13.6 | 2.0 | 3.5 | -20.4 / -20.4 | 5.0 | 117.2 / 1.9 / 3.7 |
| mus_credits | 178.0 | -15.3 | 2.2 | 5.4 | -23.0 / -23.0 | 4.6 | 155.4 / 2.3 / 4.8 |
| se_exp | -1.9 | -13.4 | 2.0 | 3.0 | -24.8 / -24.8 | - | 57.1 / 1.9 / 2.2 |
| se_faint | 0.0 | -1.3 | -5.5 | -2.4 | -29.9 / -29.9 | - | 0.0 / -1.5 / 2.4 |
| se_contest_place | 0.0 | -0.4 | -10.8 | -8.5 | -26.9 / -26.9 | - | 50.2 / -0.8 / -0.8 |
| se_truck_unload | 0.0 | -8.8 | 1.1 | 1.1 | -39.2 / -39.2 | - | 12.3 / 2.2 / 2.4 |
| se_low_health | 7.9 | -3.4 | -1.9 | -0.5 | -24.0 / -24.0 | - | 50.1 / -3.6 / -2.5 |
| se_rain_stop | 0.0 | 0.0 | -32.9 | -23.4 | -41.3 / -41.3 | - | 0.0 / -4.9 / 1.8 |

**Which predictions held.**

- *The origin and the boundary*: held.  The dispatch cycles predicted
  from the scratch program are the ones the unit test checks, and the
  per-event frames reproduce slice 1's measured statement at O = 0.
- *Lag 0.0 on every PCM row*: held (lfo_trem / lfo_pan 3.9, lfo_vib
  -3.8, lfo_delay -3.5 and bend 5.0 are rows that do not correlate; see
  below).  Also 0.0 on every PSG micro row but sq1_env / wave / wave_alt
  (0.1 .. 0.2) and lfo_psg (-18.8).
- *The PCM rows with no exception event and no LFO null as the song path
  does*: **held on all eleven**.  pcm, pcm_hi, pcm_adsr, pcm_drum,
  pcm_reverb, pcm_bass, pan, vel and tune are -inf; pcm_cmp is -55.0 /
  -50.2 and pcm_chord -55.7 / -51.9, their song-path floor.  Each of the
  eleven WAVs is **identical sample for sample to its own `--song`
  render**, and their ref / test RMS are equal.  **pcm is identical to the
  `--song` render over the whole file** (144000 samples; zero diverges at
  25080, as slice 1 found).
- *vol -25 .. -40*: held, -31.2.  The first divergence from the song
  render is on frame 140, its first exception (tick 110).
- *bend -20 .. -35*: **did not hold**, +1.0.  The first divergence is
  frame 59, its first exception (tick 45), as predicted.  The size is
  wrong because a pitch step one frame early moves a PCM sample's
  playback position for the rest of the note (a frequency x one frame of
  phase), so after the first early step the note never re-converges.  I
  had treated it as a one-frame level step, like vol.
- *The lfo rows do not null, from the LFO grid*: held (lfo_vib -1.7,
  lfo_trem / lfo_pan -13.4, lfo_delay -2.3, lfo_psg +0.2).  But the
  prediction stated the grid's direction wrong.  Against the aligned notes
  the engine's LFO steps on frames 4, 9, 14, ... where the ROM's step on
  3, 8, 13, ...: **one frame late on one tick in four** (ticks n = 0 mod
  4, once every five frames; the engine's tick n on frame 4 + n + floor(n
  / 4), the ROM's on 3 + n + floor((n + 3) / 4)).  (The engine's
  frame-2 boot tick is the one the ROM takes on frame 3, so as a set the
  engine's tick frames are the ROM's minus one; read against the music,
  the 5k + 3 step comes a frame late.)  The first divergence from the song
  render is frame 3 on lfo_vib / lfo_trem / lfo_pan (no delay) and frame
  43 on lfo_delay (a pitch, 3382 vs 3569, on a ROM-only tick frame, 43 =
  3 mod 5), and it is the LFO, not an event.
- *The PSG rows bounded by items 6 and 7*: held.  The write logs of sq1 /
  sq2 / sq1_env (a scratch build of the renderer with `--psg-log` on the
  MIDI path) carry **the same registers and values in the same order as
  the `--song` render's**, offset by item 7's entry difference frame by
  frame (measured on every shifted write of sq1 / sq1_env): -2667 cycles
  on frame 3 (1402 against 4069), -472 on the gate-expiry frames 33 and
  78, -464 on a plain tick frame (133, sq1_env; item 7's 1866 - 1402),
  -1388 / -1394 (sq1) and -1438 / -1444 (sq1_env) on the note frames 48
  and 93, -649 on frame 153 (tick 120) and +395 on frame 176.  sq1 / sq1_alt / sq2_alt / tune_psg
  diverge from the song render at frame 33's -472 (-33.5 .. -37.2);
  sq1_sweep at frame 63 (-43.1, worst -37.1).  **sq2 is -inf**: the same
  shifts on its channel-2 writes leave the output unchanged.  I had
  predicted no square would reach -inf, so this is better than predicted;
  it is not taken apart write by write.  sq1_env carries the hardware
  envelope (NR12 0xA3, period 3), so the -472 shift moves its steps
  against the 512 Hz frame sequencer from frame 48 on (-3.5, the row that
  "regresses" by 1.3 dB from noise level).  wave / wave_alt / noise /
  noise7 diverge at frame 3 (-5.1 .. -11.8).  pcm_psg's channel state is
  identical to the song path's on every frame (a scratch tick dump of
  every channel's status, key, velocity, volumes, envelope and frequency
  after each tick), so its -17.3 is its PSG writes.  psg_mix's first state
  difference is its tick-90 exception (frame 115); before that, its writes.
- *The songs*: held.  Drift 4.2 .. 5.0 ms/s, null -2.4 .. +2.3.  The lag
  moved by 50 .. 67 ms on six songs (littleroot 67.0 -> 16.7, groudon
  105.9 -> 55.7, vs_wild 117.2 -> 67.0, mt_chimney 67.0 -> 16.7,
  rg_vs_trainer 100.5 -> 33.5, vs_trainer 117.2 -> 67.0) and the other
  way on credits (155.4 -> 178.0): the aligner choosing within a drifting
  file.
- *The latch*: held, byte-identical.  *Neither flag* / *latch alone*:
  held, 0 field differences.

**Attribution of every row that does not null.**  Each row's render was
diffed sample by sample against its own `--song` render: the same engine,
differing only in the input path.

- **Frame exceptions** (step 1's list): vol (frame 140 = tick 110), bend
  (frame 59 = tick 45), psg_mix (tick 90, after its PSG residual).  For
  the SEs the same computation over each SE's own tempo (ROM tick n on the
  accumulator's frame with TEMPO = round(60e6 / us per beat), MIDI frame
  from the dispatch cycles): se_exp (240 BPM) has 20 of 131 event ticks
  one frame early, its first at tick 45; its first state difference is
  that tick's CGB pitch on frame 31.  se_contest_place (144 BPM) has
  ticks 25 .. 27 one frame early.  se_faint, se_truck_unload,
  se_low_health and se_rain_stop have none.
- **The tempo grid** (slice 1's item 2): the songs' drift, and the LFO
  grid on the lfo rows.
- **The PSG write timing** (items 6 and 7): every PSG micro row, the PSG
  half of pcm_psg, and se_faint / se_truck_unload (noise only) /
  se_low_health (one square).  Their channel state is identical to the
  song path's on every frame and the output still differs.
- **Unpredicted -- the per-tick command batching** (found by the tick
  dump, not by step 1): se_rain_stop (-32.9) and se_contest_place (frame
  25).  On the GBA, a VOL (or PAN, BEND, ...) followed in the same tick by
  a note on the same track reaches only the new note.  `ply_note` runs
  TrkVolPitSet for the new channel and clears the track's pending VOLCHG /
  PITCHG flags (`m4a_sequencer.c` ply_note's `t->flags &= 0xF0`), so the
  end-of-tick pass never updates the older channels in the chain.  Here
  that is the releasing note: se_rain_stop's tick 24 is a note-off, VOL
  70, and the same key again; the song path keeps the releasing channel
  at 20 / 19 and the MIDI path moves it to 17 / 17, a one-LSB residual
  (max 195) over that note's release.  On the MIDI path `m4a_engine_cc`
  applies the controller to every channel of the track at once.
  Unaffected by the latch: the drain applies CCs one by one, as the
  unlatched path does.  For slice 3, next to the tempo grid: a drain that
  sets the track's flags and runs the pass at its end unless a note
  consumed them, as MPlayMain does.
- **Unpredicted, no harness effect -- the track priority**: the MIDI path's
  tracks have priority 0 where mid2agb's `-P` writes the song header's
  (se_rain_stop -P2: the song path's channels carry priority 2).  It only
  enters ply_note's steal decision between players or against a busier
  track, which no single-player row exercises.  A renderer flag would map
  it (slice 3).

**Step 4, the gates.**  `claim_check.sh` against
`2026-10-25_cry-over-song-rows.json` after the change: **59 tests diffed,
0 moved, 57/66, 0 regressions, every row 0.0** (the song-input gate
unchanged), unit **1613/1613**, blip 49112/0, songs 561/561.  The two
aligned baselines saved as above.

**Recommendation on the default** (the user decides; not flipped here).
I recommend making `rom` the default for `--midi`, and `--midi-start rom`
the runner's default for `--input midi`:

- It is the origin the ROM, and the `--song` / `--cry` paths of the same
  renderer, already use.
- Nine PCM rows go from -2.5 .. -7.1 dB to -inf (bit-exact) and pcm_cmp /
  pcm_chord from -0.9 / 0.0 to their -55 floor, and the residual
  left over is attributable term by term to slice 3's items.
- The cost: 52.5 ms (2522 samples at 48 kHz) of leading boot silence in
  every MIDI render, which `--song` renders already have; `zero` stays
  available.

If the user flips it, the midi baseline family keeps its two zero-origin
files as the history, and the aligned pair becomes "the" midi baseline.

**What slice 2b (the plugin) needs.**

- *The transport-driven latch default* in `m4a_plugin.c`
  `plugin_process`: `m4a_engine_set_quantize_events(&engine, playing)`
  from `CLAP_TRANSPORT_IS_PLAYING`, before the block's events.  The flip
  to off flushes in order; the flip to on queues from the next event.
- *The manual override*, a tri-state (transport / on / off): a GUI
  Options item, a `poryaaaa.cfg` key and a CLAP state byte appended after
  the existing ones.  Back-compatible: an old state without the byte loads
  as "transport".
- *The slice-1 hazards*:
  - On the playing -> stopped edge, flush or drop the queue *before*
    `m4a_engine_reset_portamento`, or note-ons queued before the stop
    record glide history after the reset.
  - A CLAP `NOTE_CHOKE` should bypass the latch (cut at once, and drop
    that key's queued note-on), not become a deferred note-off or, in its
    own frame, a one-tick note.
  - The first block's events drain during the quiet boot.
  - The 1024-event capacity spans every block until the next SoundMain.
- *Start alignment in the plugin: none.*  The host's timeline has no
  MIDI time 0 that a ROM frame corresponds to: the plugin's frame phase is
  set by when it booted, and the latch already puts every event on a
  frame edge.  Aligning the boot so that the host's song position 0 falls
  on a SoundMain would make an offline bounce repeatable to the sample
  across runs, but that is a separate design (a frame-phase reset on the
  transport start), not this offset.  The renderer's `rom` origin stays a
  renderer / harness concept.

**Code review of slice 2a (2026-10-25).**  Two independent read-only
reviews:

- *Reviewer 1*: the offset against the ROM's boot and song start --
  AgbMain / StartTest and the interrupt enables in main.c / crt0.s,
  m4aSoundInit, MPlayStart's tempoC / tempoI, MPlayMain's tempo loop and
  ply_tempo in m4a_1.s -- the engine's frame constants, boot table,
  `run_until`, `m4a_engine_process` and the blip stepping in m4a_gba.c;
  the renderer's loop expansion, gate-carry, render length and fade in
  all three modes; the unit test.  A Python model of the stepping.
- *Reviewer 2*: every step-1 number recomputed from the constants and the
  31 micro and six SE MIDIs (its own dispatch model), the ROM tick rule
  against MPlayStart / MPlayMain / mid2agb's TEMPO, the runner switch,
  the two baselines against the plan's table field by field, the docs,
  and a stale-phrase grep.

No divergence in the offset's code or in any measured number: the first
tick cycle 883882 from the ROM source and the engine's constants; the
dispatch bound (phase-free c(N) - 1025 .. c(N) + 2047.5, with the boot's
phase -150 .. +1898, matching the measured -150 .. +1893); the origin
before frame 3's SoundMain at every rate; the latch equivalence; the
loop-expansion and fade arithmetic; the exception lists; the SE counts;
the table (44 rows, 0 mismatches); the 33 assertions.

*Acted on* (one code change; `claim_check.sh` after it: 59 diffed, 0
moved, 57/66, 0 regressions, every row 0.0, unit 1613/1613, blip
49112/0, songs 561/561; `--midi-start rom` with and without
`--midi-quantize` 0 field differences on 44 rows against the two aligned
baselines):

- *The printed fade start* (reviewer 1).  In loop-count mode with `rom`,
  the renderer printed the fade start in MIDI time, O samples before the
  one applied.  It now also prints "Fadeout: moved to ... with the
  origin".  Print only; the WAV is byte-identical.
- *The usage text* (reviewer 2) said `rom` plays tick n on the compiled
  song's frame.  It now adds "at the exact MIDI tempo" and the tempo-grid
  caveat.
- *The LFO grid's wording* (reviewer 1): "one frame late on every fifth
  tick" is one tick in four (n = 0 mod 4, once every five frames):
  corrected in the results and the skill.  Reviewer 2 confirmed the
  frames.
- *"4 frames on the others from tick 21"* (reviewer 2): bend's ticks 27,
  30, 39, ... are 3 frames early.  The step-1 sentence and the skill now
  give slice 1's exact rule (1 mod 4 from 21, 2 mod 4 from 66, 3 mod 4
  from 110).
- *The recommendation's range* (reviewer 2): nine rows go from -2.5 ..
  -7.1 to -inf, and pcm_cmp / pcm_chord from -0.9 / 0.0 to their -55
  floor.
- *"-472 on the tick frames"* (reviewer 2's open question, measured
  here).  Every shifted write of sq1 / sq1_env: -472 on the gate-expiry
  frames 33 / 78, -464 on a plain tick frame (133, item 7's 1866 - 1402),
  -1388 .. -1444 on the note frames, -649 on frame 153, +395 on 176.  The
  results paragraph now lists them.
- "Two DAC slots" -> about 1.8.

*Recorded, not acted on:*

- The guard admits a small early window (reviewer 1).  An event whose
  aligned cycle falls up to about 2.5 k cycles after a SoundMain (bound
  -2533 .. -486 at 48 kHz, -2946 .. -898 at 22050) is taken by that
  SoundMain, one frame early.  Under 1.1 % of a frame, never late; no
  micro event falls in it.  The exact bound is the blip's 1024-cycle lead
  plus one 1024-cycle step, which equals one blip frame by construction,
  not by derivation.
- The MIDI path's tempo takes effect on the current frame's add, where
  the ROM's TEMPO takes effect from the next frame's (MPlayStart seeds the
  first add with 150).  This is the cause of the LFO grid; reviewer 1
  suggests seeding tempoC = 150 - tempoI at the origin or applying tempo
  after the add.  Slice 3, with the tempo grid (no fix beyond the offset
  here).
- CC 120 with the latch off writes the PSG at its call cycle (known since
  slice 1).
- The unit test recomputes O rather than calling the renderer's static
  `midi_rom_start_samples`, and covers PCM only, not the CGB START path.
- `ROM_FIRST_TICK_CYCLE` repeats the literal 124773 instead of sharing
  FRAME1_START (private to m4a_engine.c).
- Step 1's nominal is round(2528.81) = 2529 and the test's
  floor = 2528; both are dispatched 704 cycles after S_3.
- A trailing `--midi-start` with no value falls to "Unknown option" and
  the usage text, as every other option does.
- The step-1 prediction text (the LFO direction, bend's range) is kept as
  written; the results say where it was wrong.

**Decision (2026-10-25, the user).**  The renderer's default stays off.
A user rendering a MIDI gets nothing from 52.5 ms of leading silence, and
"rom" read as if the renderer loaded a ROM.  So:

- The flag is renamed `--gba-start-delay`: a plain switch, the same
  offset.
- The harness passes it on every `--input midi` run, where the alignment
  is the point; `--no-gba-start-delay` opts out.
- The report field is `"gba_start_delay"`.  In the two aligned
  baselines' headers, `"midi_start": "rom"` became `"gba_start_delay":
  true`; their results are untouched.
- `test_midi_start_origin` is now `test_gba_start_delay_origin`.

Gate after the rename: `claim_check.sh` 59 diffed, 0 moved, 57/66, 0
regressions, every row 0.0, unit 1613/1613, blip 49112/0, songs 561/561.
The runner's new default equals `2026-10-25_midi-path-aligned.json` and
with `--midi-quantize` `-aligned-latch.json`; `--no-gba-start-delay`
equals `-start.json` and with `--midi-quantize` `-latch.json` (0 field
differences on 44 rows each).  The aligned pair is now the midi baseline
the next slice compares against.

### Milestone 5 slice 2b-i: the plugin's transport-driven latch (2026-10-25)

Scope: `plugin/m4a_plugin.c`'s `plugin_process` and the smallest engine
addition it needs.  The manual override (GUI, `poryaaaa.cfg`, CLAP state)
is slice 2b-ii; no frame-phase or start alignment in the plugin (slice
2a); the tempo / LFO grids, VOL batching, -P priority and the CgbSound
entry are slice 3.  The sequencer path and `poryaaaa_render` are not
touched, so the harness cannot see this slice: its gates are the
unchanged harness numbers and unit tests of the engine pieces.

**Step 1, the reading (written before the code).**

- *`plugin_process`* (`m4a_plugin.c`): first the tempo
  (`CLAP_TRANSPORT_HAS_TEMPO` -> `m4a_engine_set_tempo_bpm`), then the
  transport block: with `process->transport` non-NULL, `playing =
  flags & CLAP_TRANSPORT_IS_PLAYING`, and on `transportWasPlaying &&
  !playing` `m4a_engine_reset_portamento`; `transportWasPlaying` is only
  updated when a transport is given.  Then the sample-accurate loop: every
  event whose `time <= framePos` is applied (`process_clap_note_event` for
  NOTE_ON / NOTE_OFF / NOTE_CHOKE, `process_midi_event` for raw MIDI),
  then `m4a_engine_process` renders up to the next event's time.  The
  engine's VBlank ticks (and so the latch's drains) happen inside
  `m4a_engine_process`; the host delivers `in_events` sorted by sample
  (`process.h`), so arrival order is time order.  Nothing reads
  `CLAP_EVENT_TRANSPORT` events inside a block: the transport is read once,
  at the block's sample 0.
- *`process_clap_note_event`*: a channel of -1 or >= 16 becomes track 0;
  NOTE_ON's velocity is `v * 127 + 0.5`, at least 1; NOTE_OFF and
  NOTE_CHOKE both call `m4a_engine_note_off(track, (uint8_t)key)`.  A
  wildcard key (-1) becomes 255, which `note_off` rejects (> 127): a
  wildcard choke or note-off does nothing today.
  *`process_midi_event`*: 0x90 (velocity 0 = note-off), 0x80, 0xC0, 0xB0,
  0xE0 to the engine's entry points; no choke in MIDI 1.
- *Lifecycle*: `plugin_activate` re-runs `m4a_engine_init` (a `memset`:
  the latch comes back off, the queue empty) and re-applies the
  plugin's settings; `transportWasPlaying` is set false only in
  `plugin_init`, so after a deactivate / activate it can say "playing"
  while the fresh engine's latch is off.  `plugin_stop_processing` and
  `plugin_reset` call `m4a_engine_all_sound_off`, which outside a tick
  drops the queue, cuts every channel and resets the portamento history.
- *The SDK*: `CLAP_TRANSPORT_IS_PLAYING` is bit 4 of the transport flags
  (`events.h`); `process->transport` is "null [for] a free running host,
  no transport events will be provided" (`process.h`) -- the
  clap-wrapper standalone host is one (`standalone_host.cpp`:
  `process.transport = nullptr`).  NOTE_CHOKE "is meant to choke the
  voice(s), like in a drum machine when a closed hihat chokes an open
  hihat", also sent when "the user double-clicks the DAW's stop button
  ... which then stops the sound on every track"; NOTE_OFF is a key
  release.  A choke is a cut, not a release.  Note addresses use -1 as a
  wildcard (`events.h`: "(-1, 0, 60, -1) will match all channel 0 key 60
  voices"), and for a note event "key and channel are used to match
  active notes; note_id is optionally provided".  The SDK does not say
  what address a stop-button choke carries; key -1 and / or channel -1 is
  the assumption here.
- *The latch* (`m4a_engine.c`): `queue_event` queues when `quantizeEvents
  && !inTick`, a full queue drained in order first; `drain_events` sets
  `inTick` for its duration and applies the `n` events present at entry;
  `m4a_engine_set_quantize_events(false)` drains before clearing the flag
  (nothing lost), `(true)` only sets it; `m4a_engine_flush_events` drains;
  `m4a_engine_all_sound_off` drops the queue when called outside a tick.
  `m4a_engine_reset_portamento` clears every track's `portamentoPrevKey`,
  target, gliding flag and elapsed count, and does not look at the queue.
  `m4a_engine_note_off` with the latch on queues; applied, it sets STOP on
  the track's matching channels not already STOP, or gate 1 on one still
  START (the one-tick note).

**What happens today, and the hazards.**

- *Transport edges*: nothing touches the latch (the plugin never turns it
  on).  Once it follows the transport: on stopped -> playing nothing is
  queued (the latch was off), so the flip only makes the block's events
  queue.  On playing -> stopped the queue holds the events of the last
  playing block(s) that the next SoundMain has not drained yet (at most a
  frame's worth); this block's events have not been read yet.
- *The ordering hazard* (slice 1's review): if the stop edge called
  `m4a_engine_reset_portamento` with events still queued, the queued
  note-ons would apply after it -- at the next tick, or at the flush when
  the latch turns off -- and record `portamentoPrevKey`, so the first note
  played after the stop would glide from a note queued before it.  The
  queue must be emptied before the reset.
- *A choke with the latch on* would be queued as a note-off: deferred to
  the next tick, and, in its note's own frame, turned into the one-tick
  note instead of a cut.  What it must do: cut that key's sounding
  channel(s) at once, releasing or not, and remove that key's queued
  note-on (and note-off) so the next tick does not start the note it
  choked.  With the latch off it must cut too, not release.
- *No transport* (`process->transport == NULL`): no edge can be seen and
  no state is known; the latch keeps whatever it had.

**Decisions.**

- *The latch follows the transport* at the top of `plugin_process`, after
  the tempo and before the block's events:
  `m4a_engine_set_quantize_events(engine, playing)` whenever a transport is
  given.
- *No transport: off, and kept.*  The initial state is the engine's
  default after `m4a_engine_init` (off), and `plugin_activate` now resets
  `transportWasPlaying` to false so the plugin's view matches the fresh
  engine.  Off is right for the one free-running host we know, the
  standalone, which is played live from a MIDI keyboard (the milestone's
  "unquantized when the transport is stopped": no transport is no
  playback).  A host that drops the transport for some blocks keeps the
  last known state rather than flapping.
- *Stop edge: flush, then reset.*  The flip to off is
  `m4a_engine_set_quantize_events(false)`, which applies the queue in
  order at once; `m4a_engine_reset_portamento` follows.  Dropping instead
  would lose the queued note-offs as well as the note-ons: a note sounding
  before the stop whose note-off sits in the queue would hang (a host that
  sends no note-offs on stop would never end it).  Flushing loses nothing,
  matches what turning the latch off already does, and the reset after it
  clears whatever history the flushed note-ons recorded.  The flushed
  events apply up to a frame early (at the stop's block boundary instead
  of the next SoundMain) -- immaterial once the transport has stopped.
  The host's own stop handling (NOTE_OFFs, CC 123 / 120, a choke, or
  `stop_processing` / `reset`, whose all-sound-off drops the queue)
  arrives unlatched when it comes in a block whose transport reads
  stopped; sent in the last block still flagged playing it is latched and
  then flushed at the edge (a latched CC 120 applies inside the drain and
  so does not drop the events after it -- slice 1's call-time note).  A
  note-on and its note-off both queued in the last playing frame meet at
  the flush with the latch still on: the note gets the one-tick gate and
  sounds one frame more, stopped by the next tick's gate walk.
- *NOTE_CHOKE bypasses the latch*: a new engine entry point
  `m4a_engine_note_choke(engine, trackIndex, key)` that, at once and with
  the latch on or off, removes the matching queued note-ons and note-offs
  (the rest keep their order) and kills every matching channel (`CHN_ON`,
  releasing or not, shadow pools included) with `m4a_engine_kill_channel`
  (PCM status 0; CGB CgbOscOff then status 0).  A negative track or key is
  a CLAP wildcard (all tracks / all keys), so the stop-button choke works;
  the plugin maps a negative channel to -1 and passes the key as is, for
  the choke only.  `note_id` and `port_index` are ignored (one note port,
  no voice ids tracked).  The portamento history is untouched (a choke is
  not a transport event; the note it removed from the queue never
  recorded any): a note after a choked one can still glide from the
  choked key.  `m4a_engine_note_off` is
  unchanged.

**Step 2, the code.**  `plugin_process`: after the tempo, with a
transport, `m4a_engine_set_quantize_events(&engine, playing)` and then,
on the playing -> stopped edge, `m4a_engine_reset_portamento` (the
existing reset, now after the flip's flush); nothing without a
transport.  (Since the review, this is `apply_transport`, called for
`process->transport` and for each in-block `CLAP_EVENT_TRANSPORT`.)  `plugin_activate` sets `transportWasPlaying = false`.
`process_clap_note_event`: NOTE_CHOKE -> `m4a_engine_note_choke(track,
key)`, a channel of -1 passed as -1 (other out-of-range channels still
map to track 0, as for NOTE_ON / NOTE_OFF), the key passed as is (-1 a
wildcard, 0..127, above 127 ignored).  `m4a_engine_note_choke`
(`m4a_engine.h` / `.c`, next to the latch): a track >= 16 or a key > 127
does nothing; otherwise the matching NOTE_ON / NOTE_OFF entries are
compacted out of the queue in place (the rest keep their order, the
count shrinks), then every PCM and CGB channel with `CHN_ON` on a
matching track and key, the shadow pools included, goes through
`m4a_engine_kill_channel`.  It does not depend on `quantizeEvents` or
`inTick`.  The renderer and the sequencer never call it.

**Step 3, the tests** (`test/test_engine.c`; `m4a_engine_tick` driven by
hand as in `test_event_latch`, a channel's status set to
`CHN_ENV_SUSTAIN` where the mixer's envelope step would have cleared
START).  `test_note_choke`: latch off -- a sounding key cut to status 0
at once, another key on the track and the same key on another track
untouched, `note_off` still a release (STOP, channel on), a releasing
channel cut; latch on -- with a sounding key 60 and 64 and five events
queued (note-off 60, note-on 62, note-on 60, CC 7, note-off 64), the
choke of 60 cuts the sounding note at once and leaves note-on 62, CC 7,
note-off 64 in that order, key 64 not yet released; at the tick key 60
never starts, 62 starts, 64 releases, the CC applies; a note-on and its
note-off queued in one frame and choked leave nothing queued and nothing
starting; wildcards -- key -1 cuts every key of one track and removes
only that track's queued notes (a CC kept), track -1 one key on every
track, -1 / -1 every note and every queued note (the CC kept); track 16
/ key 128 change nothing; a shadow-pool channel (index
`MAX_PCM_CHANNELS`) cut; a square channel cut at once with the latch on.
`test_transport_stop_edge`, in the plugin's order: a note-off queued for
a sounding note reaches it at the flip's flush (no hang); a note-on
queued before the stop applies at the flush, gliding from the previous
key as it would have at its tick, and the reset after it leaves no
history, so the first live note applies at once without a glide, CC 5
kept; the reversed order (reset, then flush) makes that live note glide
-- the hazard, shown; the start edge queues nothing at the flip and the
next event queues, a repeated "playing" block keeping the queue.  Unit
**1673/1673** (1613 + 60).

**Step 4, the gates.**  `claim_check.sh` against
`2026-10-25_cry-over-song-rows.json`: 59 diffed, 0 moved, 57/66, 0
regressions, every row 0.0; unit 1673/1673, blip 49112/0, songs 561/561.
`accuracy.py --input midi` and `--input midi --midi-quantize`: 0 field
differences on 44 rows against `2026-10-25_midi-path-aligned.json` /
`-aligned-latch.json`.  `poryaaaa` and `poryaaaa-standalone` build.  The
standalone was not driven: in this WSL environment it exits at its MIDI
initialisation (no `/dev/snd/seq`) before audio starts, so nothing about
the change was checked by running it (it passes `transport = NULL`, so
of this slice only the choke reaches it, and RtMidi sends no choke).
README: one paragraph under "CLAP plugin in a DAW" on the transport-driven
latch and the choke.

**Code review of slice 2b-i (2026-10-25).**  Two independent read-only
reviews (reviewer 1: `plugin_process` and `m4a_engine_note_choke` against
the CLAP SDK headers and the latch code; reviewer 2: the unit tests
assertion by assertion, the plan text, and every caller that cuts notes
or touches the queue).  Neither found a fault in the choke or in the
flush-then-reset order.  Acted on, each alone, each gated by
`claim_check.sh`, the two aligned midi runs and the unit tests:

- *The reset froze a flushed glide at its start key* (reviewer 1).  The
  stop edge's flush starts a queued note-on that begins a glide (pitched
  at once to the previous key by `portamento_note_started`), and
  `reset_portamento_note_state` cleared `portamentoGliding` without
  moving the pitch: the note kept the previous key's pitch until a bend or
  an LFO refreshed it.  Measured first with a new assertion in
  `test_transport_stop_edge` (the flushed key 48 at frequency 9195, key
  50's, where key 48's is 8192), then fixed: the reset ends a glide in
  progress at its target on the track's live channels
  (`apply_portamento_pitch(target << 8)`, which skips STOP channels, so
  `all_notes_off`'s releasing notes keep their old behaviour).  Gate:
  claim_check 59 diffed, 0 moved, 57/66, 0 regressions, every row 0.0,
  unit 1675/1675, blip 49112/0, songs 561/561; both midi runs 0 field
  differences on 44 rows.  Harness-neutral by construction (portamento
  is off on every harness path).
- *In-block transport events were ignored* (reviewer 1): the SDK lets a
  host send sample-accurate `CLAP_EVENT_TRANSPORT` updates inside a block
  (`events.h`, `clap_event_transport`), and the plugin read only the
  block-start transport, so a mid-block stop flipped the latch, flushed
  and reset one block late.  The transport handling is now
  `apply_transport`, called at the top of the block and for each such
  event in the sample-accurate loop (the tempo with it).  Gate: as above,
  unit 1675/1675, 0 field differences on both midi runs; `poryaaaa` and
  `poryaaaa-standalone` build.
- *Test assertions that could not fail* (reviewer 2), fixed in the tests
  only: "the released channel still sounds" (implied by the STOP assertion;
  removed) and the releasing channel now hand-set to the mixer's release
  state (`CHN_STOP`) before the choke; the track -1 choke now has another
  key sounding on track 1 that must survive, and the -1 / -1 choke cuts
  it; the out-of-range chokes now face a sounding key 60 that must stay;
  the CGB choke checks CgbOscOff's writes (NR12 = 8, NR14 = 0x80); the
  start edge's "nothing queued" now follows a note applied at once while
  stopped; and a new case, a note-on and its note-off queued in the last
  playing frame, gets the one-tick gate at the flush and STOP at the next
  tick (on track 1: on track 0 the CC 5 legato inherit takes the note out
  of START).  Unit **1681/1681**.  Gate after all of it: claim_check 59
  diffed, 0 moved, 57/66, 0 regressions, every row 0.0, blip 49112/0,
  songs 561/561; both aligned midi runs 0 field differences on 44 rows;
  `poryaaaa` and `poryaaaa-standalone` build.
- *Plan wording* (reviewer 2): the stop-button choke's address is an
  assumption, `note_id` / `port_index` ignored; the host's stop events
  are unlatched only in a stopped block; the flush's one-tick note; any
  negative channel or key is a wildcard; a choke leaves the glide
  history at the choked key.  Edited in place above.

Recorded, not acted on: `transportWasPlaying`'s reset in
`plugin_activate` matters less than step 1 says (the latch is set from
`playing` on every block regardless; the stale value could only cause one
redundant reset); a choke addressed by `note_id` alone (channel and key
-1) cuts every note, since no voice ids are tracked -- the SDK names key
and channel as a choke's matching fields; any negative value, not only
-1, is a wildcard; a CGB channel with only SPECIAL / LOOP bits set would
survive a choke (no path produces it); a CLAP NOTE_OFF with key or
channel -1 is still no wildcard (key 255 rejected, channel -1 -> track 0;
`note_off` is not this slice's to change); a channel >= 16 maps to track
0 for every note event; a velocity above 1.0 overflows the `uint8_t`
cast (pre-existing); the main-thread timer and `state_load` write engine
state while `plugin_process` may run (pre-existing; neither touches the
queue); the tests drive the engine calls in the plugin's order but
nothing exercises `plugin_process` itself (no plugin-level harness);
untested promises: CGB shadow and wildcard chokes, queued PROGRAM / BEND
/ XCMD surviving a choke, a NULL transport keeping the state.  For 2b-ii
(reviewer 1): the latch keys on `CLAP_TRANSPORT_IS_PLAYING` alone, so
live keys played while the transport runs are latched too
(`CLAP_EVENT_IS_LIVE` marks them), and pre-roll / recording are not
considered.

**What slice 2b-ii needs.**  The override as designed (transport / on /
off; a GUI Options item, a `poryaaaa.cfg` key, an appended CLAP state
byte, an old state loading as "transport") fits what 2b-i built: "on" and
"off" call `m4a_engine_set_quantize_events` with a constant (each block,
or on change), "transport" is this slice's per-block flip.  Three things
found here bear on it.  (1) The stop-edge portamento reset must stay on
the transport edge in every mode, and with "on" the queue must still be
flushed before it (an explicit `m4a_engine_flush_events` there, since the
latch does not turn off).  (2) Switching from "on" to "off" or
"transport"-while-stopped flushes through `set_quantize_events(false)`;
the GUI change arrives on the main thread (the timer's
`m4a_gui_poll_changes` block in `m4a_plugin.c`), which today writes the
other toggles straight into the engine (`m4a_engine_set_portamento_enabled`
etc.) while `plugin_process` may be running.  The override must not copy
that: a flip to off drains the queue -- note-ons, channel allocation --
and would race the audio thread's own queueing and tick.  Store the mode
in `M4APluginData` and let `plugin_process` apply it at the top of the
next block, where 2b-i's flip already sits.  (3) With no transport (the standalone),
"transport" means off; the option's tooltip should say so.  (4) In
"transport" mode live input during playback is latched as well; whether
events flagged `CLAP_EVENT_IS_LIVE` should bypass the latch while the
transport plays (the milestone's "rubbery keyboard" concern, now while
recording) is a decision for 2b-ii, not something this slice settled.

### Milestone 5 slice 2b-ii: the manual override (2026-10-25)

Scope: the latch's tri-state override -- transport / on / off -- in the
plugin, with a GUI Options item, a `poryaaaa.cfg` key and an appended CLAP
state byte (an old state loads as "transport").  No change to the
sequencer path, the renderer or the latch itself; the harness cannot see
the plugin, so the gates are the unchanged harness numbers and unit tests.
Baselines before any change: `claim_check.sh` against
`2026-10-25_cry-over-song-rows.json` 59 diffed, 0 moved, 57/66, 0
regressions, every row 0.0, unit 1681/1681, blip 49112/0, songs 561/561;
both aligned midi runs 0 field differences on 44 rows.

**Decision (2026-10-25, the user):** in "transport" mode, events the host
flags `CLAP_EVENT_IS_LIVE` are latched like the rest while the transport
plays (what is heard while recording is what the recorded part plays back
as); "off" is the escape for anyone who wants otherwise.

**Step 1, the reading.**

- *Where each setting lives.*  `M4APluginData` (`m4a_plugin.h`) holds the
  persisted settings; `M4AGuiSettings` (`m4a_gui.h`) is the copy the GUI
  edits.  Three places build an `M4AGuiSettings` from the plugin data
  (`plugin_activate`, `state_load`, `gui_create`) and one copies it back
  (the timer's `m4a_gui_poll_changes` block).  The Options menu
  (`m4a_gui.cpp` `render_menu_bar`) holds three checkmark toggles with
  hover help (`toggle_menu_item`); a three-way choice has no helper yet.
- *Config.*  `load_config_file` reads `key=value` lines at `plugin_init`;
  the opt-in toggles take `atoi(value) != 0`, the mix rate a number snapped
  to the table.  `poryaaaa.cfg.example` documents each key.
- *State.*  `state_save` writes, in order: root length + bytes, name length
  + bytes, reverb, master volume, song volume, a zero byte (the former
  analog filter), max channels, the three toggle bytes, the PCM mix rate as
  a float in Hz.  `state_load` reads the same; everything from the analog
  byte on is optional (a short read keeps the default), so appended bytes
  are back-compatible by construction.  An old save stops after the float
  (or earlier); a new byte after the float reads short there.
- *Threads.*  `state_load` and the GUI timer run on the main thread
  (`ext/state.h` and `ext/timer-support.h`: `[main-thread]`) and today
  write the engine directly (`m4a_engine_set_portamento_enabled` etc.)
  while `plugin_process` may run; `plugin_process` is the audio thread.
- *The latch in the plugin today* (2b-i): `apply_transport` sets the latch
  from `CLAP_TRANSPORT_IS_PLAYING` and on the playing -> stopped edge
  resets the portamento after the flip's flush; no transport keeps the
  state, off from activation.

**Decisions.**

- *The mode* is `M4ALatchMode` in `m4a_engine.h` (`M4A_LATCH_TRANSPORT` 0,
  `M4A_LATCH_ON` 1, `M4A_LATCH_OFF` 2): the value 0 is "transport" so a
  zeroed or absent byte means the default.  It lives in the engine header
  because both the plugin and the GUI include it and the unit tests can
  then drive the plugin's policy (2b-i's review: nothing exercised the
  plugin's order).
- *The policy is one engine call*, `m4a_engine_apply_latch_mode(engine,
  mode, wasPlaying, playing)`, made by the plugin at the top of every
  block and for every in-block transport event (`playing` = the last
  known transport state when the block has none).  It sets the latch to
  on for ON, off for OFF, `playing` for TRANSPORT -- turning it off
  flushes in order, as before -- and on the playing -> stopped edge, in
  every mode, flushes whatever is still queued (ON keeps the latch on, so
  the flip does not flush) and then resets the portamento.  The 2b-i
  plugin code is this call with TRANSPORT.
- *Applied on the audio thread only.*  For the latch, the GUI timer and
  `state_load` write the byte `data->eventLatchMode` and nothing else;
  `plugin_process` reads it at the top of the next block (and at each
  in-block transport event) and calls the policy.  A change
  of mode thus takes effect at a block boundary, and the flush it may
  cause (ON -> OFF, or ON -> TRANSPORT while stopped) runs on the audio
  thread, never racing the queue.  The other toggles keep their existing
  main-thread writes (not this slice's to change).
- *No transport*: TRANSPORT means off (the last known state is "not
  playing" from activation); the tooltip and the config comment say so.
- *Config key* `quantize_events=transport|on|off` (also `1` / `0` for on /
  off; anything else is transport), parsed by
  `m4a_latch_mode_from_name`.
- *State byte* appended after the PCM mix-rate float: 0 transport, 1 on,
  2 off.  A short read or a value above 2 loads as transport
  (`m4a_latch_mode_from_byte`).
- *GUI*: an Options submenu "Quantize to GBA Frames" with three radio
  items -- "Follow Transport" (default), "Always", "Never" -- each with
  hover help.

**Step 2, the code.**  `m4a_engine.h` / `.c`: `M4ALatchMode`,
`m4a_engine_apply_latch_mode` (the latch per mode; on `wasPlaying &&
!playing` a drain -- a no-op unless the mode kept the latch on -- then
`m4a_engine_reset_portamento`), `m4a_latch_mode_from_name` /
`_from_byte`.  `m4a_plugin.h`: `eventLatchMode` in `M4APluginData`;
`m4a_gui.h`: the same in `M4AGuiSettings`.  `m4a_plugin.c`:
`plugin_init` sets transport; `load_config_file` reads
`quantize_events`; `apply_transport` calls the policy with the data's
mode; `plugin_process` calls `apply_transport` with the block's transport
or, without one, the policy with `transportWasPlaying` twice (so a mode
changed from the GUI applies at the next block in every host); the GUI
timer and `state_load` copy the byte only; `state_save` appends it after
the mix-rate float and `state_load` reads it optionally; the three
`M4AGuiSettings` builders and the timer's copy-back carry it.  `m4a_gui.cpp`: `radio_menu_item`
and the "Quantize to GBA Frames" submenu.  `poryaaaa.cfg.example` and the
README (the DAW paragraph, the config table, the GUI list) document it.
As with the other options, a project's saved state wins over
`poryaaaa.cfg`: an old project loads as "transport" whatever the config
says.

**Step 3, the tests.**  `test_latch_mode` (`test/test_engine.c`): the two
parsers (names, `1` / `0`, empty, unknown, NULL; bytes 0 / 1 / 2 / 3 /
255); the latch each mode gives playing and stopped (an out-of-range mode
behaves as transport); the stop edge through the policy call in
transport and in on mode -- a queued note-on on another track and a
queued note-on that glides both applied, the gliding note at its own
pitch, the history forgotten, the latch off / still on after it, and the
first note after the stop not gliding (at once / at the next tick); a
mode change without an edge (on -> transport while stopped flushes and
keeps the history; on -> off while playing flushes without a reset), and
off still resetting on the stop edge.  Unit **1733/1733** (1681 + 52).

Beyond the unit tests, a scratch CLAP host (not committed; it `dlopen`s
`build/poryaaaa.clap` and reads `plugin_data` through `m4a_plugin.h`)
drove the real `plugin_process`, `state_save` / `state_load` and
`load_config_file`, 27 checks plus 4 config runs, all passing: activation
latch off; transport playing / stopped; no transport keeping the last
state (off after a stop, on after a play); an in-block
`CLAP_EVENT_TRANSPORT` stop and play taking effect inside the block; the
override set as the GUI timer sets it (the byte only) taking effect at
the next block -- on while stopped and without a transport, off while
playing; NOTE_CHOKE through `process_clap_note_event` (a -1 / -1 choke
and a same-key choke removing a queued note-on, another key's choke
leaving it); on mode's stop edge flushing with the latch kept on; the
state byte last in the save, off and on round-tripping, a save without
the byte or without the float loading as transport with the mix rate
intact, an unknown byte loading as transport; `quantize_events=off / on /
transport / bogus` and no config file giving 2 / 1 / 0 / 0 / 0.  The GUI
was built but not looked at: this environment has no way to open the
menu (no input synthesis or screenshot tool).

**Step 4, the gates.**  `claim_check.sh` against
`2026-10-25_cry-over-song-rows.json`: 59 diffed, 0 moved, 57/66, 0
regressions, every row 0.0; unit 1733/1733, blip 49112/0, songs 561/561.
Both aligned midi runs 0 field differences on 44 rows.  `poryaaaa` and
`poryaaaa-standalone` build.

**Code review of slice 2b-ii (2026-10-25).**  Two independent read-only
reviews (reviewer 1: the policy, the plugin wiring, the state layout and
the GUI against the CLAP SDK headers and the latch code; reviewer 2: the
unit test assertion by assertion, the plan / README / cfg text, and every
place the other persisted options reach).  Neither found a fault in the
policy, the threading of the new byte, the state layout or its
back-compatibility, or the GUI's menu pairing; every place `pwmEnabled` /
`pcmFreqMode` reach, `eventLatchMode` reaches.  Acted on (all outside the
harness's paths; gated together, below):

- *Config whitespace* (both): the loader strips only the line ending, so
  `quantize_events=on ` silently meant transport.
  `m4a_latch_mode_from_name` now ignores surrounding blanks (still
  case-sensitive; `onx` / `10` are not on); the cfg example names the
  `1` / `0` aliases.
- *Test gaps* (reviewer 2), added to `test_latch_mode`: off mode at the
  stop edge (the latch stays off, a glide ends at its target); the start
  edge through the policy (transport turns the latch on, no reset, the
  next note queues); on mode with a note-on and its note-off queued in
  the last frame (the drain runs with the latch on: the one-tick gate,
  STOP at the next tick); the parser's blank / case / prefix cases; an
  inert `CHN_ENV_SUSTAIN` line removed.  Unit **1749/1749** (1733 + 16).
- *Gate after all of it*: claim_check 59 diffed, 0 moved, 57/66, 0
  regressions, every row 0.0; unit 1749/1749, blip 49112/0, songs
  561/561; both aligned midi runs 0 field differences on 44 rows;
  `poryaaaa` and `poryaaaa-standalone` build; the scratch host 27/27 and
  its config runs (`quantize_events=on ` with a trailing blank now gives
  on).
- *Plan wording* (reviewer 2), fixed in place above: the `[main-thread]`
  citation is `ext/state.h` / `ext/timer-support.h`, not `plugin.h`;
  three builders and one copy-back, not five / four; "the byte and
  nothing else" is for the latch only.  The `m4a_plugin.h` comment now
  says the mode is also applied at in-block transport events.

Recorded, not acted on (pre-existing, outside this slice): `state_load`
reads each field in one `read` call where `clap/stream.h` lets a host
return fewer bytes per call -- a chunking host could leave the float half
read and the new byte read from inside it (every earlier field has the
same exposure; a looping read helper for the whole loader is its own
change); the GUI timer's apply-all block, run for any settings change
including the latch mode, still writes the engine from the main thread
(volume refresh, portamento / PWM / poly-debug setters) while
`plugin_process` may run -- the latch's own path does not, and none of
those touch the queue; the shared settings bytes are plain `uint8_t`
(formally a C11 data race, the pattern of every setting here); the
"Follow Transport" tooltip's "with no transport ... Never" is the
last-known state, exact for the standalone, which never sends one; the
plugin-level promises (no-transport branch, next-block application,
state round trip, config parsing, in-block transport events) are guarded
only by the uncommitted scratch host, since no plugin-level harness
exists in the tree.

### Milestone 5 slice 3a: the MIDI tempo grid (2026-10-25)

Scope, small on purpose: `poryaaaa_render`'s `--midi` dispatch on the
ROM's tick grid, behind an opt-in flag.  Out of scope: the engine's own
LFO grid (slice 3b), mid2agb's velocity table, per-tick VOL batching,
`-P`, the CgbSound entry and the LFO / CgbSound order, the plugin, and
the sequencer path (`--song` / `--cry`, byte-identical).  Before any
change the baselines reproduced: `claim_check.sh` against
`2026-10-25_cry-over-song-rows.json` 59 diffed, 0 moved, 57/66, 0
regressions, every row 0.0, unit 1749/1749, blip 49112/0, songs 561/561;
`accuracy.py --input midi` 0 field differences on 44 rows against
`2026-10-25_midi-path-aligned.json`, with `--midi-quantize` 0 against
`-aligned-latch.json` (`results` field by field, ref / test paths left
out).

**Step 1, the reading (before the code).**

*MPlayMain's tempo accumulator* (`~/pokeemerald/src/m4a_1.s` 1169-1172,
1353-1359): once per call, `tempoC += tempoI` (`_081DD86C`), then
`while (tempoC >= 150)` (`_081DD9BC`, `cmp r0, 150; bcc`) one tick of
every track (`_081DD874`: chain walk, commands) and `tempoC -= 150`
(`_081DD9B6`).  The add reads `tempoI` once, before the frame's first
tick, so several ticks can run in one frame (tempoI > 150) and a TEMPO
inside a tick changes only the next frame's add.  *MPlayStart*
(`m4a.c` 634-637): tempoD = tempoI = 150, tempoU 0x100, tempoC 0, so the
first MPlayMain (frame 3, 883882 cycles, slice 2a) adds 150 and plays
tick 0 whatever the song's tempo.  *ply_tempo* (`m4a_1.s` 920-931):
tempoD = byte << 1, tempoI = tempoD x tempoU >> 8 = tempoD.
*mid2agb* (`tools/mid2agb`): the TEMPO byte is `round(60000000.0f /
us)*tbs/2` (agb.cpp 506, the float division in C, the `*tbs/2` in the
assembler: integer, left to right), `tbs` = 1, or 2 with `-X` (main.cpp
181-182); so **tempoI = 2 x floor(round(60e6 / us) x tbs / 2)** -- an
odd BPM loses one.  Event times become clocks `24 x tbs x tick /
division` (midi.cpp 635, truncating); note lengths are converted on
their own (641-646: at least 1, the LUT without `-E`).  Tempo events are
read from the first MTrk only (`ReadSeqEvents`, midi.cpp 350-366;
`ReadTrackEvent` skips every meta event but end-of-track, 523-536) and
written into the first AGB track; without one, the song runs at
MPlayStart's 150 until its first TEMPO.

*The rule, stated:* frame 3 is grid frame 0; before it tempoC = 0 and
tempoI = 150.  Grid frame j adds the tempoI in force after grid frame
j - 1's ticks, then runs ticks while tempoC >= 150.  So with TEMPO 120 at
tick 0, tick n is on grid frame ceil(1.25 n) (slice 1's 3 + ceil(1.25 n));
at 240 (se_exp) the frames run 0, 1, 2, 2, 3, 4, 4, ... (1.6 ticks a
frame, two ticks in one frame on three frames in five); a tempo change at
tick n takes effect on the add after tick n's frame.

*The renderer today* (`cmd/poryaaaa_render.c`): `parse_midi` places each
event at `tick_to_sample` (the exact tempo map, every track's tempo
events, 120 BPM by default) and adds a synthetic RENDER_EVT_TEMPO per
tempo (plus 120 BPM at sample 0 when the file does not set one at tick
0) that `dispatch_event` passes to `m4a_engine_set_tempo_bpm` (tempoI =
round(bpm), the engine's LFO only).  The loop expansion (`hasLoop`)
repeats the body events in sample space (pass k = pass 0 + k x the body's
exact duration), gate-carries short notes over the loop end and drops
whatever lands at or past `totalSamples`.  The render loop renders to
each event's `samplePos + midiOrigin` and dispatches it; with
`--gba-start-delay`, midiOrigin O = floor((883882 - 2048) x rate / 2^24),
the last sample a blip frame before frame 3's SoundMain (2522 at 48 kHz:
dispatched at cycle 882538; slice 2a's bound, dispatch at c(s) - 1025 ..
c(s) + 2047.5 from any phase), and the natural tail / loop-count fade
move by O.  The fade is a linear gain ramp over the last samples.
*The engine's `!seq` accumulator* (`m4a_engine_tick`, after the CgbSound
passes): `tempoC += tempoI; while (tempoC >= 150) { tempoC -= 150;
m4a_lfo_tick }` -- LFO only; the MIDI path's tempo reaches it at the
dispatch, i.e. on the current frame's add (the review item of slice 2a),
and its phase runs from the boot.  Not changed here.

*The design.*  A flag `--gba-tempo-grid` (the `--midi` path only; a note
with `--song` / `--cry`).  Each event carries its song tick (the SMF
tick, plus pass x the loop body's ticks for a loop pass, the gate-carried
release at its own tick); after the loop expansion, which still runs in
exact MIDI time, a walker takes the events in order, converts each song
tick to mid2agb clocks (24 x tbs x tick / division, tbs from
`--extended-clocks`), places it on the ROM's grid frame with the rule
above (tempoI from the first MTrk's tempo events, MPlayStart's 150 before
the first), and moves the event to **sample floor((883882 + j x 280896 -
2048) x rate / 2^24) - O**: the same blip-frame guard as the origin, at
every grid frame, so with `--gba-start-delay` the event is dispatched
between grid frame j - 1's SoundMain and grid frame j's (2522 + ... at
48 kHz; j = 0 gives sample 0, i.e. exactly today's origin).  Several
ticks in one frame share one sample and keep their order.  The tempo
events are dispatched like the rest, on their tick's frame, with the
value they carry today (the engine's LFO grid is slice 3b).  The
render length: an explicit `--total-duration-seconds` (the harness) is
output time and stays; the natural tail starts at the grid sample of the
last event; the loop-count fade at the grid sample of loop start + count
x body; the expansion runs with its bound widened by the largest ratio
of grid to exact time (so a slower grid cannot run out of expanded
events) and the render loop drops what lands past the end, as today.
The name says what the flag does and pairs with `--gba-start-delay`: the
game's tempo grid (tempoC / 150) instead of the MIDI's tempo map.
Without `--gba-start-delay` the grid applies the same way with sample 0
at tick 0 (an event on grid frame j >= 3 is then 3 frames early, the
start offset of slice 1; grid frames 1 and 2 fall into the boot and
reach frame 3); the harness passes both.

*Not modelled* (all no-ops on the corpus, every MIDI of which is 24
ticks per beat, `-E`, no `-X`, one tempo track, even BPMs): mid2agb's
per-note length conversion (the note-off is placed at its own tick's
clock, not at the note-on's clock plus the converted length; they differ
only when the division is not 24 x tbs), the LUT gate without `-E`,
tempo events outside the first MTrk (dispatched to the engine, ignored
by the grid, as mid2agb ignores them).

*Predictions* (a Python model of both dispatch rules over the 44 MIDIs,
the dispatch bound of slice 2a; it reproduces slice 2a's exception lists
exactly):

1. **Flag off: 0 field differences** on both midi runs against the
   aligned pair; the song-input gate unchanged (claim_check 59 / 0 moved
   / 57/66 / 0 regressions / every row 0.0).
2. **The events that change frame** (flag on vs aligned): vol 6 (110,
   114, ..., 130), bend 8 (45, 57, 69, 81, 90, 93, 102, 126), lfo_vib /
   lfo_trem / lfo_pan 2 (102, 126), psg_mix 3 (90, 102, 114): **each one
   frame later, onto the ROM's frame**.  se_exp 20 (from tick 45),
   se_contest_place 3 (25 .. 27), likewise one frame.  Every other micro
   row and se_faint / se_truck_unload / se_low_health (its loop included)
   / se_rain_stop: **no event changes frame**.  The songs: 254 (groudon)
   .. 1682 (rg_vs_trainer) events in 20 s, 1 .. 6 frames later (credits:
   1306 in 60 s, up to 17, its 22 moved TEMPO events not counted), i.e.
   the 0.4562 % drift.  No row has a note
   whose note-on and note-off share a frame on the grid (the case where
   the latch's one-tick rule and the ROM, which kills START | STOP, would
   part) nor today.
3. **Rows with no moved event: byte-identical** to their aligned WAVs
   (the dispatch sample changes, the frame does not): all of sq1 .. pcm_psg
   but vol, bend, lfo_vib / trem / pan and psg_mix, and the four SEs above.
   lfo_delay in particular stays as it is (-2.3): its residual is the LFO
   grid alone.
4. **vol and bend: -inf**, bit-identical to their `--song` renders (PCM
   voice 10, their only difference was the early frames; vol's VOL-before-
   note ticks 36 and 72 were already identical in the aligned run).
5. **lfo_vib / lfo_trem / lfo_pan**: still not null (the LFO grid, from
   frame 3); the renders change from tick 102's frame on only.
   **psg_mix**: still bounded by the CgbSound entry (-2.1 today); it
   changes from tick 90's frame on.
6. **se_exp / se_contest_place**: closer to their `--song` renders, still
   failing (se_exp's first state difference is a CGB pitch, so the PSG
   write timing remains; se_contest_place keeps the per-tick VOL batching
   at frame 25).
7. **The songs: drift 0.0** on all seven (the grid is the ROM's; the
   aligner's 4 s segments resolve a frame), lag near 0 where they
   correlate; null deeper than today's -2.4 .. +2.3 but not passing:
   the LFO grid, the PSG entry, the VOL batching and the engine track
   numbering (SMF track i, conductor 0, against the sequencer's 0..n-1:
   the steal tie-break) remain.  The velocity table is not among them:
   every note velocity in the 13 song / SE MIDIs is a multiple of 4 or
   127, the table's fixed points.
8. **The latch**: flag-on runs with and without `--midi-quantize`
   byte-identical (slice 1's prediction 12; no same-frame note pair).

**Step 2, the flag** (`cmd/gba_tempo_grid.h` / `.c`, new;
`cmd/poryaaaa_render.c`; no engine change).  The module holds the rule:
`gba_tempo_grid_tempo_i` (mid2agb's byte doubled; the `roundf` of a float
division as agb.cpp's, the byte truncated to 8 bits as gas would),
`_clock` (24 x tbs x tick / division), `_frame` (the walker:
`while (tempoC < 150) { frame++; tempoC += tempoI; } tempoC -= 150` per
tick, queries in order), `_set_tempo` (the next add), `_sample` (floor((883882 + j x
280896 - 2048) x rate / 2^24); `ROM_FIRST_TICK_CYCLE` moved here from the
renderer, which still uses it for O) and `_dispatch_sample(grid, exact,
songTick, tempoUs, rate)`, which the renderer calls for every event in
both modes: a NULL grid (flag off) returns the exact sample unchanged.
The renderer: `TempoEvent.track`; `RenderEvent.songTick` and `.loopPass`
(since the review: the SMF tick within its pass and the pass, the
gate-carried release at loop start + rr on pass + passDelta, the wrap
release at loop end on its pass; the walk takes clock(tick) + pass x
(clock(loop end) - clock(loop start)), `gba_tempo_grid_pass_clock`; at
the commit songTick was tick + pass x the body's ticks) and
`.gridTempoUs` (a first-MTrk tempo's us per beat, 0 for the synthetic 120
BPM default, other tracks' tempos and, since the review, a TEMPO on the
loop end's tick); the expansion's bound `expandLimit` (= `totalSamples`
without the flag; with it `grid_expand_limit`, the bound widened by
max(kx x kg, kx, 1), kx / kg the largest exact / grid and grid / exact
seconds-per-tick ratios over the tempos, plus four frames -- since the
review the grid's ratios scaled by the loop pass's clocks / ticks and two
of the longest clocks added); after the expansion the walk; with the
flag, the loop-count fade at the grid sample of loop start + count x
body and the natural tail after the last channel event's grid sample (at
the commit, the last event's, a tempo included).  The
render loop and `--gba-start-delay` are untouched.  `accuracy.py
--gba-tempo-grid` / `--no-gba-tempo-grid` (default off, as the renderer:
neither tool's default changes before the user decides) and the report's
`"gba_tempo_grid"`.  The name: what the flag changes is which grid the
ticks sit on (the game's tempoC / 150 one instead of the MIDI's tempo
map), and it pairs with `--gba-start-delay`, the other "as the game
would" switch.

**Step 3, the unit test** (`test_gba_tempo_grid`, 69 assertions, calling
the module the renderer calls): tempoI for 120, 120 with `-X` (240), 75
(74), 240, 130.0001 (130); clocks at division 96 and with `-X`; no TEMPO
(tick n on frame n); 120 (frame ceil(1.25 n), n = 1 .. 400); 240 (ticks
0 .. 10 on frames 0, 1, 2, 2, 3, 4, 4, 5, 5, 6, 7: two ticks in one frame);
a tempo change inside a two-tick frame (240 -> 60 at tick 2: tick 3 still
on frame 2, tick 4 on frame 4, tick 5 on frame 7; a placed tick queried
again keeps its frame); grid frame 0 at slice 2a's origin at 22050 /
44100 / 48000 / 96000 (1158 / 2317 / 2522 / 5045); two ticks of one frame
on one dispatch sample; no grid: 205 exact samples passed through
unchanged; on the engine at 22050 / 48000 / 96000, latched and not, grid
frames 1, 5 and 37: after the kept boot and the frame's dispatch sample
the machine is past the previous SoundMain and short of its own, and a
note-on there has started at that SoundMain.  Written first with one
wrong expectation of mine (tick 8 at 240 on frame 6; the walker's 5 is
right: frame 5 adds 240 to 60); unit **1818/1818** (1749 + 69).

**Step 4, the measurement.**  Song-input gate: `claim_check.sh` against
`2026-10-25_cry-over-song-rows.json` 59 diffed, 0 moved, 57/66, 0
regressions, every row 0.0 (and 0 field differences on 66 rows), unit
1818/1818, blip 49112/0, songs 561/561.  Flag off: `--input midi` and
`--input midi --midi-quantize` **0 field differences on 44 rows** against
the aligned pair, the stdout tables identical, and the 44 WAVs
**byte-identical** to those of the f80c004 renderer (built from a
scratch worktree).  Flag on: `baselines/2026-10-25_midi-path-tempo-grid.json`
/ `.txt` and `-tempo-grid-latch.json` / `.txt`, **14/44** at -40 dB
(aligned 12/44: + bend, vol); the latched run 0 field differences against
the unlatched one and its 44 WAVs byte-identical.  `compare_reports.py`
aligned -> tempo-grid: 0 regressions over 1 dB.  Both latch forms are the
same table:

| test | lag ms | gain dB | null | worst1s | ref / test dBFS | drift | aligned lag / null / worst1s / drift |
|------|-------:|--------:|-----:|--------:|----------------:|------:|------------------------------------:|
| sq1 | 0.0 | -0.0 | -37.2 | -32.9 | -16.8 / -16.8 | - | 0.0 / -37.2 / -32.9 / - |
| sq1_env | 0.2 | -2.2 | -3.5 | -1.2 | -18.3 / -18.3 | - | 0.2 / -3.5 / -1.2 / - |
| sq1_sweep | 0.0 | -0.0 | -43.1 | -37.1 | -17.0 / -17.0 | - | 0.0 / -43.1 / -37.1 / - |
| sq1_alt | 0.0 | -0.0 | -35.5 | -30.6 | -16.8 / -16.8 | - | 0.0 / -35.5 / -30.6 / - |
| sq2 | 0.0 | 0.0 | -inf | -inf | -18.1 / -18.1 | - | 0.0 / -inf / -inf / - |
| sq2_alt | 0.0 | -0.0 | -34.5 | -29.7 | -18.1 / -18.1 | - | 0.0 / -34.5 / -29.7 / - |
| wave | 0.2 | -0.3 | -11.8 | -9.4 | -19.8 / -19.8 | - | 0.2 / -11.8 / -9.4 / - |
| wave_alt | 0.1 | -0.4 | -10.6 | -8.2 | -19.8 / -19.8 | - | 0.1 / -10.6 / -8.2 / - |
| noise | 0.0 | -1.5 | -5.1 | -3.1 | -20.0 / -20.0 | - | 0.0 / -5.1 / -3.1 / - |
| noise7 | 0.0 | -1.2 | -6.0 | -2.9 | -18.5 / -18.5 | - | 0.0 / -6.0 / -2.9 / - |
| pcm | 0.0 | 0.0 | -inf | -inf | -21.4 / -21.4 | - | 0.0 / -inf / -inf / - |
| pcm_hi | 0.0 | 0.0 | -inf | -inf | -20.9 / -20.9 | - | 0.0 / -inf / -inf / - |
| pcm_adsr | 0.0 | 0.0 | -inf | -inf | -29.9 / -29.9 | - | 0.0 / -inf / -inf / - |
| pcm_drum | 0.0 | 0.0 | -inf | -inf | -21.9 / -21.9 | - | 0.0 / -inf / -inf / - |
| pcm_cmp | 0.0 | 0.0 | -55.0 | -50.2 | -27.1 / -27.1 | - | 0.0 / -55.0 / -50.2 / - |
| pcm_reverb | 0.0 | 0.0 | -inf | -inf | -21.0 / -21.0 | - | 0.0 / -inf / -inf / - |
| pcm_chord | 0.0 | -0.0 | -55.7 | -51.9 | -14.2 / -14.2 | - | 0.0 / -55.7 / -51.9 / - |
| pcm_bass | 0.0 | 0.0 | -inf | -inf | -25.2 / -25.2 | - | 0.0 / -inf / -inf / - |
| pan | 0.0 | 0.0 | -inf | -inf | -19.5 / -19.5 | - | 0.0 / -inf / -inf / - |
| vol | 0.0 | -0.0 | -47.2 | -41.4 | -25.3 / -25.3 | - | 0.0 / -31.2 / -20.3 / - |
| vel | 0.0 | 0.0 | -inf | -inf | -26.5 / -26.5 | - | 0.0 / -inf / -inf / - |
| bend | 0.0 | 0.0 | -inf | -inf | -20.2 / -20.2 | - | 5.0 / 1.0 / 3.4 / - |
| lfo_vib | -0.1 | -3.3 | -2.0 | -0.7 | -20.1 / -20.1 | - | -3.8 / -1.7 / -0.6 / - |
| lfo_trem | 0.0 | -0.0 | -27.4 | -26.2 | -20.1 / -20.1 | - | 3.9 / -13.4 / -8.6 / - |
| lfo_pan | 0.0 | -0.0 | -27.5 | -26.4 | -20.1 / -20.1 | - | 3.9 / -13.4 / -8.6 / - |
| lfo_delay | -3.5 | -3.0 | -2.3 | -1.2 | -21.0 / -21.0 | - | -3.5 / -2.3 / -1.2 / - |
| lfo_psg | -18.8 | -6.5 | 0.2 | 1.0 | -17.1 / -17.9 | - | -18.8 / 0.2 / 1.0 / - |
| tune | 0.0 | 0.0 | -inf | -inf | -21.6 / -21.6 | - | 0.0 / -inf / -inf / - |
| tune_psg | 0.0 | -0.0 | -33.5 | -26.5 | -18.0 / -18.0 | - | 0.0 / -33.5 / -26.5 / - |
| psg_mix | 0.0 | -2.1 | -3.8 | -2.0 | -15.2 / -15.1 | - | 0.0 / -2.1 / 1.9 / - |
| pcm_psg | 0.0 | -0.1 | -17.3 | -14.4 | -13.3 / -13.3 | - | 0.0 / -17.3 / -14.4 / - |
| mus_littleroot | 0.0 | -0.5 | -9.3 | -4.0 | -23.7 / -23.7 | 0.0 | 16.7 / 2.1 / 4.0 / 5.0 |
| mus_weather_groudon | -14.3 | -2.7 | -2.7 | 1.2 | -31.1 / -31.1 | -0.5 | 55.7 / -2.4 / 2.1 / 4.7 |
| mus_vs_wild | 0.0 | -0.4 | -10.0 | -5.0 | -20.9 / -20.9 | 0.0 | 67.0 / 2.1 / 3.7 / 4.2 |
| mus_mt_chimney | 0.0 | -2.8 | -2.6 | 2.0 | -23.5 / -23.5 | 0.0 | 16.7 / 1.7 / 3.7 / 4.6 |
| mus_rg_vs_trainer | 0.0 | -1.6 | -4.9 | -0.7 | -19.6 / -19.6 | 0.0 | 33.5 / 2.3 / 3.9 / 4.2 |
| mus_vs_trainer | 0.0 | -0.2 | -12.4 | -4.7 | -20.4 / -20.4 | 0.0 | 67.0 / 2.0 / 3.5 / 5.0 |
| mus_credits | 0.0 | -0.1 | -14.4 | -7.0 | -23.0 / -23.0 | 0.0 | 178.0 / 2.2 / 5.4 / 4.6 |
| se_exp | 0.2 | -4.6 | -0.8 | 1.6 | -24.8 / -24.8 | - | -1.9 / 2.0 / 3.0 / - |
| se_faint | 0.0 | -1.3 | -5.5 | -2.4 | -29.9 / -29.9 | - | 0.0 / -5.5 / -2.4 / - |
| se_contest_place | 0.0 | -0.4 | -10.8 | -8.6 | -26.9 / -26.9 | - | 0.0 / -10.8 / -8.5 / - |
| se_truck_unload | 0.0 | -8.8 | 1.1 | 1.1 | -39.2 / -39.2 | - | 0.0 / 1.1 / 1.1 / - |
| se_low_health | 7.9 | -3.4 | -1.9 | -0.5 | -24.0 / -24.0 | - | 7.9 / -1.9 / -0.5 / - |
| se_rain_stop | 0.0 | 0.0 | -32.9 | -23.4 | -41.3 / -41.3 | - | 0.0 / -32.9 / -23.4 / - |

**Which predictions held.**

1. *Flag off identical*: held (fields, tables, WAVs; the song gate).
2. *The moved events*: held.  Each moved row first departs from its
   aligned render on the ROM frame of its first moved tick, one frame
   after the aligned render's: vol tick 110 (frame 141), bend 45 (60),
   lfo_vib / lfo_trem / lfo_pan 102 (131), psg_mix 90 (116; its PSG
   difference shows on 115, the aligned one's frame), se_exp 45 (32; 31),
   se_contest_place 25 (30) -- PCM rows counted by the output sample's
   frame, one after the SoundMain that mixes it.
3. *Rows with no moved event byte-identical*: **held exactly**, 29 WAVs
   (the 25 of the 31 micro rows without a moved event and se_faint, se_truck_unload, se_low_health with
   its loop, se_rain_stop).  lfo_delay -2.3 unchanged: its residual is the
   LFO grid alone.
4. *vol and bend -inf*: **bend held** (-inf, bit-identical to its `--song`
   render over the whole file; aligned +1.0).  **vol did not** (-47.2 /
   -41.4, passing; aligned -31.2): its first difference from the `--song`
   render is in output frame 169, the one after tick 132's SoundMain
   (frame 168), 3464 samples, max 216 (about one PCM LSB).  Tick 132 carries a VOL 127 and the note's end, then FINE
   (`vol.s`: N24 at 108, `VOL 127`, `FINE`).  `ply_fine` (m4a_1.s
   751-777) sets STOP on each channel, unlinks it (`RealClearChain`) and
   clears the track's flags, so the end-of-tick VOLCHG pass never reaches
   the releasing note: it releases at 126.  The MIDI path applies CC 7 to
   it.  The per-tick command batching family (slice 2a's se_rain_stop),
   not the grid; I had not checked the last tick.
5. *lfo_* not null, psg_mix bounded*: held.  lfo_trem / lfo_pan -13.4 ->
   -27.4 (worst -8.6 -> -26.2), more than I expected: the second note now
   starts on the ROM's frame, leaving the LFO grid's one-step amplitude /
   pan differences; lfo_vib -1.7 -> -2.0 (a pitch difference accumulates
   phase, like bend did).  psg_mix -2.1 -> -3.8.
6. *se_exp / se_contest_place still failing*: held (se_exp +2.0 -> -0.8,
   first difference from its `--song` render still frame 8;
   se_contest_place -10.8 unchanged, first difference frame 3).
7. *The songs*: **drift 0.0 on six songs** (aligned 4.2 .. 5.0), lag 0.0 on
   the same six, null -2.6 .. -14.4 (aligned +1.7 .. +2.3); groudon's
   drift -0.5 and lag -14.3 did not hold, but groudon is the pure-PSG song
   whose waveforms do not correlate (null -2.7; the skill's "drift is noise
   there").  Each song's first difference from its `--song` render is on
   or after the aligned one's (littleroot frame 20 = tick 12, vs_trainer
   13 = tick 12: note-ons on PSG tracks; credits 133 -> 643, three frames
   after tick 384's ROM frame 640, the only event frame in 636 .. 644, where
   track 7 has a VOL before a note in the tick -- not taken apart further;
   the other four on frame 3, the PSG entry), with the differing sample count down on six
   (vs_trainer 949220 -> 484653, credits 2771850 -> 2067076).  Not passing,
   as predicted.
8. *The latch*: held, byte-identical.

**The songs' drift**, aligned -> tempo grid (ms/s): littleroot 5.0 ->
0.0, groudon 4.7 -> -0.5 (uncorrelated), vs_wild 4.2 -> 0.0, mt_chimney
4.6 -> 0.0, rg_vs_trainer 4.2 -> 0.0, vs_trainer 5.0 -> 0.0, credits 4.6
-> 0.0 (23 of its 61 tempo changes inside the 60 s window, all at 94 BPM
or below; the two-tick frames of rg_vs_trainer, vs_wild and vs_trainer at
172 / 196 / 198 BPM and of the SEs at 220 / 240 all on the ROM's frames).

**Attribution of what is left on the flag-on midi path** (each against
its own `--song` render):

- *The LFO grid* (slice 3b): lfo_vib, lfo_trem, lfo_pan, lfo_delay; on
  the songs wherever MOD is set.
- *The CgbSound entry and the LFO / CgbSound order*: every PSG micro row,
  psg_mix, pcm_psg's PSG half, lfo_psg, se_exp, se_faint, se_truck_unload,
  se_low_health; the songs' PSG tracks (their first differences on frame
  3 or on PSG note frames).
- *Per-tick command batching*, now with FINE: vol (the FINE tick's VOL),
  se_rain_stop and se_contest_place (a VOL before a note), credits from
  tick 384.
- Not seen here: the velocity table (a no-op on the corpus), `-P` (no
  cross-player row), the per-note length conversion and the LUT gate
  (every corpus MIDI is 24 ticks per beat with `-E`).

**What slice 3b (the engine's LFO grid) needs.**  The engine's `!seq`
accumulator does the ROM's arithmetic but with two offsets: the tempo set
by a dispatch before frame F's SoundMain is added on frame F, where the
ROM's TEMPO in a tick of frame F first adds on F + 1; and its phase runs
from the boot (MPlayStart's tempoI 150 / tempoC 0 consumed by the boot's
frame-2 tick) instead of from the song start, which puts its n = 0 mod 4
steps on frames 4, 9, 14, ... where the ROM's are on 3, 8, 13, ... at 120
(the other steps coincide).  With the
grid the renderer dispatches every TEMPO exactly on its tick's frame, so
3b's rule becomes exact rather than approximate: a MIDI-path song-start
reset at the origin (tempoC 0, tempoI 150, as MPlayStart) and a tempo
that takes effect after the current frame's add.  Then the engine's
accumulator is the grid's walker, step for step.  The grid also exposes
two inputs 3b must change: the renderer still passes `round(bpm)` to
`m4a_engine_set_tempo_bpm`, where the grid runs mid2agb's tempoI (one
less on an odd BPM, double with `-X`, MPlayStart's 150 before a first
TEMPO and none from other tracks).  Passing the grid's tempoI through the
same event would make the two accumulators agree (read from the code, no
`-X` row in the corpus: a `-X` song's LFO steps at half the ROM's rate on
the MIDI path today).  The design does not change for the
plugin: it has no song start or tempo grid (the host's timeline is the
grid), so 3b's reset and deferred tempo are renderer-path behaviour
(`!seq` with the renderer's flags), and the plugin keeps the
dispatch-frame tempo.  The LFO / CgbSound order (slice 1 item 6) is a
separate item that lfo_psg also needs.

**Code review of slice 3a (2026-10-25).**  Two independent read-only
reviews (reviewer 1: the walker, tempoI, clocks, tempo selection and the
dispatch sample against MPlayMain's tempo loop, ply_tempo, MPlayStart,
ply_fine and mid2agb's main.cpp / midi.cpp / agb.cpp, with a gas test of
`TEMPO , N*tbs/2` and a Python model of the blip stepping at 8000 ..
192000 Hz; reviewer 2: every number of this section recomputed from its
own SMF parser and model of both dispatch rules, the table against the
two baselines field by field, the 29 WAVs `cmp`ed, the unit test
assertion by assertion, the runner, the docs).

No divergence on any corpus path, and no number of the table wrong.
Confirmed: the per-frame add, MPlayStart's seed, a TEMPO taking effect on
the next frame's add (lazy placement is safe, every TEMPO being an event
in order), tempoI to the bit (float division, gas's truncating `*tbs/2`
into a `.byte`, ply_tempo's doubling), the clock conversion, the
first-MTrk tempo selection, the dispatch frame at every rate from 8000 Hz
(S_j - B >= 42 cycles over 30000 frames, three phases; without the
start delay grid frames 0 .. 3 collapse onto frame 3), the event order
after the walk, the flag-off path, the moved-event lists, origins, table
(44 rows, 0 mismatches), byte-identities and the 69 assertions.

*Acted on*, each applied alone and gated alone (claim_check 59 diffed, 0
moved, 57/66, 0 regressions, every row 0.0, 0 field differences on 66
rows; blip 49112/0, songs 561/561; the four midi runs -- aligned,
aligned-latch, tempo-grid, tempo-grid-latch -- 0 field differences on 44
rows against their baselines and the 44 grid WAVs byte-identical: every
one harness-neutral, as none of the five cases occurs in the corpus):

- *A TEMPO byte of 0 hung the walker* (reviewer 1).  A BPM of 512, 513,
  1024, ... (256, 512, ... with `-X`) wraps the byte to 0; ply_tempo stores
  tempoI 0 and the ROM never ticks again, while `while (tempoC < 150)`
  spun forever.  The walker now freezes (`GBA_TEMPO_GRID_NEVER`, a sample
  past any render; a later TEMPO, inside a tick never played, cannot
  restart it), `grid_expand_limit` skips tempoI 0 instead of treating it as
  1 (a 150x expansion), the natural tail and the print skip the
  never-played events, a fade past the stop keeps the exact-tempo fade.
  Unit test: 512 BPM -> tempoI 0, tick 0 plays, tick 1 and its TEMPO never,
  the walker stays frozen.  A scratch MIDI (TEMPO 512 at 0, a note, TEMPO
  120 at 12, a note at 24) renders in 0.5 s (the old loop's spin is read
from the code, not run).
- *A TEMPO on the loop end's tick* (reviewer 1).  mid2agb sorts the GOTO
  (LoopEnd 0x12) before a same-clock TEMPO (0x19; midi.cpp EventCompare),
  so the ROM never plays it; the expansion kept it as a grid tempo change.
  Its `gridTempoUs` is now 0 in each pass (it still reaches the engine as
  before, the flag-off path unchanged).  A scratch loop [0, 24] with TEMPO
  120 at 0 and 240 at 24: the loop-count fade now at 1.0046 s, 120
  throughout (with the TEMPO applied it would be about 0.75 s, computed).
- *Loop passes in clocks* (reviewer 1).  A pass plays at clock(t) + p x
  (clock(E) - clock(S)); the walker took clock(t + p x L), one clock short
  every few passes when the body is not whole clocks (division 96, loop
  [2, 388]: pass 2 of tick 2 is clock 194, the old rule 193).  Now
  `songTick` is the tick within its pass and `loopPass` the pass,
  `gba_tempo_grid_pass_clock` (unit test: 194, and a tick outside the loop
  its own clock).  At division 24 nothing moves: littleroot's loop-count
  render byte-identical.
- *The expansion bound's slack* (reviewer 1).  It covered the frame
  rounding but not a tick's truncation to its clock, several frames long
  at a slow tempo, nor (with the clock passes) a pass up to a clock shorter
  than its ticks, compounding per pass.  The grid's seconds per tick are
  now scaled by the pass's clocks / ticks, and two of the longest clocks
  added.  A scratch 20 BPM, division 96 loop at 8000 Hz, 30 / 37.3 / 61.7 s:
  the last placed tick past the render end each time.
- *The natural tail after a tempo* (both).  It started after the last
  event of any kind; now after the last channel event, as the exact
  path's `totalMidiSamples` (a scratch MIDI with a TEMPO at tick 96 after
  the last note-off at 24: 1.50 s both ways; the old rule gives about
  3 s, computed).
- *The unit test* (reviewer 2): "130.0001 BPM rounds to 130" was 130.00004
  and passes under truncation too; now 461539 us (129.9998: round 130,
  truncation 128).  The byte wrap is the 512 BPM case above.  Unit
  **1824/1824** (1749 + 75).
- *Messages* (reviewer 2): the usage text's 0.46 % holds at an even BPM
  (mid2agb drops an odd one by one); the print names the last MIDI
  event's tick's ROM frame, taken before the fade query, and says events
  past the render end are dropped.
- *Docs* (reviewer 2): only three songs have two-tick frames
  (rg_vs_trainer, vs_wild, vs_trainer; credits' 182 BPM is at tick 6144,
  past its window); 23 of credits' 61 tempos fall in its 60 s; credits'
  first difference at 643 is three frames after tick 384's frame 640, not
  "= tick 384"; vol's first difference is output frame 169, the frame after
  tick 132's SoundMain; "the 25 of the 31 micro rows"; credits' 1306 moved
  events exclude its 22 TEMPOs; the skill's drift is six songs' (groudon
  -0.5); the README's opt-out is accuracy.py's; the 3b paragraph's frames
  are the n = 0 mod 4 steps only.  Step 2's description now gives the
  code as it stands, with the commit's form in parentheses.

*Recorded, not acted on:*

- *Note-offs at their own tick's clock* (reviewer 1): the ROM releases at
  the note-on's clock plus the converted length (at least 1, the LUT
  without `-E`; CreateTies' EOT at time(on) + duration), which differs
  from clock(off) when the division is not 24 x tbs or without `-E`.  In
  the step-1 "not modelled" list; every corpus MIDI is 24 ticks per beat
  with `-E`.
- *Two first-MTrk tempos on one tick* (reviewer 1): `cmp_tempo_events`
  has no tie-break and qsort is not stable, where mid2agb keeps file order
  and the last wins.  Pre-existing in the exact path too; none in the
  corpus.
- *A tick shorter than an output sample* (reviewer 1): parse_midi orders a
  tempo before a note, the loop merge a carried release before a body
  event, at an equal exact sample; with different ticks the walker then
  returns the later tick's frame for the earlier one.  Needs a division
  far above any corpus file at a low rate.
- *The engine's tempo* (reviewer 1): `m4a_engine_set_tempo_bpm` still
  gets round(BPM), the 120 default and every track's tempos (slice 3b).
- *Coverage* (reviewer 2): the loop expansion's ticks, the bound, the
  fade / tail and parse_midi's tempo selection are static renderer code,
  checked here by scratch renders, not by the unit test; the engine
  sub-test covers 22050 .. 96000 Hz (the reviewers' models cover 8000).
- *Dispatch-rule ambiguity* (reviewer 2): under the phase-free dispatch
  band 21 rg_vs_trainer and 2 mt_chimney events could fall either side of
  a SoundMain on the aligned path; the step-1 counts use slice 2a's
  measured phase.  Not a grid question (the grid's dispatch has no
  ambiguity on any row).
- The step-1 and step-3 texts are kept as written; this paragraph says
  where the review changed them.

**Decision (2026-10-25, the user).**  The harness passes
`--gba-tempo-grid` on every `--input midi` run (`--no-gba-tempo-grid`
opts out; `accuracy.py`'s `--gba-tempo-grid` switch is gone, the grid
being the default), as it does `--gba-start-delay`: the grid removes a
difference already understood (the drift that decorrelated every song)
so that what is left can be attributed, and slice 3b's LFO-grid fix is
only exact on it.  The renderer's own default stays off: a user's MIDI
with no first-MTrk tempo at tick 0 would play at 150 BPM, other tracks'
tempos would be ignored, an odd BPM would lose one, and for the 480 / 960
ticks-per-beat files a DAW writes the note ends are not yet mid2agb's
(the converted length and the LUT without `-E` are not modelled), so
"plays as the game" would be promised where it does not fully hold; the
plugin cannot use the grid at all.  To revisit once the note-length
conversion and a `-E` switch exist, possibly as one "as the game"
preset with the start delay.  The tempo-grid pair is now the midi
baseline the next slice compares against; the aligned pair is history.

### Milestone 5 slice 3b: the engine's LFO grid (2026-10-25)

Scope, small on purpose: the engine's `!seq` tempo accumulator (the MIDI
path's LFO and opt-in portamento clock) steps on the frames of the
renderer's grid walker, under `poryaaaa_render --gba-tempo-grid` only.
Out of scope: the LFO / CgbSound order (slice 1 item 6), the CgbSound
entry, per-tick command batching (a VOL before a note, ply_fine's unlink),
mid2agb's note-length conversion and `-E`, `-P`; the plugin (its tempo
comes from the host per block: `m4a_plugin.c`'s `m4a_engine_set_tempo_bpm`
path stays); the sequencer path and the flag-off renderer (byte-identical).
Before any change the baselines reproduced: `claim_check.sh` against
`2026-10-25_cry-over-song-rows.json` 59 diffed, 0 moved, 57/66, 0
regressions, every row 0.0 (0 field differences on 66 rows), unit
1824/1824, blip 49112/0, songs 561/561; `accuracy.py --input midi` 0 field
differences on 44 rows against `2026-10-25_midi-path-tempo-grid.json`,
with `--midi-quantize` against `-tempo-grid-latch.json`, with
`--no-gba-tempo-grid` against `-aligned.json` and `-aligned-latch.json`.

**Step 1, the reading (before the code).**

*MPlayMain's per-track tick* (`~/pokeemerald/src/m4a_1.s` 1173-1359).
For each track with `MPT_FLG_EXIST`: the chain walk (gate countdown,
`_081DD892`), the track start (`MPT_FLG_START`: Clear64byte, lfoSpeed 22),
then the commands while `wait == 0` (`_081DD8E0`); after a command the
track is re-checked (`ldrb flags; cmp 0; beq _081DD994`: a FINE, which
clears the flags, skips the rest of the tick); then `wait -= 1` and **the
LFO step** (`_081DD938` .. `_081DD990`): nothing if lfoSpeed or mod is 0;
else if lfoDelayC != 0 decrement it and stop; else `lfoSpeedC += lfoSpeed`
(the untruncated sum kept in r1), the triangle, `modM' = mod x tri >> 6`,
and on a change of modM's low byte store it and flag PITCHG (modT 0) or
VOLCHG.  So in one tick a track's note-on comes **before** its LFO step.
After all ticks of the frame (the `while (tempoC >= 150)` loop), one
VOLCHG / PITCHG pass (`_081DD9C8`): TrkVolPitSet, then each chained
channel's volume / frequency (`ChnVolSetAsm`, MidiKeyToFreq or the CGB
freq with `MO_PIT` / `MO_VOL`), flags `&= 0xF0`.  *ply_note*
(`_081DDC54`): the new channel linked, `lfoDelayC = lfoDelay`, and with a
delay `clear_modM` (modM = lfoSpeedC = 0, PITCHG or VOLCHG), then
TrkVolPitSet.  *ply_mod* / *ply_lfos* (1887-1909): store; a 0 calls
clear_modM.  *ply_lfodl* (1019-1025): stores lfoDelay only (the next note
loads it).  *ply_modt* (1027-1041): on a change, VOLCHG | PITCHG.
*ply_tempo* (920-931): tempoD = byte << 1, tempoI = tempoD x tempoU >> 8,
read by the next frame's add (`_081DD86C`, before the frame's first tick).
*MPlayStart* (`m4a.c` 634-637): tempoD = tempoI = 150, tempoU 0x100,
tempoC 0.  *MPlayProcessPortamento* (pokeemerald ccd2ad2e, the opt-in
portamento's source): called after the VOLCHG / PITCHG pass, so it reads
the tempoI a TEMPO of this frame's ticks already stored.

*The engine* (`plugin/m4a_engine.c`).  `m4a_engine_tick`: gates, the
latched drain, CgbSound, then `if (!seq) { tempoC += tempoI; while (tempoC
>= 150) { tempoC -= 150; m4a_lfo_tick } }`, portamento (`elapsed +=
tempoI`), PWM.  `m4a_lfo_tick` is the LFO step with the VOLCHG / PITCHG
pass folded in (on a modM change it updates the track and its channels at
once; the frame's last step wins, as the ROM's single pass).  The note-on
(`note_on_restart_lfo_delay`, 1069-1083) sets lfoDelayC and, with a delay,
clear_modM; MOD (CC 1) / LFOS (0x15) store and clear on 0; LFODL (0x1A)
stores; MODT (0x16) re-evaluates on a change -- the ROM's handlers, applied
when dispatched or drained, i.e. before the frame's LFO steps, as the
ROM's commands precede the LFO step.  `m4a_engine_set_tempo_bpm`:
tempoI = round(bpm), at once, so it is added on the frame it is
dispatched before.  Callers: the renderer's RENDER_EVT_TEMPO (every SMF
track's tempos and the synthetic 120 BPM default at sample 0) and the
plugin's `plugin_process` per block with the host tempo.  Init: tempoI
150, tempoC 0; `m4a_engine_boot` ends right after frame 2's SoundMain, and
that tick consumes the 150 (tempoC 0, one LFO step on frame 2).

*The renderer* (`cmd/poryaaaa_render.c`).  `make_tempo_event` carries
round(BPM); `parse_midi` prepends the 120 default when the file has no
tempo at tick 0 and sets `gridTempoUs` on first-MTrk tempos (0 on the
default, other tracks', and since slice 3a's review a TEMPO on the loop
end's tick); with the grid every event sits at its grid frame's dispatch
sample (between frame j - 1's SoundMain and frame j's, grid frame 0 at
the origin).

*Today* (grid on): the TEMPO of tick 0 is dispatched before frame 3's
SoundMain and frame 3 adds it, from tempoC 0 left by the boot's tick.  At
120 the engine's LFO steps on frames **4, 5, 6, 7, 9, ...** (4 + n +
floor(n / 4)) where the ROM's (and the walker's) ticks are on **3, 5, 6, 7,
8, 10, ...** (3 + ceil(1.25 n)): one frame late on n = 0 mod 4.  At 240
the steps per frame from frame 3 are 1, 2, 1, 2, 2, 1, 2, 1, ... where the
walker's are 1, 1, 2, 1, 2, 2, 1, 2, ...  A tempo change (240, then 60 at
tick 2): today 1, 2, 0, 1, 0, 0, 1, 0, 1 steps on grid frames 0 .. 8, the
walker 1, 1, 2, 0, 1, 0, 0, 1, 0, 1 on 0 .. 9 (written first with 0, 0, 1 at
the end: 60 is 2.5 frames a tick, the unit test's walker corrected it).

*The mechanism.*  Two engine calls, used by the renderer under
`--gba-tempo-grid` only:
- `m4a_engine_start_song_tempo`: MPlayStart's tempo (tempoD = tempoI =
  150, tempoU 0x100, tempoC 0, no TEMPO pending), called once at the
  origin (output sample O, before frame 3's SoundMain; without
  `--gba-start-delay` sample 0, which the boot also leaves short of frame
  3's SoundMain, so both forms start the engine's grid on frame 3).
- `m4a_engine_set_tick_tempo(tempoI)`: a TEMPO played in the coming tick:
  the value is held and stored after the next frame's add (the `!seq`
  block: `tempoC += tempoI`, then the held value, then the ticks), as
  ply_tempo's store lands after `_081DD86C`'s add.  Several before one
  SoundMain: the last wins, as the ROM's last ply_tempo of the frame.
The renderer passes `gba_tempo_grid_tempo_i(gridTempoUs, clocksPerBeat)`
for a TEMPO with `gridTempoUs != 0` and nothing for the others (the
synthetic default, other tracks' tempos, the loop end's), so the engine
runs the grid's tempoI (mid2agb's byte doubled, -X included, MPlayStart's
150 until the first TEMPO).  Then the engine's accumulator is the
walker's, frame for frame: both start at frame 3 with tempoC 0 and add
150, both take a TEMPO dispatched on grid frame j (the frame of its tick)
from frame j + 1's add, and both run `while (tempoC >= 150)`.  A TEMPO
byte of 0 stops both.  Why it is the ROM's: it is MPlayStart's seed and
ply_tempo's store order, nothing fitted.  Untouched: the plugin and the
flag-off renderer never call the two functions (`set_tempo_bpm` keeps its
dispatch-frame tempo, and with nothing held the `!seq` block runs as
today); a sequencer-driven engine ignores both (they return under
`externalSequencer`; its tempo is the sequencer's own).
*Portamento* (opt-in, off in the harness): it adds `engine->tempoI` after
the LFO block, so on the frame a TEMPO is dispatched before it adds the
new tempoI -- today too, and as MPlayProcessPortamento after the
frame's ticks.  What changes for it: MPlayStart's 150 until the first
TEMPO (the 120 default is gone) and the grid's tempoI.
*Not covered* (within one frame, not the frames): with two ticks in a
frame the ROM interleaves (tick n's commands, its LFO step, tick n + 1's
commands, its step) where the engine has all of the frame's events before
its steps.  It differs only for an LFO-state command in the second tick:
a MOD from 0 (the ROM skips tick n's step), an LFOS or MODT change on a
modulated track, a note-on with an LFO delay.  On the corpus (MIDIs walked
on the grid in their windows; LFODL is never set): vs_wild 7 MOD 0 -> 5
(the first on frame 903, 15.1 s), rg_vs_trainer 2 MOD 0 -> 4 (frame
903); none elsewhere.

*Predictions* (grid on, both latch forms the same; flag off and the
song input byte-identical):

1. **Flag off: 0 field differences** on both aligned runs; the song gate
   unchanged (59 / 0 moved / 57/66 / 0 regressions / every row 0.0).
2. **Rows without MOD byte-identical** to their tempo-grid WAVs: every
   micro row but the five lfo rows (no other micro MIDI sets MOD) and all
   six SEs (none sets MOD in its MIDI) -- 32 WAVs.  The engine's LFO steps
   only on a track with mod and lfoSpeed set, and portamento is off.
3. **lfo_vib, lfo_trem, lfo_pan: -inf, bit-identical to their `--song`
   renders** over the whole file (PCM voice 10; the LFO grid was their
   only difference since slice 3a: no LFO delay, MOD 0 before the tick-96
   note end and before FINE, so ply_fine's unlink meets no LFO).
4. **lfo_delay: identical to its `--song` render up to tick 96** (frame
   123) and not after: tick 96 carries the note's end and FINE, and
   ply_fine clears the track's flags, so the ROM's track takes no LFO step
   from tick 96 on (the `beq _081DD994` after a command) while the MIDI
   path's track 0 keeps mod 24 and steps through the release (voice 10's
   release 165, about 12 frames).  The ply_fine family (as vol), not the
   grid.  Null deep but not -inf: -20 .. -35.
5. **lfo_psg**: changes from frame 3 (its first LFO step), still far from
   passing (the LFO value reaches the PSG one CgbSound late, item 6, and
   the CgbSound entry): moves, sign not predicted.
6. **The songs** (all seven set MOD): change, first difference from the
   tempo-grid render on frame 3 or 4 on any song with MOD at tick 0 on a
   track, small null changes only (their residual is the PSG entry, the
   VOL batching and the track numbering), drift 0.0 stays; none passes.
7. **The latch**: grid-on runs with and without `--midi-quantize`
   byte-identical (the held tempo is not latched and the latched drain
   precedes the LFO block in either form).

**Step 2, the code.**  `plugin/m4a_engine.h` / `.c`: `tempoINextSet` /
`tempoINext` in M4AEngine; `m4a_engine_start_song_tempo` and
`m4a_engine_set_tick_tempo` (both return under `externalSequencer`); in
`m4a_engine_tick`'s `!seq` block the held tempoI is stored after
`tempoC += tempoI`, before the `while`.  `cmd/poryaaaa_render.c`:
`dispatch_event` takes `gridClocksPerBeat` (0 without the flag): with
it a TEMPO with `gridTempoUs != 0` calls `m4a_engine_set_tick_tempo(
gba_tempo_grid_tempo_i(us, 24 or 48))` and any other TEMPO nothing;
without it `m4a_engine_set_tempo_bpm` as before.  The render loop calls
`m4a_engine_start_song_tempo` once before the first event, after
rendering to the origin (the first event is at the origin under the grid,
so the render chunks are the same).  The usage text of `--gba-tempo-grid`
says the LFO steps on the same ticks.  No new flag: the walker's frames
are what `--gba-tempo-grid` promises, and the engine's LFO is part of
"each tick on the frame the game would".  The plugin and the sequencer
code are untouched (`m4a_plugin.c` not in the diff).

**Step 3, the unit test** (`test_engine_lfo_grid`, 21 assertions): an
engine with no notes and track 0 at MOD 1 / LFOS 1 (lfoSpeedC counts the
LFO steps), tempo events dispatched on their tick's grid-frame sample as
the renderer does; with the two calls, the steps per frame equal the
walker's ticks per frame (`gba_tempo_grid_frame`) on grid frames 0 .. 399
for 120, 240, 240 -> 60 at tick 2 and 512 BPM (tempo 0), latched and not;
spelled out: 120 on frames 3, 5, 6, 7, 8, 10, ...; 240 1, 1, 2, 1, 2, 2,
1, 2 on frames 3 .. 10; 240 -> 60 1, 1, 2, 0, 1, 0, 0, 1, 0, 1; tempo 0
one step then none.  Without the calls (`m4a_engine_set_tempo_bpm`, both
latch forms): 120 step n on frame 4 + n + floor(n / 4) (320 steps in 400
frames), 240 1, 2, 1, 2, 2, 1, 2, 1, the change 1, 2, 0, 1, 0, 0, 1, 0, 1
-- today's numbers of step 1.  A sequencer-driven engine: the calls
leave tempoI / tempoC and nothing held, one LFO step per song tick (100
in 100).  Unit **1845/1845** (1824 + 21).

**Step 4, the measurement.**  Song-input gate: `claim_check.sh` against
`2026-10-25_cry-over-song-rows.json` 59 diffed, 0 moved, 57/66, 0
regressions, every row 0.0, 0 field differences on 66 rows; unit
1845/1845, blip 49112/0, songs 561/561.  Flag off: `--no-gba-tempo-grid`
0 field differences on 44 rows against `-aligned.json`, with
`--midi-quantize` against `-aligned-latch.json`, and the 44 WAVs of each
byte-identical to the runs before the change.  Grid on (the default):
`baselines/2026-10-25_midi-path-lfo-grid.json` / `.txt` and
`-lfo-grid-latch.json` / `.txt`, **17/44** (tempo-grid 14/44: + lfo_vib,
lfo_trem, lfo_pan); the latched run 0 field differences against the
unlatched one and its 44 WAVs byte-identical.  `compare_reports.py`
tempo-grid -> lfo-grid: 1 regression over 1 dB, groudon (below).  Both
latch forms are this table:

| test | lag ms | gain dB | null | worst1s | drift | tempo-grid lag / null / worst1s / drift | WAV |
|------|-------:|--------:|-----:|--------:|------:|------------------------------------:|-----|
| sq1 | 0.0 | -0.0 | -37.2 | -32.9 | - | 0.0 / -37.2 / -32.9 / - | identical |
| sq1_env | 0.2 | -2.2 | -3.5 | -1.2 | - | 0.2 / -3.5 / -1.2 / - | identical |
| sq1_sweep | 0.0 | -0.0 | -43.1 | -37.1 | - | 0.0 / -43.1 / -37.1 / - | identical |
| sq1_alt | 0.0 | -0.0 | -35.5 | -30.6 | - | 0.0 / -35.5 / -30.6 / - | identical |
| sq2 | 0.0 | 0.0 | -inf | -inf | - | 0.0 / -inf / -inf / - | identical |
| sq2_alt | 0.0 | -0.0 | -34.5 | -29.7 | - | 0.0 / -34.5 / -29.7 / - | identical |
| wave | 0.2 | -0.3 | -11.8 | -9.4 | - | 0.2 / -11.8 / -9.4 / - | identical |
| wave_alt | 0.1 | -0.4 | -10.6 | -8.2 | - | 0.1 / -10.6 / -8.2 / - | identical |
| noise | 0.0 | -1.5 | -5.1 | -3.1 | - | 0.0 / -5.1 / -3.1 / - | identical |
| noise7 | 0.0 | -1.2 | -6.0 | -2.9 | - | 0.0 / -6.0 / -2.9 / - | identical |
| pcm | 0.0 | 0.0 | -inf | -inf | - | 0.0 / -inf / -inf / - | identical |
| pcm_hi | 0.0 | 0.0 | -inf | -inf | - | 0.0 / -inf / -inf / - | identical |
| pcm_adsr | 0.0 | 0.0 | -inf | -inf | - | 0.0 / -inf / -inf / - | identical |
| pcm_drum | 0.0 | 0.0 | -inf | -inf | - | 0.0 / -inf / -inf / - | identical |
| pcm_cmp | 0.0 | 0.0 | -55.0 | -50.2 | - | 0.0 / -55.0 / -50.2 / - | identical |
| pcm_reverb | 0.0 | 0.0 | -inf | -inf | - | 0.0 / -inf / -inf / - | identical |
| pcm_chord | 0.0 | -0.0 | -55.7 | -51.9 | - | 0.0 / -55.7 / -51.9 / - | identical |
| pcm_bass | 0.0 | 0.0 | -inf | -inf | - | 0.0 / -inf / -inf / - | identical |
| pan | 0.0 | 0.0 | -inf | -inf | - | 0.0 / -inf / -inf / - | identical |
| vol | 0.0 | -0.0 | -47.2 | -41.4 | - | 0.0 / -47.2 / -41.4 / - | identical |
| vel | 0.0 | 0.0 | -inf | -inf | - | 0.0 / -inf / -inf / - | identical |
| bend | 0.0 | 0.0 | -inf | -inf | - | 0.0 / -inf / -inf / - | identical |
| lfo_vib | 0.0 | 0.0 | -inf | -inf | - | -0.1 / -2.0 / -0.7 / - | moved |
| lfo_trem | 0.0 | 0.0 | -inf | -inf | - | 0.0 / -27.4 / -26.2 / - | moved |
| lfo_pan | 0.0 | 0.0 | -inf | -inf | - | 0.0 / -27.5 / -26.4 / - | moved |
| lfo_delay | 0.0 | -0.0 | -20.3 | -7.0 | - | -3.5 / -2.3 / -1.2 / - | moved |
| lfo_psg | 21.6 | -7.2 | 0.4 | 2.3 | - | -18.8 / 0.2 / 1.0 / - | moved |
| tune | 0.0 | 0.0 | -inf | -inf | - | 0.0 / -inf / -inf / - | identical |
| tune_psg | 0.0 | -0.0 | -33.5 | -26.5 | - | 0.0 / -33.5 / -26.5 / - | identical |
| psg_mix | 0.0 | -2.1 | -3.8 | -2.0 | - | 0.0 / -3.8 / -2.0 / - | identical |
| pcm_psg | 0.0 | -0.1 | -17.3 | -14.4 | - | 0.0 / -17.3 / -14.4 / - | identical |
| mus_littleroot | 0.0 | -0.4 | -10.3 | -6.3 | 0.0 | 0.0 / -9.3 / -4.0 / 0.0 | moved |
| mus_weather_groudon | -2.4 | -4.2 | -1.1 | 2.9 | 0.0 | -14.3 / -2.7 / 1.2 / -0.5 | moved |
| mus_vs_wild | 0.0 | -0.4 | -10.1 | -5.0 | 0.0 | 0.0 / -10.0 / -5.0 / 0.0 | moved |
| mus_mt_chimney | 0.0 | -2.2 | -3.6 | -0.2 | 0.0 | 0.0 / -2.6 / 2.0 / 0.0 | moved |
| mus_rg_vs_trainer | 0.0 | -1.5 | -5.1 | -0.4 | 0.0 | 0.0 / -4.9 / -0.7 / 0.0 | moved |
| mus_vs_trainer | 0.0 | -0.2 | -12.7 | -4.7 | 0.0 | 0.0 / -12.4 / -4.7 / 0.0 | moved |
| mus_credits | 0.0 | -0.1 | -14.4 | -7.0 | 0.0 | 0.0 / -14.4 / -7.0 / 0.0 | identical |
| se_exp | 0.2 | -4.6 | -0.8 | 1.6 | - | 0.2 / -0.8 / 1.6 / - | identical |
| se_faint | 0.0 | -1.3 | -5.5 | -2.4 | - | 0.0 / -5.5 / -2.4 / - | identical |
| se_contest_place | 0.0 | -0.4 | -10.8 | -8.6 | - | 0.0 / -10.8 / -8.6 / - | identical |
| se_truck_unload | 0.0 | -8.8 | 1.1 | 1.1 | - | 0.0 / 1.1 / 1.1 / - | identical |
| se_low_health | 7.9 | -3.4 | -1.9 | -0.5 | - | 7.9 / -1.9 / -0.5 / - | identical |
| se_rain_stop | 0.0 | 0.0 | -32.9 | -23.4 | - | 0.0 / -32.9 / -23.4 / - | identical |

**Which predictions held.**

1. *Flag off identical*: held (fields and WAVs; the song gate).
2. *Rows without MOD byte-identical*: held, the 32 WAVs (26 micro rows,
   six SEs) -- and a 33rd, credits: its MIDI sets MOD only after its
   60 s window (none before ROM frame 4000, 67 s), so no track of
   its render is modulated.  My "all seven songs set MOD" was the .s
   count, not the window.
3. *lfo_vib / lfo_trem / lfo_pan -inf, bit-identical to `--song`*:
   **held** (the three WAVs identical to their `--song` renders sample
   for sample; tempo-grid -2.0 / -27.4 / -27.5).
4. *lfo_delay identical to `--song` up to tick 96*: **held**: first
   difference from its `--song` render in output frame 124, the frame
   after tick 96's SoundMain (frame 123), 7808 samples (the release),
   max 5366; null -20.3 / worst -7.0 (tempo-grid -2.3 / -1.2), inside
   the -20 .. -35 predicted, near its shallow end.
5. *lfo_psg moves, still failing*: held (0.2 -> 0.4, worst 1.0 -> 2.3,
   lag -18.8 -> 21.6: uncorrelated before and after); against its
   `--song` render the first difference stays frame 3 (the CgbSound entry)
   and the differing samples go 102316 -> 102743 (no closer); its first
   difference from the tempo-grid render is output frame 4, not 3: the
   frame-3 LFO step reaches the PSG at frame 4's CgbSound (item 6).
6. *The songs*: partly.  Each moved song's first difference from its
   tempo-grid render is 1 .. 5 frames after its first MOD > 0 (the first
   modM change): littleroot 71 (MOD at tick 45, frame 66), groudon 97
   (96), vs_wild 731 (729), rg_vs_trainer 905 (903, the two-tick-frame
   MOD 0 -> 4 of step 1's "not covered"), vs_trainer 1041 (1040); and
   mt_chimney 21, which does not fit that rule (corrected in review): its
   tick-0 MOD 1's modM changes fall on the same frames in both grids
   (frames 6, 10, 13), and the first divergence follows the tick-12 MODs
   of tracks 3, 4, 7 (frame 18; a step on frame 20 where the old grid's is
   on 21, reviewer 2's per-track modM model).  Not "frame 3 or 4": only
   mt_chimney sets MOD at tick 0, at depth 1.  Against their `--song` renders the first difference is
   unchanged on every song (frame 3, 13 or 20: the PSG entry, PSG note
   frames) and the differing samples drop on six (littleroot 905383 ->
   891267, mt_chimney 945808 -> 913310, vs_trainer 484653 -> 398897,
   vs_wild 883499 -> 883497, rg_vs_trainer 957431 -> 957421, groudon
   957183 -> 957079).  Nulls: littleroot -9.3 -> -10.3, mt_chimney -2.6 ->
   -3.6, vs_trainer -12.4 -> -12.7, rg_vs_trainer -4.9 -> -5.1, vs_wild
   -10.0 -> -10.1, groudon -2.7 -> -1.1 (worst 1.2 -> 2.9, lag -14.3 ->
   -2.4, drift -0.5 -> 0.0): groudon is the pure-PSG song that does not
   correlate (the skill: null and drift are noise there).  Against its
   `--song` render the evidence is mixed (corrected in review: I had
   written "closer" from 104 fewer differing samples in 957k): its raw
   null there goes -0.3 -> +1.9 dB, a phase-free spectral-magnitude error
   -19.3 -> -20.7 (reviewer 2).  Its compare_reports "regression" is not
   attributable to a step away from the ROM either way: the LFO now steps
   on the ROM's frames on a song whose PSG writes are bounded by the
   CgbSound entry and the LFO / CgbSound order.  Drift 0.0 on all
   seven.  None passes, as predicted.
7. *The latch*: held, byte-identical.

**Attribution of what is left on the default midi path** (each against
its own `--song` render):

- *ply_fine's unlink* (per-tick command batching, now with the LFO):
  lfo_delay (the release after FINE keeps stepping the LFO on the MIDI
  path; the ROM's track has no flags left), vol (the FINE tick's VOL).
- *The CgbSound entry and the LFO / CgbSound order*: lfo_psg, every PSG
  micro row, psg_mix, pcm_psg's PSG half, the SEs with PSG, the songs'
  PSG tracks.
- *Per-tick command batching*: se_rain_stop, se_contest_place, credits
  from tick 384.
- *Within a two-tick frame*: an LFO-state command in the second tick
  (rg_vs_trainer 2 on frame 903, vs_wild 5 from frame 903): not taken apart;
  the songs' PSG differences start on frame 3, long before.

**What the LFO / CgbSound order slice needs.**  The engine's LFO now
steps on the ROM's frames, but in `m4a_engine_tick` the `!seq` LFO block
and, after it, portamento and PWM run after the CgbSound passes, so a CGB pitch
(`cgb_set_pitch`, MO_PIT) or tremolo volume (MO_VOL) set by frame F's
LFO step is written to the PSG by frame F + 1's CgbSound, where the
ROM's MPlayMain -- tick, LFO step, the VOLCHG / PITCHG pass, and in the
eventide sources MPlayProcessPortamento / MPlayProcessPulseWidthMod --
(both called at MPlayMain's end, ccd2ad2e / 11f0b446) -- runs before
CgbSound in the same SoundMain.  The slice moves that block (the
accumulator, portamento, PWM) before the CgbSound passes (after the gates and the latched
drain), which changes the plugin as well (it is `!seq` too), so it needs
the user's decision whether the plugin takes it or only the renderer;
PCM rows are unaffected (SoundMainRAM reads the frequency after both).
lfo_psg is its test row, but lfo_psg's first difference from its `--song`
render is frame 3, the fixed CgbSound entry (1205 cycles after SoundMain
where the ROM's varies with MPlayMain's work), so the order slice alone
should leave it far from -inf; the measure there is the write log
(`--psg-log` on the MIDI path against the `--song` one: the LFO's NRx3 /
NRx4 writes one CgbSound earlier, the residual offset the entry's).

**The midi baseline** is now the lfo-grid pair
(`2026-10-25_midi-path-lfo-grid.json` / `-lfo-grid-latch.json`): the
runner's default (`--gba-start-delay --gba-tempo-grid`) reproduces it;
the tempo-grid pair is history.

**Code review of slice 3b (2026-10-25).**  Two independent read-only
reviews (reviewer 1: the change against MPlayMain's per-track tick, the
LFO handlers, MPlayStart / ply_tempo, mid2agb's tempo, MPlayProcessPortamento
and the plugin / sequencer / flag-off paths, by hand traces; reviewer 2:
every number of this section recomputed from the WAVs, reports and MIDIs,
the table against both baselines field by field, the unit test assertion
by assertion, the docs).

No divergence on any harness path and no wrong table value.  Confirmed:
the held TEMPO's store after the add (ply_tempo after `_081DD86C`), the
last of several winning, two ticks in a frame, the tempo-0 stop (and a
second TEMPO in the same frame restarting both the walker and the engine,
as the ROM), mid2agb's tempoI with -X and odd BPMs, MPlayStart's 150 with
no first-MTrk TEMPO, the loop end's TEMPO never played, the latch, the
portamento tempoI (now the ROM's on every frame), the plugin / sequencer /
flag-off renderer untouched (new fields zeroed by init's memset, the calls
only in the renderer, both guarded by `externalSequencer`), every step-1
frame list, the table (44 rows x 6 cells), the 17/44, the WAV identities
(33 grid WAVs, all aligned WAVs, latch = unlatched), the 21 assertions.

*Acted on* (gated twice: the song-start claim alone, the call removed;
then the final state with the usage text and the unit-test changes, the
one touching only text, the other only the test binary.  Both runs:
claim_check 59 diffed, 0 moved, 57/66, 0 regressions, every row 0.0;
unit 1845/1845, blip 49112/0, songs 561/561; the four midi runs 0 field
differences on 44 rows against lfo-grid, lfo-grid-latch, aligned,
aligned-latch and every WAV byte-identical to the slice commit's;
poryaaaa and poryaaaa-standalone build.  All harness-neutral):

- *The song-start call is a no-op* (reviewer 1).  The boot's frame-2 tick
  already leaves MPlayStart's state (tempoI 150, tempoC 0; init sets 150 /
  0x100 / 150 / 0 and that tick adds 150 and steps once), and with the
  grid nothing touches the engine's tempo before the origin.  Measured:
  without `m4a_engine_start_song_tempo` all four midi runs are 0 field
  differences and byte-identical.  So the whole change is the held
  TEMPO; step 1's "phase runs from the boot" (and slice 3a's "two
  offsets") was one offset, the tempo counting on its dispatch frame.
  Kept as MPlayStart's seed made explicit, the comment saying it only
  states the boot's state.
- *Without `--gba-start-delay`* (both).  The ticks of grid frames 0 .. 3
  reach frame 3 and later ones land 3 frames early (slice 3a) while the
  engine's grid still starts on frame 3, so the LFO runs 3 frames behind
  the commands and a TEMPO counts 3 accumulator frames early; "frame for
  frame" holds with the start delay only.  The usage text now says so (the
  harness always passes the delay); step 1's "both forms start the
  engine's grid on frame 3" is true but was not the whole statement.
- *The unit test* (both).  Each counted frame is now checked to end right
  after ROM frame j + 3's SoundMain (`nextSoundMain`), so a step pattern
  on the wrong ROM frame fails; the "flag off" messages say what runs
  (`set_tempo_bpm` on the same grid samples, slice 3a's default, not the
  exact-tempo flag-off renderer); the sequencer part compares with a run
  without the calls (lfoSpeedC, the CGB channels) instead of a fixed 100.
  Same 21 assertions, unit 1845/1845.
- *Docs* (reviewer 2): vs_wild's second-tick MOD 0 -> 5 are 5 in its
  20 s window, not 7 (two on frame 1197, whose SoundMain is at 20.04 s;
  my helper ignored the frame-1 start); mt_chimney's first change is not
  "1 .. 5 frames after its first MOD" (above); groudon's "closer" was not
  supported (above); lfo_psg's first change is output frame 4; the
  order paragraph's block is the `!seq` LFO block plus portamento and PWM
  after it; the step-1 citation of `note_on_restart_lfo_delay` is the
  pre-change 1069-1083 (1092 at the slice commit); "MOD 0 before the
  tick-96 note end" is MOD 0 at tick 96, in the tick after the gate walk;
  "LFODL is never set" is the songs' (lfo_delay sets 30).  The step-1
  text and predictions are kept as written; this paragraph says where the
  review changed them.

*Recorded, not acted on:*

- *Within a two-tick frame* (both): the ROM's tick n LFO step runs before
  tick n + 1's commands, the engine's steps after all of the frame's
  events; output differs for an LFOS change, a MOD / LFOS from 0, a
  note-on with an LFO delay in the second tick (MOD depth changes, a MOD /
  LFOS to 0, MODT, LFODL do not).  Corpus: rg_vs_trainer 2, vs_wild 5.
  Pre-existing; the renderer driving the engine's ticks from the walker
  would fix it.
- *LFO / portamento / PWM after CgbSound* (reviewer 1): the order slice.
  Also the engine's portamento keeps running after a song's end, where
  MPlayMain's `_081DD9A4` path returns before it (opt-in, not in the
  harness).
- *Same-tick first-MTrk tempos* (reviewer 1): `cmp_tempo_events` has no
  tie-break where mid2agb's sort is stable; walker and engine see the same
  order.  Recorded in slice 3a.
- *tempoD* (reviewer 1): `set_tick_tempo` does not store it, as
  `set_tempo_bpm` does not; nothing reads it.
- *A 0 us tempo meta event* reads as "not a TEMPO" (mid2agb's would be
  `round(inf)`).
- *Test coverage* (both): the renderer's dispatch (`gridTempoUs == 0`
  skip, 48 clocks, the loop end) is static code checked by the harness
  rows (lfo_vib / trem / pan bit-identical to `--song`), not by the unit
  test; one rate (48 kHz) with the start-delay origin; the latched
  variants latch only the two setup CCs (the tempo bypasses the latch by
  design).
- *The skill* (reviewer 2): `.claude/skills/accuracy-harness/SKILL.md`
  still calls the tempo-grid pair the midi baseline, describes the
  engine's LFO as one frame late on n = 0 mod 4 and "left to slice 3b",
  and gives six songs' drift 0.0.  Not edited: this slice changes no flag
  or default, and its brief limits skill / README edits to that case; left
  to the user.  README.md has nothing this slice made wrong.

### Milestone 5 slice 4: the LFO / CgbSound order (2026-10-25)

Scope: in `m4a_engine_tick` without a sequencer, the frame's LFO steps,
portamento and PWM run before the CgbSound passes, as MPlayMain runs
before CgbSound in SoundMain -- for the plugin too (**decision, the
user**: the ROM's order whatever the tempo source; the plugin's PSG
vibrato / tremolo / PWM / glide reach the chip one frame earlier, PCM is
unchanged, no flag).  Out of scope: the CgbSound entry (the fixed
`cgbEntryCycles` on the MIDI path), per-tick command batching and
ply_fine, the two-tick-frame interleave, the sequencer path (which runs
its own MPlayMain in the frame hook before CgbSound already; byte-identical).
Before any change the baselines reproduced (the slice 3b review's final
gate on the same code): claim_check 59 diffed, 0 moved, 57/66, 0
regressions, every row 0.0, unit 1845/1845, blip 49112/0, songs 561/561;
the four midi runs 0 field differences on 44 rows against
`2026-10-25_midi-path-lfo-grid.json`, `-lfo-grid-latch.json`,
`-aligned.json`, `-aligned-latch.json`.

**Step 1, the reading.**  *SoundMain* (`m4a_1.s` 30-82): `MPlayMainHead`
(each player's MPlayMain: the ticks with their LFO steps, the VOLCHG /
PITCHG pass that sets the CGB `frequency` / `MO_PIT` and `MO_VOL`, and in
the eventide sources MPlayProcessPortamento then MPlayProcessPulseWidthMod
at its end, ccd2ad2e / 11f0b446), then `CgbSound`, then SoundMainRAM.
So an LFO step of frame F reaches NRx3 / NRx4 (vibrato) or NRx2 and the
retrigger (tremolo / autopan, MO_VOL) at frame F's CgbSound.  *The
engine* (`sound_main`: `m4a_engine_tick`, then `m4a_sound_main_ram`):
`m4a_engine_tick` runs the frame hook (a sequencer's whole MPlayMain),
c15, the `!seq` PCM / CGB gate walks, the latched drain, the CgbSound
passes, then `if (!seq)` the accumulator with `m4a_lfo_tick`, then
`m4a_portamento_tick` and `m4a_pwm_tick` (both outside `!seq`).
`m4a_lfo_tick` writes CGB state only (`cgb_set_pitch`: frequency, modify
0x02; `cgb_chn_vol_set` and modify 0x01 for modT != 0), the PSG writes are
CgbSound's; portamento's `apply_portamento_pitch` and PWM's dutyCycle /
modify 0x04 likewise.  So on the `!seq` path the frame-F LFO value is
written at frame F + 1's CgbSound: one frame late (slice 1 item 6).  PCM
channels read `frequency` / volumes in SoundMainRAM, after the whole tick
either way.  The sequencer's LFO is inside its hook (before CgbSound);
with a sequencer the engine's portamento and PWM have nothing to do (the
sequencer never sets `portamentoGliding` / `pwmPattern`), and they stay
where they are on that path.

*The change.*  The `!seq` block (accumulator, LFO, portamento, PWM) moves
to after the latched drain and before the CgbSound passes; with a
sequencer, portamento and PWM stay after CgbSound (byte-identical by
construction).  Within the frame the order is then the ROM's: gate walk,
commands (dispatched before the SoundMain or drained), LFO steps, the
pass (folded into the steps), portamento, PWM, CgbSound, SoundMainRAM.
For the measurement, `--psg-log` also works with `--midi` (the log opened
before the boot, as the song path does; nothing else changes).

*Predictions* (grid on and off alike, both latch forms the same):

1. **Song input byte-identical**: claim_check 59 / 0 moved / 57/66 / 0
   regressions / every row 0.0.
2. **Every row without MOD byte-identical** on all four midi runs (33 on
   the grid pair as in slice 3b; the same rows on the aligned pair), and
   **the PCM LFO rows too** (lfo_vib, lfo_trem, lfo_pan, lfo_delay: PCM
   reads after the tick); credits (no MOD in its window).
3. **lfo_psg**: its MIDI-path write log carries the `--song` log's
   registers, values and order frame for frame, offset only by the
   CgbSound entry (slice 2a's -2667 on frame 3, about -464 on tick
   frames); today the LFO's NRx3 / NRx4 / NRx2 writes come one frame late.
   Null from +0.4 into the square rows' range or near it (sq1 -37.2 with
   the same entry offset), but the tremolo half (MODT 1 from tick 72:
   NRx2 rewrites with a retrigger on each step, phase reset at the entry
   offset) keeps it shallower: -15 .. -37, not passing.
4. **The songs**: the six with MOD in their windows change where a
   modulated track is PSG; small changes, none passes, drift 0.0; their
   first difference from `--song` stays (frame 3 / 13 / 20, the entry).
5. **The aligned pair** (flag off) moves on the same rows (lfo_psg and
   the songs): the plugin-wide order is not gated by the grid.  Both
   pairs saved anew.

**Step 2, the code.**  `plugin/m4a_engine.c`: `mplay_main_tail` (the
tempo add with the held TEMPO, the LFO steps, `m4a_portamento_tick`,
`m4a_pwm_tick`), called in `m4a_engine_tick` after the latched drain and
before the CgbSound passes when there is no sequencer; with one,
portamento and PWM stay after CgbSound.  `cmd/poryaaaa_render.c`:
`--psg-log` on the `--midi` path (opened before the boot, closed at the
end; the song path's code unchanged).  No flag; `m4a_plugin.c` unchanged
(it gets the order through the engine).

**Step 3, the unit test** (`test_lfo_cgbsound_order`, 6 assertions): a
sq1 note with LFOS 40 / MOD 24 through the plugin's calls
(`m4a_engine_set_tempo_bpm`, no grid), latched and not: vibrato (MODT 0)
-- after each of 90 frames NR13 / NR14 hold the channel's frequency (72
modM changes); tremolo (MODT 1) -- every frame whose LFO step changes
modM has an NR12 write in that frame; a sequencer-driven vibrato song
the same (unchanged order).  On the old engine (the test built against
the previous `m4a_engine.c`) the four MIDI-path assertions fail (72 / 72
/ 18 / 18 frames late), the sequencer one passes.  Unit **1851/1851**
(1845 + 6).

**Step 4, the measurement.**  Song-input gate: claim_check 59 diffed, 0
moved, 57/66, 0 regressions, every row 0.0, 0 field differences on 66
rows; blip 49112/0, songs 561/561; poryaaaa and poryaaaa-standalone
build.  Midi: `baselines/2026-10-25_midi-path-lfo-order.json` / `.txt`,
`-lfo-order-latch`, `-lfo-order-aligned`, `-lfo-order-aligned-latch`;
grid 17/44 (lfo-grid 17/44), aligned 12/44 (12/44); latched =
unlatched (fields and WAVs) on both pairs; `compare_reports.py` 0
regressions over 1 dB on both (lfo-grid -> lfo-order, aligned ->
lfo-order-aligned).  The rows that moved (every other row byte-identical
to its slice 3b WAV, both pairs):

| test | grid null / worst1s (lfo-grid) | aligned null / worst1s (aligned) | PSG writes vs `--song` (grid form) |
|------|-------------------------------:|---------------------------------:|------------------------------------|
| lfo_psg | -17.8 / -16.1 (+0.4 / +2.3), lag 0.0 (21.6) | -2.0 / 0.2 (0.2 / 1.0) | 265 of 265 matched, all on their frame (before: 182 matched, 92 one frame late) |
| mus_littleroot | -14.7 / -10.4 (-10.3 / -6.3) | 2.2 / 4.0 (2.1 / 4.0) | 1181 of 1181, all on their frame (before: 984, 96 one late, more 3 .. 50 off) |
| mus_weather_groudon | -2.8 / 0.3 (-1.1 / 2.9), lag -14.3, drift -0.6 | -1.5 / 3.1 (-2.4 / 2.1) | 2825 of 2825, all on their frame (before: 2692, 396 one late) |
| mus_mt_chimney | -3.2 / -0.2 (-3.6 / -0.2) | 1.7 / 3.7 (1.7 / 3.7) | 4235 of 4235, all on their frame (before: 2929, 251 one late, more up to 233 off) |
| mus_rg_vs_trainer | -5.0 / -0.2 (-5.1 / -0.4) | 2.3 / 3.9 (2.3 / 3.9) | 4228 of 4274; on their frame up to frame 905, then 30 one early and 8 six early (before: from 1031) |

("PSG writes vs `--song`": `--psg-log` of the grid-form MIDI render and
of the `--song` render, 20 s for the songs, 3 s for lfo_psg, matched as
(register, value) sequences; the cycle offsets of the matched writes are
the CgbSound entry's: lfo_psg -830 / -960 on its LFO frames, -3673 on
frame 3, sq1 unchanged at -472 .. -2667.)

**Which predictions held.**

1. *Song input byte-identical*: held.
2. *Rows without MOD and the PCM LFO rows byte-identical*: held (all four
   PCM lfo rows, credits, every SE and PSG row without MOD, both pairs),
   and also vs_wild and vs_trainer (byte-identical, so no PSG track of
   theirs changes modM in the window).
3. *lfo_psg's writes the `--song` log's, offset by the entry only*:
   **held exactly** (265 / 265, every write on its frame); null -17.8 /
   worst -16.1, inside -15 .. -37, not passing.  Its first difference
   from its `--song` render moved from frame 3 to frame 5, 78766 samples
   (102743 before).
4. *The songs*: held in kind -- four change (the ones with a modulated
   PSG track: littleroot, groudon, mt_chimney, rg_vs_trainer), none
   passes, first differences from `--song` unchanged (frame 3 / 20).
   On littleroot, groudon and mt_chimney the MIDI path's PSG writes are
   now the `--song` render's register for register and frame for frame:
   what is left on their PSG side is the CgbSound entry alone.
   mt_chimney's null reads 0.4 dB shallower (-3.6 -> -3.2) with every
   write on its frame and its differing samples against `--song` down
   (913310 -> 908585): the entry's cycle offsets, not the order.
   rg_vs_trainer departs on frame 905, the two-tick-frame MOD 0 -> 4 of
   frame 903 (slice 3b, recorded).  Groudon's drift -0.6 and lag -14.3
   are its usual uncorrelated readings.
5. *The aligned pair moves on the same rows*: held (lfo_psg -2.0, the
   songs' nulls within 0.9 dB; the aligned LFO grid is not the ROM's).

**What is left on the PSG side of the midi path** (against `--song`):
the CgbSound entry (`cgbEntryCycles` fixed at the one-track no-tick
value, where the ROM's moves with MPlayMain's work: -464 .. -2667 cycles
on the micro rows, -830 / -960 on lfo_psg's LFO frames, thousands on the
songs: mt_chimney's commonest -8942 .. -13055), now the only difference in the PSG writes of every PSG micro
row and of littleroot, groudon and mt_chimney in their windows; the
two-tick-frame interleave on rg_vs_trainer (from frame 905) and vs_wild.
The entry is the next lever (the order makes its measurement clean: the
write logs differ in cycles only).

### Next: milestone 4, cries and SE (handoff, 2026-10-24)

Timing model step 3 is done (section above): 38/38, the songs at the PCM
scheduler-lateness floor and groudon bit-exact.  The song work did not
stall, so milestone 4 follows: cries and sound effects in the test ROM and
corpus.  The ROM and `mgba_render` already accept `--type cry --id N` and
SE song ids (`test/accuracy/rom/main.c`, `ACC_TYPE_CRY`); `songs.json`
entries are vanilla songs only; the runner (`tools/accuracy.py`) needs a
test entry type for a cry (`StartCry` with pan / volume / pitch) and for
an SE (an `m4aSongNumStart` of an SE id on the SE players, whose
MPlayMain chain position and track counts differ from the BGM player's:
the frame skeleton constants `M4A_CYC_MP_BGM_ENTRY` 566 and the 10-track
loops are the BGM player's; the probe's MPlayMain entries at +291 / +346 /
... give the others).  Before any change the harness must reproduce
`baselines/2026-10-24_timing-3.json` (38/38; 0 regressions, 0
improvements) and `poryaaaa_unit_tests` 1032/1032, `poryaaaa_blip_tests`,
`poryaaaa_song_tests build/accuracy/rom` must pass.

### The songs -- timing model step 3 (handoff as given, 2026-10-24; done above)

Start from the tip of `accuracy-harness` (6757b80: the mixer review fixes,
the refreshed mixer baseline and the two skills).  Before any change the
harness must reproduce `baselines/2026-10-24_mixer.json` (31/38;
`compare_reports.py`: 0 regressions, 0 improvements) and
`build/poryaaaa_unit_tests` 1018/1018, `poryaaaa_blip_tests`,
`poryaaaa_song_tests build/accuracy/rom` must pass.

With every single-voice micro-test bit-exact and the remaining micro
residuals attributed to mgba's scheduler lateness, the seven songs are the
accuracy gap (mus_weather_groudon -5.0, mt_chimney -7.4, rg_vs_trainer
-8.3, credits -18.6, vs_wild -19.7, vs_trainer -26.9, littleroot -27.4).
The timing-model section names what bounds them: the MPlayMain costs the
probe did not resolve (the DirectSound note cost beyond the channel scan,
806-1423 measured; the per-track tick cost's dependence on track state,
61-84 per track; the VOLCHG/PITCHG pass's per-track share, 376 measured on
one track with one channel; ply_fine's and ply_note's ClearChain; the
pseudo-echo idle cost; TrackStop's write placement), which put the songs'
PSG writes +-400 cycles off on PCM-note frames, and wave/noise phase runs
on the CPU clock.

Scope: extend `test/accuracy/ref/mgba_probe.c` with breakpoints inside
MPlayMain's track loop (per-track entry/exit of the tick work), ply_note's
channel search and the VOLCHG/PITCHG pass, so each unresolved cost is a
measured number (record every number in the probe section before using
it; never fit a constant to the harness); charge them in
`m4a_sequencer.c` / `M4A_CYC_*`; verify with `--psg-log` + `probe_diff.py`
on groudon and littleroot (residual target: the micro-tests' 0-36 cycles),
then the harness.  Any constant that moves a bit-exact test is wrong.
Targets: the songs; nothing in the mixer baseline may regress.  Milestone
4 (cries and SE in the ROM and corpus) follows once the songs null, or
earlier if the song work stalls on something the corpus cannot show.

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
