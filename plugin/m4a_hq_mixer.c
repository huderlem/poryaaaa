#include "m4a_hq_mixer.h"
#include "m4a_channel.h"
#include "m4a_synth.h"
#include <string.h>
#include <stdio.h>
#include <stdlib.h>

/*
 * SoundMainRAM of the HQ-Mixer rev 4.0 (ipatix, 2021), translated from
 * test/accuracy/rom/hq/m4a_1.s (newmix's src/m4a_1.s, lines 88-1210), with
 * ENABLE_REVERB 1 and ENABLE_DMA 1 as the pret wiki installs it.  Credit:
 * ipatix (https://github.com/ipatix/gba-hq-mixer).
 *
 * The register names of the assembly are kept as variable names so the two
 * can be read side by side (the ARM mixing code's conventions):
 *
 *   r2   SoundChannel.count, r3 the sample pointer (the *next* sample:
 *        the base is r6), r4 the step (frequency * divFreq, 23-bit
 *        fraction), r5 the mix buffer position, r6 the base sample
 *        (CHN_SAMPLE_STOR across frames), r7 fw, r8 the samples left in
 *        the frame, r11 the packed volume (L << 16 | R), r12 the diff to
 *        the next sample, r9 scratch (the interpolation product / output)
 *
 * Every mixed sample is `mla word, r11, r9, word`: the 32-bit word holds
 * int16 left in the high half and int16 right in the low half, and a
 * negative product borrows from the high half (the packed-word quirk);
 * the halves wrap at 16 bits.  Flags that a later conditional instruction
 * depends on are carried as booleans named after the condition.
 *
 * The self-modifying parts (the fast loop's instruction block pasted by
 * step, the DPCM store direction, the FIX alignment code) only select
 * which block runs; the DMA3 / CPU copy of the frame's source bytes onto
 * the stack and the stack-budget test against 0x03007900 are output-neutral
 * for the paths ported here (byte content is identical either way) and are
 * not modeled -- except by the cost model, which counts what they cost
 * (the paste, the copy, where the samples are loaded from).
 */

#define BUF (engine->pcmDmaBufSize)   /* PCM_DMA_BUF_SIZE: the B half's offset */

/*
 * The cost model (docs/ACCURACY.md, "SoundMain cost model"): every block
 * below charges the cycles the HQ mixer spends on it, M4A_HQMIX_* in
 * m4a_engine.h, into engine->mixCycles.  The loops keep their count in a
 * local `cy` (the fast loops return it in FastState) and add it on the way
 * out; the multiplies whose operand changes per sample (the mixing loops'
 * mul / mla, the downsampler's reverb products) add their m where the
 * operand is known.  Counting only: no charge changes what is mixed.
 */

/* mul / mla's extra cycles, mgba's ARM_WAIT_SMUL: 1..4 by the significant
 * bytes of the Rs operand. */
static inline int mul_m(uint32_t rs)
{
    uint32_t h = rs & 0xFFFFFF00u;
    if (h == 0 || h == 0xFFFFFF00u)
        return 1;
    h = rs & 0xFFFF0000u;
    if (h == 0 || h == 0xFFFF0000u)
        return 2;
    h = rs & 0xFF000000u;
    if (h == 0 || h == 0xFF000000u)
        return 3;
    return 4;
}

/* umlal's, ARM_WAIT_UMUL: the unsigned operand's significant bytes. */
static inline int umul_m(uint32_t rs)
{
    if (!(rs & 0xFFFFFF00u))
        return 1;
    if (!(rs & 0xFFFF0000u))
        return 2;
    if (!(rs & 0xFF000000u))
        return 3;
    return 4;
}

/* F_clear_mem over r1 bytes (from the bl to its return): the computed
 * jump into C_clear_loop runs (r1 >> 4) & 3 stores, then whole passes of
 * four while r1 - 64k stays >= 0, then the word remainder. */
static int64_t clear_cycles(int32_t r1)
{
    int64_t c = M4A_HQMIX_CLEAR + M4A_HQMIX_CLEAR_STORE * ((r1 >> 4) & 3);
    for (;;) {
        r1 -= 0x40;                                      /* subs r1, r1, #0x40 */
        if (r1 < 0)
            break;
        c += 1 + 3 + 4 * M4A_HQMIX_CLEAR_STORE;          /* bpl taken, the four stores */
    }
    c += 1 + 1;                                          /* subs, bpl not taken */
    r1 &= 0xC;                                           /* ands r1, r1, #0xC */
    if (r1 == 0)
        return c + M4A_HQMIX_CLEAR_RET;
    /* ldmeqfd not taken, C_clear_loop_rest, ldmfd with pc */
    c += 1 + 1;
    for (; r1 > 0; r1 -= 4)
        c += M4A_HQMIX_CLEAR_WORD;
    return c + M4A_HQMIX_LOOP_LAST + 10;
}

/* C_channel_state_loop, the SF_START branch (section 2, "Channel init").
 * m4a_pcm_channel_start_at has already turned the XCMD 0x0D start offset
 * into currentPointer = data + offset and count = size - offset, which is
 * C_channel_init_noncomp_forward and C_channel_init_comp_forward (the
 * compressed pointer is a sample index, kept as data + index); this does
 * the rest. */
void m4a_hq_channel_init(M4APCMChannel *r4)
{
    WaveData *r3 = r4->wav;
    if (r3->size != 0) {
        /* enabled compression if sample flag is set: WaveData.type bit 0
         * (lsl #31, bmi) -> TONEDATA_TYPE_CMP OR-ed into the channel type. */
        if (r3->type & 1)
            r4->type |= VOICE_TYPE_CMP;
        /* C_channel_init_noncomp_reverse: ptr = data + size - count;
         * C_channel_init_comp_reverse: ptr = count = size - count.  Both
         * are data + the new count in the host representation. */
        if (r4->type & VOICE_TYPE_REV)
            r4->currentPointer = r3->data + r4->count;
    }
    /* C_channel_init_synth: the channel type byte becomes TONEDATA_TYPE_SPL
     * (strb r5: the voice's FIX / REV / CMP bits are gone, so a synth never
     * takes those paths) and the triangle starts at 0x40000000 --
     * m4a_pcm_channel_start_at primes synthType / fw the same way. */
    if (r3->size == 0)
        r4->type = VOICE_TYPE_SPL;
    r4->sampleStor = 0;                                  /* strb r5, [r4, CHN_SAMPLE_STOR] */
}

/* C_channel_vol_calc: v = env * (masterVolume + 1); L = (leftVolume * v)
 * >> 13, R = (rightVolume * v) >> 13 (0..127 each). */
void m4a_hq_channel_volumes(M4APCMChannel *r4, uint8_t masterVolume)
{
    uint32_t r5 = (uint32_t)r4->envelopeVolume * ((uint32_t)masterVolume + 1);
    r4->envelopeVolumeLeft = (uint8_t)(((uint32_t)r4->leftVolume * r5) >> 13);
    r4->envelopeVolumeRight = (uint8_t)(((uint32_t)r4->rightVolume * r5) >> 13);
}

/* mla \reg, r11, r9, \reg: the packed-word accumulate. */
static inline uint32_t mla(uint32_t word, uint32_t r11, int32_t r9)
{
    return r11 * (uint32_t)r9 + word;
}

/* The registers the fast loop carries out: the base sample and position;
 * and the loop's cycles (from C_fast_mixing_loop to its last bgt). */
typedef struct { int32_t r6; uint32_t r7; int64_t cy; } FastState;

/* C_fast_mixing_loop with the "Mix Freq < 1.0 * Output Frequency" block
 * (high_speed_code_resource, the first six instructions).  r8 samples
 * (a multiple of 8), the position r7 kept in [0, 2^23) by the `bic r7,
 * r7, r2, asr#1` (r2 = 0xFF000000) after each sample, r4 = step - 2^23.
 *
 *   mulne r9, r7, r12          @ NE: the flags of the previous sample's
 *                              @ subpls (diff != 0) or adds r7 (no advance)
 *   mov r9, r9, asr#22         @ Block #1
 *   adds r9, r9, r6, lsl#1     @ the output sample at double scale, flags
 *   mlane \reg, r11, r9, \reg  @ NE: the output is not 0
 *   adds r7, r7, r4
 *   addpl r6, r12, r6          @ PL: the position carried into a new sample
 *   ldrplsb r12, [r3, #1]!
 *   subpls r12, r12, r6
 *   bic r7, r7, r2, asr#1
 *
 * (quirk) when the previous sample advanced onto a diff of 0, mulne is
 * skipped and r9 keeps the previous output, whose asr #22 is 0 or -1: a
 * flat negative run comes out one unit low at double scale -- except for
 * the first sample of each 8-sample iteration, whose flags come from the
 * loop's `subs r8, r8, #8` (never zero when the loop continues): there
 * the multiply always runs.  Measured on pcm frame 64 (step 4). */
