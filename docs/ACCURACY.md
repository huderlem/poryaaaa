# poryaaaa accuracy: how it works

poryaaaa reproduces the GBA's m4a sound engine (pokeemerald's `m4a.c` /
`m4a_1.s`) closely enough that its output nulls against mgba 0.10.5
playing the same material. This document describes how that is achieved
and checked: the ground truth, the test harness, the engine's structure
(each part a port of a named ROM routine), the MIDI path the plugin uses,
the measured timing model, and what is known not to match.

The engineering log this replaces (`docs/ACCURACY_PLAN.md`, with every
measurement, prediction and review in order) is in git history up to
commit `8654e9c`. Procedures for running the harness and attributing a
residual are in the `accuracy-harness` skill
(`.claude/skills/accuracy-harness/SKILL.md`); the code-review procedure is
in `review-against-source`.  The second engine configuration, HQ-mixer
mode (ipatix's High-Quality Audio Mixer as the pret wiki installs it,
`engine->hqMixer`, off by default), has its own document with the same
structure: `docs/HQ-MIXER-ACCURACY.md`.  Everything here applies to both
modes unless that document says otherwise; only the mixer differs.

Contents:

1. Ground truth and the metric
2. The harness
3. mgba's audio path
4. The engine
5. The MIDI path
6. Known residuals and limitations
7. Working rules
- Appendix A: measured MPlayMain and CgbSound costs
- Appendix B: test-ROM and probe reference

---

## 1. Ground truth and the metric

- **mgba 0.10.5 is the reference**, not real hardware.  The reference
  renderer captures mgba's raw DAC stream and resamples it with the
  engine's own resampler (`plugin/m4a_resampler.c`), so both sides go through
  one bit-exact resampler and the comparison measures only the GBA-side
  model.  Its version string is `mGBA 0.10.5+m4a_resampler`;
  `mgba_render --native-resampler` gives mgba's own sinc output for
  listening.
- **The test ROM** (`test/accuracy/rom/`) is built with agbcc from the
  pokeemerald tree's `m4a.c`, `m4a_1.s`, `m4a_tables.c` and the full
  vanilla sound data, with the game's sound-mode init and Emerald's
  interrupt layout (`m4aSoundVSync` from the VCOUNT-150 interrupt,
  `m4aSoundMain` from VBlank).  One build serves every test; a parameter
  block in the ROM (`struct AccParams`, appendix B) selects what it plays.
- **The metric** is `20·log10(RMS(ref − test) / RMS(ref))` after time
  alignment, with no gain correction, over the whole file (`null`) and the
  worst one-second window (`worst1s`).  A test passes at −40 dB on both.
  `gm null` / `gm wrst` repeat it after one least-squares gain (a much
  better `gm` means a level offset, not a waveform error).  `−inf` means
  bit-identical.  `lag` is the alignment applied (0.0 ms everywhere since
  the output stage is cycle-based; a nonzero lag means the renders do not
  correlate); `drift` is the lag slope over 4 s segments (any nonzero
  value is a tempo mismatch).
- **Output scale is mgba's absolute scale.**  There is no gain
  normalization anywhere, and a level offset must never be fixed with a
  gain knob.

## 2. The harness

### Components

| piece | what it is |
|---|---|
| `test/accuracy/rom/` | the test ROM (`main.c`, `crt0.s`, `rom.ld`, `sound_data.s`, `accuracy_micro.inc`, `micro/*.mid` + `micro.cfg`) |
| `test/accuracy/ref/mgba_render.c` | headless mgba render of the ROM at 48 kHz from frame 0, via `mAVStream.postAudioFrame` (one int16 pair per 65536 Hz DAC sample) into `m4a_resampler` |
| `test/accuracy/ref/mgba_probe.c` | libmgba debugger probe: breakpoints on ROM labels, IO-write log, scheduler lateness (section 2, "Probe") |
| `build/poryaaaa_render` | the engine's CLI renderer (`cmd/poryaaaa_render.c`) |
| `test/accuracy/tools/accuracy.py` | renders both sides, aligns, measures, writes `build/accuracy/report.json` and a table |
| `test/accuracy/tools/metrics.py` | alignment (`find_lag`) and the metric (`compare_files`) |
| `test/accuracy/tools/compare_reports.py` | flags null regressions over 1 dB between two reports (not worst1s) |
| `test/accuracy/tools/claim_check.sh` | one-shot gate: build, the three test binaries, the PSG write-cycle diff of every probed test, the harness, `compare_reports.py` |
| `test/accuracy/songs.json` | the corpus beyond the micro-tests |
| `test/accuracy/baselines/` | saved reports (`<date>_<name>.json` / `.txt`) |
| `poryaaaa_unit_tests` | `test/test_engine.c` |
| `poryaaaa_resampler_tests` | `m4a_resampler` against mgba's resampler, bit-exact |
| `poryaaaa_song_tests build/accuracy/rom` | the `.s` parser against the ROM: every song (micro, vanilla music, SE, phonemes) relocated to its map address and compared byte for byte |

Build: `cmake -B build -DACCURACY_POKEEMERALD_DIR=~/pokeemerald
-DACCURACY_MGBA_DIR=~/mgba && cmake --build build --target accuracy`
(optional target, skipped without both paths).  The ROM build writes
`build/accuracy/rom/accuracy_test.gba`, its `.elf` and `.map`, and the
compiled songs `build/accuracy/rom/{micro,songs}/<name>.s`.

### References

References are rendered locally and cached in
`build/accuracy/refs/<rom-hash>-<mgba version>/`, so any ROM change keys a
new directory.  Only the micro-test references are committed
(`test/accuracy/refs/micro/`, FLAC, needs python `soundfile`), so the
micro set runs without mgba (`--committed-refs`).  `--update-refs` is for a
ROM or reference-definition change only, never to absorb an engine change.
Frame count of a reference: `ceil(seconds · cpu / frame) + 2`
(122 / 182 / 241 frames for 2 / 3 / 4 s); comparison truncates to the
shorter file.

### Inputs

- **`--input song`** (default): poryaaaa plays the compiled `.s` the ROM
  was built from (`poryaaaa_render --song`), so both sides consume
  byte-identical song data.  Volume, reverb and voicegroup come from the
  `.s` header.
- **`--input midi`**: poryaaaa plays the MIDI (`--midi`) with the mid2agb
  options from `midi.cfg` mapped to flags (`-V` → `--song-volume`, `-R` →
  `--reverb`, `-G` → the voicegroup, `-X` → `--extended-clocks`).  The
  runner always adds `--gba-tempo-grid` (section 5; `--no-gba-tempo-grid`
  leaves it out) and states the start origin either way (`--gba-start-delay`,
  the grid's default, or `--no-gba-start-delay` with the runner's
  `--no-gba-start-delay`) and, with `--midi-quantize`, `--quantize` (the
  frame latch).  Cries and cry-over
  rows have no MIDI and are skipped.  **Midi-input reports are their own
  baseline family** (`baselines/<date>_midi-path-*`; the report's
  `"input"`, `"midi_quantize"`, `"gba_start_delay"`, `"gba_tempo_grid"`
  fields say which form) and are never compared with song-input reports.
- **`--mixer hq`**: the HQ-mixer test ROM and `poryaaaa_render --hq-mixer`
  (`docs/HQ-MIXER-ACCURACY.md`, section 4): a third axis of the baseline
  family (`baselines/<date>_hq-*` for song input, `_hq-midi-path-*` for
  MIDI input; the report's `"mixer"` field).  `compare_reports.py` refuses
  to compare across `"input"` or `"mixer"`.
- **`--pcm-rate <hz>`**: a test ROM built at another DirectSound mix rate
  (`make RATE=<hz>`, `build/accuracy/rom[-hq]-<hz>`; `docs/HQ-MIXER-ACCURACY.md`,
  section 4) and `poryaaaa_render --pcm-mix-rate <hz>`: a fourth axis
  (`baselines/<date>_[hq-]<hz>-*`; the report's `"pcm_rate"` field, which
  `compare_reports.py` also refuses to cross).  Measured at 21024 Hz on
  both ROMs (step 11 there); the rate's micro references are not
  committed.

### Corpus

- **Micro-tests** (37, `test/accuracy/rom/micro/`, generated by
  `tools/gen_micro_midis.py`, all at 120 BPM): `sq1`, `sq1_env`,
  `sq1_sweep`, `sq1_alt`, `sq2`, `sq2_alt`, `wave`, `wave_alt`, `noise`,
  `noise7`, `pcm`, `pcm_hi`, `pcm_adsr`, `pcm_drum`, `pcm_cmp` (the only
  compressed-sample music case: vanilla music uses none), `pcm_reverb`,
  `pcm_chord`, `pcm_bass`, `pan`, `vol`, `vel`, `bend`, `lfo_vib`,
  `lfo_trem`, `lfo_pan`, `lfo_delay`, `lfo_psg`, `tune`, `tune_psg`,
  `psg_mix`, `pcm_psg`, `pcm_ratio_1x` (the flute at keys 85 / 83 / 97:
  resampling ratios just above 1.0, just below, just above 2.0),
  `pcm_short_loop` (keys 100 / 108 / 112: the flute's 562-sample loop
  shorter than a frame's advance, two wraps a frame, and a first frame
  of more than 0x400 source bytes).  Both were added for the HQ mixer's
  fast-path blocks (`docs/HQ-MIXER-ACCURACY.md`, step 5) and play on the
  vanilla ROM too; `pcm_fix_loop` (voice 15, the flute as a
  `no_resample` voice: a looped FIX sample, for the HQ mixer's FIX path,
  step 6); `pcm_reverb_max` (the `pcm_reverb` pattern at `-R127`, step
  7); `pcm_rev` (voice 16, the pizzicato as a `voice_directsound_reverse`:
  an uncompressed sample played backwards, which no vanilla music does)
  and `pcm_cmp_hi` (the Bulbasaur cry at keys 96 / 72 / 94: the HQ
  mixer's stack budget freezes the two high notes silent; the vanilla ROM
  plays them), both step 8; `synth_pulse`, `synth_pulse_lfo`, `synth_saw`, `synth_tri`
  (voices 17–20: Golden Sun synth descriptors from
  `accuracy_micro_synth.inc`, for the HQ mixer's synth paths, step 9) —
  **HQ-only rows**: the vanilla ROM plays a zero-size sample as a click,
  so `accuracy.py` runs `synth_*` only with `--mixer hq`.  Ids are line
  numbers in `micro.cfg`.
- **Songs** (`songs.json`, `{"name", "seconds"}`): mus_littleroot,
  mus_weather_groudon (pure PSG with pseudo-echo), mus_vs_wild
  (DirectSound-heavy with reverb), mus_mt_chimney (noise), mus_rg_vs_trainer
  (FRLG drumset), mus_vs_trainer (the one vanilla MEMACC), mus_credits
  (60 s, many tempo changes).
- **Sound effects** (`"type": "se"`; the player comes from
  `sound/song_table.inc` and is passed as `--player se1|se2|se3`):
  se_exp, se_faint, se_contest_place, se_truck_unload, se_low_health,
  se_rain_stop.
- **Cries** (`"type": "cry"`, `"id"` = gCryTable index; optional `"pitch"`,
  `"pan"`, `"volume"`, `"length"`, `"release"`, `"chorus"` = the
  SetPokemonCry* arguments, taken as whole modes from `PlayCryInternal`):
  defaults (pidgey, bulbasaur, wailord, jynx), variants, the length /
  release modes, reversed cries (index 388 + i), chorus modes, the
  two-chain release; a second cry (`"id2"` / `"start_frame2"`).
- **Cry over a song** (`"type": "se"|"song"` with `"id2"`,
  `"start_frame2"` and `"song"`): `cry_over_se` (se_exp + Bulbasaur from
  VBlank 30), `cry_over_bgm` (mus_littleroot + Bulbasaur from VBlank 60, a
  steal).

Adding to the corpus: the `accuracy-harness` skill, "Add a test".

### Probe and debug logs

- **`mgba_probe`**: `mgba_probe <rom> <out.txt> <frames> <type> <id>
  <syms.nm> [player-symbol] [cry pitch pan volume length release chorus]
  [id2 startFrame2] [id3 startFrame3]`.  Type 0 = song / SE (id from
  `songs.h`), 1 = cry, 2 = micro-test.  The symbol file is the ELF's `nm`
  listing (so local labels inside MPlayMain / ply_note are known) plus the
  compiled CgbSound's internal addresses appended by hand (appendix B).
  Outputs: `<out>.txt` (breakpoints `B` with registers and deciding memory
  bytes, IO writes `W`, lateness `L`), `<out>.frames` (one line per engine
  frame: the driver entries, SoundMain's and CgbSound's returns and the
  mixer's cost, "SoundMain cost model"; the selected player's state), `<out>.events`
  (mgba scheduler lateness, timer-0 slot crossings).  Breakpoints do not
  change the emulation.  Channel deaths are SoundMainRAM's `_081DCFB0`
  store, broken at its IWRAM copy (SoundMainRAM runs from
  `SoundMainRAM_Buffer`).
- **`poryaaaa_render --psg-log <file>`**: every PSG register write with its
  cycle (both paths).  `tools/probe_diff.py` diffs it against the probe.
- **`--seq-log <file>`**: the sequencer's per-frame CgbSound entry and each
  command (`c`) and track end (`t`) with its cycle; `entry_diff.py`,
  `cmd_diff.py`, `track_diff.py` diff it against the probe.  Track numbers
  are engine indices.  Its frame label is one less than the ROM frame.
- **`--pcm-log <file>`**: after each SoundMain, every DirectSound channel
  (status, key, MIDI key, track, velocity, envelope, volumes, frequency);
  frame label = ROM frame.
- **Probe analysis**: `probe_costs.py` (cost tables), `probe_mplay.py`
  (MPlayMain segments keyed on the deciding state, `--player`),
  `probe_cgb.py` (CgbSound passes and `--segments`).

**Frame numbering.**  Engine frame k ≥ 1 starts at cycle
124773 + (k − 1) · 280896; VBlank line 160 starts 197120 into it.  Probe
cycles are relative to line 160.  mgba's own `frameCounter` (`fN` in the
raw log) is one higher.

**Comparing two renders.**  Compare logs frame by frame, never as one
sequence: for PSG, each frame's ordered (register, value) list, then each
write's cycle in the equal frames (a whole-log sequence match pairs
repetitive vibrato writes frames apart); for PCM, channel by channel,
ignoring the track number (the paths number tracks differently).  Attribute
a MIDI-path difference against the same row's `--song` render.

## 3. mgba's audio path

What the engine models, measured with the probe:

- **DAC at 65536 Hz.**  Emerald's `SOUND_MODE_DA_BIT_8` writes
  SOUNDBIAS_H = 0x40, so mgba samples every 256 cycles: four samples per
  1024-cycle sample event, resampler frames of 2048 cycles.
- **Output arithmetic**: squares / wave / noise are unipolar 0..15 ×
  8 × (1 + NR50 volume), then >> 2 (SOUNDCNT_H); PCM is int8 << 2; the sum
  + bias 0x200 is clamped to 0..0x3FF, then × 48 to int16.
- **DMA FIFO samples are held** between timer-0 overflows; the PSG runs on
  the same clock.  GBA-style noise is point-sampled.  The rest of the PSG
  (duty stepping, wave RAM, 64 Hz envelope, sweep) is `src/gb/audio.c`.
