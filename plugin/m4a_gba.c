#include "m4a_gba.h"
#include <stdlib.h>
#include <string.h>

#define BLIP_SIZE 0x4000

void m4a_gba_init(M4AGbaAudio *g, double hostRate, const int16_t *ring, int ringSize,
                  int pcmSamplesPerVBlank)
{
    memset(g, 0, sizeof(*g));
    g->hostRate = hostRate;
    g->left = m4a_blip_new(BLIP_SIZE);
    g->right = m4a_blip_new(BLIP_SIZE);
    m4a_blip_set_rates(g->left, GBA_CPU_HZ, hostRate);
    m4a_blip_set_rates(g->right, GBA_CPU_HZ, hostRate);

    g->resolution = 0;
    g->sampleInterval = 512;
    g->nextSampleEvent = 0;
    g->soundbias = 0x200;
    g->masterVolume = 0x100;

    /* SOUNDCNT_H as SoundInit writes it: SOUND_ALL_MIX_FULL (volume 2, both
     * DirectSound channels at 100%), A right, B left, both on timer 0. */
    g->psgShift = 4 - 2;
    g->chARight = true;
    g->chBLeft = true;
    g->volumeChA = g->volumeChB = true;

    g->ring = ring;
    g->ringSize = ringSize;
    g->timerPeriodNum = GBA_CYCLES_PER_FRAME;
    g->timerPeriodDen = pcmSamplesPerVBlank > 0 ? pcmSamplesPerVBlank : 224;
}

void m4a_gba_destroy(M4AGbaAudio *g)
{
    m4a_blip_delete(g->left);
    m4a_blip_delete(g->right);
    g->left = g->right = NULL;
}

void m4a_gba_set_pcm_rate(M4AGbaAudio *g, int pcmSamplesPerVBlank, const int16_t *ring, int ringSize)
{
    g->timerPeriodDen = pcmSamplesPerVBlank > 0 ? pcmSamplesPerVBlank : 224;
    g->timerRem = 0;
    g->ring = ring;
    g->ringSize = ringSize;
    g->fifo[0].srcPos = g->fifo[1].srcPos = 0;
}

void m4a_gba_write_soundbias_resolution(M4AGbaAudio *g, int resolution)
{
    /* GBAAudioWriteSOUNDBIAS: catch up, then re-derive the slot index of the
     * current window for the new interval. */
    m4a_gba_catch_up(g, g->now);
    int old = g->sampleInterval;
    g->resolution = resolution & 3;
    g->sampleInterval = 0x200 >> g->resolution;
    if (old != g->sampleInterval) {
        int64_t t = g->now - g->lastSample;
        g->sampleIndex = (int)(t >> (9 - g->resolution));
        if (g->sampleIndex < 0 || g->sampleIndex >= GBA_MAX_SAMPLES)
            g->sampleIndex = 0;
    }
}

void m4a_gba_fifo_reset(M4AGbaAudio *g)
{
    for (int i = 0; i < 2; i++) {
        g->fifo[i].fifoWrite = 0;
        g->fifo[i].fifoRead = 0;
    }
}

void m4a_gba_dma_enable(M4AGbaAudio *g)
{
    /* The enable edge reloads the internal source address from DMAxSAD,
     * i.e. the start of each channel's buffer.  FIFO contents are kept. */
    if (!g->dmaEnabled) {
        g->fifo[0].srcPos = 0;
        g->fifo[1].srcPos = 0;
    }
    g->dmaEnabled = true;
}

void m4a_gba_dma_disable(M4AGbaAudio *g)
{
    g->dmaEnabled = false;
}

void m4a_gba_timer_enable(M4AGbaAudio *g)
{
    /* GBATimerWriteTMCNT_HI: the counter reloads and the first overflow
     * comes one full period later. */
    if (!g->timerEnabled) {
        g->timerEnabled = true;
        g->timerRem = 0;
        int64_t period = g->timerPeriodNum / g->timerPeriodDen;
        g->timerRem = g->timerPeriodNum % g->timerPeriodDen;
        g->timerNext = g->now + period;
    }
}

void m4a_gba_timer_disable(M4AGbaAudio *g)
{
    g->timerEnabled = false;
}

