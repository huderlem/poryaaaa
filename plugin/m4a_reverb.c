#include "m4a_reverb.h"
#include <stdlib.h>
#include <string.h>

/*
 * GBA reverb implementation.
 *
 * The GBA's reverb works on the PCM DMA buffer (1584 bytes per channel).
 * The algorithm (from SoundMainRAM_Reverb in m4a_1.s):
 *   For each sample position:
 *     sum  = pcmBuffer_L[pos]           (left channel, current frame)
 *     sum += pcmBuffer_R[pos]           (right channel, current frame)
 *     sum += pcmBuffer_L[otherPos]      (left channel, other frame area)
 *     sum += pcmBuffer_R[otherPos]      (right channel, other frame area)
 *     result = (sum * reverbAmount) >> 9  (with rounding toward zero)
 *     Write result to BOTH L and R channels (mono reverb)
 *
 * The 4-tap sum with >>9 means partial cancellation between taps dampens
 * the reverb compared to a naive per-channel approach. The mono write
 * prevents stereo differences from accumulating across feedback iterations.
 *
 * For the plugin, we scale the delay length proportionally to the DAW sample rate:
 *   delayLen = 1584 * (daw_rate / gba_rate)
 * and the "other" tap offset equals one VBlank frame:
 *   frameSize = delayLen / pcmDmaPeriod
 */

#define GBA_PCM_BUF_SIZE    1584
#define GBA_SAMPLE_RATE     13379.0f
#define GBA_PCM_DMA_PERIOD  7       /* 1584 / 224 for 13379 Hz */

void m4a_reverb_init(M4AReverb *reverb, float sampleRate, uint8_t amount)
{
    /* Scale delay buffer to match DAW sample rate */
    int delayLen = (int)(GBA_PCM_BUF_SIZE * sampleRate / GBA_SAMPLE_RATE);
    if (delayLen < 1) delayLen = 1;

    reverb->bufferSize = delayLen;
    reverb->frameSize = delayLen / GBA_PCM_DMA_PERIOD;
    if (reverb->frameSize < 1) reverb->frameSize = 1;
    reverb->buffer = (int8_t *)calloc(delayLen * 2, sizeof(int8_t)); /* stereo */
    reverb->pos = 0;
    reverb->amount = amount;
}

void m4a_reverb_destroy(M4AReverb *reverb)
{
    free(reverb->buffer);
    reverb->buffer = NULL;
    reverb->bufferSize = 0;
}

void m4a_reverb_reset(M4AReverb *reverb)
{
    if (reverb->buffer)
        memset(reverb->buffer, 0, reverb->bufferSize * 2 * sizeof(int8_t));
    reverb->pos = 0;
}

void m4a_reverb_set_amount(M4AReverb *reverb, uint8_t amount)
{
    reverb->amount = amount;
}

/*
 * Process reverb for one stereo sample pair.
 *
 * Matches the GBA's 4-tap reverb algorithm:
 *   - Read L and R from current delay position
 *   - Read L and R from "other" position (one frame offset)
 *   - Sum all 4 taps, multiply by reverb amount, >>9
 *   - Add the same mono reverb value to both output channels
 *   - Write combined output back to delay buffer
 */
void m4a_reverb_process(M4AReverb *reverb, int32_t *sampleL, int32_t *sampleR)
{
    if (!reverb->buffer || reverb->amount == 0)
        return;

    int pos = reverb->pos;
    int idx = pos * 2;

    /* "Other" position: one frame ahead in the circular buffer */
    int otherPos = (pos + reverb->frameSize) % reverb->bufferSize;
    int otherIdx = otherPos * 2;

    /* Read 4 taps (matching GBA's SoundMainRAM_Reverb) */
    int32_t sum = reverb->buffer[idx]           /* L at current pos */
                + reverb->buffer[idx + 1]       /* R at current pos */
                + reverb->buffer[otherIdx]      /* L at other pos */
                + reverb->buffer[otherIdx + 1]; /* R at other pos */

    /* Apply reverb scaling: (sum * amount) >> 9
     * Use arithmetic shift with rounding toward zero (matching GBA) */
    int32_t wet = (sum * reverb->amount) >> 9;

    /* GBA writes the same mono reverb value to both channels */
    *sampleL += wet;
    *sampleR += wet;

    /* Clamp to int8_t range and write back to delay buffer */
    int32_t writeL = *sampleL;
    int32_t writeR = *sampleR;
    if (writeL > 127) writeL = 127;
    if (writeL < -128) writeL = -128;
    if (writeR > 127) writeR = 127;
    if (writeR < -128) writeR = -128;

    reverb->buffer[idx] = (int8_t)writeL;
    reverb->buffer[idx + 1] = (int8_t)writeR;

    reverb->pos++;
    if (reverb->pos >= reverb->bufferSize)
        reverb->pos = 0;
}
