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

### Next: the songs -- timing model step 3 (handoff, 2026-10-24)

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
