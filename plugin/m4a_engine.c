#include "m4a_engine.h"
#include "m4a_seq_estimate.h"
#include "m4a_channel.h"
#include "m4a_mixer.h"
#include "m4a_hq_mixer.h"
#include "m4a_reverb.h"
#include "m4a_tables.h"
#include <string.h>
#include <stdlib.h>

/* A cross-thread flag (frameCostMaxResetPending): raised by any thread,
 * taken (read and cleared in one step) by the engine's thread.  Compiler
 * builtins rather than <stdatomic.h>, whose _Atomic types a C++ host
 * including m4a_engine.h could not compile. */
#if defined(_MSC_VER) && !defined(__clang__)
#include <intrin.h>
static inline void flag_raise(volatile int32_t *f) { _InterlockedExchange((volatile long *)f, 1); }
static inline bool flag_take(volatile int32_t *f) { return _InterlockedExchange((volatile long *)f, 0) != 0; }
static inline void flag_clear(volatile int32_t *f) { _InterlockedExchange((volatile long *)f, 0); }
static inline void seq_publish(volatile uint32_t *s, uint32_t v) { _InterlockedExchange((volatile long *)s, (long)v); }
#else
static inline void flag_raise(volatile int32_t *f) { __atomic_store_n(f, 1, __ATOMIC_RELEASE); }
static inline bool flag_take(volatile int32_t *f) { return __atomic_exchange_n(f, 0, __ATOMIC_ACQ_REL) != 0; }
static inline void flag_clear(volatile int32_t *f) { __atomic_store_n(f, 0, __ATOMIC_RELEASE); }
static inline void seq_publish(volatile uint32_t *s, uint32_t v) { __atomic_store_n(s, v, __ATOMIC_RELEASE); }
#endif

/* Duty cycle patterns for the pulse-width modulation effect (PWMC command).
 * They loop from start to end while the effect is running.  Hardware duty-cycle
 * values: 0 = 12.5%, 1 = 25%, 2 = 50%, 3 = 75%.  Matches gPulseWidthModPatterns
 * in pokeemerald's m4a_tables.c. */
const PulseWidthModPattern gPulseWidthModPatterns[] =
{
    { 0, {0} },           /* 0: none */
    { 3, {2, 1, 0} },     /* 1: descending 50% -> 25% -> 12.5% */
    { 3, {0, 1, 2} },     /* 2: ascending 12.5% -> 25% -> 50% */
    { 4, {0, 1, 2, 1} },  /* 3: triangle 12.5% -> 25% -> 50% -> 25% */
    { 4, {2, 1, 0, 1} },  /* 4: inverted triangle 50% -> 25% -> 12.5% -> 25% */
    { 2, {1, 2} },        /* 5: alternating 25% <-> 50% */
    { 2, {0, 2} },        /* 6: alternating 12.5% <-> 50% */
    { 2, {0, 1} },        /* 7: alternating 12.5% <-> 25% */
};

const uint8_t gNumPulseWidthModPatterns =
    sizeof(gPulseWidthModPatterns) / sizeof(gPulseWidthModPatterns[0]);

/*
 * MidiKeyToFreq - matches m4a.c
 * Converts MIDI key + fine adjust to a frequency word for PCM playback.
 */
uint32_t m4a_midi_key_to_freq(WaveData *wav, uint8_t key, uint8_t fineAdjust)
{
    uint32_t val1, val2;
    uint32_t fineAdjustShifted = (uint32_t)fineAdjust << 24;

    if (key > 178) {
        key = 178;
        fineAdjustShifted = 255u << 24;
    }

    val1 = gScaleTable[key];
    val1 = gFreqTable[val1 & 0xF] >> (val1 >> 4);

    val2 = gScaleTable[key + 1];
    val2 = gFreqTable[val2 & 0xF] >> (val2 >> 4);

    return umul3232H32(wav->freq, val1 + umul3232H32(val2 - val1, fineAdjustShifted));
}

/*
 * MidiKeyToCgbFreq - matches m4a.c
 */
uint32_t m4a_midi_key_to_cgb_freq(uint8_t chanNum, uint8_t key, uint8_t fineAdjust)
{
    if (chanNum == 4) {
        /* Noise channel */
        if (key <= 20)
            key = 0;
        else {
            key -= 21;
            if (key > 59)
                key = 59;
        }
        return gNoiseTable[key];
    } else {
        int32_t val1, val2;

        if (key <= 35) {
            fineAdjust = 0;
            key = 0;
        } else {
            key -= 36;
            if (key > 130) {
                key = 130;
                fineAdjust = 255;
            }
        }

        val1 = gCgbScaleTable[key];
        val1 = gCgbFreqTable[val1 & 0xF] >> (val1 >> 4);

        val2 = gCgbScaleTable[key + 1];
        val2 = gCgbFreqTable[val2 & 0xF] >> (val2 >> 4);

        return (uint32_t)(val1 + ((fineAdjust * (val2 - val1)) >> 8) + 2048);
    }
}

/*
 * TONEDATA_TYPE_FIX quantization for the `_alt` PSG voices - matches the
 * `ch < 4 && (type & TONEDATA_TYPE_FIX)` block in CgbSound (m4a.c).  Rounding
 * the 11-bit frequency register makes the oscillator period an integer
 * multiple of the DAC's PWM period, which suppresses PWM beating at the cost
 * of a slight detune (the error is +-1 in (2048 - reg), so roughly f/131072
 * for the square channels and f/65536 for the wave channel -- a few cents low
 * in the register, tens of cents up high).
 *
 * The GBA picks the rounding step from REG_SOUNDBIAS_H, i.e. the DAC's PWM
 * rate.  Emerald calls m4aSoundMode(SOUND_MODE_DA_BIT_8 | ...) at startup
 * (m4a.c:78), which leaves REG_SOUNDBIAS_H at 0x40 = 65536 Hz, so only the
 * `(freq + 1) & 0x7fe` branch is ever taken; the 32768 Hz branch would round
 * to a multiple of 4 instead.  Nothing in this engine can change the PWM
 * rate, so only the live branch is implemented.
 *
 * Applied only where the GBA sets CGB_CHANNEL_MO_PIT (note-on, track pitch
 * updates, portamento) -- not to the frequency sweep unit in m4a_channel.c,
 * which drives NR13/NR14 directly and bypasses this path.  CgbSound also
 * guards the block with `ch < 4`, so the FIX bit is inert on the noise
 * channel: voice_noise_alt behaves exactly like voice_noise, and note-on
 * leaves fixedFreq false for type 4.
 */
static inline uint32_t m4a_cgb_fix_freq(const M4ACGBChannel *ch, uint32_t freq)
{
    return ch->fixedFreq ? ((freq + 1) & 0x7FE) : freq;
}

/*
 * The frequency half of a CGB_CHANNEL_MO_PIT update: the channel's register
 * value for `key` (clamped at 0) + `fine`, with the FIX rounding applied.
 * Every driver-side pitch write goes through here (or cgb_set_pitch below) so
 * the quantization can't be missed on one path and present on another.
 */
static inline uint32_t cgb_pitch_freq(const M4ACGBChannel *ch, int32_t key, uint8_t fine)
{
    if (key < 0) key = 0;
    return m4a_cgb_fix_freq(ch, m4a_midi_key_to_cgb_freq(ch->type, (uint8_t)key, fine));
}

/*
 * cgb_pitch_freq + register write for a channel that is already sounding.
 * Preserves NR43 bit 3 (7-bit LFSR mode) on the noise channel: gNoiseTable
 * entries always have bit 3 = 0, the period bit is ORed in at note-on time
 * and must survive frequency updates.
 */
static inline void cgb_set_pitch(M4ACGBChannel *ch, int32_t key, uint8_t fine)
{
    uint32_t newFreq = cgb_pitch_freq(ch, key, fine);
    if (ch->type == 4)
        newFreq |= ch->frequency & 0x08;
    ch->frequency = newFreq;
    /* MPlayMain's PITCHG pass: the next CgbSound writes NRx3/NRx4. */
    ch->modify |= 0x02;
}

/*
 * The volume half of TrkVolPitSet, for an arbitrary track volume: the track's
 * pan, volX and (tremolo/autopan) LFO state applied to `volume`.  Auditions
 * hold a volume of their own, so the same math has to run for a value that is
 * not the one currently on the track.
 */
static void track_vol_pair(const M4ATrack *track, uint32_t volume, int32_t pan, uint8_t *volMR,
                           uint8_t *volML)
{
    int32_t x = (volume * track->volX) >> 5;

    if (track->modT == 1)
        x = ((uint32_t)x * (track->modM + 128)) >> 7;

    int32_t y = 2 * pan + track->panX;

    if (track->modT == 2)
        y += track->modM;

    if (y < -128) y = -128;
    else if (y > 127) y = 127;

    *volMR = (uint8_t)((uint32_t)((y + 128) * x) >> 8);
    *volML = (uint8_t)((uint32_t)((127 - y) * x) >> 8);
}

/* The volume basis a channel's own levels are computed from: an audition
 * keeps the VOL and PAN it was keyed at, everything else follows the track.
 * Either override can stand alone -- a channel with only one of them takes
 * the track's live value for the other. */
static void chn_vol_pair(const M4ATrack *track, uint8_t auditionVolume, int8_t auditionPan,
                         uint8_t *volMR, uint8_t *volML)
{
    if (auditionVolume != M4A_AUDITION_VOL_NONE || auditionPan != M4A_AUDITION_PAN_NONE) {
        uint32_t volume = auditionVolume != M4A_AUDITION_VOL_NONE ? auditionVolume : track->volume;
        int32_t pan = auditionPan != M4A_AUDITION_PAN_NONE ? auditionPan : track->pan;
        track_vol_pair(track, volume, pan, volMR, volML);
        return;
    }
    *volMR = track->volMR;
    *volML = track->volML;
}

/*
 * Track volume and pitch calculation - matches TrkVolPitSet in m4a.c
 */
void m4a_track_vol_pit_set(M4ATrack *track)
{
    /* Volume calculation */
    track_vol_pair(track, track->volume, track->pan, &track->volMR, &track->volML);

    /* Pitch calculation */
    int32_t bend = (int32_t)track->bend * track->bendRange;
    int32_t pitchVal = (track->tune + bend) * 4
                     + ((int32_t)track->keyShift << 8)
                     + ((int32_t)track->keyShiftX << 8)
                     + track->pitX;

    if (track->modT == 0)
        pitchVal += 16 * track->modM;

    track->keyM = (int8_t)(pitchVal >> 8);
    track->pitM = (uint8_t)pitchVal;
}

/*
 * Channel volume calculation - matches ChnVolSetAsm in m4a_1.s
 */
static void chn_vol_set(M4APCMChannel *ch, M4ATrack *track)
{
    uint8_t volMR, volML;
    chn_vol_pair(track, ch->auditionVolume, ch->auditionPan, &volMR, &volML);

    uint32_t velocity = ch->velocity;
    int32_t rhythmPan = ch->rhythmPan;
    uint32_t panR = (uint32_t)(0x80 + rhythmPan);
    uint32_t volR = panR * velocity;
    uint32_t result = (volR * volMR) >> 14;
    if (result > 0xFF) result = 0xFF;
    ch->rightVolume = (uint8_t)result;

    uint32_t panL = (uint32_t)(0x7F - rhythmPan);
    uint32_t volL = panL * velocity;
    result = (volL * volML) >> 14;
    if (result > 0xFF) result = 0xFF;
    ch->leftVolume = (uint8_t)result;
}

static void cgb_chn_vol_set(M4ACGBChannel *ch, M4ATrack *track)
{
    uint8_t volMR, volML;
    chn_vol_pair(track, ch->auditionVolume, ch->auditionPan, &volMR, &volML);

    uint32_t velocity = ch->velocity;
    int32_t rhythmPan = ch->rhythmPan;
    uint32_t panR = (uint32_t)(0x80 + rhythmPan);
    uint32_t volR = panR * velocity;
    uint32_t result = (volR * volMR) >> 14;
    if (result > 0xFF) result = 0xFF;
    ch->rightVolume = (uint8_t)result;

    uint32_t panL = (uint32_t)(0x7F - rhythmPan);
    uint32_t volL = panL * velocity;
    result = (volL * volML) >> 14;
    if (result > 0xFF) result = 0xFF;
    ch->leftVolume = (uint8_t)result;
}

/* The audition VOL a note-on latches into the channel it starts: the host's
 * raw byte scaled by the song's master volume, matching track->volume. */
static uint8_t m4a_audition_volume(const M4AEngine *engine)
{
    if (engine->auditionVolume == M4A_AUDITION_VOL_NONE)
        return M4A_AUDITION_VOL_NONE;
    return (uint8_t)((uint32_t)engine->auditionVolume * engine->songMasterVolume
                     / MAX_SONG_VOLUME);
}

/* The audition PAN a note-on latches into the channel it starts. Unlike the
 * volume it needs no scaling: PAN is the track's own value either way. */
static int8_t m4a_audition_pan(const M4AEngine *engine)
{
    return engine->auditionPan;
}

/*
 * Resolve voice for a given key - handles keysplit and rhythm types
 */
static ToneData *resolve_voice(ToneData *voice, uint8_t key)
{
    if (!voice) return NULL;

    uint8_t type = voice->type;

    if (type & VOICE_KEYSPLIT_ALL) {
        /* Rhythm/drumset: each key maps to a different voice entry */
        ToneData *subGroup = (ToneData *)voice->subGroup;
        if (!subGroup) return NULL;
        ToneData *resolved = &subGroup[key];
        /* Don't allow nested keysplit */
        if (resolved->type & (VOICE_KEYSPLIT | VOICE_KEYSPLIT_ALL))
            return NULL;
        return resolved;
    }

    if (type & VOICE_KEYSPLIT) {
        /* Key split: lookup table maps key to sub-voice index */
        ToneData *subGroup = (ToneData *)voice->subGroup;
        uint8_t *splitTable = voice->keySplitTable;
        if (!subGroup || !splitTable) return NULL;
        uint8_t idx = splitTable[key];
        ToneData *resolved = &subGroup[idx];
        if (resolved->type & (VOICE_KEYSPLIT | VOICE_KEYSPLIT_ALL))
            return NULL;
        return resolved;
    }

    return voice;
}

/*
 * ---- Output stage and scheduler --------------------------------------------
 *
 * The engine runs on the GBA's CPU clock (16.78 MHz, 280896 cycles per video
 * frame).  Two interrupt handlers do the driver's work each frame, at the
 * cycle offsets measured in mgba 0.10.5 running the accuracy test ROM (which
 * mirrors Emerald's interrupt layout):
 *
 *   VCOUNT 150 (line start + 250 cycles): m4aSoundVSync -- counts PCM DMA
 *     frames and every pcmDmaPeriod frames restarts the FIFO DMAs at the
 *     start of the ring buffer.
 *   VBLANK, line 160: m4aSoundMain -- MPlayMain (the sequencer frame hook),
 *     the envelopes (SoundMainRAM / CgbSound), then the mix of
 *     pcmSamplesPerVBlank samples into the ring slot given by pcmDmaCounter.
 *     The handler is entered at line start + 197, where the frame is
 *     stamped.  CgbSound's register writes are then placed by the SoundMain
 *     timing model: the sequencer accounts MPlayMain's work in CPU cycles
 *     (engine->cgbEntryCycles, M4A_CYC_* in m4a_engine.h) and the CgbSound
 *     driver (m4a_channel.c) advances a write cursor through the measured
 *     per-channel offsets, running the hardware up to each write.  The PCM
 *     mix only fills a ring slot the DMA reads frames later, so its stamp
 *     does not matter.
 *
 * mgba's video starts a reset at line 126, so frame k >= 1 starts at
 * 124773 + (k - 1) * 280896 cycles.  m4aSoundInit's own register writes
 * (SoundInit, two SampleFreqSet passes that each wait for line 159) and the
 * first interrupt the game catches (frame 2's) are replayed by
 * m4a_engine_boot from the same measurements; see docs/ACCURACY.md, "mgba's audio path".
 */