- **Register writes**: NR11–14 / 21–24 / 31–34 / 41–44 are handled byte by
  byte; NR10 / NR30 / NR50 / NR51 / NR52 go through a merged 16-bit path
  that ignores writes while NR52 is off.  A CPU store at cycle T lands
  after the hardware events due at T (mgba runs events due by an
  instruction's start before executing it).
- **Timer 0 reload** is the game's integer `TM0CNT_L = −(280896 / spv)`
  (1254 cycles at 224 samples per VBlank), not an exact rational.
- **The FIFO refill DMA** (channels 1 / 2, four words each at one
  overflow, every 16 samples = 20064 cycles) stalls the CPU 18 cycles; in
  the frame the writes sit at cycle 15091..15105.
- **VSync restart**: m4aSoundVSync's DMA restart is a disable / enable
  pair; the START_NOW write happens with the DMA enabled, so only the
  source address reloads and the FIFO keeps its contents.
- **Open bus**: a load outside cartridge space returns `prefetch[1]`
  (`GBALoadBad`), the instruction word two ahead, at byte lane
  `address & 3`.
- **Scheduler lateness**: mgba processes timer events at the late time of
  the instruction that crossed them and does not correct
  `GBAAudioSampleFIFO`'s `until`, so while the CPU runs a long instruction
  stream a FIFO sample can land one 65536 Hz slot late.  This is the PCM
  floor (section 6); it is not modeled.

**Boot timeline** (cycles from reset; replayed by `m4a_engine_boot`):
video starts at line 126; m4aSoundInit writes SOUNDCNT_X 0x8F @5720,
SOUNDCNT_H 0xA90E @5737, SOUNDBIAS_H 0x40 @5760, DMA on @15873, timer on
@39794; MPlayExtender's PSG writes @39875–39935 (NR52 0x8F, NR50/51 0,
NR12/22/42 = 8, NR14/24/44 = 0x80, NR30 = 0, NR50 = 0x77); DMA off @41238,
timer off @47452, DMA on @47893, timer on @320682 (the HQ-mixer test ROM's
cycles, replayed when `engine->hqMixer` is set: `docs/HQ-MIXER-ACCURACY.md`,
appendix B).  IME is set at 323299,
after frame 1's VBlank, so the first handled interrupts are frame 2's:
VSync at line 150 + 164 (DMA restart + 250), SoundMain at line 160 + 197.
There is no frame-1 SoundMain.  The song's first MPlayMain is frame 3's.
The FIFO carries 2 stale words + 3 internal bytes across the second init
pass, so ring byte k plays at timer tick 12 + k after the final enable.

## 4. The engine

### Frame structure

One VBlank frame of the engine (`m4a_engine_tick`, then
`m4a_sound_main_ram`), placed on the cycle clock where SoundMain runs:

```
SoundMain (line 160 + 197)
 ├─ MPlayMain of every player            (sequencer hook, or the MIDI path below)
 │    per frame: tempoC += tempoI; while tempoC >= 150: one tick of every track
 │    per track tick: chain walk (gates) → commands → LFO step
 │    then the VOLCHG / PITCHG pass
 ├─ CgbSound                              (m4a_channel.c, PSG writes on the CPU clock)
 └─ SoundMainRAM                          (m4a_mixer.c, PCM into the int8 DMA ring)
```

Without a sequencer (the MIDI path) the same order is kept: the gate walks,
the latched events, `mplay_main_tail` (tempo add, LFO steps, the pass,
portamento, PWM), then CgbSound and SoundMainRAM.

### Boot and the cycle clock

`plugin/m4a_gba.{h,c}` and the scheduler in `m4a_engine.c` run a cycle
clock (280896 cycles per frame) with the timer-0 FIFO sample-and-hold, the
DMA ring, the SOUNDBIAS stage and the boot replay above.
`m4a_engine_boot` ends right after frame 2's SoundMain (cycle 602986); the
renderer keeps its output, so output sample s is ROM time s · 2²⁴ / rate.
The frame sequencer is hooked into `m4a_gba_run` ahead of a same-cycle
sample event.  `m4a_engine_process` runs `run_until(now + 1024)` until the
resampler has enough samples, so the machine is up to a resampler frame past a
sample's cycle when that sample is returned (this matters for when host
events land, section 5).

### Output stage

- `plugin/m4a_resampler.{h,c}`: the two-tap linear delta-buffer
  resampler, bit-exact with mgba's fast path (the engine's resampler, and
  the reference's).
- `plugin/m4a_psg.{h,c}`: the PSG ported from mgba `src/gb/audio.c` with
  GBA register semantics; two instances (the real one and the
  polyphony-debug shadow).