static FastState fast_loop_lt1(uint32_t *r5, int32_t r8, const int8_t *r3, int32_t r6,
                               uint32_t r7, uint32_t r4, uint32_t r11, int ld)
{
    bool ne = true;                                      /* movs r2, #0xFF000000 */
    int32_t r12 = (int32_t)r3[0] - r6;                   /* ldrsb r12, [r3]; sub r12, r12, r6 */
    uint32_t r9 = 0;                                     /* holds r3 + advance; never read before mulne */
    int64_t cy = 0;
    do {
        ne = true;                                       /* subs r8, r8, #8 (or movs r2) */
        cy += M4A_HQMIX_FAST_ITER + 8 * M4A_HQMIX_FAST_SAMPLE;
        for (int k = 0; k < 8; k++) {
            if (ne) {
                r9 = r7 * (uint32_t)r12;
                cy += mul_m((uint32_t)r12);
            }
            r9 = (uint32_t)((int32_t)r9 >> 22);
            r9 = r9 + ((uint32_t)r6 << 1);
            if (r9 != 0) {
                r5[k] = mla(r5[k], r11, (int32_t)r9);
                cy += M4A_HQMIX_MLA + mul_m(r9);
            }
            r7 = r7 + r4;
            if ((int32_t)r7 >= 0) {                      /* PL */
                r6 = r12 + r6;
                r3 += 1;
                r12 = (int32_t)r3[0] - r6;
                ne = (r12 != 0);
                cy += ld;                                /* ldrplsb */
            } else {
                ne = true;                               /* a negative sum is never 0 */
            }
            r7 &= 0x007FFFFFu;
        }
        r5 += 8;
        r8 -= 8;
    } while (r8 > 0);
    FastState s = { r6, r7, cy + M4A_HQMIX_LOOP_LAST };
    return s;
}

/* C_fast_mixing_loop with the "Mix Freq > 1.0 and < 2.0" block (the second
 * six instructions).  r4 = step - 2^23 - 2^23 (the selection's second
 * subpls, in [-2^23, 0)); r11 doubled back (movpl r11, r11, lsl#1); every
 * output sample advances one source sample, two when the fraction wraps.
 *
 *   mulne r9, r7, r12          @ NE: the previous sample's subs r12 (diff != 0)
 *   adds r9, r6, r9, asr#23    @ Block #2: the output at single scale, flags
 *   add r6, r12, r6            @ the next base (one sample on)
 *   mlane \reg, r11, r9, \reg  @ NE: the output is not 0
 *   adds r7, r7, r4            @ PL: fraction + step >= 2.0
 *   ldrplsb r6, [r3, #1]!      @ a second sample on
 *   ldrsb r12, [r3, #1]!
 *   subs r12, r12, r6
 *   bic r7, r7, r2, asr#1
 *
 * (quirk) as the < 1.0 block: a diff of 0 skips the next multiply and r9
 * keeps the previous output, whose asr #23 is 0 or -1 (a negative output
 * leaves the next sample one unit low), except at an iteration's first
 * sample (subs r8, r8, #8). */
static FastState fast_loop_1to2(uint32_t *r5, int32_t r8, const int8_t *r3, int32_t r6,
                                uint32_t r7, uint32_t r4, uint32_t r11, int ld)
{
    bool ne = true;                                      /* movs r2, #0xFF000000 */
    int32_t r12 = (int32_t)r3[0] - r6;                   /* ldrsb r12, [r3]; sub r12, r12, r6 */
    uint32_t r9 = 0;
    int64_t cy = 0;
    do {
        ne = true;                                       /* subs r8, r8, #8 (or movs r2) */
        cy += M4A_HQMIX_FAST_ITER + 8 * (M4A_HQMIX_FAST_SAMPLE + ld);   /* + the ldrsb r12 */
        for (int k = 0; k < 8; k++) {
            if (ne) {
                r9 = r7 * (uint32_t)r12;
                cy += mul_m((uint32_t)r12);
            }
            r9 = (uint32_t)r6 + (uint32_t)((int32_t)r9 >> 23);
            r6 = r12 + r6;
            if (r9 != 0) {
                r5[k] = mla(r5[k], r11, (int32_t)r9);
                cy += M4A_HQMIX_MLA + mul_m(r9);
            }
            r7 = r7 + r4;
            if ((int32_t)r7 >= 0) {                      /* PL */
                r3 += 1;
                r6 = r3[0];
                cy += ld;                                /* ldrplsb */
            }
            r3 += 1;
            r12 = (int32_t)r3[0] - r6;
            ne = (r12 != 0);
            r7 &= 0x007FFFFFu;
        }
        r5 += 8;
        r8 -= 8;
    } while (r8 > 0);
    FastState s = { r6, r7, cy + M4A_HQMIX_LOOP_LAST };
    return s;
}

/* C_fast_mixing_loop with the "Mix Freq > 2.0" block (the third six
 * instructions).  r4 = step - 2^23 (the selection's addpl puts the second
 * 2^23 back); r11 doubled back; the pointer moves by the whole part of
 * fraction + step - 1 each sample (r3 is one past the base).
 *
 *   mulne r9, r7, r12          @ NE: the previous sample's subs r12 (diff != 0)
 *   adds r9, r6, r9, asr#23    @ Block #3: the output at single scale, flags
 *   add r7, r7, r4             @ no flags
 *   mlane \reg, r11, r9, \reg  @ NE: the output is not 0
 *   add r3, r3, r7, lsr#23
 *   ldrsb r6, [r3]
 *   ldrsb r12, [r3, #1]!
 *   subs r12, r12, r6
 *   bic r7, r7, r2, asr#1
 *
 * (quirk) the diff-0 skip as in the other blocks. */
static FastState fast_loop_ge2(uint32_t *r5, int32_t r8, const int8_t *r3, int32_t r6,
                               uint32_t r7, uint32_t r4, uint32_t r11, int ld)
{
    bool ne = true;                                      /* movs r2, #0xFF000000 */
    int32_t r12 = (int32_t)r3[0] - r6;                   /* ldrsb r12, [r3]; sub r12, r12, r6 */
    uint32_t r9 = 0;
    int64_t cy = 0;
    do {
        ne = true;                                       /* subs r8, r8, #8 (or movs r2) */
        cy += M4A_HQMIX_FAST_ITER + 8 * (M4A_HQMIX_FAST_SAMPLE + 2 * ld);   /* two ldrsb */
        for (int k = 0; k < 8; k++) {
            if (ne) {
                r9 = r7 * (uint32_t)r12;
                cy += mul_m((uint32_t)r12);
            }
            r9 = (uint32_t)r6 + (uint32_t)((int32_t)r9 >> 23);
            r7 = r7 + r4;
            if (r9 != 0) {
                r5[k] = mla(r5[k], r11, (int32_t)r9);
                cy += M4A_HQMIX_MLA + mul_m(r9);
            }
            r3 += r7 >> 23;
            r6 = r3[0];
            r3 += 1;
            r12 = (int32_t)r3[0] - r6;
            ne = (r12 != 0);
            r7 &= 0x007FFFFFu;
        }
        r5 += 8;
        r8 -= 8;
    } while (r8 > 0);
    FastState s = { r6, r7, cy + M4A_HQMIX_LOOP_LAST };
    return s;
}

