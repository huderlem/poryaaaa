#ifndef M4A_ENGINE_H
#define M4A_ENGINE_H

#include <stdint.h>
#include <stdbool.h>

#ifdef __cplusplus
extern "C" {
#endif

#define MAX_PCM_CHANNELS 15
#define MAX_CGB_CHANNELS 4
/* The engine keeps a second "shadow" pool of channels after the real ones.
 * Shadow channels never participate in normal note allocation; they exist so
 * the polyphony-overflow debug mode can audibly play the sounds that were
 * lost to the polyphony limit (dropped notes and the remainders of stolen
 * notes).  Indices >= MAX_*_CHANNELS are shadow channels. */
#define TOTAL_PCM_CHANNELS (MAX_PCM_CHANNELS * 2)
#define TOTAL_CGB_CHANNELS (MAX_CGB_CHANNELS * 2)
#define MAX_TRACKS 16
#define VBLANK_RATE 59.7275f
#define MAX_SONG_VOLUME 127 // called "mxv" in pokeemerald

/* Voice types (matching GBA ToneData.type) */
#define VOICE_DIRECTSOUND           0x00
#define VOICE_SQUARE_1              0x01
#define VOICE_SQUARE_2              0x02
#define VOICE_PROGRAMMABLE_WAVE     0x03
#define VOICE_NOISE                 0x04
#define VOICE_DIRECTSOUND_NO_RESAMPLE 0x08
#define VOICE_SQUARE_1_ALT          0x09
#define VOICE_SQUARE_2_ALT          0x0A
#define VOICE_PROGRAMMABLE_WAVE_ALT 0x0B
#define VOICE_NOISE_ALT             0x0C
#define VOICE_DIRECTSOUND_ALT       0x10
#define VOICE_CRY                   0x20
#define VOICE_CRY_REVERSE           0x30
#define VOICE_KEYSPLIT              0x40
#define VOICE_KEYSPLIT_ALL          0x80

#define VOICE_TYPE_CGB_MASK         0x07
#define VOICE_TYPE_FIX              0x08

/* Golden Sun synth instruments (ipatix improved-mixer feature).
 * A DirectSound sample whose WaveData.size is 0 is not PCM data but a
 * synthesized-tone descriptor: data[1] selects the waveform (0 = pulse,
 * 1 = pseudo sawtooth, anything else = triangle) and, for the pulse wave,
 * data[2] = base duty cycle, data[3] = duty LFO step per frame,
 * data[4] = modulation amount, data[5] = duty LFO phase offset.
 * These values classify a PCM channel; 0 means "normal sample playback"
 * so zero-initialized channels are never misread as synths. */
#define M4A_SYNTH_NONE      0
#define M4A_SYNTH_PULSE     1
#define M4A_SYNTH_SAW       2
#define M4A_SYNTH_TRIANGLE  3

/* Channel status flags (matching GBA) */
#define CHN_START       0x80
#define CHN_STOP        0x40
#define CHN_LOOP        0x10
#define CHN_IEC         0x04
#define CHN_ENV_MASK    0x03
#define CHN_ENV_ATTACK  0x03
#define CHN_ENV_DECAY   0x02
#define CHN_ENV_SUSTAIN 0x01
#define CHN_ENV_RELEASE 0x00
#define CHN_ON          (CHN_START | CHN_STOP | CHN_IEC | CHN_ENV_MASK)

/* WaveData header (matches GBA binary format) */
typedef struct {
    uint16_t type;
    uint16_t status;
    uint32_t freq;
    uint32_t loopStart;
    uint32_t size;
    int8_t *data;
} WaveData;

/* ToneData (voice/instrument definition) */
typedef struct {
    uint8_t type;
    uint8_t key;
    uint8_t length;
    uint8_t panSweep;
    union {
        WaveData *wav;
        uint32_t *wavePointer;  /* for programmable wave */
        void *subGroup;         /* for keysplit: points to ToneData array */
    };
    union {
        uint8_t *keySplitTable; /* for keysplit type */
    };
    uint8_t attack;
    uint8_t decay;
    uint8_t sustain;
    uint8_t release;
} ToneData;

/* Pulse-width modulation duty-cycle pattern (matches GBA PulseWidthModPattern).
 * The PWMC command selects a pattern by index and the PWMS command sets the
 * modulation speed; the effect cycles a CGB square channel's duty cycle through
 * the pattern's steps.  Hardware duty values: 0=12.5%, 1=25%, 2=50%, 3=75%.
 * Only used when the opt-in pulse-width modulation feature is enabled. */
#define MAX_PWM_PATTERN_STEPS 7

typedef struct {
    uint8_t numSteps;                    /* 0 = disabled */
    uint8_t duty[MAX_PWM_PATTERN_STEPS]; /* duty cycle per step (0-3) */
} PulseWidthModPattern;

extern const PulseWidthModPattern gPulseWidthModPatterns[];
extern const uint8_t gNumPulseWidthModPatterns;

/* Track state (per MIDI channel) */
typedef struct {
    uint8_t flags;
    uint8_t volume;         /* track volume scaled by songMasterVolume (0-127) */
    uint8_t rawVolume;      /* raw CC 0x7 volume before songMasterVolume scaling */
    uint8_t volX;           /* external volume multiplier (0-64) */
    int8_t pan;             /* track pan (-64 to +63) */
    int8_t panX;            /* external pan adjustment */
    int8_t bend;            /* pitch bend (-64 to +63) */
    uint8_t bendRange;      /* bend range in semitones (default 2) */
    uint8_t lfoSpeed;
    uint8_t lfoSpeedC;
    uint8_t lfoDelay;
    uint8_t lfoDelayC;
    uint8_t mod;            /* modulation depth */
    uint8_t modT;           /* 0=vibrato, 1=tremolo, 2=autopan */
    int8_t modM;            /* current modulation output */
    int8_t keyShift;
    int8_t keyShiftX;
    int8_t tune;
    uint8_t pitX;
    int8_t keyM;            /* computed key after modifications */
    uint8_t pitM;           /* computed fine pitch */
    uint8_t volMR;          /* computed right volume */
    uint8_t volML;          /* computed left volume */
    uint8_t pseudoEchoVolume;
    uint8_t pseudoEchoLength;
    uint8_t portamentoDuration;  /* PORTAMENTO (CC 5) glide duration in song ticks; 0 = off */
    uint8_t portamentoPrevKey;   /* channel key of the last note played (glide start key);
                                  * 0 = no note played yet.  Updated on every note trigger
                                  * so glides never depend on whether the previous note's
                                  * channel is still alive. */
    uint8_t portamentoTargetKey; /* glide destination key (newest note's channel key) */
    bool portamentoGliding;      /* a glide is in progress */
    uint32_t portamentoElapsed;  /* accumulated tempoI units; glide done at duration*150 */
    uint8_t pwmPattern;          /* PWMC (CC 0x17): duty-cycle pattern index; 0 = off */
    uint8_t pwmSpeed;            /* PWMS (CC 0x19): VBlank frames per step; 0 = off */
    uint8_t pwmSpeedCounter;     /* counts down from pwmSpeed to the next step */
    uint8_t pwmStep;             /* current index into the pattern's duty[] */
    uint8_t priority;
    uint8_t currentProgram; /* last program_change index (0-127) */
    ToneData currentVoice;  /* current instrument */
} M4ATrack;

/* PCM Sound Channel */
typedef struct {
    uint8_t status;
    uint8_t type;
    uint8_t rightVolume;
    uint8_t leftVolume;
    uint8_t attack;
    uint8_t decay;
    uint8_t sustain;
    uint8_t release;
    uint8_t key;
    uint8_t envelopeVolume;
    uint8_t envelopeVolumeRight;
    uint8_t envelopeVolumeLeft;
    uint8_t pseudoEchoVolume;
    uint8_t pseudoEchoLength;
    uint8_t midiKey;
    uint8_t velocity;
    uint8_t priority;
    int8_t rhythmPan;
    uint8_t gateTime;

    /* Sample playback */
    WaveData *wav;
    int8_t *currentPointer;
    int32_t count;          /* remaining samples */
    uint32_t fw;            /* fractional position (23-bit fraction) */
    uint32_t frequency;     /* playback frequency word */

    /* Owner */
    int trackIndex;
    /* Started by a host audition (engine->auditionNote at note-on): stays
     * audible in the polyphony-overflow invert mode. */
    bool audition;
    bool isLoop;
    int32_t loopLen;        /* loop length in samples */
    int8_t *loopStart;      /* pointer to loop start in sample data */

    /* Golden Sun synth voice (M4A_SYNTH_*; M4A_SYNTH_NONE = normal sample).
     * When active, the channel reuses fw as the 32-bit oscillator phase (one
     * wave period = 2^32) and count as the pulse duty LFO accumulator / saw
     * filter state, exactly like the GBA improved mixer reuses those fields. */
    uint8_t synthType;
    uint32_t synthPulseDuty; /* pulse: phase threshold; recomputed every tick */
} M4APCMChannel;

/* CGB Channel (square, noise, programmable wave) */
typedef struct {
    uint8_t status;
    uint8_t type;           /* 1=sq1, 2=sq2, 3=progwave, 4=noise */
    uint8_t rightVolume;
    uint8_t leftVolume;
    uint8_t attack;
    uint8_t decay;
    uint8_t sustain;
    uint8_t release;
    uint8_t key;
    uint8_t envelopeVolume;
    uint8_t envelopeGoal;
    uint8_t envelopeCounter;
    uint8_t pseudoEchoVolume;
    uint8_t pseudoEchoLength;
    uint8_t midiKey;
    uint8_t velocity;
    uint8_t priority;
    int8_t rhythmPan;
    uint8_t gateTime;
    uint8_t sustainGoal;
    uint8_t length;
    uint8_t sweep;
    uint8_t dutyCycle;
    uint8_t pan;
    uint8_t panMask;
    uint8_t modify;

    /* Square-1 (NR10) hardware frequency-sweep unit, modeled on mGBA's
     * gb/audio.c.  `sweep` above holds the NR10 byte captured at note-on
     * (0x08 = inert): bits 6-4 = step time in 128 Hz clocks (0 -> 8),
     * bit 3 = direction (1 = down), bits 2-0 = shift.  Every `time` clocks
     * the unit recomputes the frequency from its internal shadow register,
     * f' = f +/- (f >> shift); an upward sweep overflowing 2047 silences the
     * channel (sweepMuted) until the next retrigger.  CgbSound rewrites NRx4
     * with the trigger bit whenever a channel's volume changes (MO_VOL), so
     * the unit is re-armed -- shadow reloaded from the frequency register,
     * timer reset, overflow recheck -- at note start, every envelope phase
     * transition, and every track volume change. */
    uint16_t sweepShadowFreq;
    uint8_t sweepStep;       /* countdown to the next sweep calculation */
    bool sweepEnabled;
    bool sweepMuted;
    float sweepClockAccum;   /* fractional 128 Hz clock, advanced per sample */

    uint32_t frequency;
    uint32_t phase;         /* phase accumulator for synthesis; for the noise
                             * channel, the Q16 fraction (< 0x10000) of the
                             * current LFSR period already elapsed */
    uint32_t *wavePointer;  /* programmable wave data */

    /* Cached per-sample phase increment.  The increment derived from
     * `frequency` is constant between tick-rate frequency updates, so it is
     * recomputed (a couple of float divides) only when `frequency` changes
     * rather than every rendered sample.  phaseIncFreq holds the `frequency`
     * value phaseInc was computed for; a sentinel of 0xFFFFFFFF (set at note
     * start) forces a recompute on the first sample of a note.  For the noise
     * channel, phaseInc is Q16.16 LFSR clocks per output sample (the LFSR can
     * clock up to ~11x faster than the output rate, so it needs an integer
     * part). */
    uint32_t phaseInc;
    uint32_t phaseIncFreq;

    /* Cached DC-offset sum of the programmable-wave table (sum of all 32 raw
     * nibbles).  Depends only on wavePointer contents, so it is recomputed
     * only when wavePointer changes rather than every sample. */
    int32_t waveSum;
    uint32_t *waveSumPointer;

    uint16_t lfsr;          /* noise LFSR state */

    /* GBA hardware envelope unit (NRx2 low bits + NRx4 trigger), modeled on
     * mGBA's gb/audio.c.  CgbSound drives the AUDIBLE volume through this
     * unit: every MO_VOL apply rewrites NRx2 as (envelopeVolume << 4) |
     * hwEnvStepDir and sets the trigger bit, reloading the unit.  Between
     * writes the unit steps on its own free-running 64 Hz clock (one volume
     * step per `stepTime` clocks), while the software envelope is silent
     * bookkeeping that only surfaces at phase transitions.  Percussive
     * envelopes (sustain 0) therefore decay ~2x faster than the software
     * envelope alone, with a per-note phase variation (the 64 Hz clock is not
     * reset by the trigger).  Types 1, 2, 4 only; the wave channel has no
     * hardware envelope (NR32 volume shift, driven per-frame by MO_VOL). */
    uint8_t hwEnvStepDir;   /* NRx2 low nibble: step time 0-7, 0x08 = increase */
    uint8_t hwEnvVolume;    /* current hardware volume 0-15: the audible level */
    uint8_t hwEnvNextStep;  /* 64 Hz clocks until the next step */
    uint8_t hwEnvDead;      /* nonzero = unit stopped (step time 0 or clamped) */
    float hwEnvClockAccum;  /* fractional free-running 64 Hz clock */

    int trackIndex;
    /* Started by a host audition (engine->auditionNote at note-on): stays
     * audible in the polyphony-overflow invert mode. */
    bool audition;

    /* Wave channel (type 3) declick: avoids a pop when the note ends by
     * smoothly fading the last sample to zero over DECLICK_SAMPLES frames. */
    int32_t declickSample;           /* last rendered sample, pre-pan, post-scale */
    int32_t declickSamplesRemaining; /* countdown; 0 = no declick active */
} M4ACGBChannel;

/* Polyphony-overflow debug events.  One is recorded whenever a sound is lost
 * to the polyphony limit: a note-on that found no channel (DROPPED), an
 * actively-sounding note cut off by a new note stealing its channel (STOLEN),
 * or a releasing note whose tail was cut short the same way (TAIL_CUT). */
#define M4A_POLY_DROPPED  0  /* note never sounded: no channel could be taken */
#define M4A_POLY_STOLEN   1  /* active note cut off by a new note */
#define M4A_POLY_TAIL_CUT 2  /* releasing note's tail cut off by a new note */

/* Sentinel for M4APolyEvent.tick: the host was not sequencing when the sound
 * was lost (live/preview note, or a host that never sets polyEventClock). */
#define M4A_POLY_TICK_NONE 0xFFFFFFFFu

typedef struct {
    uint8_t type;       /* M4A_POLY_* */
    uint8_t trackIndex; /* track whose sound was lost */
    uint8_t midiKey;    /* MIDI key of the lost sound */
    uint8_t byTrack;    /* STOLEN/TAIL_CUT: track of the note that took the channel */
    uint8_t program;    /* losing track's program (voicegroup index) at event time */
    uint32_t tick;      /* host timeline position (copied from polyEventClock).
                         * DROPPED: the dropped note's own tick; STOLEN/TAIL_CUT:
                         * the tick of the note-on that took the channel. */
} M4APolyEvent;

/* Ring-buffer capacity for recent overflow events (power of two not required;
 * the ring index is polyEventTotal % capacity). */
#define M4A_POLY_EVENT_CAPACITY 64

/* Forward declaration */
typedef struct M4AEngine M4AEngine;

#include "m4a_reverb.h"

/* Engine state */
struct M4AEngine {
    M4ATrack tracks[MAX_TRACKS];
    /* First MAX_*_CHANNELS entries are the real channels; the second half is
     * the shadow pool used only by the polyphony-overflow debug mode. */
    M4APCMChannel pcmChannels[TOTAL_PCM_CHANNELS];
    M4ACGBChannel cgbChannels[TOTAL_CGB_CHANNELS];
    M4AReverb reverb;

    float sampleRate;
    float samplesPerTick;
    float tickAccumulator;

    /* DirectSound (PCM) mixing rate.  On real hardware the m4a engine mixes all
     * PCM channels at a fixed low rate (SOUND_MODE_FREQ, 13379 Hz in pokeemerald),
     * so high notes alias heavily -- part of the GBA's characteristic sound.
     * PCM channels are mixed + reverbed at this rate into an intermediate signal,
     * then linearly upsampled to sampleRate.  0 means "follow sampleRate" (clean,
     * alias-free mixing).  Default 13379 Hz for hardware accuracy. */
    float pcmMixRate;
    /* Linear-interpolation state for upsampling the PCM mix to the host rate. */
    float pcmResampleAccum;
    int32_t pcmPrevL, pcmPrevR;
    int32_t pcmCurL, pcmCurR;

    uint8_t masterVolume;   /* 0-15 */
    uint8_t songMasterVolume; /* 0-127 */
    uint8_t maxPcmChannels; /* active PCM channel count */
    uint8_t c15;            /* counter 0-14 for CGB envelope double-step */

    /* Opt-in effect features (off by default; toggled from the GUI/config).
     * These extend the base m4a behavior and are not enabled in the stock
     * pokeemerald/pokefirered engine, so they are gated behind flags. */
    bool respectBaseMidiKey; /* PCM voices: treat voice->key as the sample's base
                              * MIDI note and transpose so the pressed key plays
                              * at the intended pitch (eventide-style behavior). */
    bool portamentoEnabled;  /* honor PORTAMENTO (CC 5) glides between notes */
    bool pwmEnabled;         /* honor pulse-width modulation (CC 0x17/0x19) */
    bool pwmActiveFlag;      /* true while any track has pulse-width modulation running */

    /* Polyphony-overflow debugging.  Overflow events (dropped notes, stolen
     * channels) are always counted; when polyDebugInvert is on, the normal
     * audio output is muted and only the lost sounds are audible, played on
     * the shadow channel pool.  The real channels keep running (muted) so the
     * engine's allocation behavior is identical to normal playback.
     * Written by the audio thread, read by the GUI thread without locking:
     * all fields are small scalars, so torn reads are benign for a monitor.
     * polyEventTotal is incremented after the ring entry is filled in, so the
     * GUI never sees a half-written event. */
    bool polyDebugInvert;
    /* Host-set timeline clock (ticks) stamped into new poly events, or
     * M4A_POLY_TICK_NONE when not sequencing.  A sequencing host sets this
     * directly (like maxPcmChannels) before each note-on it dispatches.
     * Single writer: must be written from the same thread that drives
     * note-ons (the audio thread), never from a GUI thread. */
    uint32_t polyEventClock;
    /* Host-set flag marking the next note-on(s) as auditions (live preview
     * notes, not sequenced playback): the channels they start stay audible
     * in the invert mode, so the user can play against the lost sounds
     * they're investigating.  Same single-writer contract as
     * polyEventClock; hosts that never set it (default false) get the
     * plain invert behavior. */
    bool auditionNote;
    uint32_t polyDropCount[MAX_TRACKS];    /* notes that never sounded */
    uint32_t polyStealCount[MAX_TRACKS];   /* active notes cut off */
    uint32_t polyTailCutCount[MAX_TRACKS]; /* releasing tails cut off */
    uint32_t polyEventTotal;               /* total events; ring head = total % capacity */
    M4APolyEvent polyEvents[M4A_POLY_EVENT_CAPACITY];

    /* GBA analog output emulation: IIR low-pass filter */
    bool analogFilter;      /* enable/disable the hardware output filter */
    float lowPassLeft;
    float lowPassRight;

    /* Tempo system (matches GBA MPlayMain tempo accumulator).
     * tempoD = base tempo (ply_tempo param * 2), default 150.
     * tempoU = user tempo multiplier (default 0x100 = 1.0x).
     * tempoI = (tempoD * tempoU) >> 8, the effective tempo increment.
     * tempoC = accumulator, incremented by tempoI each VBlank.
     * When tempoC >= 150, one "tempo tick" fires (LFO advances). */
    uint16_t tempoD;
    uint16_t tempoU;
    uint16_t tempoI;
    uint16_t tempoC;

    /* Loaded voice data */
    ToneData *voiceGroup;   /* array of 128 ToneData entries */
};

/* Engine lifecycle */
void m4a_engine_init(M4AEngine *engine, float sampleRate);
void m4a_engine_destroy(M4AEngine *engine);

/* Set the DirectSound (PCM) mixing rate in Hz.  Pass 0 to follow the host
 * sample rate (clean, alias-free mixing); pass 13379 for GBA-accurate aliasing.
 * Reinitializes the reverb delay line for the new rate; call while stopped for a
 * glitch-free change (active notes correct their pitch on the next event). */
void m4a_engine_set_pcm_mix_rate(M4AEngine *engine, float rate);

/* Set voicegroup (must be loaded by voicegroup_loader) */
void m4a_engine_set_voicegroup(M4AEngine *engine, ToneData *voiceGroup);

/* Re-copy voiceGroup[currentProgram] into each track's currentVoice.
 * Call after editing voicegroup entries to propagate changes to active tracks. */
void m4a_engine_refresh_voices(M4AEngine *engine);

/* MIDI event handling */
void m4a_engine_note_on(M4AEngine *engine, int trackIndex, uint8_t key, uint8_t velocity);
void m4a_engine_note_off(M4AEngine *engine, int trackIndex, uint8_t key);
void m4a_engine_program_change(M4AEngine *engine, int trackIndex, uint8_t program);
void m4a_engine_cc(M4AEngine *engine, int trackIndex, uint8_t cc, uint8_t value);
void m4a_engine_pitch_bend(M4AEngine *engine, int trackIndex, int16_t bend);
void m4a_engine_all_notes_off(M4AEngine *engine, int trackIndex);
void m4a_engine_all_sound_off(M4AEngine *engine);

/* Forget every track's portamento note history (the "previous note" a glide
 * would start from).  Called automatically by all_notes_off/all_sound_off;
 * also call on DAW transport stop so the first note after the playhead moves
 * doesn't glide from a note that was sounding before playback paused. */
void m4a_engine_reset_portamento(M4AEngine *engine);

/* Toggle the opt-in effect features at runtime.  Disabling portamento or
 * pulse-width modulation also clears any in-progress effect state (active
 * glides, modulated duty cycles) so toggling mid-playback is glitch-free. */
void m4a_engine_set_portamento_enabled(M4AEngine *engine, bool enabled);
void m4a_engine_set_pwm_enabled(M4AEngine *engine, bool enabled);

/* Polyphony-overflow debug mode: when enabled, normal playback is muted and
 * only the sounds lost to the polyphony limit are audible (dropped notes and
 * the remainders of stolen notes, played on the shadow channel pool).
 * Disabling kills any shadow channels.  Overflow statistics are collected
 * regardless of this flag. */
void m4a_engine_set_poly_debug_invert(M4AEngine *engine, bool enabled);

/* Clear the overflow counters and the recent-event ring. */
void m4a_engine_reset_poly_stats(M4AEngine *engine);

void m4a_engine_set_song_volume(M4AEngine *engine, uint8_t volume);

/* Set tempo from DAW BPM.  The GBA relationship is tempoI ≈ BPM
 * (24 ticks per quarter note at ~59.7 Hz VBlank gives BPM ≈ tempoI). */
void m4a_engine_set_tempo_bpm(M4AEngine *engine, double bpm);

/* Audio processing */
void m4a_engine_process(M4AEngine *engine, float *outL, float *outR, int numSamples);

/* Internal: engine tick (~60Hz) */
void m4a_engine_tick(M4AEngine *engine);

/* Internal: track volume/pitch calculation (matches TrkVolPitSet) */
void m4a_track_vol_pit_set(M4ATrack *track);

/* Frequency helpers */
uint32_t m4a_midi_key_to_freq(WaveData *wav, uint8_t key, uint8_t fineAdjust);
uint32_t m4a_midi_key_to_cgb_freq(uint8_t chanNum, uint8_t key, uint8_t fineAdjust);

/* 32x32->high32 multiply (matches GBA umul3232H32) */
static inline uint32_t umul3232H32(uint32_t a, uint32_t b)
{
    return (uint32_t)(((uint64_t)a * (uint64_t)b) >> 32);
}

#ifdef __cplusplus
}
#endif

#endif /* M4A_ENGINE_H */
