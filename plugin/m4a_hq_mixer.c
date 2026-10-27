#include "m4a_hq_mixer.h"
#include "m4a_channel.h"
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
 * not modeled.
 */

#define BUF GBA_PCM_DMA_BUF_SIZE

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
    /* C_channel_init_synth: TONEDATA_TYPE_SPL and the triangle's
     * 0x40000000 phase -- m4a_pcm_channel_start_at primes synthType / fw
     * the same way. */
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

/* The registers the fast loop carries out: the base sample and position. */
typedef struct { int32_t r6; uint32_t r7; } FastState;

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
                               uint32_t r7, uint32_t r4, uint32_t r11)
{
    bool ne = true;                                      /* movs r2, #0xFF000000 */
    int32_t r12 = (int32_t)r3[0] - r6;                   /* ldrsb r12, [r3]; sub r12, r12, r6 */
    uint32_t r9 = 0;                                     /* holds r3 + advance; never read before mulne */
    do {
        ne = true;                                       /* subs r8, r8, #8 (or movs r2) */
        for (int k = 0; k < 8; k++) {
            if (ne)
                r9 = r7 * (uint32_t)r12;
            r9 = (uint32_t)((int32_t)r9 >> 22);
            r9 = r9 + ((uint32_t)r6 << 1);
            if (r9 != 0)
                r5[k] = mla(r5[k], r11, (int32_t)r9);
            r7 = r7 + r4;
            if ((int32_t)r7 >= 0) {                      /* PL */
                r6 = r12 + r6;
                r3 += 1;
                r12 = (int32_t)r3[0] - r6;
                ne = (r12 != 0);
            } else {
                ne = true;                               /* a negative sum is never 0 */
            }
            r7 &= 0x007FFFFFu;
        }
        r5 += 8;
        r8 -= 8;
    } while (r8 > 0);
    FastState s = { r6, r7 };
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
                                uint32_t r7, uint32_t r4, uint32_t r11)
{
    bool ne = true;                                      /* movs r2, #0xFF000000 */
    int32_t r12 = (int32_t)r3[0] - r6;                   /* ldrsb r12, [r3]; sub r12, r12, r6 */
    uint32_t r9 = 0;
    do {
        ne = true;                                       /* subs r8, r8, #8 (or movs r2) */
        for (int k = 0; k < 8; k++) {
            if (ne)
                r9 = r7 * (uint32_t)r12;
            r9 = (uint32_t)r6 + (uint32_t)((int32_t)r9 >> 23);
            r6 = r12 + r6;
            if (r9 != 0)
                r5[k] = mla(r5[k], r11, (int32_t)r9);
            r7 = r7 + r4;
            if ((int32_t)r7 >= 0) {                      /* PL */
                r3 += 1;
                r6 = r3[0];
            }
            r3 += 1;
            r12 = (int32_t)r3[0] - r6;
            ne = (r12 != 0);
            r7 &= 0x007FFFFFu;
        }
        r5 += 8;
        r8 -= 8;
    } while (r8 > 0);
    FastState s = { r6, r7 };
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
                               uint32_t r7, uint32_t r4, uint32_t r11)
{
    bool ne = true;                                      /* movs r2, #0xFF000000 */
    int32_t r12 = (int32_t)r3[0] - r6;                   /* ldrsb r12, [r3]; sub r12, r12, r6 */
    uint32_t r9 = 0;
    do {
        ne = true;                                       /* subs r8, r8, #8 (or movs r2) */
        for (int k = 0; k < 8; k++) {
            if (ne)
                r9 = r7 * (uint32_t)r12;
            r9 = (uint32_t)r6 + (uint32_t)((int32_t)r9 >> 23);
            r7 = r7 + r4;
            if (r9 != 0)
                r5[k] = mla(r5[k], r11, (int32_t)r9);
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
    FastState s = { r6, r7 };
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
static bool fix_path(M4APCMChannel *r4, uint32_t *r5, int32_t r8, int32_t r2, int8_t *r3, uint32_t r11)
{
    int32_t loopLen = 0;                                 /* [sp, ARG_LOOP_LENGTH] */
    int8_t *loopStart = NULL;                            /* [sp, ARG_LOOP_START_POS] */
    if (r4->status & CHN_LOOP) {
        loopStart = r4->wav->data + r4->wav->loopStart;  /* add r3, r0 (not compressed) */
        loopLen = (int32_t)(r4->wav->size - r4->wav->loopStart);
    }
    for (;;) {
        /* C_fixed_mixing_length_check */
        int32_t lr = ((r2 > r8 ? r8 : r2) - 1) >> 2;
        if (lr != 0) {
            r8 -= lr << 2;
            r2 -= lr << 2;
            /* C_fixed_mixing_loop */
            do {
                for (int k = 0; k < 4; k++) {
                    int32_t r6 = r3[k];
                    if (r6 != 0)
                        r5[k] = mla(r5[k], r11, r6);
                }
                r3 += 4;
                r5 += 4;
            } while (--lr != 0);
        }
        /* C_fixed_mixing_process_rest / C_fixed_mixing_unaligned_loop */
        for (int r1 = 4; r1 > 0; r1--) {
            int32_t r6 = *r3++;
            *r5 = mla(*r5, r11, r6);
            r5++;
            if (--r2 == 0) {
                /* C_fixed_mixing_loop_or_end */
                if (loopLen != 0) {
                    r2 = loopLen;
                    r3 = loopStart;
                } else {
                    r4->status = 0;                      /* C_mixing_end_and_stop_channel */
                    return true;
                }
            }
        }
        r8 -= 4;
        if (r8 <= 0)
            break;
    }
    /* C_mixing_end_store */
    r4->count = r2;
    r4->currentPointer = r3;
    return true;
}

bool m4a_hq_mix_channel(M4AEngine *engine, M4APCMChannel *r4, uint32_t *buf)
{
    /* C_mixing_setup */
    int32_t r6 = r4->sampleStor;                         /* ldrsb r6, [r4, CHN_SAMPLE_STOR] */
    int32_t r8 = engine->pcmSamplesPerVBlank;            /* ldr r8, [sp, ARG_FRAME_LENGTH] */
    uint32_t r11 = ((uint32_t)r4->envelopeVolumeLeft << 16) | r4->envelopeVolumeRight;
    if (r11 == 0)
        return true;                                     /* beq C_mixing_epilogue: nothing stored */
    uint32_t r0 = r4->type;
    if (r0 & (VOICE_TYPE_CMP | VOICE_TYPE_REV))          /* -> C_data_load_comp / _uncomp_rev */
        return false;
    if (r0 & VOICE_TYPE_FIX)                             /* bne C_setup_fixed_freq_mixing */
        return fix_path(r4, buf, r8, r4->count, r4->currentPointer, r11);
    if (r4->synthType != M4A_SYNTH_NONE)                 /* TONEDATA_TYPE_SPL -> C_setup_synth */
        return false;

    /* C_mixing_setup_comp_rev */
    uint32_t r7 = r4->fw;
    uint32_t r4step = (uint32_t)engine->pcmDivFreq * r4->frequency;   /* mul r4, lr, r12 */
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

    /* C_data_load_uncomp_for: cmp r2, r0; ble C_unbuffered_mixing */
    if (r2 > (int32_t)advance) {
        /* The fast path: the frame cannot reach the sample's end.  count
         * and the final pointer are computed here (stmfd {r2, r9}) and
         * stored after the loop; the source bytes come from the stack
         * copy or ROM, identical content. */
        /* C_select_highspeed_codepath: the block by step.
         *   subs r4, r4, #0x800000      @ r4 = step - 1.0
         *   movpl r11, r11, lsl#1       @ >= 1.0: 0-127 -> 0-254 volume
         *   subpls r4, r4, #0x800000    @ >= 1.0: r4 = step - 2.0
         *   addpl r4, r4, #0x800000     @ >= 2.0: r4 = step - 1.0
         * The byte source (a DMA3 / CPU copy onto the stack below 0x400
         * bytes, else ROM) has the same content either way and is not
         * modelled. */
        uint32_t r4adj = r4step - 0x800000u;
        FastState s;
        if ((int32_t)r4adj < 0) {
            s = fast_loop_lt1(buf, r8, r3, r6, r7, r4adj, r11);
        } else {
            r11 <<= 1;
            r4adj -= 0x800000u;
            if ((int32_t)r4adj < 0)
                s = fast_loop_1to2(buf, r8, r3, r6, r7, r4adj, r11);
            else
                s = fast_loop_ge2(buf, r8, r3, r6, r7, r4adj + 0x800000u, r11);
        }
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
    for (;;) {
        uint32_t r9 = r7 * (uint32_t)r12;                /* mul r9, r7, r12 */
        r9 = (uint32_t)((int32_t)r9 >> 22);
        r9 = r9 + ((uint32_t)r6 << 1);                   /* adds */
        if (r9 != 0) {
            uint32_t *w = r5 - r8;                       /* [r5, -r8, lsl#2] */
            *w = mla(*w, r11, (int32_t)r9);
        }
        r7 = r7 + r4step;
        uint32_t adv = r7 >> 23;                         /* movs r9, r7, lsr#23 */
        if (adv != 0) {
            r2 -= (int32_t)adv;                          /* subs r2, r2, r9 */
            if (r2 <= 0) {                               /* ble C_unbuffered_mixing_loop_or_end */
                if (loopLen != 0) {
                    r3 -= loopLen;
                    r2 += loopLen;
                } else {
                    r4->status = 0;                      /* C_mixing_end_and_stop_channel */
                    return true;
                }
            }
            /* C_unbuffered_mixing_loop_continue */
            adv -= 1;
            if (adv == 0) {
                r6 = r12 + r6;
            } else {
                r3 += adv;
                r6 = r3[0];
            }
            r3 += 1;
            r12 = (int32_t)r3[0] - r6;
            r7 &= ~0x3F800000u;
        }
        /* C_unbuffered_mixing_skip_load */
        if (--r8 <= 0)
            break;
    }
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
        r10 += 2;
        r8 -= 2;
    } while (r8 > 0);
    /* C_downsampler_return */
    engine->hqNoiseShapeLeft = (uint8_t)(r4 >> 16);
    engine->hqNoiseShapeRight = (uint8_t)(r5 >> 16);
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
    for (int i = 0; i < TOTAL_PCM_CHANNELS; i++) {
        M4APCMChannel *r4 = &engine->pcmChannels[i];
        if (!(r4->status & CHN_ON))
            continue;
        if ((r4->status & (CHN_START | CHN_STOP)) == CHN_START)
            m4a_hq_channel_init(r4);
        /* The envelope state machine is vanilla's; the volumes are not. */
        m4a_pcm_channel_tick(r4, engine->masterVolume);
        if (!(r4->status & CHN_ON))
            continue;
        m4a_hq_channel_volumes(r4, engine->masterVolume);
        bool audible = ((i >= MAX_PCM_CHANNELS) == invert)
                    || (i < MAX_PCM_CHANNELS && r4->audition);
        if (!m4a_hq_mix_channel(engine, r4, audible ? engine->hqBuffer : muted))
            engine->hqUnportedChannelFrames++;
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