/* C_setup_fixed_freq_mixing .. C_mixing_end_store: the FIX path.  One
 * source sample per output sample at the unhalved volume r11, raw +-128.
 *
 *   C_fixed_mixing_length_check:
 *     cmp r2, r8; subgt lr, r8, #1; suble lr, r2, #1   @ min(count, left) - 1
 *     movs lr, lr, lsr#2; beq C_fixed_mixing_process_rest
 *     sub r8, r8, lr, lsl#2; sub r2, r2, lr, lsl#2
 *     (the alignment-selected sign-extension code is pasted; ldmia r3!, {r10})
 *   C_fixed_mixing_loop: 4 samples a word, mlane \reg, r11, r6, \reg (NE:
 *     the sample is not 0, a no-op skip), subs lr, lr, #1; bne
 *     sub r3, r3, #4
 *   C_fixed_mixing_process_rest: mov r1, #4
 *   C_fixed_mixing_unaligned_loop: ldrsb r6, [r3], #1; mla; subs r2, r2, #1;
 *     beq C_fixed_mixing_loop_or_end (loop: r2 = loopLen, r3 = loopStart
 *     absolute; else status 0, nothing stored); subs r1, r1, #1; bgt
 *   subs r8, r8, #4; bgt C_fixed_mixing_length_check
 *
 * The word loads ignore the pointer's low two bits and the pasted code
 * picks the bytes from the alignment, so every block mixes the four
 * bytes at the pointer; after the blocks the pointer is rewound to the
 * next unread byte.  The remainder is always four samples (r8 is a
 * multiple of 4 throughout), so the frame mixes exactly r8 samples or
 * stops at the data's end.  fw and sampleStor are not touched (the path
 * skips C_end_mixing). */
static bool fix_path(M4AEngine *engine, M4APCMChannel *r4, uint32_t *r5, int32_t r8, int32_t r2, int8_t *r3,
                     uint32_t r11)
{
    int64_t cy = M4A_HQMIX_TO_FIX + M4A_HQMIX_FIX_SETUP;
    int32_t loopLen = 0;                                 /* [sp, ARG_LOOP_LENGTH] */
    int8_t *loopStart = NULL;                            /* [sp, ARG_LOOP_START_POS] */
    if (r4->status & CHN_LOOP) {
        loopStart = r4->wav->data + r4->wav->loopStart;  /* add r3, r0 (not compressed) */
        loopLen = (int32_t)(r4->wav->size - r4->wav->loopStart);
    }
    /* Host guard, not in the source: a FIX channel started at its size
     * (an XCMD start offset clamped to the end) enters with count 0, which
     * the length check turns into 0x3FFFFFFF blocks (the ROM reads through
     * memory until the frame is full).  Stop the channel instead. */
    if (r2 <= 0) {                                       /* (cost: as a stop) */
        r4->status = 0;
        engine->mixCycles += cy + M4A_HQMIX_END_STOP;
        return true;
    }
    for (;;) {
        /* C_fixed_mixing_length_check */
        int32_t lr = ((r2 > r8 ? r8 : r2) - 1) >> 2;
        if (lr != 0) {
            r8 -= lr << 2;
            r2 -= lr << 2;
            cy += M4A_HQMIX_FIX_CHECK + (int64_t)lr * M4A_HQMIX_FIX_BLOCK + M4A_HQMIX_LOOP_LAST
                + M4A_HQMIX_FIX_REWIND;
            /* C_fixed_mixing_loop */
            do {
                for (int k = 0; k < 4; k++) {
                    int32_t r6 = r3[k];
                    if (r6 != 0) {
                        r5[k] = mla(r5[k], r11, r6);
                        cy += M4A_HQMIX_FIX_NONZERO;
                    }
                }
                r3 += 4;
                r5 += 4;
            } while (--lr != 0);
        } else {
            cy += M4A_HQMIX_FIX_NOBLOCK;
        }
        /* C_fixed_mixing_process_rest / C_fixed_mixing_unaligned_loop */
        cy += M4A_HQMIX_FIX_REST + 4 * M4A_HQMIX_FIX_SAMPLE + M4A_HQMIX_LOOP_LAST;
        for (int r1 = 4; r1 > 0; r1--) {
            int32_t r6 = *r3++;
            *r5 = mla(*r5, r11, r6);
            r5++;
            if (--r2 == 0) {
                /* C_fixed_mixing_loop_or_end */
                if (loopLen != 0) {
                    r2 = loopLen;
                    r3 = loopStart;
                    cy += M4A_HQMIX_FIX_WRAP;
                } else {
                    r4->status = 0;                      /* C_mixing_end_and_stop_channel */
                    /* this pass ends at _loop_or_end; the ones after it
                     * (and the last pass's bgt) never run */
                    cy += M4A_HQMIX_FIX_END + M4A_HQMIX_END_STOP - (r1 - 1) * M4A_HQMIX_FIX_SAMPLE
                        - M4A_HQMIX_LOOP_LAST;
                    engine->mixCycles += cy;
                    return true;
                }
            }
        }
        r8 -= 4;
        if (r8 <= 0)
            break;
        cy += M4A_HQMIX_FIX_NEXT;
    }
    /* C_mixing_end_store */
    r4->count = r2;
    r4->currentPointer = r3;
    engine->mixCycles += cy + M4A_HQMIX_FIX_DONE + M4A_HQMIX_END_STORE;
    return true;
}


/* ---- The pre-cached paths: C_data_load_comp / _uncomp_rev (section 2 of
 * docs/HQ-MIXER-ACCURACY.md).  The sample bytes of the frame are decoded
 * (DPCM) or byte-swapped (reversed) onto the stack, then the fast loop
 * plays them; `stack` stands in for [sp - r8, sp). ---- */

#define HQ_BLK          64                /* BDPCM_BLK_SIZE */
#define HQ_BLK_STRIDE   0x21              /* BDPCM_BLK_STRIDE */
#define HQ_STACK_BOUND  0x03007900u       /* stack_boundary_literal */
#define HQ_STACK_MAX    0x700             /* 0x03008000 - HQ_STACK_BOUND: the largest budget
                                           * any sp in IWRAM allows, so the host cap never
                                           * skips a frame the ROM's check would mix */

/* delta_lookup_table as F_decode_compressed indexes it.  The first pair's
 * delta is `ldrb r1, [r12, r3]` with the WHOLE second byte of the block,
 * so bytes 16..255 of the view are what follows the table in the mixer
 * image: stack_boundary_literal, then C_data_load_comp's code.  Read from
 * the HQ test ROM (the instructions are position-independent and no
 * self-modified slot lies in the range); real blocks keep that nibble 0. */
static const uint8_t sHqDeltaView[256] = {
    0x00, 0x01, 0x04, 0x09, 0x10, 0x19, 0x24, 0x31, 0xC0, 0xCF, 0xDC, 0xE7, 0xF0, 0xF7, 0xFC, 0xFF,
    0x00, 0x79, 0x00, 0x03, 0x2C, 0x90, 0x4F, 0x52, 0x28, 0x90, 0x4F, 0x42, 0x00, 0x50, 0x99, 0xE8,
    0x4C, 0x90, 0x4F, 0xE2, 0x00, 0x50, 0x89, 0xE8, 0x30, 0xC0, 0x4F, 0xE2, 0x20, 0x00, 0x00, 0x4A,
    0x00, 0xE0, 0x83, 0xE0, 0x40, 0xE0, 0x8E, 0xE2, 0x3F, 0xE0, 0xCE, 0xE3, 0x23, 0x93, 0xA0, 0xE1,
    0x09, 0x83, 0x4E, 0xE0, 0x3C, 0x10, 0x1F, 0xE5, 0x08, 0x10, 0x81, 0xE0, 0x0D, 0x00, 0x51, 0xE1,
    0xFB, 0x00, 0x00, 0x2A, 0x00, 0x10, 0x83, 0xE0, 0x00, 0x00, 0x52, 0xE0, 0x03, 0x00, 0x2D, 0xE9,
    0x08, 0xD0, 0x4D, 0xE0, 0x06, 0x00, 0x00, 0xCA, 0x02, 0x10, 0x83, 0xE0, 0x3F, 0x10, 0x81, 0xE2,
    0x3F, 0x10, 0xC1, 0xE3, 0x01, 0x10, 0x4E, 0xE0, 0x01, 0x80, 0x48, 0xE0, 0x08, 0x00, 0x8D, 0xE0,
    0x00, 0x02, 0x00, 0xEB, 0x3F, 0x30, 0x03, 0xE2, 0x0D, 0x00, 0xA0, 0xE1, 0x08, 0x20, 0x9A, 0xE5,
    0x00, 0x10, 0xC2, 0x05, 0x24, 0x20, 0x92, 0xE5, 0x10, 0x20, 0x82, 0xE2, 0x21, 0x10, 0xA0, 0xE3,
    0x91, 0x29, 0x29, 0xE0, 0xC1, 0xFF, 0xFF, 0xEB, 0x40, 0x80, 0x58, 0xE2, 0xFC, 0xFF, 0xFF, 0xCA,
    0x4B, 0x00, 0x00, 0xEA, 0x3F, 0xE0, 0x83, 0xE2, 0x3F, 0xE0, 0xCE, 0xE3, 0x00, 0x90, 0x43, 0xE0,
    0x01, 0x90, 0x49, 0xE2, 0x29, 0x93, 0xA0, 0xE1, 0x09, 0x83, 0x4E, 0xE0, 0xC4, 0xE0, 0x1F, 0xE5,
    0x08, 0xE0, 0x8E, 0xE0, 0x0D, 0x00, 0x5E, 0xE1, 0xD9, 0x00, 0x00, 0x2A, 0x00, 0xE0, 0x43, 0xE0,
    0x00, 0x00, 0x52, 0xE0, 0x01, 0x40, 0x2D, 0xE9, 0x0D, 0x00, 0xA0, 0xE1, 0x08, 0xD0, 0x4D, 0xE0,
    0x04, 0x00, 0x00, 0xCA, 0x02, 0x10, 0x43, 0xE0, 0x09, 0x13, 0x41, 0xE0, 0x01, 0x80, 0x48, 0xE0,
};

