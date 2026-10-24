#ifndef M4A_CHANNEL_H
#define M4A_CHANNEL_H

#include "m4a_engine.h"

/* PCM channel operations */
void m4a_pcm_channel_start(M4APCMChannel *ch, WaveData *wav, uint8_t type);
void m4a_pcm_channel_stop(M4APCMChannel *ch);
void m4a_pcm_channel_tick(M4APCMChannel *ch, uint8_t masterVolume);
void m4a_pcm_channel_render(M4APCMChannel *ch, int32_t *mixL, int32_t *mixR);

/* CGB channel operations: the CgbSound register driver (m4a_channel.c) on
 * top of the PSG model (m4a_psg.c).  Writes are stamped at gba->now. */
void m4a_cgb_channel_start(M4ACGBChannel *ch);                  /* flag CHN_START */
/* `cursor` is the CPU cycle at which CgbSound reaches this channel; on return
 * it is the cycle at which it moves to the next one.  Register writes are
 * placed at the measured offsets inside that span (docs/ACCURACY_PLAN.md). */
void m4a_cgb_channel_tick(M4ACGBChannel *ch, uint8_t c15, M4APsg *psg, M4AGbaAudio *gba,
                          int64_t *cursor);
void m4a_cgb_osc_off(M4ACGBChannel *ch, M4APsg *psg, M4AGbaAudio *gba, int64_t *cursor); /* CgbOscOff */

/* Volume calculation for CGB channels (matches CgbModVol) */
void m4a_cgb_mod_vol(M4ACGBChannel *ch);

#endif /* M4A_CHANNEL_H */
