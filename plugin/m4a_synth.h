#ifndef M4A_SYNTH_H
#define M4A_SYNTH_H

#include "m4a_engine.h"

#ifdef __cplusplus
extern "C" {
#endif

/*
 * The synth oscillators of ipatix's HQ mixer (C_setup_synth ..
 * C_synth_triangle_loop in test/accuracy/rom/hq/m4a_1.s; the Golden Sun
 * instruments: a zero-size DirectSound sample whose data bytes are the
 * descriptor, data[1] the waveform, data[2..5] the pulse's duty, LFO step,
 * modulation amount and LFO offset).  Shared by the HQ port
 * (m4a_hq_mixer.c, measured against the HQ test ROM: docs/HQ-MIXER-ACCURACY.md,
 * step 9) and the vanilla-mode extension (m4a_mixer.c mix_synth: the same
 * oscillator with vanilla's accumulate; not a thing vanilla m4a_1.s has).
 *
 * A synth channel reuses fw as the 32-bit oscillator phase (r7: one period
 * = 2^32, advanced by (step << 3) per output sample, so the header's
 * frequency 0x01058920 puts a 64-sample period at middle C) and count as
 * the pulse LFO accumulator / the saw's filter state (r2).  The registers
 * are kept as names.
 */

/* C_setup_synth, the pulse part, once per MIXED frame (a frame the
 * volume-zero skip drops does not advance the LFO):
 *
 *   ldrb r6, [r3, #SYNTH_WIDTH_CHANGE_1]   @ data[3]
 *   add r2, r2, r6, lsl#24                 @ count += step << 24
 *   ldrb r6, [r3, #SYNTH_WIDTH_CHANGE_2]   @ data[5]
 *   adds r6, r2, r6, lsl#24                @ t = count + offset << 24
 *   mvnmi r6, r6                           @ folded: t < 0 -> ~t
 *   mov r10, r6, lsr#8
 *   ldrb r1, [r3, #SYNTH_MOD_AMOUNT]       @ data[4]
 *   ldrb r0, [r3, #SYNTH_BASE_WAVE_DUTY]   @ data[2]
 *   mov r0, r0, lsl#24
 *   mla r6, r10, r1, r0                    @ duty = (t >> 8) * mod + base << 24
 *
 * Stores the new count and the threshold (synthPulseDuty), returns it. */
static inline uint32_t m4a_synth_pulse_setup(M4APCMChannel *ch)
{
    const uint8_t *r3 = (const uint8_t *)ch->wav->data;
    uint32_t r2 = (uint32_t)ch->count + ((uint32_t)r3[3] << 24);
    ch->count = (int32_t)r2;
    uint32_t r6 = r2 + ((uint32_t)r3[5] << 24);
    if ((int32_t)r6 < 0)
        r6 = ~r6;
    r6 = (r6 >> 8) * r3[4] + ((uint32_t)r3[2] << 24);
    ch->synthPulseDuty = r6;
    return r6;
}

/* C_synth_pulse_loop, one sample: +64 while the phase is below the
 * threshold (cmp r7, r6: unsigned), -64 from it on, compared BEFORE the
 * advance (adds r7, r7, r4, lsl#3). */
static inline int32_t m4a_synth_pulse_sample(uint32_t *r7, uint32_t r6, uint32_t step8)
{
    int32_t v = (*r7 < r6) ? 64 : -64;
    *r7 += step8;
    return v;
}

/* C_synth_saw_loop, one sample: the advance first, then
 *
 *   rsb r9, r12, r7, lsr#24      @ raw = (phase >> 24) - 0x70
 *   mov r6, r7, lsl#1
 *   sub r9, r9, r6, lsr#27       @ raw -= (phase << 1) >> 27
 *   adds r2, r9, r2, asr#1       @ state = raw + (state >> 1)
 *
 * The sample is the filter state itself (mixed at the halved volume; the
 * mixer skips it when it is 0).  Not a true saw: a jump in the middle. */
static inline int32_t m4a_synth_saw_sample(uint32_t *r7, int32_t *r2, uint32_t step8)
{
    *r7 += step8;
    int32_t r9 = (int32_t)(*r7 >> 24) - 0x70;
    r9 -= (int32_t)((*r7 << 1) >> 27);
    *r2 = r9 + (*r2 >> 1);
    return *r2;
}

/* C_synth_triangle_loop, one sample: the advance first, the sign of the
 * advanced phase picks the slope:
 *
 *   adds r7, r7, r4, lsl#3
 *   rsbpl r9, r6, r7, asr#23     @ phase >= 0: (phase >> 23) - 0x80
 *   submi r9, r12, r7, lsr#23    @ phase <  0: 0x180 - (phase >> 23)
 *
 * -128..127, exactly a full-scale sample. */
static inline int32_t m4a_synth_triangle_sample(uint32_t *r7, uint32_t step8)
{
    *r7 += step8;
    if ((int32_t)*r7 >= 0)
        return (int32_t)(*r7 >> 23) - 0x80;
    return 0x180 - (int32_t)(*r7 >> 23);
}

#ifdef __cplusplus
}
#endif

#endif /* M4A_SYNTH_H */