- The renderer writes samples as `lrintf(x · 32768)` (exact for the
  engine's int16 / 32768 output).
- There is no analog filter (the option was removed; the CLAP state byte
  is still written as 0 for old saves).
- The PCM mix rate is the game's sound-mode enum: `engine->pcmFreqMode` =
  SOUND_MODE_FREQ_xxxxx >> 16 (1..12, indexing
  `gPcmSamplesPerVBlankTable`, default 4 = 13379 Hz).
  `m4a_engine_set_pcm_freq_mode` runs SampleFreqSet's arithmetic with
  m4aSoundMode's surround (DMA off, ring cleared, timer reload, DMA on,
  timer on one period later; the VCOUNT 159 wait is not modeled).  Config
  `pcm_mix_rate` / `--pcm-mix-rate` snap to the nearest table rate.
  Before the boot replay has started, a rate change only sets the numbers
  (the mode word is m4aSoundInit's own; the boot table turns the DMA and
  timer on at the ROM's cycles, and its first `SampleFreqSet` pass runs at
  the 224-sample period whatever the rate, as SoundInit's does); the ring
  is `engine->pcmDmaBufSize` samples per side (1584, or 7 frames in HQ
  mode at another rate).  The 21024 Hz builds of both test ROMs have their
  own measured boot tables; other non-default rates replay the 13379 Hz
  table of the mixer (`docs/HQ-MIXER-ACCURACY.md`, section 5 and
  appendix B).

### Mixer (SoundMainRAM)

`plugin/m4a_mixer.{h,c}` is `m4a_1.s` lines 88–709 (SoundMainRAM,
SoundMainRAM_Unk1, Unk2) translated with the register names kept.

- **The DMA ring** `int8_t pcmBuffer[2 · 1584]` (SoundInfo layout) is also
  the reverb memory.  The FIFO sees the wrapped bytes.
- **Reverb**: both taps, `(a + b + c + d) · reverb >> 9` with the
  `tst 0x80; addne 1` rounding toward zero, mono to both sides; no reverb
  means zero fill.  `M4AReverb` (`m4a_reverb.h`) holds only the amount.
- **Accumulation**: the packed-word `mul / bic 0xFF0000 / add ror 8`
  (`m4a_mixer_acc`): each of four samples wraps in its 8-bit lane.  The
  wrap is stock behaviour; there is no clip mode.
- **Loops**: the fixed-frequency block / single loops with their r8 / r9
  bookkeeping; the interpolating loop with `bic r9, 0x3F800000`, the
  `lr − 1 == 0 → r0 += r1` advance and the loop-end overshoot (it
  interpolates the last sample toward `data[size]`).
- `SoundChannel.frequency` is MidiKeyToFreq's result; divFreq is applied in
  the mixer.  `masterVolume` is 12 (m4aSoundInit's value).
- **The PCM envelope** (`m4a_pcm_channel_tick`) runs inside SoundMainRAM,
  per channel, just before that channel's mix.  **Release**:
  `env = env · release >> 8` per pass from the EOT frame itself (ply_endtie
  sets SF_STOP before that frame's SoundMainRAM); the channel stops on the
  pass whose product is 0 (`_081DCFB6`, unsigned compare against
  pseudoEchoVolume).  From 255, release 200 gives 199, 155, 121, … 2, 1
  (18 mixed passes).  A channel with START and STOP both set is killed
  silently (`_081DCF6A` → `_081DCFB0`).
- **Compressed and reversed samples** (SF_SPECIAL, set up on the first
  mix): REV pointer flip `data + size − offset`; CMP index `ptr − data`;
  three Unk1 loops; one engine-wide decoding buffer (`sDecodingBuffer`)
  with a per-channel xpi tag, so two compressed channels clobber each
  other's block as on the GBA; a CMP-typed voice on uncompressed data mixes
  nothing (`_081DD468`).  The reversed DPCM walk is `_081DD3C0`.
- **XCMD 0x0D** (`unk_3C`, sample start offset): stored in
  `M4ATrack.sampleStart`, handed to the channel as its count at note-on;
  SF_START converts it to `currentPointer = data + count`,
  `count = size − count` (`m4a_pcm_channel_start_at`).  The track's start
  tick clears it; it persists across notes.
- **Golden Sun synths** (zero-size DirectSound samples, `mix_synth`) are
  the one deliberate extension in the mixer (stock behaviour would be a
  one-sample click).  The oscillators are ipatix's HQ mixer's
  (`plugin/m4a_synth.h`, measured bit-exact against the HQ test ROM:
  `docs/HQ-MIXER-ACCURACY.md`, step 9) under vanilla's accumulate: phase
  step = `(divFreq · frequency) << 3` per sample, the pulse's duty LFO
  advanced by the mixer once per mixed frame, the saw's filter state
  mixed at `floor(v / 2)`.

### Samples and the voicegroup loader

`plugin/voicegroup_loader.{h,c}` loads pokeemerald / pokefirered
voicegroups and emits the exact GBA layouts (ToneData 12 bytes, WaveData
16-byte header + data) with host pointers, so ported code reads them
unchanged.  Rules that matter for accuracy:

- **The look-ahead byte**: pokeemerald's `.bin` files carry one sample
  after the data (the loop-start sample for a looped sound, 0 for an
  unlooped one), which the interpolating mixer reads.  The `.bin` path
  reads it from the file; the `.wav` / `.aif` path uses the source's own
  extra sample (`sample[size]` when the header size is an `agbl` override
  below the written count, else 0).  Verified byte-exact against all 105
  vanilla `.bin` files.
- **DPCM blocks**: `size/64` full 0x21-byte blocks plus a partial last
  block; the loader allocates `size/64 + 1` blocks and fills the bytes the
  `.bin` lacks with what follows it in the ROM — `.align 2` zeros, then
  the next entry's header and data in `direct_sound_data.inc` order (the
  vanilla mixer never outputs those samples, indices ≥ size; ipatix's HQ
  mixer plays a few of them as a cry runs out).  The ROM's bytes *before*
  a sample (its header, the padding, the previous entry's tail, up to
  1280) are kept in front of the data as well (`WaveData.romBefore`; a
  `.wav`-loaded sample gets its header rebuilt from the fields, byte for
  byte the `.bin`'s): the HQ mixer's reversed uncompressed end frame reads
  them.  Both come from the symbol map's file order; a sample without a
  neighbour (the first / last entry, another layout) gets zeros, and the
  header alone before it.
- **Fallbacks**: a compressed voice whose `.bin` is missing or stale falls
  back to the uncompressed source with the CMP bit cleared; a loop with
  loopStart ≥ size is disabled.
- **Voice arguments** are parsed with `%i` (hex operands such as sweep
  `0x3A`).
- **Cries**: `voicegroup_cry_table_entry` counts `cry` / `cry_reverse` /
  `cry_uncomp` / `cry_reverse_uncomp` lines across both `gCryTable` and
  `gCryTable_Reverse` (index 388 + i is the reversed cry i) and returns the
  macro's type byte (0x20, or 0x30 = CMP | REV for a reversed cry, the same
  wav).  `voicegroup_load_cry` / `_typed` build the voice.
- `probe_keysplit_data_in_dir` (additive keysplit-table discovery for the
  eventide layout) is unconditional and a no-op on standard projects.

### PSG register driver (CgbSound)

`plugin/m4a_channel.c` is CgbSound transliterated from `m4a.c` with its
labels kept, on top of `m4a_psg`.  CGB notes start at the next SoundMain
(CHN_START).

- **CgbModVol** is called only where CgbSound calls it: at note start and
  when envelopeCounter reaches 0.  MPlayMain's VOLCHG pass is ChnVolSetAsm
  + MO_VOL.
- **c15** (the 15-step envelope frame counter) starts at 0 (SoundInit);
  frame 2's CgbSound reloads 14, frame 3 sees 13, so the envelope double
  step falls on frames 17, 32, 47, …
- **gCgb3Vol over-range**: on a centre-panned wave, envelopeGoal reaches
  16..31; agbcc stores `(left + right) / 16` without a u8 wrap and the GBA
  reads past gCgb3Vol into gClockTable (bytes < 0x20), giving NR32 volume
  0.  It is modeled that way.
- **Fixed-frequency voices** (`_alt`, TONEDATA_TYPE_FIX on square 1 / 2 /
  wave): the 11-bit frequency is quantized to `(freq + 1) & 0x7FE`
  (Emerald's 65536 Hz DAC branch), applied wherever the ROM sets MO_PIT.
  `voice_noise_alt` is a hardware no-op.
- **Register sequences** (byte writes): square start NR10, NRx1, then
  MO_PIT NRx3 / NRx4 (no trigger), then MO_VOL NR51, NRx2, NRx4 | 0x80 (a
  second NR14 when NR10 bit 3 is clear); wave NR30 = 0x40, wave RAM × 4,
  NR30 = 0, NR31, NR33, NR34 = 0x80 | hi, NR51, NR32, NR30 = 0x80, NR34;
  noise NR41, NR43 = period << 3, NR43 = (old & 8) | freq, NR44 = 0, NR51,
  NR42, NR44 = 0x80; CgbOscOff NRx2 = 8, NRx4 = 0x80 (wave NR30 = 0).
- **Write timing.**  A cycle cursor runs through the four channels from
  the CgbSound entry, charging each pass's measured segments (`CGB_CYC_*`,
  appendix A); each write first runs the hardware to its cycle.  The entry
  is `engine->soundMainStamp + engine->cgbEntryCycles` (section "SoundMain
  timing model").  Square duty phase is free-running, so squares only need
  DAC-slot (256-cycle) accuracy; wave RAM rotation and the LFSR restart at
  the trigger, so a wave / noise trigger a few cycles off costs 20–40 dB.

### Sequencer (MPlayMain)

- `plugin/m4a_song.{h,c}` parses mid2agb's `.s` output (the GNU as subset
  it uses plus the MPlayDef.s vocabulary built in; no binutils at run
  time) into the song's `.rodata` image with a relocation list.  The
  voicegroup comes from the header's `_grp` symbol.
- `plugin/m4a_sequencer.{h,c}` ports MPlayMain and the `ply_*` handlers,
  structured like `m4a_1.s`.  It runs as the engine's frame hook at the top
  of `m4a_engine_tick` (MPlayMain's place in SoundMain) with
  `externalSequencer` set, which turns off the engine's own gate / tempo /
  LFO work.  The tempo is the accumulator verbatim (`tempoC += tempoI`,
  ticks while ≥ 150), never derived from BPM.
- Both MPlayMain loops (the tick and the VOLCHG / PITCHG pass) run over the
  player's whole `trackCount` (BGM 10, SE1 3, SE2 9, SE3 1, cry 2), not
  the song's.
- Track changes are written to the engine track and applied by the pass
  (`m4a_engine_track_apply`), with ply_note's `flags &= 0xF0` (a note that
  takes a channel drops the track's pending VOLCHG / PITCHG).
- `m4a_engine_note_on` returns the channel it started, so the sequencer
  sets gate times in ticks and finds the newest channel for EOT.  Chain
  membership and order (`chainOwner` / `chanSeq`) reproduce the ROM's
  channel chains, including neighbours changing as dead channels drop.
- **ply_fine**: SF_STOP on every chained channel, RealClearChain
  (`channel->track` = `M4A_NO_TRACK`), track flags 0; the track takes no
  further LFO step, and an orphaned releasing channel is reached by no
  later pass.  **TrackStop** kills only channels still chained to the
  track.  `M4ASong.dataInRam` (cry songs) makes every song-data read 7
  cycles cheaper.
- `poryaaaa_render --song <file.s>`: without `--total-duration-seconds`
  the song's FINE ends the render and `--loop-count` counts GOTO passes.

### Players and chaining

- `m4a_sequencer_set_player`: BGM (10 tracks), SE1 (3), SE2 (9), SE3 (1),
  `M4A_PLAYER_CRY0` / `CRY1` (2 each), matching `gMPlayTable` and
  m4aSoundInit's MPlayOpen order.  A song's tracks are clamped to the
  player's (MPlayStart's `min`).
- **The chain**: MPlayOpen prepends each player to SoundInfo's list (open
  order BGM, SE1, SE2, SE3, cry 0, cry 1), and each MPlayMain calls the
  previously opened player before its own body, so the bodies run BGM
  first.  Frame cost (CgbSound entry) = **1007 + Σ (79 + body)** over the
  playing players, where body = `_081DD9BC` → `_081DDA6C` and 1007 is the
  all-paused frame.
- **Several players in one render**: `m4a_sequencer_init_chained_player(
  outer, inner, trackBase, player)` (and `init_chained`, which picks
  `inner->player + 1`) puts a second sequencer on engine tracks
  `[trackBase, trackBase + tracks)` (MAX_TRACKS 16), with its own
  voicegroup (`m4a_sequencer_set_voicegroup`).  Accepted chains include
  BGM → cry 0 (tracks 10..11), SE1 → cry 0 (3..4), cry 0 → cry 1 (2..3).
  The inner's hook runs the whole chain; idle gap players are charged
  63 each.
- **Cross-player steal**: when a note takes a channel another player's
  chain holds, the owner is found along the chain (`chain_owner_of`), the
  unlink is charged against the owner's links (`NOTE_UNLINK_*`) and the
  owner's chain loses the channel at once, as ply_note's ClearChain does
  on the GBA.

### Cries

- **The cry song** (`plugin/m4a_cry.{h,c}`): `gM4ACrySongTemplate` is the
  ROM's `gPokemonCrySongTemplate` (52 bytes: TUNE, GOTO, VOICE 0, VOL,
  XCMD 0x0D, xRELE, PAN, TIE, xWAIT, EOT, FINE; template offsets TUNE
  0x12, TUNE2 0x19, VOL 0x1D, xRELE 0x26, PAN 0x28, TIE key 0x2A, xWAIT
  0x2E..0x2F).  `m4a_cry_song_new(M4ACryParams)` applies the
  SetPokemonCry* setters in PlayCryInternal's order and patches the pointer
  words as SetPokemonCryTone does; the data is IWRAM (`dataInRam`).
  - Pitch: b = pitch + 0x80; TIE key = b >> 8; TUNE = (b >> 1) & 0x7F;
    TUNE2 = (16 + TUNE) & 0x7F.  Pan: (pan + 0x40) & 0x7F.  Volume:
    & 0x7F.  Length → xWAIT, release → xRELE.
  - Chorus ≠ 0 makes the song two tracks: track 1's TUNE2 =
    (chorus + TUNE) & 0x7F (s8: the game's u32 192 is −64).
  - Defaults (`StartCry`'s CRY_MODE_NORMAL): pan 0, volume 120, pitch
    15360 (key 60, 10512 Hz), length 140, release 0, chorus 0, priority 10.
- **Samples**: every cry is 10512 Hz DPCM (type 1, status 0, no loop),
  1902 (Pidgey) to 23524 (Jynx) samples; at the default pitch none is still
  sounding at the 140-tick EOT.
- **`poryaaaa_render --cry <id>`** plays the template with the cry's
  sample as voice 0 on cry player 0; `--cry-pitch/-pan/-volume/-length/
  -release/-chorus/-progress`.  `--cry2 <id> --cry2-start-frame <F>`: a
  second cry at the defaults, started at the top of engine frame F + 2's
  hook (VBlank F's StartCry → its start tick on frame F + 2): with `--cry`
  on cry player 1 (engine tracks 2..3), with `--song` on cry 0 chained
  after the song's player.
- **Player choice** (`SetPokemonCryTone`, `m4a.c` 1660–1699;
  `m4a_cry_choose_player`): the first free cry player (track 0's flags 0
  and no channel of its own).  When both are busy the game replaces the
  player with the strictly larger `clock` (ticks since its own MPlayStart;
  a tie → player 0) and MPlayStart's TrackStop cuts its chained channels
  outright (status 0, no release, no `_081DCFB0` stamp).  poryaaaa refuses
  that case (section 6).
- **Steal rule** (ply_note, `m4a_1.s` 1690–1740): a stopping candidate
  (ON | STOP) wins regardless of priority; otherwise the lowest-priority ON
  channel, ties going to the later track; a later STOP channel of the same
  track at equal priority is preferred.
- **Channel death**: through the count path (sample out of data: CHN_ON
  clears after frame N − 1's mix, the unlink tick is frame N, no stamp) or
  through `_081DCFB0` (release / echo to 0, or START with STOP; two
  channels in one pass stamp 69 cycles apart in index order).

### SoundMain timing model

The PSG writes happen inside CgbSound, which starts when MPlayMain has
finished, so their cycles depend on what MPlayMain did that frame.  The
engine charges MPlayMain's work cycle for cycle from measured segment
costs and publishes the sum as `engine->cgbEntryCycles`.

- **Fixed points** (from line 160): m4aSoundMain + 185, SoundMain + 197,
  the six MPlayMain entries at 291 / 346 / 401 / 456 / 511 / 566 (cry 1,
  cry 0, SE3, SE2, SE1, BGM), the BGM body from 566.  Paused BGM: entry →
  `_081DDA6C` 71; each paused player after a deeper one 63; the last
  `_081DDA6C` → CgbSound 55.  All paused: 1007.  BGM playing: 566 + 150 +
  body + 370.
- **Charging** (`m4a_sequencer.c`, constants `M4A_CYC_*` in
  `plugin/m4a_engine.h`): per tick `TICK_ENTER` and, over the player's whole
  trackCount, `TICK_TRACK` / `TICK_NO_TRACK`; per existing track the chain
  walk by gate state and RealClearChain's link case, the START init, then
  the wait / LFO path or the command loop (fetch, explicit byte vs running
  status, the handler, ply_note's byte parse, voice resolution, the CGB
  test or the DirectSound scan channel by channel, the unlink, TrkVolPitSet,
  ChnVolSetAsm, the frequency routine, `W##`); then the pass per track and
  chained channel.  Appendix A lists the values.
- **The FIFO DMA stall** (18 cycles at frame cycle 15091 and every 20064
  after) is charged on whichever segment spans it, in both the sequencer
  and the CgbSound cursor (`m4a_gba_fifo_dma_stall`).
- **Song data in IWRAM** (cries) costs 7 less per byte read
  (`M4A_CYC_ROM_BYTE`).
- **Without a sequencer** the entry is fixed at the one-track no-tick value
  (1205 after SoundMain = 1402 from line 160) unless a timing-only
  sequencer publishes it (section 5).  That places the PSG writes; the
  cost model's published sequencer term is then the MIDI-path estimate
  ("MIDI-path sequencer estimate" below), which moves no write.
- `TrackStop` runs in the main loop, outside SoundMain; the engine charges
  nothing for it.

Every constant is a probe-measured segment; a few paths the corpus never
takes are estimates (marked *est.* in the header).  A future song that
takes one shows up in `cmd_diff` / `track_diff` as a constant offset on
that command.

### SoundMain cost model

The CPU a frame's SoundMain takes (`m4aSoundMain` entry to its return,
the number a host shows as a percentage of VBlank) is the sequencer term
above, CgbSound, and the mixer.  What the probe measures (2026-10-29,
mGBA 0.10.5, both ROMs at 13379 and 21024 Hz):

- **The exits.**  SoundMain tail-jumps (`bx r3` after `SoundMain_5`) into
  `SoundMainRAM_Buffer`, whose last instruction returns through the lr
  SoundMain pushed, i.e. to the instruction after m4aSoundMain's
  `bl SoundMain`; the HQ mixer returns the same way.  `mgba_probe` decodes
  three Thumb `bl`s from the ROM (no symbol names them) and breaks after
  each: `SoundMain_return` (in m4aSoundMain, `RET`), `m4aSoundMain_return`
  (in the test ROM's `VBlankIntr`, `EXIT`) and `SoundMain_cgbret`
  (CgbSound's return into SoundMain, after the `bl` that follows
  `SoundMain_4`, `CR`).  `.frames` carries them after `RAM` and the
  derived `mix = EXIT − RAM`, `tot = EXIT − sm`, `tail = CR − ` channel 4's
  `CgbSound_next` breakpoint and `s2r = RAM − CR`.  A frame's terms for
  `tools/cost_diff.py`: seq = CGB − sm, cgb = RAM − CGB, mixer = EXIT − RAM;
  they sum to `tot`.  The returns (and channel 4's `CgbSound_next`) are
  charged to the frame of the m4aSoundMain they return from, relative to
  that frame's line 160, so a SoundMain longer than VBlank keeps its exit
  (RET / EXIT past the frame window) instead of falling into the next
  frame's record.  The frame counter is the one `--pcm-log` and the
  engine's `--cost-log` use: frame k starts at cycle 124773 + (k − 1) ·
  280896, the first SoundMain is frame 2, `--pcm-log`'s `f 3` is probe
  frame 3.  The new breakpoints change nothing: with them, all 81
  vanilla logs are the earlier probe's line for line once their own lines
  are removed (raw log, `.events` and every older `.frames` column).
- **Fixed segments.**  m4aSoundMain's epilogue (`RET` → `EXIT`) 10 (28
  with the FIFO DMA stall in it).  CgbSound's tail: 36 from channel 4's
  `CgbSound_next` to `CR` in every frame (54 with the stall); in the
  CgbSound cursor's terms (`m4a_engine.c`, the cursor after channel 4 is
  its `next` + 16, where the next loop head would be) **`M4A_CYC_CGB_TAIL`
  = 20**: a frame with no CGB channel runs CgbSound → `CR` in
  53 + 4 × 44 + 20 = 249 (247 on the c15 reload).  **`M4A_CYC_SM_TO_RAM`
  = 63** (`CR` → `SoundMainRAM_Buffer`: `SoundMain_4`'s `ldr`/`str`s, the
  ring-offset multiply, `SoundMain_5`, `bx r3`), **59** on the frame whose
  `pcmDmaCounter` is 1 (the `bls SoundMain_5` is taken and the multiply
  skipped; one frame in `pcmDmaPeriod`), 77 / 81 with the stall.  The HQ
  mixer's `C_downsampler_return` → `RET` is 39 (57 with the stall).
- **Ownership.**  The engine's `mixer` term owns `RET` → `EXIT` (10; 28
  with the stall) and, in HQ mode, `C_downsampler_return` → `RET` (39;
  57), so that total = seq + cgb + mixer = `EXIT` − m4aSoundMain entry.
- **Mixer floor** (no DirectSound channel; `mix`, so the 10-cycle epilogue
  is in it):

  | | 13379 Hz | 21024 Hz |
  |---|---|---|
  | vanilla, reverb 0 | 498 | 658 |
  | vanilla, reverb on (the pass over the ring, any amount) | 6489 | 10091 |
  | HQ (reverb or not) | 7732 | 12038 |

  The vanilla reverb pass is about 26.8 cycles per output sample
  (5991 / 224, 9433 / 352); `pcm_reverb` (R50) and `pcm_reverb_max` (R127)
  cost the same.  The HQ floor is the 7683 of `docs/HQ-MIXER-ACCURACY.md`
  (to `C_downsampler_return`) plus 39 + 10, and does not move with reverb.
  A frame the FIFO stall misses reads 18 less (HQ 21024's minimum, 12020).
- **Range over the corpus** (81 logs, 85 on HQ with the `synth_*` rows;
  frame 2, the idle SoundMain before a test starts, is the minimum).
  `mix`: vanilla 13379 498 – 53841 (`cry_over_bgm` frame 87: five BGM
  channels and a DPCM cry), HQ 13379 7732 – 39923 (`littleroot` frame
  702), vanilla 21024 658 – 72801 (`cry_over_bgm` frame 95), HQ 21024
  12020 – 57133 (`littleroot` frame 702).  `tot` (the whole
  m4aSoundMain): vanilla 13379 1626 – 64790, HQ 13379 8860 – 52657,
  HQ 21024 13166 – 65146 (all three maxima `mt_chimney` frame 3, the song
  start's sequencer), vanilla 21024 1786 – **87254**.
- **Past the VBlank window** (83776, the CPU meter's 100 %): only
  vanilla at 21024 Hz, only `rg_vs_trainer` (the FRLG drumset: up to five
  DirectSound channels with reverb), 11 frames: 51, 72, 92, 98, 103, 113,
  119, 134, 140, 145, 160, `tot` 83938 – 87254 (frame 92, 104 %; its mixer
  71734).  Every other log stays inside VBlank; none comes near the frame
  (280896).
- **First look per channel** (`cost_diff.py --summarize`: a least-squares
  `mix = a + b · n` over frames 3.., n = the channels `--pcm-log` lists
  after the frame; b at the two rates split as b = per channel-frame +
  per output sample):

  | test (path) | vanilla 13379 b (rms) | vanilla 21024 b | → split | HQ 13379 b (rms) | HQ 21024 b | → split |
  |---|---|---|---|---|---|---|
  | `pcm` (interpolating, step < 1) | 5937 (176) | 8899 | 753 + 23.1 / smp | 3740 (761) | 5643 | 410 + 14.9 |
  | `pcm_hi` (interpolating, step > 1) | 8454 (432) | 11955 | 2328 + 27.4 | 6428 (1996) | 9064 | 1816 + 20.6 |
  | `pcm_short_loop` (loop wrap) | 8916 (8) | 13740 | 473 + 37.7 | 8027 (1587) | 12318 | 517 + 33.5 |
  | `pcm_fix_loop` (FIX) | 4853 (14) | 7566 | 105 + 21.2 | 2266 (410) | 3455 | 184 + 9.3 |
  | `pcm_drum` (FIX, 1–2 ch) | 4134 (956) | 7265 | — | 1994 (391) | 3287 | — |
  | `pcm_cmp` (DPCM forward) | 19047 (1637) | 22296 | 13361 + 25.4 | 7176 (674) | 9080 | 3844 + 14.9 |
  | `pcm_rev` (reverse) | 6896 (624) | 9986 | 1490 + 24.1 | 4077 (509) | 5986 | 737 + 14.9 |
  | `pcm_reverb`, `pcm_reverb_max` | 5937 (176), a = 6494 | 8881, a = 10096 | 785 + 23.0 | as `pcm` | | |
  | `synth_pulse` (HQ) | | | | 1660 (294) | 2505 | 182 + 6.6 |
  | `littleroot` (2–5 ch) | 7793 (1302), a = 5791 | 11086 | 2031 + 25.7 | 3474 (3361), a = 11045 | 4941 | 906 + 11.5 |
  | `vs_wild` (1–5 ch) | 7752 (1601), a = 6614 | 11175 | 1760 + 26.8 | 4212 (2948), a = 7100 | 6063 | 973 + 14.5 |

  `pcm_chord` (four channels in every frame) is 25329 (vanilla 13379),
  37301 (21024), 24100 / 36521 (HQ), rms 16 / 16 / 1050 / 1452.  The micro
  rows' a is the floor (502 / 662, 7735 / 12041 HQ; the reverb rows 6494 /
  10096).  Not a model, a map for the per-block constants: the residual
  is per note (the interpolating cost follows the step, i.e. how many
  source samples a frame fetches: `pcm`'s one-channel frames run 6428 – 6756), a
  channel that dies inside the frame (count path, release reaching 0) is
  not counted but was walked, and DPCM's cost is mostly the block decode,
  which follows the source rate, not the output rate.  The HQ residuals
  are larger (its fast loop has three blocks by step).

**The model (step 2: the vanilla mixer).**  The engine publishes
`M4AFrameCost {sequencer, cgb, mixer, total, valid}` as `engine->frameCost`
(the last SoundMain) and `engine->frameCostMax` (the largest total since
`m4a_engine_reset_frame_cost_max`, with its components), written at the end
of `sound_main` after SoundMainRAM and before `frameEndHook`
(`publish_frame_cost`, `m4a_engine.c`).  The terms are the probe's split,
on the CPU clock from the SoundMain stamp:

- **sequencer** = CgbSound entry − m4aSoundMain entry = `cgbEntryCycles`
  + 12 (`M4A_CYC_SOUNDMAIN_ENTRY` 197 − `M4A_CYC_M4A_SOUNDMAIN` 185).  With
  an `M4ASequencer` (or the timing-only one of `--gba-tempo-grid`) it is
  MPlayMain's charged work, exact on the corpus; without one, the
  MIDI-path estimate (`frameCost.sequencerEstimated`; "MIDI-path sequencer
  estimate" below).  The estimate moves CgbSound's start, not its work:
  the cgb span the engine ran from the fixed entry (its FIFO stalls taken
  out) is placed at the estimated entry and the stalls are charged again
  there, and SoundMainRAM's entry and the mixer's stalls follow.
- **cgb** = SoundMainRAM entry − CgbSound entry: the real pool's CgbSound
  cursor after channel 4 (`engine->cgbCursorEnd`) − the entry, +
  `M4A_CYC_CGB_TAIL` (20) to CgbSound's return, + `M4A_CYC_SM_TO_RAM` (63;
  `M4A_CYC_SM_TO_RAM_FIRST` 59 when `pcmDmaCounter` ≤ 1), the FIFO stall
  charged on each span.  The probe's `tail` and `s2r` columns live here, as
  in `cost_diff.py`'s cgb = RAM − CGB.
- **mixer** = EXIT − RAM: the `M4A_MIX_*` charges `m4a_mixer.c` sums into
  `engine->mixCycles` (the epilogue's `RET` → `EXIT` 10 included; in HQ
  mode the `M4A_HQMIX_*` charges of `m4a_hq_mixer.c`, below), plus every
  FIFO stall due in [RAM, EXIT − 8], in order (`m4a_engine_mixer_stall`,
  `m4a_engine.c`): a stall due in m4aSoundMain's last 8 cycles
  (`M4A_MIX_EXIT_BX`, the final `bx r0` and the cycle before) is serviced
  after the return and not counted (over the four probed rates, a stall
  due at `RET` + 3 or later never was; one due at `RET` + 2 was in 3 frames
  of 4).  Step 2 had the bound at EXIT − 7, which counts `RET` + 3: no
  vanilla frame of the corpora is due there, six HQ 21024 Hz frames are
  (`lfo_pan` 25, 53, 62, 63, `littleroot` 230, `vs_trainer` 296), none
  counted by mgba; with 8 every vanilla and HQ frame matches.
- **total** = sequencer + cgb + mixer = EXIT − m4aSoundMain entry (`tot`).
- **valid** is false in live mode (the piecewise mix charges nothing);
  `frameCostMax` ignores invalid frames.  The polyphony-debug shadow pool
  costs nothing (in HQ mode it also leaves `previous_fast_code` and the
  DMA3 record alone).
- **Threading.**  Both fields are written by plain struct assignment on
  the thread that calls `m4a_engine_process`; a host snapshots them there
  (atomics, total last) and never reads them concurrently.  One process
  call can run several SoundMains: `frameCost` is the last, `frameCostMax`
  covers the rest.  `m4a_engine_reset_frame_cost_max` only raises a flag
  the next SoundMain consumes, so any thread may call it.  The live-mode,
  HQ, sound-mode-rate and voicegroup setters clear both (no stale peak
  from another configuration); a host resets the peak itself on
  play-from-stop.

The mixer constants (`plugin/m4a_engine.h`, each with its label pair) are
**derived, none fitted**: the instruction count of each block of m4a_1.s at
mgba's ARM7 timing, with SoundMainRAM, Unk1 and Unk2 in IWRAM
(`SoundMainRAM_Buffer`; the copy covers all three) and SoundInfo, the
channels, the ring and `sDecodingBuffer` in IWRAM: an instruction 1, a
taken branch or `bx` 3 (1 not taken), `ldr`/`ldrb`/`ldrsb` from IWRAM 3,
`str` 2, `ldm` n + 2 (+2 loading pc), `stm` n + 1, `mul` 1 + m (m 1..4 from
the Rs operand's significant bytes, mgba's `ARM_WAIT_MUL`), an 8/16-bit ROM
load 6 (WS0 N 3 + 2 + the fetch; the prefetch buffer does nothing for code
in IWRAM: `GBAMemoryStall` only runs for code in ROM), a 32-bit ROM load 8.
The test ROM's `WAITCNT` is the game's (WS0 3/1, prefetch on).

| Constant(s) | Labels | Value |
|---|---|---|
| `ENTRY`, `NOREVERB` (+`_4`, `_8`), `NOREVERB_LOOP` | SoundMainRAM → `_NoReverb` → `_Loop` (16 samples a pass) | 7, 11 (+2, +6), 20 |
| `REVERB`, `REVERB_SAMPLE` | → `_081DCEC4` and back to `_081DCF36`; one `_081DCEC4` pass | 15, 28 |
| `CHAN_SETUP`, `SLOT_OFF`, `SLOT_LAST` | `_081DCF36` → ChanLoop; an idle slot; the last slot's `ble` | 11, 30, −2 |
| `EXIT`, `EPILOGUE`, `EXIT_BX` | `_081DD24A` → `RET` (the `bx r3` refills from ROM); `RET` → `EXIT`; its `bx r0` and the cycle before | 33, 10 (measured), 8 (measured) |
| `CHAN_ON`, `CHAN_NEXT` | ChanLoop → `_081DCF6A`; `_081DD240` → ChanLoop | 20, 9 |
| `ENV_START` (+`_LOOP`), `ENV_START_STOP`, `ENV_END` | SF_START: `_081DCF6A` → `_081DCFF8` (+ the loop flag); SF_START\|SF_STOP: → `_081DCFB0`; `_081DCFB0` → `_081DD240` | 44 (+5), 8, 6 |
| `ENV_HEAD`, `ENV_IEC`, `ENV_IEC_END`, `ENV_NOT_IEC` | not START: `_081DCF6A` → `_081DCFA0`'s IEC test; IEC: → `_081DD006` / → `_081DCFB0`; → `_081DCFB6` | 10, 10, 8, 5 |
| `ENV_RELEASE`, `ENV_RELEASE_LOW`, `ENV_ECHO_START`, `ENV_ECHO_NONE` | SF_STOP: `_081DCFB6` → `_081DD006` / → `_081DCFC8`; `_081DCFC8` → `_081DD006` (IEC set) / → `_081DCFB0` | 14, 12, 12, 7 |
| `ENV_NOT_STOP`, `ENV_DECAY`, `ENV_DECAY_SUSTAIN`, `ENV_DECAY_ZERO`, `ENV_SUSTAIN`, `ENV_TO_ATTACK`, `ENV_ATTACK`, `ENV_ATTACK_PEAK` | → `_081DCFD6`; decay → `_081DD006` / reaching sustain / reaching 0 → `_081DCFC8`; sustain → `_081DCFF4` → `_081DD006`; → `_081DCFF8`; `_081DCFF8` → `_081DD006` below / at 0xFF | 6, 14, 20, 16, 7, 5, 8, 10 |
| `VOL`, `VOL_LOOP` | `_081DD006` → `_081DD03A` → `_081DD044`'s type test; + loop start / length from ROM | 63, 22 |
| `TO_FIX`, `TO_INTERP`, `TO_UNK1`, `STORE`, `STORE_FW`, `RETURN_UNK1` | → `_081DD07C`; → `_081DD19C`; the `bl`; `_081DD22C` / `_081DD228` → `_081DD240`; Unk1's return | 10, 12, 4, 11, 13, 16 |
| `WORD_LOAD`, `NEXT_SAMPLE`, `WORD_STORE` (`_SP`), `NEXT_WORD`, `LOOP_LAST` | per-word instruction groups of every word loop: the word head's two `ldr` (`_081DD0A8`, `_081DD0EC`, `_081DD1B4`, `_081DD308`, `_081DD3D8`, `_081DD480`); the sample tail `adds r5` / `bcc` taken (`_081DD0B0`, `_081DD118`, `_081DD208`, `_081DD370`, `_081DD440`, `_081DD4CC`); the same `bcc` falling through to the two `str` and `subs r8` (Unk1: and `ldr`/`subs`/`str` of `[sp]`); the `bgt` back to the word head (1 when it falls through); any loop's last backward branch | 6, 4, 7 (12), 3, −2 |
| `INTERP_SETUP` (+ m), `INTERP_SAMPLE`, `HOLD`, `INTERP_STEP1`, `INTERP_STEPN` | `_081DD19C` → `_081DD1B4`; `_081DD1BC` to the `movs`; no advance; advance by one (one ROM `ldrsb`) / more (two) | 20, 13, 3, 14, 19 |
| `INTERP_WRAP`, `WRAP_ITER`, `INTERP_END`, `INTERP_DONE` | `_081DD134` → `_081DD148` → `_081DD1FC`; another `_081DD148` pass; `_081DD158` → `_081DD174` → `_081DD240`; the frame's end | 32, 6, 45, 18 |
| `FIX_SHORT`, `FIX_BLOCK`, `FIX_BLOCK_END`, `FIX_SAMPLE`, `FIX_BLOCK_DONE`/`_TAIL`, `FIX_TAIL`, `FIX_WRAP`, `FIX_END`, `FIX_TAIL_DONE` | `_081DD07C`'s three ways into `_081DD0A8` / `_081DD0EC`; a sample (ROM `ldrsb`, both accumulates); after the block; `_081DD0F4`'s count test; `_081DD164` looping / ending; the tail's exit | 4, 7, 11, 14, 4 / 2, 2, 14, 34, 4 |
| `UNK1_HEAD`, `UNK1_FIRST` (`_REV`), `UNK1_FIRST_RAW`/`_CMP`, `UNK1_SETUP` (+ m), `UNK1_RAW`, `UNK1_CMP`, `UNK1_CMP_FWD`/`_REV`, `UNK1_RAW_FWD`/`_REV` | Unk1 → `_081DD2B4` (SF_SPECIAL set / first frame); `_081DD29C`; the step; the type tests; around the two first Unk2 calls; `_081DD468` | 10, 18 (29), 10 / 12, 22, 3, 8, 4 / 7, 14 / 18 |
| `CMP_STEP1`/`STEPN`, `CMP_WRAP`, `CMP_END`, `REV_STEP`, `REV_END`, `UNK1_DONE` (`_RAW`) | a DPCM advance (+ one / two Unk2 calls); `_081DD398` → `_081DD3B0` → `_081DD358`; → `_081DD4F4`; a raw REV advance (two ROM `ldrsb`); past the start; the frame's end | 12 / 12, 31, 35, 17, 28, 11 (8) |
| `UNK2_CALL`, `UNK2_HIT`, `UNK2_DECODE` (+ m), `UNK2_UNMAPPED` | the `bl SoundMainRAM_Unk2`; SoundMainRAM_Unk2 → `_081DD594` → its `pop {…, pc}` with the block held; SoundMainRAM_Unk2 → `_081DD568`/`_081DD57C` (32 × 14 + 31 × 16 − 2) → `_081DD594` → the `pop`, 46 + 17 around the loop; a block before the sample's start (REV): its address wraps to region 0x8C, which mgba's `GBALoad8` serves from its `default:` case (no waitstate), so the 33 byte loads cost 3 instead of 6 | 3, 32, 1005, −99 |
| `SYNTH_SETUP`, `SYNTH_SAMPLE` | poryaaaa's Golden Sun synths under the vanilla accumulate (no ROM routine): *est.* as the interpolating loop without its fetch | 20, 16 |

Two values came from the probe rather than the first count, both
mechanisms of mgba rather than fits: the stall placement at the final
`bx r0` (18 cycles in 4 frames at 13379, 12 at 21024) and the unmapped
decode (99 in `cry_bulbasaur_reverse` frame 49 and the two
`cry_pidgey_echo_start` rows).

**Gate** (`cost_diff.py --corpus`, every frame's total within 2 % or 300
cycles; the cost log against the probe's `tot`):

| corpus | tests | frames gated | max \|Δ\| | frames within 1 % |
|---|---|---|---|---|
| vanilla 13379 (`build/probe`) | 79 | 18333 | **0** | 100 % |
| vanilla 21024 (`build/probe-21024`) | 79 | 18333 | **0** | 100 % |
| vanilla 15768 (13 tests, a scratch probe: the `spv & 8` clear) | 13 | 5251 | 18 | 100 % |
| vanilla 42048 (13 tests, 704 samples a frame) | 13 | 5251 | 0 | 100 % |

Every column (seq, cgb, mixer) matches, not only the total.  Not gated:
`cry_third_jynx_wailord_bulbasaur` (and its `_p1`), a third cry
poryaaaa's renderer cannot start.  The corpus does not exercise (derived,
unchecked): the pseudo-echo (IEC) envelope paths, START|STOP, an attack
step below 0xFF, the echo start, a DPCM loop wrap or multi-sample DPCM
advance, the uncompressed CMP no-op, a second `_081DD148` / `_081DD3B0`
pass, the `spv & 4` clear (7884 Hz only, which the pristine tree cannot
build: no `SOUND_MODE_FREQ_7884`) and the synth estimate.  Their wiring
(each charged where the translation takes the branch, with the expected
iteration and decode counts) is unit-tested (`test_frame_cost_wiring`).

**Residual sources.**  The FIFO stall at the epilogue boundary (one frame
in the 15768 check, 18 cycles: mgba's servicing order around the final
`bx` is not a pure offset rule).  A DPCM block past the end of a sample
reads cartridge space, 6 per byte even beyond the ROM's size (mgba's
`GBALoad8` sets the cart waitstate before its bounds check).  The model
assumes the game's
`WAITCNT`, samples in ROM, the vanilla m4a_1.s and `maxLines` 0, and
mgba's timing (docs/cpu-meter D7: hardware may differ by a few percent).

**The model (step 3: the HQ mixer).**  In HQ mode the same publication,
the same sequencer and cgb terms; the **mixer** term is EXIT − RAM over
ipatix's mixer: `SoundMainRAM_Buffer` → `C_channel_state_loop` … the
channel walk … `C_main_mixer_return` → `C_downsampler` → its loop →
`C_downsampler_return` → `RET` (39; the noise-shape stores, the ident
store, the pops and the `bx r3` into ROM) → `EXIT` (10), the FIFO
stalls in [RAM, EXIT − 8] on top.  `m4a_hq_mixer.c` charges
`M4A_HQMIX_*` (`plugin/m4a_engine.h`) into `engine->mixCycles` at the
ported labels, in program order (the walk too, unlike the vanilla
model's separate sum, and the last slot's shorter exit, `LOOP_LAST`,
after the walk rather than up front: inside the walk `mixCycles` is the
true clock, which the DMA3 copies' recorded starts and the FIFO rule
below are measured on; moving a charge ahead of its place would shift
that rule).  The
floor is 16 + 5 × (16 + 9) − 2 + 23 + 17 + 112 × 67 + 39 + 10 = **7732**
(224 samples), 12020 at 352 (12038 with the stall the probe saw).

The instruction-count constants (the table, the FIFO row aside) are
**derived**, none fitted:
every label pair counted from
`test/accuracy/rom/hq/m4a_1.s` at mgba's timing (the rules above; the
mixer runs from its IWRAM copy, Thumb for the walk and the envelope, ARM
for the rest; SoundInfo, the channels, `hq_buffer` and the stack are
IWRAM; samples, WaveData headers and synth descriptors ROM), then each
checked against a probe run breaking on *every* `C_` / `F_` label of the
mixer (a scratch `mgba_probe` with the label filter widened; breakpoints
do not change the emulation): the per-pair delta histograms are single
valued except where a FIFO stall (+18) lands or the data decides.  Two
mgba rules the vanilla model did not need: an `ldm` of ROM words costs
4n (`GBALoadMultiple`: S32 − N32 + n (S32 + 1) + 1 at WS0 3/1; the
two-word loop bounds 8, the FIX path's one-word loads 4), and the DMA3
copy 5 per word + 4 (`GBADMAService`).  Where an operand varies per
sample the charge carries its m: the fast loops' `mulne` (by the diff),
`mlane` (by the output, 2 + m when executed, 1 when not), the
unbuffered loop's `mul` / `mla`, the saw / triangle `mla` (by the wave
value), the downsampler's two reverb products (by the tap sums), the
block-address `mla` (by the block index), `umlal` (by the frame length,
unsigned) and the step `mul` (by divFreq: 2 at 13379 and 21024).

| Constant(s) | Labels | Value |
|---|---|---|
| `ENTRY`, `SKIP`, `LOOP_NEXT`, `LOOP_LAST` | SoundMainRAM → `C_channel_state_loop`; → `C_skip_channel` → `C_end_channel_state_loop`; → the next `C_channel_state_loop`; → `C_main_mixer_return` instead | 16, 16, 9, −2 |
| `MAIN_RETURN`, `DOWNSAMPLE`, `DOWNSAMPLE_PAIR`, `RETURN` | `C_main_mixer_return` → `C_downsampler`; its setup and exit; one `C_downsampler_loop` pass (2 samples; two `mul` at m 1); `C_downsampler_return` → `RET` | 23, 17, 67, 39 (measured in step 1; counts to 39) |
| `ENV_HEAD`, `ENV_IEC` / `_IEC_END`, `ENV_NOT_IEC` | not START: → `C_adsr_echo_check`; IEC: → `C_channel_vol_calc` / → `C_stop_channel`; → `C_adsr_release_check` | 15, 14 / 12, 7 |
| `ENV_RELEASE`, `_RELEASE_ZERO`, `_RELEASE_LOW`, `ENV_ECHO_NONE`, `_ECHO_START` | STOP: → vol calc; released to 0 (`ble`) / at or below the echo volume → `C_adsr_released`; → `C_stop_channel`; IEC set → `C_adsr_save_and_finalize` → vol calc | 16, 11, 14, 7, 12 |
| `ENV_NOT_STOP`, `ENV_DECAY`, `_DECAY_SUSTAIN`, `_DECAY_ZERO`, `ENV_NOT_DECAY`, `ENV_SUSTAIN`, `ENV_TO_ATTACK`, `ENV_ATTACK`, `_ATTACK_PEAK` | → `C_adsr_decay_check`; decay → vol calc / reaching sustain → `C_adsr_next_state` → vol calc / reaching sustain 0 → `C_adsr_released`; → `C_adsr_attack_check`; → vol calc; → `C_adsr_attack`; `C_adsr_attack` → vol calc below / at 0xFF | 4, 17, 23, 19, 6, 4, 2, 10, 10 |
| `STOP`, `START_STOP` | `C_stop_channel` → `C_end_channel_state_loop`; START\|STOP → `C_stop_channel` | 6, 16 |
| `INIT_FWD`, `_REV`, `_CMP_FWD`, `_CMP_REV`, `_SYNTH`, `_SYNTH_TRI`, `INIT_CHECK`, `INIT_FLAGS` (+`_LOOP`) | START: → `C_channel_init_check_loop` by `C_channel_init_noncomp_forward` / `_noncomp_reverse` / `_comp` + `_comp_forward` / `_comp_reverse` / `_synth`; the triangle → `_no_fine_pos`; `_check_loop` → `_no_fine_pos`; → `C_adsr_attack` (WaveData.flags from ROM) | 48, 51, 53, 53, 42, 48, 3, 19 (+2) |
| `VOL` (+`_LOOP`, `_LOOP_CMP`) | `C_channel_vol_calc` → `C_mixing_setup` (three `mul` at m 1, the `bx` to ARM); + the loop bounds (`ldmia` of two ROM words 8) | 48 (+19, +18) |
| `VOL_ZERO`, `TO_COMP_REV`, `TO_FIX`, `SETUP` (+ m + m), `SETUP_CMP`, `SETUP_UREV`, `SETUP_SYNTH` (+ m) | volume 0: → `C_mixing_epilogue` → the walk; → `C_mixing_setup_comp_rev`; → `C_setup_fixed_freq_mixing`; `_comp_rev` → `C_data_load_uncomp_for` (step `mul`, `umlal`); → `C_data_load_comp` / `_uncomp_rev` instead; → `C_setup_synth` | 14, 12, 14, 30, −2, −1, 14 |
| `END`, `END_STORE`, `END_STOP` | `C_end_mixing` → `C_mixing_end_store` → `C_mixing_epilogue` → `C_end_channel_state_loop`; from `C_mixing_end_store`; from `C_mixing_end_and_stop_channel` | 20, 8, 9 |
| `COPY`, `DMA3` + `DMA3_WORD` × n, `VLA`, `DIRECT` | count > advance: → `_vla_r3_and3` (the `stmia` that starts DMA3); the copy, n = (advance + 7) >> 2; → `C_select_highspeed_codepath`; no copy (advance ≥ 0x400 or no stack): → `C_select_highspeed_codepath` | 25, 4 + 5n, 2, 16 |
| `SELECT`, `PASTE`, `FAST_ENTRY`, `FAST_ITER`, `FAST_SAMPLE`, `LOAD_STACK` / `_ROM`, `MLA`, `FAST_EXIT` | → `C_skip_fast_mixing_creation`; + the block pasted (`previous_fast_code` differs: the engine keeps it, `hqFastCode`); → `C_fast_mixing_loop`; one pass outside its samples; a sample's nine instructions; an executed `ldrsb`'s extra from the stack / ROM; an executed `mlane`'s extra (+ m); after the last `bgt` → `C_end_mixing` | 16, 103, 9 (+ a load), 26, 9, 2 / 5, 1, 10 |
| `UNBUF`, `UNBUF_SAMPLE`, `_MIX`, `_ADVANCE`, `_ADVANCE_N`, `_WRAP`, `_END` | count ≤ advance: → `C_unbuffered_mixing_loop`; a pass (no advance, output 0, + the `mul`'s m); + output ≠ 0 (+ m); + an advance by one / more; + a loop wrap; the end without a loop (to `C_mixing_end_and_stop_channel`) | 12, 15, 4, 11, 5, 11, 13 |
| `FIX_SETUP`, `FIX_CHECK`, `FIX_NOBLOCK`, `FIX_BLOCK`, `FIX_NONZERO`, `FIX_REWIND`, `FIX_REST`, `FIX_SAMPLE`, `FIX_WRAP`, `FIX_END`, `FIX_NEXT`, `FIX_DONE` | `stmfd`; `C_fixed_mixing_length_check` → `C_fixed_mixing_loop` (the code paste, the first word) / → `_process_rest`; a 4-sample block (one ROM word, `mlane` not executed); + a nonzero sample; `sub r3`; `mov r1`; an unaligned-loop pass; + a wrap; + the end; → `_length_check`; → `C_mixing_end_store` | 3, 42, 7, 30, 2, 1, 1, 20, 12, 8, 4, 6 |
| `COMP`, `COMP_REV`, `COMP_FOR` (`_END`), `COMP_REV_SETUP` (`_END`), `STACK_SKIP` (`_REV`) | `C_data_load_comp` → `_comp_for` / `_comp_rev` (the store code pasted); → `C_data_load_comp_decode` (through `F_clear_mem` at the end); the stack check failing → `C_end_mixing` | 12, 14, 22 (29), 27 (32), 13 (14) |
| `DECODE_SETUP` (+ m, + 1), `BLOCK`, `BLOCK_OPENBUS`, `DECODE_DONE` | → `C_data_load_comp_loop` (+ the block `mla`'s m, + the `streqb` of the end frame); one block: `bl`, `F_decode_compressed` 748 (33 ROM byte loads), `subs`, `bgt`; a block read from open bus (33 loads of `GBALoad8`'s default case, 3 instead of 6); → `C_select_highspeed_codepath` | 11, 755, −99, 4 |
| `UREV` (`_END`), `UREV_WORD`, `UREV_DONE` | `C_data_load_uncomp_rev` → its loop (through `F_clear_mem` and the status store); a word (`ldmia` of one ROM word 4, the byte swap); → `C_select_highspeed_codepath` | 22 (32), 14, 6 |
| `CLEAR`, `CLEAR_STORE`, `CLEAR_RET`, `CLEAR_WORD` | `F_clear_mem` to its computed jump; one 4-word `stmia` ((r1 >> 4) & 3 of them first, then passes of 4 + `subs`, `bpl`); `ands`, the `ldmeqfd` with pc; a remainder word | 16, 5, 11, 6 |
| `FIFO_PRE`, `FIFO_FIRST` (+ p), `FIFO_WORD` (+ (p − 8) mod 5), `FIFO_LAST` | a FIFO refill due p cycles from the end of the `stmia` that starts a DMA3 copy (halt L): p −3 … −1; 0 … 7; 8 … L − 2; L − 1 (18 outside −3 … L − 1) | 16, 9, 12, 17 (mgba's scheduler, below) |
| `SYNTH`, `PULSE_SETUP`, `PULSE_ITER`, `PULSE_EXIT`, `SAW`, `SAW_ITER` + 4 × `SAW_SAMPLE`, `TRI`, `TRI_ITER` + 4 × `TRI_SAMPLE`, `SYNTH_EXIT` | `C_setup_synth`, pulse; → `C_synth_pulse_loop`; a pass (8 samples); → `C_end_mixing`; → `C_synth_saw_loop`; a pass (4 samples, + `MLA` + m per executed `mlane`); → `C_synth_triangle_loop`; a pass (+ m per `mla`); the `b C_end_mixing` | 8, 37, 55, 9, 16, 15 + 24, 16, 15 + 20, 3 |

**The FIFO refill inside a DMA3 copy.**  A refill due while the fast
path's DMA3 copy runs does not cost 18.  The engine records each copy
(`hqDma3Start`, `hqDma3Len` on `mixCycles`' true clock) and
`m4a_engine_mixer_stall` charges a refill due p cycles after the copy's
start (the `stmia`'s end, moved on by the span's earlier stalls), the
copy halting the CPU for L = 4 + 5n: 16 at p −3 … −1, 9 + p at 0 … 7, 12
+ (p − 8) mod 5 at 8 … L − 2, 17 at L − 1, 18 otherwise
(`M4A_HQMIX_FIFO_*`).

- **Derived from mgba's code**: the whole rule, from a simulation of
  0.10.5's `dma.c` / `timing.c` (`GBADMAService`, `GBADMAUpdate`,
  `_dmaEvent`, the timer-0 event at priority 0x20 before the DMA event's
  0x40): the `stmia`'s store to DMA3CNT schedules DMA3 at the
  instruction's start + 3 (the CPU's cycle count is not yet advanced),
  i.e. p −1; events due inside the `stmia` are dispatched at its end in
  (when, priority) order, so a refill due at p 0 comes after DMA3's
  first word (9) and one due at p −3 … −1 before the whole copy (16);
  `_dmaEvent` re-anchors the first word at `now`; DMA3 runs a word at a
  time (7 cycles, then 5, 2 more after the last), DMA1 / DMA2 preempt it
  between words.  The same simulation gives the copy's halt 5n + 4 from
  the `stmia`'s end, the measured `DMA3` + `DMA3_WORD` × n.  No offset
  is taken from the corpus.
- **Measured** (the corpus, 255 frames at 13379 Hz and 263 at 21024 with
  a refill inside or just before a copy, each matched): p −3, −2, −1 (16),
  0, 2, 4, 7 (9 + p), every residue of the word region, and the
  boundaries on both sides (p ≤ −4 and ≥ L: 18).  Charging p 0 as 16 or
  p −3 as 15 (an earlier, simpler simulation without the in-`stmia`
  dispatch) breaks `vs_trainer` 21024 f353 by 7 and two frames by 1.
  Before the rule: up to 6 cycles high, the p mod 5 pattern exact.
- **Unverified**: p L − 1 (17), the simulation's only (no corpus frame).

Unit-tested (`test_frame_cost_mixer_stall`): every boundary of a 20-word
and a one-word copy, an earlier stall moving a copy on, vanilla mode
ignoring the record, and the exit bound (due at `RET` + 2 counted, `RET`
+ 3 not).

**Gate** (`cost_diff.py --corpus`, every frame's total within 2 % or 300
cycles of the probe's `tot`):

| corpus | tests | frames gated | max \|Δ\| | p99 | frames within 1 % |
|---|---|---|---|---|---|
| HQ 13379 (`build/probe-hq`) | 83 | 19041 | **0** | 0 | 100 % |
| HQ 21024 (`build/probe-hq-21024`) | 83 | 19041 | **0** | 0 | 100 % |
| vanilla 13379 / 21024 (again, with `EXIT_BX` 8) | 79 / 79 | 18333 / 18333 | 0 / 0 | 0 | 100 % |

Every column matches.  Not gated: the two `cry_third_*` rows (no third
cry player).  The HQ corpora do not exercise (charged as counted, their
wiring unit-tested in `test_frame_cost_hq_wiring` /
`test_frame_cost_hq_paths`): START\|STOP, the pseudo-echo countdown, a
release into the echo, an attack step after the first frame, a looping
compressed sample's loop setup, the stack check of the reversed paths
(the forward DPCM one is: `pcm_cmp_hi`), a FIX voice with REV, a DPCM
block index past 255 (the `mla`'s m 2), the FIX count-0 host guard and
`M4A_HQMIX_FIFO_LAST`.  The 15768 / 42048 scratch checks of step 2,
rerun with `EXIT_BX` 8 (review of step 3): 15768 13/13 PASS, max |Δ|
still 18 (`rg_vs_trainer` f595, engine mixer 41233 vs the probe's 41215:
the known `RET` + 2 frame mgba does not count, now on the counted side of
the bound); 42048 13/13 at 0.

**Residual sources (HQ).**  The vanilla ones (mgba's servicing order at
the exit boundary: the one `RET` + 2 frame in 4 that mgba does not
count; hardware vs mgba), and: the stack budget follows
`engine->hqStackSp` (the test ROM's sp; a game's call depth at the
interrupt moves the copy / skip decisions); `ENABLE_DMA` 1 and
`ENABLE_REVERB` 1 as the wiki installs the mixer (the CPU-copy variant
and a reverb-less build cost differently and are not modeled);
`previous_fast_code` starts invalid after `m4a_engine_set_hq_mixer`
(the ROM after `m4aSoundInit`), so a mixer switched on mid-song pastes
once on its first frame like the ROM's first; the open-bus decode
assumes mgba's `GBALoadBad` timing.

`tools/probe_corpus.sh` regenerates the logs (`build/probe`,
`build/probe-hq`, `build/probe-21024`, `build/probe-hq-21024`, each with
its own `syms.nm`); `cost_diff.py --summarize <dir> --render` renders the
`--pcm-log`s it needs and prints the tables above;
`cost_diff.py <.frames> <cost log>` is the acceptance diff against
`poryaaaa_render --cost-log` (`frame seq cgb mixer total`; every frame's
total within 2 % or 300 cycles), per column; `cost_diff.py --corpus <dir>`
renders every probed test with `--cost-log` and gates the whole directory
(one row per test: frames, max |Δ| and p99 on total, PASS / FAIL).

### MIDI-path sequencer estimate

Without an `M4ASequencer` (the plugin, porydaw, `poryaaaa_render --midi`)
no song data exists for MPlayMain to run, so the published sequencer term
is an **estimate from the events the engine applies** (cpu-meter step 4;
`plugin/m4a_seq_estimate.c`, hooks in `m4a_engine.c`).  It charges the same
`M4A_CYC_*` segments `m4a_sequencer.c` sums, by the same branch rules,
deciding each branch from engine state instead of song bytes; nothing is
fitted.  Integer adds in the tick and short loops over the 19 real
channels; no allocation.  `frameCost.sequencerEstimated` marks such a
frame.  A timing-only sequencer (`--gba-tempo-grid`) still publishes its
exact term unless `m4a_engine_set_cost_estimate_forced` (renderer
`--cost-estimate`) asks for the estimate; an `M4ASequencer` never uses it.

**What is charged, from what.**

| piece | charge | decided by |
|---|---|---|
| frame skeleton | the player's entry / head / exit (`MP_BGM_ENTRY`, `MP_HEAD`, `MP_EXIT`; SE players by `m4a_sequencer_set_player`'s rule), or the paused player (822 for BGM) | `m4a_engine_cost_song_start` (player), whether any track exists |
| tick passes | per tempo tick `TICK_ENTER`, the trackCount slots (`TICK_TRACK` + `TICK_TRACK_END` per existing track, `TICK_NO_TRACK` per empty slot, `TICK_NEXT` / `TICK_LAST`), `TICK_EXIT`; `TICK_ALL_DONE` and then paused once every track has ended | the engine's own tempoC / 150 loop (`mplay_main_tail`); the existing tracks |
| chain walk | per existing track and tick, its chained channels newest first, those started before the tick: dead → `WALK_DEAD` + the unlink by links (`WALK_UNLINK_*`); the tick its N## gate expires (start + length, known at the note-off) → `GATE_EXPIRE`, after it `GATE0`, before it `GATE_DEC`; released otherwise, or a TIE (predicted, ended by EOT, or older than 96 ticks) → `GATE0`; else `GATE_DEC`; `TICK_CHAIN` + `NEXT` / `END`, or `TICK_NO_CHAIN` | the estimate's own chains (membership and order by note-on, like the sequencer's `chainOwner` / `chanSeq`) over the engine's channel status |
| START | `TICK_START` on a track's first tick, else `TICK_NO_START` | the song start / a track's first note |
| wait / LFO path | per existing track and tick, the branch the LFO step takes: `WAIT_LFOS0`, `WAIT_NO_LFO`, `WAIT_LFO_DELAY`, `LFO_STEP` + `NEG` / `POS` + `SAME` / `PIT` / `VOL` | the engine's `m4a_lfo_tick` (lfoSpeed, mod, lfoDelayC, modM) |
| W## | 93 per track on a tick with any of its events, on mid2agb's bar marks, and 96 ticks after its last one (TimeSplit) | events, `m4a_engine_cost_time_signature`, the clock |
| VOLCHG / PITCHG pass | `APPLY_*` per slot: flagged tracks `FLAGGED` + `TVPS` + VOLSET / PITSET, then per chained channel `CH`, VOL (`_PCM` / `_CGB`) or not, PIT (`_PCM` / `_CGB` / `_NOISE` + the low-key / noise-clamp adjustments) or not, dead `CH_DEAD` | the engine's `chgFlags` before its own pass, plus the flags of commands it never sees (KEYSH, the implicit VOL); keyM from TrkVolPitSet on a copy |
| note-on | the command byte (running status only when key / velocity / gate bytes follow, mid2agb's last op is the same N## and a wait came between: 25, else explicit 21 + 10; a repeat with the same key and velocity is the bare explicit command), `CMD_NOTE`, ply_note's bytes (key when key or velocity changed, velocity when it changed, the `-E` gate byte when the length is not a gClockTable value), voice (plain / keysplit / drum, the ROM-voice extras), the CGB test or the DirectSound scan channel by channel over the engine's channels as the note finds them, `NOTE_TAKE` + the unlink (`NOTE_UNLINK_*` by links or `_NONE`) + `CHAIN_EMPTY`, `LFODL0` / `LFODL` (clear_modM), `NOTE_TVPS` + the pending flags, `CHNVOL`, the frequency routine (PCM / CGB / noise + key adjustments + sweep), `STORE` + `EXIT`; a rejected note `EXIT` | the note's length predicted from the track's previous note (mid2agb writes it at the note-on; the real one is known only at the note-off, which then corrects the running-status state); the 4-step velocity table |
| note-off | no command for an N## note (its gate expires in the walk); a note longer than 96 ticks was a TIE: EOT (`CMD_TABLE` + `ENDTIE` + key byte + the walk to the match) | the note's length in ticks |
| controllers | `CMD_TABLE` + the handler (VOL 89, PAN 91, BEND 91, BENDR 89, LFOS 69 / 109 at 0, LFODL 81, MOD 69 / 109 at 0, MODT 91 / 95 changed, TUNE 91, VOICE 184, xIECV / xIECL `XCMD_SUB` + `XIEC`, PRIO the default), explicit or running status by mid2agb's last op | the CC number and value, the old modT |
| song structure | KEYSH and the START per track at the song start, the implicit `VOL 127` when no VOL came before a track's first note, TEMPO (`m4a_engine_set_tick_tempo`, or a changed `m4a_engine_set_tempo_bpm` while a song plays; once per tick), FINE (`m4a_engine_track_end`, with ply_fine's chain), the loop's GOTO per track and the labels' compression reset (`m4a_engine_cost_loop_point`) | the host's calls |
| FIFO stall | 18 per refill due in the span, from the player's entry | the CPU clock |

A channel a command takes from a chain (a note stealing it, FINE) was
already met by its owner's walks of the frame's earlier ticks, and of
this tick when the owner is the same or an earlier track (MPlayMain walks
and runs track by track): those steps are charged then.  On a frame with
several ticks (the renderer's tick boundaries) each event, channel start
and gate expiry belongs to its own tick.  **Events in a frame without a
tick** (a free-running accumulator below 150 BPM, events between its
ticks) are held, cost and W## alike, for the next frame with a tick, where
MPlayMain would read them.  All costs are ROM-song values (a song from MIDI
is in ROM).

**Two forms.**  *Declared*: the host calls `m4a_engine_cost_song_start`
with the song's tracks and player, then gives the time signatures and the
loop points, so the song exists from its first tick with mid2agb's bar
marks (every 96 ticks until a time signature says otherwise), GOTOs and
labels; only the declared tracks' events are commands (mid2agb drops a
channel without notes), and a note on another track adds it.  *Inferred*
(a host that calls none of them, e.g. the plugin): a track exists from its
first note (with its START and KEYSH then), the player is BGM (an SE plays
on fewer slots), there are no bar marks, GOTOs or labels, and a
controller of any track is a command; `m4a_engine_all_sound_off` (a stop)
ends the inferred song, so the engine shows the paused player again.  In
both forms `m4a_engine_set_voicegroup` with a new voicegroup and
`m4a_engine_cost_song_stop` reset the estimate to the paused player with
no song state, and all sound off forgets the chains and held notes.

**The host contract** (`m4a_engine.h`; every call on the thread that
drives the engine, like the MIDI events, between `m4a_engine_process`
calls): `m4a_engine_cost_song_start(trackMask, player)` on play-from-stop
(and after a seek that restarts the song), with the engine tracks that
have notes anywhere in the song (not only those heard so far: a track
starting late exists in the game from tick 0) and `M4A_PLAYER_BGM` for
music; `m4a_engine_cost_time_signature(96 · num · clocksPerBeat /
2^den)` at tick 0 and at each time signature change, as the event of
that tick; `m4a_engine_cost_loop_point(false)` at the loop start and
`(true)` at the wrap, before the next pass's events of that tick;
`m4a_engine_set_tempo_bpm` with the song's tempo (it is also the tick
rate); `m4a_engine_cost_song_stop` on stop.  Not calling them is the
inferred form.

**Assumptions.**  mid2agb with `-E` and compression on (the vanilla
`midi.cfg`); one AGB track per engine track (a Type 1 MTrk with one note
channel); a tick's controllers before its notes; priority 0 (no PRIO
handler); the vanilla player table.  The note length at a note-on is the
previous note's, so the first note of a track and a length change cost the
running-status (6) and gate-byte (15 + the key / velocity bytes it forces)
difference.  Placement approximations: the implicit `VOL 127` (mid2agb
writes it at the track's head, tick 0, before KEYSH) is charged at the
track's first note, so on that note's frame when the first note is
later; a host's tempo change is charged as TEMPO on the tick it reaches
the engine.

**Measured on the game's grid** (2026-10-29, `tools/seq_estimate_diff.py`,
`build/probe`: every probed test with a MIDI rendered `--midi
--gba-tempo-grid --cost-log` twice, with and without `--cost-estimate`;
the declared form; frames 3 to the probe's last; cycles):

| test | frames | mean | mean \|d\| | max \|d\| | p50 | p90 | p99 | p99 % of total |
|---|---|---|---|---|---|---|---|---|
| credits | 596 | −0.1 | 1.8 | 18 | 0 | 6 | 6 | 0.04 % |
| groudon | 1196 | −3.5 | 3.5 | 1151 | 0 | 0 | 0 | 0.00 % |
| littleroot | 1196 | −1.3 | 1.3 | 386 | 0 | 0 | 12 | 0.02 % |
| mt_chimney | 1196 | −6.8 | 6.8 | 1258 | 0 | 6 | 30 | 0.06 % |
| rg_vs_trainer | 1196 | −7.1 | 10.0 | 1330 | 0 | 12 | 93 | 0.20 % |
| vs_trainer | 1196 | −11.3 | 11.3 | 1446 | 0 | 0 | 332 | 0.61 % |
| vs_wild | 1196 | −15.7 | 15.7 | 1749 | 0 | 0 | 978 | 1.68 % |
| **songs** | 7772 | −7.0 | 7.6 | 1749 | 0 | 0 | **36** | 0.08 % |
| SEs (6) | 1056 | 0.5 | 0.6 | 24 | 0 | 0 | 12 | 0.13 % |
| micro-tests (38) | 4378 | −0.0 | 0.0 | 6 | 0 | 0 | 0 | 0.00 % |
| **all** | 13206 | −4.1 | 4.5 | 1749 | 0 | 0 | 18 | 0.07 % |

Against the probe (mgba's CgbSound entry − m4aSoundMain entry) the table
is the same frame for frame: the timing-only sequencer's term equals the
probe's on every one of these frames.  `--quantize` (the plugin's latch)
gives the same numbers.  The micro-tests are exact but for single 6-cycle
running-status guesses; the SEs but for `se_exp`'s first note (a TIE the
estimate cannot know for its first 96 ticks: `GATE_DEC` 26 instead of
`GATE0` 20 per tick) and one frame of `se_contest_place`.

**Residual sources** (the songs; `--seq-log` of the exact render names
each frame's commands):

- **PATT / PEND** (mid2agb's compression of a repeated bar: `PATT` 237 at
  the repeat's start, `PEND` 137 at the end of both bars): all of the
  large misses.  On the frames that run them the residual is −17858 /
  −12769 / −6764 / −8983 / −945 / −4148 (vs_wild, vs_trainer, mt_chimney,
  rg_vs_trainer, littleroot, groudon) against their PATT / PEND cost of
  16778 / 12079 / 6628 / 8750 / 885 / 4051; the rest is the compression
  reset at the pattern labels (explicit command, key and velocity bytes
  the estimate predicted as running).  Not causal: a bar is a pattern only
  once its repeat is known.  These are 4–14 frames in 1196.
- **mid2agb's LUT wait split** (a wait not in gClockTable is two W##, the
  second mid-wait): −93 on a frame with no event.  Not causal (the split
  point depends on the next event).
- **The note length guess** (running status ±6, the gate byte 15, the key
  and velocity bytes 19 / 17 it forces) and the scan / unlink differences
  of channels the two models hold differently.  Together with the split:
  on the frames without PATT / PEND the songs' p99 is 0–36, max 93.
- **TIEs before 96 ticks**: +6 per tick.

**Without the grid** (a plugin's situation: the MIDI's own tempo, no
timing-only sequencer, MIDI time 0 at sample 0; `seq_estimate_diff.py
--no-grid`).  Frames do not line up with the game's, so per test the
distributions of the sequencer term over frames 3 to the probe's last are
compared: the mean's difference and W1, the mean |difference| of the two
sorted series (the earth mover's distance of the per-frame values).
Declared is the renderer's default (song start with the tracks that have
notes, the MIDI's time signatures and loop points: the form porydaw uses
from cpu-meter step 5, with its own time signatures and loop points);
inferred is `--cost-inferred`.

| song | probe mean / p50 / p99 / max | declared mean / p50 / p99 / max (Δmean, W1) | inferred mean / p50 / p99 / max (Δmean, W1) |
|---|---|---|---|
| credits | 2110 / 2365 / 5129 / 20696 | 2107 / 2345 / 5155 / 18716 (−2.9, 16.6) | 1839 / 1937 / 4757 / 14606 (−271.4, 270.0) |
| groudon | 1778 / 1817 / 4470 / 10518 | 1777 / 1817 / 4470 / 10518 (−0.9, 7.8) | 1776 / 1817 / 4470 / 10876 (−2.0, 6.7) |
| littleroot | 2709 / 2361 / 8526 / 22510 | 2715 / 2361 / 8530 / 21190 (+5.6, 23.6) | 2511 / 2089 / 8304 / 18824 (−198.5, 215.2) |
| mt_chimney | 3554 / 2998 / 11449 / 25374 | 3552 / 3016 / 11339 / 25546 (−1.6, 45.4) | 3483 / 2947 / 11271 / 25724 (−70.1, 89.7) |
| rg_vs_trainer | 3693 / 2548 / 12554 / 23988 | 3701 / 2564 / 11816 / 23988 (+8.2, 64.6) | 3601 / 2494 / 11850 / 22196 (−91.9, 118.3) |
| vs_trainer | 3101 / 2319 / 9696 / 17899 | 3104 / 2325 / 9167 / 17899 (+3.3, 60.4) | 3055 / 2319 / 9167 / 16599 (−45.3, 74.7) |
| vs_wild | 3164 / 2368 / 10023 / 15735 | 3165 / 2400 / 9266 / 15735 (+1.0, 65.8) | 3053 / 2314 / 9148 / 13561 (−110.5, 129.7) |

Per group (the mean over the tests of Δmean, of |Δmean| in % of the
probe's mean, and of W1):

| group | declared Δmean / \|Δmean\| % / W1 | inferred Δmean / \|Δmean\| % / W1 |
|---|---|---|
| songs (7) | +1.8 / 0.11 % / 40.6 | −112.8 / 4.25 % / 129.2 |
| SEs (6) | −14.5 / 1.20 % / 23.4 | +171.0 / 14.34 % / 190.6 |
| micro-tests (38) | −13.8 / 0.86 % / 10.3 | −13.7 / 0.86 % / 10.9 |

Declared, the songs' means agree within 0.3 % and the tails are only short
of PATT / PEND; the micro-tests' −14 is the window (without the start
delay the render's song ends three frames earlier, which adds three
paused frames).  Inferred lacks the bar-mark W##s, the START and KEYSH of
tracks before their first note, the GOTOs and labels, and an SE's player
(BGM's 10 slots instead of 1–9: `se_low_health` +563 per frame): about
4 % low on songs and well off on SEs.  The sequencer term is a few
percent of the frame's total (the mixer dominates), so even the inferred
form's error is under 1 % of VBlank on most frames.

**Exactness.**  The estimate only publishes a number: with a sequencer
attached nothing changes (`cost_diff.py --corpus` on all four probe
corpora: max \|Δ\| 0, no probe-only or engine-only frame), the PSG writes
stay where the fixed entry puts them (`claim_check.sh`, vanilla and HQ: 0
moved writes, no harness row moved), and a `--midi` render is the same
sample for sample with and without the renderer's cost events
(`RENDER_EVT_TIMESIG`, `RENDER_EVT_LOOP`).  Unit-tested
(`test_seq_estimate`): the paused player, the no-tick and tick skeletons
(1217 and 1655 with one track), a note-on's sum, the walk's gate
decrement and expiry, a controller's command, running status, the
identical-repeat note (explicit), a command held from a frame without a
tick, an undeclared track's controller, TEMPO from the host's tempo
(once per tick), the two-tick frame (a note on the second tick, a note-off
expiring there), the inferred song and its end at all sound off, the
reset on a new voicegroup and at song stop, the flag, and that a
sequencer (forced or not) or a timing-only sequencer keeps its exact term.

`tools/seq_estimate_diff.py [--probe DIR] [--out DIR] [--jobs N]
[--quantize] [--worst N] [--no-grid] [name ...]` reproduces the tables
(`--worst N` lists the N largest frames per test).

## 5. The MIDI path

The plugin, and `poryaaaa_render --midi`, drive the engine from MIDI
events instead of a compiled song.  Some of the ROM's behaviour can be
computed from what the engine sees and applies to every source; some needs
knowledge only an offline renderer has (where the song's ticks fall,
mid2agb's byte encoding, where a track ends) and is the renderer's alone,
under `--gba-tempo-grid`.

### Entry points and the frame latch

The entry points are `m4a_engine_note_on` / `_note_off` / `_cc` /
`_program_change` / `_pitch_bend` / `_xcmd`, plus the renderer-only
`m4a_engine_track_end` and `m4a_engine_tick_boundary`.  MIDI controllers
map to the m4a commands mid2agb would write: CC 7 VOL (× song volume /
127), CC 10 PAN (v − 64), bend MSB (`((MSB<<7|LSB) − 8192) >> 7`), CC
0x14 BENDR, CC 1 MOD, 0x15 LFOS, 0x16 MODT, 0x18 TUNE, 0x1A LFODL, CC 5
portamento (opt-in), 0x17 / 0x19 PWM (opt-in), and XCMD as mid2agb's
two-CC encoding (CC 0x1E latches the sub-command, CC 0x1D / 0x1F carries
the value: 0x08 xIECV, 0x09 xIECL).

**The latch** (`engine->quantizeEvents`, `m4a_engine_set_quantize_events`,
renderer `--quantize`): with it on, an entry point called outside a tick
queues an `M4AQueuedEvent` (capacity 1024; a full queue is drained in
order first) and the next tick's drain applies the queue in arrival order,
after the gate walks and before the LFO steps and CgbSound — where
MPlayMain's commands land.  It exists because a host applies events
mid-frame between sub-block renders; the GBA can only start a note on a
frame edge.

- **One-tick notes**: a latched note-off that finds its channel still
  START sets `gateTime = 1` instead of STOP, so the next frame's gate walk
  stops it (the GBA's one-tick note).  Unlatched, a note-on and its
  note-off in one frame give START | STOP and the mixer kills the note
  unheard.
- `m4a_engine_all_sound_off` outside a tick drops the queue (a panic must
  not release queued note-ons next frame).  Turning the latch off drains
  first.  Tempo is not latched.
- On the renderer the latch is harness-neutral: an event dispatched
  between two SoundMains is applied before the second one either way.

### Rules for every source (the plugin included)

- **Frame order**: without a sequencer, the frame's LFO steps, the VOLCHG
  / PITCHG pass, portamento and PWM run after the latched drain and
  **before CgbSound** (`mplay_main_tail`), as MPlayMain precedes CgbSound in
  SoundMain.  An LFO step of frame F reaches the PSG at frame F's CgbSound;
  PCM reads its frequency and volumes in SoundMainRAM, after all of it.
- **Per-tick command batching** (the ROM's flag model):
  - The handlers only flag the track (`M4ATrack.chgFlags`:
    `M4A_TRK_VOLCHG` 0x03, `M4A_TRK_PITCHG` 0x0C, the MPT_FLG values): VOL
    and PAN → VOLCHG; bend, BENDR, TUNE → PITCHG; MODT on a change → both;
    MOD 0 / LFOS 0 → `clear_mod_m` (modM = 0, PITCHG for modT 0, else
    VOLCHG); nonzero MOD / LFOS and LFODL set nothing.  The LFO step flags a
    changed modM.
  - A note that takes a channel computes its own volume and pitch from the
    track's current fields and drops the pending flags
    (`note_took_channel`, ply_note's `flags &= 0xF0`); a rejected note
    keeps them.  With an LFO delay the note restarts it (lfoDelayC =
    lfoDelay, clear_modM) for the new note only.
  - `track_change_pass`, once per frame after the LFO steps, applies what
    is still flagged to every channel the track owns, sounding or
    releasing.
  - So a controller reaches a channel at its own note start if it precedes
    the note in the frame, and at the frame's pass only if no note took a
    channel on that track after it in the frame.  In the plugin: a
    controller and a note-on on one channel within one frame (about
    16.7 ms) reach only the new note.  mid2agb (and every vanilla MIDI)
    puts a tick's controllers before its notes; the order inside a tick is
    otherwise the host's.
- `m4a_engine_set_song_volume` applies at once (it is not a song command).

### The renderer's "as the game" mode

`poryaaaa_render --midi` with **`--gba-tempo-grid`** (off by default in
the renderer, on in the harness) plays a MIDI the way the game plays
mid2agb's conversion of it.  On the corpus this form is the `--song` render
sample for sample.  Checked 2026-10-27 on mus_littleroot: bit-identical
over 10 s; with the origin left out (`--no-gba-start-delay`) the render is
about 50 ms early and matches at no shift, because the first frames' ticks
collapse onto frame 3 and the LFO runs 3 frames ahead of the ticks.  The
output's first 63 ms are the sound hardware's boot in every render, so the
origin adds no leading silence.

- **Start origin** (on with the grid; `--no-gba-start-delay` leaves it out,
  `--gba-start-delay` adds it to an exact-tempo render for comparing with an
  emulator recording): MIDI time 0 is placed where the
  ROM starts the song, frame 3's SoundMain at `ROM_FIRST_TICK_CYCLE` =
  124773 + 2 · 280896 + 160 · 1232 + 197 = 883882 cycles.  Every event is
  dispatched `midiOrigin` = floor((883882 − 2048) · rate / 2²⁴) samples
  later (2522 at 48 kHz): the last sample at least one resampler frame before
  that SoundMain, because the machine runs up to a resampler frame past a
  sample's cycle when the sample is dispatched (the nominal 2529 would put
  every first note one frame late).  The fade and natural tail move with
  it; an explicit `--total-duration-seconds` does not.
- **Tempo grid** (`--gba-tempo-grid`, `cmd/gba_tempo_grid.{h,c}`): each
  MIDI tick is played on the frame MPlayMain's accumulator plays it.
  - mid2agb's rules: tempoI = 2 · floor(round(60e6 / µs) · tbs / 2) (the
    TEMPO byte doubled by ply_tempo; tbs = 1, or 2 with `-X`; an odd BPM
    loses one; a byte that wraps to 0 stops the song); clocks =
    24 · tbs · tick / division (truncated); tempo events come from the
    first MTrk only, and a song without one runs at 150 until its first
    TEMPO.
  - The rule: grid frame 0 = ROM frame 3, tempoC 0, tempoI 150 (MPlayStart);
    grid frame j adds the tempoI in force after frame j − 1's ticks, then
    plays ticks while tempoC ≥ 150.  At 120 BPM tick n is on grid frame
    ceil(1.25 n); the game is 0.46 % slower than the MIDI's tempo map
    (59.7275 Hz VBlank).
  - Every event of grid frame j is dispatched at
    `gba_tempo_grid_sample(j)` (the same resampler-frame guard); several ticks
    of one frame share one sample, in order.  Loop passes add whole clocks
    (`gba_tempo_grid_pass_clock`); the loop-count fade and the natural tail
    move onto the grid.
- **The engine's LFO clock on the grid**: `m4a_engine_start_song_tempo`
  (MPlayStart's seed, at the origin) and `m4a_engine_set_tick_tempo(tempoI)`
  (a TEMPO played in the coming tick, stored after the next frame's add,
  as ply_tempo's store lands after `_081DD86C`).  The renderer passes the
  grid's tempoI for first-MTrk tempos and nothing for the others.  Needs
  the start origin too (the grid's default).
- **The CgbSound entry**: the renderer converts the MIDI with mid2agb
  itself (`third_party/mid2agb`, vendored from pokeemerald, MIT; built as a
  library with `RaiseError` throwing and state reset between conversions;
  its text is identical to the harness's `.s` on all 560 MIDIs that have
  one), options `-E -V<vol> [-X]`, and runs a **timing-only sequencer**
  (`m4a_sequencer_init_timing`) over that song: MPlayMain's whole control
  flow and every cost, on its own copies of the channel and track state,
  with channel liveness taken from the MIDI engine at the end of each
  SoundMain (`frameEndHook`).  It drives nothing; it publishes
  `cgbEntryCycles`, the ROM's entry on every frame.  With the grid,
  `--player` and `--seq-log` apply on `--midi` too.  If mid2agb rejects the
  MIDI the fixed entry stays, with a warning.
- **Track ends** (every `--midi` render of a song without a loop):
  `m4a_engine_track_end` (latched, `M4A_EV_TRACK_END`) is ply_fine: STOP
  and `M4A_NO_TRACK` on the track's channels, its pending changes dropped,
  the track `finished` (no LFO step) until its next note.  The renderer
  sends it for every channel with notes in each MTrk at the later of that
  MTrk's end-of-track and the first MTrk's (mid2agb's FINE), after its own
  MTrk's events of that tick and before the next MTrk's.  Track ends never
  lengthen a render.
- **Ticks within one frame**: in a frame with two ticks the ROM runs, per
  track, tick n's commands, its LFO step, tick n + 1's commands, its step,
  then the pass.  (Running every track's tick-n events, then every tick-n
  step, then tick n + 1 gives the same state: a step touches only its own
  track.)  The renderer sends `m4a_engine_tick_boundary` before the first
  event of each later tick of a frame (`gba_tempo_grid_tick_in_frame`,
  `RenderEvent.gridTickInFrame`); applied (latched as
  `M4A_EV_TICK_BOUNDARY`), it runs the step of the tick that ended and
  counts it (`lfoTicksAhead`), and the frame's tick loop skips the counted
  ones.  The order matters for a MOD or LFOS from 0, an LFOS change, a note
  restarting an LFO delay, or a note on a vibrato track, in the later tick.
- The renderer's MIDI handling otherwise: Type 0 and 1 files, running
  status, tempo changes, loop markers (a text / marker meta event with `[`
  / `]`), `--loop-count` (default 2), `--fadeout`, `--total-duration-
  seconds`.  Engine track = SMF track number (Type 0: the channel).

Every one of these renderer-only calls is a no-op while a sequencer drives
the engine.

### The plugin

`plugin/m4a_plugin.c` (CLAP) uses the latch and the every-source rules
above, and none of the renderer-only mechanisms:

- **Latch policy** (`M4ALatchMode`: `M4A_LATCH_TRANSPORT` 0, the default,
  `ON` 1, `OFF` 2; `m4a_engine_apply_latch_mode`): latched while the host
  transport plays (an honest preview of recorded parts), unlatched while it
  is stopped (live playing without frame jitter), or forced either way.  On
  the playing → stopped edge, in every mode, the queue is drained and then
  portamento history is reset.  Live events are latched like the rest while
  playing.  Settings: GUI Options → "Quantize to GBA Frames" (Follow
  Transport / Always / Never), config `quantize_events=transport|on|off`
  (or 1 / 0), one CLAP state byte after the PCM-rate float (absent or > 2
  loads as transport).  The mode byte is written on the main thread and
  applied on the audio thread at the next block.
- **Note choke** (`m4a_engine_note_choke`) bypasses the latch: it removes
  matching queued note events and kills matching channels at once
  (negative track or key = wildcard).
- **Tempo**: the host tempo per block through `m4a_engine_set_tempo_bpm`
  (tempoI = round(bpm), at once); the engine's accumulator runs from the
  boot, not phase-locked to the host's beat grid.
- **Cost model**: the sequencer term is the MIDI-path estimate in its
  inferred form (no `m4a_engine_cost_song_start`: a track exists from its
  first note, BGM, no bar marks, all sound off ends the song); see
  "MIDI-path sequencer estimate".
- What the plugin cannot have, and why: the tick grid and the start origin
  (the host's timeline is the clock), the timing-only CgbSound entry (a
  live stream does not carry mid2agb's encoding, part of which is not even
  causal: N## vs TIE is known only at the note-off, a PATT only once its
  bar repeats), track ends (a MIDI stream never says where a track ends),
  tick boundaries (nothing says which of a frame's accumulator ticks an
  event belongs to).  So the plugin plays the song at the host's tempo
  (0.46 % faster than the game), with the fixed CgbSound entry (PSG writes
  a few hundred to a few thousand cycles early or late: phase, not timbre)
  and a frame's events before its LFO steps.  The harness's "aligned" pair
  (`--no-gba-tempo-grid`) is the stand-in for this situation.

### What the MIDI path does not model

mid2agb's velocity table (a no-op on 4-step velocities), its note-length
conversion without `-E`, `-P` (the song priority, which within one player
changes no steal) and the PRIO command (CC 0x21 / 0x27: the engine has no
handler, so MIDI-path tracks have priority 0), tempo events outside the
first MTrk, and the host's order inside one tick.

## 6. Known residuals and limitations

- **The PCM floor** (mgba scheduler lateness, section 3): one FIFO sample
  a DAC slot late a few times per second while the CPU is busy.  Songs and
  mixed SEs null at about −49..−55 dB instead of −inf; DPCM cries show it
  harder (their waveform jumps several LSB per sample), and a few cries and
  se_rain_stop fail the worst-1 s gate at it.  Not modeled: it would need
  instruction timing.  Confirm a suspected floor run with the probe's
  `.events` slot crossings.
- **Channels above `maxPcmChannels`** (both mixers): SoundMainRAM
  envelopes and mixes exactly `maxChans` channels, so a channel above a
  lowered limit freezes with its status untouched; both ports process every
  ON channel.  Reachable only by lowering the limit while notes play
  (allocation is bounded by the limit); recorded by the step 12 review of
  `docs/HQ-MIXER-ACCURACY.md`, not changed.
- **Malformed loop data** (both mixers): a looped sample with `loopStart >
  size` gives SoundMainRAM a negative loop length (the pointer moves
  forward at the end and reads past the data); the ports clear the loop
  flag and stop the channel at the end.  No pokeemerald sample has one.
- **HQ mode's open bus is mgba's**: the HQ mixer's reversed end frame
  decodes every block of that frame from a wrapped address past the ROM;
  the port reproduces mgba's `GBALoadBad` (the prefetch word two
  instructions ahead, lane `address & 3`), which is the harness's truth.
  Real hardware's open bus for IWRAM code differs, so those samples of a
  reversed cry's end are an emulator model, not a hardware one
  (`docs/HQ-MIXER-ACCURACY.md`, section 2).
- **Reversed DPCM cry end**: the GBA's look-behind of sample 0 decodes
  "block −1" from open bus (38 at block offset 63 on the three probed
  cries); the port decodes 0.  One sample per reversed track.  A build
  holding mgba's 33 bytes renders bit-exact; emulating mgba's prefetch was
  not done.
- **pcm_psg frame 123, 4 cycles**: the tick on which track 0 (the
  three-note chord track) reaches its gate end and its FINE in the same
  pass ends 4 cycles early in the engine (`track_diff`: the `_081DD998`
  position before `ply_fine`), so CgbSound's entry and the two square
  writes of that frame are 4 cycles early; on both ROMs, harmless to the
  harness (square writes need only DAC-slot accuracy, the row is at the
  PCM floor), visible to `claim_check`'s write diff since the row's probe
  log grew from 70 to 180 frames (2026-10-27).  pcm_chord's four- and
  six-note gate ends (frame 63, no FINE on that tick) are exact, so the
  cost sits in the gate-end + FINE combination; not yet measured.
- **A third cry while both cry players are busy** (the clock-based
  replacement) is refused by poryaaaa; the ROM side is measured (probe
  step 13 in the history, `build/scratch/cry_third_jynx_wailord_bulbasaur.wav`).
- **Not ported / not modeled**: an SE and music on one frame from the
  ROM side (needs a second song id in `StartTest`); the sound-mode Mono
  bit; SampleFreqSet's replay when the PCM rate changes after boot (plugin
  only); hand-written XCMDs xwave and the tone-field ones (consumed, not
  applied); MEMACC's out-of-bounds read (masked to the 16-byte area);
  MPlayStart's priority / `unk_B` gating against a playing song;
  `m4a_engine_kill_channel` leaving the track field that TrackStop_2 zeroes
  (nothing reads it).
- **MIDI path corner cases** (no corpus row reaches them): latched, a
  note-off in a later tick of its own frame gets gate 1 and sounds one
  frame where the ROM kills it unheard; the timing-only sequencer's copy
  can disagree with the MIDI engine about channel ownership where they
  allocate differently (event order within a tick, the missing PRIO, an
  MTrk with notes on two channels), affecting only `cgbEntryCycles`; loop
  markers are taken from any track (mid2agb: the first MTrk); a Type 0
  file's track ends follow the whole MTrk's events of their tick.
- **Opt-in extensions** (off by default, outside the stock paths and the
  harness): portamento (CC 5), PWM (CC 0x17 / 0x19), the base-MIDI-key
  option.  Their own divergences from the eventide sources: PWM sets the
  duty-modify bit on every step and writes NRx1 before the pitch block;
  portamento applies to every channel of the track and keeps running after
  a song's end.

## 7. Working rules

- **mgba decides.**  A claim about what the source does is tested against
  the harness before it is adopted, one claim at a time
  (`claim_check.sh`).  A change that improves most tests but costs one
  `−inf` is wrong: the `−inf` tests are the ones whose data can be checked
  byte for byte.
- **Read the ROM bytes, not the converter source.**  What a sample, voice
  or header byte is gets settled from `accuracy_test.gba` and its map.
- **Never fit a constant to the harness.**  Every timing constant is a
  probe-measured segment, recorded with its label; a new frame is first
  checked against the composed existing constants.
- **Port the arithmetic verbatim**: tempo accumulator, volume math, the
  8-bit wrap, the rounding quirks.  No gain knobs.
- **Never `--update-refs` to make numbers better.**
- **The ROM boot-path rule**: `AgbMain`'s boot path and per-frame loop stay
  instruction-identical across ROM changes; new work goes in `StartTest`.
  Two extra prologue instructions once moved timer 0's start by 6 cycles
  and, through mgba's ordering of an overflow against a coinciding DAC
  slot, every DirectSound reference, while the probe's `.frames` stayed
  identical.  After a ROM change, render pcm, pcm_cmp, cry_bulbasaur and
  sq1 with `mgba_render` and `cmp` them against the current references
  before running the harness; check the compiled CgbSound is unchanged
  address-normalised and shift the `CgbSound_*` probe labels.
- **The sequencer path stays byte-identical** when the MIDI path changes
  (`claim_check`: 0 moved writes, no row moved).  A MIDI-path behaviour
  computable from what the engine sees goes in the engine for every source;
  one that needs the renderer's knowledge is gated by `--gba-tempo-grid`
  (or renderer-only); every new entry point is latched and is a no-op under
  a sequencer.
- **Unit tests for MIDI-path rules** use the `batch_pair` pattern
  (`test/test_engine.c`): the same material through the MIDI path (the
  renderer's calls) and a sequencer song, both engines booted with their
  output (booting without it shifts c15's phase), every PCM / CGB channel
  compared after every frame, latched and not; check the test fails on the
  old engine.

---

## Appendix A: measured MPlayMain and CgbSound costs

Cycles; a delta between consecutive probe breakpoints, keyed on the
deciding state.  *est.* = never taken by the corpus.  A taken Thumb branch
in ROM costs about 4 more than a not-taken one.  The code is the reference
(`plugin/m4a_engine.h`, `plugin/m4a_channel.c`); this is the map.

**Player skeleton**

| player | body start | FadeOutBody | `_081DD9BC` | `_081DDA6C` → CgbSound |
|---|---|---|---|---|
| BGM (10 tracks) | 566 | 662 | 716 | 370 (5 × 63 + 55) |
| SE1 (3) | 637 | 725 | 779 | 307 |
| SE2 (9) | 700 | 788 | 842 | 244 |
| SE3 (1) | 763 | 851 | 905 | 181 |
| cry 0 (2) | 826 | 914 | 968 | 118 |
| cry 1 (2) | 889 | 977 | 1031 | 55 |

Constants: `M4A_CYC_MP_BGM_ENTRY` 566, `_MP_HEAD` 150, `_MP_PAUSED` 71,
`_MP_IDLE` 63, `_MP_SE_HEAD` 142, `_MP_TAIL` 55.

**No-tick frame**: `_081DD9BC` → `_081DD9C4` 12 → `_081DD9C8` 4; the pass
loop per track: no EXIST 14, EXIST without flags 20, next track 16, after
the last 10.  BGM, no tick: 1396 + 6 per existing track.

**Tick**: `_081DD9BC` → `_081DD874` 14 → `_081DD87C` 8.  Per track: no
EXIST 16; EXIST 14, then 12 to the chain walk or 18 to `_081DD8BA` with an
empty chain.  Chain walk per channel: ON gate 0 20, gate decremented 26,
gate reaching 0 34; dead 14 + ClearChain 12 + RealClearChain 33 + 54 / 50
/ 56 / 52 *est.* (prev 0 next 0 / prev 0 next set / prev set next 0 /
both); next channel 12, end 6.  `_081DD8BA` → `_081DD938` 14, or with
MPT_FLG_START 20 + Clear64byte 33 + SoundMainBTM 68 (121).  Wait path at
`_081DD938`: no LFO 28 (lfoSpeed 0 *est.* 23), delayed 40, step 34 + 26 /
24 + 20 unchanged / 38 PITCHG / 36 VOLCHG.  `_081DD994` → `_081DD998` 4;
next track 18, last 10; `_081DD9A4` → `_081DD9BC` 22; no track left
(PAUSE) `_081DD9A4` → `_081DDA6C` 24.

**Command dispatch**: `_081DD938` → `_081DD8E0` 12; explicit byte 21 (14
from IWRAM), then 14 below 0xBD / 10 at or above; running status 25; note
34 to ply_note; other commands 10 + 40 to the handler; `W##` 93 in all.
Handlers (entry → `_081DD938`, ROM data): tempo 91, keysh 89, voice 184,
vol 89, pan 91, bend 91, bendr 89, lfos 69, lfodl 81, mod 69 (0: 109),
modt 91 / 95 changed, xcmd 61 + xiecv / xiecl 49 (others *est.* 110),
xcmd 0x0D 120, xrele 48, xwait 108 / 102 at expiry (RAM), goto 110, patt
140, pend 40 (34 at level 0), endtie 61 + 28 per match + 6 key byte
(`_SKIP` 18 *est.*), fine 44 empty chain / 114 one channel; tune, rept,
memacc, prio, port *est.* 90.  ply_note exit 30.

**ply_note**: entry → `_081DDB46` 78 / 97 key byte / 114 key + velocity /
*est.* 129 gate byte; voice plain 26, keysplit 92, drum 111; then 48 to
the CGB test or 40 (47 from a ROM table, + 25 in TrkVolPitSet) to the
DirectSound scan.  CGB test: free 20, STOP 26, by equal priority and track
order 48 (by priority *est.* 32; rejected *est.* 34 / 42).  DirectSound
scan `_081DDBEC` → `_081DDBFA` 14, per channel: `SCAN_FREE` 14,
`SCAN_ON` 20, `SCAN_ON_CMP` 4, `SCAN_CMP` 12, `SCAN_EARLIER_TRACK` 22,
`SCAN_SAME_TRACK` 18, `SCAN_FIRST_STOP` 34, `SCAN_STOP` 24, `SCAN_ON_SKIP`
10, `SCAN_HIGHER` 8, `SCAN_NEXT` 12, `SCAN_DONE` 12 (passing a live
equal-priority channel of an earlier track: 70).  Setup: ClearChain 12 +
RealClearChain 33, then to `_081DDC54` by links: unchained 44, 68 / 64 /
70 / 66, + 4 when the new chain is empty; lfoDelay 0 18, else 24 +
clear_modM 36; TrkVolPitSet 14 → ChnVolSetAsm 100 + 82 VOLSET (+4 modT 1)
+ 50 PITSET (−2 modT ≠ 0); ChnVolSetAsm → `_081DDCA0` 78; CGB 28 (32 real
sweep) + 30 + MidiKeyToCgbFreq 190 (noise 98, low key 180); DirectSound 12
+ 20 + MidiKeyToFreq 235; `_081DDCDC` → `_081DDCEA` 14.  A note on a free
channel with a plain voice: 895 from `_081DD938` to `_081DD938`.

**VOLCHG / PITCHG pass** (a flagged track): `_081DD9C8` → TrkVolPitSet 30;
→ `_081DD9E6` 62 + 82 VOLSET + 50 PITSET (empty chain + 6); per chained
channel 14, VOLCHG 24 + 72 DS / 74 CGB (else 20), PITCHG 24 + CGB 32 + 200
/ DS 10 + 16 + 237 (else 14); next 12, after the last 6; → `_081DDA62` 10.

**CgbSound** (`CGB_CYC_*`): entry → channel-1 loop head 53 (51 on a c15
reload); inactive channel 44; loop → active 26; register switch 69 sq1 /
61 sq2 / 75 wave / 69 noise; → start 24 / nostart 30; start → CgbModVol
44 → first write NR10 135 / NR21 131 / NR30 147 (153 without a RAM
upload) / NR41 141, plus CgbPan's class 0 / 4 / 6 / 10; NR10 → NR11 10,
NR30 → WAVE 26, WAVE → WAVE 15, NR41 → NR43 8; envelope step: step →
modvol 18 (wave 20) → CgbModVol 10 → complete by phase (sustain 147,
release 155, attack 173, decay 179, attack → decay 211, decay → sustain
219); counter ≠ 0 12; pitch block NRx3 28 (+40 FIX; noise NR43 32), NRx3 →
NRx4 32 (noise 24); volume block NR51 21, → NRx2 28 (wave NR32 32), → NRx4
10; second NR14 24 + 2; CgbOscOff → NR12 37 (→ NR14 14), NR22 37 (→ 6),
NR30 51, NR42 45 (→ 6); after channel 4 → SoundMainRAM 99 / 95.

## Appendix B: test-ROM and probe reference

**`struct AccParams`** (`PORYACC1` block in `crt0.s`, 28 bytes, at ROM
0xC0): type, id, then the cry parameters pitch / pan / volume; `u16
length` at 16, `u8 release` at 18 (ROM defaults 140 / 0), `s8 chorus` at
19, `u16 id2` at 20, `u8 startFrame2` at 22, padding at 23, `u16 id3` at
24, `u8 startFrame3` at 26, one reserved byte.  New fields come from the
reserved bytes and default to 0, so an old patcher's block plays as
before.  `StartTest` (`rom/main.c`) starts the test, then waits frame by
frame for `startFrame2` / `startFrame3` and runs `StartCry` at the defaults
— after the type switch, so a cry can start over a song or SE.  Patchers:
`mgba_render --type song|cry|micro --id N [--pitch --pan --volume --length
--release --chorus --id2 --start-frame2 --id3 --start-frame3 --frames]`;
the probe's positional arguments in the same order.  Start frames above
255 are truncated to the low byte.

**Moving addresses.**  Any change to `main.c` or `crt0.s` moves every ROM
symbol from `AgbMain` on by one constant; IWRAM / EWRAM do not move.
Re-derive the compiled CgbSound's internal labels from
`arm-none-eabi-objdump -d` (the header of `mgba_probe.c` names each one)
and append them to `build/probe/syms.nm`; the current values are in the
`accuracy-harness` skill.  Fixed IWRAM: `gSoundInfo` 0x03000f70,
`SoundMainRAM_Buffer` 0x03000770 (`_081DCFB0` copy 0x03000878), channels
0x03000FC0 + 0x40 · n, `gPokemonCrySongs` 0x03001f20 (+ 0x34 · i),
`gPokemonCryMusicPlayers` 0x03001f88 (+ 0x40 · i), `gPokemonCryTracks`
0x03002258, `gMPlayTrack_BGM` 0x03000040.  Track address order is the
engine's track-index order.

**Cry mode values** (`src/sound.c` `PlayCryInternal`): FAINT pitch 14440,
length 140, release 200; HIGH_PITCH 15800 / 50 / 200, chorus 20, volume
90; ENCOUNTER 15600, release 225, chorus 20, volume 90; DOUBLES 15360 / 20
/ 225; ROAR_2 15616 / 60 / 225; GROWL_1 reversed, 15200 / 15 / 125;
ECHO_START reversed, 15600 / 25 / 100, chorus 192, volume 90; ECHO_END
15555, release 220, chorus 192, volume 70.  Opponent-side pan 63.
