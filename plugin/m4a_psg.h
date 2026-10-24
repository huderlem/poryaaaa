#ifndef M4A_PSG_H
#define M4A_PSG_H

/*
 * GBA PSG (the four Game Boy sound channels) modeled on mgba 0.10.5's
 * src/gb/audio.c in its GB_AUDIO_GBA style, so that the engine's PSG output
 * nulls against mgba's:
 *
 *   - squares: an 8-step duty index caught up by elapsed time in units of
 *     4 * (2048 - frequency) * 4 CPU cycles, including the two catch-up
 *     cases that run while a channel is not sounding (channels == 0x1/0x2
 *     and a lastUpdate more than 0x40000000 cycles behind);
 *   - wave: the GBA's rotating wave RAM, 4 or 8 words (size bit) and two
 *     banks, one nibble per step of 2 * (2048 - rate) * 4 cycles, with
 *     mgba's volume shifts and 75% mode;
 *   - noise: a point-sampled LFSR (15- or 7-bit), its last output bit
 *     times the envelope volume;
 *   - the 64 Hz envelope with mgba's dead states, the sweep unit with its
 *     `occurred` flag, length counters, and the frame sequencer stepping
 *     every 32768 cycles from reset, restarted at frame 7 when NR52 turns
 *     the unit on;
 *   - GBAudioSamplePSG's GBA scale: unipolar 0..15 per tone channel, << 3,
 *     noise << 3, times (1 + NR50 volume) per side.
 *
 * Register writes follow mgba's GBAIOWrite8 / GBAIOWrite semantics: the
 * byte-addressable registers (NRx1..NRx4) are handled individually, the
 * others (NR10, NR30, NR50/51, NR52) go through the merged 16-bit path that
 * ignores writes while NR52 is off, and each register keeps mgba's stored-bit
 * image for read-back (CgbSound reads NRx2, NR10, NR43 and NR51 back).
 *
 * Time is the CPU cycle count since reset (the engine's int64 clock); the
 * caller runs the DAC catch-up (m4a_gba_catch_up) before every write, as
 * mgba's GBAAudioSample does, and calls m4a_psg_frame every 32768 cycles.
 */

#include <stdint.h>
#include <stdbool.h>

#ifdef __cplusplus
extern "C" {
#endif

/* GBA I/O register offsets (byte addresses, REG_BASE relative). */
#define PSG_REG_NR10 0x60
#define PSG_REG_NR11 0x62
#define PSG_REG_NR12 0x63
#define PSG_REG_NR13 0x64
#define PSG_REG_NR14 0x65
#define PSG_REG_NR21 0x68
#define PSG_REG_NR22 0x69
#define PSG_REG_NR23 0x6C
#define PSG_REG_NR24 0x6D
#define PSG_REG_NR30 0x70
#define PSG_REG_NR31 0x72
#define PSG_REG_NR32 0x73
#define PSG_REG_NR33 0x74
#define PSG_REG_NR34 0x75
#define PSG_REG_NR41 0x78
#define PSG_REG_NR42 0x79
#define PSG_REG_NR43 0x7C
#define PSG_REG_NR44 0x7D
#define PSG_REG_NR50 0x80
#define PSG_REG_NR51 0x81
#define PSG_REG_NR52 0x84
#define PSG_REG_WAVE_RAM 0x90   /* 4 words, 0x90..0x9F */

#define PSG_FRAME_CYCLES 32768  /* frame sequencer period, CPU cycles */
#define PSG_TIMING_FACTOR 4     /* GBA CPU cycles per Game Boy clock */

typedef struct {
    int length;
    int duty;
    int stepTime;
    int initialVolume;
    int currentVolume;
    bool direction;
    int dead;
    int nextStep;
} M4APsgEnvelope;

typedef struct {
    int shift;
    int time;
    int step;
    bool direction;
    bool enable;
    bool occurred;
    int realFrequency;
} M4APsgSweep;

typedef struct {
    M4APsgSweep sweep;
    M4APsgEnvelope envelope;
    int frequency;
    int length;
    bool stop;
    int64_t lastUpdate;
    uint8_t index;
    int8_t sample;
} M4APsgSquare;

typedef struct {
    bool size;
    bool bank;
    bool enable;
    int8_t sample;
    unsigned length;
    int volume;
    int rate;
    bool stop;
    int window;
    bool readable;
    uint32_t wavedata32[8];
    int64_t nextUpdate;
} M4APsgWave;

typedef struct {
    M4APsgEnvelope envelope;
    int ratio;
    int frequency;
    bool power;
    bool stop;
    int length;
    uint32_t lfsr;
    int64_t lastEvent;
    int8_t sample;
} M4APsgNoise;

/* Optional write observer for tests: every byte register write and wave RAM
 * word write the PSG receives, in order. */
typedef void (*M4APsgWriteLogFn)(void *user, uint32_t reg, uint32_t value);

typedef struct {
    M4APsgSquare ch1, ch2;
    M4APsgWave ch3;
    M4APsgNoise ch4;

    uint8_t volumeRight, volumeLeft;
    bool ch1Right, ch2Right, ch3Right, ch4Right;
    bool ch1Left, ch2Left, ch3Left, ch4Left;
    bool playingCh1, playingCh2, playingCh3, playingCh4;

    int frame;
    bool enable;

    /* mgba's memory.io image of REG_SOUND1CNT_LO .. REG_SOUNDCNT_X, indexed
     * by (reg - 0x60) >> 1, with the stored-bit masks applied. */
    uint16_t io[0x14];

    M4APsgWriteLogFn writeLog;
    void *writeLogUser;
} M4APsg;

/* Power-on state (GBAudioReset + GBAAudioReset at cycle 0). */
void m4a_psg_reset(M4APsg *p);

/* A CPU byte write to I/O register `reg` (0x60..0x85) at cycle `now`. */
void m4a_psg_write8(M4APsg *p, uint32_t reg, uint8_t value, int64_t now);
/* A 32-bit write to wave RAM word `index` (0..3) at cycle `now`. */
void m4a_psg_write_wave_ram(M4APsg *p, int index, uint32_t value, int64_t now);
/* A CPU byte read of register `reg`, as GBAIORead returns it. */
uint8_t m4a_psg_read8(const M4APsg *p, uint32_t reg);

/* GBAudioRun: bring the channels in `channels` (bit 0 = ch1 .. bit 3 = ch4)
 * up to `timestamp`. */
void m4a_psg_run(M4APsg *p, int64_t timestamp, int channels);
/* GBAudioUpdateFrame: one frame-sequencer step at `now`. */
void m4a_psg_frame(M4APsg *p, int64_t now);
/* GBAudioSamplePSG: the current output of the channels in `chMask` (bit 0 =
 * ch1 .. bit 3 = ch4) in mgba's PSG units, before the SOUNDCNT_H shift. */
void m4a_psg_sample(const M4APsg *p, int chMask, int *left, int *right);

#ifdef __cplusplus
}
#endif

#endif /* M4A_PSG_H */
