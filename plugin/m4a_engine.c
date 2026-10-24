#include "m4a_engine.h"
#include "m4a_channel.h"
#include "m4a_reverb.h"
#include "m4a_tables.h"
#include <string.h>
#include <stdlib.h>

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
 * PCM playback frequency word for a MidiKeyToFreq result: the per-sample step
 * of the SoundMainRAM mixer (23-bit fraction) at the engine's PCM rate --
 * MidiKeyToFreq(...) * SoundInfo.divFreq, as SoundMainRAM computes it.
 */
static inline uint32_t pcm_freq_word(const M4AEngine *engine, uint32_t freq)
{
    return (uint32_t)((uint64_t)freq * (uint32_t)engine->pcmDivFreq);
}

/* Fixed-frequency (TONEDATA_TYPE_FIX) voices advance exactly one source
 * sample per mixed sample. */
#define PCM_FIXED_FREQ_WORD 0x800000u

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
 * m4a_engine_boot from the same measurements; see docs/ACCURACY_PLAN.md.
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
static const struct { int64_t cycle; int action; uint32_t reg; uint8_t value; } sBootTable[] = {
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
#define BOOT_TABLE_LEN ((int)(sizeof(sBootTable) / sizeof(sBootTable[0])))

static int64_t frame_start(int frame)
{
    return FRAME1_START + (int64_t)(frame - 1) * GBA_CYCLES_PER_FRAME;
}

/* SampleFreqSet's per-rate numbers for the configured mix rate, and the ring
 * buffer that goes with them.  Rates from gPcmSamplesPerVBlankTable give the
 * game's exact figures (13379 Hz -> 224 samples, timer period 1254 cycles,
 * 7 VBlanks per DMA buffer); any other rate is rounded to a whole number of
 * samples per VBlank. */
static void pcm_rate_setup(M4AEngine *engine)
{
    float rate = engine->pcmMixRate > 0.0f ? engine->pcmMixRate : engine->sampleRate;
    int spv = (int)(rate / VBLANK_RATE + 0.5f);
    if (spv < 16) spv = 16;
    if (spv > 8192) spv = 8192;
    engine->pcmSamplesPerVBlank = spv;
    engine->pcmFreq = (597275 * spv + 5000) / 10000;
    engine->pcmDivFreq = (16777216 / engine->pcmFreq + 1) >> 1;
    int period = GBA_PCM_DMA_BUF_SIZE / spv;
    if (period < 1) period = 1;
    engine->pcmDmaPeriod = (uint8_t)period;
    engine->pcmDmaCounter = 0;

    int ringSize = GBA_PCM_DMA_BUF_SIZE;
    if (period * spv + 16 > ringSize)
        ringSize = period * spv + 16;
    free(engine->pcmRing);
    engine->pcmRing = (int16_t *)calloc((size_t)ringSize * 2, sizeof(int16_t));
    engine->pcmRingSize = ringSize;
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
    switch (sBootTable[step].action) {
    case BOOT_PSG_WRITE:
        psg_write_both(engine, sBootTable[step].reg, sBootTable[step].value);
        break;
    case BOOT_FIFO_RESET: m4a_gba_fifo_reset(g); break;
    case BOOT_SOUNDBIAS:  m4a_gba_write_soundbias_resolution(g, 1); break;
    case BOOT_DMA_ON:
    case BOOT_DMA_ON_2:   m4a_gba_dma_enable(g); break;
    case BOOT_DMA_OFF:    m4a_gba_dma_disable(g); break;
    case BOOT_TIMER_ON:
    case BOOT_TIMER_ON_2: m4a_gba_timer_enable(g); break;
    case BOOT_TIMER_OFF:  m4a_gba_timer_disable(g); break;
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

/* SoundMainRAM's frame: mix pcmSamplesPerVBlank samples of every PCM channel
 * (plus reverb) into the ring slot SoundMain selects from pcmDmaCounter.
 * In the polyphony-overflow debug mode the shadow pool is heard instead of
 * the real channels, which still render (into a discarded sum) so their
 * positions advance exactly as in normal playback. */
static void mix_pcm_frame(M4AEngine *engine)
{
    int spv = engine->pcmSamplesPerVBlank;
    int counter = engine->pcmDmaCounter;
    int offset = 0;
    if (counter - 1 > 0)
        offset = (engine->pcmDmaPeriod - (counter - 1)) * spv;
    if (offset + spv > engine->pcmRingSize)
        offset = 0;
    int16_t *ringA = engine->pcmRing + offset;                        /* right */
    int16_t *ringB = engine->pcmRing + engine->pcmRingSize + offset;  /* left */
    bool invert = engine->polyDebugInvert;

    for (int i = 0; i < spv; i++) {
        int32_t pcmL = 0, pcmR = 0;
        int32_t mutedL = 0, mutedR = 0;
        for (int ch = 0; ch < TOTAL_PCM_CHANNELS; ch++) {
            M4APCMChannel *c = &engine->pcmChannels[ch];
            if (!(c->status & CHN_ON))
                continue;
            /* Real-pool audition notes stay audible in invert mode so the
             * user can play against the lost sounds. */
            bool audible = ((ch >= MAX_PCM_CHANNELS) == invert)
                        || (ch < MAX_PCM_CHANNELS && c->audition);
            m4a_pcm_channel_render(c, audible ? &pcmL : &mutedL, audible ? &pcmR : &mutedR);
        }
        /* Reverb is a DirectSound-buffer effect: it runs at the PCM mix rate,
         * before the FIFO and before the PSG is added. */
        m4a_reverb_process(&engine->reverb, &pcmL, &pcmR);
        if (pcmL > 32767) pcmL = 32767; else if (pcmL < -32768) pcmL = -32768;
        if (pcmR > 32767) pcmR = 32767; else if (pcmR < -32768) pcmR = -32768;
        ringA[i] = (int16_t)pcmR;
        ringB[i] = (int16_t)pcmL;
    }
}

/* m4aSoundMain: the VBlank frame. */
static void sound_main(M4AEngine *engine)
{
    m4a_engine_tick(engine);
    mix_pcm_frame(engine);
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
 * the driver's interrupts in time order.  On a tie the CPU's register writes
 * come first: in mgba the store completes inside the instruction and the
 * events due at that cycle are processed after it. */
static void run_until(M4AEngine *engine, int64_t until)
{
    M4AGbaAudio *g = &engine->gba;
    for (;;) {
        int64_t next = engine->nextVSync < engine->nextSoundMain ? engine->nextVSync
                                                                 : engine->nextSoundMain;
        if (engine->bootStep < BOOT_TABLE_LEN && sBootTable[engine->bootStep].cycle < next)
            next = sBootTable[engine->bootStep].cycle;
        if (next > until) {
            m4a_gba_run(g, until);
            return;
        }
        m4a_gba_run(g, next - 1);
        if (g->now < next)   /* a frame's register writes may already be past */
            g->now = next;
        while (engine->bootStep < BOOT_TABLE_LEN && sBootTable[engine->bootStep].cycle == next) {
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

    engine->sampleRate = sampleRate;
    /* Default to the GBA's hardware DirectSound mix rate so high notes alias
     * the way they do in-game.  Set to 0 (follow host rate) for clean mixing. */
    engine->pcmMixRate = 13379.0f;
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
    engine->cgbEntryCycles = M4A_CYC_CGB_ENTRY_BASE + M4A_CYC_PER_TRACK - M4A_CYC_SOUNDMAIN_ENTRY;
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

    /* Output stage: PCM ring + GBA hardware model at the host rate, and the
     * reverb (a DirectSound-buffer effect that runs at the mixing rate). */
    pcm_rate_setup(engine);
    m4a_gba_init(&engine->gba, sampleRate, engine->pcmRing, engine->pcmRingSize,
                 engine->pcmSamplesPerVBlank);
    m4a_psg_reset(&engine->psg[0]);
    m4a_psg_reset(&engine->psg[1]);
    scheduler_reset(engine);
    m4a_reverb_init(&engine->reverb, (float)engine->pcmFreq, 0);
}

void m4a_engine_destroy(M4AEngine *engine)
{
    m4a_reverb_destroy(&engine->reverb);
    m4a_gba_destroy(&engine->gba);
    free(engine->pcmRing);
    engine->pcmRing = NULL;
}

void m4a_engine_set_pcm_mix_rate(M4AEngine *engine, float rate)
{
    /* 0 means "follow host rate"; otherwise clamp to a sane audio range. */
    if (rate != 0.0f) {
        if (rate < 1000.0f)   rate = 1000.0f;
        if (rate > 192000.0f) rate = 192000.0f;
    }
    engine->pcmMixRate = rate;
    pcm_rate_setup(engine);
    m4a_gba_set_pcm_rate(&engine->gba, engine->pcmSamplesPerVBlank, engine->pcmRing,
                         engine->pcmRingSize);

    /* The reverb delay line is sized for the mixing rate; rebuild it, keeping
     * the current amount. */
    uint8_t amount = engine->reverb.amount;
    m4a_reverb_destroy(&engine->reverb);
    m4a_reverb_init(&engine->reverb, (float)engine->pcmFreq, amount);
}

void m4a_engine_set_tempo_bpm(M4AEngine *engine, double bpm)
{
    if (bpm < 1.0) bpm = 1.0;
    engine->tempoI = (uint16_t)(bpm + 0.5);
}

void m4a_engine_set_voicegroup(M4AEngine *engine, ToneData *voiceGroup)
{
    engine->voiceGroup = voiceGroup;
}

/*
 * Program Change - select instrument from voicegroup
 */
void m4a_engine_program_change(M4AEngine *engine, int trackIndex, uint8_t program)
{
    /* voiceGroup holds exactly 128 entries; a wild program must not pick a
     * voice from whatever memory follows it. */
    if (trackIndex < 0 || trackIndex >= MAX_TRACKS || !engine->voiceGroup || program > 127)
        return;

    M4ATrack *track = &engine->tracks[trackIndex];
    track->currentProgram = program;
    track->currentVoice = engine->voiceGroup[program];
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

    engine->lastNoteScan = count;
    for (int i = first; i < first + count; i++) {
        M4APCMChannel *ch = &engine->pcmChannels[i];

        if (!(ch->status & CHN_ON)) {
            /* Free channel - use immediately */
            engine->lastNoteScan = i - first;
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
        uint32_t freq = m4a_midi_key_to_freq(ch->wav, (uint8_t)key, fine);
        ch->frequency = pcm_freq_word(engine, freq);
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
static void clear_mod_m(M4AEngine *engine, M4ATrack *track, int trackIndex);

/* ply_note, after a channel has been taken: the LFO delay restarts for the
 * new note (lfoDelayC = lfoDelay) and, when a delay is set, clear_modM zeroes
 * the LFO output so the note starts unmodulated.  Returns true when keyM /
 * pitM changed and note keys must be recomputed. */
static bool note_on_restart_lfo_delay(M4AEngine *engine, M4ATrack *track, int trackIndex)
{
    track->lfoDelayC = track->lfoDelay;
    if (track->lfoDelay == 0)
        return false;
    if (engine->externalSequencer) {
        /* clear_modM only flags the track; ply_note then drops the flags
         * (flags &= 0xF0), so older channels keep their LFO-offset pitch and
         * any pending VOL/PAN/BEND until the next change.  Only the new
         * note's keyM/pitM see the recentred LFO. */
        track->modM = 0;
        track->lfoSpeedC = 0;
        m4a_track_vol_pit_set(track);
    } else {
        clear_mod_m(engine, track, trackIndex);
    }
    return true;
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

int m4a_engine_note_on(M4AEngine *engine, int trackIndex, uint8_t key, uint8_t velocity)
{
    /* Keysplit/rhythm voices index 128-entry tables by key. */
    if (trackIndex < 0 || trackIndex >= MAX_TRACKS || key > 127)
        return M4A_CHANNEL_NONE;

    M4ATrack *track = &engine->tracks[trackIndex];
    ToneData *voice = resolve_voice(&track->currentVoice, key);
    if (!voice) return M4A_CHANNEL_NONE;

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
        if (!shadow && note_on_restart_lfo_delay(engine, track, trackIndex))
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
             * and sweep (MO_VOL on the next tick); the volume refresh that
             * an LFO-delay restart may have run above would otherwise do so.
             * The new frequency still has to reach NRx3/NRx4. */
            ch->modify = 0x02;
        } else {
            m4a_cgb_channel_start(ch);
        }

        /* A dropped note never sounded on the GBA, so it must not become the
         * start key of a later portamento glide. */
        if (!shadow)
            portamento_note_started(engine, track, trackIndex, ch->key);
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
        if (!shadow && note_on_restart_lfo_delay(engine, track, trackIndex))
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

        /* Calculate the playback frequency word (SoundMainRAM's per-sample
         * step).  Fixed-frequency voices ignore the key and play one source
         * sample per mixed sample. */
        if (voice->type & VOICE_TYPE_FIX)
            ch->frequency = PCM_FIXED_FREQ_WORD;
        else
            ch->frequency = pcm_freq_word(engine,
                m4a_midi_key_to_freq(voice->wav, (uint8_t)pcmFinalKey, track->pitM));

        m4a_pcm_channel_start(ch, voice->wav, voice->type);

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
                uint8_t prevVolume = prev->envelopeVolume;
                ch->envelopeVolume = (prevVolume > ch->sustain) ? prevVolume : ch->sustain;
                ch->status = CHN_ENV_SUSTAIN | (prev->status & CHN_LOOP);
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
        if (!shadow)
            portamento_note_started(engine, track, trackIndex, ch->key);
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

    /* Stop matching PCM channels (shadow channels release too, so a lost
     * sound audible in the overflow-debug mode ends at its note-off) */
    for (int i = 0; i < TOTAL_PCM_CHANNELS; i++) {
        M4APCMChannel *ch = &engine->pcmChannels[i];
        if ((ch->status & CHN_ON) && !(ch->status & CHN_STOP)
            && ch->trackIndex == trackIndex && ch->midiKey == key) {
            ch->status |= CHN_STOP;
        }
    }

    /* Stop matching CGB channels */
    for (int i = 0; i < TOTAL_CGB_CHANNELS; i++) {
        M4ACGBChannel *ch = &engine->cgbChannels[i];
        if ((ch->status & CHN_ON) && !(ch->status & CHN_STOP)
            && ch->trackIndex == trackIndex && ch->midiKey == key) {
            ch->status |= CHN_STOP;
        }
    }
}

/*
 * Recalculate and push updated frequencies into every active PCM/CGB channel
 * on the given track.  Called when pitch-related track state changes (pitch
 * bend, LFO vibrato) so that already-playing notes follow the new pitch.
 * Matches MPlayMain's per-tick note re-evaluation on real GBA hardware.
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
            uint32_t freq = m4a_midi_key_to_freq(ch->wav, (uint8_t)finalKey, track->pitM);
            ch->frequency = pcm_freq_word(engine, freq);
        }
    }
    for (int i = 0; i < TOTAL_CGB_CHANNELS; i++) {
        M4ACGBChannel *ch = &engine->cgbChannels[i];
        if ((ch->status & CHN_ON) && ch->trackIndex == trackIndex)
            cgb_set_pitch(ch, (int32_t)ch->key + track->keyM, track->pitM);
    }
}

/* Recalculate track vol/pan and push updated rightVolume/leftVolume into
* all active channels on the track. Matches MPlayMain's behavior of calling
* ChnVolSetAsm on every active channel when MPT_FLG_VOLCHG is set. */
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

/* Mirrors the GBA's clear_modM: zero the LFO accumulator and immediately push
 * the recentered pitch (or volume, for modT 1/2) to every sounding channel on
 * the track.  Without the push, a held note stays offset by the last LFO value
 * until the next pitch/volume event, because m4a_lfo_tick skips tracks whose
 * mod depth is 0. */
static void clear_mod_m(M4AEngine *engine, M4ATrack *track, int trackIndex)
{
    track->lfoSpeedC = 0;
    track->modM = 0;
    if (track->modT == 0) {
        m4a_track_vol_pit_set(track);
        refresh_channel_pitches(engine, track, trackIndex);
    } else {
        refresh_volumes(engine, track, trackIndex);
    }
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

    M4ATrack *track = &engine->tracks[trackIndex];

    switch (cc) {
    case 0x1:  /* Mod wheel -> LFO depth */
        track->mod = value;
        if (value == 0)
            clear_mod_m(engine, track, trackIndex);
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
        refresh_volumes(engine, track, trackIndex);
        break;
    case 0xA: /* Pan */
        track->pan = (int8_t)(value - 64);
        refresh_volumes(engine, track, trackIndex);
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
        m4a_track_vol_pit_set(track);
        refresh_channel_pitches(engine, track, trackIndex);
        break;
    case 0x15: /* LFO speed (LFOS) */
        /* Mirrors ply_lfos: only a speed of 0 resets the LFO (clear_modM).
         * Any other value just changes the rate, keeping the phase and the
         * current offset, so a stream of LFOS changes (a knob, or a song
         * repeating the command) doesn't restart the vibrato each time. */
        track->lfoSpeed = value;
        if (value == 0)
            clear_mod_m(engine, track, trackIndex);
        break;
    case 0x16: /* Modulation type (MODT): 0 vibrato, 1 tremolo, 2 auto-pan.
                * Mirrors ply_modt: a change re-evaluates both volume and pitch
                * (the LFO output moves from one to the other). */
        if (track->modT != value) {
            track->modT = value;
            m4a_track_vol_pit_set(track);
            refresh_channel_pitches(engine, track, trackIndex);
            refresh_volumes(engine, track, trackIndex);
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
        m4a_track_vol_pit_set(track);
        refresh_channel_pitches(engine, track, trackIndex);
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

    M4ATrack *track = &engine->tracks[trackIndex];

    /* Scale 14-bit MIDI bend to m4a's -64..+63 range */
    track->bend = (int8_t)(bend >> 7);

    /* Recompute keyM/pitM and push the new pitch into every active channel
     * on this track, matching MPlayMain's per-tick note re-evaluation. */
    m4a_track_vol_pit_set(track);
    refresh_channel_pitches(engine, track, trackIndex);
}

/*
 * Forget a track's portamento note history.  Called when notes are cut off
 * out-of-band (DAW transport stop/reset, All Notes Off / All Sound Off) so
 * that the first note after playback resumes doesn't glide from a note that
 * was sounding before the interruption.  portamentoDuration is kept: it's a
 * parameter (CC 5), not note state.
 */
static void reset_portamento_note_state(M4ATrack *track)
{
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
    reset_portamento_note_state(&engine->tracks[trackIndex]);
}

/*
 * All Sound Off - immediately silence everything
 */
void m4a_engine_all_sound_off(M4AEngine *engine)
{
    for (int i = 0; i < TOTAL_PCM_CHANNELS; i++)
        engine->pcmChannels[i].status = 0;
    for (int i = 0; i < TOTAL_CGB_CHANNELS; i++)
        if (engine->cgbChannels[i].status)
            m4a_engine_kill_channel(engine, TOTAL_PCM_CHANNELS + i);
    m4a_engine_reset_portamento(engine);
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
        reset_portamento_note_state(&engine->tracks[i]);
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
    for (int i = 0; i < MAX_TRACKS; i++) {
        M4ATrack *track = &engine->tracks[i];
        if (track->lfoSpeed == 0 || track->mod == 0)
            continue;

        if (track->lfoDelayC > 0) {
            track->lfoDelayC--;
            continue;
        }

        track->lfoSpeedC += track->lfoSpeed;
        uint8_t lfoPos = track->lfoSpeedC;
        int8_t lfoVal;

        /* Triangle wave */
        if ((int8_t)(lfoPos - 0x40) < 0) {
            lfoVal = (int8_t)lfoPos;
        } else {
            lfoVal = (int8_t)(0x80 - lfoPos);
        }

        int8_t newModM = (int8_t)((track->mod * lfoVal) >> 6);
        if (newModM != track->modM) {
            track->modM = newModM;
            m4a_track_vol_pit_set(track);

            /* Update active channels for this track */
            for (int j = 0; j < TOTAL_PCM_CHANNELS; j++) {
                M4APCMChannel *ch = &engine->pcmChannels[j];
                if ((ch->status & CHN_ON) && ch->trackIndex == i) {
                    chn_vol_set(ch, track);
                    /* Recalculate frequency for pitch mod (fixed-rate voices
                     * are immune, as in refresh_channel_pitches) */
                    if (track->modT == 0 && ch->wav && !(ch->type & VOICE_TYPE_FIX)) {
                        int32_t finalKey = (int32_t)ch->key + track->keyM;
                        if (finalKey < 0) finalKey = 0;
                        uint32_t freq = m4a_midi_key_to_freq(ch->wav, (uint8_t)finalKey, track->pitM);
                        ch->frequency = pcm_freq_word(engine, freq);
                    }
                }
            }
            for (int j = 0; j < TOTAL_CGB_CHANNELS; j++) {
                M4ACGBChannel *ch = &engine->cgbChannels[j];
                if ((ch->status & CHN_ON) && ch->trackIndex == i) {
                    cgb_chn_vol_set(ch, track);
                    /* Tremolo/autopan LFO steps are volume changes (VOLCHG ->
                     * MO_VOL on the GBA), so they retrigger the square-1
                     * sweep; vibrato steps are MO_PIT and do not. */
                    if (track->modT != 0)
                        ch->modify |= 0x01;
                    if (track->modT == 0)
                        cgb_set_pitch(ch, (int32_t)ch->key + track->keyM, track->pitM);
                }
            }
        }
    }
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

        /* Move to the next step once the per-step counter expires.  The note's
         * first duty cycle (pattern step 0) is set at note-on, mirroring the
         * SOUND_CHANNEL_SF_START branch of the GBA's process routine. */
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

/*
 * Engine tick - called at ~60Hz (VBlank rate)
 * Advances envelopes at VBlank rate and LFO at tempo rate,
 * matching the GBA's split between SoundMainRAM and MPlayMain.
 */
void m4a_engine_tick(M4AEngine *engine)
{
    /* The song sequencer runs first, like MPlayMain inside SoundMain. */
    if (engine->frameHook)
        engine->frameHook(engine, engine->frameHookUser);
    bool seq = engine->externalSequencer;

    /* Advance c15 counter (0-14 cycle) */
    if (engine->c15 > 0)
        engine->c15--;
    else
        engine->c15 = 14;

    /* Process PCM channel envelopes (VBlank rate); the shadow pool keeps its
     * envelopes running too so lost sounds evolve like real ones. */
    for (int i = 0; i < TOTAL_PCM_CHANNELS; i++) {
        M4APCMChannel *ch = &engine->pcmChannels[i];
        if (ch->status & CHN_ON) {
            /* Decrement gate time (the sequencer counts gates in song ticks) */
            if (!seq && ch->gateTime > 0) {
                ch->gateTime--;
                if (ch->gateTime == 0)
                    ch->status |= CHN_STOP;
            }
            m4a_pcm_channel_tick(ch, engine->masterVolume);
        }
    }

    /* Process CGB channel envelopes (VBlank rate): CgbSound.  Its register
     * writes are placed on the CPU clock: the entry follows MPlayMain's work
     * (cgbEntryCycles) and each channel advances a cursor through the
     * measured write offsets.  The shadow pool (polyphony debug) replays
     * the same timeline on its own PSG. */
    int64_t cgbEntry = engine->gba.now + engine->cgbEntryCycles;
    for (int pool = 0; pool < TOTAL_CGB_CHANNELS / MAX_CGB_CHANNELS; pool++) {
        int64_t cursor = cgbEntry + M4A_CYC_CGB_PROLOGUE;
        for (int c = 0; c < MAX_CGB_CHANNELS; c++) {
            int i = pool * MAX_CGB_CHANNELS + c;
            M4ACGBChannel *ch = &engine->cgbChannels[i];
            if (!(ch->status & CHN_ON)) {
                cursor += M4A_CYC_CGB_INACTIVE;
                continue;
            }
            if (!seq && ch->gateTime > 0) {
                ch->gateTime--;
                if (ch->gateTime == 0)
                    ch->status |= CHN_STOP;
            }
            m4a_cgb_channel_tick(ch, engine->c15, &engine->psg[pool], &engine->gba, &cursor);
        }
    }

    /* Tempo accumulator drives LFO ticks, matching MPlayMain's tempo loop.
     * tempoC += tempoI each VBlank; fires one LFO tick per 150 accumulated. */
    if (!seq) {
        engine->tempoC += engine->tempoI;
        while (engine->tempoC >= 150) {
            engine->tempoC -= 150;
            m4a_lfo_tick(engine);
        }
    }

    /* Advance portamento glides last so they override any pitch the LFO
     * wrote this tick, matching MPlayMain's ordering on the GBA. */
    m4a_portamento_tick(engine);

    /* Advance pulse-width modulation duty cycles for square-wave channels,
     * matching MPlayProcessPulseWidthMod's placement in MPlayMain. */
    m4a_pwm_tick(engine);
}

/*
 * Main audio processing function.
 * Generates numSamples of stereo float output at the host rate by running the
 * cycle scheduler until the resampler holds that many samples.
 */
void m4a_engine_process(M4AEngine *engine, float *outL, float *outR, int numSamples)
{
    if (!engine->booted)
        m4a_engine_boot(engine, false);

    M4AGbaAudio *g = &engine->gba;
    int done = 0;
    while (done < numSamples) {
        int want = numSamples - done;
        if (want > 4096) want = 4096;
        while (m4a_gba_samples_avail(g) < want)
            run_until(engine, g->now + GBA_SAMPLE_EVENT);
        int got = m4a_gba_read(g, outL + done, outR + done, want);
        if (got <= 0)
            break;
        done += got;
    }
    for (int i = done; i < numSamples; i++)
        outL[i] = outR[i] = 0.0f;
}