static void timer_schedule_next(M4AGbaAudio *g)
{
    int64_t period = g->timerPeriodNum / g->timerPeriodDen;
    g->timerRem += g->timerPeriodNum % g->timerPeriodDen;
    if (g->timerRem >= g->timerPeriodDen) {
        g->timerRem -= g->timerPeriodDen;
        period++;
    }
    g->timerNext += period;
}

/* GBAAudioWriteFIFO: one DMA word into the FIFO ring (no fullness check,
 * exactly like the hardware model). */
static void fifo_push(M4AGbaFifo *f, M4AFifoWord w)
{
    f->fifo[f->fifoWrite] = w;
    if (++f->fifoWrite == GBA_FIFO_WORDS)
        f->fifoWrite = 0;
}

/* The FIFO DMA: 4 words from the channel's ring position.  Reading past the
 * end of A's buffer continues into B's (they are contiguous in SoundInfo);
 * past B's buffer reads silence. */
static void fifo_dma(M4AGbaAudio *g, int ch)
{
    M4AGbaFifo *f = &g->fifo[ch];
    const int16_t *base = g->ring + ch * g->ringSize;
    int limit = (2 - ch) * g->ringSize;
    for (int w = 0; w < 4; w++) {
        M4AFifoWord word;
        for (int i = 0; i < 4; i++) {
            int pos = f->srcPos + i;
            word.s[i] = pos < limit ? base[pos] : 0;
        }
        f->srcPos += 4;
        fifo_push(f, word);
    }
}

/* GBAAudioSampleFIFO: one timer-0 overflow as seen by FIFO `ch`. */
static void fifo_tick(M4AGbaAudio *g, int ch)
{
    M4AGbaFifo *f = &g->fifo[ch];
    int fifoSize;
    if (f->fifoWrite >= f->fifoRead)
        fifoSize = f->fifoWrite - f->fifoRead;
    else
        fifoSize = GBA_FIFO_WORDS - f->fifoRead + f->fifoWrite;

    if (GBA_FIFO_WORDS - fifoSize > 4 && g->dmaEnabled)
        fifo_dma(g, ch);

    if (!f->internalRemaining && fifoSize) {
        f->internal = f->fifo[f->fifoRead];
        f->internalRemaining = 4;
        if (++f->fifoRead == GBA_FIFO_WORDS)
            f->fifoRead = 0;
    }

    /* Slots of the current window from the one this overflow lands in. */
    int64_t until = (g->nextSampleEvent - g->now) - 1;
    int bits = 2 << g->resolution;
    until += (int64_t)1 << (9 - g->resolution);
    until >>= 9 - g->resolution;
    if (bits < until)
        until = bits;
    for (int i = bits - (int)until; i < bits; i++)
        f->samples[i] = f->internal.s[0];

    if (f->internalRemaining) {
        f->internal.s[0] = f->internal.s[1];
        f->internal.s[1] = f->internal.s[2];
        f->internal.s[2] = f->internal.s[3];
        f->internal.s[3] = 0;
        f->internalRemaining--;
    }
}

/* _applyBias */
static inline int16_t apply_bias(const M4AGbaAudio *g, int sample)
{
    sample += g->soundbias;
    if (sample >= 0x400)
        sample = 0x3FF;
    else if (sample < 0)
        sample = 0;
    return (int16_t)(((sample - g->soundbias) * g->masterVolume * 3) >> 4);
}

/* GBAAudioSample: compute the window's slots whose interval has elapsed by
 * `timestamp`. */
