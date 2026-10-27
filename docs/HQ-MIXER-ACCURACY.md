# poryaaaa HQ-mixer accuracy: the plan

poryaaaa reproduces vanilla pokeemerald's m4a engine closely enough to null
against mgba 0.10.5 (`docs/ACCURACY.md`).  This document plans the same
result for a second engine configuration: pokeemerald with **ipatix's
High-Quality Audio Mixer** installed as the pret wiki describes
(<https://github.com/pret/pokeemerald/wiki/Implementing-ipatix's-High-Quality-Audio-Mixer>),
selectable in poryaaaa as an "HQ mixer mode" option that is off by default.

It is written for agents working one step at a time, in the order of
section 7, each step directed by the user.  Everything the vanilla effort
settled (ground truth, metric, harness, probe, the sequencer, CgbSound, the
timing model, the MIDI path, the working rules) carries over unchanged and
is not repeated here; read `docs/ACCURACY.md` first, then this.  Sections
1 to 3 are what was read from the sources before any code was written;
every statement of the form "the mixer does X" in them is a reading of the
assembly and is **to be confirmed by measurement** in its step, as the
working rules require.  As steps land, their durable findings replace the
readings here (as `ACCURACY.md` did for its plan); measurements, predictions
and rejected claims go in commit messages.

Contents:

1. The HQ mixer: source, version, configuration, what the wiki changes
2. How it differs from SoundMainRAM (the port specification)
3. What carries over unchanged, and what may not
4. Ground truth for HQ mode: the HQ test ROM and the harness
5. The engine in HQ mode
6. Coverage: mixer code paths and the tests that reach them
7. The plan, step by step
8. Risks and expected residuals
9. Working rules for this effort
- Appendix A: HQ mixer source map
- Appendix B: arithmetic reference (as read)
- Appendix C: the wiki's tables

---

## 1. The HQ mixer: source, version, configuration, what the wiki changes

### Sources

| what | where | notes |
|---|---|---|
| The wiki page | pret/pokeemerald wiki, "Implementing ipatix's High-Quality Audio Mixer" | the installation recipe hackers follow; it links the two sources below |
| **The mixer as installed in pokeemerald** | `https://raw.githubusercontent.com/Kurausukun/pokeemerald/newmix/src/m4a_1.s` (branch `newmix`; the file last changed in commit `6eb6282` "add missing constant usage in m4a_1", 2023-09-23; sha256 of the download starts `c926c56d54f6f1c1`) | **the canonical source for this effort**: "HQ-Mixer rev 4.0 created by ipatix (c) 2021", adapted to pokeemerald's struct-offset symbols and build (`.syntax divided`, `thumb_func_start`), with `ENABLE_REVERB 1` and `ENABLE_DMA 1`.  Lines 88–1210 are the mixer; the rest of the file (SoundMain, SoundMainBTM, RealClearChain, MPlayMain, every `ply_*`, CgbSound's helpers …) is byte-for-byte vanilla apart from the label `call_r3` being named `_081DD25E` |
| ipatix's repository | <https://github.com/ipatix/gba-hq-mixer> (`m4a_hq_mixer.s`, README) | the same rev 4.0 in its generic form (`hq_buffer_ptr` as an `.equ`, `POKE_CHN_INIT`, `ENABLE_STEREO`, `ENABLE_REVERB 0` by default).  Its README explains the design (16-bit mix buffer, one quantization, the packed-word stereo trick and its "negative sample subtracts 1 from the other half" caveat, loop unrolling, self-modifying code, the stack pre-cache).  **Not the build target**: the wiki points at the newmix file, and the two differ in defaults and symbol forms (3521 diff lines) |
| The eventide tree | `~/eventide/src/m4a_1.s` | the same newmix mixer (its mixer region diffs from newmix only in `.section .iwram.code` and a blank line) plus eventide's own MPlayMain additions (portamento, PWM, base-key).  The engine's Golden Sun synth was ported from this copy.  Not a source for this effort; the newmix file is |

### What the wiki changes in the game

1. `src/m4a_1.s` replaced by the newmix file (SoundMainRAM, SoundMainRAM_Unk1,
   Unk2 and the reverb pass are gone; one new SoundMainRAM, Thumb entry with
   ARM inner loops, position-independent, copied to IWRAM as before).
2. `src/m4a.c`: `SoundMainRAM_Buffer` grows from `0x800` to `0xB40` bytes
   (the IWRAM copy the mixer runs from, `CpuCopy32` in `m4aSoundInit`), and a
   new `BSS_CODE ALIGNED(4) u32 hq_buffer_ptr[0xE0]` — the 16-bit stereo mix
   buffer, one word per output sample of the frame (0xE0 = 224 = the
   13379 Hz frame).  **Nothing else in m4a.c changes** (verified: the newmix
   m4a.c diffs from vanilla in exactly those two lines).
3. IWRAM freeing (`link_rfu_2.c`, `librfu_rfu.c`): the game's RFU buffers
   move to EWRAM.  Irrelevant to the test ROM (no RFU).
4. Optional: `MAX_DIRECTSOUND_CHANNELS` 12 → 5 in `m4a_internal.h` and
   `m4a_constants.inc` (newmix itself has 5).  Emerald's `m4aSoundInit`
   already asks for 5 channels (`5 << SOUND_MODE_MAXCHN_SHIFT`, master volume
   12) and `m4aSoundMode` only clamps to the define, so `soundInfo->maxChans`
   is 5 either way; the define sizes the channel array (IWRAM layout) and
   `m4aSoundInit`'s clearing loop.  Recommendation: **keep 12** in the HQ test
   ROM (section 4); it is the wiki's optional RAM step, not a mixer change.
5. Optional: a higher engine rate (`m4aSoundInit`'s `SOUND_MODE_FREQ_*`),
   with `hq_buffer_ptr` sized to the rate's frame (appendix C) and, with
   reverb, `PCM_DMA_BUF_SIZE` resized to 7 frames.  7884 Hz is unsupported
   (its frame, 0x84, is not a multiple of 8).  The HQ test ROM stays at
   13379 Hz; other rates are a later variant (section 7, step 11).
6. `DmaSetUnchecked` in `include/gba/macro.h` writes the three DMA registers
   with one `stmia` (the mixer uses DMA3; the game's own DMA3 users must not
   be interrupted between register writes).  The test ROM has no other DMA3
   user; apply it anyway so the ROM is built "as the wiki says".
7. Mixer configuration (`.equ` at the top of the mixer): `ENABLE_REVERB`
   (1: the m4a reverb; 0: none, and the mix buffer is cleared instead of
   seeded), `ENABLE_DMA` (1: DMA3 copies sample data to the stack; 0: a CPU
   copy loop).  Both 1 in newmix.

---

## 2. How it differs from SoundMainRAM (the port specification)

This is the mixer read side by side with `plugin/m4a_mixer.c` (the vanilla
port) and `m4a_1.s` lines 88–709.  Appendix A maps the labels; appendix B
has the formulas.  Items marked **(quirk)** are behaviours that follow from
the instruction encoding rather than the intent and must be ported as the
instructions do them — the kind of thing a "cleaned-up" translation loses
and the harness then fails on.

### Frame structure

Vanilla: reverb pass over the ring slot (or zero fill), then per channel
envelope step + mix straight into the int8 ring with the 8-bit wrapping
packed-word add.  **HQ**: per channel envelope step + mix into the 32-bit
`hq_buffer` (int16 left in the high half, int16 right in the low half, one
word per output sample), which already holds this frame's **reverb seed**;
then one **downsampler** pass converts the buffer to int8 with
first-order noise shaping and saturation, stores it into the ring slot
(A = right, B = left, as before), and while doing so reads the ring taps and
writes the **next** frame's reverb seed back into `hq_buffer`.

Consequences to port:

- **The reverb is one frame later than vanilla.**  Vanilla frame N's reverb
  reads ring slot N (old, frame N−7) and slot N+1 (frame N−6) into frame N's
  output.  The HQ downsampler of frame N reads the same two slots (the
  current slot's old bytes before overwriting them, the next slot's — the
  ring start when the buffer-position hint `pcmDmaCounter == 2`) but the
  product seeds frame N+1.  Same taps, same wrap rule, used a frame later.
- **Reverb arithmetic**: per sample, `sum` = the four int8 taps
  (next A, next B, current B, current A); the seed word is
  `sum × ((reverb >> 2) | (reverb >> 2) << 16)` — the same mono value in
  both halves, scaled by `reverb >> 2` (0..31), with the packed-word borrow
  **(quirk)**: a negative `sum` leaves the high half one less than the low
  half.  Vanilla scales by `reverb >> 9` with a round-toward-zero.  Level
  127 becomes 31 (vanilla 127/4 = 31.75): slightly less reverb, different
  rounding.  With `reverb == 0` the seed is 0 (the taps are still read).
- **The mix buffer is seeded, not cleared** (with `ENABLE_REVERB 1`); the
  first frame's buffer is BSS zero.
- **Downsampler** per channel side (left uses the whole word `w`, right
  uses `w << 16`): `t = state + w` (state = the previous sample's residual,
  bits 16..22, kept in a register across the frame and in SoundInfo bytes
  0xE / 0xF across frames — "VAR_EXT_NOISE_SHAPE_LEFT/RIGHT", the gap bytes
  after `maxLines`); saturate `t` to 31 bits (`adds t2 = t + t; eorvs t =
  0xC0000000 ^ (t2 asr 31)` → `0x3FFFFFFF` on positive overflow,
  `0xC0000000` on negative); output byte = bits 23..30 of `t`; new state =
  `t & 0x007F0000`.  The right half of the word rides along in the low 16
  bits of the left side's `t`, but the residual's low half is always 0 and
  the output and the new residual only read bits 16 and up, so it never
  reaches the left output (step 4 corrected the earlier reading of this as
  a quirk); the one place right-channel content enters the left half is
  the accumulate's borrow (`mla` of a negative product), which the word
  arithmetic reproduces — port the words, never split the channels first.
- **Saturation replaces the 8-bit wrap**: a loud mix clips at ±full scale
  instead of wrapping.  The 16-bit halves themselves still wrap during
  accumulation (`mla` on packed words), and an overflowing right half
  carries into the left half **(quirk)**.
- Two output samples per downsampler iteration; the int8 pairs are stored
  as halfwords (`strh`), low byte first — byte order in the ring is
  unchanged.
- The downsampler ends by writing `ID_NUMBER` (0x68736D53) to
  `SoundInfo.ident` and unwinding SoundMain's frame, as vanilla does.

### Channel init (SF_START) and the ADSR step

The envelope state machine is the same as vanilla's (`m4a_pcm_channel_tick`
semantics: START|STOP killed silently; IEC countdown with the unsigned
`sub; bhi`; release `env × release >> 8` to 0 → echo or stop; decay to
sustain; attack add capped at 0xFF; the first START frame runs the attack
and mixes).  Differences:

- **Volume**: `v = env × (masterVolume + 1)`; `left = (leftVolume × v) >> 13`,
  `right = (rightVolume × v) >> 13`, each 0..127.  Vanilla:
  `((masterVolume + 1) × env) >> 4`, then `× side >> 8` (0..255, two
  truncations).  Different intermediate rounding and half the range.
- **Compression is decided by the sample, not the voice**: `WaveData.type`
  bit 0 set → the channel type gets `TONEDATA_TYPE_CMP` OR-ed in and the
  compressed paths are used, whatever the voice said.  Vanilla enters Unk1
  on the voice's CMP|REV bits and mixes nothing for a CMP voice on
  uncompressed data.  The loader's "missing `.bin` → uncompressed with CMP
  cleared" fallback is consistent with both.
- **Pointer semantics are set at init** (vanilla sets REV/CMP up lazily on
  the first mix, `CHN_SPECIAL`): uncompressed forward `ptr = data + count`,
  `count = size − count` (count = the XCMD 0x0D start offset, 0 normally);
  uncompressed reverse `ptr = data + size − count`, `count = size − count`;
  compressed forward `ptr = count` (a **sample index**), `count = size −
  count`; compressed reverse `ptr = count = size − count`.
- **Zero-size sample = synth** (`C_channel_init_synth`): channel type
  becomes `TONEDATA_TYPE_SPL` (0x40), triangle (descriptor type 2) starts
  at phase 0x40000000, others at 0; `count = 0`, `ptr = data`.  This is the
  real thing the engine's Golden Sun extension imitates (section 5).
- **`CHN_SAMPLE_STOR`** (SoundChannel byte 0x3F, the high byte of `xpc`):
  the last base sample of the linear interpolation, zeroed at init and
  carried **across frames** — HQ keeps `base` and points `ptr` at the *next*
  sample, where vanilla reloads `data[ptr]` each frame.  New channel state
  for the engine.  A consequence (measured, step 4): a note's first source
  sample is interpolated from the zeroed base, so every note starts with a
  one-source-sample ramp from 0 instead of vanilla's step onto `data[0]`.
- The loop flag comes from `WaveData.flags` bits 6..7 (`lsr #6` of the
  byte at offset 3 → the two loop bits of the 16-bit status), as vanilla.

### Mixing setup

- `step = frequency × divFreq` (`VAR_DEF_PITCH_FAC`, SoundInfo offset 0x18),
  23-bit fraction, as vanilla; FIX voices force `step = 0x800000` (1.0).
- **Both volumes zero → the channel is skipped for the frame** without
  advancing `fw` / `count` / `ptr` **(quirk)**: a channel at VOL 0 (or at
  the start of a slow attack while `left × v >> 13` is still 0) freezes its
  playback position and resumes where it stopped.  Vanilla keeps advancing.
  Needs a micro-test (section 6).
- Path selection: `CMP|REV` → the pre-cached path (compressed or reversed
  data decoded / byte-reversed onto the stack, then the fast loop);
  `FIX` → the fixed-frequency path; synth (`SPL`) → the synth loops; else
  the interpolating path.  A `FIX|REV` voice takes the pre-cached path at
  step 1.0.
- Interpolating and pre-cached paths halve the volumes to 0..63 with
  round-up (`lsr #1; adc #0x8000; bic #0x8000` on the packed word →
  `ceil(v / 2)` per half) and mix the sample at double scale
  (`2 × (base + frac × diff)`), or at single scale with the volume doubled
  back for step ≥ 1.0; the FIX path mixes `v × sample` unhalved; pulse
  synth `±64 × v`, triangle `±128 × v`, saw `filter × floor(v / 2)`.  All
  land at the same nominal scale, with different rounding per path.

### The interpolating path (uncompressed, not FIX)

- `advance = (fw + step × frameLength) >> 23` (a 64-bit `umlal`).
  **If `count > advance`** the frame cannot reach the sample's end: the
  **fast path** — the frame's `advance + 1` source bytes are read either
  directly from ROM (when they exceed 0x400 bytes or the stack has no room)
  or, after a DMA3 word copy onto the stack (the stack boundary check is
  against the literal `0x03007900`: `boundary + bytes < sp` required), from
  the stack; byte content is identical either way, so the copy is
  output-neutral by construction.  Then the self-modifying 8-samples-per-
  iteration loop with one of three instruction blocks by `step`:
  `< 1.0` (position kept as `p − 2^23`, the sample advance taken from the
  sign of `p + step`, `bic` of bits 23..31 after each sample), `[1.0, 2.0)`
  (`asr #23` interpolation at single scale, one or two loads per sample),
  `≥ 2.0` (pointer += `fw >> 23` each sample).  The block is pasted over
  the NOP slots once per change (`previous_fast_code`), which has no
  behavioural effect beyond selecting the block.
  **(quirk)** the `mulne` condition is the flags left by the *previous*
  sample's `subpls` / `adds`: after a step where the new `diff == 0`, the
  multiply is skipped and `r9` keeps the previous output value, whose
  `asr #22` is 0 or −1 — a flat negative run comes out one unit (at
  double scale) low; the `mlane` condition is the output itself (`adds
  r9`), a no-op skip.  **Measured (step 4)**: the first sample of each
  8-sample iteration takes its flags from the loop's `subs r8, r8, #8`
  instead, so it always multiplies — pcm frame 64 was off by one output
  sample until the port did the same.  Port the condition codes, and the
  loop structure they live in.
- **Else the unbuffered loop**: one sample per iteration, mixed *before*
  the end check; at a loop end `ptr −= loopLen; count += loopLen` (a
  relative wrap, not a reset to `loopStart`; the vanilla while-loop for
  multi-wrap is a repeated hit here); without a loop the channel's
  `statusFlags` is set to 0 and the rest of the frame is not mixed, with
  `fw` / `count` / `ptr` **not** written back.  The look-ahead byte
  `data[size]` is read for the interpolation as in vanilla.

### The fixed-frequency path (FIX)

Groups of 4 output samples from a word load with alignment-selected
sign-extension code, `min(count, frameLength)` at a time; the remainder
sample by sample; on exhaustion `ptr = loopStart` (absolute) and `count =
loopLen`, or the channel stops (status 0).  Unhalved volume, raw ±128
sample.