#define FRAME1_START        124773
#define VSYNC_FRAME_OFFSET  (150 * GBA_CYCLES_PER_LINE + 250)
#define SOUNDMAIN_FRAME_OFFSET (160 * GBA_CYCLES_PER_LINE + M4A_CYC_SOUNDMAIN_ENTRY)
#define FIRST_IRQ_FRAME     2
/* The frame boot ends in: right after its SoundMain. */
#define BOOT_END_CYCLE      (FRAME1_START + (FIRST_IRQ_FRAME - 1) * GBA_CYCLES_PER_FRAME \
                             + SOUNDMAIN_FRAME_OFFSET)

enum {
    BOOT_PSG_WRITE,      /* a PSG register byte write (SoundInit, MPlayExtender) */
    BOOT_FIFO_RESET,     /* SoundInit: SOUNDCNT_H with both FIFO reset bits */
    BOOT_SOUNDBIAS,      /* SoundInit: SOUNDBIAS_H = 0x40 (65536 Hz DAC) */
    BOOT_DMA_ON,         /* m4aSoundVSyncOn from SoundInit's SampleFreqSet */
    BOOT_TIMER_ON,       /* SampleFreqSet, after waiting for VCOUNT 159 */
    BOOT_DMA_OFF,        /* m4aSoundMode -> m4aSoundVSyncOff */
    BOOT_TIMER_OFF,      /* m4aSoundMode -> SampleFreqSet */
    BOOT_DMA_ON_2,
    BOOT_TIMER_ON_2,     /* second VCOUNT 159 wait, frame 1 */
    BOOT_DONE
};
/* MPlayExtender's PSG setup (m4a.c) is ten register writes measured between
 * cycles 39875 and 39935; they are spread evenly over that span.  Its
 * 16-bit SOUNDCNT_L = 0 is two byte writes at one cycle. */
typedef struct { int64_t cycle; int action; uint32_t reg; uint8_t value; } BootStep;
static const BootStep sBootTableVanilla[] = {
    { 5720,   BOOT_PSG_WRITE, PSG_REG_NR52, 0x8F }, /* SoundInit: SOUNDCNT_X */
    { 5737,   BOOT_FIFO_RESET, 0, 0 },
    { 5760,   BOOT_SOUNDBIAS, 0, 0 },
    { 15873,  BOOT_DMA_ON, 0, 0 },
    { 39794,  BOOT_TIMER_ON, 0, 0 },
    { 39875,  BOOT_PSG_WRITE, PSG_REG_NR52, 0x8F }, /* MPlayExtender */
    { 39881,  BOOT_PSG_WRITE, PSG_REG_NR50, 0x00 },
    { 39881,  BOOT_PSG_WRITE, PSG_REG_NR51, 0x00 },
    { 39887,  BOOT_PSG_WRITE, PSG_REG_NR12, 0x08 },
    { 39893,  BOOT_PSG_WRITE, PSG_REG_NR22, 0x08 },
    { 39899,  BOOT_PSG_WRITE, PSG_REG_NR42, 0x08 },
    { 39905,  BOOT_PSG_WRITE, PSG_REG_NR14, 0x80 },
    { 39911,  BOOT_PSG_WRITE, PSG_REG_NR24, 0x80 },
    { 39917,  BOOT_PSG_WRITE, PSG_REG_NR44, 0x80 },
    { 39923,  BOOT_PSG_WRITE, PSG_REG_NR30, 0x00 },
    { 39929,  BOOT_PSG_WRITE, PSG_REG_NR50, 0x77 },
    { 41238,  BOOT_DMA_OFF, 0, 0 },
    { 47452,  BOOT_TIMER_OFF, 0, 0 },
    { 47893,  BOOT_DMA_ON_2, 0, 0 },
    { 320682, BOOT_TIMER_ON_2, 0, 0 },
};
/* The HQ-mixer test ROM (docs/HQ-MIXER-ACCURACY.md, appendix B, measured
 * with mgba_probe 2026-10-26): m4aSoundInit's CpuCopy32 of the larger
 * mixer moves SoundInit's block +2080 cycles; the IWRAM copy being the
 * next thing, SampleFreqSet's first pass and MPlayExtender land 8 cycles
 * earlier (39794 -> 39786, 39875..39935 -> 39867..39927, 41238 -> 41230,
 * 47452 -> 47444, 47893 -> 47885); the final timer-0 start, frame 1's
 * VCOUNT 159 wait, 4 cycles later (320682 -> 320686).  Everything from
 * frame 2's VSync on is identical to vanilla. */
static const BootStep sBootTableHq[] = {
    { 7800,   BOOT_PSG_WRITE, PSG_REG_NR52, 0x8F }, /* SoundInit: SOUNDCNT_X */
    { 7817,   BOOT_FIFO_RESET, 0, 0 },
    { 7840,   BOOT_SOUNDBIAS, 0, 0 },
    { 17953,  BOOT_DMA_ON, 0, 0 },
    { 39786,  BOOT_TIMER_ON, 0, 0 },
    { 39867,  BOOT_PSG_WRITE, PSG_REG_NR52, 0x8F }, /* MPlayExtender */
    { 39873,  BOOT_PSG_WRITE, PSG_REG_NR50, 0x00 },
    { 39873,  BOOT_PSG_WRITE, PSG_REG_NR51, 0x00 },
    { 39879,  BOOT_PSG_WRITE, PSG_REG_NR12, 0x08 },
    { 39885,  BOOT_PSG_WRITE, PSG_REG_NR22, 0x08 },
    { 39891,  BOOT_PSG_WRITE, PSG_REG_NR42, 0x08 },
    { 39897,  BOOT_PSG_WRITE, PSG_REG_NR14, 0x80 },
    { 39903,  BOOT_PSG_WRITE, PSG_REG_NR24, 0x80 },
    { 39909,  BOOT_PSG_WRITE, PSG_REG_NR44, 0x80 },
    { 39915,  BOOT_PSG_WRITE, PSG_REG_NR30, 0x00 },
    { 39921,  BOOT_PSG_WRITE, PSG_REG_NR50, 0x77 },
    { 41230,  BOOT_DMA_OFF, 0, 0 },
    { 47444,  BOOT_TIMER_OFF, 0, 0 },
    { 47885,  BOOT_DMA_ON_2, 0, 0 },
    { 320686, BOOT_TIMER_ON_2, 0, 0 },
};
/* The same two ROMs built at 21024 Hz (make RATE=21024; docs/HQ-MIXER-ACCURACY.md,
 * step 11; measured with mgba_probe 2026-10-27).  m4aSoundInit's mode word
 * is the only code change in the vanilla build, so SoundInit and its
 * SampleFreqSet pass are the vanilla cycles; m4aSoundMode's pass differs by
 * its divisions' operands (timer off 47452 -> 47450, DMA on 47893 -> 47887)
 * and the final VCOUNT 159 poll exits 6 cycles later (320682 -> 320688).
 * In the HQ build PCM_DMA_BUF_SIZE is 2464 (7 frames), so SoundInit's
 * CpuFill32 of the 1760-byte-larger SoundInfo moves its SampleFreqSet pass
 * +2636 cycles (DMA on 17953 -> 20589), the pass still ends on frame 0's
 * line 159 at vanilla's 39794 (so MPlayExtender's writes are vanilla's
 * 39875..39935 and the DMA off 41238), m4aSoundVSyncOff's CpuFill32 of
 * the larger ring moves the second pass (timer off 47444 -> 50088, DMA on
 * 47885 -> 50525), and the final timer start is the HQ ROM's 320686.
 * Every other table rate is unmeasured and replays the mixer's 13379 Hz
 * table (the rate's own SampleFreqSet numbers come from pcm_rate_setup). */
static const BootStep sBootTableVanilla21024[] = {
    { 5720,   BOOT_PSG_WRITE, PSG_REG_NR52, 0x8F },
    { 5737,   BOOT_FIFO_RESET, 0, 0 },
    { 5760,   BOOT_SOUNDBIAS, 0, 0 },
    { 15873,  BOOT_DMA_ON, 0, 0 },
    { 39794,  BOOT_TIMER_ON, 0, 0 },
    { 39875,  BOOT_PSG_WRITE, PSG_REG_NR52, 0x8F },
    { 39881,  BOOT_PSG_WRITE, PSG_REG_NR50, 0x00 },
    { 39881,  BOOT_PSG_WRITE, PSG_REG_NR51, 0x00 },
    { 39887,  BOOT_PSG_WRITE, PSG_REG_NR12, 0x08 },
    { 39893,  BOOT_PSG_WRITE, PSG_REG_NR22, 0x08 },
    { 39899,  BOOT_PSG_WRITE, PSG_REG_NR42, 0x08 },
    { 39905,  BOOT_PSG_WRITE, PSG_REG_NR14, 0x80 },
    { 39911,  BOOT_PSG_WRITE, PSG_REG_NR24, 0x80 },
    { 39917,  BOOT_PSG_WRITE, PSG_REG_NR44, 0x80 },
    { 39923,  BOOT_PSG_WRITE, PSG_REG_NR30, 0x00 },
    { 39929,  BOOT_PSG_WRITE, PSG_REG_NR50, 0x77 },
    { 41238,  BOOT_DMA_OFF, 0, 0 },
    { 47450,  BOOT_TIMER_OFF, 0, 0 },
    { 47887,  BOOT_DMA_ON_2, 0, 0 },
    { 320688, BOOT_TIMER_ON_2, 0, 0 },
};
static const BootStep sBootTableHq21024[] = {
    { 7800,   BOOT_PSG_WRITE, PSG_REG_NR52, 0x8F },
    { 7817,   BOOT_FIFO_RESET, 0, 0 },
    { 7840,   BOOT_SOUNDBIAS, 0, 0 },
    { 20589,  BOOT_DMA_ON, 0, 0 },
    { 39794,  BOOT_TIMER_ON, 0, 0 },
    { 39875,  BOOT_PSG_WRITE, PSG_REG_NR52, 0x8F },
    { 39881,  BOOT_PSG_WRITE, PSG_REG_NR50, 0x00 },
    { 39881,  BOOT_PSG_WRITE, PSG_REG_NR51, 0x00 },
    { 39887,  BOOT_PSG_WRITE, PSG_REG_NR12, 0x08 },
    { 39893,  BOOT_PSG_WRITE, PSG_REG_NR22, 0x08 },
    { 39899,  BOOT_PSG_WRITE, PSG_REG_NR42, 0x08 },
    { 39905,  BOOT_PSG_WRITE, PSG_REG_NR14, 0x80 },
    { 39911,  BOOT_PSG_WRITE, PSG_REG_NR24, 0x80 },
    { 39917,  BOOT_PSG_WRITE, PSG_REG_NR44, 0x80 },
    { 39923,  BOOT_PSG_WRITE, PSG_REG_NR30, 0x00 },
    { 39929,  BOOT_PSG_WRITE, PSG_REG_NR50, 0x77 },
    { 41238,  BOOT_DMA_OFF, 0, 0 },
    { 50088,  BOOT_TIMER_OFF, 0, 0 },
    { 50525,  BOOT_DMA_ON_2, 0, 0 },
    { 320686, BOOT_TIMER_ON_2, 0, 0 },
};
#define BOOT_TABLE_LEN ((int)(sizeof(sBootTableVanilla) / sizeof(sBootTableVanilla[0])))
_Static_assert(sizeof(sBootTableHq) == sizeof(sBootTableVanilla), "the boot tables hold the same steps");
_Static_assert(sizeof(sBootTableVanilla21024) == sizeof(sBootTableVanilla), "the boot tables hold the same steps");
_Static_assert(sizeof(sBootTableHq21024) == sizeof(sBootTableVanilla), "the boot tables hold the same steps");

static const BootStep *boot_table(const M4AEngine *engine)
{
    if (engine->pcmFreqMode == 7)   /* SOUND_MODE_FREQ_21024: the measured raised rate */
        return engine->hqMixer ? sBootTableHq21024 : sBootTableVanilla21024;
    return engine->hqMixer ? sBootTableHq : sBootTableVanilla;
}

static int64_t frame_start(int frame)
{
    return FRAME1_START + (int64_t)(frame - 1) * GBA_CYCLES_PER_FRAME;
}

int m4a_pcm_freq_mode_hz(int mode)
{
    if (mode < M4A_PCM_FREQ_MODE_MIN || mode > M4A_PCM_FREQ_MODE_MAX)
        mode = M4A_PCM_FREQ_MODE_DEFAULT;
    return (597275 * gPcmSamplesPerVBlankTable[mode - 1] + 5000) / 10000;
}

int m4a_pcm_freq_mode_from_hz(double hz)
{
    if (hz <= 0.0)
        return M4A_PCM_FREQ_MODE_DEFAULT;
    int best = M4A_PCM_FREQ_MODE_DEFAULT;
    double bestDist = -1.0;
    for (int m = M4A_PCM_FREQ_MODE_MIN; m <= M4A_PCM_FREQ_MODE_MAX; m++) {
        double d = hz - m4a_pcm_freq_mode_hz(m);
        if (d < 0) d = -d;
        if (bestDist < 0 || d < bestDist) {
            bestDist = d;
            best = m;
        }
    }
    return best;
}

/* PCM_DMA_BUF_SIZE of a configuration: the game's 1584 everywhere except
 * the HQ mixer at a rate other than 13379 Hz, where the wiki resizes the
 * define to 7 frames (docs/HQ-MIXER-ACCURACY.md, appendix C: 21024 Hz ->
 * 2464; the vanilla ROM keeps 1584 and pcmDmaPeriod = 1584 / frame). */
static int pcm_dma_buf_size(bool hqMixer, int spv)
{
    return (hqMixer && spv != 224) ? 7 * spv : GBA_PCM_DMA_BUF_SIZE;
}

/* SampleFreqSet (m4a.c): the per-rate numbers for the sound-mode frequency
 * (13379 Hz -> 224 samples per VBlank, timer period 1254 cycles, 7 VBlanks
 * per DMA buffer). */
static void pcm_rate_setup(M4AEngine *engine)
{
    int mode = engine->pcmFreqMode;
    if (mode < M4A_PCM_FREQ_MODE_MIN || mode > M4A_PCM_FREQ_MODE_MAX)
        mode = M4A_PCM_FREQ_MODE_DEFAULT;
    engine->pcmFreqMode = (uint8_t)mode;
    int spv = gPcmSamplesPerVBlankTable[mode - 1];
    engine->pcmSamplesPerVBlank = spv;
    engine->pcmDmaBufSize = pcm_dma_buf_size(engine->hqMixer, spv);
    engine->pcmDmaPeriod = (uint8_t)(engine->pcmDmaBufSize / spv);
    engine->pcmFreq = (597275 * spv + 5000) / 10000;
    engine->pcmDivFreq = (16777216 / engine->pcmFreq + 1) >> 1;
    engine->pcmDmaCounter = 0;
}

static void scheduler_reset(M4AEngine *engine)
{
    engine->bootStep = 0;
    engine->booted = false;
    engine->nextVSync = frame_start(FIRST_IRQ_FRAME) + VSYNC_FRAME_OFFSET;
    engine->nextSoundMain = frame_start(FIRST_IRQ_FRAME) + SOUNDMAIN_FRAME_OFFSET;
}

/* A CPU byte write to a PSG register, seen by both PSG instances (the
 * shadow pool mirrors the hardware state). */
static void psg_write_both(M4AEngine *engine, uint32_t reg, uint8_t value)
{
    M4AGbaAudio *g = &engine->gba;
    m4a_gba_catch_up(g, g->now);
    for (int i = 0; i < 2; i++)
        m4a_psg_write8(&engine->psg[i], reg, value, g->now);
}

