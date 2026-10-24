#ifndef M4A_REVERB_H
#define M4A_REVERB_H

#include <stdint.h>

/* SoundInfo.reverb: the amount SoundMainRAM_Reverb (m4a_mixer.c) feeds the
 * previous contents of the DMA ring back with.  0 = off (the ring slot is
 * zero-filled instead). */
typedef struct {
    uint8_t amount;     /* 0-127 */
} M4AReverb;

static inline void m4a_reverb_set_amount(M4AReverb *reverb, uint8_t amount)
{
    reverb->amount = amount;
}

#endif /* M4A_REVERB_H */