void m4a_gba_catch_up(M4AGbaAudio *g, int64_t timestamp)
{
    int64_t t = timestamp - g->lastSample - (int64_t)g->sampleIndex * g->sampleInterval;
    int maxSample = 2 << g->resolution;
    int sample;
    for (sample = g->sampleIndex; t >= g->sampleInterval && sample < maxSample;
         sample++, t -= g->sampleInterval) {
        int psgL = 0, psgR = 0;
        int64_t slotTime = g->lastSample + (int64_t)sample * g->sampleInterval;
        if (g->psgSample)
            g->psgSample(g->psgUser, slotTime, &psgL, &psgR);
        int16_t left = (int16_t)(psgL >> g->psgShift);
        int16_t right = (int16_t)(psgR >> g->psgShift);

        int a = g->fifo[0].samples[sample];
        int b = g->fifo[1].samples[sample];
        if (g->chALeft)  left  = (int16_t)(left  + ((a << 2) >> !g->volumeChA));
        if (g->chARight) right = (int16_t)(right + ((a << 2) >> !g->volumeChA));
        if (g->chBLeft)  left  = (int16_t)(left  + ((b << 2) >> !g->volumeChB));
        if (g->chBRight) right = (int16_t)(right + ((b << 2) >> !g->volumeChB));

        g->curLeft[sample] = apply_bias(g, left);
        g->curRight[sample] = apply_bias(g, right);
    }
    g->sampleIndex = sample;
    if (sample == maxSample) {
        g->lastSample += GBA_SAMPLE_EVENT;
        g->sampleIndex = 0;
    }
}

/* _sample: end of a 1024-cycle window. */
static void sample_event(M4AGbaAudio *g)
{
    m4a_gba_catch_up(g, g->now);

    int samples = 2 << g->resolution;
    for (int ch = 0; ch < 2; ch++) {
        int16_t last = g->fifo[ch].samples[samples - 1];
        for (int i = 0; i < GBA_MAX_SAMPLES; i++)
            g->fifo[ch].samples[i] = last;
    }

    for (int i = 0; i < samples; i++) {
        int16_t l = g->curLeft[i], r = g->curRight[i];
        if (m4a_blip_samples_avail(g->left) < BLIP_SIZE - 64) {
            m4a_blip_add_delta(g->left, (unsigned)g->clock, l - g->lastLeft);
            m4a_blip_add_delta(g->right, (unsigned)g->clock, r - g->lastRight);
            g->lastLeft = l;
            g->lastRight = r;
            g->clock += g->sampleInterval;
            if (g->clock >= GBA_BLIP_FRAME) {
                m4a_blip_end_frame(g->left, GBA_BLIP_FRAME);
                m4a_blip_end_frame(g->right, GBA_BLIP_FRAME);
                g->clock -= GBA_BLIP_FRAME;
            }
        }
    }
    g->nextSampleEvent += GBA_SAMPLE_EVENT;
}

int64_t m4a_gba_next_event(const M4AGbaAudio *g)
{
    int64_t next = g->nextSampleEvent;
    if (g->timerEnabled && g->timerNext < next)
        next = g->timerNext;
    return next;
}

void m4a_gba_run(M4AGbaAudio *g, int64_t until)
{
    for (;;) {
        int64_t next = m4a_gba_next_event(g);
        if (next > until)
            break;
        g->now = next;
        /* mgba orders ties by priority: the sample event (0x18) before the
         * timer (0x20). */
        if (g->nextSampleEvent == next) {
            sample_event(g);
        } else {
            fifo_tick(g, 0);
            fifo_tick(g, 1);
            timer_schedule_next(g);
        }
    }
    if (g->now < until)
        g->now = until;
}

int m4a_gba_samples_avail(const M4AGbaAudio *g)
{
    return m4a_blip_samples_avail(g->left);
}

int m4a_gba_read(M4AGbaAudio *g, float *outL, float *outR, int count)
{
    short tmp[512];
    int done = 0;
    while (done < count) {
        int n = count - done;
        if (n > 512) n = 512;
        int got = m4a_blip_read_samples(g->left, tmp, n, 0);
        if (got == 0)
            break;
        for (int i = 0; i < got; i++)
            outL[done + i] = (float)tmp[i] * (1.0f / 32768.0f);
        m4a_blip_read_samples(g->right, tmp, got, 0);
        for (int i = 0; i < got; i++)
            outR[done + i] = (float)tmp[i] * (1.0f / 32768.0f);
        done += got;
    }
    return done;
}

void m4a_gba_discard(M4AGbaAudio *g)
{
    short tmp[512];
    while (m4a_blip_samples_avail(g->left) > 0) {
        int n = m4a_blip_read_samples(g->left, tmp, 512, 0);
        m4a_blip_read_samples(g->right, tmp, n, 0);
    }
}