/* A frame whose first block lies before the sample (the reversed end
 * frame, section 2 and step 2(e)): r9 = (idx - advance - 1) lsr 6 wraps,
 * the block address data + 0x21 * r9 lands past the ROM (bit 31 set) and
 * the ldrb reads open bus -- for EVERY block of the frame, since the
 * decode loop steps that wrapped address by 0x21 and the real blocks 0,
 * 1, .. are read at data + 0x84000000 + 0x21 k, not at data + 0x21 k
 * (measured: cry_pidgey_echo_start_chorus frame 12, three open-bus blocks
 * where the port first decoded the two real ones).  mgba (GBALoadBad, ARM
 * code): cpu->prefetch[1] = the word two instructions ahead of the load,
 * byte-selected by address & 3; the three loads of F_decode_compressed
 * see F + 16, F + 20 and F + 28.  The WaveData is word-aligned in ROM and
 * 0x21 = 1 (mod 4), so the lane of byte j of block k is (j + k) & 3.  Real
 * hardware's open bus differs; the harness's truth is mgba's (ACCURACY.md,
 * known residuals). */
#define HQ_OPENBUS_W0 0xEA000003u         /* b C_bdpcm_decoder_loop_entry   (F + 16) */
#define HQ_OPENBUS_W1 0xE4D93001u         /* ldrb r3, [r9], #1              (F + 20) */
#define HQ_OPENBUS_WL 0xE0812002u         /* add r2, r1, r2                 (F + 28) */

static inline uint8_t hq_block_byte(const WaveData *wav, int32_t k, bool openBus, int j, uint32_t openBusWord)
{
    if (!openBus)
        return (uint8_t)wav->data[k * HQ_BLK_STRIDE + j];
    return (uint8_t)(openBusWord >> (8 * ((j + k) & 3)));
}

/* F_decode_compressed: block k (0x21 bytes) -> 64 samples in `out`.
 *   ldrb r2, [r9], #1; ldrb r3, [r9], #1
 *   entry: ldrb r1, [r12, r3]; add r1, r1, r2; (store r2, r1)
 *   loop:  ldrb r3, [r9], #1; ldrb r2, [r12, r3, lsr#4]; add r2, r1, r2;
 *          and r3, r3, #0xF; -> entry; subs lr, #2; bgt loop */
static void hq_decode_block(const WaveData *wav, int32_t k, bool openBus, uint8_t out[HQ_BLK])
{
    int j = 0, n = 0;
    uint8_t r2 = hq_block_byte(wav, k, openBus, j++, HQ_OPENBUS_W0);
    uint8_t r3 = hq_block_byte(wav, k, openBus, j++, HQ_OPENBUS_W1);
    uint8_t r1 = (uint8_t)(sHqDeltaView[r3] + r2);
    out[n++] = r2; out[n++] = r1;
    for (int lr = HQ_BLK - 2; lr > 0; lr -= 2) {
        r3 = hq_block_byte(wav, k, openBus, j++, HQ_OPENBUS_WL);
        r2 = (uint8_t)(sHqDeltaView[r3 >> 4] + r1);
        r3 &= 0xF;
        r1 = (uint8_t)(sHqDeltaView[r3] + r2);
        out[n++] = r2; out[n++] = r1;
    }
}

/* C_data_load_uncomp_rev reads words from (idx - advance - 1) & ~3 on; in
 * the end frame that is before the sample's data: the ROM's 16-byte
 * WaveData header (type, status, freq, loopStart, size, little-endian)
 * and, before it, the previous entry's tail.  The loader keeps those
 * bytes in front of the data (WaveData.romBefore); without them (a .wav
 * source, a hand-built sample) the header is rebuilt from the fields and
 * the rest reads as 0 (a residual, ACCURACY.md).  Past the data the ROM
 * holds the look-ahead byte and padding; only data[size] is kept. */
static uint8_t hq_rev_src_byte(const WaveData *wav, int32_t i)
{
    if (i >= 0)
        return i <= (int32_t)wav->size ? (uint8_t)wav->data[i] : 0;
    if (i >= -(int32_t)wav->romBefore)                   /* the loader kept the ROM's bytes */
        return (uint8_t)wav->data[i];
    if (i >= -16) {
        uint8_t hdr[16];
        uint32_t f[4] = { (uint32_t)wav->type | ((uint32_t)wav->status << 16), wav->freq, wav->loopStart, wav->size };
        for (int w = 0; w < 4; w++)
            for (int b = 0; b < 4; b++)
                hdr[4 * w + b] = (uint8_t)(f[w] >> (8 * b));
        return hdr[16 + i];
    }
    return 0;
}

/* C_select_highspeed_codepath: the block by step (subs r4, #0x800000;
 * movpl r11, lsl#1; subpls r4, #0x800000; addpl r4, #0x800000) and the
 * fast loop over the frame from `src`.  The cost: the selection (and the
 * paste when previous_fast_code holds another block) to C_end_mixing; `ld`
 * is an executed ldrsb's extra cycles from where `src` lives (the stack
 * copy, or ROM). */
static FastState fast_blocks(M4AEngine *engine, uint32_t *buf, int32_t r8, const int8_t *src, int32_t r6,
                             uint32_t r7, uint32_t r4step, uint32_t r11, int ld)
{
    FastState s;
    uint8_t code;
    uint32_t r4adj = r4step - 0x800000u;
    if ((int32_t)r4adj < 0) {
        code = 1;
        s = fast_loop_lt1(buf, r8, src, r6, r7, r4adj, r11, ld);
    } else {
        r11 <<= 1;
        r4adj -= 0x800000u;
        if ((int32_t)r4adj < 0) {
            code = 2;
            s = fast_loop_1to2(buf, r8, src, r6, r7, r4adj, r11, ld);
        } else {
            code = 3;
            s = fast_loop_ge2(buf, r8, src, r6, r7, r4adj + 0x800000u, r11, ld);
        }
    }
    s.cy += M4A_HQMIX_SELECT + (engine->hqFastCode != code ? M4A_HQMIX_PASTE : 0)
          + M4A_HQMIX_FAST_ENTRY + ld + M4A_HQMIX_FAST_EXIT;
    engine->hqFastCode = code;
    return s;
}

/* C_data_load_comp (forward / reverse) and C_data_load_uncomp_rev, then
 * the fast loop and C_end_mixing.  `idx` is the sample index the GBA keeps
 * in currentPointer (the byte offset from data for uncompressed data),
 * r0 the advance, r2 the count.  Every path: the byte range of the frame
 * rounded to blocks (words), the stack check (bhs C_end_mixing: the
 * channel is left exactly as it was, fw included), the end case when
 * count - advance <= 0 (F_clear_mem over the bytes past the sample's end,
 * then `streqb` status 0 -- the frame is still mixed), the fill, the fast
 * loop from sp + (idx & mask) and the stores (fw, sampleStor, count -
 * advance, the final index).  The reversed fills store downward from r0,
 * which F_clear_mem leaves where it was: in the end frame that is the
 * middle of the buffer, so the decoded (open-bus / pre-sample) bytes land
 * below the zeros and the real tail is never played (measured, step 2(e)). */