static void boot_action(M4AEngine *engine, int step)
{
    M4AGbaAudio *g = &engine->gba;
    const BootStep *b = &boot_table(engine)[step];
    switch (b->action) {
    case BOOT_PSG_WRITE:
        psg_write_both(engine, b->reg, b->value);
        break;
    case BOOT_FIFO_RESET: m4a_gba_fifo_reset(g); break;
    case BOOT_SOUNDBIAS:  m4a_gba_write_soundbias_resolution(g, 1); break;
    case BOOT_DMA_ON:
    case BOOT_DMA_ON_2:   m4a_gba_dma_enable(g); break;
    case BOOT_DMA_OFF:    m4a_gba_dma_disable(g); break;
    case BOOT_TIMER_ON:
        /* SoundInit's own SampleFreqSet(SOUND_MODE_FREQ_13379): the first
         * pass runs the timer at the 224-sample period whatever the game's
         * rate; m4aSoundMode's pass (TIMER_OFF .. TIMER_ON_2) programs it. */
        g->timerPeriodDen = 224;
        m4a_gba_timer_enable(g);
        break;
    case BOOT_TIMER_ON_2: m4a_gba_timer_enable(g); break;
    case BOOT_TIMER_OFF:
        m4a_gba_timer_disable(g);
        g->timerPeriodDen = engine->pcmSamplesPerVBlank;
        break;
    default: break;
    }
}

/* m4aSoundVSync (m4a_1.s): every pcmDmaPeriod VBlanks, restart the FIFO DMAs
 * so they read from the start of the ring buffer again.  The disable/enable
 * pair reloads the DMA source address; the FIFO contents are untouched. */
static void sound_vsync(M4AEngine *engine)
{
    engine->pcmDmaCounter--;
    if (engine->pcmDmaCounter > 0)
        return;
    engine->pcmDmaCounter = (int8_t)engine->pcmDmaPeriod;
    m4a_gba_dma_disable(&engine->gba);
    m4a_gba_dma_enable(&engine->gba);
}

/* m4aSoundMain: the VBlank frame.  MPlayMain and CgbSound (the tick), then
 * SoundMainRAM into the ring slot pcmDmaCounter selects (SoundMain_4/5:
 * pcmBuffer + (pcmDmaPeriod - (pcmDmaCounter - 1)) * pcmSamplesPerVBlank
 * when pcmDmaCounter - 1 > 0, else the start). */
/* The FIFO refill stalls in the mixer's span [ram, ram + mix + stall]:
 * each refill due in it, in order, until one is due during m4aSoundMain's
 * final bx (serviced after the return).  Each costs M4A_CYC_FIFO_DMA_STALL,
 * less when it falls in one of the HQ fast path's DMA3 copies (the
 * copies' positions are in mixCycles' clock; the stalls before them move
 * them on). */
int64_t m4a_engine_mixer_stall(const M4AEngine *engine, int64_t ram, int64_t mix)
{
    const M4AGbaAudio *g = &engine->gba;
    int64_t stall = 0;
    int k = 0, n = engine->hqMixer ? engine->hqDma3Count : 0;
    for (int64_t t = m4a_gba_fifo_dma_next(g, ram); t >= 0 && t <= ram + mix + stall - M4A_MIX_EXIT_BX;
         t = m4a_gba_fifo_dma_next(g, t)) {
        int64_t c = M4A_CYC_FIFO_DMA_STALL;
        for (; k < n; k++) {
            int64_t p = t - (ram + engine->hqDma3Start[k] + stall), len = engine->hqDma3Len[k];
            if (p < -3)
                break;                       /* this copy is later */
            if (p >= len)
                continue;                    /* this copy is over */
            if (p < 0)
                c = M4A_HQMIX_FIFO_PRE;
            else if (p <= 7)
                c = M4A_HQMIX_FIFO_FIRST + p;
            else if (p < len - 1)
                c = M4A_HQMIX_FIFO_WORD + (p - 8) % 5;
            else
                c = M4A_HQMIX_FIFO_LAST;
            break;
        }
        stall += c;
    }
    return stall;
}

/* The frame's cost into the per-frame history (costHistory): the entry
 * first, then the count that publishes it.  Only the engine's thread
 * writes, so the count is read plainly here. */
static void cost_history_push(M4AEngine *engine, const M4AFrameCost *c, int64_t stamp)
{
    uint32_t seq = engine->costHistorySeq;
    uint32_t i = seq & (M4A_COST_HISTORY - 1);
    engine->costHistory[i] = *c;
    engine->costHistoryStamp[i] = stamp;
    engine->costHistoryEpoch[i] = engine->costEpoch;
    seq_publish(&engine->costHistorySeq, seq + 1);
}

/* SoundMain's cost for the frame just run (docs/ACCURACY.md, "SoundMain
 * cost model"), on the CPU clock from `stamp` (SoundMain's entry):
 * m4aSoundMain's entry 12 before it; CgbSound's entry from the sequencer
 * term (cgbEntryCycles); CgbSound's return from the cursor after its last
 * channel; SoundMainRAM's entry after SoundMain_4/5; the mixer's cycles
 * (m4a_mixer.c, or m4a_hq_mixer.c in HQ mode) to m4aSoundMain's return.  The FIFO DMA stall is charged
 * on every span it falls in, as the sequencer and CgbSound charge it. */
static void publish_frame_cost(M4AEngine *engine, int64_t stamp, int pcmDmaCounter)
{
    M4AFrameCost c = { 0, 0, 0, 0, false, false };
    /* The MIDI-path estimate's frame ends here whether or not it is used. */
    bool estimated = !engine->externalSequencer && (!engine->frameHook || engine->costEstimateForced);
    int32_t estimate = engine->externalSequencer ? 0 : m4a_est_publish(engine, stamp);
    if (flag_take(&engine->frameCostMaxResetPending)) {
        memset(&engine->frameCostMax, 0, sizeof(engine->frameCostMax));
        engine->costEpoch++;
    }
    /* Live mode mixes in pieces (nothing to cost): invalid.  The vanilla
     * and the HQ mixer charge mixCycles the same way (M4A_MIX_* /
     * M4A_HQMIX_*), from SoundMainRAM's entry to m4aSoundMain's return. */
    if (engine->liveMode) {
        engine->frameCost = c;
        cost_history_push(engine, &c, stamp);
        return;
    }
    const M4AGbaAudio *g = &engine->gba;
    int64_t sm = stamp - (M4A_CYC_SOUNDMAIN_ENTRY - M4A_CYC_M4A_SOUNDMAIN);
    /* The sequencer term.  With an M4ASequencer (or a timing-only one),
     * cgbEntryCycles is MPlayMain's charged work, exact on the corpus.
     * Without one, the MIDI-path estimate (m4a_seq_estimate.c): CgbSound's
     * work is the same, shifted from where the engine ran it (the fixed
     * entry, which places the PSG writes) to the estimated entry, the FIFO
     * stalls moving with it. */
    int64_t cgb = stamp + engine->cgbEntryCycles;
    int64_t cursorEnd = engine->cgbCursorEnd;
    if (estimated) {
        int64_t work = cursorEnd - cgb - m4a_gba_fifo_dma_stall(g, cgb, cursorEnd);
        cgb = sm + estimate;
        cursorEnd = cgb + work;
        cursorEnd += m4a_gba_fifo_dma_stall(g, cgb, cursorEnd);
    }
    int64_t cr = cursorEnd + M4A_CYC_CGB_TAIL;
    cr += m4a_gba_fifo_dma_stall(g, cursorEnd, cr);
    int s2r = pcmDmaCounter <= 1 ? M4A_CYC_SM_TO_RAM_FIRST : M4A_CYC_SM_TO_RAM;
    int64_t ram = cr + s2r;
    ram += m4a_gba_fifo_dma_stall(g, cr, ram);
    int64_t mix = engine->mixCycles;
    int64_t stall = m4a_engine_mixer_stall(engine, ram, mix);
    int64_t exit = ram + mix + stall;
    c.sequencer = (int32_t)(cgb - sm);
    c.cgb = (int32_t)(ram - cgb);
    c.mixer = (int32_t)(exit - ram);
    c.total = (int32_t)(exit - sm);
    c.valid = true;
    c.sequencerEstimated = estimated;
    engine->frameCost = c;
    if (!engine->frameCostMax.valid || c.total > engine->frameCostMax.total)
        engine->frameCostMax = c;
    cost_history_push(engine, &c, stamp);
}

void m4a_engine_reset_frame_cost_max(M4AEngine *engine)
{
    /* Consumed by the next publish_frame_cost, on the thread that runs
     * m4a_engine_process: a UI thread may call this while it runs. */
    flag_raise(&engine->frameCostMaxResetPending);
}

/* A configuration change (live mode, mixer, sound-mode rate, voicegroup):
 * the last frame's cost and the peak belong to the old one.  Called by the
 * setters, on the thread that drives the engine. */
static void frame_cost_invalidate(M4AEngine *engine)
{
    engine->frameCost.valid = false;
    memset(&engine->frameCostMax, 0, sizeof(engine->frameCostMax));
    flag_clear(&engine->frameCostMaxResetPending);
    /* The history's entries go with the peak (the count stays). */
    memset(engine->costHistory, 0, sizeof(engine->costHistory));
    engine->costEpoch++;
}

static void sound_main(M4AEngine *engine)
{
    int64_t stamp = engine->gba.now;
    engine->soundMainStamp = stamp;
    /* The mixer's charge is per frame (live mode's pieces add to it but are
     * never costed): start every frame from zero. */
    engine->mixCycles = 0;
    m4a_engine_tick(engine);
    engine->soundMainStamp = -1;
    int counter = engine->pcmDmaCounter;
    int offset = 0;
    if (counter - 1 > 0)
        offset = (engine->pcmDmaPeriod - (counter - 1)) * engine->pcmSamplesPerVBlank;
    if (engine->liveMode)
        /* The frame's envelope step only; live_mix_ahead mixes the ring. */
        m4a_sound_main_ram_range(engine, offset, counter, 0, 0, M4A_ENVELOPE_STEP_ALL);
    else if (engine->hqMixer)
        m4a_hq_sound_main_ram(engine, offset, counter);
    else
        m4a_sound_main_ram(engine, offset, counter);
    publish_frame_cost(engine, stamp, counter);
    if (engine->frameEndHook)
        engine->frameEndHook(engine, engine->frameEndHookUser);
    if (engine->frameLogHook)
        engine->frameLogHook(engine, engine->frameLogHookUser);
}

/* PSG level at one DAC slot, in mgba's PSG units before the SOUNDCNT_H
 * shift (GBAAudioSample: GBAudioRun to the slot time, then
 * GBAudioSamplePSG).  Both PSG instances advance; which one is heard depends
 * on the polyphony-debug invert mode, where real-pool audition channels stay
 * audible on top of the shadow pool. */
static void psg_sample_cb(void *user, int64_t timestamp, int *left, int *right)
{
    M4AEngine *engine = (M4AEngine *)user;
    m4a_psg_run(&engine->psg[0], timestamp, 0xF);
    m4a_psg_run(&engine->psg[1], timestamp, 0xF);
    if (!engine->polyDebugInvert) {
        m4a_psg_sample(&engine->psg[0], 0xF, left, right);
        return;
    }
    int auditionMask = 0;
    for (int ch = 0; ch < MAX_CGB_CHANNELS; ch++) {
        if ((engine->cgbChannels[ch].status & CHN_ON) && engine->cgbChannels[ch].audition)
            auditionMask |= 1 << ch;
    }
    int l = 0, r = 0, al = 0, ar = 0;
    m4a_psg_sample(&engine->psg[1], 0xF, &l, &r);
    if (auditionMask)
        m4a_psg_sample(&engine->psg[0], auditionMask, &al, &ar);
    *left = l + al;
    *right = r + ar;
}

/* The PSG frame sequencer (512 Hz), for both instances. */
static void psg_frame_cb(void *user, int64_t timestamp)
{
    M4AEngine *engine = (M4AEngine *)user;
    m4a_psg_frame(&engine->psg[0], timestamp);
    m4a_psg_frame(&engine->psg[1], timestamp);
}

/* Advance the whole machine to `until` cycles, running hardware events and
 * the driver's interrupts in time order.  Events due at a cycle run before the
 * driver's writes at that cycle: mgba processes every event due by an
 * instruction's start cycle before executing it. */
static void run_until(M4AEngine *engine, int64_t until)
{
    M4AGbaAudio *g = &engine->gba;
    const BootStep *table = boot_table(engine);
    for (;;) {
        int64_t next = engine->nextVSync < engine->nextSoundMain ? engine->nextVSync
                                                                 : engine->nextSoundMain;
        if (engine->bootStep < BOOT_TABLE_LEN && table[engine->bootStep].cycle < next)
            next = table[engine->bootStep].cycle;
        if (next > until) {
            m4a_gba_run(g, until);
            return;
        }
        /* Events due at `next` run before the driver's writes at `next`, as
         * mgba processes every event due by an instruction's start cycle
         * before executing it.  A frame's register writes may already have
         * carried `now` past `next`; m4a_gba_run then does nothing. */
        m4a_gba_run(g, next);
        while (engine->bootStep < BOOT_TABLE_LEN && table[engine->bootStep].cycle == next) {
            boot_action(engine, engine->bootStep);
            engine->bootStep++;
        }
        if (engine->nextVSync == next) {
            sound_vsync(engine);
            engine->nextVSync += GBA_CYCLES_PER_FRAME;
        }
        if (engine->nextSoundMain == next) {
            sound_main(engine);
            engine->nextSoundMain += GBA_CYCLES_PER_FRAME;
        }
    }
}

void m4a_engine_boot(M4AEngine *engine, bool emitAudio)
{
    if (engine->booted)
        return;
    engine->gba.psgSample = psg_sample_cb;
    engine->gba.psgFrame = psg_frame_cb;
    engine->gba.psgUser = engine;
    run_until(engine, BOOT_END_CYCLE);
    if (!emitAudio)
        m4a_gba_discard(&engine->gba);
    engine->booted = true;
}

/* Initialize engine */
void m4a_engine_init(M4AEngine *engine, float sampleRate)
{
    memset(engine, 0, sizeof(M4AEngine));
    engine->hqStackSp = M4A_HQ_STACK_SP_DEFAULT;

    engine->sampleRate = sampleRate;
    engine->pcmFreqMode = M4A_PCM_FREQ_MODE_DEFAULT;
    /* 12 matches Pokemon Emerald's m4aSoundInit (12 << SOUND_MODE_MASVOL_SHIFT).
     * masterVolume scales only the PCM path -- CGB channels bypass it -- so a
     * higher value makes DirectSound too loud relative to PSG. */
    engine->masterVolume = 12;
    engine->songMasterVolume = MAX_SONG_VOLUME;
    engine->auditionVolume = M4A_AUDITION_VOL_NONE;
    engine->auditionPan = M4A_AUDITION_PAN_NONE;
    engine->maxPcmChannels = 5;  /* default, matches Pokemon Emerald init */
    engine->polyEventClock = M4A_POLY_TICK_NONE;
    /* c15 phase.  SoundInit zero-fills SoundInfo and the first CgbSound the
     * ROM runs is frame 2's (there is no frame-1 interrupt: DISPSTAT's IRQ
     * enables are written after frame 1's VBlank), measured in mgba as c15
     * 0 -> 14 on frame 2 and 14 -> 13 on frame 3, the song's first
     * MPlayMain.  The engine's first tick is frame 2's SoundMain, so it
     * starts from 0 as the ROM does.  (The harness cannot tell 0 from 14:
     * every micro-test envelope is in sustain when the double-step lands.) */
    engine->c15 = 0;
    engine->cgbEntryCycles = M4A_CYC_CGB_ENTRY_DEFAULT;
    engine->soundMainStamp = -1;
    engine->tempoD = 150;
    engine->tempoU = 0x100;
    engine->tempoI = 150;
    engine->tempoC = 0;

    /* Initialize tracks with defaults */
    for (int i = 0; i < MAX_TRACKS; i++) {
        M4ATrack *track = &engine->tracks[i];
        track->bendRange = 2;
        track->volX = 64;
        track->rawVolume = 127;
        track->volume = 127;
        track->lfoSpeed = 22;
        track->pan = 0;
    }

    /* A zeroed auditionVolume would read as "audition at VOL 0" (silence),
     * and a zeroed auditionPan as "audition dead center", so every channel
     * starts at the no-override sentinels; note-on overwrites them on the
     * paths that do audition. */
    for (int i = 0; i < TOTAL_PCM_CHANNELS; i++) {
        engine->pcmChannels[i].auditionVolume = M4A_AUDITION_VOL_NONE;
        engine->pcmChannels[i].auditionPan = M4A_AUDITION_PAN_NONE;
    }
    for (int i = 0; i < TOTAL_CGB_CHANNELS; i++) {
        engine->cgbChannels[i].auditionVolume = M4A_AUDITION_VOL_NONE;
        engine->cgbChannels[i].auditionPan = M4A_AUDITION_PAN_NONE;
    }

    /* Initialize CGB channels with proper types and pan masks.  The shadow
     * pool (second half) mirrors the real channels one-to-one so a lost CGB
     * sound plays on a shadow channel of the same type. */
    for (int i = 0; i < TOTAL_CGB_CHANNELS; i += MAX_CGB_CHANNELS) {
        engine->cgbChannels[i + 0].type = 1;
        engine->cgbChannels[i + 0].panMask = 0x11;
        engine->cgbChannels[i + 1].type = 2;
        engine->cgbChannels[i + 1].panMask = 0x22;
        engine->cgbChannels[i + 2].type = 3;
        engine->cgbChannels[i + 2].panMask = 0x44;
        engine->cgbChannels[i + 3].type = 4;
        engine->cgbChannels[i + 3].panMask = 0x88;
    }

    /* Output stage: the GBA hardware model at the host rate reading the
     * PCM ring (SoundInit's CpuFill32 zeroes it). */
    pcm_rate_setup(engine);
    m4a_gba_init(&engine->gba, sampleRate, engine->pcmBuffer, engine->pcmDmaBufSize,
                 engine->pcmSamplesPerVBlank);
    m4a_psg_reset(&engine->psg[0]);
    m4a_psg_reset(&engine->psg[1]);
    scheduler_reset(engine);
    engine->reverb.amount = 0;
    m4a_est_init(engine);
}