### The pre-cached paths (CMP, REV)

- **Compressed forward**: the DPCM blocks covering `[ptr, ptr + advance + 1]`
  are decoded (0x21-byte blocks of 64 samples, the same
  `delta_lookup_table` as vanilla's `gDeltaEncodingTable`) onto the stack,
  then played by the fast loop.  **No loop support** ("TODO" in the source):
  a looping compressed sample ends.  At the end (`count ≤ advance`) the
  bytes past the end are zero-filled (`F_clear_mem`), the channel's
  `statusFlags` is set to **0 immediately** (`streqb`), and the frame is
  still mixed (interpolating into the zeros) — the sample's tail of the
  frame is silence rather than an abrupt stop, and the channel dies at
  this frame, not the next.
- **Compressed reverse**: blocks decoded in reverse order with the reversed
  store, so the fast loop plays backwards.  The lowest block index is
  `(ptr − advance − 1) >> 6`; in the end frame this is **negative**, and
  the block address wraps to open bus (`data + 0x21 × 0x3FFFFFxx`).
  **Measured (step 2(e), `cry_bulbasaur_reverse` frame 50, 88 samples
  left, advance 176)**: `r9 = (0x58 − 0xB0 − 1) lsr 6 = 0x03FFFFFE`, the
  buffer is 4 blocks (0x100 bytes), `count − advance < 0` so the top 0x80
  bytes (the region of blocks 0 and 1, the sample's real first 88 samples)
  are **cleared**, and the two blocks decoded are the open-bus ones at
  `data − 0x42 + 0x84000000` = 0x8C0D3DDE and 0x8C0D3DFF, stored reversed
  into the bottom 0x80 bytes.  The real tail is never decoded: the end
  frame plays open-bus garbage from read offset 0x28 and then zeros.
  mgba's open bus for an unmapped address read by ARM code
  (`GBALoadBad`) is `cpu->prefetch[1]`, the instruction word two ahead of
  the `ldrb` (`[pc + 8]`), byte-selected by `address & 3`: the three
  loads of `F_decode_compressed` see the words at `F + 16` (`EA000003`,
  the `b` to the entry), `F + 20` (`E4D93001`) and `F + 28` (`E0812002`).
  Decoding the two blocks with that rule and the 256-entry view of the
  delta table reproduces the probe's buffer dump byte for byte
  (`tools/hq_decode_check.py`), and the same decoder reproduces the
  previous frame's real blocks.  The port can
  therefore reproduce the end frame exactly from the mixer's own code
  bytes; `pcm_rev` and the reversed cries decide (step 8).
- **Uncompressed reverse**: words byte-swapped onto the stack, then the fast
  loop; at the end the zero fill and status 0 as above.
- The stack budget: if `0x03007900 + bytes ≥ sp` the pre-cached paths
  **skip the channel for the frame** (not even the unbuffered fallback),
  with `fw` unchanged — a channel that needs more than the budget every
  frame never advances and never ends.  `sp` at SoundMainRAM entry is a
  property of the ROM (the test ROM: `sp_sys = 0x03007E40` minus the IRQ
  dispatcher's and the sound driver's frames, so roughly 0x4E0 bytes of
  budget; in the game it varies with the main loop's call depth at the
  interrupt).  Measure it (step 2) and make it an engine parameter with
  the test ROM's value as the default.

### The synth paths (SPL)

As the engine's extension already has them (`mix_synth`,
`m4a_pcm_synth_pulse_update`): pulse `±64 × v` against a duty threshold
recomputed once per frame from the descriptor (`count` as the LFO
accumulator), saw with the `count` filter state and the half-volume, triangle
`±128 × v`, phase step `step << 3` per sample, 8 (pulse) or 4 samples per
iteration.  Differences to check against the extension: the pulse compares
`phase < duty` *before* advancing, the triangle's sign test is on the
pre-advance phase (`adds` then `rsbpl / submi` on the result), the saw
filter is `state = raw + (state asr 1)` with `mlane` skipping zero.  The
extension was written from this code and cross-checked with agbplay; HQ
mode makes it measurable for the first time (step 9).

### Volume-zero and frame-length constraints

Frame lengths must be multiples of 8 (the fast loop, the pulse loop) —
every table rate but 7884 Hz is.  The engine must refuse or snap
`pcmFreqMode 2` in HQ mode.

---

## 3. What carries over unchanged, and what may not

**Unchanged by construction** (the sources outside the mixer are identical):
MPlayMain and every command handler, ply_note's channel scan and steal rule,
CgbSound and its register sequences, the SoundMain timing model and every
measured cost in appendix A of `ACCURACY.md` (the mixer runs *after*
CgbSound; nothing later in the frame is timed), `m4aSoundVSync`'s DMA
restart and `pcmDmaCounter`, the FIFO / timer / DAC output stage, the MIDI
path and the plugin's latch rules, the `.s` parser and the loader layouts.
**Prediction to confirm in step 2**: the HQ ROM's PSG write cycles equal
the vanilla ROM's on every probed test (`probe_diff.py`: 0 moved writes),
modulo a constant boot offset if the boot moved.

**Likely to move: the boot timeline.**  `m4aSoundInit`'s `CpuCopy32` copies
0xB40 instead of 0x800 bytes (+0x340), and the crt0's BSS clear covers
0x340 + 0x380 more IWRAM; every later boot event (SOUNDCNT writes, DMA on,
timer on, MPlayExtender's PSG writes, the second init pass, IME) shifts by
the extra cycles.  The first handled interrupts stay frame 2's (IME is set
1406 cycles after frame 1's VBlank start today; later is still after), but
the timer-0 start cycle relative to the DAC slots — and with it which
ring byte plays in which slot — may change, as the "6 boot cycles moved
every PCM reference" episode showed.  `m4a_engine_boot` replays measured
constants; HQ mode needs its own measured table (step 2 → step 3).  The
sq1 canary (pure PSG) decides at once whether the boot moved: identical
reference → unchanged; else the probe's IO-write log gives the new cycles.

**Possibly different: the PCM floor.**  mgba's scheduler lateness depends on
the instruction stream; the HQ mixer's DMA3 halts and different loop costs
change when timer-0 overflows land late.  So the set of −inf rows may differ
from vanilla's, and the floor on songs may sit at a different level.  Not
modeled, as before; attribute with `.events` slot crossings.  **Measured
(step 2(f))**: the same mechanism at the same scale — timer-0 `max_late`
10–11 cycles on both ROMs, slot crossings per test of the same order
(vanilla / HQ: pcm 0 / 1, pcm_cmp 9 / 0, pcm_chord 6 / 0, littleroot
81 / 56, vs_wild 62 / 59, credits 35 / 73, cry_jynx 19 / 1; the sample and
PSG-frame events cross nowhere on either).  No DMA3 term: a DMA3 block is
just another stretch of the instruction stream the timer overflow waits
out, and the `ENABLE_DMA` variants of step 1 differ only through it (pcm's
frames 63–68 are the HQ run's one crossing, `first at cycle 17751296`,
frame 64).  **Step 4 put numbers on the floor rows**: with the ring slots
byte-identical to the ROM's in every frame (`tools/hq_ring_compare.py`),
pcm / pcm_reverb (one crossing in frame 63's SoundMain, audio frame 64),
pan (107 → 108), lfo_delay (127 → 128), pcm_chord (112 → 113) and bend
(7 → 8) each keep a single-frame residual of one DAC step — −66 to −88 dB
null, passing, where vanilla has them at −inf.  The crossings sit inside
the HQ mixer's longer SoundMain; they are the reference's, not the port's.

**Not output-neutral: `ENABLE_DMA`** (measured in step 1).  The DMA3 copy
and the CPU copy read the same bytes, but the two ROMs' references differ:
`pcm_cmp` byte-identical (the compressed path never uses DMA3), `pcm`
−71.4 dB (566 differing 16-bit samples, all in frames 63–68), `pcm_chord`
−60.0 dB (frames 100–137 in three runs), `mus_vs_wild` −48.3 dB null /
−39.8 dB worst second (runs of 5–10 consecutive frames throughout).  Bursts
of consecutive frames, nothing from the first sample: timing, not
arithmetic — the DMA3 block halts the CPU for the whole copy, so an mgba
event that falls inside it (timer-0 overflow, the FIFO DMA) is serviced
later than it would be under the instruction-granular CPU copy.  This is
the PCM floor's mechanism, and `ENABLE_DMA 1` is what hackers ship, so it
is the ground truth.  Step 2(f) measured it as the ordinary floor (above):
the HQ ROM's crossings are no more frequent than vanilla's, so the floor
should sit where vanilla's does.  The `ENABLE_DMA 0` variant is dropped.

---

## 4. Ground truth for HQ mode: the HQ test ROM and the harness

### The HQ test ROM

A second ROM from the same `test/accuracy/rom/` sources, built by the same
Makefile with `MIXER=hq`, into `build/accuracy/rom-hq/` (`accuracy_test.gba`,
`.elf`, `.map`); the compiled songs are the vanilla build's (identical
inputs; symlink or rebuild).  Differences from the vanilla build:

- `m4a_1.s`: the vendored newmix file `test/accuracy/rom/hq/m4a_1.s`
  (header kept; a `SOURCE` note with the URL, branch, commit
  and sha256) in place of `$(POKEEMERALD)/src/m4a_1.s`.
- `m4a.c`: the pokeemerald tree's file with the two wiki lines applied at
  build time (`sed` in the rule, or a `.patch` under `rom/hq/`): buffer
  `0xB40`, `hq_buffer_ptr[0xE0]`.  Nothing else.
- `macro.h`: the atomic `DmaSetUnchecked` — the pokeemerald tree's header
  is included by path, so the HQ build points `-iquote` at an overlay
  include directory holding the patched header first.
- `MAX_DIRECTSOUND_CHANNELS` stays 12; `ENABLE_REVERB 1`, `ENABLE_DMA 1`
  (the file's values).  **Decided 2026-10-26** (the user): these values.
- `main.c`, `crt0.s`, `rom.ld`, `sound_data.s`, the micro voicegroup and
  songs: unchanged — the ROM boot-path rule holds (`AgbMain`
  instruction-identical; addresses shift because `m4a_1.o` changes size).

**Checks after the build** (step 1; results in "Step 1 — done" below and in
appendix B): `AgbMain` and the compiled `CgbSound`
and `MPlayMain` unchanged address-normalised (`objdump -d`, the vanilla
check); the new ROM symbol offset and the new IWRAM map (`SoundMainRAM_Buffer`
0xB40 moves `gSoundInfo`, the channels, `gPokemonCry*`, `gMPlayTrack_BGM`,
the `_081DCFB0`-equivalent stop label no longer exists — the probe's
channel-death breakpoint becomes the HQ labels `C_stop_channel` /
`C_mixing_end_and_stop_channel` and the `streqb`/`strb` status stores in the
pre-cached paths, at their IWRAM copy addresses); the mixer fits in 0xB40
(`SoundMainRAM_End − SoundMainRAM` from the map).  **The four canaries**:
render `sq1`, `pcm`, `pcm_cmp`, `cry_bulbasaur` with `mgba_render` from the
HQ ROM and compare against the vanilla references — sq1 identical means the
boot did not move; the PCM three are expected to differ (that is the
mixer).  Keep the HQ `.frames` summary of a probe run next to the vanilla
one: identical MPlayMain entries and CgbSound entry cycles is the
"timing model carries over" confirmation.

### The harness

- `accuracy.py --mixer vanilla|hq` (default vanilla): selects the ROM
  (`--rom` default `build/accuracy/rom-hq/...` for hq), passes
  `--hq-mixer` to `poryaaaa_render`, and writes `"mixer": "hq"` into the
  report.  References are already keyed by ROM hash, so the HQ references
  land in their own `refs/<hash-mgba>/` directory; the committed micro
  references get a sibling `refs/micro-hq/` with its own manifest
  (`--committed-refs` honours `--mixer`).
- **HQ reports are their own baseline family**: `baselines/<date>_hq-*.json`
  (and the `hq-midi-path-*` family for `--input midi`).  `compare_reports.py`
  refuses to compare reports whose `"mixer"` fields differ (as it should
  for `"input"`; add the check).
- `claim_check.sh --mixer hq` (or `MIXER=hq`): probe logs under
  `build/probe-hq/`, the HQ baseline, the HQ ROM.  The write-cycle diff uses
  the HQ probe logs.
- CMake: `accuracy_rom_hq`, `accuracy_hq` targets; `accuracy` stays vanilla.
  `poryaaaa_song_tests build/accuracy/rom-hq` must pass too (same songs,
  moved addresses — a free check of the relocation logic).
- The probe: `mgba_probe` takes the ROM and symbol file as arguments; the
  HQ run needs the HQ `syms.nm` plus the HQ mixer's internal labels
  (appendix B of `ACCURACY.md`: "moving addresses").  New probe
  breakpoints worth having: SoundMainRAM entry (its `sp` — the stack budget)
  and exit (the frame's mixer cost, for interest only), the three channel
  stop stores.

### Metric and gate

Unchanged: −40 dB on `null` and `worst1s`, no gain correction, −inf for
bit-identical.  The expectation for a correct port is the vanilla picture:
−inf on single-voice PCM micro-tests, the PCM floor on songs and cries,
the PSG rows exactly as today.

---

## 5. The engine in HQ mode

- **One flag**, `engine->hqMixer` (bool, default false), set through
  `m4a_engine_set_hq_mixer(engine, bool)`, which resets the HQ-only state
  (mix buffer, noise-shape bytes, every channel's `sampleStor`) and refuses
  / snaps `pcmFreqMode 2` (7884 Hz).  Off: the engine is byte-identical to
  today (the gate for every step: `claim_check` against the vanilla
  baseline, 0 moved, no row moved).
- **A new file**, `plugin/m4a_hq_mixer.{h,c}`: `m4a_hq_sound_main_ram(engine,
  frameOffset, pcmDmaCounter)` with the same signature and place as
  `m4a_sound_main_ram` (`m4a_engine.c` dispatches on the flag).  Written
  like `m4a_mixer.c`: the register names kept, every label as a comment,
  the condition-code quirks reproduced explicitly (a small
  "flags" struct or the equivalent booleans for N/Z/C/V where a conditional
  instruction depends on an earlier instruction's flags).  Header crediting
  ipatix and the source file (section 1).
- **New state**: `int32_t hqBuffer[GBA_PCM_DMA_BUF_SIZE]` sized to the
  largest frame (or `pcmSamplesPerVBlank` words, resized with the rate),
  `uint8_t noiseShapeLeft, noiseShapeRight` (SoundInfo 0xE / 0xF),
  `int8_t sampleStor` per PCM channel (`CHN_SAMPLE_STOR`), an engine
  `hqStackBudget` (bytes; default the test ROM's measured value, step 2),
  and the compressed-pointer convention (a sample index from init: the
  vanilla port already stores `data + index`, so the same representation
  serves; `CHN_SPECIAL` is unused in HQ mode).
- **The envelope step** stays `m4a_pcm_channel_tick` (identical state
  machine) with the HQ volume formula selected by the flag — or the HQ
  mixer computes its two volumes itself and stores them into
  `envelopeVolumeRight/Left` for `--pcm-log` (the log's columns keep their
  meaning: the per-side volumes the mixer used).
- **Synth voices**: the HQ mixer's synth loops are the measured truth; the
  vanilla-mode extension (`mix_synth`) keeps working as today.  Once the HQ
  synth is bit-exact, re-derive the extension from the shared code (same
  oscillator, vanilla accumulate) and record whatever changed.
- **Shadow pool / polyphony debug**: the HQ pass mixes the shadow channels
  into a discarded buffer as the vanilla pass does (`mutedFrame` → a muted
  HQ buffer); the invert mode's downsampler output is the audible one.
- **Where the flag surfaces**: `poryaaaa_render --hq-mixer`; the plugin's
  Options menu "HQ Mixer (ipatix)" with a tooltip, `poryaaaa.cfg` key
  `hq_mixer`, one CLAP state byte appended after the latch byte (absent
  loads as off), applied in `plugin_activate` / `state_load` / GUI
  `poll_changes` like `pcmFreqMode`.  The GUI's PCM-rate combo hides or
  greys 7884 Hz while HQ is on.
- **Interactions**: the opt-in extensions (portamento, PWM, base-key) are
  sequencer / CgbSound-side and orthogonal; XCMD 0x0D's start offset enters
  `count` as today and the HQ init consumes it (section 2); the cry player
  and SE players are untouched; `--pcm-log` gains `sampleStor` and the
  noise-shape bytes.
- **The MIDI path**: nothing changes — the mixer is after everything the
  MIDI path affects.  The `--input midi` HQ run should reproduce the
  vanilla picture (grid 43/44 = `--song` sample for sample) once `--song`
  is right.

---

## 6. Coverage: mixer code paths and the tests that reach them

Map each path to the rows that exercise it; compute a row's step ratio and
path from `--pcm-log` (frequency × divFreq / 2^23) rather than guessing.

| path | existing rows (expected) | to add (HQ-only micro-tests, `gen_micro_midis.py` + `accuracy_micro.inc`) |
|---|---|---|
| downsampler, noise shaping, seed = 0 | every PCM row | — |
| interpolating fast path, `< 1.0` | `pcm` (key 55), `pcm_bass`, `pcm_adsr` | — |
| `[1.0, 2.0)` | `pcm` (key 67?), `pcm_chord` | `pcm_ratio_1x` (a key with step just above 1.0) |
| `≥ 2.0` | `pcm_hi` (keys 84–96) | — |
| direct-from-ROM read (> 0x400 bytes or no stack) | none at 13379 Hz? (448 B max at ratio 2) | `pcm_hi` at a key with ratio > 4.6 (0x400 / 224) |
| unbuffered loop: loop wrap inside a frame | `pcm` (looped flute), `pcm_chord` | `pcm_short_loop` (a loop shorter than a frame's advance: multi-wrap) |
| unbuffered loop: sample end inside a frame | `pcm_adsr`, `pcm_drum`? | — |
| volume-zero freeze | none | `pcm_vol0` (VOL 0 for a few ticks mid-note, then back), `pcm_slow_attack` (attack 1–2) |
| FIX path (block + remainder, alignment variants) | `pcm_drum` (voice 12, `no_resample`); vanilla music uses no FIX DirectSound voices (0 in `sound/voice_groups.inc`; FRLG drumsets to check) | `pcm_fix_loop` (a looped FIX voice; the remainder loop's `loopStart` reset) |
| reverb seed | `pcm_reverb`, mus_littleroot, mus_vs_wild | `pcm_reverb_max` (`-R127`: the `>> 2` scaling) |
| saturation | none deliberate | `pcm_clip` (6+ notes at velocity 127, VOL 127: expect clipping, not wrap) |
| 16-bit half overflow / borrow | every row with negative samples | `pcm_clip` with a pan test |
| compressed forward | `pcm_cmp`, every forward cry | — |
| compressed end frame (zero fill, immediate stop) | `pcm_cmp`, cries with the sample ending (pidgey) | — |
| compressed reverse + the negative block | reversed cries | — (measure first) |
| uncompressed reverse | none in vanilla (reversed cries are DPCM) | `pcm_rev` (`voice_directsound` with the REV bit on an uncompressed sample; mid2agb cannot express it — a hand-written voice in the micro voicegroup) |
| stack budget exceeded | none | `pcm_cmp_hi` (a compressed sample at ratio ≥ 6: expect a frozen, never-ending silent channel) |
| synth pulse / saw / triangle, duty LFO | none | `synth_pulse`, `synth_pulse_lfo`, `synth_saw`, `synth_tri` — `.bin` descriptors in the micro voicegroup (the repo's `duty.bin` / `saw.bin` / `triangle.bin` layouts); **vanilla-ROM rows of these are not meaningful** (a click) and are skipped unless `--mixer hq` |
| frame lengths ≠ 224 | none | step 11 |

The micro-test additions change the ROM (both builds, since they share the
voicegroup): `--update-refs` for the vanilla micro set is then legitimate
(a ROM change) and the canary check applies.

---

## 7. The plan, step by step

Each step: what it needs, what it does, what it measures, what "done" is.
The gate on **every** step from 3 on: the vanilla configuration unchanged
(`claim_check.sh` against the latest vanilla baseline: 0 moved writes, no
row moved; unit / blip / song tests pass; poryaaaa and poryaaaa-standalone
build).  HQ numbers are reported against the latest `hq-*` baseline.

**Step 0 — decisions** (the user; no code).  **Done 2026-10-26.**  The HQ
ROM keeps `MAX_DIRECTSOUND_CHANNELS` 12, `ENABLE_DMA 1`, `ENABLE_REVERB 1`
(section 4).  HQ mode is a user-facing feature of poryaaaa and of
`poryaaaa_render`, opt-in and off by default, so downstream users (porydaw
included) can choose HQ-mixer audio; it is not a harness-only mode
(section 5, step 10).

**Step 1 — the HQ test ROM.**  Vendor the newmix `m4a_1.s`; the `MIXER=hq`
build (section 4); CMake targets; `poryaaaa_song_tests` on the HQ build.
Checks: `AgbMain` / `CgbSound` / `MPlayMain` unchanged address-normalised;
the mixer size vs 0xB40; the IWRAM map recorded in appendix B here; the four
canaries; the `ENABLE_DMA 0` variant's references `cmp`-equal to the
`ENABLE_DMA 1` ones on pcm / pcm_cmp / pcm_chord / mus_vs_wild (then the
variant is dropped).  Done: the HQ ROM renders every corpus row through
`mgba_render` (`accuracy.py --mixer hq` against the *current* engine reports
numbers; they are the "before" column — expect the PSG rows unchanged if
the boot did not move, the PCM rows failing).

**Step 1 — done 2026-10-26.**  `test/accuracy/rom/hq/` (`m4a_1.s` vendored
unchanged under a `SOURCE` comment, `m4a.c.sed`, `include/gba/macro.h`),
`make MIXER=hq`, CMake `accuracy_rom_hq` / `accuracy_song_tests_hq` /
`accuracy_hq`; `accuracy.py --mixer hq [--engine-mixer vanilla]`
(`report-hq.json`, `"mixer"` / `"engine_mixer"` fields, `refs/micro-hq`
committed with its manifest), `compare_reports.py` refuses mixed families,
`claim_check.sh --mixer hq`.  Checks: `AgbMain`, `main`, `StartTest`,
`IntrMain`, `m4aSoundInit`, `m4aSoundMain`, `m4aSoundVSync`, `SoundMain`,
`SoundMainBTM`, `MPlayMain`, `CgbSound`, `ply_note`, `TrkVolPitSet`,
`ChnVolSetAsm` identical address-normalised; only `SoundMainRAM` differs
(868 vs 371 disassembly lines).  `m4a.c` never uses `DmaSetUnchecked`, so
the `macro.h` overlay changes nothing in this ROM.  The mixer is 0xACC
bytes (`SoundMainRAM_End − SoundMainRAM`, 0x74 spare in the 0xB40 buffer).
`poryaaaa_song_tests build/accuracy/rom-hq`: 561/561.  Canaries against the
vanilla references: `sq1` byte-identical (the boot did not move as far as
the PSG can see; every one of the 17 pure-PSG rows is −inf against the
unchanged engine); `pcm`, `pcm_cmp`, `cry_bulbasaur` differ from the first
sound (0.0705 s).  The "before" column:
`baselines/2026-10-26_hq-before-vanilla-engine.json` (the vanilla engine
against the HQ ROM): 17/66, the 17 PSG rows unchanged, every DirectSound
row failing (micro −7.6 … −27.5 dB, songs −8.2 … −9.9, cries −6.6 … −21.4).
`ENABLE_DMA 0` is not output-neutral (section 3); the variant is dropped.

**Step 2 — measure the HQ ROM.**  Probe runs on the HQ ROM for the probed
corpus (`build/probe-hq/`).  (a) Boot: the IO-write log's cycles for every
event in `ACCURACY.md`'s boot timeline; the delta table.  (b) PSG: 
`probe_diff.py` of each probed test against the current engine's
`--psg-log` — expect 0 moved writes, or a constant offset explained by (a).
(c) MPlayMain / CgbSound entries per frame equal to the vanilla `.frames`.
(d) `sp` at SoundMainRAM entry (the stack budget) and the mixer's entry /
exit cycles on a few frames.  (e) The reversed-cry end frame: the open-bus
block's bytes (break on `F_decode_compressed` with `r9` out of range; log
`r9` and the 0x21 bytes read) on the three probed reversed cries.  (f) The
DMA3 lateness (section 3): on `pcm` frames 63–68, where the `ENABLE_DMA 0`
and `1` references differ, the IO-write / event log around the DMA3 copies
— which event (timer-0 overflow, FIFO DMA, the audio sample) is serviced
late, by how much, and whether the vanilla model's `.events` lateness
attribution covers it or the HQ timing model needs a DMA3 term.  Done:
the numbers in a commit and in appendix B here; nothing in the engine yet.

**Step 2 — done 2026-10-26.**  `tools/probe_corpus.sh` (the 67 probed
logs from one table: frames, players, cry parameters) run on both ROMs
into `build/probe-van/` and `build/probe-hq/` with the same probe build;
`tools/probe_hq_compare.py` compares them.  The probe gained the HQ
labels (broken at the IWRAM copy like `_081DCFB0`, every register logged,
`sp` at `SoundMainRAM_Buffer`, the decoded block buffer at
`C_select_highspeed_codepath_vla_r3`); a vanilla run is byte-identical to
before.  (a) Boot: m4aSoundInit's register block (SOUNDCNT_X/H, SOUNDBIAS,
the DMA setup, the first timer on @39794 → 39786, the MPlayExtender
writes, DMA off, timer off @47452 → 47444, DMA on @47893 → 47885) moves
+2080 then −8; the IRQ enables and the **final timer-0 enable move +4
(320682 → 320686)**; everything from frame 2's VSync DMA restart
(@590705) on is identical, cycle for cycle, in all 67 logs.  The on/off
tick count of the first timer pass is unchanged (7658), so the FIFO's
stale content is too; the +4 is a timer-0 phase shift against the DAC
grid — the vanilla precedent moved every DirectSound reference by 6, so
step 3 is needed.  (b) PSG: every `.frames` write list identical in all 67
logs.  (c) MPlayMain / CgbSound / VSync / SoundMain entries identical in
every frame of 66 logs; rg_vs_trainer differs in frames 732–735 only
(CgbSound entry −95 / +64 / +32 / +127): a DirectSound channel whose sample
runs out ends in frame 731 under the vanilla mixer (the count path, no
stamp; MPlayMain's `ClearChain` in 732) and in frame 732 under HQ
(`C_stop_channel`, 9189 cycles into the mixer; the unlink in 733), a
second one 734 vs 735 — the mixers end a sample a frame apart, which the
port reproduces by construction; no PSG write in those frames.  (d) `sp`
at SoundMainRAM entry 0x03007DD0 (0x03007DB8 in the runs with a second
cry or a cry over a song: the main thread's call depth when the VBlank
hits), budget 0x4D0 / 0x4B8 bytes above `stack_boundary`; the stack-skip
branch never taken (0 of 35 000 channel setups); mixer cost 7683 cycles
(no channel) to 39874 (littleroot).  (e) The reversed end frame: section
2, reproduced byte for byte.  (f) The floor: section 3.

**Step 3 — the boot table** (needed: 2(a) moved the final timer-0 enable
by +4 and the earlier blocks by +2080 / −8).  Parameterize
`m4a_engine_boot`'s table by mixer mode (the HQ cycles: 7800 / 7817 /
7840 / 17953, 39786, the MPlayExtender writes 39867–39927, 41230, 47444,
47885, 320686); HQ mode replays the HQ timeline.  Measure: the pure-PSG
rows (sq1, groudon, se_exp …) with `--hq-mixer` reach their vanilla
numbers (−inf where vanilla is −inf); `probe_diff` 0 moved on the HQ
logs; the PCM rows' numbers against the HQ ROM move (the timer phase)
but stay failing until step 4.  Vanilla unchanged.

**Step 3 — done 2026-10-26.**  `engine->hqMixer` (`m4a_engine_set_hq_mixer`,
`poryaaaa_render --hq-mixer`; nothing in the plugin yet, step 10) selects
`sBootTableHq` in `m4a_engine.c`: the same twenty steps as the vanilla
table at the HQ ROM's cycles (SoundInit's block 7800 / 7817 / 7840 /
17953, SampleFreqSet and MPlayExtender 39786 / 39867–39921 / 41230 /
47444 / 47885, the final timer-0 enable 320686), MPlayExtender's ten
writes spread 6 apart from 39867 as the vanilla table spreads them from
39875.  Nothing else reads the flag yet; the mixer dispatch comes with
step 4.  Measured: vanilla `claim_check` 0 moved writes on 59 logs, 57/66,
no row moved against 2026-10-25_cry-over-song-rows (every `-inf` row
`-inf`); HQ `claim_check --mixer hq` 0 moved writes on the 59 HQ logs,
17/66 against the before-baseline, the 17 pure-PSG rows `-inf` as they
already were with the vanilla engine (the boot's PSG writes are the same
registers and values, only earlier, and the first CgbSound is frame 2's
on both ROMs: the +4 moves the PCM timer phase only), the PCM rows
within 0.2 dB (pcm_drum −1.9) of before — the vanilla mixer's output
against the HQ reference, now on the HQ timer phase — still failing
until step 4.  Baseline `2026-10-26_hq-boot-table`.  Unit 1907/1907,
blip 49112/0, songs 561/561.

**Step 4 — the HQ mixer skeleton: downsampler, seed, the `< 1.0` fast
path.**  `m4a_hq_mixer.c` with the frame structure of section 2, the
volume formula, `sampleStor`, the noise shaping and saturation, `reverb
== 0` seeding, the fast path's `< 1.0` block and the unbuffered loop (loop
wrap and end), the volume-zero skip.  No FIX / CMP / REV / synth yet (those
channels are skipped with a counter in the log).  Measure: `pcm_bass`,
`pcm_adsr`, `pcm` → −inf predicted where the paths are covered; `vol`,
`pan`, `vel`, `bend`, `lfo_*` (PCM rows) likewise.  Unit tests: the
`test_mixer_arithmetic` pattern with hand-built channels (the saturation
formula, the noise-shaping state across a frame boundary, the borrow quirk,
the `diff == 0` quirk, the volume-zero freeze), each checked against the
assembly by hand in the test's comment.

**Step 4 — done 2026-10-26.**  `plugin/m4a_hq_mixer.{h,c}` (ported from
the newmix file, credit ipatix; registers kept as names, every label a
comment, flags as named booleans): `m4a_hq_channel_init` (the SF_START
part the vanilla start pass lacks: CMP from the sample's flag, the reverse
pointer, `sampleStor`), `m4a_hq_channel_volumes` (`>> 13`), `m4a_hq_mix_channel`
(C_mixing_setup's volume-zero skip and halving, the `umlal` advance, the
fast loop's `< 1.0` block with the `mulne` / iteration-boundary flags, the
unbuffered loop with its relative loop wrap and silent end),
`m4a_hq_downsample` (saturation, the 7-bit residuals in
`engine->hqNoiseShapeLeft/Right`, the four taps and the `reverb >> 2`
seed with the packed-word borrow, written back into `engine->hqBuffer`)
and `m4a_hq_sound_main_ram` (the frame; the shadow pool into
`hqMutedBuffer`).  `sound_main` dispatches on `engine->hqMixer`;
`m4a_engine_set_hq_mixer` resets the HQ state and snaps `pcmFreqMode 2`.
A channel on a path not ported yet (FIX, CMP / REV, synth, the `>= 1.0`
blocks) is left untouched for the frame and counted in
`engine->hqUnportedChannelFrames` (the renderer prints the count).
`--pcm-log` adds `ss= cnt= fw=` per channel and `ns=` per frame in HQ
mode; `HQ_DEBUG=1` in the environment dumps the mix buffer and the ring
slot per frame, which `tools/hq_ring_compare.py` sets against the probe's
new `ringA= / ringB=` dump at `C_downsampler_return`; `tools/frame_diff.py`
finds the differing output frames.  Measured: vanilla `claim_check` 0
moved on 59 logs, 57/66, no row moved.  HQ 29/66 (from 17): the 17 PSG
rows and `vol`, `vel`, `lfo_vib`, `lfo_trem`, `lfo_pan`, `tune` at −inf,
`pcm` −71.4, `pcm_reverb` −72.3, `pcm_chord` −88.1, `pan` −74.7, `bend`
−76.1, `lfo_delay` −66.1 (the floor rows, section 3; their ring slots equal
the ROM's in all 177 frames), `se_faint` −49.2.  Predicted −inf for `pcm`,
`pcm_bass`, `pcm_adsr`: `pcm` is the floor; `pcm_bass` (step 1.26) and
`pcm_adsr` (step 1.0 exactly) are `>= 1.0` rows, step 5's.  One bug found
by measurement: the iteration-boundary flags (section 2).  Every row with
an unported channel renders that channel silent, so those rows read 0 dB
(the cries, pcm_cmp / drum / hi / bass, the songs) — not regressions, the
step 5–9 queue; and seven PSG logs (the five songs, cry_over_se / bgm) show
moved writes (93–532 cycles) because a frozen channel never ends and
MPlayMain's channel bookkeeping costs differ: the HQ "0 moved" gate holds
for every log without unported channel-frames (the 26 others) and is
expected back once the paths exist.  Baseline `2026-10-26_hq-mixer-skeleton`.
Unit tests `test_hq_mixer` (40 assertions: volumes, init, the fast block,
the diff-0 quirk across an iteration boundary, volume zero, the unbuffered
end and wrap, the downsampler's residual carry and both saturations, the
seed and its borrow, the frame and the unported count); unit 1947/1947,
blip 49112/0, songs 561/561.

**Step 5 — the `[1.0, 2.0)` and `≥ 2.0` blocks, direct-from-ROM reads.**
`pcm_hi`, `pcm_chord`, the new `pcm_ratio_1x` / `pcm_short_loop` rows.

**Step 6 — the FIX path.**  `pcm_drum`, `pcm_fix_loop`; se_faint's snare and
rg_vs_trainer's drumset if they are FIX (check the voicegroups).

**Step 7 — reverb.**  The seed arithmetic and the one-frame delay;
`pcm_reverb`, `pcm_reverb_max`, mus_littleroot, mus_vs_wild, mus_credits.
Prediction to write down first: the songs reach their vanilla floor
(−49..−55 dB); a level error shows as `gm` ≫ raw.

**Step 8 — compressed and reversed samples.**  Forward DPCM with the zero
fill and immediate stop (`pcm_cmp`, forward cries), then the reversed
paths with step 2(e)'s measurement in hand: decide whether the negative
block is reproduced (mgba's open-bus value is deterministic per ROM) or
recorded as a residual, as the vanilla reversed-cry end is.  The stack
budget skip (`pcm_cmp_hi`).  Rows: every cry and `pcm_rev`.

**Step 9 — synths.**  The four synth micro-tests in the HQ ROM; the HQ
synth loops bit-exact; then the vanilla-mode extension re-derived from the
shared oscillator code (its own unit tests kept).

**Step 10 — the plugin, renderer, config, state, docs.**  Section 5's
surfaces; `--input midi --mixer hq` run and its baseline family; README and
`docs/ACCURACY.md` cross-references; the `accuracy-harness` and
`review-against-source` skills learn `--mixer hq`; this document rewritten
from plan to reference as the facts settle.

**Step 11 — rates other than 13379 Hz** (optional, later).  An HQ ROM variant
per rate needs `hq_buffer_ptr` resized and, with reverb, `PCM_DMA_BUF_SIZE`
at 7 frames (appendix C) — the ring size then differs from the engine's
constant 1584, which becomes a per-mode value.  Since the vanilla effort
did not measure other rates either, this is a shared gap.

**Step 12 — review** (`review-against-source`): two independent reviews of
`m4a_hq_mixer.c` against the newmix assembly, every claim tested one at a
time with `claim_check.sh --mixer hq`; then the HQ baseline is final.

---

## 8. Risks and expected residuals

- **The PCM floor** stays (mgba scheduler lateness), possibly at other
  levels and on other rows than vanilla; confirm with `.events`.  Step 1
  showed the DMA3 copies alone move `mus_vs_wild` by −48 dB null / −40 dB
  worst second between the `ENABLE_DMA` variants (section 3), so the HQ
  floor on DirectSound-heavy rows may touch the gate.  Step 2(f) found the
  same mechanism at the same scale on both ROMs: no DMA3 term.
- **Reversed DPCM end frame**: two open-bus blocks and the real tail
  cleared (section 2), reproduced from the mixer's own code bytes; a
  residual only if step 8 chooses not to port it.
- **The stack budget** is ROM-dependent (the game's `sp` at the interrupt
  varies); the engine's default is the test ROM's.  Document as a
  limitation; no vanilla corpus row should reach it.
- **Compressed loops are not looped** in HQ — a behaviour difference hackers
  may hit with looped compressed samples; stock HQ behaviour, reproduce it.
- **The eventide synth extension** may turn out to differ from the measured
  HQ synth in rounding (e.g. the saw's `mlane` zero skip, the pulse's DC);
  step 9 settles it, and the vanilla-mode extension follows the measured
  code.
- **IWRAM layout / probe labels** all move; every probe recipe in the
  skills needs the HQ addresses alongside the vanilla ones.
- **Other HQ variants in the wild**: ipatix's master branch (`ENABLE_REVERB
  0` default, `POKE_CHN_INIT`), older revisions, pokeemerald-expansion's
  copy, eventide's.  This effort targets the wiki's file; differences in
  other copies are out of scope until measured.

---

## 9. Working rules for this effort

All of `ACCURACY.md` section 7, plus:

- **The vanilla configuration is frozen.**  No step may move a vanilla
  write or row; HQ code lives behind the flag and in its own file.
- **One mixer path per step**, each with a row that reaches it and a
  predicted number written before the run.
- **Port the instruction, not the intent**: condition codes, packed-word
  borrows, truncation order, the skipped-channel cases.  Where a quirk is
  reproduced, the code comment names the instruction and label.
- **HQ baselines are their own family**; never compare across `"mixer"`.
- **The ROM is built as the wiki says.**  No "fixes" to the mixer source,
  even obvious ones (the forward-compressed loop TODO, the negative block):
  the target is what hackers ship.
- **Read the newmix file**, not ipatix's master or eventide's copy, for
  every "the mixer does X"; cite the label.

---

## Appendix A: HQ mixer source map

newmix `src/m4a_1.s` lines 88–1210 (`SoundMainRAM` … `SoundMainRAM_End`).
Stack frame (SoundMain's, unchanged): `ARG_FRAME_LENGTH` 0x0,
`ARG_REMAIN_CHN` 0x4, `ARG_BUFFER_POS` 0x8 (SoundMain's r5), `ARG_LOOP_START_POS`
0xC, `ARG_LOOP_LENGTH` 0x10, `ARG_BUFFER_POS_INDEX_HINT` 0x14 (SoundMain's r4 =
`pcmDmaCounter`), `ARG_PCM_STRUCT` 0x18 (SoundInfo).  SoundInfo fields used:
`VAR_REVERB` 0x5, `VAR_MAX_CHN` 0x6, `VAR_MASTER_VOL` 0x7,
`VAR_EXT_NOISE_SHAPE_LEFT/RIGHT` 0xE / 0xF, `VAR_DEF_PITCH_FAC` 0x18
(`divFreq`), `VAR_FIRST_CHN` 0x50, `VAR_PCM_BUFFER` 0x350.  Channel:
`CHN_SAMPLE_STOR` 0x3F.  Synth descriptor bytes after the WaveData header:
type 1, base duty 2, width change 1 at 3, mod amount 4, width change 2 at 5.

| label | role |
|---|---|
| `SoundMainRAM` (Thumb) | saves the hint, frame length, loads divFreq and maxChans, enters the channel loop |
| `C_channel_state_loop` | per channel: status test (`0xC7`), START → init, else ADSR |
| `C_channel_init_*` (`_synth`, `_noncomp_forward`, `_noncomp_reverse`, `_comp`, `_comp_forward`, `_comp_reverse`, `_check_loop`) | section 2 "Channel init" |
| `C_adsr_echo_check`, `C_adsr_release_check`, `C_adsr_released`, `C_adsr_decay_check`, `C_adsr_attack_check`, `C_adsr_attack`, `C_adsr_next_state`, `C_adsr_save_and_finalize` | the envelope state machine |
| `C_stop_channel`, `C_skip_channel` | status 0 / skip |
| `C_channel_vol_calc` | `env × (mv + 1)`, `>> 13` per side → r10 left, r11 right; loop setup (`ARG_LOOP_START_POS`, `ARG_LOOP_LENGTH`) |
| `C_mixing_setup` (ARM) | `sampleStor` → r6, packed volume r11 = L<<16 \| R, zero-volume skip, path dispatch |
| `C_mixing_setup_comp_rev` | step = freq × divFreq; synth test; volume halving; `advance` via `umlal`; dispatch to `C_data_load_comp` / `_uncomp_rev` / `_uncomp_for` |
| `F_decode_compressed`, `C_bdpcm_decoder_loop`, `bdpcm_instructions`, `bdpcm_instruction_resource_for/rev`, `delta_lookup_table` | DPCM block decode onto the stack, forward or reversed store (self-modified) |
| `stack_boundary_literal` | `0x03007900` |
| `C_data_load_comp`, `_comp_for`, `_comp_rev`, `_comp_decode`, `_comp_loop` | block range, stack check, zero fill at the end, `streqb` status 0, decode |
| `C_data_load_uncomp_rev`, `_uncomp_rev_loop` | word byte-swap onto the stack; status 0 at the end |
| `C_data_load_uncomp_for` | `count > advance` test; stack / 0x400 test; DMA3 or CPU copy (`ENABLE_DMA`) |
| `C_select_highspeed_codepath*` | block selection by step (`previous_fast_code`), volume doubling for ≥ 1.0, code paste |
| `C_fast_mixing_loop`, `fast_mixing_instructions`, `high_speed_code_resource` | the 8-sample unrolled loop and the three 6-instruction blocks |
| `C_unbuffered_mixing`, `_loop`, `_skip_load`, `_loop_or_end` | one sample per iteration with loop / end handling |
| `C_end_mixing` | store `fw`, `sampleStor`; → `C_mixing_end_store` (count, pointer) |
| `C_mixing_end_and_stop_channel` | status 0 (the `strb r0, [r4]`), no state store |
| `C_setup_fixed_freq_mixing`, `C_fixed_mixing_length_check`, `_loop`, `fixed_mixing_instructions`, `fixed_mixing_code_resource`, `_process_rest`, `_unaligned_loop`, `_loop_or_end` | the FIX path |
| `C_setup_synth`, `C_synth_pulse_loop`, `C_check_synth_saw`, `C_synth_saw_loop`, `C_synth_triangle`, `C_synth_triangle_loop` | the synth paths |
| `F_clear_mem` | zero fill (returns with Z set when the length is a multiple of 4) |
| `C_end_channel_state_loop`, `C_main_mixer_return` | next channel; load noise-shape state, reverb >> 2, the hint |
| `C_downsampler`, `C_downsampler_loop`, `C_downsampler_return` | section 2 "Frame structure"; `ID_NUMBER` store; SoundMain's epilogue |

Register conventions in the ARM mixing code: r2 count, r3 pointer, r4 step,
r5 mix buffer, r6 base sample, r7 fine position, r8 samples left, r11 packed
volume, r12 diff.

## Appendix B: arithmetic reference (as read; confirm per step)

- Volume: `v = env × (mv + 1)`; `L = (leftVolume × v) >> 13`, `R =
  (rightVolume × v) >> 13`; packed `P = L << 16 | R`.  Halved: `P' =
  (P >> 1) + carry + 0x8000` then `& ~0x8000` ⇒ `ceil(L/2) << 16 | ceil(R/2)`.
  Doubled for step ≥ 1.0: `P' << 1`.
- Accumulate: `word += P × s` (32-bit `mla`; `s` the signed sample value at
  the path's scale).
- Interpolation (`< 1.0`): `s = ((p × diff) asr 22) + 2 × base`, `p` the
  23-bit position; (`[1, 2)`, `≥ 2`): `s = base + ((p × diff) asr 23)`.
- Advance per frame: `(fw + step × n) >> 23`; fast path iff `count >
  advance`.
- Reverb seed per sample: `sum = A_next + B_next + B_cur + A_cur` (int8
  taps), `seed = sum × (rv | rv << 16)`, `rv = reverb >> 2`.
- Downsample per side: `t = state + w` (left) or `state + (w << 16)`
  (right); `t2 = t + t`; if overflow `t = 0xC0000000 ^ (t2 asr 31)`; `out =
  (t >> 23) & 0xFF`; `state = t & 0x7F0000`.
- Synth: pulse `s = (phase < duty) ? +64 : −64` at unhalved `P`; triangle
  `s = (phase' ≥ 0) ? (phase' >> 23) − 0x80 : 0x180 − (phase' >>> 23)` after
  `phase' = phase + (step << 3)`; saw `raw = (phase' >> 24) − 0x70 −
  ((phase' << 1) >>> 27)`, `state = raw + (state asr 1)`, `s = state` at
  `floor(P/2)` with `& ~0xFF00`.  Duty per frame: `count += w1 << 24`;
  `t = count + (w2 << 24)`; `t = ~t` if negative; `duty = (base << 24) +
  (t >> 8) × mod`.
- Test ROM stack at the interrupt: `sp_sys = 0x03007E40` (`crt0.s`), the IRQ
  dispatcher in system mode pushes `lr`, then `VBlankIntr` → `m4aSoundMain`
  → `SoundMain` (0x40 bytes of frame) → SoundMainRAM; measure the exact `sp`
  in step 2(d).  Budget = `sp − 0x03007900 − (bytes already pushed by the
  path)`.
- IWRAM map of the HQ ROM (step 1; vanilla → HQ): the vanilla `m4a_1.o`'s
  0x40-byte `.bss` (`sDecodingBuffer`, the DPCM decode buffer) is gone, so
  `sound_data.o`'s BSS starts at 0x03000000: `gMPlayTrack_BGM` 0x03000040 →
  0x03000000, `gMPlayTrack_SE1` 0x03000360 → 0x03000320, `gMPlayTrack_SE2`
  0x03000450 → 0x03000410, `gMPlayTrack_SE3` 0x03000720 → 0x030006E0;
  `SoundMainRAM_Buffer` 0x03000770 → 0x03000730 (0xB40 long),
  `hq_buffer_ptr` 0x03001270 (0x380); `gSoundInfo` 0x03000F70 → 0x030015F0,
  `gPokemonCrySongs` 0x03001F20 → 0x030025A0, `gPokemonCryMusicPlayers`
  0x03001F88 → 0x03002608, `gMPlayInfo_BGM` 0x03002008 → 0x03002688,
  `gCgbChans` 0x030020D8 → 0x03002758, `gMPlayInfo_SE1` 0x030021D8 →
  0x03002858, `gMPlayInfo_SE2` 0x03002218 → 0x03002898, `gPokemonCryTracks`
  0x03002258 → 0x030028D8, `gPokemonCrySong` 0x03002398 → 0x03002A18,
  `gMPlayMemAccArea` 0x030023CC → 0x03002A4C, `gMPlayInfo_SE3` 0x030023DC →
  0x03002A5C.  ROM: `AgbMain` 0x08000370, `m4aSoundInit` 0x08000474,
  `CgbSound` 0x080010AC, `SoundMain` 0x08001C70, `SoundMainRAM` 0x08001CF4
  unchanged; `SoundMainBTM` 0x080023F8 → 0x080027C0 (the vanilla mixer region
  `SoundMainRAM` … `SoundMainBTM` is 0x704 bytes, the HQ one 0xACC: +0x3C8),
  `MPlayMain` 0x08002670 → 0x08002A38, `gCgb3Vol` 0x08002F98 → 0x08003360:
  every symbol after the mixer moves by +0x3C8.
  The HQ ROM's sha256 starts `bf7c9463fc8c` (its reference cache key).
- Boot timeline of the HQ ROM (step 2(a); vanilla → HQ, cycles from
  reset): SOUNDCNT_X 0x8F 5720 → 7800, SOUNDCNT_H 0xA90E 5737 → 7817,
  SOUNDBIAS_H 0x40 5760 → 7840, DMA on 15873 → 17953 (+2080: the larger
  CpuCopy32); timer on 39794 → 39786, MPlayExtender's PSG writes
  39875–39935 → 39867–39927, DMA off 41238 → 41230, timer off 47452 →
  47444, DMA on 47893 → 47885 (−8); timer on 320682 → **320686**, DISPSTAT
  323278 → 323282, IE / IME 323293 / 323299 → 323297 / 323303 (+4); from
  frame 2's VSync (DMA restart @590705) on, identical.  `sp` at
  SoundMainRAM entry 0x03007DD0 (0x03007DB8 with a second cry / a cry over
  a song).  Mixer cost: 7683 cycles with no channel (7 skipped channels
  and the downsampler), up to 39874 (littleroot).

## Appendix C: the wiki's tables

Frame length (samples per VBlank = `hq_buffer_ptr` words) by engine rate:
5734 Hz 0x60, 7884 Hz 0x84 (unsupported), 10512 Hz 0xB0, 13379 Hz 0xE0
(default), 15768 Hz 0x108, 18157 Hz 0x130, 21024 Hz 0x160, 26758 Hz 0x1C0,
31536 Hz 0x210, 36314 Hz 0x260, 40137 Hz 0x2A0, 42048 Hz 0x2C0.  These
equal `gPcmSamplesPerVBlankTable`.

`PCM_DMA_BUF_SIZE` for 7 frames at a raised rate with reverb: 5734 Hz 672,
7884 Hz 924 (unsupported), 10512 Hz 1232, 13379 Hz 1568 (the vanilla define
is 1584 and stays), 15768 Hz 1848, 18157 Hz 2128, 21024 Hz 2464, 26758 Hz
3136, 31536 Hz 3696, 36314 Hz 4256, 40137 Hz 4704, 42048 Hz 4928.

The wiki's `DmaSetUnchecked`: `register` r0/r1/r2 bindings and one
`asm volatile("stmia %0!, {%1, %2, %3}")` with a `"memory"` clobber in place
of the three stores.