static bool precache_path(M4AEngine *engine, M4APCMChannel *r4, uint32_t *buf, int32_t r8,
                          int32_t r2, int32_t idx, int32_t r6, uint32_t r7, uint32_t r4step,
                          uint32_t r11, int32_t r0, uint32_t type)
{
    const WaveData *wav = r4->wav;
    static uint8_t stack[HQ_STACK_MAX];
    uint32_t spCheck = engine->hqStackSp - 12;           /* after stmfd sp!, {r4, r9, r12} */
    int32_t lr, lo, r8b, srcOff, finalIdx;
    int32_t newCount = r2 - r0;                          /* subs r0, r2, r0 */
    bool stop = false;
    int64_t cy;

    if (type & VOICE_TYPE_CMP) {
        uint32_t r9;                                     /* the first block's index (the mla's Rs) */
        int32_t blocks;
        bool openBus = false;
        if (!(type & VOICE_TYPE_REV)) {
            /* C_data_load_comp_for (no loop support: "TODO" in the source) */
            cy = M4A_HQMIX_COMP;
            lr = (idx + r0 + 1 + (HQ_BLK - 1)) & ~(HQ_BLK - 1);
            lo = idx & ~(HQ_BLK - 1);                    /* r9 << 6 */
            r9 = (uint32_t)idx >> 6;
            r8b = lr - lo;
            if (HQ_STACK_BOUND + (uint32_t)r8b >= spCheck || r8b > HQ_STACK_MAX) {
                engine->mixCycles += cy + M4A_HQMIX_STACK_SKIP + M4A_HQMIX_END;
                return true;
            }
            finalIdx = idx + r0;
            cy += M4A_HQMIX_COMP_FOR;
            if (newCount <= 0) {
                int32_t end = (idx + r2 + HQ_BLK - 1) & ~(HQ_BLK - 1);
                int32_t r1 = lr - end;
                r8b -= r1;
                memset(stack + r8b, 0, (size_t)r1);
                stop = true;
                cy += M4A_HQMIX_COMP_FOR_END - M4A_HQMIX_COMP_FOR + clear_cycles(r1);
            }
            srcOff = idx & (HQ_BLK - 1);
            uint8_t *dst = stack;
            for (int32_t k = lo >> 6, n = r8b; n > 0; n -= HQ_BLK, k++, dst += HQ_BLK)
                hq_decode_block(wav, k, false, dst);
        } else {
            /* C_data_load_comp_rev */
            cy = M4A_HQMIX_COMP_REV;
            lr = (idx + HQ_BLK - 1) & ~(HQ_BLK - 1);
            lo = (idx - r0 - 1) & ~(HQ_BLK - 1);         /* (lsr #6) << 6: floor, may be negative */
            r9 = (uint32_t)(idx - r0 - 1) >> 6;
            r8b = lr - lo;
            if (HQ_STACK_BOUND + (uint32_t)r8b >= spCheck || r8b > HQ_STACK_MAX) {
                engine->mixCycles += cy + M4A_HQMIX_STACK_SKIP_REV + M4A_HQMIX_END;
                return true;
            }
            finalIdx = idx - r0;
            cy += M4A_HQMIX_COMP_REV_SETUP;
            int32_t dst = r8b;                           /* mov r0, sp (the top) */
            if (newCount <= 0) {
                int32_t r1 = (idx - r2) - lo;
                r8b -= r1;
                memset(stack + r8b, 0, (size_t)r1);
                dst = r8b;
                stop = true;
                cy += M4A_HQMIX_COMP_REV_END - M4A_HQMIX_COMP_REV_SETUP + clear_cycles(r1);
            }
            srcOff = (-idx) & (HQ_BLK - 1);              /* rsb r3, r3, #0; and r3, #63 */
            uint8_t tmp[HQ_BLK];
            openBus = lo < 0;                            /* the wrapped r9: the whole frame */
            for (int32_t k = lo >> 6, n = r8b; n > 0; n -= HQ_BLK, k++) {
                hq_decode_block(wav, k, openBus, tmp);
                for (int m = 0; m < HQ_BLK; m++)         /* strb [r0, #-1]! */
                    stack[--dst] = tmp[m];
            }
        }
        /* C_data_load_comp_decode .. the last C_data_load_comp_loop pass
         * (the loop runs once even for an empty range: subs, bgt after
         * the first bl) */
        blocks = r8b > 0 ? (r8b + HQ_BLK - 1) / HQ_BLK : 1;
        cy += M4A_HQMIX_DECODE_SETUP + mul_m(r9) + (stop ? 1 : 0)
            + (int64_t)blocks * (M4A_HQMIX_BLOCK + (openBus ? M4A_HQMIX_BLOCK_OPENBUS : 0))
            + M4A_HQMIX_LOOP_LAST + M4A_HQMIX_DECODE_DONE;
    } else {
        /* C_data_load_uncomp_rev */
        lr = (idx + 3) & ~3;
        lo = (idx - r0 - 1) & ~3;
        r8b = lr - lo;
        if (HQ_STACK_BOUND + (uint32_t)r8b >= spCheck || r8b > HQ_STACK_MAX) {
            engine->mixCycles += M4A_HQMIX_STACK_SKIP_REV + M4A_HQMIX_END;
            return true;
        }
        finalIdx = idx - r0;
        cy = M4A_HQMIX_UREV;
        int32_t dst = r8b;
        if (newCount <= 0) {
            int32_t r1 = (idx - r2) - lo;
            r8b -= r1;
            memset(stack + r8b, 0, (size_t)r1);
            dst = r8b;
            stop = true;
            cy = M4A_HQMIX_UREV_END + clear_cycles(r1);
        }
        srcOff = (-idx) & 3;
        cy += (int64_t)(r8b > 0 ? (r8b + 3) / 4 : 1) * M4A_HQMIX_UREV_WORD + M4A_HQMIX_LOOP_LAST
            + M4A_HQMIX_UREV_DONE;
        for (int32_t i = lo, n = r8b; n > 0; n -= 4, i += 4) {
            /* ldmia r9!, {r1}; byte swap; stmdb r0!, {r1} */
            stack[dst - 1] = hq_rev_src_byte(wav, i);
            stack[dst - 2] = hq_rev_src_byte(wav, i + 1);
            stack[dst - 3] = hq_rev_src_byte(wav, i + 2);
            stack[dst - 4] = hq_rev_src_byte(wav, i + 3);
            dst -= 4;
        }
    }
    if (stop)
        r4->status = 0;                                  /* streqb r1, [r2, statusFlags] */
    if (getenv("HQ_DEBUG")) {                            /* the stack buffer, as the probe's buf= dump */
        fprintf(stderr, "HQSTACK idx %d adv %d cnt %d r8 %d off %d step %08X fw %08X vol %08X ss %d buf=",
                idx, r0, r2, r8b, srcOff, r4step, r7, r11, r6);
        for (int i = 0; i < r8b; i++) fprintf(stderr, "%02X", stack[i]);
        fputc('\n', stderr);
    }
    FastState s = fast_blocks(engine, buf, r8, (const int8_t *)stack + srcOff, r6, r7, r4step, r11,
                              M4A_HQMIX_LOAD_STACK);
    engine->mixCycles += cy + s.cy + M4A_HQMIX_END;
    /* C_end_mixing / C_mixing_end_store */
    r4->fw = s.r7;
    r4->sampleStor = (int8_t)s.r6;
    r4->count = newCount;
    r4->currentPointer = wav->data + finalIdx;
    return true;
}

/* C_setup_synth .. C_synth_triangle_loop: the synth paths, reached from
 * C_mixing_setup_comp_rev after the step multiply and BEFORE the volume
 * halving, so r11 is the unhalved 00LL00RR; r3 = the sample pointer = the
 * descriptor (count and the pointer are stored back by C_mixing_end_store:
 * the pointer unchanged, count as the pulse's LFO accumulator / the saw's
 * filter state).  The oscillators are m4a_synth.h's; per path:
 *
 * pulse (data[1] == 0), 8 samples an iteration (the frame is a multiple
 * of 8 in HQ mode):
 *   cmp r7, r6 / addlo \reg, \reg, r11, lsl#6 / subhs \reg, \reg, r11, lsl#6
 *   adds r7, r7, r4, lsl#3
 * i.e. +-64 * v at the packed word (the subtract borrows across the
 * halves exactly as mla with -64 does); r6 = the threshold, whose low
 * byte C_end_mixing stores as CHN_SAMPLE_STOR.
 *
 * saw (data[1] == 1), 4 an iteration: the volume halved to floor(v / 2)
 * per side (mov r11, r11, lsr#1 / bic r11, r11, #0xFF00), then per sample
 *   mlane \reg, r11, r2, \reg     @ the filter state, skipped when 0
 * with r6 = r7 << 1 left from the last sample (its low byte stored).
 *
 * triangle (anything else), 4 an iteration: mla \reg, r11, r9, \reg at
 * the unhalved volume; r6 = 0x80 stored. */
