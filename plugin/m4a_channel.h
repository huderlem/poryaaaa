#ifndef M4A_CHANNEL_H
#define M4A_CHANNEL_H

#include "m4a_engine.h"

/* PCM channel operations */
void m4a_pcm_channel_start(M4APCMChannel *ch, WaveData *wav, uint8_t type);
void m4a_pcm_channel_stop(M4APCMChannel *ch);
void m4a_pcm_channel_tick(M4APCMChannel *ch, uint8_t masterVolume);
void m4a_pcm_channel_render(M4APCMChannel *ch, int32_t *mixL, int32_t *mixR);

/* CGB channel operations */
void m4a_cgb_channel_start(M4ACGBChannel *ch);
void m4a_cgb_channel_stop(M4ACGBChannel *ch);
void m4a_cgb_channel_tick(M4ACGBChannel *ch, uint8_t c15);
void m4a_cgb_channel_render(M4ACGBChannel *ch, int32_t *mixL, int32_t *mixR,
                            float sampleRate);

/* Volume calculation for CGB channels (matches CgbModVol) */
void m4a_cgb_mod_vol(M4ACGBChannel *ch);

#endif /* M4A_CHANNEL_H */