void m4a_engine_destroy(M4AEngine *engine)
{
    m4a_gba_destroy(&engine->gba);
}

/* m4aSoundMode -> m4aSoundVSyncOff (DMA off, ring cleared) ->
 * SampleFreqSet (timer off, reload, DMA on, timer on one period later;
 * its wait for VCOUNT 159 is not modeled: call while stopped). */
static void pcm_reprogram(M4AEngine *engine)
{
    M4AGbaAudio *g = &engine->gba;
    /* Before the boot replay has started, the rate is m4aSoundInit's own
     * (the mode word of its m4aSoundMode call): only the numbers change,
     * and the boot table turns the DMA and the timer on at the ROM's
     * cycles.  Enabling them here would run the FIFO from cycle 0 and
     * leave a different stale state at the final timer start (measured
     * 2026-10-27 at 21024 Hz: the note's ring slot fetched 10 timer ticks
     * early in vanilla, 7 late in HQ). */
    bool live = engine->booted || engine->bootStep > 0;
    if (live)
        m4a_gba_dma_disable(g);
    memset(engine->pcmBuffer, 0, sizeof(engine->pcmBuffer));
    engine->liveMixPos = 0;
    pcm_rate_setup(engine);
    if (live)
        m4a_gba_timer_disable(g);
    m4a_gba_set_pcm_rate(g, engine->pcmSamplesPerVBlank, engine->pcmBuffer,
                         engine->pcmDmaBufSize);
    if (live) {
        m4a_gba_dma_enable(g);
        m4a_gba_timer_enable(g);
    }
}

void m4a_engine_set_live_mode(M4AEngine *engine, bool on)
{
    if (on == engine->liveMode)
        return;
    engine->liveMode = on;
    engine->liveMixPos = 0;
    frame_cost_invalidate(engine);
    if (on && engine->hqMixer)
        m4a_engine_set_hq_mixer(engine, false);
}

void m4a_engine_set_pcm_freq_mode(M4AEngine *engine, int mode)
{
    if (mode < M4A_PCM_FREQ_MODE_MIN || mode > M4A_PCM_FREQ_MODE_MAX)
        mode = M4A_PCM_FREQ_MODE_DEFAULT;
    if (engine->hqMixer && mode == 2)
        mode = M4A_PCM_FREQ_MODE_DEFAULT;   /* 7884 Hz: 0x84 samples, not a multiple of 8 */
    if (mode == engine->pcmFreqMode)
        return;
    engine->pcmFreqMode = (uint8_t)mode;
    pcm_reprogram(engine);
    frame_cost_invalidate(engine);
}

void m4a_engine_set_hq_mixer(M4AEngine *engine, bool on)
{
    if (on && engine->liveMode)
        on = false;                /* live mode mixes with the vanilla mixer only */
    /* The boot table is replayed by m4a_engine_boot; sound_main reads the
     * flag each frame.  The HQ-only state starts as the ROM's BSS -- and
     * stays as it is while the mode is unchanged: hq_buffer (the next
     * frame's reverb seed), the noise-shape residuals and CHN_SAMPLE_STOR
     * persist across frames in the ROM, so a re-application of the same
     * setting (the plugin applies its settings on every GUI change) must
     * not clear them mid-note. */
    if (on == engine->hqMixer)
        return;
    engine->hqMixer = on;
    frame_cost_invalidate(engine);
    memset(engine->hqBuffer, 0, sizeof(engine->hqBuffer));
    memset(engine->hqMutedBuffer, 0, sizeof(engine->hqMutedBuffer));
    engine->hqNoiseShapeLeft = engine->hqNoiseShapeRight = 0;
    engine->hqUnportedChannelFrames = 0;
    engine->hqFastCode = 0;
    for (int i = 0; i < TOTAL_PCM_CHANNELS; i++)
        engine->pcmChannels[i].sampleStor = 0;
    if (on && engine->pcmFreqMode == 2)
        m4a_engine_set_pcm_freq_mode(engine, M4A_PCM_FREQ_MODE_DEFAULT);
    /* The ring size follows the mixer at a non-default rate (7 frames in
     * HQ, 1584 in vanilla): reprogram as a rate change does. */
    else if (pcm_dma_buf_size(on, engine->pcmSamplesPerVBlank) != engine->pcmDmaBufSize)
        pcm_reprogram(engine);
}

void m4a_engine_set_tempo_bpm(M4AEngine *engine, double bpm)
{
    if (bpm < 1.0) bpm = 1.0;
    uint16_t tempoI = (uint16_t)(bpm + 0.5);
    /* A tempo change while a song plays is the song's TEMPO command (the
     * cost estimate; once per tick with m4a_engine_set_tick_tempo). */
    if (tempoI != engine->tempoI && m4a_est_active(engine))
        m4a_est_tempo(engine);
    engine->tempoI = tempoI;
}

void m4a_engine_start_song_tempo(M4AEngine *engine)
{
    if (engine->externalSequencer)
        return;
    /* MPlayStart: tempoD = tempoI = 150, tempoU 0x100, tempoC 0 */
    engine->tempoD = 150;
    engine->tempoU = 0x100;
    engine->tempoI = 150;
    engine->tempoC = 0;
    engine->tempoINextSet = false;
    engine->lfoTicksAhead = 0;
}

void m4a_engine_set_tick_tempo(M4AEngine *engine, uint16_t tempoI)
{
    if (engine->externalSequencer)
        return;
    engine->tempoINext = tempoI;
    engine->tempoINextSet = true;
    m4a_est_tempo(engine);    /* the TEMPO command of the coming tick */
}

void m4a_engine_set_voicegroup(M4AEngine *engine, ToneData *voiceGroup)
{
    if (voiceGroup != engine->voiceGroup) {
        frame_cost_invalidate(engine);   /* a new song's voices: a new peak */
        m4a_est_reset(engine);           /* and no song state from the old one */
    }
    engine->voiceGroup = voiceGroup;
}

/* ------------------------------------------------------------------------
 * The MIDI event latch (m4a_engine.h, quantizeEvents)
 * ------------------------------------------------------------------------ */

static void drain_events(M4AEngine *engine);

/* Queue `ev` when the latch is on and we are outside a tick.  Returns false
 * when the caller must apply it now: latch off, or inside a tick (the frame
 * hook's sequencer, a drain).  A full queue is applied first, in order, so
 * the overflowing event still follows everything that arrived before it
 * (a note-off overtaking its own queued note-on would leave the note
 * stuck); the overflow is counted. */
static bool queue_event(M4AEngine *engine, M4AQueuedEvent ev)
{
    if (!engine->quantizeEvents || engine->inTick)
        return false;
    if (engine->eventQueueCount >= M4A_EVENT_QUEUE_CAPACITY) {
        engine->eventQueueOverflow++;
        drain_events(engine);
    }
    engine->eventQueue[engine->eventQueueCount++] = ev;
    return true;
}

static void apply_event(M4AEngine *engine, const M4AQueuedEvent *ev)
{
    switch (ev->kind) {
    case M4A_EV_NOTE_ON:  (void)m4a_engine_note_on(engine, ev->track, ev->a, ev->b); break;
    case M4A_EV_NOTE_OFF: m4a_engine_note_off(engine, ev->track, ev->a); break;
    case M4A_EV_CC:       m4a_engine_cc(engine, ev->track, ev->a, ev->b); break;
    case M4A_EV_PROGRAM:  m4a_engine_program_change(engine, ev->track, ev->a); break;
    case M4A_EV_BEND:     m4a_engine_pitch_bend(engine, ev->track, ev->bend); break;
    case M4A_EV_XCMD:     m4a_engine_xcmd(engine, ev->track, ev->a, ev->b); break;
    case M4A_EV_TRACK_END: m4a_engine_track_end(engine, ev->track); break;
    case M4A_EV_TICK_BOUNDARY: m4a_engine_tick_boundary(engine); break;
    default: break;
    }
}

/* Drain the queue in arrival order.  The entry points apply at once while
 * inTick is set; outside a tick the flush sets it for the drain so a
 * re-entrant call from an applied event cannot re-queue. */
static void drain_events(M4AEngine *engine)
{
    bool wasInTick = engine->inTick;
    engine->inTick = true;
    /* An applied event never queues (inTick), so the count is stable. */
    int n = engine->eventQueueCount;
    for (int i = 0; i < n; i++)
        apply_event(engine, &engine->eventQueue[i]);
    engine->eventQueueCount = 0;
    engine->inTick = wasInTick;
}

void m4a_engine_flush_events(M4AEngine *engine)
{
    drain_events(engine);
}

void m4a_engine_set_quantize_events(M4AEngine *engine, bool enabled)
{
    if (!enabled && engine->quantizeEvents)
        drain_events(engine);        /* nothing queued is lost on the way out */
    engine->quantizeEvents = enabled;
}

void m4a_engine_apply_latch_mode(M4AEngine *engine, M4ALatchMode mode,
                                 bool wasPlaying, bool playing)
{
    bool latch = mode == M4A_LATCH_ON  ? true
               : mode == M4A_LATCH_OFF ? false
               : playing;               /* transport (and any other value) */
    m4a_engine_set_quantize_events(engine, latch);
    if (wasPlaying && !playing) {
        /* With the latch still on (ON) the flip did not flush: do it here,
         * so the reset below sees every queued note-on already applied. */
        drain_events(engine);
        m4a_engine_reset_portamento(engine);
    }
}

M4ALatchMode m4a_latch_mode_from_name(const char *name)
{
    if (!name)
        return M4A_LATCH_TRANSPORT;
    /* The config loader strips only the line ending: ignore surrounding
     * blanks, so "on " still means on. */
    while (*name == ' ' || *name == '\t')
        name++;
    size_t n = strlen(name);
    while (n > 0 && (name[n - 1] == ' ' || name[n - 1] == '\t'))
        n--;
    if ((n == 2 && strncmp(name, "on", 2) == 0) || (n == 1 && name[0] == '1'))
        return M4A_LATCH_ON;
    if ((n == 3 && strncmp(name, "off", 3) == 0) || (n == 1 && name[0] == '0'))
        return M4A_LATCH_OFF;
    return M4A_LATCH_TRANSPORT;
}

M4ALatchMode m4a_latch_mode_from_byte(uint8_t value)
{
    return value == M4A_LATCH_ON  ? M4A_LATCH_ON
         : value == M4A_LATCH_OFF ? M4A_LATCH_OFF
         : M4A_LATCH_TRANSPORT;
}

static bool choke_matches(int trackIndex, int key, int chTrack, int chKey)
{
    return (trackIndex < 0 || chTrack == trackIndex) && (key < 0 || chKey == key);
}

void m4a_engine_note_choke(M4AEngine *engine, int trackIndex, int key)
{
    if (trackIndex >= MAX_TRACKS || key > 127)
        return;

    /* The key's latched note-ons and note-offs go: the choked note must not
     * start at the next tick, and its note-off has nothing left to end. */
    int kept = 0;
    for (int i = 0; i < engine->eventQueueCount; i++) {
        const M4AQueuedEvent *ev = &engine->eventQueue[i];
        if ((ev->kind == M4A_EV_NOTE_ON || ev->kind == M4A_EV_NOTE_OFF)
            && choke_matches(trackIndex, key, ev->track, ev->a))
            continue;
        engine->eventQueue[kept++] = *ev;
    }
    engine->eventQueueCount = kept;

    for (int i = 0; i < TOTAL_PCM_CHANNELS; i++) {
        M4APCMChannel *ch = &engine->pcmChannels[i];
        if ((ch->status & CHN_ON) && choke_matches(trackIndex, key, ch->trackIndex, ch->midiKey))
            m4a_engine_kill_channel(engine, i);
    }
    for (int i = 0; i < TOTAL_CGB_CHANNELS; i++) {
        M4ACGBChannel *ch = &engine->cgbChannels[i];
        if ((ch->status & CHN_ON) && choke_matches(trackIndex, key, ch->trackIndex, ch->midiKey))
            m4a_engine_kill_channel(engine, TOTAL_PCM_CHANNELS + i);
    }
}

/*
 * Program Change - select instrument from voicegroup
 */
void m4a_engine_program_change_from(M4AEngine *engine, int trackIndex,
                                    const ToneData *voiceGroup, uint8_t program)
{
    if (!voiceGroup)
        voiceGroup = engine->voiceGroup;
    /* voiceGroup holds exactly 128 entries; a wild program must not pick a
     * voice from whatever memory follows it. */
    if (trackIndex < 0 || trackIndex >= MAX_TRACKS || !voiceGroup || program > 127)
        return;

    M4ATrack *track = &engine->tracks[trackIndex];
    track->currentProgram = program;
    track->currentVoice = voiceGroup[program];
}

void m4a_engine_program_change(M4AEngine *engine, int trackIndex, uint8_t program)
{
    if (trackIndex >= 0 && trackIndex < MAX_TRACKS
        && queue_event(engine, (M4AQueuedEvent){ M4A_EV_PROGRAM, (uint8_t)trackIndex, program, 0, 0 }))
        return;
    if (trackIndex >= 0 && trackIndex < MAX_TRACKS && m4a_est_active(engine))
        m4a_est_program(engine, trackIndex);
    m4a_engine_program_change_from(engine, trackIndex, NULL, program);
}

void m4a_engine_refresh_voices(M4AEngine *engine)
{
    if (!engine->voiceGroup)
        return;
    for (int i = 0; i < MAX_TRACKS; i++) {
        M4ATrack *track = &engine->tracks[i];
        track->currentVoice = engine->voiceGroup[track->currentProgram];
    }
}

/*
 * Record a polyphony-overflow event: bump the per-track counter and append to
 * the recent-event ring.  The ring entry is fully written before the total is
 * bumped so a concurrent GUI reader never sees a half-written event.
 */
static void record_poly_event(M4AEngine *engine, uint8_t type, uint8_t trackIndex,
                              uint8_t midiKey, uint8_t byTrack)
{
    uint8_t program = 0;
    if (trackIndex < MAX_TRACKS) {
        switch (type) {
        case M4A_POLY_DROPPED:  engine->polyDropCount[trackIndex]++;    break;
        case M4A_POLY_STOLEN:   engine->polyStealCount[trackIndex]++;   break;
        case M4A_POLY_TAIL_CUT: engine->polyTailCutCount[trackIndex]++; break;
        }
        /* The losing track's current program identifies the instrument.  For
         * stolen sounds this can in principle be stale (a program change after
         * the note started), but that's rare and fine for a debug display. */
        program = engine->tracks[trackIndex].currentProgram;
    }
    M4APolyEvent *ev = &engine->polyEvents[engine->polyEventTotal % M4A_POLY_EVENT_CAPACITY];
    ev->type = type;
    ev->trackIndex = trackIndex;
    ev->midiKey = midiKey;
    ev->byTrack = byTrack;
    ev->program = program;
    ev->tick = engine->polyEventClock;
    engine->polyEventTotal++;
}