static void synth_path(M4AEngine *engine, M4APCMChannel *r4, uint32_t *r5, int32_t r8, uint32_t r7,
                       uint32_t r4step, uint32_t r11)
{
    int64_t cy;
    const uint8_t *r3 = (const uint8_t *)r4->currentPointer;   /* the descriptor */
    uint32_t step8 = r4step << 3;
    int32_t r2 = r4->count;
    uint32_t r6;
    uint8_t r12 = r3[1];                                 /* ldrb r12, [r3, #SYNTH_TYPE] */
    if (r12 == 0) {
        r6 = m4a_synth_pulse_setup(r4);                  /* r2 (count) advanced in the channel */
        r2 = r4->count;
        for (int32_t i = 0; i < r8; i++)
            r5[i] = mla(r5[i], r11, m4a_synth_pulse_sample(&r7, r6, step8));
        cy = M4A_HQMIX_SYNTH + M4A_HQMIX_PULSE_SETUP + (int64_t)(r8 >> 3) * M4A_HQMIX_PULSE_ITER
           + M4A_HQMIX_LOOP_LAST + M4A_HQMIX_PULSE_EXIT;
    } else if (r12 == 1) {                               /* C_check_synth_saw: subs r12, #1 */
        uint32_t r11h = (r11 >> 1) & ~0xFF00u;
        r6 = 0;
        cy = M4A_HQMIX_SAW + (int64_t)(r8 >> 2) * (M4A_HQMIX_SAW_ITER + 4 * M4A_HQMIX_SAW_SAMPLE)
           + M4A_HQMIX_LOOP_LAST + M4A_HQMIX_SYNTH_EXIT;
        for (int32_t i = 0; i < r8; i++) {
            int32_t v = m4a_synth_saw_sample(&r7, &r2, step8);
            r6 = r7 << 1;                                /* mov r6, r7, lsl#1 */
            if (v != 0) {                                /* mlane */
                r5[i] = mla(r5[i], r11h, v);
                cy += M4A_HQMIX_MLA + mul_m((uint32_t)v);
            }
        }
    } else {                                             /* C_synth_triangle */
        r6 = 0x80;
        cy = M4A_HQMIX_TRI + (int64_t)(r8 >> 2) * (M4A_HQMIX_TRI_ITER + 4 * M4A_HQMIX_TRI_SAMPLE)
           + M4A_HQMIX_LOOP_LAST + M4A_HQMIX_SYNTH_EXIT;
        for (int32_t i = 0; i < r8; i++) {
            int32_t v = m4a_synth_triangle_sample(&r7, step8);
            r5[i] = mla(r5[i], r11, v);
            cy += mul_m((uint32_t)v);
        }
    }
    engine->mixCycles += cy + M4A_HQMIX_END;
    /* C_end_mixing / C_mixing_end_store */
    r4->fw = r7;
    r4->sampleStor = (int8_t)(uint8_t)r6;
    r4->count = r2;
}

bool m4a_hq_mix_channel(M4AEngine *engine, M4APCMChannel *r4, uint32_t *buf)
{
    /* C_mixing_setup */
    int32_t r6 = r4->sampleStor;                         /* ldrsb r6, [r4, CHN_SAMPLE_STOR] */
    int32_t r8 = engine->pcmSamplesPerVBlank;            /* ldr r8, [sp, ARG_FRAME_LENGTH] */
    uint32_t r11 = ((uint32_t)r4->envelopeVolumeLeft << 16) | r4->envelopeVolumeRight;
    if (r11 == 0) {
        engine->mixCycles += M4A_HQMIX_VOL_ZERO;
        return true;                                     /* beq C_mixing_epilogue: nothing stored */
    }
    uint32_t r0 = r4->type;
    /* tst CMP|REV; bne C_mixing_setup_comp_rev; tst FIX; bne C_setup_fixed_freq_mixing */
    if (!(r0 & (VOICE_TYPE_CMP | VOICE_TYPE_REV)) && (r0 & VOICE_TYPE_FIX))
        return fix_path(engine, r4, buf, r8, r4->count, r4->currentPointer, r11);
    /* C_mixing_setup_comp_rev */
    uint32_t r7 = r4->fw;
    uint32_t r4step = (uint32_t)engine->pcmDivFreq * r4->frequency;   /* mul r4, lr, r12 */
    int mDiv = mul_m((uint32_t)engine->pcmDivFreq);
    if (r0 & VOICE_TYPE_SPL) {                           /* tst r0, SPL; bne C_setup_synth */
        engine->mixCycles += M4A_HQMIX_TO_COMP_REV + M4A_HQMIX_SETUP_SYNTH + mDiv;
        synth_path(engine, r4, buf, r8, r7, r4step, r11);
        return true;
    }
    /* to C_data_load_uncomp_for (the step multiply, umlal by the frame
     * length); the pre-cached paths branch off before its end */
    int64_t setup = M4A_HQMIX_TO_COMP_REV + M4A_HQMIX_SETUP + mDiv + umul_m((uint32_t)r8);
    if (r0 & VOICE_TYPE_FIX)
        r4step = 0x800000u;                              /* tst r0, FIX; movne r4, #0x800000 */
    /* Mixing goes with volume ranges 0-127: movs lsr #1 / adc #0x8000 /
     * bic #0x8000 -> ceil(L / 2) << 16 | ceil(R / 2). */
    {
        uint32_t c = r11 & 1;
        r11 = (r11 >> 1) + 0x8000u + c;
        r11 &= ~0x8000u;
    }
    int32_t r2 = r4->count;
    int8_t *r3 = r4->currentPointer;
    /* umlal r1, r0, r4, r8: advance = (fw + step * frameLength) >> 23 (the
     * type bits r0 carries in its top five bits are shifted out). */
    uint64_t sum = (uint64_t)r7 + (uint64_t)r4step * (uint64_t)r8;
    uint32_t advance = (uint32_t)(sum >> 23);

    /* bcs C_data_load_comp; bmi C_data_load_uncomp_rev (the flags of
     * movs r0, r0, lsl#27: C = CMP, N = REV) */
    if (r0 & (VOICE_TYPE_CMP | VOICE_TYPE_REV)) {
        engine->mixCycles += setup + ((r0 & VOICE_TYPE_CMP) ? M4A_HQMIX_SETUP_CMP : M4A_HQMIX_SETUP_UREV);
        return precache_path(engine, r4, buf, r8, r2, (int32_t)(r3 - r4->wav->data), r6, r7,
                             r4step, r11, (int32_t)advance, r0);
    }

    /* C_data_load_uncomp_for: cmp r2, r0; ble C_unbuffered_mixing */
    if (r2 > (int32_t)advance) {
        /* The fast path: the frame cannot reach the sample's end.  count
         * and the final pointer are computed here (stmfd {r2, r9}) and
         * stored after the loop; the source bytes come from the stack
         * copy (DMA3 / CPU, below 0x400 bytes) or ROM, identical content,
         * not modelled.  Then C_select_highspeed_codepath's block.  The
         * cost: the copy (0x03007900 + advance < sp, sp after the stmfd
         * {r4, r9, r12}, and advance < 0x400) by DMA3 of (advance + 7) >> 2
         * words, the loads then from the stack; otherwise from ROM. */
        bool copy = HQ_STACK_BOUND + advance < engine->hqStackSp - 12 && advance < 0x400;
        if (copy) {
            /* the DMA3 block, from the stmia that starts it: the span's
             * FIFO refills that fall in it cost less (m4a_engine.c) */
            int32_t words = (int32_t)((advance + 7) >> 2);
            int64_t len = M4A_HQMIX_DMA3 + M4A_HQMIX_DMA3_WORD * (int64_t)words;
            if (engine->hqDma3Count < M4A_HQ_DMA3_MAX) {
                engine->hqDma3Start[engine->hqDma3Count] = (int32_t)(engine->mixCycles + setup + M4A_HQMIX_COPY);
                engine->hqDma3Len[engine->hqDma3Count] = (int32_t)len;
                engine->hqDma3Count++;
            }
            setup += M4A_HQMIX_COPY + len + M4A_HQMIX_VLA;
        } else {
            setup += M4A_HQMIX_DIRECT;
        }
        FastState s = fast_blocks(engine, buf, r8, r3, r6, r7, r4step, r11,
                                  copy ? M4A_HQMIX_LOAD_STACK : M4A_HQMIX_LOAD_ROM);
        engine->mixCycles += setup + s.cy + M4A_HQMIX_END;
        /* C_end_mixing / C_mixing_end_store */
        r4->fw = s.r7;
        r4->sampleStor = (int8_t)s.r6;
        r4->count = r2 - (int32_t)advance;
        r4->currentPointer = r3 + advance;
        return true;
    }

    /* C_unbuffered_mixing: one sample per iteration, mixed before the
     * end check; loop wrap relative, end -> status 0 and nothing stored. */
    int32_t loopLen = 0;                                 /* [sp, ARG_LOOP_LENGTH] */
    if (r4->status & CHN_LOOP)
        loopLen = (int32_t)(r4->wav->size - r4->wav->loopStart);
    int32_t r12 = (int32_t)r3[0] - r6;
    uint32_t *r5 = buf + r8;                             /* add r5, r5, r8, lsl#2 */
    int64_t cy = setup + M4A_HQMIX_UNBUF + (int64_t)r8 * M4A_HQMIX_UNBUF_SAMPLE + M4A_HQMIX_LOOP_LAST;
    for (;;) {
        uint32_t r9 = r7 * (uint32_t)r12;                /* mul r9, r7, r12 */
        cy += mul_m((uint32_t)r12);
        r9 = (uint32_t)((int32_t)r9 >> 22);
        r9 = r9 + ((uint32_t)r6 << 1);                   /* adds */
        if (r9 != 0) {
            uint32_t *w = r5 - r8;                       /* [r5, -r8, lsl#2] */
            *w = mla(*w, r11, (int32_t)r9);
            cy += M4A_HQMIX_UNBUF_MIX + mul_m(r9);
        }
        r7 = r7 + r4step;
        uint32_t adv = r7 >> 23;                         /* movs r9, r7, lsr#23 */
        if (adv != 0) {
            r2 -= (int32_t)adv;                          /* subs r2, r2, r9 */
            if (r2 <= 0) {                               /* ble C_unbuffered_mixing_loop_or_end */
                if (loopLen != 0) {
                    r3 -= loopLen;
                    r2 += loopLen;
                    cy += M4A_HQMIX_UNBUF_WRAP;
                } else {
                    r4->status = 0;                      /* C_mixing_end_and_stop_channel */
                    /* this pass ends at _loop_or_end; the ones after it
                     * (and the last one's bgt) never run */
                    engine->mixCycles += cy + M4A_HQMIX_UNBUF_END + M4A_HQMIX_END_STOP
                                       - (int64_t)(r8 - 1) * M4A_HQMIX_UNBUF_SAMPLE - M4A_HQMIX_LOOP_LAST;
                    return true;
                }
            }
            /* C_unbuffered_mixing_loop_continue */
            adv -= 1;
            cy += M4A_HQMIX_UNBUF_ADVANCE;
            if (adv == 0) {
                r6 = r12 + r6;
            } else {
                r3 += adv;
                r6 = r3[0];
                cy += M4A_HQMIX_UNBUF_ADVANCE_N;
            }
            r3 += 1;
            r12 = (int32_t)r3[0] - r6;
            r7 &= ~0x3F800000u;
        }
        /* C_unbuffered_mixing_skip_load */
        if (--r8 <= 0)
            break;
    }
    engine->mixCycles += cy + M4A_HQMIX_END;
    /* C_end_mixing / C_mixing_end_store */
    r4->fw = r7;
    r4->sampleStor = (int8_t)r6;
    r4->count = r2;
    r4->currentPointer = r3;
    return true;
}

