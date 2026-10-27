#include "m4a_mixer.h"
#include "m4a_channel.h"
#include "m4a_synth.h"
#include "m4a_tables.h"
#include <string.h>

/*
 * SoundMainRAM, translated from pokeemerald src/m4a_1.s (lines 88-709).
 *
 * The register names of the assembly are kept as variable names so the two
 * can be read side by side:
 *
 *   r5   the current 4-sample word of the frame buffer (A = right side);
 *        the B (left) word sits PCM_DMA_BUF_SIZE bytes further on
 *   r6   the A word being accumulated, r7 the B word
 *   r10  envelopeVolumeRight << 16, r11 envelopeVolumeLeft << 16
 *   r2   SoundChannel.count, r3 currentPointer, r9 fw, r8 the samples
 *        left in the frame (main paths) or the step (Unk1)
 *
 * Every mix step is `mul r1, r10, r0 / bic r1, r1, 0xFF0000 / add r6, r1,
 * r6, ror 8` (m4a_mixer_acc): the product's high byte lands in the word's
 * top byte with 8-bit wrap-around, and the ror 8 rotates the four byte
 * lanes so that after four samples the word holds them little-endian.
 * The assembly counts the samples of a word in the top two bits of r5
 * (`adds r5, r5, 0x40000000; bcc`); here `n` counts them.
 *
 * The host-pointer WaveData layout replaces the two GBA address
 * computations: `size + 2*wav + 32 - ptr` (reverse start) becomes
 * `data + size - (ptr - data)`, and `ptr - wav - 16` (compressed sample
 * index) becomes `ptr - data`.  Compressed channels keep their position as
 * a sample index in currentPointer, as the GBA does, and decode through one
 * engine-wide 64-sample buffer with the per-channel block tag `xpi`, so
 * two compressed channels interleaving the same block index share (and
 * clobber) that buffer exactly like sDecodingBuffer.
 */

#define BUF GBA_PCM_DMA_BUF_SIZE

static inline uint32_t ld32(const int8_t *p)
{
    return (uint32_t)(uint8_t)p[0] | ((uint32_t)(uint8_t)p[1] << 8)
         | ((uint32_t)(uint8_t)p[2] << 16) | ((uint32_t)(uint8_t)p[3] << 24);
}

static inline void st32(int8_t *p, uint32_t w)
{
    p[0] = (int8_t)(uint8_t)w;
    p[1] = (int8_t)(uint8_t)(w >> 8);
    p[2] = (int8_t)(uint8_t)(w >> 16);
    p[3] = (int8_t)(uint8_t)(w >> 24);
}

static inline uint32_t ror(uint32_t w, unsigned bits)
{
    bits &= 31;
    return bits ? (w >> bits) | (w << (32 - bits)) : w;
}

/* _081DD174 / _081DD4F4: the channel stops in the middle of a word.  n is
 * the number of samples already rotated into the word before the one just
 * mixed (r5 >> 30); the remaining rotation lines the lanes up. */
static inline void finish_word(int8_t *r5, uint32_t r6, uint32_t r7, int n)
{
    unsigned r0 = (unsigned)(3 - n) << 3;
    st32(r5 + BUF, ror(r7, r0));
    st32(r5, ror(r6, r0));
}

/* SoundMainRAM_Reverb: the previous contents of this frame's slot and of
 * the next one (both sides) feed back, scaled, into both sides. */
static void reverb_pass(M4AEngine *engine, int8_t *r5, int r4_counter, int r3_reverb)
{
    int8_t *r7 = (r4_counter == 2) ? engine->pcmBuffer : r5 + engine->pcmSamplesPerVBlank;
    int r4 = engine->pcmSamplesPerVBlank;
    do {
        int32_t r0 = r5[BUF];
        r0 += r5[0];
        r0 += r7[BUF];
        r0 += *r7++;
        int32_t r1 = r0 * r3_reverb;
        r0 = r1 >> 9;
        if (r0 & 0x80)
            r0 += 1;
        r5[BUF] = (int8_t)r0;
        *r5++ = (int8_t)r0;
    } while (--r4 > 0);
}

