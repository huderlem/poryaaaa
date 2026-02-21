#ifndef M4A_REVERB_H
#define M4A_REVERB_H

#include <stdint.h>

typedef struct {
    int8_t *buffer;     /* stereo interleaved: L,R,L,R,... */
    int bufferSize;     /* total buffer size in samples (per channel) */
    int pos;
    uint8_t amount;     /* 0-127 */
} M4AReverb;

void m4a_reverb_init(M4AReverb *reverb, float sampleRate, uint8_t amount);
void m4a_reverb_destroy(M4AReverb *reverb);
void m4a_reverb_reset(M4AReverb *reverb);
void m4a_reverb_set_amount(M4AReverb *reverb, uint8_t amount);
void m4a_reverb_process(M4AReverb *reverb, int32_t *sampleL, int32_t *sampleR);

#endif /* M4A_REVERB_H */
