---
name: review-against-source
description: Run the two independent source-comparison code reviews this project requires after an engine port or milestone (mixer, sequencer, PSG, loader, plugin state), brief the reviewers without conclusions, then test every "the source does X" claim one at a time against the accuracy harness before adopting it. Use when a milestone's code is done and before its baseline is final, or when asked for a code review of engine/accuracy work.
---

# Review against the source

Every engine milestone ends with two independent reviews of the diff
against the original sources, and every claim they make is tested one at a
time with the accuracy harness (`accuracy-harness` skill, "Testing a review
claim or a constant change").  Reviews have been wrong in every milestone
so far (milestone 2, the timing model, the mixer), and they have also found
real regressions the unit tests passed (the mixer's synth pitch step).
Neither outcome is visible until the claim is measured.

## Set up

```sh
git diff <base-commit> -- plugin cmd test/test_engine.c CMakeLists.txt > $CLAUDE_JOB_DIR/tmp/<name>.diff
```

Spawn two `general-purpose` agents in one message so they run in parallel.
They are read-only: tell them not to modify files, not to build, not to
run the harness or the unit tests.  Give them the diff path, the source
files with line ranges, and the questions.  Give them **no conclusions**:
not the harness numbers, not what you believe matches, not the memory
notes.  Ask for the report as a numbered list where each item names the
source line(s) and what they do, the port line(s) and what they do, and
whether it is a divergence (changes output), a cosmetic difference, or a
confirmation, phrased as a testable claim ("the source does X at line N;
the port does Y").  Ask them to say so when something matches.

## The two briefs

**Reviewer 1: the ported routine against its source.**  For the mixer:
`~/pokeemerald/src/m4a_1.s` SoundMainRAM (lines 88-470), Unk1/Unk2
(470-710), ply_note's channel setup (1740-1800); `~/pokeemerald/src/m4a.c`
SoundInit / SampleFreqSet / m4aSoundMode / MidiKeyToFreq; the struct and
flag definitions in `include/gba/m4a_internal.h` and
`constants/m4a_constants.inc`.  Enumerate the checks: every loop, every
exit path and what it stores, the arithmetic quirks (`bic 0x3F800000`,
`tst 0x80; addne`, the word-sized xpi store), the first-call setups, the
shared buffers, and host-pointer or UB hazards.  For the sequencer the
equivalent is MPlayMain and the ply_* handlers; for the PSG, mgba's
`src/gb/audio.c`; for CgbSound, m4a.c's CgbSound.

**Reviewer 2: the data and plumbing around it.**  Loader struct layouts
and field values against `m4a_internal.h`, the sample converter
(`tools/wav2agb/converter.cpp`), the voice macros (`asm/macros/music_voice.inc`)
and the ROM's placement (`sound/direct_sound_data.inc`), with the test ROM
and map available for byte checks (`build/accuracy/rom/accuracy_test.gba`,
`build/accuracy/rom/accuracy_test.map`); any option or enum against
its m4a.c origin; plugin state save/load byte order and old-save
compatibility (`plugin/m4a_plugin.c` state_save / state_load), config keys,
GUI bounds, CLI messages; and a grep for stale uses of removed fields
across plugin/, cmd/, test/.

Both reports are model output: every "the source does X" is a claim.

## Process the claims

For each claim, in order of expected impact:

1. Apply it alone.  Rebuild `poryaaaa_render` and `poryaaaa_unit_tests`.
2. `test/accuracy/tools/accuracy.py`, then `compare_reports.py <current
   baseline> build/accuracy/report.json`.  Every `-inf` must stay `-inf`.
3. Regression: revert; record it in the plan as rejected by measurement
   with the numbers.  Neutral and source-verified: adopt, say
   harness-neutral in the plan.  Improvement: adopt and record the numbers.
4. A claim about code outside the stock paths (an extension such as the
   Golden Sun synths, a fallback, a GUI path) is harness-neutral by
   construction; its gate is a unit test that would have caught it.  Write
   that test with the fix.
5. Claims you decide not to act on (cosmetic, malformed-data edge cases,
   unmeasurable memory reads) go in the plan too, in one paragraph, so the
   next review does not re-raise them.

Then refresh the baseline from a full default harness run, re-run
`poryaaaa_unit_tests`, `poryaaaa_blip_tests` and `poryaaaa_song_tests
build/accuracy/rom`, and commit the fixes, the plan paragraph and the
baseline together.

## What past reviews got wrong, so you check it

- Timing model: "the tick's per-track cost is 34 + 26 per live channel"
  broke four bit-exact start frames (charged per chained channel; the
  measured cost applies with an empty chain).
- Mixer: "wav2agb pads with 0 after the data" was true of one code path in
  the converter's source but not of the .bin files the ROM is built from;
  two -inf tests caught it.  Read the ROM bytes, not the tool.
- Mixer: "the synth triangle starts at the negative peak now" was raised
  as a regression of the envelope-step move; the real cause was the phase
  step losing its divFreq factor in the same port.  Debug the claim's
  symptom, do not just apply its suggested fix.