/* SoundMainRAM_Unk2: sample `r3` of a DPCM-compressed wave through the
 * shared decoding buffer.  Blocks past the loader's data (an index that
 * ran below 0 or beyond the extra block) decode from zero bytes, where the
 * GBA would read whatever memory follows the sample. */
static int32_t unk2(M4AEngine *engine, M4APCMChannel *r4, int32_t r3)
{
    uint32_t r0 = (uint32_t)r3 >> 6;
    if (r0 != r4->xpi) {
        r4->xpi = r0;
        uint8_t zero[0x21];
        const uint8_t *r2;
        /* The loader allocates (size / 64 + 1) blocks. */
        uint32_t blocks = r4->wav->size / M4A_DPCM_BLOCK_SAMPLES + 1;
        if (r0 < blocks) {
            r2 = (const uint8_t *)r4->wav->data + (size_t)r0 * 0x21;
        } else {
            memset(zero, 0, sizeof(zero));
            r2 = zero;
        }
        int8_t *r5 = engine->decodingBuffer;
        const int8_t *r6 = gDeltaEncodingTable;
        int r7 = 0x40;
        uint8_t lr = *r2++;
        *r5++ = (int8_t)lr;
        uint8_t r1 = *r2++;
        for (;;) {
            lr = (uint8_t)(lr + r6[r1 & 0xF]);
            *r5++ = (int8_t)lr;
            r7 -= 2;
            if (r7 <= 0)
                break;
            r1 = *r2++;
            lr = (uint8_t)(lr + r6[r1 >> 4]);
            *r5++ = (int8_t)lr;
        }
    }
    return engine->decodingBuffer[r3 & 0x3F];
}

/* ---- Golden Sun synth voices (ipatix's mixer; not in m4a_1.s) ----
 * The HQ mixer's synth oscillators (m4a_synth.h, measured against the HQ
 * test ROM: docs/HQ-MIXER-ACCURACY.md, step 9) run under vanilla's
 * accumulate: one generated value per output sample, mixed like a sample.
 * As in C_setup_synth, the pulse's duty LFO advances once per mixed frame
 * (here every frame: vanilla has no volume-zero skip); the saw's filter
 * state is mixed at the halved volume (floor(v / 2), the HQ mixer's
 * `mov r11, r11, lsr#1`), the pulse at +-64 and the triangle at +-128
 * unhalved.  Vanilla's ror-8 packing runs on every sample, so a zero saw
 * state is accumulated (as 0) rather than skipped. */
static void mix_synth(const M4AEngine *engine, M4APCMChannel *ch, int8_t *r5, int8_t *bufB,
                      int spv, uint32_t r10, uint32_t r11)
{
    uint32_t step8 = ((uint32_t)engine->pcmDivFreq * ch->frequency) << 3;
    uint32_t r7 = ch->fw;
    int32_t r2 = ch->count;
    uint32_t duty = 0;
    if (ch->synthType == M4A_SYNTH_PULSE) {
        duty = m4a_synth_pulse_setup(ch);
        r2 = ch->count;
    } else if (ch->synthType == M4A_SYNTH_SAW) {
        r10 = ((r10 >> 16) >> 1) << 16;
        r11 = ((r11 >> 16) >> 1) << 16;
    }
    for (int i = 0; i < spv; i += 4) {
        uint32_t a = ld32(r5), b = ld32(bufB);
        for (int k = 0; k < 4; k++) {
            int32_t value;
            if (ch->synthType == M4A_SYNTH_PULSE)
                value = m4a_synth_pulse_sample(&r7, duty, step8);
            else if (ch->synthType == M4A_SYNTH_SAW)
                value = m4a_synth_saw_sample(&r7, &r2, step8);
            else
                value = m4a_synth_triangle_sample(&r7, step8);
            a = m4a_mixer_acc(a, r10, value);
            b = m4a_mixer_acc(b, r11, value);
        }
        st32(r5, a);
        st32(bufB, b);
        r5 += 4;
        bufB += 4;
    }
    ch->fw = r7;
    ch->count = r2;
}

