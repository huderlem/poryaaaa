# poryaaaa HQ-mixer accuracy: how it works

poryaaaa reproduces vanilla pokeemerald's m4a engine closely enough to null
against mgba 0.10.5 (`docs/ACCURACY.md`).  This document describes the
same result for a second engine configuration: pokeemerald with **ipatix's
High-Quality Audio Mixer** installed as the pret wiki describes
(<https://github.com/pret/pokeemerald/wiki/Implementing-ipatix's-High-Quality-Audio-Mixer>),
selectable in poryaaaa as "HQ mixer mode" (`engine->hqMixer`; the plugin's
**Options → HQ Mixer (ipatix)**, `hq_mixer` in `poryaaaa.cfg`,
`poryaaaa_render --hq-mixer`), off by default.

Everything the vanilla effort settled (ground truth, metric, harness,
probe, the sequencer, CgbSound, the timing model, the MIDI path, the
working rules) carries over unchanged and is not repeated here; read
`docs/ACCURACY.md` first, then this.  Sections 1 to 3 describe the mixer
as it was read from the assembly **and then measured**: every "the mixer
does X" below was confirmed against the HQ test ROM in the step that
ported it (section 7), and where the reading was wrong the measured
behaviour replaced it, with the correction noted.  The plan this document
grew from, with each step's measurements, predictions and rejected
claims, is in git history (commits "HQ mixer step 1" to "step 12",
2026-10-26 / 27) and summarised in section 7.

Contents:

1. The HQ mixer: source, version, configuration, what the wiki changes
2. How it differs from SoundMainRAM (the port)
3. What carries over unchanged, and what moved
4. Ground truth for HQ mode: the HQ test ROM and the harness
5. The engine in HQ mode
6. Coverage: mixer code paths and the rows that reach them
7. How it was done: the steps
8. Known residuals and limitations
9. Working rules for HQ mode
- Appendix A: HQ mixer source map
- Appendix B: arithmetic reference (measured)
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
   (its frame, 0x84, is not a multiple of 8).  The HQ test ROM's default
   build stays at 13379 Hz; `make RATE=<hz>` builds the variant at another
   rate with these two sizes applied (section 4; measured at 21024 Hz, the
   wiki's own example rate, in step 11).
6. `DmaSetUnchecked` in `include/gba/macro.h` writes the three DMA registers
   with one `stmia` (the mixer uses DMA3; the game's own DMA3 users must not
   be interrupted between register writes).  The test ROM has no other DMA3
   user; apply it anyway so the ROM is built "as the wiki says".
7. Mixer configuration (`.equ` at the top of the mixer): `ENABLE_REVERB`
   (1: the m4a reverb; 0: none, and the mix buffer is cleared instead of
   seeded), `ENABLE_DMA` (1: DMA3 copies sample data to the stack; 0: a CPU
   copy loop).  Both 1 in newmix.

---

## 2. How it differs from SoundMainRAM (the port)

This is the mixer read side by side with `plugin/m4a_mixer.c` (the vanilla
port) and `m4a_1.s` lines 88–1210, as `plugin/m4a_hq_mixer.c` ports it
(every path measured −inf or at the scheduler floor on the rows of section
6).  Appendix A maps the labels; appendix B has the formulas.  Items
marked **(quirk)** are behaviours that follow from the instruction
encoding rather than the intent and are ported as the instructions do
them — the kind of thing a "cleaned-up" translation loses and the harness
then fails on; each such item names the instruction in the code's comment.

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
  **Confirmed (step 7)**: with the FIX drums mixed, mus_littleroot,
  mus_vs_wild (reverb-heavy) and mus_credits have their ring slots
  byte-identical to the ROM's over the whole probe (1193 / 1193 / 596
  frames) — the taps, the `>> 2` scaling, the borrow and the one-frame
  delay as written here, at the games' reverb levels.
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
  Ported and unit-tested (the synth LFO test covers a volume-zero frame);
  no dedicated harness row reaches it (section 6).
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
  `≥ 2.0` (pointer += `fw >> 23` each sample).  **Confirmed (step 5)**:
  the selection leaves `r4 = step − 2.0` for the middle block (the
  `adds r7, r7, r4` is PL exactly when the fraction plus the step reaches
  2.0: the second load, `ldrplsb r6, [r3, #1]!`) and `r4 = step − 1.0` for
  the top one (`add r3, r3, r7, lsr#23` moves the pointer, which is one
  past the base, by the whole samples minus one); both double the halved
  volume back (`movpl r11, r11, lsl#1`: `2 × ceil(v / 2)` per side, so an
  odd volume rounds up) and mix `base + ((fraction × diff) asr 23)`; in
  every block the fraction `r7` is a plain 23-bit value at the multiply
  (the `bic` after each sample), the `< 1.0` block's `p − 2^23` being only
  the `adds r7, r7, r4` carry trick.  The block is pasted over
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
  sample until the port did the same.  In the two `≥ 1.0` blocks the
  flags come from `subs r12, r12, r6` (the new diff), executed every
  sample, so the skip follows every advance onto a flat run (one unit low
  at single scale, `asr #23` of a negative output) rather than every
  other sample; the iteration rule is the same (step 5, unit-tested).
  Port the condition codes, and the loop structure they live in.
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
sample.  **Confirmed (step 6)**: `C_fixed_mixing_length_check` takes
`(min(count, left) − 1) >> 2` blocks of four, the word loads ignore the
pointer's low two bits and the pasted code picks the bytes by that
alignment, the pointer is rewound to the next unread byte after the
blocks, and the remainder is always four samples (`left` stays a multiple
of 4), so a frame mixes exactly `frameLength` samples or stops at the
data's end — a per-sample model with the loop reset and the stop at
`count == 0` is exact.  The block's `mlane` skips a zero sample (a no-op);
the remainder's `mla` does not.  `fw` and `sampleStor` are not written
(the path skips `C_end_mixing`).  The FIX test precedes the synth test in
`C_mixing_setup`, but no synth reaches it: `C_channel_init_synth` replaces
the channel's type byte with `TONEDATA_TYPE_SPL` (an earlier reading here,
"a FIX synth voice takes this path", was wrong — corrected in step 9).

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
  previous frame's real blocks.  **Ported (step 8)**: the port reproduces
  the end frame from the mixer's own code bytes (the three prefetch words
  and the 256-byte table view are constants of the image).  **Measured
  (step 8, `cry_pidgey_echo_start_chorus` frame 12)**: once the frame's
  first block is before the sample, *every* block of that frame is read
  from open bus — the decode loop steps the wrapped address by 0x21, so
  the real blocks 0, 1, … are read at `data + 0x84000000 + 0x21 k`, never
  at `data + 0x21 k`; a port that only wraps the negative blocks plays the
  real ones where the ROM plays prefetch garbage.  The lane of byte `j` of
  block `k` is `(j + k) & 3` in both cases (the WaveData is word-aligned
  in ROM, `0x21 ≡ 1 mod 4`).
- **The last block's tail (step 8)**: a cry's `.bin` stops at the
  look-ahead sample (Bulbasaur: 29 of the last block's 33 bytes), but the
  mixer decodes the whole block, so its remaining bytes are whatever
  follows the `.incbin` in the ROM: `.align 2` zeros, then the next
  entry's 16-byte header (`01 00 00 00 …` for the next cry).  Vanilla
  never outputs those samples (indices ≥ size); the HQ mixer plays them
  as the sample runs out (a few samples of ±1), and — the larger effect —
  the downsampler's 7-bit residual then stays shifted through the
  following silence, so every later note's rounding differs by one (a
  pcm_cmp second note at −29 dB, cry_over_bgm's song at −33).  The loader
  fills a truncated last block from the next entry's file in
  `direct_sound_data.inc` order (`symbol_map_next_path`); all 20
  compressed rows went from 0/−11..−57 dB to −inf or the floor.
- **Before the sample (step 8)**: `C_data_load_uncomp_rev`'s end frame
  reads from `(idx − advance − 1) & ~3`, i.e. before `data`: the entry's
  own header, the padding and the previous entry's tail (up to
  `advance + 4` bytes, ≤ 1132 at 13379 Hz).  The loader keeps up to 1280
  of those bytes in front of the data (`WaveData.romBefore`, from the
  previous entry's `.bin` in the same order; a `.wav`-loaded sample is
  re-laid-out with the header rebuilt from its fields, which the `.bin`
  carries byte for byte); without them the header alone is rebuilt and
  the rest reads 0.  `pcm_rev` −31.9 → −inf.
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
  the test ROM's value as the default.  **Done (step 8)**:
  `engine->hqStackSp` (default `0x03007DD0`, `poryaaaa_render
  --hq-stack-sp`); the check sees `sp − 12` (the one `stmfd {r4, r9,
  r12}` before it), so the budget is 1220 bytes: 19 blocks of a
  compressed frame pass, 20 skip.  `pcm_cmp_hi` (the Bulbasaur cry at
  keys 96 / 72 / 94: ratios 6.3 / 1.6 / 5.6) plays only its middle note
  on the HQ ROM, the other two frozen silent, and reads −inf; the vanilla
  ROM plays all three (−43.6).  A skipped channel's `C_end_mixing` stores
  the unchanged registers, so nothing moves until the envelope ends the
  note.

