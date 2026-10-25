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
388 + species: the map has `gCryTable` 0x0802afac and `gCryTable_Reverse`
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
`size / 64 + 1` blocks of 0x21 bytes.

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