/* SoundMainRAM_Unk1: TONEDATA_TYPE_CMP / TONEDATA_TYPE_REV channels. */
static void unk1(M4AEngine *engine, M4APCMChannel *r4, int8_t *r5, int32_t *pr2,
                 int8_t **pr3, uint32_t *pr9, uint32_t r10, uint32_t r11,
                 int32_t loopLen /* [sp, 0x1C] */)
{
    WaveData *r6w = r4->wav;
    int8_t *data = r6w->data;
    int32_t r2 = *pr2;
    uint32_t r9 = *pr9;
    /* r3 is a pointer for uncompressed data and a sample index for
     * compressed data; keep both forms. */
    int8_t *p3 = *pr3;
    int32_t i3 = (int32_t)(p3 - data);

    if (!(r4->status & CHN_SPECIAL)) {
        r4->status |= CHN_SPECIAL;
        if (r4->type & VOICE_TYPE_REV) {
            p3 = data + r6w->size - (p3 - data);
            r4->currentPointer = p3;
        }
        if (r6w->type != 0) {
            i3 = (int32_t)(p3 - data);
            r4->currentPointer = data + i3; /* an index, as the GBA stores ptr - wav - 16 */
        }
    }
    i3 = (int32_t)(p3 - data);

    uint32_t r8;
    if (r4->type & VOICE_TYPE_FIX)
        r8 = 0x800000;
    else
        r8 = (uint32_t)engine->pcmDivFreq * r4->frequency;

    int32_t frameLeft = engine->pcmSamplesPerVBlank; /* [sp] */
    uint32_t r6 = 0, r7 = 0;
    int32_t r0, r1;
    uint32_t lr;
    int n = 0;

    if (r6w->type != 0) {
        r4->xpi = 0xFF000000u;
        if (!(r4->type & VOICE_TYPE_REV)) {
            /* compressed, forward */
            r0 = unk2(engine, r4, i3);
            i3 += 1;
            r1 = unk2(engine, r4, i3) - r0;
            for (;;) {                                   /* _081DD308 */
                r6 = ld32(r5);
                r7 = ld32(r5 + BUF);
                n = 0;
                for (;;) {                               /* _081DD310 */
                    int32_t s = r0 + ((int32_t)(r9 * (uint32_t)r1) >> 23);
                    r6 = m4a_mixer_acc(r6, r10, s);
                    r7 = m4a_mixer_acc(r7, r11, s);
                    r9 += r8;
                    lr = r9 >> 23;
                    if (lr != 0) {
                        r9 &= ~0x3F800000u;
                        r2 -= (int32_t)lr;
                        if (r2 <= 0) {                   /* _081DD398 */
                            if (loopLen == 0) {
                                goto stop_special;
                            }
                            i3 = (int32_t)r6w->loopStart;
                            lr = (uint32_t)(-r2);
                            for (;;) {                   /* _081DD3B0 */
                                r2 += loopLen;
                                if (r2 > 0) break;
                                lr -= (uint32_t)loopLen;
                            }
                            i3 += (int32_t)lr;           /* _081DD358 */
                            r0 = unk2(engine, r4, i3);
                        } else {
                            lr -= 1;
                            if (lr == 0) {
                                r0 += r1;
                                goto cmp_fwd_next;       /* _081DD364 */
                            }
                            i3 += (int32_t)lr;           /* _081DD358 */
                            r0 = unk2(engine, r4, i3);
                        }
                    cmp_fwd_next:                        /* _081DD364 */
                        i3 += 1;
                        r1 = unk2(engine, r4, i3) - r0;
                    }
                    if (++n < 4)                         /* _081DD370 */
                        continue;
                    st32(r5 + BUF, r7);
                    st32(r5, r6);
                    r5 += 4;
                    frameLeft -= 4;
                    break;
                }
                if (frameLeft <= 0)
                    break;
            }
            i3 -= 1;
            goto done_special;
        } else {
            /* compressed, reverse (_081DD3C0) */
            i3 -= 1;
            r0 = unk2(engine, r4, i3);
            i3 -= 1;
            r1 = unk2(engine, r4, i3) - r0;
            for (;;) {                                   /* _081DD3D8 */
                r6 = ld32(r5);
                r7 = ld32(r5 + BUF);
                n = 0;
                for (;;) {                               /* _081DD3E0 */
                    int32_t s = r0 + ((int32_t)(r9 * (uint32_t)r1) >> 23);
                    r6 = m4a_mixer_acc(r6, r10, s);
                    r7 = m4a_mixer_acc(r7, r11, s);
                    r9 += r8;
                    lr = r9 >> 23;
                    if (lr != 0) {
                        r9 &= ~0x3F800000u;
                        r2 -= (int32_t)lr;
                        if (r2 <= 0)
                            goto stop_special;           /* _081DD4F4 */
                        lr -= 1;
                        if (lr == 0) {
                            r0 += r1;
                        } else {
                            i3 -= (int32_t)lr;           /* _081DD428 */
                            r0 = unk2(engine, r4, i3);
                        }
                        i3 -= 1;                         /* _081DD434 */
                        r1 = unk2(engine, r4, i3) - r0;
                    }
                    if (++n < 4)                         /* _081DD440 */
                        continue;
                    st32(r5 + BUF, r7);
                    st32(r5, r6);
                    r5 += 4;
                    frameLeft -= 4;
                    break;
                }
                if (frameLeft <= 0)
                    break;
            }
            i3 += 2;
            goto done_special;
        }
    } else {
        /* _081DD468: uncompressed.  Only the reverse case is mixed; a CMP
         * voice on uncompressed data returns without mixing (and without
         * advancing), as the assembly does. */
        if (!(r4->type & VOICE_TYPE_REV))
            goto done_special;
        /* The look-behind of the first sample reads the byte before the
         * data: the last header byte (size >> 24, i.e. 0) on the GBA. */
#define REV_RD(idx) ((idx) < 0 ? 0 : (int32_t)data[(idx)])
        i3 -= 1;
        r0 = REV_RD(i3);
        r1 = REV_RD(i3 - 1) - r0;
        for (;;) {                                       /* _081DD480 */
            r6 = ld32(r5);
            r7 = ld32(r5 + BUF);
            n = 0;
            for (;;) {                                   /* _081DD488 */
                int32_t s = r0 + ((int32_t)(r9 * (uint32_t)r1) >> 23);
                r6 = m4a_mixer_acc(r6, r10, s);
                r7 = m4a_mixer_acc(r7, r11, s);
                r9 += r8;
                lr = r9 >> 23;
                if (lr != 0) {
                    r9 &= ~0x3F800000u;
                    r2 -= (int32_t)lr;
                    if (r2 <= 0)
                        goto stop_special;               /* _081DD4F4 */
                    i3 -= (int32_t)lr;
                    r0 = REV_RD(i3);
                    r1 = REV_RD(i3 - 1) - r0;
                }
                if (++n < 4)                             /* _081DD4CC */
                    continue;
                st32(r5 + BUF, r7);
                st32(r5, r6);
                r5 += 4;
                frameLeft -= 4;
                break;
            }
            if (frameLeft <= 0)
                break;
        }
        i3 += 1;
#undef REV_RD
        goto done_special;
    }

stop_special:                                            /* _081DD4F4: mov r2, 0 */
    r2 = 0;
    r4->status = 0;
    finish_word(r5, r6, r7, n);
done_special:                                            /* _081DD4F0 */
    *pr2 = r2;
    *pr3 = data + i3;
    *pr9 = r9;
}

