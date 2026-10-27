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
in `review-against-source`.

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
  engine's own resampler (`plugin/m4a_blip.c`), so both sides go through
  one bit-exact resampler and the comparison measures only the GBA-side
  model.  Its version string is `mGBA 0.10.5+m4a_blip`;
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
| `test/accuracy/ref/mgba_render.c` | headless mgba render of the ROM at 48 kHz from frame 0, via `mAVStream.postAudioFrame` (one int16 pair per 65536 Hz DAC sample) into `m4a_blip` |
| `test/accuracy/ref/mgba_probe.c` | libmgba debugger probe: breakpoints on ROM labels, IO-write log, scheduler lateness (section 2, "Probe") |
| `build/poryaaaa_render` | the engine's CLI renderer (`cmd/poryaaaa_render.c`) |
| `test/accuracy/tools/accuracy.py` | renders both sides, aligns, measures, writes `build/accuracy/report.json` and a table |
| `test/accuracy/tools/metrics.py` | alignment (`find_lag`) and the metric (`compare_files`) |
| `test/accuracy/tools/compare_reports.py` | flags null regressions over 1 dB between two reports (not worst1s) |
| `test/accuracy/tools/claim_check.sh` | one-shot gate: build, the three test binaries, the PSG write-cycle diff of every probed test, the harness, `compare_reports.py` |
| `test/accuracy/songs.json` | the corpus beyond the micro-tests |
| `test/accuracy/baselines/` | saved reports (`<date>_<name>.json` / `.txt`) |
| `poryaaaa_unit_tests` | `test/test_engine.c` |
| `poryaaaa_blip_tests` | `m4a_blip` against mgba's blip_buf, bit-exact |
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
  runner always adds `--gba-start-delay` and `--gba-tempo-grid` (section 5;
  `--no-gba-start-delay` / `--no-gba-tempo-grid` leave them out) and, with
  `--midi-quantize`, `--quantize` (the frame latch).  Cries and cry-over
  rows have no MIDI and are skipped.  **Midi-input reports are their own
  baseline family** (`baselines/<date>_midi-path-*`; the report's
  `"input"`, `"midi_quantize"`, `"gba_start_delay"`, `"gba_tempo_grid"`
  fields say which form) and are never compared with song-input reports.

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
  plays them), both step 8.  Ids are line numbers in `micro.cfg`.
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
  frame: CgbSound entry, the selected player's state), `<out>.events`
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
  1024-cycle sample event, blip frames of 2048 cycles.
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
blip has enough samples, so the machine is up to a blip frame past a
sample's cycle when that sample is returned (this matters for when host
events land, section 5).

### Output stage

- `plugin/m4a_blip.{h,c}`: an MIT reimplementation of blip_buf's
  fast linear-delta path (the engine's resampler, and the reference's).
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
  one-sample click).  Phase step = `divFreq · frequency` (scaled).

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
  sequencer publishes it (section 5).
- `TrackStop` runs in the main loop, outside SoundMain; the engine charges
  nothing for it.

Every constant is a probe-measured segment; a few paths the corpus never
takes are estimates (marked *est.* in the header).  A future song that
takes one shows up in `cmd_diff` / `track_diff` as a constant offset on
that command.

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

`poryaaaa_render --midi` with **`--gba-start-delay`** and
**`--gba-tempo-grid`** (both off by default in the renderer, on in the
harness) plays a MIDI the way the game plays mid2agb's conversion of it.
On the corpus this form is the `--song` render sample for sample.

- **Start origin** (`--gba-start-delay`): MIDI time 0 is placed where the
  ROM starts the song, frame 3's SoundMain at `ROM_FIRST_TICK_CYCLE` =
  124773 + 2 · 280896 + 160 · 1232 + 197 = 883882 cycles.  Every event is
  dispatched `midiOrigin` = floor((883882 − 2048) · rate / 2²⁴) samples
  later (2522 at 48 kHz): the last sample at least one blip frame before
  that SoundMain, because the machine runs up to a blip frame past a
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
    `gba_tempo_grid_sample(j)` (the same blip-frame guard); several ticks
    of one frame share one sample, in order.  Loop passes add whole clocks
    (`gba_tempo_grid_pass_clock`); the loop-count fade and the natural tail
    move onto the grid.
- **The engine's LFO clock on the grid**: `m4a_engine_start_song_tempo`
  (MPlayStart's seed, at the origin) and `m4a_engine_set_tick_tempo(tempoI)`
  (a TEMPO played in the coming tick, stored after the next frame's add,
  as ply_tempo's store lands after `_081DD86C`).  The renderer passes the
  grid's tempoI for first-MTrk tempos and nothing for the others.  Needs
  the start delay too.
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