/* One side of the downsampler: t = state + w; saturate t to 31 bits
 * (adds t2 = t + t; eorvs t = 0xC0000000 ^ (t2 asr 31)); the output byte
 * is bits 23..30 of t, the new state bits 16..22. */
static inline uint32_t downsample_side(uint32_t *state, uint32_t w, uint32_t *outByte)
{
    uint32_t r12 = *state + w;
    uint32_t t2 = r12 + r12;
    /* V: the signs of the two addends agree and differ from the sum's. */
    if (((r12 ^ t2) & 0x80000000u) != 0)
        r12 = 0xC0000000u ^ (uint32_t)((int32_t)t2 >> 31);
    *state = r12 & 0x007F0000u;
    *outByte = (r12 >> 15) & 0xFF00u;                    /* and rX, r11, r12, lsr#15 */
    return r12;
}

/* A signed 8-bit ring byte as the ldrsh + asr / lsl-asr pair reads it. */
static inline int32_t ring_tap(const int8_t *p)
{
    return (int32_t)p[0];
}

void m4a_hq_downsample(M4AEngine *engine, int frameOffset, int pcmDmaCounter)
{
    /* C_main_mixer_return */
    uint32_t r4 = (uint32_t)engine->hqNoiseShapeLeft << 16;
    uint32_t r5 = (uint32_t)engine->hqNoiseShapeRight << 16;
    uint32_t r2 = (uint32_t)engine->reverb.amount >> 2;
    /* C_downsampler */
    int32_t r8 = engine->pcmSamplesPerVBlank;
    int8_t *r9 = engine->pcmBuffer + frameOffset;        /* ARG_BUFFER_POS */
    r2 |= r2 << 16;
    /* r3: the offset to the next slot's bytes -- the frame length, or
     * back to the ring start when this is the last slot (hint == 2). */
    int32_t r3 = (pcmDmaCounter != 2) ? r8 : (int32_t)(engine->pcmBuffer - r9);
    uint32_t *r10 = engine->hqBuffer;
    /* The cost from C_main_mixer_return to m4aSoundMain's return; each
     * pass's two reverb products add their m beyond 1 (Rs = the tap sum). */
    int64_t cy = M4A_HQMIX_MAIN_RETURN + M4A_HQMIX_DOWNSAMPLE + (int64_t)(r8 >> 1) * M4A_HQMIX_DOWNSAMPLE_PAIR
               + M4A_HQMIX_RETURN + M4A_MIX_EPILOGUE;
    do {
        uint32_t w0 = r10[0], w1 = r10[1];
        uint32_t r6, r7, b;
        downsample_side(&r4, w0, &r6);                   /* left sample #1 */
        downsample_side(&r5, w0 << 16, &r7);             /* right sample #1 */
        downsample_side(&r4, w1, &b);                    /* left sample #2 */
        r6 = b | (r6 >> 8);
        downsample_side(&r5, w1 << 16, &b);              /* right sample #2 */
        r7 = b | (r7 >> 8);
        /* The reverb taps (ENABLE_REVERB 1): next slot A, next slot B,
         * this slot's old B, this slot's old A -- sample 1 into r0,
         * sample 2 into r1 -- then the two int8 pairs are stored. */
        int8_t *next = r9 + r3;
        int32_t r0 = ring_tap(next), r1 = ring_tap(next + 1);
        r0 += ring_tap(next + BUF); r1 += ring_tap(next + BUF + 1);
        r0 += ring_tap(r9 + BUF);   r1 += ring_tap(r9 + BUF + 1);
        r9[BUF] = (int8_t)(uint8_t)r6;                   /* strh r6, [r9 + BUF] */
        r9[BUF + 1] = (int8_t)(uint8_t)(r6 >> 8);
        r0 += ring_tap(r9);         r1 += ring_tap(r9 + 1);
        r9[0] = (int8_t)(uint8_t)r7;                     /* strh r7, [r9], #2 */
        r9[1] = (int8_t)(uint8_t)(r7 >> 8);
        r9 += 2;
        r10[0] = r2 * (uint32_t)r0;                      /* the next frame's seed */
        r10[1] = r2 * (uint32_t)r1;
        cy += mul_m((uint32_t)r0) + mul_m((uint32_t)r1) - 2;
        r10 += 2;
        r8 -= 2;
    } while (r8 > 0);
    /* C_downsampler_return */
    engine->mixCycles += cy;
    engine->hqNoiseShapeLeft = (uint8_t)(r4 >> 16);
    engine->hqNoiseShapeRight = (uint8_t)(r5 >> 16);
}