void m4a_mixer_mix_channel(M4AEngine *engine, M4APCMChannel *r4, int8_t *frame)
{
    int8_t *r5 = frame;
    uint32_t r10 = (uint32_t)r4->envelopeVolumeRight;
    uint32_t r11 = (uint32_t)r4->envelopeVolumeLeft;
    /* [sp, 0x10] / [sp, 0xC]: loop length and loop start, or 0. */
    int32_t loopLen = 0;
    int8_t *loopStart = NULL;
    if (r4->status & CHN_LOOP) {
        loopStart = r4->wav->data + r4->wav->loopStart;
        loopLen = (int32_t)(r4->wav->size - r4->wav->loopStart);
    }
    int32_t r2 = r4->count;
    int8_t *r3 = r4->currentPointer;
    uint32_t r9 = r4->fw;
    int32_t r8 = engine->pcmSamplesPerVBlank;

    r10 <<= 16;
    r11 <<= 16;

    if (r4->synthType != M4A_SYNTH_NONE) {
        mix_synth(engine, r4, r5, r5 + BUF, r8, r10, r11);
        return;
    }

    if (r4->type & (VOICE_TYPE_CMP | VOICE_TYPE_REV)) {
        unk1(engine, r4, r5, &r2, &r3, &r9, r10, r11, loopLen);
        r4->fw = r9;                                     /* _081DD228 */
        r4->count = r2;                                  /* _081DD22C */
        r4->currentPointer = r3;
        return;
    }

    uint32_t r6 = 0, r7 = 0;
    int32_t r0, r1;
    int n = 0;
    uint32_t lr;

    if (r4->type & VOICE_TYPE_FIX) {
        /* Fixed frequency: one source sample per output sample. */
        int32_t r9f = 0;
        for (;;) {                                       /* _081DD07C */
            if (r2 > 4) {
                r2 -= r8;
                if (r2 > 0) {
                    r9f = 0;
                } else {
                    r9f = r8;
                    r2 += r8;
                    r8 = r2 - 4;
                    r9f -= r8;
                    r2 &= 3;
                    if (r2 == 0)
                        r2 = 4;
                }
                do {                                     /* _081DD0A8 */
                    r6 = ld32(r5);
                    r7 = ld32(r5 + BUF);
                    for (int k = 0; k < 4; k++) {        /* _081DD0B0 */
                        r0 = *r3++;
                        r6 = m4a_mixer_acc(r6, r10, r0);
                        r7 = m4a_mixer_acc(r7, r11, r0);
                    }
                    st32(r5 + BUF, r7);
                    st32(r5, r6);
                    r5 += 4;
                    r8 -= 4;
                } while (r8 > 0);
                r8 += r9f;
                if (r8 == 0)
                    goto store_count_ptr;                /* _081DD22C */
            }
            r6 = ld32(r5);                               /* _081DD0EC */
            r7 = ld32(r5 + BUF);
            n = 0;
            for (;;) {                                   /* _081DD0F4 */
                r0 = *r3++;
                r6 = m4a_mixer_acc(r6, r10, r0);
                r7 = m4a_mixer_acc(r7, r11, r0);
                r2 -= 1;
                if (r2 == 0) {                           /* _081DD164 */
                    r2 = loopLen;
                    if (r2 == 0)
                        goto stop_channel;               /* _081DD174 */
                    r3 = loopStart;
                }
                if (++n < 4)                             /* _081DD118 */
                    continue;
                st32(r5 + BUF, r7);
                st32(r5, r6);
                r5 += 4;
                r8 -= 4;
                break;
            }
            if (r8 <= 0)
                goto store_count_ptr;                    /* _081DD22C */
        }
    }

    /* _081DD19C: the interpolating loop. */
    {
        uint32_t r4step = (uint32_t)engine->pcmDivFreq * r4->frequency;
        r0 = r3[0];
        r3 += 1;
        r1 = r3[0] - r0;
        for (;;) {                                       /* _081DD1B4 */
            r6 = ld32(r5);
            r7 = ld32(r5 + BUF);
            n = 0;
            for (;;) {                                   /* _081DD1BC */
                int32_t s = r0 + ((int32_t)(r9 * (uint32_t)r1) >> 23);
                r6 = m4a_mixer_acc(r6, r10, s);
                r7 = m4a_mixer_acc(r7, r11, s);
                r9 += r4step;
                lr = r9 >> 23;
                if (lr != 0) {
                    r9 &= ~0x3F800000u;
                    r2 -= (int32_t)lr;
                    if (r2 <= 0) {                       /* _081DD134 */
                        if (loopLen == 0) {
                            r2 = 0;                      /* _081DD158 */
                            goto stop_channel;
                        }
                        r3 = loopStart;
                        lr = (uint32_t)(-r2);
                        for (;;) {                       /* _081DD148 */
                            r2 += loopLen;
                            if (r2 > 0) break;
                            lr -= (uint32_t)loopLen;
                        }
                        r3 += (int32_t)lr;               /* _081DD1FC (ne) */
                        r0 = r3[0];
                    } else {
                        lr -= 1;
                        if (lr == 0) {
                            r0 += r1;
                        } else {
                            r3 += (int32_t)lr;           /* _081DD1FC */
                            r0 = r3[0];
                        }
                    }
                    r3 += 1;
                    r1 = r3[0] - r0;
                }
                if (++n < 4)                             /* _081DD208 */
                    continue;
                st32(r5 + BUF, r7);
                st32(r5, r6);
                r5 += 4;
                r8 -= 4;
                break;
            }
            if (r8 <= 0)
                break;
        }
        r3 -= 1;
        r4->fw = r9;                                     /* _081DD228 */
        goto store_count_ptr;
    }

stop_channel:                                            /* _081DD174 */
    r4->status = 0;
    {
        unsigned rot = (unsigned)(3 - n) << 3;
        st32(r5 + BUF, ror(r7, rot));
        st32(r5, ror(r6, rot));
    }
    return;                                              /* _081DD234: nothing stored */

store_count_ptr:                                         /* _081DD22C */
    r4->count = r2;
    r4->currentPointer = r3;
}