### The synth paths (SPL)

Ported in step 9 (`plugin/m4a_synth.h` holds the oscillators, shared with
the vanilla-mode extension; `synth_path` in `m4a_hq_mixer.c` the loops),
measured −inf on the four synth rows at the first run:

- Reached from `C_mixing_setup_comp_rev` after the step multiply and
  **before the volume halving**: the pulse and triangle mix at the
  unhalved `v` (0..127 per side), the saw halves it itself
  (`lsr #1; bic #0xFF00` → `floor(v / 2)`).  `r3` is the sample pointer =
  the descriptor; `C_mixing_end_store` writes it back unchanged and
  `count` as the pulse's LFO accumulator / the saw's filter state.
- **Pulse**: `C_setup_synth` runs once per *mixed* frame (a frame the
  volume-zero skip drops does not advance the LFO): `count += data[3] <<
  24`, `t = count + data[5] << 24`, `mvnmi` folds a negative `t` to `~t`,
  threshold `= (t >> 8) × data[4] + data[2] << 24`.  Per sample `cmp r7,
  r6` (unsigned, *before* the advance) then `addlo / subhs r11 << 6`:
  `±64 × v` at the packed word, the subtract borrowing across the halves
  exactly as `mla` with −64.  `sampleStor` gets the threshold's low byte.
- **Saw**: the advance first, `raw = (phase >> 24) − 0x70 − ((phase << 1)
  >> 27)`, `state = raw + (state asr 1)`, `mlane` with the state itself
  (skipped when 0); `sampleStor` = the low byte of the last `phase << 1`.
