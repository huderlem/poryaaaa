#ifndef M4A_HQ_MIXER_H
#define M4A_HQ_MIXER_H

#include "m4a_engine.h"

#ifdef __cplusplus
extern "C" {
#endif

/*
 * SoundMainRAM of ipatix's HQ-Mixer rev 4.0 as the pret wiki installs it
 * (test/accuracy/rom/hq/m4a_1.s, the Kurausukun newmix file; the port
 * specification is docs/HQ-MIXER-ACCURACY.md, section 2).  Engine->hqMixer
 * selects it in place of m4a_sound_main_ram.
 *
 * Per channel the envelope step and the mix of one VBlank frame into the
 * 32-bit mix buffer (int16 left in the high half, int16 right in the low
 * half, one word per output sample) that already holds this frame's reverb
 * seed; then the downsampler converts the buffer to the int8 DMA ring slot
 * `frameOffset` with first-order noise shaping and saturation, reading the
 * ring taps for the next frame's seed as it goes.
 *
 * Ported so far (steps 4-8 of the plan): every sample path (the fast
 * loop's three blocks, the unbuffered loop, the FIX path, the pre-cached
 * compressed and reversed paths with the stack budget), the volume-zero
 * skip and the downsampler.  A synth channel (step 9) is left untouched
 * for the frame and counted in engine->hqUnportedChannelFrames.
 */
void m4a_hq_sound_main_ram(M4AEngine *engine, int frameOffset, int pcmDmaCounter);

/* The SF_START part of C_channel_state_loop that m4a_pcm_channel_start_at
 * does not do: the compressed flag from the sample, the reverse pointer,
 * sampleStor.  Run before the channel's first envelope step. */
void m4a_hq_channel_init(M4APCMChannel *ch);

/* C_channel_vol_calc: the HQ per-side volumes (0..127) from the envelope. */
void m4a_hq_channel_volumes(M4APCMChannel *ch, uint8_t masterVolume);

/* C_mixing_setup .. C_mixing_end_store for one channel into `buf`
 * (engine->pcmSamplesPerVBlank words).  Returns false when the channel's
 * path is not ported yet (nothing touched). */
bool m4a_hq_mix_channel(M4AEngine *engine, M4APCMChannel *ch, uint32_t *buf);

/* C_downsampler over engine->hqBuffer into the ring slot, with the
 * noise-shape state in engine->hqNoiseShapeLeft/Right and the next
 * frame's reverb seed written back into the buffer. */
void m4a_hq_downsample(M4AEngine *engine, int frameOffset, int pcmDmaCounter);

#ifdef __cplusplus
}
#endif

#endif /* M4A_HQ_MIXER_H */