void m4a_sound_main_ram(M4AEngine *engine, int frameOffset, int pcmDmaCounter)
{
    int8_t *r5 = engine->pcmBuffer + frameOffset;
    int spv = engine->pcmSamplesPerVBlank;

    if (engine->reverb.amount != 0) {
        reverb_pass(engine, r5, pcmDmaCounter, engine->reverb.amount);
    } else {
        memset(r5, 0, (size_t)spv);                      /* SoundMainRAM_NoReverb */
        memset(r5 + BUF, 0, (size_t)spv);
    }

    /* SoundMainRAM_ChanLoop over the real pool, then the shadow pool
     * (polyphony debug), which mixes into a discarded frame unless the
     * invert mode makes it the audible one. */
    int8_t *muted = engine->mutedFrame;
    bool invert = engine->polyDebugInvert;
    for (int i = 0; i < TOTAL_PCM_CHANNELS; i++) {
        M4APCMChannel *r4 = &engine->pcmChannels[i];
        if (!(r4->status & CHN_ON))
            continue;
        /* The envelope step (the SF_START / IEC / STOP / DECAY / ATTACK
         * block of SoundMainRAM, _081DCF6A.._081DD006 and the volume
         * products that follow). */
        m4a_pcm_channel_tick(r4, engine->masterVolume);
        if (!(r4->status & CHN_ON))
            continue;
        bool audible = ((i >= MAX_PCM_CHANNELS) == invert)
                    || (i < MAX_PCM_CHANNELS && r4->audition);
        if (audible) {
            m4a_mixer_mix_channel(engine, r4, r5);
        } else {
            memset(muted, 0, (size_t)spv);
            memset(muted + BUF, 0, (size_t)spv);
            m4a_mixer_mix_channel(engine, r4, muted);
        }
    }
}
