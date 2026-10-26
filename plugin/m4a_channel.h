#ifndef M4A_CHANNEL_H
#define M4A_CHANNEL_H

#include "m4a_engine.h"

/* PCM channel operations */
void m4a_pcm_channel_start(M4APCMChannel *ch, WaveData *wav, uint8_t type);
/* ply_note with track->unk_3C = startOffset (XCMD 0x0D): the channel starts
 * `startOffset` samples into the wave. */
void m4a_pcm_channel_start_at(M4APCMChannel *ch, WaveData *wav, uint8_t type,
                              uint32_t startOffset);
void m4a_pcm_channel_stop(M4APCMChannel *ch);
void m4a_pcm_channel_tick(M4APCMChannel *ch, uint8_t masterVolume);

/* CGB channel operations: the CgbSound register driver (m4a_channel.c) on
 * top of the PSG model (m4a_psg.c).  Writes are stamped at gba->now. */
void m4a_cgb_channel_start(M4ACGBChannel *ch);                  /* flag CHN_START */
/* `cursor` is the CPU cycle at which CgbSound reaches this channel; on return
 * it is the cycle at which it moves to the next one.  Register writes are
 * placed at the measured offsets inside that span (docs/ACCURACY.md, appendix A). */
void m4a_cgb_channel_tick(M4ACGBChannel *ch, uint8_t c15, M4APsg *psg, M4AGbaAudio *gba,
                          int64_t *cursor);
void m4a_cgb_osc_off(M4ACGBChannel *ch, M4APsg *psg, M4AGbaAudio *gba, int64_t *cursor); /* CgbOscOff */

/* Volume calculation for CGB channels (matches CgbModVol) */
void m4a_cgb_mod_vol(M4ACGBChannel *ch);

#endif /* M4A_CHANNEL_H */
