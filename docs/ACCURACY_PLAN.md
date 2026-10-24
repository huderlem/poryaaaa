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
5. **MIDI quantization**: latch MIDI events to the next frame tick by
   default, with a live-play override.

## Corpus

Vanilla Emerald, five to eight songs chosen to isolate features (pure PSG,
DirectSound-heavy with reverb, compressed samples, noise channel, etc.),
plus one or two of the user's own tracks as a practical check. Cries and SE
added in milestone 4.

## Explicitly deferred

- ipatix's HQ mixer (later effort).
- GUI song loader, cry and SE browsing in the plugin.
- Hardware comparisons.