- **Triangle**: the advance first, the sign of the *advanced* phase picks
  `(phase >> 23) − 0x80` or `0x180 − (phase >> 23)` (an earlier reading
  here said "pre-advance": the flags are the `adds` result's); the peak at
  exactly 0x80000000 is +128; `sampleStor` = 0x80.
- 8 (pulse) or 4 samples per iteration, so the frame length must be a
  multiple of 8 (the 7884 Hz snap).

The extension (written from this code and cross-checked with agbplay)
differed in two places, both re-derived in step 9: its pulse LFO advanced
at every envelope tick and once more at note start (one step ahead of the
mixer, and advancing through volume-zero frames), and its saw mixed
`state >> 1` at the full volume where the mixer mixes the state at
`floor(v / 2)`.  Measured against the HQ ROM, the re-derived extension
(the same oscillators under vanilla's 8-bit accumulate) sits at −32..−34
dB on the four rows: the mixer difference, informational.

### Volume-zero and frame-length constraints

Frame lengths must be multiples of 8 (the fast loop, the pulse loop) —
every table rate but 7884 Hz is.  `m4a_engine_set_hq_mixer` and
`m4a_engine_set_pcm_freq_mode` snap `pcmFreqMode 2` to the default in HQ
mode; the plugin snaps its own setting the same way and its PCM Mix Rate
combo omits 7884 Hz while the mode is on.

---

## 3. What carries over unchanged, and what moved

**Unchanged by construction** (the sources outside the mixer are identical):
MPlayMain and every command handler, ply_note's channel scan and steal rule,
CgbSound and its register sequences, the SoundMain timing model and every
measured cost in appendix A of `ACCURACY.md` (the mixer runs *after*
CgbSound; nothing later in the frame is timed), `m4aSoundVSync`'s DMA
restart and `pcmDmaCounter`, the FIFO / timer / DAC output stage, the MIDI
path and the plugin's latch rules, the `.s` parser and the loader layouts.
**Measured (step 2)**: the HQ ROM's PSG write lists are identical to the
vanilla ROM's in all 67 probed logs, and the MPlayMain / CgbSound / VSync
/ SoundMain entry cycles are identical in every frame but four of
rg_vs_trainer (a run-out sample the two mixers end a frame apart, which
the port reproduces by construction).

**What moved: the boot timeline.**  `m4aSoundInit`'s `CpuCopy32` copies
0xB40 instead of 0x800 bytes (+0x340), and the crt0's BSS clear covers
more IWRAM, so m4aSoundInit's register block lands +2080 cycles later,
the SampleFreqSet / MPlayExtender block −8, and the **final timer-0
enable +4 (320682 → 320686)**; everything from frame 2's VSync DMA
restart (cycle 590705) on is identical, cycle for cycle.  The first
handled interrupts stay frame 2's, the first timer pass's on/off tick
count is unchanged (7658, so the FIFO's stale content is too); the +4 is
a timer-0 phase shift against the DAC grid, invisible to the PSG (the sq1
canary is byte-identical) and worth one DAC step on every DirectSound
reference.  `m4a_engine_boot` replays the HQ table (`sBootTableHq`,
section 5) when the flag is set.

**The PCM floor** is the same mechanism at the same scale: mgba's
scheduler lateness depends on the instruction stream, and the HQ mixer's
DMA3 halts and loop costs move where timer-0 overflows land late, so the
set of −inf rows differs from vanilla's and the floor on songs sits at a
different level.  Not modeled, as before; attributed with `.events` slot
crossings and the ring compare.  **Measured
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
Step 5 added `pcm_bass` (frames 36 / 44 / 64 of its 8 crossings, −61.4),
`pcm_psg` (20 / 25 of 13, −59.4) and `se_contest_place` (28 / 44 of 6,
−69.2) to the list, each with the ring byte-identical in all 177 frames;
step 6 `pcm_drum` (24 / 36 / 52 of 7, −50.6: a drum's samples jump, so one
DAC step costs more, as the cries' do in vanilla).  With the FIX drums
mixed the songs sit at their vanilla floor or past it (littleroot −54.4
vs vanilla −49.6, vs_wild −53.7 / −49.4, vs_trainer −53.1 / −52.7,
rg_vs_trainer −51.1 / −49.3), credits −47.7 / −51.3 and mt_chimney −44.0
/ −49.6 with a −28.9 worst window at 5 s: the floor at a different level
on a different set of crossings (48 in mt_chimney's 1200 frames, 30
frames over 200 / 32768), as this section predicted: mt_chimney re-probed
for 1200 frames has its ring byte-identical in all 1193 compared frames,
so the −28.9 window is the reference's crossings on a noise-heavy
passage, nothing of the port's.

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

**The rate variants** (step 11): `make RATE=<hz>` (CMake
`accuracy_rom_<hz>` / `accuracy_rom_hq_<hz>` into `build/accuracy/rom-<hz>`
/ `rom-hq-<hz>`, for every `<hz>` in the cache list `ACCURACY_PCM_RATES`,
default `21024`) builds either ROM at another sound-mode rate: the
`| SOUND_MODE_FREQ_13379` line of `m4aSoundInit`'s `m4aSoundMode` call
becomes `SOUND_MODE_FREQ_<hz>` (the wiki's "engine rate" line, `m4a.c`
line 79; SoundInit's own `SampleFreqSet(SOUND_MODE_FREQ_13379)` stays, as
in the game, so the boot's first timer pass is always the 224-sample
one).  The vanilla variant changes nothing else (`PCM_DMA_BUF_SIZE` stays
1584, `pcmDmaPeriod` = 1584 / frame = 4 at 21024 Hz).  The HQ variant
applies the wiki's two sizes: `hq_buffer_ptr[<frame>]` and
`PCM_DMA_BUF_SIZE` = 7 × frame (2464 at 21024 Hz) in generated copies of
`include/gba/m4a_internal.h` (first on `-iquote`) and
`constants/m4a_constants.inc` (the mixer's `.include` redirected to it in a
generated `m4a_1.s`); `SoundInfo` grows by 2 × 1760 bytes, so the IWRAM
map after `gSoundInfo` moves (`gSoundInfo` 0x030017F0, `gMPlayInfo_BGM`
0x03002F68 at 21024 Hz) and the probe takes the variant's own `nm`.  The
ROM symbols (`AgbMain`, `CgbSound` 0x080010AC, `MPlayMain`, `SoundMainRAM`)
are those of the 13379 Hz build of the same mixer, and the `.frames`
entries (MPlayMain, CgbSound) are identical frame for frame: the timing
model carries over; only the SoundMainRAM entry moves by 4 cycles (the
slot-offset multiply's operand), which no write depends on.  7884 Hz is
refused with `MIXER=hq`.

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
  `--hq-mixer` to `poryaaaa_render`, and writes `"mixer": "hq"` and
  `"engine_mixer"` into the report (`--engine-mixer vanilla` asks the HQ
  ROM against the vanilla engine: an informational number, never a
  baseline).  References are keyed by ROM hash, so the HQ references land
  in their own `refs/<hash-mgba>/` directory; the committed micro
  references have a sibling `refs/micro-hq/` with its own manifest
  (`--committed-refs` honours `--mixer`); the report is
  `build/accuracy/report-hq.json`.  The `synth_*` rows run only with
  `--mixer hq` (`HQ_ONLY_PREFIX`).
- **HQ reports are their own baseline family**: `baselines/<date>_hq-*.json`
  for song input and `<date>_hq-midi-path-*.json` for `--input midi`.
  `compare_reports.py` refuses to compare reports whose `"mixer"` or
  `"input"` fields differ.
- `claim_check.sh --mixer hq` (or `MIXER=hq`): probe logs under
  `build/probe-hq/`, the newest HQ song-input baseline (never a midi one),
  the HQ ROM.  The write-cycle diff uses the HQ probe logs.  An HQ change
  runs both gates: vanilla must show 0 moved writes and no row moved, HQ
  no regression.
- CMake: `accuracy_rom_hq`, `accuracy_song_tests_hq`, `accuracy_hq`
  targets; `accuracy` stays vanilla.  `poryaaaa_song_tests
  build/accuracy/rom-hq` passes (same songs, moved addresses — a free
  check of the relocation logic).
- The probe: `mgba_probe` takes the ROM and symbol file as arguments; the
  HQ run uses `build/probe-hq/syms.nm` from the HQ ELF plus the mixer's
  internal labels at the IWRAM copy (`C_*` / `F_*`: every register,
  `sp` at `SoundMainRAM_Buffer`, the decoded block buffer at
  `C_select_highspeed_codepath_vla_r3`, the ring slot written);
  `tools/probe_corpus.sh` runs the probed corpus on either ROM and
  `tools/probe_hq_compare.py` compares two corpora.  Engine side:
  `HQ_DEBUG=1` dumps the ring slot, the mix buffer and each pre-cached
  buffer per frame; `tools/hq_ring_compare.py` sets them against the
  probe's; `tools/frame_diff.py` finds the differing frames of two WAVs;
  `tools/hq_decode_check.py` decodes a DPCM block through the open-bus
  model by hand.  The `accuracy-harness` skill has the recipes.

### Metric and gate

Unchanged: −40 dB on `null` and `worst1s`, no gain correction, −inf for
bit-identical.  The result is the vanilla picture: −inf on single-voice
PCM micro-tests not at the floor, the PCM floor on songs and cries, the
PSG rows exactly as vanilla; 72/76 on song input, 50/54 on MIDI input
(section 7, steps 9 and 10); at 21024 Hz 73/76 (HQ) and 60/72 (vanilla),
every failure the floor at the 798-cycle timer's crossing rate (step 11).

---

## 5. The engine in HQ mode

- **One flag**, `engine->hqMixer` (bool, default false), set through
  `m4a_engine_set_hq_mixer(engine, bool)`, which on a change of mode
  resets the HQ-only state (mix buffers, noise-shape bytes, every
  channel's `sampleStor`: the ROM's BSS at install) and snaps
  `pcmFreqMode 2` (7884 Hz) to the default; re-applying the current mode
  is a no-op (step 12: the plugin re-applies its settings on every GUI
  change, and the seed / residuals / `CHN_SAMPLE_STOR` persist across
  frames in the ROM).  Set before the first process
  block it also selects the boot table (`sBootTableHq`, section 3; the
  boot replays once).  Off, the engine is byte-identical to the vanilla
  engine (the gate for every step: `claim_check` against the vanilla
  baseline, 0 moved writes, no row moved).
- **The ring size follows the configuration** (step 11):
  `engine->pcmDmaBufSize` is `PCM_DMA_BUF_SIZE` as the ROM would define
  it — the game's 1584 for vanilla at any rate and for HQ at 13379 Hz, 7 ×
  frame for HQ at another rate (the wiki's table, appendix C) — and both
  mixers' `BUF` (the B half's offset, the reverb taps) and the output
  stage's DMA reader (`m4a_gba_set_pcm_rate`) take it; the buffers are
  sized for the largest (`GBA_PCM_DMA_BUF_MAX`, 42048 Hz).  Whichever of
  `m4a_engine_set_hq_mixer` / `m4a_engine_set_pcm_freq_mode` comes last
  reprograms the ring (`pcm_reprogram`: m4aSoundMode's DMA off, ring
  clear, SampleFreqSet, DMA on, timer on).  **Before the boot replay has
  started, a rate change only sets the numbers**: m4aSoundInit's mode word
  is the rate, and the boot table turns the DMA and the timer on at the
  ROM's cycles.  (Enabling them at cycle 0, as the engine did before step
  11 for any non-default rate, ran the FIFO through the boot and left a
  different stale state at the final timer start: the note's ring slot
  fetched 10 timer ticks early in vanilla and 7 late in HQ at 21024 Hz —
  every PCM row at −20..−35 dB with a 0.5 / −0.3 ms lag.)  The boot
  replays SoundInit's first `SampleFreqSet` pass at the 224-sample period
  whatever the rate (`BOOT_TIMER_ON`), and the measured tables of the
  21024 Hz ROMs (`sBootTableVanilla21024` / `sBootTableHq21024`, appendix
  B) are selected for `pcmFreqMode 7`; every other non-default rate is
  unmeasured and replays the mixer's 13379 Hz table (section 8).
  `GBA_FIFO_DEBUG=1` prints every FIFO DMA refill (cycle, channel, ring
  offset, the four words) to set the cadence against a probe log's
  `040000A0` / `040000A4` writes.
- **The mixer**, `plugin/m4a_hq_mixer.{h,c}`: `m4a_hq_sound_main_ram(engine,
  frameOffset, pcmDmaCounter)` with the same signature and place as
  `m4a_sound_main_ram` (`sound_main` in `m4a_engine.c` dispatches on the
  flag).  Written like `m4a_mixer.c`: the register names kept, every label
  as a comment, the condition-code quirks as named booleans where a
  conditional instruction depends on an earlier instruction's flags.
  Header crediting ipatix and the source file (section 1).  Its parts:
  `m4a_hq_channel_init` (the SF_START work the vanilla start pass lacks:
  CMP from the sample's flag, the pointer conventions, the `SPL` type
  byte, `sampleStor`), `m4a_hq_channel_volumes` (`>> 13`), the dispatch
  of `C_mixing_setup` (volume-zero skip, FIX before synth, then the
  pre-cached test), `fast_loop_lt1` / `fast_loop_1to2` / `fast_loop_ge2`
  with the unbuffered loop, `fix_path`, `precache_path` (with
  `hq_decode_block` and the open-bus lanes), `synth_path`
  (`plugin/m4a_synth.h`), and `m4a_hq_downsample` (saturation, the 7-bit
  residuals, the reverb seed).
- **State** (`M4AEngine`): `hqBuffer[M4A_HQ_BUFFER_WORDS]` (`hq_buffer_ptr`:
  int16 left << 16 | int16 right per output sample, carrying the next
  frame's reverb seed between frames), `hqMutedBuffer` (the shadow pool's
  copy), `hqNoiseShapeLeft` / `hqNoiseShapeRight` (SoundInfo 0xE / 0xF),
  `hqStackSp` (`sp` at SoundMainRAM entry; default the test ROM's
  0x03007DD0, a 1220-byte budget; `--hq-stack-sp`),
  `hqUnportedChannelFrames` (a counter kept for the contract, always 0
  since step 9); per PCM channel `sampleStor` (`CHN_SAMPLE_STOR`).  The
  compressed pointer is a sample index from init, the representation the
  vanilla port already stores; `CHN_SPECIAL` is unused in HQ mode.
- **The envelope step** stays `m4a_pcm_channel_tick` (identical state
  machine); the HQ mixer computes its two volumes itself and stores them
  into `envelopeVolumeRight/Left`, so `--pcm-log`'s columns keep their
  meaning (the per-side volumes the mixer used); HQ mode adds `ss=`
  (`sampleStor`), `cnt=`, `fw=` per channel and `ns=` (the noise-shape
  bytes) per frame.
- **Synth voices**: the HQ mixer's synth loops are the measured truth
  (step 9: `synth_path`, −inf on the four rows); the oscillators live in
  `plugin/m4a_synth.h` and the vanilla-mode extension (`mix_synth` in
  `m4a_mixer.c`) runs the same ones under vanilla's accumulate (its LFO
  and saw arithmetic re-derived from the measured code, its own unit
  tests kept).
- **CPU cost** (cpu-meter step 3): the mixer charges `M4A_HQMIX_*` into
  `engine->mixCycles` at the ported labels and `frameCost` is published
  in HQ mode as in vanilla (`docs/ACCURACY.md`, "SoundMain cost model":
  the HQ table, the DMA3 / FIFO interleave, both HQ corpora at max |Δ| 0).
  Counting-only state: `hqFastCode` (`previous_fast_code`, reset with the
  mode) and the frame's DMA3 copies (`hqDma3Start` / `hqDma3Len`).
- **Shadow pool / polyphony debug**: the HQ pass mixes the shadow channels
  into `hqMutedBuffer` as the vanilla pass mixes into `mutedFrame`; the
  invert mode's downsampler output is the audible one.
- **Where the flag surfaces** (step 10): `poryaaaa_render --hq-mixer`
  (and `--hq-stack-sp`); the plugin's Options menu "HQ Mixer (ipatix)"
  with a tooltip, `poryaaaa.cfg` key `hq_mixer`, one CLAP state byte
  appended after the latch byte (absent loads as off), applied in
  `plugin_activate` / `state_load` / the GUI's `poll_changes` through
  `apply_mixer_settings` (the HQ flag first, then the mix rate, so the
  7884 Hz snap holds; `hq_snap_pcm_mode` snaps the plugin's own setting
  so what is saved is what plays).  The GUI's PCM Mix Rate combo omits
  7884 Hz while HQ is on.  A live toggle (after the boot) only switches
  the mixer, as the mix-rate change does; the boot table is the first
  process block's.
- **Interactions**: the opt-in extensions (portamento, PWM, base-key) are
  sequencer / CgbSound-side and orthogonal; XCMD 0x0D's start offset enters
  `count` as today and the HQ init consumes it (section 2); the cry player
  and SE players are untouched.
- **The MIDI path**: nothing changes — the mixer is after everything the
  MIDI path affects.  **Measured (step 10)**: `--input midi --mixer hq`
  (the runner's default form and `--midi-quantize`) reads 50/54, every row
  at its HQ song-input number, and all 54 renders are byte-identical to
  the `--song` renders (the vanilla picture: 43/44 there, the cries being
  the rows a MIDI run has no input for).

---

## 6. Coverage: mixer code paths and the rows that reach them

Each path and the rows that exercise it; a row's step ratio and path come
from `--pcm-log` (frequency × divFreq / 2^23), not from guessing.  The
rows in the right column were added for this effort (`gen_micro_midis.py`
+ `accuracy_micro.inc`; they play on the vanilla ROM too except the
`synth_*` rows); "none" marks a path without a dedicated row.

| path | vanilla-corpus rows | rows added for the HQ effort |
|---|---|---|
| downsampler, noise shaping, seed = 0 | every PCM row | — |
| interpolating fast path, `< 1.0` | `pcm` (key 55), `pcm_bass`, `pcm_adsr` | — |
| `[1.0, 2.0)` | `pcm_adsr` (1.0 exactly), `pcm_bass` (1.26), `pcm_hi` (keys 84 / 91: 1.0 / 1.5), `pcm_psg` | `pcm_ratio_1x` (keys 85 / 83 / 97: 1.059, 0.944, 2.119) — added in step 5 |
| `≥ 2.0` | `pcm_hi` (key 96: 2.0 exactly) | `pcm_ratio_1x` (key 97), `pcm_short_loop` (keys 100 / 108 / 112: 2.52, 4.0, 5.04) — step 5 |
| direct-from-ROM read (> 0x400 bytes or no stack) | none at 13379 Hz (448 B max at ratio 2) | `pcm_short_loop` key 112: the note's first frame (advance 1128 > 0x400, count 1874) — step 5; output-neutral by construction, the port does not model the copy |
| unbuffered loop: loop wrap inside a frame | `pcm` (looped flute), `pcm_chord` | `pcm_short_loop`: the flute's 562-sample loop against advances of 564 / 896 / 1128 (one, then two wraps a frame) — step 5 |
| unbuffered loop: sample end inside a frame | `pcm_adsr`, `pcm_drum`? | — |
| volume-zero freeze | none dedicated (songs reach it through VOL 0 and slow attacks; unit-tested) | none — `pcm_vol0` / `pcm_slow_attack` were planned and not needed for a −inf corpus |
| FIX path (block + remainder, alignment variants) | `pcm_drum` (voice 12, `no_resample`); every vanilla song through its drumset (`voice_keysplit_all voicegroup_*_drumset`: 26 `no_resample` voices in emerald_1 alone, the FRLG drumset's kick and snares in rg_vs_trainer); se_faint's rs_sfx_1 has none | `pcm_fix_loop` (voice 15, the flute looped at its own rate; the remainder's `loopStart` reset) — added in step 6 |
| reverb seed | `pcm_reverb`, mus_littleroot, mus_vs_wild, mus_credits | `pcm_reverb_max` (`-R127`: the `>> 2` scaling at 31) — added in step 7 |
| saturation | none deliberate (unit-tested) | none — `pcm_clip` was planned and not added |
| 16-bit half overflow / borrow | every row with negative samples | — |
| compressed forward | `pcm_cmp`, every forward cry | — (step 8: −inf) |
| compressed end frame (zero fill, immediate stop) | `pcm_cmp`, cries with the sample ending (pidgey) | — |
| compressed reverse + the negative block | reversed cries, `cry_pidgey_echo_start_chorus` (two channels ending in different frames) | — (step 8: −inf; the whole end frame on open bus) |
| uncompressed reverse | none in vanilla (reversed cries are DPCM) | `pcm_rev` (voice 16, `voice_directsound_reverse` on the pizzicato) — step 8, −inf with the loader's preceding bytes |
| stack budget exceeded | none | `pcm_cmp_hi` (the Bulbasaur cry at keys 96 / 72 / 94: the two high notes frozen silent on the HQ ROM) — step 8, −inf |
| synth pulse / saw / triangle, duty LFO | none | `synth_pulse` (voice 17: 50 %, no LFO), `synth_pulse_lfo` (18: base 0x20, step 0x10, mod 0x80, offset 0x40 — the fold reached every 16 frames), `synth_saw` (19), `synth_tri` (20) — `set_synth_*` descriptors in `accuracy_micro_synth.inc` (assembled into both ROMs, read by the loader as the overlay's `direct_sound_synth_data.inc`); **HQ-only rows**: the vanilla ROM plays a zero-size sample as a click, so `accuracy.py` skips `synth_*` unless `--mixer hq` — added in step 9, −inf |
| frame lengths ≠ 224 (21024 Hz: 0x160, ring 2464, `pcmDmaPeriod` 7; vanilla: ring 1584, period 4) | none at 13379 Hz | the whole corpus on the 21024 Hz builds of both ROMs (`accuracy.py --pcm-rate 21024 [--mixer hq]`) — step 11 |

A micro-test addition changes the ROM (both builds, since they share the
voicegroup): `--update-refs` for both micro sets is then legitimate (a ROM
change) and the canary check applies (`pcm`, `pcm_cmp`, `cry_bulbasaur`
byte-identical on both ROMs, `sq1` for the boot).

---

## 7. How it was done: the steps

The effort ran as twelve planned steps, one mixer path per step, each with
a row that reaches it and a predicted number written before the run.
Each step's plan paragraph is kept here above its "done" paragraph (the
measurements, the corrections to the reading, the baseline names); the
full numbers are in the commit messages.  The gate on every step from 3
on: the vanilla configuration unchanged (`claim_check.sh` against the
latest vanilla baseline: 0 moved writes, no row moved; unit / resampler / song
tests pass; poryaaaa and poryaaaa-standalone build), HQ numbers against
the latest `hq-*` baseline with no regression.

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
`poryaaaa_render --hq-mixer`; the plugin followed in step 10) selects
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
resampler 49112/0, songs 561/561.

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
resampler 49112/0, songs 561/561.

**Step 5 — the `[1.0, 2.0)` and `≥ 2.0` blocks, direct-from-ROM reads.**
`pcm_hi`, `pcm_chord`, the new `pcm_ratio_1x` / `pcm_short_loop` rows.

**Step 5 — done 2026-10-27.**  `fast_loop_1to2` and `fast_loop_ge2` in
`plugin/m4a_hq_mixer.c` (the second and third `high_speed_code_resource`
blocks in the `C_fast_mixing_loop` frame, each instruction in the
comment), selected as `C_select_highspeed_codepath` does (`r4 = step −
2.0` / `step − 1.0`, the halved volume doubled back); the direct-from-ROM
read is the same bytes and is not modelled (section 2).  Predicted before
the run: `pcm_bass`, `pcm_adsr`, `pcm_hi`, `pcm_psg` to −inf or the floor,
the songs and cries unchanged at 0 dB (FIX / CMP channels), vanilla
unchanged.  Measured, HQ (`claim_check --mixer hq` against the skeleton
baseline): 34/66, 0 regressions, 13 rows improved — `pcm_hi`, `pcm_adsr`,
`se_rain_stop` −inf; `pcm_bass` −61.4, `pcm_psg` −59.4, `se_contest_place`
−69.2 are the floor (section 3: rings identical in all 177 frames, the
error frames at the probe's timer-0 crossings; pcm_psg and
se_contest_place re-probed for the ring dump); the songs moved from 0 dB
to −5..−48 (their non-FIX / non-CMP channels now sound; `mus_credits`
−47.7) and `cry_over_bgm` to −6.6.  The same seven PSG logs show moved
writes (93–532 cycles, the frozen FIX / CMP channels), 0 moved on the
other 52.  Then the two rows: `gen_micro_midis.py` appends `pcm_ratio_1x`
(flute keys 85 / 83 / 97) and `pcm_short_loop` (keys 100 / 108 / 112) at
the end of `micro.cfg` (ids 31 / 32; every earlier id and MIDI
unchanged), both ROMs rebuilt (symbols unchanged; vanilla sha256
`88d5eca15f2d…`, HQ `8e02da3a345c…`), the canary renders pcm / pcm_cmp /
cry_bulbasaur byte-identical to the previous references on both ROMs,
`--update-refs` on both families (the 31 old FLACs unchanged, two added
each, the vanilla manifest's hash — stale since 2026-10-24 — now
current).  Both rows −inf on both ROMs: vanilla 59/68 (0 regressions, no
row moved; baseline `2026-10-27_step5-micro-rows`), HQ 36/68 (baseline
`2026-10-27_hq-fast-blocks`).  `--pcm-log` confirms the ratios (0.944 /
1.059 / 2.119; 2.52 / 4.0 / 5.04) and key 112's first frame on the fast
path (count 1874 → 746, advance 1128 > 0x400) before the loop wraps twice
a frame.  Unit tests: 8 new assertions in `test_hq_mixer` (the `[1, 2)`
ramp with the odd volume rounded up, the `≥ 2.0` ramp with the packed
borrow, the diff-0 skip in the `[1, 2)` block, the state after each);
unit 1955/1955, resampler 49112/0, songs 563/563 on both ROMs.  No source
correction was needed: the three blocks' flag analysis in section 2 held.

**Step 6 — the FIX path.**  `pcm_drum`, `pcm_fix_loop`; se_faint's snare and
rg_vs_trainer's drumset if they are FIX (check the voicegroups).

**Step 6 — done 2026-10-27.**  `fix_path` in `plugin/m4a_hq_mixer.c`
(`C_setup_fixed_freq_mixing` .. `C_mixing_end_store`, the length check,
the four-sample blocks, the remainder with its loop-or-end, every
instruction in the comment); the dispatch now tests FIX before the synth
flag as `C_mixing_setup` does.  Checked first: rg_vs_trainer's voicegroup
opens with `voice_keysplit_all voicegroup_frlg_drumset` (kick and snares
`no_resample`), and every vanilla drumset is FIX (26 voices in emerald_1),
while se_faint's `voicegroup_rs_sfx_1` has none — so the prediction was
`pcm_drum` to −inf or the floor, every song improved, se_faint unchanged,
the CMP rows and cries at 0 dB, vanilla untouched.  Measured, HQ against
`2026-10-27_hq-fast-blocks`: 0 regressions; `pcm_drum` −50.6 is the floor
(section 3), the six DirectSound songs from −5..−48 to −44..−54 (their
drums were the frozen channels), se_faint −49.2 as before, the cries 0
dB.  The write diff: the five songs' PSG logs are back to 0 moved (the
frozen-channel bookkeeping is gone), `cry_over_se` / `cry_over_bgm` still
move (the frozen CMP cry, step 8), and `pcm_psg` shows two writes 4
cycles early in frame 123 — exposed by its new 180-frame probe log (the
corpus had 70 frames), present on the vanilla ROM too (`track_diff`:
track 0, the three-note chord track, ends 4 cycles early before its
`ply_fine` on the tick where its gate ends and FINE follows), so a vanilla
sequencer-cost residual recorded in `ACCURACY.md` section 6, not this
step's.  The row: voice 15 `voice_directsound_no_resample` on the flute
in `accuracy_micro.inc`, `pcm_fix_loop` = the standard three-note pattern
(id 33), both ROMs rebuilt (symbols unchanged; vanilla sha256
`2d2db7fd59e3…`, HQ `6fc8845e6782…`; canaries byte-identical), refs
updated on both families.  `pcm_fix_loop` −inf on both ROMs.  Baselines
`2026-10-27_step6-fix-loop-row` (vanilla 60/69, 0 regressions, no row
moved) and `2026-10-27_hq-fix-path` (HQ 42/69).  Unit tests: 7 new
assertions (the FIX frame, the end without a loop, the loop reset; the
frame test's unported example is now CMP); unit 1962/1962.

**Step 7 — reverb.**  The seed arithmetic and the one-frame delay;
`pcm_reverb`, `pcm_reverb_max`, mus_littleroot, mus_vs_wild, mus_credits.
Prediction to write down first: the songs reach their vanilla floor
(−49..−55 dB); a level error shows as `gm` ≫ raw.

**Step 7 — done 2026-10-27.**  A verification step: the seed was ported
in step 4 and no engine change was needed.  The songs' prediction was
already met by step 6 (littleroot −54.4, vs_wild −53.7, credits −47.7,
`gm` equal to raw everywhere); the check that settles it is the ring:
littleroot, vs_wild and credits re-probed on the HQ ROM with the ring
dump are byte-identical to the engine's slots over the whole run (1193 /
1193 / 596 frames), so the taps, the `>> 2` factor, the borrow and the
one-frame delay hold at the games' reverb levels (section 2).  The
`pcm_reverb_max` row (`-R127`, the standard pattern; id 34) was predicted
−inf or the floor and reads −68.5 on HQ: the floor (ring identical in all
177 frames, the one error frame 64 = the probe's single crossing at cycle
17751296, pcm's), −inf on vanilla.  Both ROMs rebuilt (symbols unchanged;
vanilla sha256 `4501e5289d08…`, HQ `6cdb5ad99540…`; canaries
byte-identical), refs updated on both families (35 micro rows).
Baselines `2026-10-27_step7-reverb-max-row` (vanilla 61/70) and
`2026-10-27_hq-reverb-check` (HQ 43/70), 0 regressions on either.  Unit
tests: the seed at reverb 127 (factor 31, 310 and −31 with the borrow);
unit 1964/1964.

**Step 8 — compressed and reversed samples.**  Forward DPCM with the zero
fill and immediate stop (`pcm_cmp`, forward cries), then the reversed
paths with step 2(e)'s measurement in hand: decide whether the negative
block is reproduced (mgba's open-bus value is deterministic per ROM) or
recorded as a residual, as the vanilla reversed-cry end is.  The stack
budget skip (`pcm_cmp_hi`).  Rows: every cry and `pcm_rev`.

**Step 8 — done 2026-10-27.**  `precache_path` in `plugin/m4a_hq_mixer.c`
(`C_data_load_comp` forward and reverse, `C_data_load_uncomp_rev`, each
with its byte range, the stack check against `hqStackSp − 12`, the end
case's `F_clear_mem` and `streqb`, the fill, the fast loop from
`sp + (idx & mask)` and `C_end_mixing`), `hq_decode_block`
(`F_decode_compressed` with the 256-byte table view) and the open-bus
lanes; `C_mixing_setup_comp_rev`'s FIX-forced step; the engine's
`hqStackSp` and the renderer's `--hq-stack-sp`; `HQ_DEBUG=1` dumps each
pre-cache buffer (`HQSTACK`, comparable with the probe's `buf=`).
Predicted: every cry and `pcm_cmp` to −inf or the cry floor, the two
cry-over rows to the floor with their PSG logs back to 0 moved, the
songs unchanged.  Measured in three rounds, each attributed with the
ring compare, the register compare at `C_data_load_comp_*` and the
buffer compare: (1) the first port (negative blocks only from open bus)
gave `cry_bulbasaur_reverse` −inf but left 20 compressed rows at
0/−11..−57 — one cause, the truncated last block (section 2: the next
entry's header bytes and the shifted residual), fixed in the loader;
(2) `pcm_rev` −31.9: the bytes before the sample, kept by the loader
(`romBefore`); (3) `cry_pidgey_echo_start_chorus` −11.2: the whole end
frame on open bus (section 2), found by comparing the engine's `HQSTACK`
against the probe's `buf=` with every register equal.  Final: HQ 68/72
(from 43), 0 regressions; `pcm_cmp`, `pcm_rev`, `pcm_cmp_hi` and 20 of
the 22 cry rows −inf, `cry_wailord_doubles` −69.6 (the floor: ring
identical, 5 crossings), `cry_over_se` −66.7, `cry_over_bgm` −51.0
(the vanilla floor; their PSG logs 0 moved, so the HQ gate is now 62
logs diffed with only pcm_psg's known 4 cycles).  The four rows left are
the floor (`mt_chimney`, `credits`, `se_contest_place`) and `se_faint`
−49.2 (no FIX / CMP voice; open).  Rows: voice 16 (`voice_directsound_reverse`
on the pizzicato) and `pcm_rev`, `pcm_cmp_hi`; both ROMs rebuilt twice
(symbols unchanged; final vanilla sha256 `3faa13cab66a…`, HQ
`475cc577aa29…`; canaries byte-identical each time), refs updated (37
micro rows).  Vanilla 63/72, 0 regressions, no row moved (the loader's
new bytes are never output by the vanilla mixer; an earlier
`pcm_cmp_hi` with three high compressed notes read −35.4 on vanilla —
a vanilla high-ratio DPCM residual noted, not pursued).  Baselines
`2026-10-27_step8-cmp-rows` (vanilla) and `2026-10-27_hq-precached`
(HQ).  Unit tests: the forward decode, the end's zero fill and stop,
the reversed layout, the open-bus end (both forms), the stack skip, the
uncompressed reverse with the rebuilt header and with the loader's
bytes; unit 1988/1988.

**Step 9 — synths.**  Done 2026-10-27.  `plugin/m4a_synth.h` (the three
oscillators and the pulse's `C_setup_synth`, registers as names) and
`synth_path` in `plugin/m4a_hq_mixer.c` (the dispatch on the SPL type
byte after the step multiply and before the halving, the three loops, the
`C_end_mixing` state: `fw`, `sampleStor` = the threshold's low byte / the
last `phase << 1` / 0x80, `count`); `m4a_hq_channel_init` replaces the
type byte with `VOICE_TYPE_SPL` (new define) for a zero-size sample, as
`C_channel_init_synth` does — so the doc's "a FIX synth takes the FIX
path" was wrong, corrected in section 2.  Rows: `accuracy_micro_synth.inc`
(the `set_synth_pulse` / `_saw` / `_triangle` macros assembling the
loader's in-memory WaveData byte for byte, four descriptors; included by
`sound_data.s` before the voicegroup, linked by the overlay as
`sound/direct_sound_synth_data.inc`), voices 17–20, `synth_pulse`,
`synth_pulse_lfo`, `synth_saw`, `synth_tri` (ids 37–40), HQ-only
(`accuracy.py`'s `HQ_ONLY_PREFIX`); both ROMs rebuilt (symbols unchanged;
vanilla sha256 `6d53e9862beb…`, HQ `f47dea23ff9f…`; canaries pcm /
pcm_cmp / cry_bulbasaur byte-identical on both), `--update-refs` both
families (41 HQ micro rows, 37 vanilla; the old FLACs unchanged).
Measured: all four rows −inf on the HQ ROM at the first run (no
correction needed).  The vanilla-mode extension re-derived from
`m4a_synth.h` (the pulse LFO now advances in the mixer once per mixed
frame, no priming at note start — `m4a_pcm_synth_pulse_update` removed
from `m4a_channel.c`; the saw mixes the state at `floor(v / 2)`), −32..−34
dB against the HQ ROM (the 8-bit accumulate; informational).  Unit tests:
the pulse frame (±64 × v with the packed borrow, the state after), the LFO
over four mixed frames with the fold and a volume-zero frame that does not
advance it, the saw's eight states at the halved volume and the `mlane`
zero skip, the triangle's two slopes and the +128 peak, the frame test
(nothing is unported now); the Golden Sun tests re-derived (the threshold
appears at the first mixed frame); unit 2014/2014, resampler 49112/0, songs
571/571 on both ROMs.  Gate, vanilla: claim_check 59 diffed, 1 moved
(pcm_psg's known 4 cycles), 63/72, 0 regressions and no row moved vs
`2026-10-27_step8-cmp-rows`; baseline `2026-10-27_step9-synth-rows`.  HQ:
62 diffed, 1 moved (pcm_psg), 72/76 (the four synth rows −inf; left:
mt_chimney / credits / se_contest_place at the floor and se_faint −49.2,
open), 0 regressions vs `2026-10-27_hq-precached`; baseline
`2026-10-27_hq-synths`.

**Step 10 — the plugin, renderer, config, state, docs.**  Section 5's
surfaces; `--input midi --mixer hq` run and its baseline family; README and
`docs/ACCURACY.md` cross-references; the `accuracy-harness` and
`review-against-source` skills learn `--mixer hq`; this document rewritten
from plan to reference as the facts settle.

**Step 10 — done 2026-10-27.**  No engine change.  Plugin: `hqMixer` in
`M4APluginData` and `M4AGuiSettings`, config key `hq_mixer`, the CLAP
state byte after the latch byte (absent loads as off), the Options item
"HQ Mixer (ipatix)" with its tooltip, the combo without 7884 Hz while
on, `apply_mixer_settings` / `hq_snap_pcm_mode` (section 5).  Harness:
`claim_check.sh`'s default baseline skips midi-input files (a family of
its own).  Measured: vanilla `claim_check` 59 diffed, 1 moved (pcm_psg's
known 4 cycles), 63/72, 0 regressions and no row moved vs
2026-10-27_step9-synth-rows; HQ 62 diffed, 1 moved (the same), 72/76, 0
regressions vs 2026-10-27_hq-synths.  `--input midi --mixer hq`: 50/54 in
both forms (plain and `--midi-quantize`), every row at its HQ song number
and all 54 renders byte-identical to the `--song` ones; baselines
`2026-10-27_hq-midi-path.json` / `-latch.json`.  Unit 2014/2014, resampler
49112/0, songs 571/571 on both ROMs.  Docs: README (renderer flags, the
config key, the Options item, an "HQ mixer mode" section), `ACCURACY.md`
(the second configuration and the `--mixer` axis of the baseline family),
both skills, and this document rewritten from plan to reference.

**Step 11 — rates other than 13379 Hz** (optional).  An HQ ROM variant
per rate needs `hq_buffer_ptr` resized and, with reverb, `PCM_DMA_BUF_SIZE`
at 7 frames (appendix C) — the ring size then differs from the engine's
constant 1584, which becomes a per-mode value.  Since the vanilla effort
did not measure other rates either, this is a shared gap.

**Step 11 — done 2026-10-27**, at 21024 Hz (the wiki's own example rate:
"melodic instruments around 16000 Hz, drums at 22050"), on both ROMs.
ROMs: `make RATE=<hz>` (section 4; CMake `accuracy_rom_21024` /
`accuracy_rom_hq_21024`), the default builds byte-identical to before
(sha256 `6d53e9862beb` / `f47dea23ff9f`); the 21024 Hz pair `bbbc9775a172`
(vanilla) / `e05d95784fd3` (HQ), `poryaaaa_song_tests` 571/571 on both,
the `.frames` entries identical to the 13379 Hz builds' (only the
SoundMainRAM entry +4 cycles), the boot timelines in appendix B.  Harness:
`accuracy.py --pcm-rate <hz>` (the `rom[-hq]-<hz>` build,
`poryaaaa_render --pcm-mix-rate`, `report[-hq]-<hz>.json`, a `"pcm_rate"`
family `compare_reports.py` refuses to cross), `claim_check.sh --pcm-rate`,
probe logs under `build/probe[-hq]-<hz>/`; the probe's ring dump reads the
B half at `DMA2SAD − DMA1SAD` instead of a hard-coded 0x630 (with 0x630 it
read the wrong memory on the 2464 ring and reported every B slot
differing while A matched — a tooling artifact, found and fixed here).
Engine (section 5): `pcmDmaBufSize` per configuration, `BUF` and the DMA
reader on it, the buffers at `GBA_PCM_DMA_BUF_MAX`, `pcm_reprogram`
shared by the two setters, the measured boot tables for mode 7, the
first `SampleFreqSet` pass at 224 in the boot, `GBA_FIFO_DEBUG`.
Measured in two rounds.  (1) The first run read every PCM row at
−20..−35 dB with lag 0.5 ms (vanilla) / −0.3 ms (HQ) and the PSG rows
−inf: not the mixers (both wrong by different amounts) but the FIFO —
`GBA_FIFO_DEBUG` showed the engine's refills starting at cycle 798 while
the ROM's first fill is at 41049: a non-default rate ran `pcm_reprogram`
before the boot, which enabled the timer and the DMA from cycle 0, so the
stale FIFO state at the final timer start was not the ROM's (the note's
slot 704 fetched at 1153002 vs the ROM's 1160982 in vanilla, 10 ticks
early; 1164970 vs 1159384 in HQ, 7 ticks late).  With the pre-boot
reprogram only setting the numbers, the refill cycles match the probe's
exactly (41048, 321486, 331062, … slot 704 at 1160982 / 1159384) and
`pcm` reads −72.4 / −59.0.  (2) The full runs: vanilla 60/72, HQ 73/76
(baselines `2026-10-27_21024-step11-rate-variants` /
`_hq-21024-step11-rate-variants`), the PSG rows −inf as at 13379 Hz,
20 / 27 rows −inf against 29 / 42 at 13379 Hz, and every row that moved
off −inf at the floor: the probe's `.events` count 1.5–2.4× the timer-0
slot crossings (pcm 0 → 2 vanilla, 1 → 14 HQ; cry_jynx 19 → 31;
mt_chimney 89 → 179; rg_vs_trainer 93 → 212; pcm_short_loop 7 / 10,
pcm_fix_loop 12, pcm_cmp_hi 16, cry_bulbasaur_reverse 0 → 11), every
differing frame a single one-slot shift with the resampler's tail
(`frame_diff`: pcm_short_loop 7 crossings → 37 differing frames), and
the HQ rings byte-identical in all 177 frames on pcm_short_loop,
se_rain_stop and cry_pidgey_echo_start (the three HQ failures; the ring
compare of the 13379 Hz `pcm` still 0 differing with the fixed probe).
The failures are the floor on loud high-ratio rows (pcm_short_loop
−38.9 / −39.3, pcm_fix_loop −42.1 / −39.6, pcm_cmp_hi −34.8 vanilla),
the two noise-heavy songs on vanilla (mt_chimney −46.1 / −37.3,
rg_vs_trainer −43.5 / −36.8: 179 and 212 crossings) and the cries that
were at the floor already (cry_pidgey_echo_start −35.7 vanilla, −37.4
HQ; se_rain_stop −42.7 / −45.0 with a −37.8 / −39.9 worst second).
Nothing in the engine fixes a crossing (`ACCURACY.md`, section 6).
The write-cycle gate at 21024 Hz (`probe_corpus.sh` on both ROMs, then
`claim_check.sh --pcm-rate 21024 [--mixer hq]`): 62 / 60 logs diffed, 0
moved writes on either ROM — the PSG timing model carries over to the
rate unchanged — and 0 regressions vs the new baselines.
Gates at 13379 Hz: vanilla `claim_check` 59 diffed, 1 moved (pcm_psg's
4 cycles), 63/72, 0 regressions and no row moved vs
2026-10-27_step9-synth-rows; HQ 62 diffed, 1 moved (pcm_psg), 72/76,
0 regressions and no row moved vs 2026-10-27_hq-synths (baselines
`2026-10-27_step11-rate-variants` / `_hq-step11-rate-variants`).  Unit
2026/2026 (+12: the ring size in both setter orders, the DMA reader,
the largest ring).  The other table rates are built the same way but
unmeasured; their boot tables are the mixer's 13379 Hz ones (section 8).

**Step 12 — review** (`review-against-source`; done 2026-10-27): two
independent read-only reviews, one of `m4a_hq_mixer.c` / `m4a_synth.h`
against the vendored newmix assembly (lines 88-1210), one of the data and
plumbing around it (loader bytes against the HQ ROM, ring size, boot
tables, plugin state, CLI, tests).  The mixer review confirmed every path
a pokeemerald sample reaches instruction by instruction — the three fast
blocks' condition codes and whose flags they read, the unbuffered loop,
the FIX path, both pre-cached paths with the open-bus block addressing
and the delta table's whole-byte first index, the three synths, the
downsampler's saturation and the reverb seed, every store — with no
divergence on a stock path.  The plumbing review verified the loader's
`romBefore` layout and the next-entry fill against the ROM bytes of
Cry_Bulbasaur / Cry_Ivysaur / Cry_Venusaur, the ring sizes, the boot
order (SoundInit's own 13379 Hz pass, then m4aSoundMode's) and the state
bytes.  Six findings were fixed, each gated by a unit test (none reaches
the harness's stock path, so all were predicted harness-neutral and
measured so): (1) `m4a_engine_set_hq_mixer` cleared `hq_buffer` (the
next frame's reverb seed), the noise-shape residuals and
`CHN_SAMPLE_STOR` on every call, and the plugin re-applies its settings
on every GUI change — now a no-op when the mode is unchanged; (2)
`poryaaaa_render --hq-stack-sp` was ignored on the `--midi` path; (3)
the host's 0x600 cap on the pre-cached stack image was below the budget
a `--hq-stack-sp` near the top of IWRAM allows (the ROM's check would
mix a 1536..1780-byte frame the port skipped) — the cap is 0x700 =
0x03008000 − 0x03007900, beyond any IWRAM sp; (4) a FIX channel entering
with count 0 (an XCMD start offset clamped to the sample's size) turned
the length check's −1 >> 2 into a 2^31-iteration write past `hq_buffer`
(the ROM reads 0x3FFFFFFF blocks through memory) — a host guard stops
the channel; (5) the portamento legato inherit (opt-in, not in the
source) copied the position but not the type bits the channel init added
(CMP from the WaveData: the inherited channel took the uncompressed path
with an index-form pointer) nor `CHN_SAMPLE_STOR` — both copied now; (6)
unit coverage for the re-apply rule and the 7884 Hz snap in both setter
orders.  Not acted on, recorded in `ACCURACY.md` section 6: channels
above a lowered `maxPcmChannels` (SoundMainRAM freezes exactly
`maxChans` channels, both ports process every ON channel) and a loop
with `loopStart > size` (negative loop length).  Cosmetic, left as is:
the decay→IEC transition keeps `ENV_DECAY` in the status byte in the
source and clears it in the port (the mixer tests IEC first, no output
change); the `ID_NUMBER` store and `previous_fast_code` are not
modelled; the loader's prev/next walk stops at the synth descriptor
file (only the last `direct_sound_data.inc` entry, if compressed and
truncated, would lack its fill); the direct-from-ROM read of a > 0x400
byte uncompressed frame reads the same bytes; `hq_report`'s
unported-frame count is always 0 now; right shifts of negative values
are arithmetic on every supported compiler.  Gate, vanilla:
`claim_check` 59 diffed, 1 moved (pcm_psg's known 4 cycles), 63/72, 0
regressions and no row moved vs 2026-10-27_step11-rate-variants.  HQ:
62 diffed, 1 moved (pcm_psg), 72/76, 0 regressions and no row moved vs
2026-10-27_hq-step11-rate-variants.  Unit 2043/2043 (+17), resampler 49112/0,
songs 571/571 on both ROMs; the 21024 Hz runs unchanged (60/72 and
73/76, 0 regressions, no row moved vs their step 11 baselines).
Baselines `2026-10-27_step12-review` / `_hq-step12-review`: **the HQ
baseline is final.**

---

## 8. Known residuals and limitations

Residuals (rows under −40 dB that are not −inf, and the four failing rows)
as of the 2026-10-27_hq-step12-review baseline (72/76):

- **The PCM floor** (mgba scheduler lateness, section 3): rows whose ring
  slots are byte-identical to the ROM's and whose residual is one DAC step
  in the frame of a timer-0 slot crossing — pcm, pcm_reverb,
  pcm_reverb_max, pan, bend, lfo_delay, pcm_chord, pcm_bass, pcm_psg,
  pcm_drum, se_contest_place, cry_wailord_doubles, cry_over_se,
  cry_over_bgm, and the six songs (−44 … −54).  Four rows fail the gate on
  their worst second at the floor: mus_mt_chimney (−44.0 / −28.9, 48
  crossings on a noise-heavy passage, ring identical over 1193 frames),
  mus_credits (−47.7 / −39.1), se_contest_place (−69.2 / −39.6) and
  se_faint (−49.2 / −38.5).
- **se_faint −49.2 is unattributed**: its voicegroup has no FIX, CMP, REV
  or synth voice, and the row did not move through steps 6–9; not yet
  probed with the ring compare.  Open.
- **pcm_psg's CgbSound entry 4 cycles early in frame 123** on both ROMs: a
  vanilla sequencer-cost residual (`ACCURACY.md` section 6), shown by both
  gates as the one moved write.
- **The open-bus end frame** of a reversed DPCM sample is reproduced as
  mgba reads it (`GBALoadBad`: the prefetch word two ahead, byte-selected
  by the address; section 2), an emulator model recorded in `ACCURACY.md`;
  hardware reads something else there.

Limitations of the mode (stock HQ behaviour, reproduced on purpose):

- **The stack budget** is ROM-dependent (the game's `sp` at the interrupt
  varies with the main loop's call depth); the engine's default is the
  test ROM's 0x03007DD0 (1220 bytes: 19 blocks of a compressed frame
  pass, 20 skip).  `--hq-stack-sp` sets another; the plugin has no
  surface for it.  No vanilla corpus row reaches the budget.
- **Compressed loops are not looped** in HQ (the source's TODO): a looping
  compressed sample ends.
- **7884 Hz** is not an HQ mix rate (its frame is not a multiple of 8);
  the engine and the plugin snap it to 13379 Hz.
- **Rates other than 13379 and 21024 Hz are unmeasured** (step 11): the
  engine mixes any table rate but 7884 through the same code with the
  wiki's 7-frame ring, but only the 21024 Hz builds of the two ROMs have
  been probed and nulled (vanilla 60/72, HQ 73/76, every miss the floor:
  the 798-cycle timer crosses a DAC slot boundary 1.5–2.4× as often as the
  1254-cycle one, so rows that were −inf sit at −45..−80 and the loud
  high-ratio rows, the noise songs and the near-floor cries miss the gate
  — `2026-10-27_21024-step11-rate-variants` / `_hq-21024-…`).  Another
  rate replays the mixer's 13379 Hz boot table (its SampleFreqSet pass
  differs by a few cycles: the divisions' operands and the VCOUNT poll's
  exit phase, 2 / 6 / 6 cycles at 21024 Hz in vanilla), so its first
  frames may carry a one-slot offset until measured.  Vanilla has the
  same gap.
- **Other HQ variants in the wild**: ipatix's master branch (`ENABLE_REVERB
  0` default, `POKE_CHN_INIT`), older revisions, pokeemerald-expansion's
  copy, eventide's.  The mode reproduces the wiki's file; differences in
  other copies are out of scope until measured.
- **Paths without a dedicated row**: the volume-zero freeze and the
  saturation are unit-tested and reached by songs, not by a micro row
  (section 6).
- **Reviewed** (step 12, 2026-10-27): both source-comparison reviews have
  run; the HQ baseline `2026-10-27_hq-step12-review` is final.  Two
  limitations they recorded are in `ACCURACY.md` section 6 (channels
  above a lowered `maxPcmChannels`, `loopStart > size`).

---

## 9. Working rules for HQ mode

All of `ACCURACY.md` section 7, plus:

- **The vanilla configuration is frozen.**  No HQ change may move a vanilla
  write or row; HQ code lives behind the flag and in its own file, and
  every HQ change runs both gates.
- **One mixer path per change**, each with a row that reaches it and a
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

## Appendix B: arithmetic reference (measured)

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
- Boot timelines of the 21024 Hz builds (step 11, `mgba_probe` on the
  `pcm` row, 2026-10-27).  Vanilla 21024 (vs vanilla): SoundInit's block
  and its SampleFreqSet pass identical (5720 / 5737 / 5760, DMA on 15873,
  timer on 39794, MPlayExtender 39875–39935, DMA off 41238); m4aSoundMode's
  pass: timer off 47452 → 47450, DMA on 47893 → 47887 (the divisions'
  operands), timer on 320682 → **320688** (the VCOUNT 159 poll's exit
  phase), DISPSTAT / IE / IME 323284 / 323299 / 323305; the timer reload
  is −798 (0xFCE2).  HQ 21024 (vs HQ): SoundInit's block identical (7800 /
  7817 / 7840), DMA on 17953 → **20589** (+2636: CpuFill32 of the
  1760-byte-larger SoundInfo), the pass still ending on frame 0's line 159
  at vanilla's 39794 (so MPlayExtender 39875–39935 and DMA off 41238 are
  vanilla's, not the HQ ROM's 39867 / 41230), timer off 47444 → **50088**
  and DMA on 47885 → **50525** (m4aSoundVSyncOff's CpuFill32 of the
  larger ring), timer on 320686 (the HQ ROM's), DISPSTAT / IE / IME
  323282 / 323297 / 323303.  The FIFO refill cadence after the final
  start is the model's on both: the first fill one tick (798 cycles)
  later, the second 12 ticks after it, then every 16 ticks.  ROM sha256
  prefixes: vanilla 21024 `bbbc9775a172`, HQ 21024 `e05d95784fd3`.

## Appendix C: the wiki's tables

Frame length (samples per VBlank = `hq_buffer_ptr` words) by engine rate:
5734 Hz 0x60, 7884 Hz 0x84 (unsupported), 10512 Hz 0xB0, 13379 Hz 0xE0
(default), 15768 Hz 0x108, 18157 Hz 0x130, 21024 Hz 0x160, 26758 Hz 0x1C0,
31536 Hz 0x210, 36314 Hz 0x260, 40137 Hz 0x2A0, 42048 Hz 0x2C0.  These
equal `gPcmSamplesPerVBlankTable`.

`PCM_DMA_BUF_SIZE` for 7 frames at a raised rate with reverb: 5734 Hz 672,
7884 Hz 924 (unsupported), 10512 Hz 1232, 13379 Hz 1568 (the vanilla define
is 1584 and stays), 15768 Hz 1848, 18157 Hz 2128, 21024 Hz 2464, 26758 Hz
3136, 31536 Hz 3696, 36314 Hz 4256, 40137 Hz 4704, 42048 Hz 4928.  The
wiki applies this only with the rate raised and reverb kept ("the reverb
sounds shorter" without it, the game does not crash); the test ROM's
`RATE=` build and the engine's `pcmDmaBufSize` follow the table at every
rate but 13379 (section 4, section 5).

The wiki's `DmaSetUnchecked`: `register` r0/r1/r2 bindings and one
`asm volatile("stmia %0!, {%1, %2, %3}")` with a `"memory"` clobber in place
of the three stores.