/*
 * Allocate a PCM channel for a new note from the pool [first, first+count).
 * Matches the channel allocation logic in ply_note (m4a_1.s).  Normal notes
 * allocate from [0, maxPcmChannels); the polyphony-overflow debug mode
 * allocates lost sounds from the shadow pool with the same rules.
 */
static M4APCMChannel *allocate_pcm_channel(M4AEngine *engine, uint8_t priority,
                                            int trackIndex, int first, int count)
{
    M4APCMChannel *best = NULL;
    uint8_t bestPriority = priority;
    int bestTrackIndex = trackIndex;
    int bestIsStopping = 0;

    for (int i = first; i < first + count; i++) {
        M4APCMChannel *ch = &engine->pcmChannels[i];

        if (!(ch->status & CHN_ON)) {
            /* Free channel - use immediately */
            return ch;
        }

        if (ch->status & CHN_STOP) {
            /* Stopping channel - prefer over active ones */
            if (!bestIsStopping) {
                bestIsStopping = 1;
                bestPriority = ch->priority;
                bestTrackIndex = ch->trackIndex;
                best = ch;
            } else if (ch->priority < bestPriority) {
                bestPriority = ch->priority;
                bestTrackIndex = ch->trackIndex;
                best = ch;
            } else if (ch->priority == bestPriority && ch->trackIndex >= bestTrackIndex) {
                bestTrackIndex = ch->trackIndex;
                best = ch;
            }
            continue;
        }

        if (!bestIsStopping) {
            if (ch->priority < bestPriority) {
                bestPriority = ch->priority;
                bestTrackIndex = ch->trackIndex;
                best = ch;
            } else if (ch->priority == bestPriority && ch->trackIndex >= bestTrackIndex) {
                bestTrackIndex = ch->trackIndex;
                best = ch;
            }
        }
    }

    /* Only steal if our priority is high enough */
    if (best && (bestIsStopping || priority >= bestPriority))
        return best;

    return NULL;
}

/*
 * Polyphony-overflow debug mode: preserve a channel that is about to be
 * stolen by copying its state into the shadow pool, where it keeps playing
 * (audible only in invert mode).  Its track/key stay intact, so note-off,
 * pitch, and volume updates keep applying to the survivor.  If the shadow
 * pool itself is full, the lower-priority lost sound is simply not preserved.
 */
static void preserve_stolen_pcm(M4AEngine *engine, const M4APCMChannel *victim)
{
    M4APCMChannel *shadow = allocate_pcm_channel(engine, victim->priority,
                                                 victim->trackIndex,
                                                 MAX_PCM_CHANNELS, MAX_PCM_CHANNELS);
    if (shadow)
        *shadow = *victim;
}

static void preserve_stolen_cgb(M4AEngine *engine, const M4ACGBChannel *victim, int cgbIdx)
{
    /* One shadow slot per CGB channel type; a newer lost sound replaces an
     * older one, mirroring the mono nature of the hardware channel. */
    M4ACGBChannel *shadow = &engine->cgbChannels[MAX_CGB_CHANNELS + cgbIdx];
    *shadow = *victim;
    /* The shadow PSG never saw this note: hand it the real PSG's envelope
     * register and have the driver redo the start-frame writes. */
    static const uint32_t nrx2[4] = { PSG_REG_NR12, PSG_REG_NR22, PSG_REG_NR32, PSG_REG_NR42 };
    M4AGbaAudio *g = &engine->gba;
    m4a_gba_catch_up(g, g->now);
    m4a_psg_write8(&engine->psg[1], nrx2[cgbIdx],
                   m4a_psg_read8(&engine->psg[0], nrx2[cgbIdx]), g->now);
    shadow->currentWavePointer = NULL;
    shadow->hwResync = true;
}

/*
 * Apply an interpolated portamento key (8.8 fixed point: (key << 8) | fine) to
 * every active, non-releasing channel on the track.  The track-level pitch
 * adjustments (key shift, bend, tune, vibrato) are layered on top, matching
 * MPlayProcessPortamento in pokeemerald's m4a.c.
 */
static void apply_portamento_pitch(M4AEngine *engine, M4ATrack *track,
                                   int trackIndex, int32_t currentKey16)
{
    int32_t fullPitch = currentKey16 + ((int32_t)track->keyM << 8) + track->pitM;
    int32_t key = fullPitch >> 8;
    if (key < 0) key = 0;
    else if (key > 178) key = 178;
    uint8_t fine = (uint8_t)(fullPitch & 0xFF);

    for (int i = 0; i < TOTAL_PCM_CHANNELS; i++) {
        M4APCMChannel *ch = &engine->pcmChannels[i];
        if (!(ch->status & CHN_ON) || (ch->status & CHN_STOP)
            || ch->trackIndex != trackIndex || !ch->wav)
            continue;
        /* Fixed-frequency voices ignore the MIDI key entirely */
        if (ch->type & VOICE_TYPE_FIX)
            continue;
        ch->frequency = m4a_midi_key_to_freq(ch->wav, (uint8_t)key, fine);
    }
    for (int i = 0; i < TOTAL_CGB_CHANNELS; i++) {
        M4ACGBChannel *ch = &engine->cgbChannels[i];
        if (!(ch->status & CHN_ON) || (ch->status & CHN_STOP)
            || ch->trackIndex != trackIndex)
            continue;
        cgb_set_pitch(ch, key, fine);
    }
}

/*
 * Called after a new note has successfully started a channel.  Decides whether
 * the note begins a portamento glide from the track's previous note key.
 * Mirrors the new-note handling in MPlayProcessPortamento.  No-op unless the
 * opt-in portamento feature is enabled.
 */
static void portamento_note_started(M4AEngine *engine, M4ATrack *track,
                                    int trackIndex, uint8_t chanKey)
{
    if (!engine->portamentoEnabled)
        return;

    /* Note that portamentoPrevKey is intentionally NOT updated here when a new
     * note interrupts an in-progress glide.  It still holds the glide's
     * original start key, so the next glide restarts from the previous note's
     * pitch (snapping back if the glide hadn't finished) -- matching
     * MPlayProcessPortamento on the GBA, which only advances portamentoPrevKey
     * once a glide completes. */
    track->portamentoElapsed = 0;
    if (track->portamentoDuration != 0 && track->portamentoPrevKey != 0
        && track->portamentoPrevKey != chanKey) {
        track->portamentoGliding = true;
        track->portamentoTargetKey = chanKey;
        /* Pitch the new note to the glide start key right away so it never
         * sounds at the target pitch before the first engine tick.  (On the
         * GBA, MPlayProcessPortamento runs before any audio is mixed.) */
        apply_portamento_pitch(engine, track, trackIndex,
                               (int32_t)track->portamentoPrevKey << 8);
    } else {
        /* Always remember the last note played -- even while portamento is
         * off or when the same key repeats -- so a later glide starts from
         * the actual previous note regardless of whether its channel was
         * still alive when CC 5 arrived. */
        track->portamentoPrevKey = chanKey;
        track->portamentoGliding = false;
    }
}

/*
 * Note On
 */
/* ply_note, after a channel has been taken: the LFO delay restarts for the
 * new note (lfoDelayC = lfoDelay) and, when a delay is set, clear_modM zeroes
 * the LFO output so the note starts unmodulated.  Returns true when keyM /
 * pitM changed and note keys must be recomputed. */
static bool note_on_restart_lfo_delay(M4ATrack *track)
{
    track->lfoDelayC = track->lfoDelay;
    if (track->lfoDelay == 0)
        return false;
    /* clear_modM only flags the track; ply_note then drops the flags
     * (flags &= 0xF0, note_took_channel), so older channels keep their
     * LFO-offset pitch and any pending VOL/PAN/BEND until the next change.
     * Only the new note's keyM/pitM see the recentred LFO. */
    track->modM = 0;
    track->lfoSpeedC = 0;
    m4a_track_vol_pit_set(track);
    return true;
}

/* The end of ply_note for a note that took a channel: TrkVolPitSet ran for
 * it, and the track's pending VOLCHG / PITCHG are dropped (flags &= 0xF0),
 * so the channels it already had keep their values until the next change.
 * A rejected note (_081DDCEA) leaves the flags alone.  A note brings a
 * finished track back. */
static void note_took_channel(M4ATrack *track)
{
    track->chgFlags = 0;
    track->finished = false;
}

/* Final keys for a note: the played key plus the track's keyM transposition,
 * with the PCM variants used by the base-MIDI-key opt-in. */
static void note_on_keys(const M4AEngine *engine, const M4ATrack *track, uint8_t useKey,
                         int32_t pcmBaseAdjust, int32_t *finalKey, int32_t *pcmKey,
                         int32_t *pcmFinalKey)
{
    /* ply_note clamps key + keyM at 0 only; MidiKeyToFreq saturates at 178
     * and MidiKeyToCgbFreq at 166, the same way the per-frame pitch pass
     * (refresh_channel_pitches) computes it, so a note does not jump at its
     * first pitch update. */
    (void)engine;
    int32_t fk = (int32_t)useKey + track->keyM;
    if (fk < 0) fk = 0;
    if (fk > 178) fk = 178;
    int32_t pk = (int32_t)useKey + pcmBaseAdjust;
    if (pk < 0) pk = 0;
    if (pk > 255) pk = 255;
    int32_t pfk = pk + track->keyM;
    if (pfk < 0) pfk = 0;
    if (pfk > 178) pfk = 178;
    *finalKey = fk;
    *pcmKey = pk;
    *pcmFinalKey = pfk;
}

const ToneData *m4a_engine_resolve_voice(const ToneData *voice, uint8_t key)
{
    return resolve_voice((ToneData *)voice, key);
}