/* C_channel_state_loop for a channel with a status flag set, by its state
 * before the step: the cycles to C_channel_vol_calc, or to
 * C_end_channel_state_loop (M4A_HQMIX_STOP included) when the step ends
 * the channel.  The same decisions as m4a_hq_channel_init and
 * m4a_pcm_channel_tick, which run after this. */
static int env_cycles(const M4APCMChannel *ch)
{
    uint8_t st = ch->status;
    unsigned env = ch->envelopeVolume;
    int c;
    if (st & CHN_START) {
        if (st & CHN_STOP)
            return M4A_HQMIX_START_STOP + M4A_HQMIX_STOP;
        const WaveData *w = ch->wav;
        if (w->size == 0)                                /* C_channel_init_synth */
            c = (uint8_t)w->data[1] == 2 ? M4A_HQMIX_INIT_SYNTH_TRI
                                         : M4A_HQMIX_INIT_SYNTH + M4A_HQMIX_INIT_CHECK;
        else if (w->type & 1)                            /* C_channel_init_comp */
            c = ((ch->type & VOICE_TYPE_REV) ? M4A_HQMIX_INIT_CMP_REV : M4A_HQMIX_INIT_CMP_FWD) + M4A_HQMIX_INIT_CHECK;
        else
            c = ((ch->type & VOICE_TYPE_REV) ? M4A_HQMIX_INIT_REV : M4A_HQMIX_INIT_FWD) + M4A_HQMIX_INIT_CHECK;
        c += M4A_HQMIX_INIT_FLAGS + ((w->status & 0xC000) ? M4A_HQMIX_INIT_FLAGS_LOOP : 0);
        env = 0;
        goto attack;
    }
    c = M4A_HQMIX_ENV_HEAD;
    if (st & CHN_IEC)                                    /* sub r0, #1; bhi: the length was >= 2 */
        return c + (ch->pseudoEchoLength <= 1 ? M4A_HQMIX_ENV_IEC_END + M4A_HQMIX_STOP : M4A_HQMIX_ENV_IEC);
    c += M4A_HQMIX_ENV_NOT_IEC;
    if (st & CHN_STOP) {
        env = (env * ch->release) >> 8;
        if (env == 0) {                                  /* lsr; ble */
            c += M4A_HQMIX_ENV_RELEASE_ZERO;
            goto released;
        }
        if (env > ch->pseudoEchoVolume)
            return c + M4A_HQMIX_ENV_RELEASE;
        c += M4A_HQMIX_ENV_RELEASE_LOW;
        goto released;
    }
    c += M4A_HQMIX_ENV_NOT_STOP;
    if ((st & CHN_ENV_MASK) == CHN_ENV_DECAY) {
        env = (env * ch->decay) >> 8;
        if (env > ch->sustain)
            return c + M4A_HQMIX_ENV_DECAY;
        if (ch->sustain != 0)
            return c + M4A_HQMIX_ENV_DECAY_SUSTAIN;
        c += M4A_HQMIX_ENV_DECAY_ZERO;
        goto released;
    }
    c += M4A_HQMIX_ENV_NOT_DECAY;
    if ((st & CHN_ENV_MASK) != CHN_ENV_ATTACK)
        return c + M4A_HQMIX_ENV_SUSTAIN;
    c += M4A_HQMIX_ENV_TO_ATTACK;
attack:                                                  /* C_adsr_attack */
    return c + (env + ch->attack >= 0xFF ? M4A_HQMIX_ENV_ATTACK_PEAK : M4A_HQMIX_ENV_ATTACK);
released:                                                /* C_adsr_released */
    return c + (ch->pseudoEchoVolume == 0 ? M4A_HQMIX_ENV_ECHO_NONE + M4A_HQMIX_STOP : M4A_HQMIX_ENV_ECHO_START);
}

void m4a_hq_sound_main_ram(M4AEngine *engine, int frameOffset, int pcmDmaCounter)
{
    int spv = engine->pcmSamplesPerVBlank;
    /* C_channel_state_loop over the real pool, then the shadow pool
     * (polyphony debug), which mixes into a discarded buffer unless the
     * invert mode makes it the audible one (as m4a_sound_main_ram). */
    uint32_t *muted = engine->hqMutedBuffer;
    bool invert = engine->polyDebugInvert;
    memset(muted, 0, (size_t)spv * sizeof(uint32_t));
    /* The cost (m4a_engine.c publishes it): the entry, the walk over
     * maxChans slots of the real pool (the shadow pool, debug only, costs
     * nothing and leaves previous_fast_code alone), the downsampler. */
    int slots = engine->maxPcmChannels < MAX_PCM_CHANNELS ? engine->maxPcmChannels : MAX_PCM_CHANNELS;
    engine->mixCycles = M4A_HQMIX_ENTRY + M4A_HQMIX_LOOP_LAST;
    engine->hqDma3Count = 0;
    for (int i = 0; i < TOTAL_PCM_CHANNELS; i++) {
        M4APCMChannel *r4 = &engine->pcmChannels[i];
        bool slot = i < slots;
        if (!(r4->status & CHN_ON)) {
            if (slot)
                engine->mixCycles += M4A_HQMIX_SKIP + M4A_HQMIX_LOOP_NEXT;
            continue;
        }
        /* Charged in order (not summed apart as the vanilla walk is): the
         * DMA3 copies' positions in the span matter (m4a_engine.c). */
        if (slot)
            engine->mixCycles += env_cycles(r4);
        if ((r4->status & (CHN_START | CHN_STOP)) == CHN_START)
            m4a_hq_channel_init(r4);
        /* The envelope state machine is vanilla's; the volumes are not. */
        m4a_pcm_channel_tick(r4, engine->masterVolume);
        if (!(r4->status & CHN_ON)) {
            if (slot)
                engine->mixCycles += M4A_HQMIX_LOOP_NEXT;
            continue;
        }
        m4a_hq_channel_volumes(r4, engine->masterVolume);
        /* SF_LOOP as the ROM sets it at SF_START: WaveData's loop flag
         * (the port drops CHN_LOOP for a loop of length 0, e.g. a synth) */
        if (slot)
            engine->mixCycles += M4A_HQMIX_VOL + ((r4->wav->status & 0xC000)
                                   ? ((r4->type & VOICE_TYPE_CMP) ? M4A_HQMIX_VOL_LOOP_CMP : M4A_HQMIX_VOL_LOOP)
                                   : 0);
        int64_t before = engine->mixCycles;
        uint8_t fastCode = engine->hqFastCode;
        int dma3 = engine->hqDma3Count;
        bool audible = ((i >= MAX_PCM_CHANNELS) == invert)
                    || (i < MAX_PCM_CHANNELS && r4->audition);
        if (!m4a_hq_mix_channel(engine, r4, audible ? engine->hqBuffer : muted))
            engine->hqUnportedChannelFrames++;
        if (slot)
            engine->mixCycles += M4A_HQMIX_LOOP_NEXT;
        if (!slot) {
            engine->mixCycles = before;
            engine->hqFastCode = fastCode;
            engine->hqDma3Count = dma3;
        }
    }
    if (getenv("HQ_DEBUG")) {
        int n = engine->pcmSamplesPerVBlank;
        fprintf(stderr, "HQDBG ns %02X,%02X off %d cnt %d buf", engine->hqNoiseShapeLeft, engine->hqNoiseShapeRight, frameOffset, pcmDmaCounter);
        for (int k = 0; k < n; k++) fprintf(stderr, " %08X", engine->hqBuffer[k]);
        fputc('\n', stderr);
    }
    m4a_hq_downsample(engine, frameOffset, pcmDmaCounter);
    if (getenv("HQ_DEBUG")) {
        int n = engine->pcmSamplesPerVBlank;
        fprintf(stderr, "HQRING ringA=");
        for (int k = 0; k < n; k++) fprintf(stderr, "%02X", (uint8_t)engine->pcmBuffer[frameOffset + k]);
        fprintf(stderr, " ringB=");
        for (int k = 0; k < n; k++) fprintf(stderr, "%02X", (uint8_t)engine->pcmBuffer[BUF + frameOffset + k]);
        fputc('\n', stderr);
    }
}
