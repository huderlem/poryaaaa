#include "m4a_reverb.h"
#include <stdlib.h>
#include <string.h>

/*
 * GBA reverb implementation.
 *
 * The GBA's reverb works on the PCM DMA buffer (1584 bytes per channel).
 * The algorithm (from SoundMainRAM_Reverb in m4a_1.s):
 *   For each sample position:
 *     r0 = pcmBuffer[pos + bufSize]  (right channel, previous frame)
 *     r0 += pcmBuffer[pos]           (left channel, previous frame)
 *     r0 += nextBuf[pos + bufSize]   (right channel, next frame area)
 *     r0 += nextBuf[pos]             (left channel, next frame area)
 *     r0 = (r0 * reverbAmount) >> 9  (with rounding)
 *     Write r0 to both channels
 *
 * This is effectively a 4-tap feedback delay where the delay length equals
 * the PCM buffer size (1584 samples at the GBA's sample rate).
 *
 * For the plugin, we scale the delay length proportionally to the DAW sample rate:
 *   delayLen = 1584 * (daw_rate / gba_rate)
 * where gba_rate defaults to 13379 Hz (SOUND_MODE_FREQ_13379, the most common).
 */

#define GBA_PCM_BUF_SIZE 1584
#define GBA_SAMPLE_RATE  13379.0f

void m4a_reverb_init(M4AReverb *reverb, float sampleRate, uint8_t amount)
{
    /* Scale delay buffer to match DAW sample rate */
    int delayLen = (int)(GBA_PCM_BUF_SIZE * sampleRate / GBA_SAMPLE_RATE);
    if (delayLen < 1) delayLen = 1;

    reverb->bufferSize = delayLen;
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
 * The GBA algorithm averages 4 taps and multiplies by reverb amount.
 * We simplify: read delayed sample, mix with input, write back.
 */
void m4a_reverb_process(M4AReverb *reverb, int32_t *sampleL, int32_t *sampleR)
{
    if (!reverb->buffer || reverb->amount == 0)
        return;

    int pos = reverb->pos;
    int idx = pos * 2;

    /* Read delayed samples */
    int32_t delayedL = reverb->buffer[idx];
    int32_t delayedR = reverb->buffer[idx + 1];

    /* Mix: apply reverb feedback
     * Matching GBA: (sum_of_4_taps * reverbAmount) >> 9
     * Since we only have 2 taps (L+R) in our simplified model,
     * we use: delayed * reverbAmount >> 7 as a close approximation */
    int32_t wetL = (delayedL * reverb->amount) >> 7;
    int32_t wetR = (delayedR * reverb->amount) >> 7;

    *sampleL += wetL;
    *sampleR += wetR;

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