int m4a_engine_note_on(M4AEngine *engine, int trackIndex, uint8_t key, uint8_t velocity)
{
    /* Keysplit/rhythm voices index 128-entry tables by key. */
    if (trackIndex < 0 || trackIndex >= MAX_TRACKS || key > 127)
        return M4A_CHANNEL_NONE;
    if (queue_event(engine, (M4AQueuedEvent){ M4A_EV_NOTE_ON, (uint8_t)trackIndex, key, velocity, 0 }))
        return M4A_CHANNEL_QUEUED;

    M4ATrack *track = &engine->tracks[trackIndex];
    ToneData *voice = resolve_voice(&track->currentVoice, key);
    if (!voice) {
        if (m4a_est_active(engine))
            m4a_est_note_on(engine, trackIndex, key, velocity, NULL);
        return M4A_CHANNEL_NONE;
    }

    uint8_t voiceType = voice->type & VOICE_TYPE_CGB_MASK;
    int8_t rhythmPan = 0;
    uint8_t useKey = key;
    int32_t pcmBaseAdjust = 0;

    /* For rhythm (keysplit_all) voices: the MIDI note selects which drum voice
     * to play, but the playback pitch is fixed to the drum voice's own key --
     * not the note the player pressed.  Apply per-note pan while we're here. */
    if (track->currentVoice.type & VOICE_KEYSPLIT_ALL) {
        useKey = voice->key;
        if (voice->panSweep & 0x80) {
            rhythmPan = (int8_t)((voice->panSweep - 0xC0) * 2);
        }
    } else if (voiceType == 0 && engine->respectBaseMidiKey) {
        /* Opt-in: PCM voice_directsound (non-rhythm).  Treat voice->key as the
         * sample's base MIDI note.  wav->freq is calibrated so MIDI 60 plays at
         * the sample's natural rate, so shift by (60 - voice->key) to make the
         * sample play at the correct pitch when the player presses any note. */
        pcmBaseAdjust = 60 - (int32_t)voice->key;
    }

    /* Calculate combined priority */
    uint8_t combinedPriority = track->priority;

    /* Calculate track volumes */
    m4a_track_vol_pit_set(track);
    /* MPlayMain's cost for this note (no sequencer): before the channel is
     * taken, from the channels as ply_note finds them. */
    if (m4a_est_active(engine))
        m4a_est_note_on(engine, trackIndex, key, velocity, voice);

    /* Calculate final key with transposition.  pcmBaseAdjust (non-zero only when
     * the base-MIDI-key opt-in is on) is folded into the PCM key/clamp; with the
     * feature off pcmBaseAdjust is 0 and pcmKey/pcmFinalKey collapse to the
     * original useKey/finalKey values. */
    int32_t finalKey, pcmKey, pcmFinalKey;
    note_on_keys(engine, track, useKey, pcmBaseAdjust, &finalKey, &pcmKey, &pcmFinalKey);

    if (voiceType >= 1 && voiceType <= 4) {
        /* CGB channel */
        int cgbIdx = voiceType - 1;
        M4ACGBChannel *ch = &engine->cgbChannels[cgbIdx];
        bool shadow = false;

        /* Check if we can steal this channel */
        if ((ch->status & CHN_ON) && !(ch->status & CHN_STOP)
            && (ch->priority > combinedPriority
                || (ch->priority == combinedPriority && ch->trackIndex < trackIndex))) {
            /* Can't steal: the note is lost to the polyphony limit.  In the
             * overflow-debug invert mode it plays on the shadow channel of
             * the same type instead; otherwise it's dropped. */
            record_poly_event(engine, M4A_POLY_DROPPED, (uint8_t)trackIndex, key,
                              (uint8_t)trackIndex);
            if (!engine->polyDebugInvert)
                return M4A_CHANNEL_NONE;
            ch = &engine->cgbChannels[MAX_CGB_CHANNELS + cgbIdx];
            shadow = true;
        } else if ((ch->status & CHN_ON) && ch->trackIndex != trackIndex) {
            /* Taking the channel from another track cuts that track's sound
             * short.  Same-track retriggers are ordinary mono behavior on a
             * CGB channel, not polyphony pressure, so they aren't recorded. */
            record_poly_event(engine,
                              (ch->status & CHN_STOP) ? M4A_POLY_TAIL_CUT : M4A_POLY_STOLEN,
                              (uint8_t)ch->trackIndex, ch->midiKey, (uint8_t)trackIndex);
            if (engine->polyDebugInvert)
                preserve_stolen_cgb(engine, ch, cgbIdx);
        }

        /* A dropped note never got this far on the GBA. */
        if (!shadow && note_on_restart_lfo_delay(track))
            note_on_keys(engine, track, useKey, pcmBaseAdjust, &finalKey, &pcmKey, &pcmFinalKey);

        /* Portamento legato: CGB tone types share one channel slot, so a
         * non-zero envelope volume means the previous note was still audible
         * at the moment of retrigger (zero gap).  In that case skip the note
         * trigger entirely -- keep the oscillator phase and envelope and put
         * the channel into sustain so it carries the previous note's state. */
        bool portamentoInherit = !shadow
                              && engine->portamentoEnabled
                              && track->portamentoDuration != 0
                              && (ch->status & CHN_ON)
                              && ch->envelopeVolume != 0;

        ch->midiKey = key;
        ch->key = useKey;
        ch->velocity = velocity;
        ch->priority = combinedPriority;
        ch->trackIndex = trackIndex;
        ch->audition = engine->auditionNote;
        ch->auditionVolume = m4a_audition_volume(engine);
        ch->auditionPan = m4a_audition_pan(engine);
        ch->rhythmPan = rhythmPan;
        ch->attack = voice->attack;
        ch->decay = voice->decay;
        ch->sustain = voice->sustain;
        ch->release = voice->release;
        ch->pseudoEchoVolume = track->pseudoEchoVolume;
        ch->pseudoEchoLength = track->pseudoEchoLength;
        ch->length = voice->length;
        ch->gateTime = 0;
        /* `_alt` PSG voices; inert on noise -- see m4a_cgb_fix_freq(). */
        ch->fixedFreq = (voice->type & VOICE_TYPE_FIX) && voiceType != 4;

        cgb_chn_vol_set(ch, track);   /* CgbModVol runs in CgbSound at the start frame */

        if (voiceType == 1 || voiceType == 2) {
            ch->dutyCycle = (uint8_t)(uintptr_t)voice->wavePointer & 0x03;
            if (voiceType == 1) {
                /* panSweep is an NR10 sweep value only when the pan bit is
                 * clear AND the sweep-time bits are nonzero; otherwise the
                 * hardware gets the inert value 8 (ply_note in m4a_1.s). */
                if ((voice->panSweep & 0x80) || !(voice->panSweep & 0x70))
                    ch->sweep = 0x08;
                else
                    ch->sweep = voice->panSweep;
            }

            /* Pulse-width modulation (opt-in): if the effect is active on this
             * track, start the note on the pattern's first duty cycle and reset
             * the pattern position.  Mirrors the new-note (SF_START) branch of
             * MPlayProcessPulseWidthMod on the GBA. */
            if (engine->pwmEnabled && track->pwmSpeed != 0 && track->pwmPattern != 0) {
                const PulseWidthModPattern *p = &gPulseWidthModPatterns[track->pwmPattern];
                if (p->numSteps != 0) {
                    ch->dutyCycle = p->duty[0];
                    /* A dropped (shadow) note never started on the GBA, so it
                     * must not restart the track's modulation pattern. */
                    if (!shadow) {
                        track->pwmStep = 0;
                        track->pwmSpeedCounter = track->pwmSpeed;
                    }
                }
            }
        } else if (voiceType == 3) {
            ch->wavePointer = voice->wavePointer;
        }

        /* Calculate frequency (before m4a_cgb_channel_start latches it into
         * sweepShadowFreq, so the sweep starts from the FIX-rounded value). */
        ch->frequency = cgb_pitch_freq(ch, finalKey, track->pitM);
        /* Noise channel: apply period bit (NR43 bit 3) from wavePointer.
         * period=0 → 15-bit LFSR, period=1 → 7-bit short-period LFSR. */
        if (voiceType == 4)
            ch->frequency |= ((uintptr_t)voice->wavePointer & 0x01) << 3;

        if (portamentoInherit) {
            ch->status = CHN_ENV_SUSTAIN;
            /* A legato continuation must not re-arm the hardware envelope
             * and sweep (MO_VOL); only the new frequency has to reach
             * NRx3/NRx4. */
            ch->modify = 0x02;
        } else {
            m4a_cgb_channel_start(ch);
        }

        /* A dropped note never sounded on the GBA, so it must not become the
         * start key of a later portamento glide. */
        if (!shadow) {
            portamento_note_started(engine, track, trackIndex, ch->key);
            note_took_channel(track);
        }
        return (int)(TOTAL_PCM_CHANNELS + (ch - engine->cgbChannels));
    } else {
        /* PCM DirectSound channel */
        if (!voice->wav) return M4A_CHANNEL_NONE;

        bool shadow = false;
        M4APCMChannel *ch = allocate_pcm_channel(engine, combinedPriority, trackIndex,
                                                 0, engine->maxPcmChannels);
        if (ch && (ch->status & CHN_ON)) {
            /* Reusing an occupied channel cuts its current sound short. */
            record_poly_event(engine,
                              (ch->status & CHN_STOP) ? M4A_POLY_TAIL_CUT : M4A_POLY_STOLEN,
                              (uint8_t)ch->trackIndex, ch->midiKey, (uint8_t)trackIndex);
            if (engine->polyDebugInvert)
                preserve_stolen_pcm(engine, ch);
        } else if (!ch) {
            /* No channel could be taken: the note is lost to the polyphony
             * limit.  In the overflow-debug invert mode it plays on a shadow
             * channel instead; otherwise it's dropped. */
            record_poly_event(engine, M4A_POLY_DROPPED, (uint8_t)trackIndex, key,
                              (uint8_t)trackIndex);
            if (!engine->polyDebugInvert)
                return M4A_CHANNEL_NONE;
            ch = allocate_pcm_channel(engine, combinedPriority, trackIndex,
                                      MAX_PCM_CHANNELS, MAX_PCM_CHANNELS);
            if (!ch) return M4A_CHANNEL_NONE;
            shadow = true;
        }

        /* A dropped note never got this far on the GBA. */
        if (!shadow && note_on_restart_lfo_delay(track))
            note_on_keys(engine, track, useKey, pcmBaseAdjust, &finalKey, &pcmKey, &pcmFinalKey);

        ch->midiKey = key;
        ch->key = (uint8_t)pcmKey;
        ch->velocity = velocity;
        ch->priority = combinedPriority;
        ch->trackIndex = trackIndex;
        ch->audition = engine->auditionNote;
        ch->auditionVolume = m4a_audition_volume(engine);
        ch->auditionPan = m4a_audition_pan(engine);
        ch->rhythmPan = rhythmPan;
        ch->attack = voice->attack;
        ch->decay = voice->decay;
        ch->sustain = voice->sustain;
        ch->release = voice->release;
        ch->pseudoEchoVolume = track->pseudoEchoVolume;
        ch->pseudoEchoLength = track->pseudoEchoLength;
        ch->gateTime = 0;

        chn_vol_set(ch, track);

        /* ply_note: MidiKeyToFreq (the mixer ignores it for fixed-frequency
         * voices, which play one source sample per mixed sample). */
        ch->frequency = m4a_midi_key_to_freq(voice->wav, (uint8_t)pcmFinalKey, track->pitM);

        m4a_pcm_channel_start_at(ch, voice->wav, voice->type, track->sampleStart);

        /* Portamento legato (opt-in): if the previous note on this track is
         * still in any envelope phase (zero gap between notes), the new note
         * inherits its sample position, envelope, and loop state instead of
         * triggering a fresh attack/sample start, making the glide perfectly
         * smooth.  The previous channel is silenced so the two don't
         * double-voice.  Disallowed if the voice changed (different wav),
         * or if only one of the two plays its (shared) wav in reverse: the
         * reverse mixer keeps currentPointer one past the current sample
         * (data + count) while the forward mixer keeps it AT the sample, so
         * handing a reverse channel's position to a forward one would start
         * it at data + count and read `count` samples past the end of the
         * data.  A shadow (dropped) note must not inherit -- or silence --
         * a real channel's state. */
        if (!shadow && engine->portamentoEnabled && track->portamentoDuration != 0) {
            for (int i = 0; i < engine->maxPcmChannels; i++) {
                M4APCMChannel *prev = &engine->pcmChannels[i];
                if (prev == ch || !(prev->status & CHN_ON)
                    || prev->trackIndex != trackIndex || prev->wav != voice->wav
                    || ((prev->type ^ voice->type) & VOICE_TYPE_REV))
                    continue;
                ch->currentPointer = prev->currentPointer;
                ch->count = prev->count;
                ch->fw = prev->fw;
                ch->synthPulseDuty = prev->synthPulseDuty;
                /* The mixer's per-channel state goes with the position: the
                 * type bits the channel init added (CMP from the WaveData,
                 * SPL for a synth -- the new channel never runs its START
                 * init, so it would take the uncompressed path with an
                 * index-form pointer) and CHN_SAMPLE_STOR, the HQ mixer's
                 * interpolation base. */
                ch->type = prev->type;
                ch->sampleStor = prev->sampleStor;
                uint8_t prevVolume = prev->envelopeVolume;
                ch->envelopeVolume = (prevVolume > ch->sustain) ? prevVolume : ch->sustain;
                ch->status = CHN_ENV_SUSTAIN | (prev->status & (CHN_LOOP | CHN_SPECIAL));
                ch->xpi = prev->xpi;
                prev->status = 0;
                break;
            }
        }

        /* Compute initial envelope volumes so the channel produces sound
         * before the first engine tick (~60Hz). On the GBA, SoundMainRAM
         * handles this every frame, but our render loop runs at DAW rate. */
        {
            uint32_t vol = ((uint32_t)(engine->masterVolume + 1) * ch->envelopeVolume) >> 4;
            ch->envelopeVolumeRight = ((uint32_t)ch->rightVolume * vol) >> 8;
            ch->envelopeVolumeLeft = ((uint32_t)ch->leftVolume * vol) >> 8;
        }

        /* A dropped note never sounded on the GBA, so it must not become the
         * start key of a later portamento glide. */
        if (!shadow) {
            portamento_note_started(engine, track, trackIndex, ch->key);
            note_took_channel(track);
        }
        return (int)(ch - engine->pcmChannels);
    }
}

/*
 * Note Off - transition matching channels to release
 */
void m4a_engine_note_off(M4AEngine *engine, int trackIndex, uint8_t key)
{
    /* No note-on ever registers a key above 127 (see m4a_engine_note_on). */
    if (trackIndex < 0 || trackIndex >= MAX_TRACKS || key > 127)
        return;
    if (queue_event(engine, (M4AQueuedEvent){ M4A_EV_NOTE_OFF, (uint8_t)trackIndex, key, 0, 0 }))
        return;
    if (m4a_est_active(engine))
        m4a_est_note_off(engine, trackIndex, key);

    /* With the latch on, a note-off reaching a channel that is still START
     * was latched into the same tick as its note-on: a note shorter than a
     * frame.  SoundMainRAM would kill START | STOP silently; the GBA plays
     * such a note (mid2agb's one-clock minimum) for one tick, its gate
     * expiring in the next tick's chain walk -- so hand it a gate of 1 and
     * let m4a_engine_tick's gate walk set STOP next frame. */
    bool oneTick = engine->quantizeEvents;

    /* Stop matching PCM channels (shadow channels release too, so a lost
     * sound audible in the overflow-debug mode ends at its note-off) */
    for (int i = 0; i < TOTAL_PCM_CHANNELS; i++) {
        M4APCMChannel *ch = &engine->pcmChannels[i];
        if ((ch->status & CHN_ON) && !(ch->status & CHN_STOP)
            && ch->trackIndex == trackIndex && ch->midiKey == key) {
            if (oneTick && (ch->status & CHN_START))
                ch->gateTime = 1;
            else
                ch->status |= CHN_STOP;
        }
    }

    /* Stop matching CGB channels */
    for (int i = 0; i < TOTAL_CGB_CHANNELS; i++) {
        M4ACGBChannel *ch = &engine->cgbChannels[i];
        if ((ch->status & CHN_ON) && !(ch->status & CHN_STOP)
            && ch->trackIndex == trackIndex && ch->midiKey == key) {
            if (oneTick && (ch->status & CHN_START))
                ch->gateTime = 1;
            else
                ch->status |= CHN_STOP;
        }
    }
}

/*
 * Push the track's (already computed) keyM / pitM into the frequency of every
 * channel it owns: the PITCHG half of MPlayMain's pass (track_change_pass, and
 * the sequencer's m4a_engine_track_apply).
 */
static void refresh_channel_pitches(M4AEngine *engine, M4ATrack *track, int trackIndex)
{
    for (int i = 0; i < TOTAL_PCM_CHANNELS; i++) {
        M4APCMChannel *ch = &engine->pcmChannels[i];
        /* Fixed-rate (no-resample) voices advance one sample per mix sample
         * on the GBA regardless of the frequency MPlayMain writes. */
        if ((ch->status & CHN_ON) && ch->trackIndex == trackIndex && ch->wav
            && !(ch->type & VOICE_TYPE_FIX)) {
            int32_t finalKey = (int32_t)ch->key + track->keyM;
            if (finalKey < 0) finalKey = 0;
            ch->frequency = m4a_midi_key_to_freq(ch->wav, (uint8_t)finalKey, track->pitM);
        }
    }
    for (int i = 0; i < TOTAL_CGB_CHANNELS; i++) {
        M4ACGBChannel *ch = &engine->cgbChannels[i];
        if ((ch->status & CHN_ON) && ch->trackIndex == trackIndex)
            cgb_set_pitch(ch, (int32_t)ch->key + track->keyM, track->pitM);
    }
}

/* Push the track's (already computed) volMR/volML into its channels: the
 * ChnVolSetAsm half of MPlayMain's VOLCHG pass. */
static void apply_track_volumes(M4AEngine *engine, M4ATrack *track, int trackIndex)
{
    for (int i = 0; i < TOTAL_PCM_CHANNELS; i++) {
        M4APCMChannel *ch = &engine->pcmChannels[i];
        if ((ch->status & CHN_ON) && ch->trackIndex == trackIndex)
            chn_vol_set(ch, track);
    }
    for (int i = 0; i < TOTAL_CGB_CHANNELS; i++) {
        M4ACGBChannel *ch = &engine->cgbChannels[i];
        if ((ch->status & CHN_ON) && ch->trackIndex == trackIndex) {
            cgb_chn_vol_set(ch, track);
            /* MPlayMain's VOLCHG pass is ChnVolSetAsm + MO_VOL only; the pan
             * routing and envelope goal (CgbModVol) are recomputed by CgbSound
             * at the next envelope step, not before. */
            ch->modify |= 0x01;
        }
    }
}

static inline void refresh_volumes(M4AEngine *engine, M4ATrack *track, int trackIndex)
{
    m4a_track_vol_pit_set(track);
    apply_track_volumes(engine, track, trackIndex);
}

/* MPlayMain's VOLCHG / PITCHG pass (_081DD9C8 .. _081DDA62) for the MIDI
 * path: after the frame's ticks, each flagged track's TrkVolPitSet, then
 * ChnVolSetAsm (VOLCHG) and the frequency (PITCHG) on every channel it
 * still owns, releasing ones included (docs/ACCURACY.md, "Rules for every source"). */
static void track_change_pass(M4AEngine *engine)
{
    for (int i = 0; i < MAX_TRACKS; i++) {
        M4ATrack *track = &engine->tracks[i];
        uint8_t chg = track->chgFlags;
        if (!chg)
            continue;
        track->chgFlags = 0;
        m4a_track_vol_pit_set(track);
        if (chg & M4A_TRK_VOLCHG)
            apply_track_volumes(engine, track, i);
        if (chg & M4A_TRK_PITCHG)
            refresh_channel_pitches(engine, track, i);
    }
}

void m4a_engine_track_apply(M4AEngine *engine, int trackIndex, bool volume, bool pitch)
{
    if (trackIndex < 0 || trackIndex >= MAX_TRACKS)
        return;
    M4ATrack *track = &engine->tracks[trackIndex];
    m4a_track_vol_pit_set(track);
    if (volume)
        apply_track_volumes(engine, track, trackIndex);
    if (pitch)
        refresh_channel_pitches(engine, track, trackIndex);
}

/* Mirrors the GBA's clear_modM: zero the LFO accumulator and flag the
 * recentered pitch (or volume, for modT 1/2) for the frame's pass, which
 * reaches every channel of the track unless a note drops the flag first. */
static void clear_mod_m(M4ATrack *track)
{
    track->lfoSpeedC = 0;
    track->modM = 0;
    track->chgFlags |= track->modT == 0 ? M4A_TRK_PITCHG : M4A_TRK_VOLCHG;
}

/* Return the active CGB square channel (sq1/sq2) currently owned by a track,
 * or NULL if the track isn't driving a square channel.  Pulse-width modulation
 * only affects the two square-wave channels. */
static M4ACGBChannel *find_track_square_channel(M4AEngine *engine, int trackIndex)
{
    for (int i = 0; i < MAX_CGB_CHANNELS; i++) {
        M4ACGBChannel *ch = &engine->cgbChannels[i];
        if ((ch->type == 1 || ch->type == 2)
            && (ch->status & CHN_ON)
            && ch->trackIndex == trackIndex)
            return ch;
    }
    return NULL;
}

/*
 * Control Change
 */
