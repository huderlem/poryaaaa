#ifndef M4A_GBA_H
#define M4A_GBA_H

/*
 * GBA sound output stage: the hardware between the m4a mixer and the host's
 * speakers, modeled on mgba 0.10.5 (src/gba/audio.c, timer.c, dma.c) so that
 * the engine's output can null against it.
 *
 *   - CPU clock of 16.78 MHz; 280896 cycles per video frame, 1232 per line.
 *   - Timer 0 overflows every 280896 / pcmSamplesPerVBlank cycles (1254 at
 *     13379 Hz) and clocks both DirectSound FIFOs: one sample is pulled from
 *     the FIFO's 4-sample internal word and held until the next overflow
 *     (sample-and-hold; mgba fills the sample slots the overflow lands in
 *     and after).  When the FIFO drops below 4 words the DMA fetches 4
 *     words from the m4a PCM ring buffer.
 *   - The PSG frame sequencer steps every 32768 cycles from reset (the
 *     callback belongs to the PSG model, m4a_psg.c).
 *   - The output DAC samples every 512 >> resolution cycles (SOUNDBIAS bits
 *     14-15; Emerald sets resolution 1 = 65536 Hz).  Every 1024 cycles the
 *     2 << resolution pending samples are produced: PSG (>> psgShift) plus
 *     FIFO (<< 2), SOUNDBIAS added and clamped to 10 bits, scaled by
 *     mgba's output gain (x48) and fed to the band-limited resampler as
 *     level deltas; resampler frames are 2048 cycles.
 *
 * The m4a driver side (SoundMain at VBlank, m4aSoundVSync at VCOUNT 150)
 * lives in m4a_engine.c; this module only knows about the ring buffer it is
 * handed and the DMA control writes the driver makes.
 */

#include <stdint.h>
#include <stdbool.h>
#include "m4a_resampler.h"

