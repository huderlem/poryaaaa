#ifndef M4A_MIXER_H
#define M4A_MIXER_H

#include "m4a_engine.h"

#ifdef __cplusplus
extern "C" {
#endif

/*
 * SoundMainRAM (pokeemerald src/m4a_1.s lines 88-709), translated: the
 * reverb pass over the DMA ring, then for every DirectSound channel the
 * envelope step and the mix of one VBlank frame (pcmSamplesPerVBlank
 * samples) into the ring slot `frameOffset` with the 8-bit wrapping ror-8
 * sum, the fixed-frequency and interpolating loops, and the Unk1/Unk2 paths
 * for reversed (TONEDATA_TYPE_REV) and DPCM-compressed samples.
 *
 * `frameOffset` is SoundMain's r5 relative to pcmBuffer (the slot
 * pcmDmaCounter selects); `pcmDmaCounter` picks the reverb's second tap.
 */
void m4a_sound_main_ram(M4AEngine *engine, int frameOffset, int pcmDmaCounter);

/* The mix of a single channel into one frame of a buffer laid out like the
 * DMA ring: `frame` holds the right (A) bytes, `frame + pcmDmaBufSize` (PCM_DMA_BUF_SIZE)
 * the left (B) bytes.  The envelope step is not run here. */
void m4a_mixer_mix_channel(M4AEngine *engine, M4APCMChannel *ch, int8_t *frame);

/* A range of the slot, [start, start + count), for live mode
 * (m4a_engine_set_live_mode): the reverb pass over that range, then every
 * channel's mix of `count` samples (a multiple of 4, the mixer's word).
 * `envelopeStep` says which channels get the envelope step first:
 * M4A_ENVELOPE_STEP_ALL is SoundMainRAM's (once per frame, count may be 0
 * to step without mixing); M4A_ENVELOPE_STEP_STARTING steps only a channel
 * still CHN_START (keyed since the frame's step), so it joins this piece;
 * M4A_ENVELOPE_STEP_NONE steps nobody.  A channel left CHN_START (a
 * STARTING step never ran for it) is not mixed. */
#define M4A_ENVELOPE_STEP_NONE     0
#define M4A_ENVELOPE_STEP_ALL      1
#define M4A_ENVELOPE_STEP_STARTING 2
void m4a_sound_main_ram_range(M4AEngine *engine, int frameOffset, int pcmDmaCounter, int start,
                              int count, int envelopeStep);
/* m4a_mixer_mix_channel over `count` samples (a multiple of 4) in place of
 * pcmSamplesPerVBlank; the channel's state carries into the next call.
 * `frameStart`: this call begins the frame (the once-per-frame work, the
 * pulse synth's duty LFO step, runs); false for a later piece of it. */
void m4a_mixer_mix_channel_n(M4AEngine *engine, M4APCMChannel *ch, int8_t *frame, int count,
                             bool frameStart);

/* The packed-word accumulate of one sample at one lane volume, exactly the
 * `mul r1, r10, r0 / bic r1, r1, 0xFF0000 / add r6, r1, r6, ror 8`
 * sequence: `acc` is the word so far, `vol16` the envelope volume << 16,
 * `sample` a sign-extended 8-bit (or interpolated) sample. */
static inline uint32_t m4a_mixer_acc(uint32_t acc, uint32_t vol16, int32_t sample)
{
    uint32_t r1 = vol16 * (uint32_t)sample;
    r1 &= ~0xFF0000u;
    return r1 + ((acc >> 8) | (acc << 24));
}

#ifdef __cplusplus
}
#endif

#endif /* M4A_MIXER_H */