void m4a_engine_cc(M4AEngine *engine, int trackIndex, uint8_t cc, uint8_t value)
{
    if (trackIndex < 0 || trackIndex >= MAX_TRACKS)
        return;
    if (queue_event(engine, (M4AQueuedEvent){ M4A_EV_CC, (uint8_t)trackIndex, cc, value, 0 }))
        return;
    if (m4a_est_active(engine))
        m4a_est_cc(engine, trackIndex, cc, value);

    M4ATrack *track = &engine->tracks[trackIndex];

    switch (cc) {
    case 0x1:  /* Mod wheel -> LFO depth */
        track->mod = value;
        if (value == 0)
            clear_mod_m(track);
        break;
    case 0x5:  /* Portamento time (PORTAMENTO) -- glide duration in song ticks, 0 = off.
                * Opt-in: ignored unless the portamento feature is enabled.  Unlike
                * the GBA's ply_portamento, no channel-state capture is needed here:
                * every note records its key at note-on, so the glide start key is
                * always known regardless of envelope state. */
        if (engine->portamentoEnabled)
            track->portamentoDuration = value;
        break;
    case 0x7:  /* Volume */
        track->rawVolume = value;
        track->volume = value * engine->songMasterVolume / MAX_SONG_VOLUME;
        track->chgFlags |= M4A_TRK_VOLCHG;
        break;
    case 0xA: /* Pan */
        track->pan = (int8_t)(value - 64);
        track->chgFlags |= M4A_TRK_VOLCHG;
        break;
    case 0xC:
    case 0xD:
    case 0xE:
    case 0xF:
    case 0x10:
        /* MEMACC-related -- we don't care about these. */
        break;
    case 0x11:
        /* Label command --we don't care about these. */
        break;
    case 0x14: /* Bend range (BENDR) */
        track->bendRange = value;
        track->chgFlags |= M4A_TRK_PITCHG;
        break;
    case 0x15: /* LFO speed (LFOS) */
        /* Mirrors ply_lfos: only a speed of 0 resets the LFO (clear_modM).
         * Any other value just changes the rate, keeping the phase and the
         * current offset, so a stream of LFOS changes (a knob, or a song
         * repeating the command) doesn't restart the vibrato each time. */
        track->lfoSpeed = value;
        if (value == 0)
            clear_mod_m(track);
        break;
    case 0x16: /* Modulation type (MODT): 0 vibrato, 1 tremolo, 2 auto-pan.
                * Mirrors ply_modt: a change re-evaluates both volume and pitch
                * (the LFO output moves from one to the other). */
        if (track->modT != value) {
            track->modT = value;
            track->chgFlags |= M4A_TRK_VOLCHG | M4A_TRK_PITCHG;
        }
        break;
    case 0x17: { /* Pulse-width mod duty-cycle pattern (PWMC); 0 = disable.
                  * Opt-in: ignored unless the PWM feature is enabled.  Mirrors
                  * ply_pwmc on the GBA. */
        if (!engine->pwmEnabled)
            break;
        uint8_t pattern = value;
        if (pattern >= gNumPulseWidthModPatterns)
            pattern = 0;
        track->pwmPattern = pattern;
        track->pwmStep = 0;
        track->pwmSpeedCounter = track->pwmSpeed;
        /* If the speed was already set, this command re-enables the effect on
         * its own, so mark the engine active again.  Without this, the sequence
         * "PWMS n ... PWMC 0 ... PWMC m" would leave the effect permanently
         * dead: m4a_pwm_tick clears pwmActiveFlag while all patterns are 0,
         * and only the PWMS handler would ever set it again.  Mirrors the
         * matching fix in ply_pwmc on the GBA. */
        if (pattern != 0 && track->pwmSpeed != 0)
            engine->pwmActiveFlag = true;
        break;
    }
    case 0x19: /* Pulse-width mod speed (PWMS), VBlank frames per step; 0 = off.
                * Opt-in: ignored unless the PWM feature is enabled.  Mirrors
                * ply_pwms on the GBA. */
        if (!engine->pwmEnabled)
            break;
        if (value > 0) {
            /* Only restart the pattern when the effect turns off->on, so the
             * speed can be modulated smoothly while the effect is running. */
            if (track->pwmSpeed == 0) {
                track->pwmStep = 0;
                track->pwmSpeedCounter = value;
            } else if (track->pwmSpeedCounter > value) {
                track->pwmSpeedCounter = value;
            }
            track->pwmSpeed = value;
            engine->pwmActiveFlag = true;
        } else {
            /* Disable the effect and restore the voice's default duty cycle. */
            track->pwmSpeed = 0;
            track->pwmSpeedCounter = 0;
            track->pwmStep = 0;
            uint8_t voiceType = track->currentVoice.type & VOICE_TYPE_CGB_MASK;
            if (voiceType == 1 || voiceType == 2) {
                M4ACGBChannel *ch = find_track_square_channel(engine, trackIndex);
                if (ch != NULL) {
                    ch->dutyCycle = (uint8_t)(uintptr_t)track->currentVoice.wavePointer & 0x03;
                    ch->modify |= 0x04;
                }
            }
        }
        break;
    case 0x18: /* Micro tuning (TUNE), c_v-centred like PAN/BEND: mirrors ply_tune. */
        track->tune = (int8_t)(value - 64);
        track->chgFlags |= M4A_TRK_PITCHG;
        break;
    case 0x1A: /* LFO delay (LFODL) in song ticks: mirrors ply_lfodl.  Takes
                * effect at the next note start, which reloads lfoDelayC. */
        track->lfoDelay = value;
        break;
    case 0x1E: /* XCMD select: stashed until CC 0x1D/0x1F fires it (mid2agb's
                * s_extendedCommand). */
        track->xcmdSelect = value;
        break;
    case 0x1D: /* XCMD fire */
    case 0x1F:
        m4a_engine_xcmd(engine, trackIndex, track->xcmdSelect, value);
        break;
    case 0x7B: /* All Notes Off */
        m4a_engine_all_notes_off(engine, trackIndex);
        break;
    case 0x78: /* All Sound Off */
        m4a_engine_all_sound_off(engine);
        break;
    default:
        break;
    }
}

/*
 * Extended command (XCMD).  Only the two mid2agb can emit are handled; like
 * ply_xiecv / ply_xiecl they only write the track, so notes already sounding
 * keep the values they latched at note-on.
 */
void m4a_engine_xcmd(M4AEngine *engine, int trackIndex, uint8_t command, uint8_t value)
{
    if (trackIndex < 0 || trackIndex >= MAX_TRACKS)
        return;
    if (queue_event(engine, (M4AQueuedEvent){ M4A_EV_XCMD, (uint8_t)trackIndex, command, value, 0 }))
        return;
    if (m4a_est_active(engine))
        m4a_est_xcmd(engine, trackIndex, command);

    M4ATrack *track = &engine->tracks[trackIndex];

    switch (command) {
    case M4A_XCMD_IECV:
        track->pseudoEchoVolume = value;
        break;
    case M4A_XCMD_IECL:
        track->pseudoEchoLength = value;
        break;
    default:
        break;
    }
}

/*
 * Pitch Bend (14-bit, -8192 to +8191)
 */
void m4a_engine_pitch_bend(M4AEngine *engine, int trackIndex, int16_t bend)
{
    if (trackIndex < 0 || trackIndex >= MAX_TRACKS)
        return;
    if (queue_event(engine, (M4AQueuedEvent){ M4A_EV_BEND, (uint8_t)trackIndex, 0, 0, bend }))
        return;
    if (m4a_est_active(engine))
        m4a_est_bend(engine, trackIndex);

    M4ATrack *track = &engine->tracks[trackIndex];

    /* Scale 14-bit MIDI bend to m4a's -64..+63 range */
    track->bend = (int8_t)(bend >> 7);

    /* ply_bend: PITCHG, for the frame's pass (track_change_pass). */
    track->chgFlags |= M4A_TRK_PITCHG;
}

void m4a_engine_track_end(M4AEngine *engine, int trackIndex)
{
    if (trackIndex < 0 || trackIndex >= MAX_TRACKS)
        return;
    if (queue_event(engine, (M4AQueuedEvent){ M4A_EV_TRACK_END, (uint8_t)trackIndex, 0, 0, 0 }))
        return;
    if (m4a_est_active(engine))
        m4a_est_track_end(engine, trackIndex);

    /* ply_fine_loop: SF_STOP on each chained channel that is on, then
     * RealClearChain (channel->track = 0): it releases owned by no track,
     * so no later VOLCHG / PITCHG pass or LFO step of this track reaches
     * it.  ply_fine_done: the track's flags = 0 (no LFO step, no pass). */
    for (int i = 0; i < TOTAL_PCM_CHANNELS; i++) {
        M4APCMChannel *ch = &engine->pcmChannels[i];
        if ((ch->status & CHN_ON) && ch->trackIndex == trackIndex) {
            ch->status |= CHN_STOP;
            ch->trackIndex = M4A_NO_TRACK;
        }
    }
    for (int i = 0; i < TOTAL_CGB_CHANNELS; i++) {
        M4ACGBChannel *ch = &engine->cgbChannels[i];
        if ((ch->status & CHN_ON) && ch->trackIndex == trackIndex) {
            ch->status |= CHN_STOP;
            ch->trackIndex = M4A_NO_TRACK;
        }
    }
    M4ATrack *track = &engine->tracks[trackIndex];
    track->chgFlags = 0;
    track->finished = true;
}

/*
 * Forget a track's portamento note history.  Called when notes are cut off
 * out-of-band (DAW transport stop/reset, All Notes Off / All Sound Off) so
 * that the first note after playback resumes doesn't glide from a note that
 * was sounding before the interruption.  portamentoDuration is kept: it's a
 * parameter (CC 5), not note state.
 */
static void reset_portamento_note_state(M4AEngine *engine, int trackIndex)
{
    M4ATrack *track = &engine->tracks[trackIndex];
    /* A glide in progress ends at its target: its note keeps sounding (the
     * transport-stop edge flushes the latch's queued note-ons first) and
     * must not stay at the interpolated pitch once the glide is forgotten. */
    if (track->portamentoGliding)
        apply_portamento_pitch(engine, track, trackIndex,
                               (int32_t)track->portamentoTargetKey << 8);
    track->portamentoPrevKey = 0;
    track->portamentoTargetKey = 0;
    track->portamentoGliding = false;
    track->portamentoElapsed = 0;
}

/*
 * All Notes Off for a channel
 */
void m4a_engine_all_notes_off(M4AEngine *engine, int trackIndex)
{
    if (trackIndex < 0 || trackIndex >= MAX_TRACKS)
        return;
    for (int i = 0; i < TOTAL_PCM_CHANNELS; i++) {
        M4APCMChannel *ch = &engine->pcmChannels[i];
        if ((ch->status & CHN_ON) && ch->trackIndex == trackIndex)
            ch->status |= CHN_STOP;
    }
    for (int i = 0; i < TOTAL_CGB_CHANNELS; i++) {
        M4ACGBChannel *ch = &engine->cgbChannels[i];
        if ((ch->status & CHN_ON) && ch->trackIndex == trackIndex)
            ch->status |= CHN_STOP;
    }
    reset_portamento_note_state(engine, trackIndex);
}

/*
 * All Sound Off - immediately silence everything
 */
void m4a_engine_all_sound_off(M4AEngine *engine)
{
    if (!engine->inTick)
        engine->eventQueueCount = 0;    /* a panic drops what the latch still holds */
    for (int i = 0; i < TOTAL_PCM_CHANNELS; i++)
        engine->pcmChannels[i].status = 0;
    for (int i = 0; i < TOTAL_CGB_CHANNELS; i++)
        if (engine->cgbChannels[i].status)
            m4a_engine_kill_channel(engine, TOTAL_PCM_CHANNELS + i);
    m4a_engine_reset_portamento(engine);
    if (m4a_est_active(engine))
        m4a_est_sound_off(engine);       /* the cost estimate's chains (and an inferred song) end too */
}

void m4a_engine_kill_channel(M4AEngine *engine, int handle)
{
    if (handle < 0)
        return;
    if (handle < TOTAL_PCM_CHANNELS) {
        engine->pcmChannels[handle].status = 0;
        return;
    }
    int i = handle - TOTAL_PCM_CHANNELS;
    if (i >= TOTAL_CGB_CHANNELS)
        return;
    M4ACGBChannel *ch = &engine->cgbChannels[i];
    /* TrackStop: CgbOscOff before clearing a CGB channel's status.  The
     * writes land at the current CPU cycle (MPlayMain time). */
    if (ch->status) {
        int64_t cursor = engine->gba.now;
        m4a_cgb_osc_off(ch, &engine->psg[i >= MAX_CGB_CHANNELS], &engine->gba, &cursor);
    }
    ch->status = 0;
}

void m4a_engine_reset_portamento(M4AEngine *engine)
{
    for (int i = 0; i < MAX_TRACKS; i++)
        reset_portamento_note_state(engine, i);
}

void m4a_engine_set_portamento_enabled(M4AEngine *engine, bool enabled)
{
    engine->portamentoEnabled = enabled;
    if (!enabled) {
        /* Drop any in-progress glide and the CC 5 duration so a later re-enable
         * doesn't resume a stale glide; note history is cleared too. */
        for (int i = 0; i < MAX_TRACKS; i++)
            engine->tracks[i].portamentoDuration = 0;
        m4a_engine_reset_portamento(engine);
    }
}

void m4a_engine_set_pwm_enabled(M4AEngine *engine, bool enabled)
{
    engine->pwmEnabled = enabled;
    if (!enabled) {
        /* Stop modulation everywhere and restore each square channel's default
         * duty cycle so the sound doesn't freeze on a modulated duty. */
        for (int i = 0; i < MAX_TRACKS; i++) {
            M4ATrack *track = &engine->tracks[i];
            uint8_t voiceType = track->currentVoice.type & VOICE_TYPE_CGB_MASK;
            if (track->pwmSpeed != 0 && (voiceType == 1 || voiceType == 2)) {
                M4ACGBChannel *ch = find_track_square_channel(engine, i);
                if (ch != NULL) {
                    ch->dutyCycle = (uint8_t)(uintptr_t)track->currentVoice.wavePointer & 0x03;
                    ch->modify |= 0x04;
                }
            }
            track->pwmPattern = 0;
            track->pwmSpeed = 0;
            track->pwmSpeedCounter = 0;
            track->pwmStep = 0;
        }
        engine->pwmActiveFlag = false;
    }
}

void m4a_engine_set_poly_debug_invert(M4AEngine *engine, bool enabled)
{
    engine->polyDebugInvert = enabled;
    if (!enabled) {
        /* Kill the shadow pool: lost sounds only exist for the invert mode. */
        for (int i = MAX_PCM_CHANNELS; i < TOTAL_PCM_CHANNELS; i++)
            engine->pcmChannels[i].status = 0;
        for (int i = MAX_CGB_CHANNELS; i < TOTAL_CGB_CHANNELS; i++)
            m4a_engine_kill_channel(engine, TOTAL_PCM_CHANNELS + i);
    }
}

void m4a_engine_reset_poly_stats(M4AEngine *engine)
{
    memset(engine->polyDropCount, 0, sizeof(engine->polyDropCount));
    memset(engine->polyStealCount, 0, sizeof(engine->polyStealCount));
    memset(engine->polyTailCutCount, 0, sizeof(engine->polyTailCutCount));
    engine->polyEventTotal = 0;
}

void m4a_engine_set_song_volume(M4AEngine *engine, uint8_t volume)
{
    engine->songMasterVolume = volume;
    for (int i = 0; i < MAX_TRACKS; i++) {
        M4ATrack *track = &engine->tracks[i];
        track->volume = track->rawVolume * volume / MAX_SONG_VOLUME;
        refresh_volumes(engine, track, i);
    }
}

/*
 * Process one LFO tempo tick for all active tracks.
 * In the GBA, this runs inside MPlayMain's tempo loop, so it fires
 * at the tempo rate (tempoI/150 times per VBlank), not at a fixed 60Hz.
 */