#ifdef __cplusplus
extern "C" {
#endif

#define GBA_CPU_HZ            16777216
#define GBA_CYCLES_PER_FRAME  280896
#define GBA_CYCLES_PER_LINE   1232
#define GBA_LINES_PER_FRAME   228
#define GBA_FIFO_WORDS        8        /* 32 bytes */
#define GBA_MAX_SAMPLES       16       /* per 1024-cycle sample event */
#define GBA_SAMPLE_EVENT      1024     /* cycles between sample events */
#define GBA_RESAMPLER_FRAME        2048     /* cycles per resampler frame */
#define GBA_PCM_DMA_BUF_SIZE  1584     /* m4a's PCM_DMA_BUF_SIZE, samples per side (the game's) */
/* The largest ring the engine mixes into: the HQ mixer at another rate uses 7
 * frames (the wiki's PCM_DMA_BUF_SIZE table; 42048 Hz -> 7 * 0x2C0). */
#define GBA_PCM_DMA_BUF_MAX   (7 * 0x2C0)

/* One 32-bit FIFO word: four consecutive PCM samples (int8 on the
 * hardware; held sign-extended here). */
typedef struct {
    int16_t s[4];
} M4AFifoWord;

typedef struct {
    M4AFifoWord fifo[GBA_FIFO_WORDS];
    int fifoWrite;
    int fifoRead;
    M4AFifoWord internal;      /* word being shifted out */
    int internalRemaining;     /* samples left in `internal` */
    int16_t samples[GBA_MAX_SAMPLES]; /* held value per DAC slot */
    int srcPos;                /* DMA source: index into the ring (samples) */
} M4AGbaFifo;

/* Per-slot PSG level callback: the output of the four PSG channels at
 * `timestamp`, in mgba's PSG units (channel volume 0-15, << 3, times
 * 1 + NR50 volume), before the SOUNDCNT_H shift. */
typedef void (*M4AGbaPsgSampleFn)(void *user, int64_t timestamp, int *left, int *right);
/* Frame-sequencer callback: one 512 Hz PSG frame step at `timestamp` (mgba's
 * _updateFrame, every 32768 cycles from reset, run after the DAC catch-up and
 * before a sample event due at the same cycle). */
typedef void (*M4AGbaPsgFrameFn)(void *user, int64_t timestamp);

typedef struct {
    int64_t now;                /* CPU cycles since reset */

    /* DAC sampling */
    int resolution;             /* SOUNDBIAS bits 14-15 */
    int sampleInterval;         /* 512 >> resolution */
    int64_t lastSample;         /* start of the current 1024-cycle window */
    int64_t nextSampleEvent;
    int sampleIndex;            /* slots of the window already computed */
    int16_t curLeft[GBA_MAX_SAMPLES], curRight[GBA_MAX_SAMPLES];
    int soundbias;              /* bias value (0x200) */
    int masterVolume;           /* mgba GBA_AUDIO_VOLUME_MAX = 0x100 */

    /* Resampler */
    M4AResampler *left, *right;
    int16_t lastLeft, lastRight;
    int clock;                  /* cycles into the current resampler frame */
    double hostRate;

    /* SOUNDCNT_H routing (Emerald: A right, B left, both full, PSG 100%) */
    int psgShift;
    bool chALeft, chARight, chBLeft, chBRight;
    bool volumeChA, volumeChB;

    /* DirectSound FIFOs and the DMA that feeds them */
    M4AGbaFifo fifo[2];         /* 0 = A, 1 = B */
    bool dmaEnabled;            /* both FIFO DMAs on/off together */
    int64_t fifoDmaCycle;       /* cycle of the last FIFO refill DMA (-1 before the first) */
    const int8_t *ring;        /* [2][ringSize]: A's buffer then B's */
    int ringSize;

    /* Timer 0, period 280896 / pcmSamplesPerVBlank cycles, integer like the
     * game's TM0CNT_L reload (1254 at 13379 Hz) */
    bool timerEnabled;
    int64_t timerNext;
    int timerPeriodNum;         /* 280896 */
    int timerPeriodDen;         /* pcmSamplesPerVBlank */

    M4AGbaPsgSampleFn psgSample;
    M4AGbaPsgFrameFn psgFrame;
    void *psgUser;
    int64_t nextFrameEvent;     /* next PSG frame-sequencer step */
} M4AGbaAudio;

/* Reset to mgba's power-on state at cycle 0.  `ring` is the m4a PCM buffer
 * pair (A then B, `ringSize` samples each) the DMA reads; the caller owns it. */
void m4a_gba_init(M4AGbaAudio *g, double hostRate, const int8_t *ring, int ringSize,
                  int pcmSamplesPerVBlank);
void m4a_gba_destroy(M4AGbaAudio *g);

/* Driver-side register writes, stamped at g->now. */
void m4a_gba_write_soundbias_resolution(M4AGbaAudio *g, int resolution);
void m4a_gba_fifo_reset(M4AGbaAudio *g);           /* SOUNDCNT_H FIFO reset bits */
void m4a_gba_dma_enable(M4AGbaAudio *g);           /* DMA1/2CNT_H enable (source reload) */
void m4a_gba_dma_disable(M4AGbaAudio *g);
/* CPU cycles the sound FIFO refill DMAs steal from the instruction stream
 * between `from` (exclusive) and `to` (inclusive): both FIFOs are refilled
 * with 4 words each at the same timer overflow every 16 samples, 18 cycles
 * measured in mgba (test/accuracy/ref/mgba_probe.c: eight 2-cycle FIFO
 * writes at frame cycle 15091..15105, every 20064 cycles).  The timing
 * model adds it to MPlayMain's and CgbSound's cursors. */
int64_t m4a_gba_fifo_dma_stall(const M4AGbaAudio *g, int64_t from, int64_t to);
#define M4A_CYC_FIFO_DMA_STALL 18
void m4a_gba_timer_enable(M4AGbaAudio *g);         /* TM0CNT_H enable: first overflow after one period */
void m4a_gba_timer_disable(M4AGbaAudio *g);
void m4a_gba_set_pcm_rate(M4AGbaAudio *g, int pcmSamplesPerVBlank, const int8_t *ring, int ringSize);

/* Next hardware event time (PSG frame step, sample event or timer overflow). */
int64_t m4a_gba_next_event(const M4AGbaAudio *g);

/* Run the hardware up to `until` (inclusive of events at `until`): timer
 * overflows and sample events in mgba's order. */
void m4a_gba_run(M4AGbaAudio *g, int64_t until);

/* Compute the DAC slots that have fully elapsed before `timestamp`
 * (mgba's GBAAudioSample catch-up, done before every PSG register write). */
void m4a_gba_catch_up(M4AGbaAudio *g, int64_t timestamp);

/* Host-rate output: samples available, and read up to count into float
 * buffers (int16 / 32768).  Returns the number read. */
int m4a_gba_samples_avail(const M4AGbaAudio *g);
int m4a_gba_read(M4AGbaAudio *g, float *outL, float *outR, int count);
void m4a_gba_discard(M4AGbaAudio *g);              /* drop buffered output */

#ifdef __cplusplus
}
#endif

#endif /* M4A_GBA_H */