static void m4a_lfo_tick(M4AEngine *engine)
{
    /* The MIDI-path estimate charges each existing track's wait / LFO path
     * (_081DD938 -> _081DD994, m4a_sequencer.c lfo_step) by the branch the
     * step takes. */
    uint16_t costMask = m4a_est_lfo_mask(engine);
    for (int i = 0; i < MAX_TRACKS; i++) {
        M4ATrack *track = &engine->tracks[i];
        bool cost = (costMask >> i) & 1;
        if (track->lfoSpeed == 0 || track->mod == 0 || track->finished) {
            if (cost && !track->finished)
                m4a_est_charge(engine, track->lfoSpeed == 0 ? M4A_CYC_WAIT_LFOS0 : M4A_CYC_WAIT_NO_LFO);
            continue;
        }

        if (track->lfoDelayC > 0) {
            track->lfoDelayC--;
            if (cost)
                m4a_est_charge(engine, M4A_CYC_WAIT_LFO_DELAY);
            continue;
        }

        /* The asm keeps the untruncated sum for the rising half (see
         * lfo_step in m4a_sequencer.c). */
        int32_t lfoSum = (int32_t)track->lfoSpeedC + track->lfoSpeed;
        track->lfoSpeedC = (uint8_t)lfoSum;
        uint8_t lfoPos = track->lfoSpeedC;
        int32_t lfoVal;

        /* Triangle wave */
        if ((int8_t)(lfoPos - 0x40) < 0) {
            lfoVal = (int8_t)lfoPos;
        } else {
            lfoVal = 0x80 - lfoSum;
        }

        int8_t newModM = (int8_t)((track->mod * lfoVal) >> 6);
        if (cost)
            m4a_est_charge(engine, M4A_CYC_LFO_STEP
                           + ((int8_t)(lfoPos - 0x40) < 0 ? M4A_CYC_LFO_NEG : M4A_CYC_LFO_POS)
                           + (newModM == track->modM ? M4A_CYC_LFO_SAME
                              : track->modT == 0 ? M4A_CYC_LFO_PIT : M4A_CYC_LFO_VOL));
        if (newModM != track->modM) {
            /* MPlayMain's LFO half only flags the track: PITCHG for
             * vibrato, VOLCHG for tremolo / autopan, for the frame's pass. */
            track->modM = newModM;
            track->chgFlags |= track->modT == 0 ? M4A_TRK_PITCHG : M4A_TRK_VOLCHG;
        }
    }
}

/* The boundary between two ticks of one frame (m4a_engine.h): the step of
 * the tick that ended, now, after its events and before the next tick's
 * (MPlayMain's _081DD938 at the end of each track's tick). */
void m4a_engine_tick_boundary(M4AEngine *engine)
{
    if (engine->externalSequencer)
        return;
    if (queue_event(engine, (M4AQueuedEvent){ M4A_EV_TICK_BOUNDARY, 0, 0, 0, 0 }))
        return;
    m4a_lfo_tick(engine);
    m4a_est_tick_boundary(engine);
    if (engine->lfoTicksAhead < UINT8_MAX)
        engine->lfoTicksAhead++;
}

/*
 * Advance active portamento glides.  Called once per engine tick (~60Hz),
 * matching MPlayProcessPortamento which runs once per VBlank so the glide
 * stays smooth regardless of tempo.  Tracks are assumed monophonic for
 * portamento.  The glide spans `duration` song ticks: elapsed accumulates
 * tempoI per VBlank and the glide completes at duration*150 (one song tick
 * fires per 150 accumulated tempo units).  No-op unless portamento is enabled.
 */
static void m4a_portamento_tick(M4AEngine *engine)
{
    if (!engine->portamentoEnabled)
        return;

    for (int i = 0; i < MAX_TRACKS; i++) {
        M4ATrack *track = &engine->tracks[i];
        if (!track->portamentoGliding)
            continue;

        int32_t elapsed = (int32_t)track->portamentoElapsed + engine->tempoI;
        int32_t totalDurationUnits = (int32_t)track->portamentoDuration * 150;
        int32_t startKey = track->portamentoPrevKey;
        int32_t targetKey = track->portamentoTargetKey;
        int32_t currentKey16; /* current pitch in 8.8 fixed point */

        track->portamentoElapsed = (uint32_t)elapsed;
        if (totalDurationUnits == 0 || elapsed >= totalDurationUnits) {
            /* Portamento glide is complete! */
            currentKey16 = targetKey << 8;
            track->portamentoPrevKey = (uint8_t)targetKey;
            track->portamentoElapsed = 0;
            track->portamentoGliding = false;
        } else {
            /* Interpolate to get the current key */
            currentKey16 = (startKey << 8)
                         + (((targetKey - startKey) * elapsed) << 8) / totalDurationUnits;
        }

        apply_portamento_pitch(engine, track, i, currentKey16);
    }
}

/*
 * Advance the pulse-width modulation duty-cycle pattern for each track.  Called
 * once per engine tick (~60Hz/VBlank), so the modulation rate is tempo-
 * independent.  Only affects CGB square channels 1 and 2.  Mirrors
 * MPlayProcessPulseWidthMod in pokeemerald's m4a.c.
 */
static void m4a_pwm_tick(M4AEngine *engine)
{
    if (!engine->pwmActiveFlag)
        return;

    bool anyActive = false;

    for (int i = 0; i < MAX_TRACKS; i++) {
        M4ATrack *track = &engine->tracks[i];

        if (track->pwmSpeed == 0 || track->pwmPattern == 0)
            continue;

        anyActive = true;

        M4ACGBChannel *ch = find_track_square_channel(engine, i);
        if (ch == NULL)
            continue;

        const PulseWidthModPattern *pattern = &gPulseWidthModPatterns[track->pwmPattern];
        if (pattern->numSteps == 0)
            continue;

        /* A note CgbSound starts this frame: MPlayProcessPulseWidthMod's
         * SOUND_CHANNEL_SF_START branch -- pattern step 0, the counter
         * reloaded, no decrement (it runs before CgbSound since slice 4, so
         * START is still set here; note-on already chose step 0). */
        if (ch->status & CHN_START) {
            track->pwmStep = 0;
            track->pwmSpeedCounter = track->pwmSpeed;
            ch->dutyCycle = pattern->duty[0];
            continue;
        }

        /* Move to the next step once the per-step counter expires. */
        if (--track->pwmSpeedCounter > 0)
            continue;

        track->pwmSpeedCounter = track->pwmSpeed;
        uint8_t step = track->pwmStep + 1;
        if (step >= pattern->numSteps)
            step = 0;
        track->pwmStep = step;
        ch->dutyCycle = pattern->duty[step];
        ch->modify |= 0x04;   /* the driver rewrites NRx1 (eventide's MO_DUTY) */
    }

    engine->pwmActiveFlag = anyActive;
}

/* MPlayMain's tempo loop and its tail for the MIDI path (no sequencer):
 * tempoC += tempoI each VBlank, one LFO step per 150 accumulated; then
 * MPlayProcessPortamento and MPlayProcessPulseWidthMod, which MPlayMain
 * calls after the VOLCHG / PITCHG pass (ccd2ad2e / 11f0b446). */
static void mplay_main_tail(M4AEngine *engine)
{
    engine->tempoC += engine->tempoI;
    /* A TEMPO held by m4a_engine_set_tick_tempo: ply_tempo runs inside
     * the frame's ticks, after _081DD86C's add. */
    if (engine->tempoINextSet) {
        engine->tempoI = engine->tempoINext;
        engine->tempoINextSet = false;
    }
    int ticks = 0;
    while (engine->tempoC >= 150) {
        engine->tempoC -= 150;
        ticks++;
        /* A tick whose step m4a_engine_tick_boundary already ran. */
        if (engine->lfoTicksAhead > 0)
            engine->lfoTicksAhead--;
        else
            m4a_lfo_tick(engine);
    }
    engine->lfoTicksAhead = 0;
    /* The MIDI-path estimate's tick passes and VOLCHG / PITCHG pass, from
     * the flags the engine's pass is about to consume. */
    m4a_est_frame(engine, ticks);
    /* The VOLCHG / PITCHG pass after the ticks. */
    track_change_pass(engine);

    /* Portamento after the LFO so a glide overrides the pitch the LFO
     * wrote this tick, matching MPlayMain's ordering on the GBA. */
    m4a_portamento_tick(engine);
    m4a_pwm_tick(engine);
}

/*
 * Engine tick - called at ~60Hz (VBlank rate)
 * Advances envelopes at VBlank rate and LFO at tempo rate,
 * matching the GBA's split between SoundMainRAM and MPlayMain.
 */
void m4a_engine_tick(M4AEngine *engine)
{
    engine->inTick = true;
    /* The song sequencer runs first, like MPlayMain inside SoundMain. */
    if (engine->frameHook)
        engine->frameHook(engine, engine->frameHookUser);
    bool seq = engine->externalSequencer;

    /* Advance c15 counter (0-14 cycle) */
    if (engine->c15 > 0)
        engine->c15--;
    else
        engine->c15 = 14;

    /* PCM channel gate times (the sequencer counts gates in song ticks);
     * the PCM envelopes themselves step inside SoundMainRAM
     * (m4a_sound_main_ram), per channel just before its mix. */
    for (int i = 0; i < TOTAL_PCM_CHANNELS; i++) {
        M4APCMChannel *ch = &engine->pcmChannels[i];
        if ((ch->status & CHN_ON) && !seq && ch->gateTime > 0) {
            ch->gateTime--;
            if (ch->gateTime == 0)
                ch->status |= CHN_STOP;
        }
    }
    /* The CGB channels' gate times, the same walk (MPlayMain's chain walk
     * precedes the track's commands, so it runs before the latched events
     * below: a note that got gateTime 1 last frame stops here). */
    for (int i = 0; i < TOTAL_CGB_CHANNELS; i++) {
        M4ACGBChannel *ch = &engine->cgbChannels[i];
        if ((ch->status & CHN_ON) && !seq && ch->gateTime > 0) {
            ch->gateTime--;
            if (ch->gateTime == 0)
                ch->status |= CHN_STOP;
        }
    }

    /* The latched MIDI-path events: MPlayMain's commands for this frame, in
     * arrival order, before CgbSound, the LFO and the mixer's envelope step. */
    if (engine->eventQueueCount > 0)
        drain_events(engine);

    /* The rest of MPlayMain without a sequencer: the frame's LFO steps
     * (tempo loop), the VOLCHG / PITCHG pass, then portamento and PWM, all
     * before CgbSound as in SoundMain, so a CGB pitch / volume / duty they
     * set is written by this frame's CgbSound (docs/ACCURACY.md, "Rules for every source"). */
    if (!seq)
        mplay_main_tail(engine);

    /* Process CGB channel envelopes (VBlank rate): CgbSound.  Its register
     * writes are placed on the CPU clock: the entry follows MPlayMain's work
     * (cgbEntryCycles) and each channel advances a cursor through the
     * measured write offsets.  The shadow pool (polyphony debug) replays
     * the same timeline on its own PSG. */
    int64_t cgbEntry = (engine->soundMainStamp >= 0 ? engine->soundMainStamp : engine->gba.now)
                     + engine->cgbEntryCycles;
    for (int pool = 0; pool < TOTAL_CGB_CHANNELS / MAX_CGB_CHANNELS; pool++) {
        /* The cursor is the cycle of each channel's loop head (`for (ch = 1
         * ...)` in CgbSound); a channel's pass moves it to the next one's. */
        int entryToLoop = engine->c15 == 14 ? M4A_CYC_CGB_ENTRY_RELOAD : M4A_CYC_CGB_ENTRY_TO_LOOP;
        int64_t cursor = cgbEntry + entryToLoop + m4a_gba_fifo_dma_stall(&engine->gba, cgbEntry, cgbEntry + entryToLoop);
        for (int c = 0; c < MAX_CGB_CHANNELS; c++) {
            int i = pool * MAX_CGB_CHANNELS + c;
            M4ACGBChannel *ch = &engine->cgbChannels[i];
            if (!(ch->status & CHN_ON)) {
                cursor += M4A_CYC_CGB_INACTIVE + m4a_gba_fifo_dma_stall(&engine->gba, cursor, cursor + M4A_CYC_CGB_INACTIVE);
                continue;
            }
            m4a_cgb_channel_tick(ch, engine->c15, &engine->psg[pool], &engine->gba, &cursor);
        }
        if (pool == 0)
            engine->cgbCursorEnd = cursor;   /* the cost model's CgbSound tail starts here */
    }

    /* With a sequencer, portamento and PWM keep their place after CgbSound
     * (the sequencer never starts a glide or a PWM pattern: no-ops there). */
    if (seq) {
        m4a_portamento_tick(engine);
        m4a_pwm_tick(engine);
    }
    engine->inTick = false;
}

/*
 * Main audio processing function.
 * Generates numSamples of stereo float output at the host rate by running the
 * cycle scheduler until the resampler holds that many samples.
 */
/* Live mode (m4a_engine_set_live_mode): mix the ring up to M4A_LIVE_AHEAD
 * samples past the DMA's fetch position, in pieces, each through the
 * mixer's range entry with the slot's own reverb tap (the slot after it,
 * SoundMainRAM's; the ring's first slot for its last, as pcmDmaCounter 2
 * selects).  The DMA restarts at the ring's start every pcmDmaPeriod
 * frames, exactly ring-size samples apart (the timer period is
 * 280896 / pcmSamplesPerVBlank), so the cursor is circular and the first
 * samples of the ring are mixed just before the restart.  A cursor far from
 * the fetch position (the mode just turned on, a rate change) re-syncs to
 * it. */
static void live_mix_ahead(M4AEngine *engine)
{
    M4AGbaAudio *g = &engine->gba;
    if (!g->dmaEnabled)
        return;
    const int spv = engine->pcmSamplesPerVBlank;
    const int period = engine->pcmDmaPeriod;
    /* The span the DMA walks before m4aSoundVSync restarts it: whole
     * slots only.  The vanilla ring (1584) is longer than its 7 slots of
     * 224 (1568); the tail is never read. */
    const int ring = period * spv;
    int src = g->fifo[0].srcPos % ring;
    src -= src & 3;
    int ahead = (engine->liveMixPos - src + ring) % ring;
    /* Far ahead only when the cursor is stale (the mode just turned on, a
     * rate change): a re-sync re-mixes what the DMA has fetched already.
     * Normal operation stays within AHEAD plus the 16-sample fetch the
     * scheduler step in between may have made, plus a late VSync's. */
    if (ahead > 3 * M4A_LIVE_AHEAD) {
        engine->liveMixPos = src;
        ahead = 0;
    }
    while (ahead < M4A_LIVE_AHEAD) {
        int pos = engine->liveMixPos;
        int slot = pos / spv;
        int start = pos - slot * spv;
        int n = M4A_LIVE_PIECE;
        if (start + n > spv)
            n = spv - start;
        m4a_sound_main_ram_range(engine, slot * spv, slot == period - 1 ? 2 : 1, start, n,
                                 M4A_ENVELOPE_STEP_STARTING);
        pos += n;
        if (pos >= ring)
            pos -= ring;
        engine->liveMixPos = pos;
        ahead += n;
    }
}

/* Live mode: a PSG channel keyed since the last tick gets CgbSound's start
 * pass now (its registers written at the current cycle), not at the next
 * tick.  The tick then steps it as any sounding channel. */
static void live_start_cgb(M4AEngine *engine)
{
    for (int i = 0; i < TOTAL_CGB_CHANNELS; i++) {
        M4ACGBChannel *ch = &engine->cgbChannels[i];
        if (!(ch->status & CHN_ON) || !(ch->status & CHN_START) || (ch->status & CHN_STOP))
            continue;
        int64_t cursor = engine->gba.now;
        m4a_cgb_channel_tick(ch, engine->c15, &engine->psg[i / MAX_CGB_CHANNELS], &engine->gba,
                             &cursor);
    }
}

void m4a_engine_process(M4AEngine *engine, float *outL, float *outR, int numSamples)
{
    if (!engine->booted)
        m4a_engine_boot(engine, false);

    M4AGbaAudio *g = &engine->gba;
    int done = 0;
    while (done < numSamples) {
        int want = numSamples - done;
        if (want > 4096) want = 4096;
        while (m4a_gba_samples_avail(g) < want) {
            if (engine->liveMode) {
                live_start_cgb(engine);
                live_mix_ahead(engine);
            }
            run_until(engine, g->now + GBA_SAMPLE_EVENT);
        }
        int got = m4a_gba_read(g, outL + done, outR + done, want);
        if (got <= 0)
            break;
        done += got;
    }
    for (int i = done; i < numSamples; i++)
        outL[i] = outR[i] = 0.0f;
}
