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

/* engine->auditionVolume / channel auditionVolume: no override, use the
 * track's own VOL. */
#define M4A_AUDITION_VOL_NONE 0xFF
/* engine->auditionPan / channel auditionPan: no override, use the track's own
 * PAN. Outside the -64..63 a PAN event can produce, so no real pan collides
 * with it. */
#define M4A_AUDITION_PAN_NONE 127

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
/* TONEDATA_TYPE_REV: the sample plays backwards, from its last sample to its
 * first (voice_directsound_alt, cry_reverse).  Loops are ignored: the
 * channel ends when it runs off the front of the data. */
#define VOICE_TYPE_REV              0x10
/* TONEDATA_TYPE_CMP: the voice's WaveData is DPCM-compressed (the `cry`
 * macros).  Routes the channel through SoundMainRAM_Unk1, which decodes
 * when the WaveData.type header field is nonzero and mixes nothing when it
 * is zero (the loader clears the bit when a compressed sample fell back to
 * its uncompressed source). */
#define VOICE_TYPE_CMP              0x20

/* DPCM (aif2pcm/wav2agb `--compress`) sample layout: 64 samples per block,
 * 33 bytes each -- one raw sample, then 4-bit deltas (the second byte's low
 * nibble first, then high nibble before low nibble for the rest) indexed into
 * gDeltaEncodingTable.  Matches SoundMainRAM_Unk2 in m4a_1.s. */
#define M4A_DPCM_BLOCK_SAMPLES      64
#define M4A_DPCM_BLOCK_BYTES        33

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
#define CHN_SPECIAL     0x20  /* SoundMainRAM_Unk1 has set the channel up (REV flip, CMP index) */
#define CHN_LOOP        0x10
#define CHN_IEC         0x04
#define CHN_ENV_MASK    0x03
#define CHN_ENV_ATTACK  0x03
#define CHN_ENV_DECAY   0x02
#define CHN_ENV_SUSTAIN 0x01
#define CHN_ENV_RELEASE 0x00
#define CHN_ON          (CHN_START | CHN_STOP | CHN_IEC | CHN_ENV_MASK)

/* A channel's trackIndex after RealClearChain nulled SoundChannel.track:
 * no track's VOLCHG / PITCHG pass or ply_fine selects it, and it ranks
 * below every track in ply_note's equal-priority tie-break (a null pointer
 * compares lowest on the GBA). */
#define M4A_NO_TRACK    (-1)

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
    uint32_t sampleStart;        /* MusicPlayerTrack.unk_3C: XCMD 0x0D's word, the sample
                                  * offset ply_note hands the next DirectSound channel as
                                  * its count (SetPokemonCryProgress; 0 everywhere in
                                  * vanilla Emerald) */
    uint8_t xcmdSelect;          /* extended command chosen by CC 0x1E, fired by CC 0x1D/0x1F */
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
    /* Per-tick command batching on the MIDI path (plan, milestone 5 slice
     * 6): a VOL / PAN / BEND / BENDR / TUNE / MODT / clear_modM and an LFO
     * step only flag the track, as ply_vol & co. do -- its pending
     * MPT_FLG_VOLCHG / PITCHG (M4A_TRK_VOLCHG / PITCHG).  A note that takes
     * a channel drops them (ply_note's flags &= 0xF0); MPlayMain's VOLCHG /
     * PITCHG pass after the frame's ticks applies what is left to every
     * channel the track still owns.  finished: m4a_engine_track_end
     * (ply_fine) has run, until the track's next note. */
    uint8_t chgFlags;
    bool finished;
} M4ATrack;

#define M4A_TRK_VOLCHG 0x03
#define M4A_TRK_PITCHG 0x0C

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

    /* Sample playback (SoundChannel.count / fw / frequency / wav /
     * currentPointer).  currentPointer is a pointer into wav->data, or, for
     * a DPCM-compressed wave once SoundMainRAM_Unk1 has set the channel up
     * (CHN_SPECIAL), data + <sample index>.  frequency is the MidiKeyToFreq
     * result; the mixer multiplies it by SoundInfo.divFreq per sample. */
    WaveData *wav;
    int8_t *currentPointer;
    int32_t count;          /* remaining samples */
    uint32_t fw;            /* fractional position (23-bit fraction) */
    uint32_t frequency;     /* MidiKeyToFreq(wav, key, pitch) */
    /* SoundChannel.xpi: the DPCM block held in engine->decodingBuffer by
     * this channel's last decode (0xFF000000 = none). */
    uint32_t xpi;

    /* Owner (SoundChannel.track): M4A_NO_TRACK once ply_fine's RealClearChain
     * has released it from its track while it still sounds. */
    int trackIndex;
    /* Started by a host audition (engine->auditionNote at note-on): stays
     * audible in the polyphony-overflow invert mode. */
    bool audition;
    /* The track VOL this note was keyed at (already scaled by
     * songMasterVolume), or M4A_AUDITION_VOL_NONE to follow the track's own.
     * An audition belongs to the point in the song it was taken from, so it
     * keeps this volume for its whole life -- through the track volume
     * changes and LFO recalculations that would otherwise pull it onto
     * whatever VOL the track currently holds. */
    uint8_t auditionVolume;
    /* The track PAN this note was keyed at (-64..63), or M4A_AUDITION_PAN_NONE
     * to follow the track's own. Kept for the note's whole life for the same
     * reason as the volume: an audition belongs to the point in the song it
     * was taken from. */
    int8_t auditionPan;
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

    uint32_t frequency;
    /* Set from the voice's TONEDATA_TYPE_FIX bit (the `_alt` PSG voice macros:
     * voice_square_1_alt, voice_square_2_alt, voice_programmable_wave_alt).
     * When set, every driver-side (MO_PIT) frequency write to this channel is
     * quantized -- see m4a_cgb_fix_freq() in m4a_engine.c for the rounding,
     * the sweep-unit exclusion and why it stays false for the noise channel. */
    bool fixedFreq;
    uint32_t *wavePointer;  /* programmable wave data */
    /* The wave table last uploaded to the wave RAM: CgbSound's
     * currentPointer.  m4a rewrites wave RAM at note start only when the
     * voice's table differs from what is already loaded. */
    uint32_t *currentWavePointer;
    /* CgbChannel.n4: the NRx4 value CgbSound builds (length-enable bit 6,
     * trigger bit 7 for the wave channel's first MO_VOL, frequency high
     * bits). */
    uint8_t n4;
    /* Polyphony-debug: this channel was copied to the shadow pool mid-note
     * and its PSG still needs the start-frame registers (m4a_channel.c). */
    bool hwResync;

    int trackIndex;
    /* Started by a host audition (engine->auditionNote at note-on): stays
     * audible in the polyphony-overflow invert mode. */
    bool audition;
    /* The track VOL and PAN this note was keyed at; see the PCM channel's
     * fields. */
    uint8_t auditionVolume;
    int8_t auditionPan;
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

/* One latched MIDI-path event (M4AEngine.eventQueue). */
enum {
    M4A_EV_NOTE_ON = 1,
    M4A_EV_NOTE_OFF,
    M4A_EV_CC,
    M4A_EV_PROGRAM,
    M4A_EV_BEND,
    M4A_EV_XCMD,
    M4A_EV_TRACK_END
};
typedef struct {
    uint8_t kind;    /* M4A_EV_* */
    uint8_t track;
    uint8_t a;       /* key / cc / program / xcmd */
    uint8_t b;       /* velocity / value */
    int16_t bend;    /* M4A_EV_BEND */
} M4AQueuedEvent;
/* Enough for a DAW block's worth of events; a full queue is applied at
 * once, in order, before the next event is queued (eventQueueOverflow
 * counts it). */
#define M4A_EVENT_QUEUE_CAPACITY 1024

/* Frame hook: called at the start of every VBlank tick (m4a_engine_tick),
 * before the envelopes advance -- where MPlayMain runs inside SoundMain on
 * the GBA.  Installed by the song sequencer (m4a_sequencer.c). */
typedef void (*M4AFrameHook)(M4AEngine *engine, void *user);

#include "m4a_reverb.h"
#include "m4a_gba.h"
#include "m4a_psg.h"

/* Engine state */
struct M4AEngine {
    M4ATrack tracks[MAX_TRACKS];
    /* First MAX_*_CHANNELS entries are the real channels; the second half is
     * the shadow pool used only by the polyphony-overflow debug mode. */
    M4APCMChannel pcmChannels[TOTAL_PCM_CHANNELS];
    M4ACGBChannel cgbChannels[TOTAL_CGB_CHANNELS];
    M4AReverb reverb;

    float sampleRate;       /* host output rate */

    /* DirectSound (PCM) mixing rate: the sound-mode frequency
     * (SOUND_MODE_FREQ_xxxxx >> 16, 1..12 into gPcmSamplesPerVBlankTable;
     * pokeemerald uses SOUND_MODE_FREQ_13379 = 4, 224 samples per VBlank).
     * SoundMainRAM mixes the PCM channels at this rate into the DMA ring
     * the timer-driven FIFOs read, so high notes alias as on the GBA.
     * SampleFreqSet's derived numbers follow. */
    uint8_t pcmFreqMode;
    int32_t pcmSamplesPerVBlank; /* samples mixed per VBlank frame */
    int32_t pcmFreq;             /* SoundInfo.pcmFreq for that count */
    int32_t pcmDivFreq;          /* SoundInfo.divFreq */
    uint8_t pcmDmaPeriod;        /* VBlanks per DMA restart (PCM_DMA_BUF_SIZE / spv) */
    int8_t pcmDmaCounter;        /* SoundInfo.pcmDmaCounter */

    /* Output stage: the GBA hardware the mixed PCM and the PSG go through
     * (timer-driven FIFOs, DAC sample grid, bias, host-rate resampler), the
     * PCM ring buffer SoundMain writes (A = right, then B = left, each
     * GBA_PCM_DMA_BUF_SIZE samples), and the scheduler state for the driver's two
     * per-frame interrupts.  See m4a_gba.h and the scheduler notes in
     * m4a_engine.c. */
    M4AGbaAudio gba;
    /* The PSG: [0] is the hardware the real CGB channels drive, [1] the
     * shadow pool's (polyphony-overflow debug); both see the boot writes. */
    M4APsg psg[2];
    /* SoundInfo.pcmBuffer: the DMA ring, A (right) then B (left), int8 as
     * the hardware FIFOs read it. */
    int8_t pcmBuffer[GBA_PCM_DMA_BUF_SIZE * 2];
    /* sDecodingBuffer: the one DPCM block decoder shared by every channel. */
    int8_t decodingBuffer[M4A_DPCM_BLOCK_SAMPLES];
    /* A discarded frame the muted pool mixes into (polyphony debug). */
    int8_t mutedFrame[GBA_PCM_DMA_BUF_SIZE * 2];
    int64_t nextVSync;           /* next m4aSoundVSync (VCOUNT 150 interrupt) */
    int64_t nextSoundMain;       /* next m4aSoundMain (VBLANK interrupt) */
    int bootStep;                /* progress through the boot-time register writes */
    bool booted;                 /* m4a_engine_boot has run */

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

    /* MIDI event latch (milestone 5).  With quantizeEvents set, the event
     * entry points (note on / off, CC, program change, pitch bend, XCMD)
     * received outside a tick are queued and applied, in arrival order, at
     * the top of the next m4a_engine_tick after the gate walk -- where
     * MPlayMain consumes a track's commands -- instead of at the moment of
     * the call.  Off (the default) they apply at once, as before.  inTick is
     * set while m4a_engine_tick runs, so an entry point called from the
     * frame hook (the sequencer's ply_* handlers) always applies at once. */
    bool quantizeEvents;
    bool inTick;
    M4AQueuedEvent eventQueue[M4A_EVENT_QUEUE_CAPACITY];
    int eventQueueCount;
    uint32_t eventQueueOverflow; /* times a full queue was applied early to make room */

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
    /* Host-set track VOL (0-127, before songMasterVolume) for the next
     * audition note-on, or M4A_AUDITION_VOL_NONE (the default) to use the
     * track's own.  Latched into the channel the note-on starts, so it
     * survives the track volume changes and LFO recalculations that follow.
     * Independent of auditionNote (which is about polyphony debugging), and
     * under the same single-writer contract as polyEventClock. */
    uint8_t auditionVolume;
    /* Host-set track PAN (-64..63) for the next audition note-on, or
     * M4A_AUDITION_PAN_NONE (the default) to use the track's own. Latched
     * into the channel the note-on starts, like auditionVolume, so a later
     * PAN event does not drag a sounding audition off the pan it was taken
     * at. The two are independent: either may be set on its own. */
    int8_t auditionPan;
    uint32_t polyDropCount[MAX_TRACKS];    /* notes that never sounded */
    uint32_t polyStealCount[MAX_TRACKS];   /* active notes cut off */
    uint32_t polyTailCutCount[MAX_TRACKS]; /* releasing tails cut off */
    uint32_t polyEventTotal;               /* total events; ring head = total % capacity */
    M4APolyEvent polyEvents[M4A_POLY_EVENT_CAPACITY];

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
    /* m4a_engine_set_tick_tempo: a TEMPO of the coming tick, stored after
     * the next frame's add (ply_tempo's order); unused by the plugin. */
    bool     tempoINextSet;
    uint16_t tempoINext;

    /* Loaded voice data */
    ToneData *voiceGroup;   /* array of 128 ToneData entries */

    /* External sequencer (m4a_sequencer.c).  frameHook runs first in every
     * tick; while externalSequencer is set the engine leaves the per-song-tick
     * work (channel gate times, the tempo accumulator and the LFO) to it. */
    M4AFrameHook frameHook;
    void *frameHookUser;
    bool externalSequencer;
    /* Runs at the end of every SoundMain, after SoundMainRAM (a timing-only
     * sequencer's channel snapshot, m4a_sequencer_init_timing). */
    M4AFrameHook frameEndHook;
    void *frameEndHookUser;
    /* Debug only (poryaaaa_render --pcm-log): runs after frameEndHook. */
    M4AFrameHook frameLogHook;
    void *frameLogHookUser;

    /* SoundMain timing model: CPU cycles from the SoundMain entry to the
     * CgbSound entry this frame.  The sequencer's frame hook sets it from
     * the work MPlayMain did (M4A_CYC_* below), a timing-only sequencer's
     * (m4a_sequencer_init_timing) from the work MPlayMain would do for its
     * song; without either the steady one-track value applies.  See docs/ACCURACY_PLAN.md,
     * "SoundMain timing probe". */
    int32_t cgbEntryCycles;
    int64_t soundMainStamp;      /* cycle of the current SoundMain (-1 outside one) */
    };

/*
 * SoundMain timing model, measured in mgba 0.10.5 on the accuracy test ROM
 * (test/accuracy/ref/mgba_probe.c; docs/ACCURACY_PLAN.md, "SoundMain timing
 * probe").  Cycles relative to the start of VBlank line 160 unless noted.
 */
#define M4A_CYC_SOUNDMAIN_ENTRY    197   /* SoundMain entered (m4aSoundMain +185) */
#define M4A_CYC_CGB_ENTRY_PAUSED   1007  /* CgbSound entry, BGM player paused */
#define M4A_CYC_CGB_ENTRY_BASE     1396  /* CgbSound entry, playing, no tempo tick ... */
#define M4A_CYC_PER_TRACK          6     /* ... + 6 per existing track */
/*
 * MPlayMain, segment by segment (docs/ACCURACY_PLAN.md, "SoundMain timing
 * probe, step 3": every number is the measured delta between two of
 * m4a_1.s's labels; "est." marks a path the corpus never took, estimated
 * from the neighbouring measured path and the instruction difference).
 * The sequencer (m4a_sequencer.c) sums these while it runs MPlayMain's
 * control flow, so the CgbSound entry follows the song state exactly as it
 * does on the GBA.
 */
/* Frame skeleton.  Six players are chained (MPlayOpen order: BGM, SE1, SE2,
 * SE3, two cry players); each MPlayMain calls the previously opened player
 * before its own body, so the BGM player (deepest) runs first, then SE1,
 * SE2, SE3 and the cry players unwind.  With the other players paused, a
 * player p (gMPlayTable index) starts its body at the deeper player's
 * _081DDA6C: BGM at its entry 566, SE1 at 637, SE2 at 700, SE3 at 763
 * (docs/ACCURACY_PLAN.md, "milestone 4 slice 1": measured on the SE corpus,
 * every frame).  m4a_sequencer_set_player derives the per-player values. */
#define M4A_CYC_MP_BGM_ENTRY       566   /* BGM MPlayMain entry (first entry 291, five idle entries 55 apart) */
#define M4A_CYC_MP_HEAD            150   /* BGM: entry -> FadeOutBody 96 -> _081DD9BC 54 */
#define M4A_CYC_MP_PAUSED          71    /* BGM, status < 0: entry -> _081DDA6C */
#define M4A_CYC_MP_IDLE            63    /* a paused player after a deeper one: _081DDA6C -> its own _081DDA6C */
#define M4A_CYC_MP_SE_HEAD         142   /* a playing SE player: the deeper player's _081DDA6C -> FadeOutBody 88 -> _081DD9BC 54 */
#define M4A_CYC_MP_TAIL            55    /* the last player's _081DDA6C -> CgbSound */
#define M4A_CYC_MP_CHAIN           6     /* players in the chain */
#define M4A_CYC_MP_EXIT            (5 * M4A_CYC_MP_IDLE + M4A_CYC_MP_TAIL)   /* BGM: 370 */
/* The cry players (gPokemonCryMusicPlayers, two tracks each) follow SE3 in
 * the chain: cry 0 starts its body at SE3's _081DDA6C 826 and exits in
 * 63 + 55 = 118, measured on four cries (plan, "step 5"); cry 1 (entry
 * 889, exit 55) follows from the same rule, unmeasured. */
#define M4A_CYC_TICK_ALL_DONE      24    /* _081DD9A4 -> _081DDA6C when no track exists any more (clock++, status = PAUSE) */
/* The VOLCHG/PITCHG pass over the player's trackCount tracks (also the
 * no-tick frame's only work). */
#define M4A_CYC_APPLY_ENTER        16    /* _081DD9BC -> _081DD9C4 12 -> _081DD9C8 4 */
#define M4A_CYC_APPLY_NO_TRACK     14    /* _081DD9C8 -> _081DDA62, no MPT_FLG_EXIST */
#define M4A_CYC_APPLY_NO_FLAGS     20    /* ... exists, no VOLCHG/PITCHG */
#define M4A_CYC_APPLY_NEXT         16    /* _081DDA62 -> _081DD9C8 */
#define M4A_CYC_APPLY_LAST         10    /* _081DDA62 -> _081DDA6C */
#define M4A_CYC_APPLY_FLAGGED      30    /* _081DD9C8 -> TrkVolPitSet */
#define M4A_CYC_APPLY_TVPS         62    /* TrkVolPitSet -> _081DD9E6, no VOLSET/PITSET work (+ M4A_CYC_TVPS_* below) */
#define M4A_CYC_APPLY_TVPS_EMPTY   6     /* ... -> _081DDA58 instead, chain empty */
#define M4A_CYC_APPLY_CH           14    /* _081DD9E6 -> _081DD9F6, a sounding channel */
#define M4A_CYC_APPLY_CH_VOL       24    /* VOLCHG: _081DD9F6 -> ChnVolSetAsm ... */
#define M4A_CYC_APPLY_CH_VOL_PCM   72    /* ... -> _081DDA14, DirectSound */
#define M4A_CYC_APPLY_CH_VOL_CGB   74    /* ... -> _081DDA14, CGB (MO_VOL set) */
#define M4A_CYC_APPLY_CH_NOVOL     20    /* no VOLCHG: _081DD9F6 -> _081DDA14 */
#define M4A_CYC_APPLY_CH_PIT       24    /* PITCHG: _081DDA14 -> _081DDA28 ... */
#define M4A_CYC_APPLY_CH_PIT_CGB   232   /* ... MidiKeyToCgbFreq 32 + 200 to _081DDA52 (+ M4A_CYC_CGB_FREQ_LOW_KEY, est. by ply_note's) */
#define M4A_CYC_APPLY_CH_PIT_NOISE 140   /* ... noise: 32 + 108 (est.: the 98 of ply_note's noise call + 10) */
#define M4A_CYC_APPLY_CH_PIT_PCM   263   /* ... _081DDA46 10, MidiKeyToFreq 16 + 237 */
#define M4A_CYC_APPLY_CH_NOPIT     14    /* no PITCHG: _081DDA14 -> _081DDA52 */
#define M4A_CYC_APPLY_CH_LAST      6     /* _081DDA52 -> _081DDA58 */
#define M4A_CYC_APPLY_CH_NEXT      12    /* _081DDA52 -> _081DD9E6, the next chained channel */
#define M4A_CYC_APPLY_CH_DEAD      119   /* a dead channel met here (est.: 14 + ClearChain 12 + 33 + 54 + 6) */
#define M4A_CYC_APPLY_TRACK_END    10    /* _081DDA58 -> _081DDA62 */
/* A tempo tick: the track loop. */
#define M4A_CYC_TICK_ENTER         22    /* _081DD9BC -> _081DD874 14 -> _081DD87C 8 */
#define M4A_CYC_TICK_NO_TRACK      16    /* _081DD87C -> _081DD998, no MPT_FLG_EXIST */
#define M4A_CYC_TICK_TRACK         14    /* _081DD87C -> _081DD886 */
#define M4A_CYC_TICK_CHAIN         12    /* _081DD886 -> _081DD892, chain not empty */
#define M4A_CYC_TICK_NO_CHAIN      18    /* _081DD886 -> _081DD8BA, chain empty */
#define M4A_CYC_WALK_GATE0         20    /* _081DD892 -> _081DD8B4: ON, gate time 0 */
#define M4A_CYC_WALK_GATE_DEC      26    /* ... gate time decremented, still nonzero */
#define M4A_CYC_WALK_GATE_EXPIRE   34    /* ... gate time reaching 0, SF_STOP set */
#define M4A_CYC_WALK_DEAD          59    /* a dead channel: _081DD8AE 14 + ClearChain 12 + RealClearChain 33 ... */
#define M4A_CYC_WALK_UNLINK_00     54    /* ... RealClearChain -> _081DD8B4, prev 0 / next 0 */
#define M4A_CYC_WALK_UNLINK_01     50    /* ... prev 0 / next set */
#define M4A_CYC_WALK_UNLINK_10     56    /* ... prev set / next 0 */
#define M4A_CYC_WALK_UNLINK_11     52    /* ... both set (est.) */
#define M4A_CYC_WALK_NEXT          12    /* _081DD8B4 -> _081DD892 */
#define M4A_CYC_WALK_END           6     /* _081DD8B4 -> _081DD8BA */
#define M4A_CYC_TICK_NO_START      14    /* _081DD8BA -> _081DD938 */
#define M4A_CYC_TICK_START         121   /* ... MPT_FLG_START: Clear64byte 20 + 33, SoundMainBTM 68 */
#define M4A_CYC_WAIT_NO_LFO        28    /* _081DD938 -> _081DD994: wait != 0, lfoSpeed != 0, mod == 0 */
#define M4A_CYC_WAIT_LFOS0         23    /* ... lfoSpeed == 0 (est.) */
#define M4A_CYC_WAIT_LFO_DELAY     40    /* ... lfoDelayC != 0, decremented */
#define M4A_CYC_LFO_STEP           34    /* ... LFO active: _081DD938 -> _081DD95A ... */
#define M4A_CYC_LFO_NEG            26    /* ... -> _081DD972, lfoSpeedC - 0x40 negative (lsls/asrs/b) */
#define M4A_CYC_LFO_POS            24    /* ... -> _081DD972, otherwise (the taken bpl) */
#define M4A_CYC_LFO_SAME           20    /* ... -> _081DD994, modM unchanged */
#define M4A_CYC_LFO_PIT            38    /* ... modM changed, modT 0 (PITCHG) */
#define M4A_CYC_LFO_VOL            36    /* ... modM changed, modT 1/2 (VOLCHG) */
#define M4A_CYC_TICK_TRACK_END     4     /* _081DD994 -> _081DD998 */
#define M4A_CYC_TICK_NEXT          18    /* _081DD998 -> _081DD87C */
#define M4A_CYC_TICK_LAST          10    /* _081DD998 -> _081DD9A4 */
#define M4A_CYC_TICK_EXIT          22    /* _081DD9A4 -> _081DD9BC */
/* Command dispatch (wait == 0). */
#define M4A_CYC_CMD_FETCH          12    /* _081DD938 -> _081DD8E0 */
#define M4A_CYC_CMD_BYTE           21    /* an explicit command byte: _081DD8E0 -> _081DD8EC ... */
#define M4A_CYC_CMD_BYTE_LOW       14    /* ... -> _081DD8F6, byte < 0xBD (the taken bcc) */
#define M4A_CYC_CMD_BYTE_RS        10    /* ... -> _081DD8F6, byte >= 0xBD (running status stored) */
#define M4A_CYC_CMD_RUNNING        25    /* a data byte under running status: _081DD8E0 -> _081DD8F6 */
#define M4A_CYC_CMD_NOTE           34    /* _081DD8F6 -> ply_note */
#define M4A_CYC_CMD_TABLE          50    /* _081DD8F6 -> _081DD90C 10 -> the ply_* handler 40 */
#define M4A_CYC_CMD_WAIT           46    /* a W## byte: _081DD8F6 -> _081DD90C 10 -> _081DD92E 10 -> _081DD938 26 */
#define M4A_CYC_CMD_DEFAULT        90    /* an unmeasured handler to _081DD938 (est.: the one-byte handlers' median) */
/* Song data in IWRAM (gPokemonCrySongs: the cry song, M4ASong.dataInRam):
 * every song-data byte MPlayMain reads costs this less than from ROM
 * (WS0 3/1 with prefetch).  Measured on seven label pairs, ROM song vs
 * cry (plan, "SoundMain timing probe, step 5"): the command fetch 21 / 14,
 * ply_vol 89 / 82, ply_pan 91 / 84, ply_voice 184 / 177, ply_goto 110 / 82
 * (four bytes), ply_xcmd -> sub-handler 61 / 54, ply_note with key and
 * velocity 114 / 93 (three reads).  The M4A_CYC_* handler costs are ROM
 * values; the sequencer subtracts this per byte read for a RAM song. */
#define M4A_CYC_ROM_BYTE           7
/* ply_xcmd's sub-commands (m4a.c, compiled C).  xiecv / xiecl were measured
 * on ROM songs (step 3); the others on the cry (RAM) and are given here as
 * ROM values est. from the measured RAM cost + M4A_CYC_ROM_BYTE per byte
 * read: xrele 48 + 7, xcmd_0D 120 + 28, xwait 108 / 102 + 14. */
#define M4A_CYC_XCMD_SUB           61    /* ply_xcmd -> the sub-command handler (reads the sub-command byte) */
#define M4A_CYC_XIEC               49    /* ply_xiecv / ply_xiecl -> _081DD938 (one byte) */
#define M4A_CYC_XRELE              55    /* ply_xrele -> _081DD938 (one byte; RAM 48 measured) */
#define M4A_CYC_XCMD_0D            148   /* ply_xcmd_0D -> _081DD938 (four bytes; RAM 120 measured) */
#define M4A_CYC_XWAIT              122   /* ply_xwait -> _081DD938 while timer < length (two bytes; RAM 108 measured) */
#define M4A_CYC_XWAIT_DONE         116   /* ... on the tick the wait expires (RAM 102 measured) */
/* ply_endtie: the byte test, then the track's chain walked newest-first
 * until a channel that is ON, not STOP and holds the key.  ROM songs (step
 * 3, rg_vs_trainer): 89 without a key byte and 95 with one, each with one
 * chained channel that matched; the cry (RAM, step 5): 54 without a key
 * byte and an empty chain.  So 61 = 54 + one byte for the empty walk,
 * 28 for the matching channel (89 - 61), 6 for the key byte (95 - 89); a
 * chained channel that does not match is est. (eight instructions). */
#define M4A_CYC_ENDTIE             61    /* ply_endtie -> _081DD938, no key byte, empty chain */
#define M4A_CYC_ENDTIE_KEY         6     /* ... a key byte consumed */
#define M4A_CYC_ENDTIE_MATCH       28    /* ... the chained channel holding the key gets SF_STOP (ends the walk) */
#define M4A_CYC_ENDTIE_SKIP        18    /* ... a chained channel tested and passed over (est.) */
/* ply_fine (measured on the SE corpus, milestone 4 slice 1): the handler
 * returns to the flags test, which finds the track gone and takes the
 * `beq _081DD994`, so its cost ends at _081DD994 instead of _081DD938. */
#define M4A_CYC_FINE_EMPTY         16    /* ply_fine -> ply_fine_done, no chained channel */
#define M4A_CYC_FINE_LOOP          10    /* ply_fine -> ply_fine_loop */
#define M4A_CYC_FINE_CHAN          14    /* ply_fine_loop -> ply_fine_ok: SF_ON tested and SF_STOP set (every chained channel is ON here: the tick's walk dropped the dead ones) */
#define M4A_CYC_FINE_UNLINK_CALL   12    /* ply_fine_ok -> RealClearChain */
#define M4A_CYC_FINE_UNLINK_LAST   50    /* RealClearChain -> ply_fine_done, prev 0 / next 0 (the walk's 54 less the loop's taken bne) */
#define M4A_CYC_FINE_DONE          28    /* ply_fine_done -> _081DD994 */
/* ply_note: entry, voice, channel, setup (handler entry to _081DD938). */
#define M4A_CYC_NOTE_ENTRY         78    /* ply_note -> _081DDB46, no data bytes ... */
#define M4A_CYC_NOTE_KEY_BYTE      19    /* ... + a key byte */
#define M4A_CYC_NOTE_VEL_BYTE      17    /* ... + a velocity byte */
#define M4A_CYC_NOTE_GATE_BYTE     15    /* ... + a gate-extension byte (est.) */
#define M4A_CYC_NOTE_VOICE_PLAIN   26    /* _081DDB46 -> _081DDB9C, the track's own voice */
#define M4A_CYC_NOTE_VOICE_SPLIT   92    /* ... keysplit (table and sub-voice in ROM) */
#define M4A_CYC_NOTE_VOICE_DRUM    111   /* ... rhythm */
#define M4A_CYC_NOTE_VOICE_BAD     94    /* a keysplit/rhythm sub-voice that is itself split: to _081DDCEA (est.: 45 + 35 + 14) */
#define M4A_CYC_NOTE_CGB_PATH      48    /* _081DDB9C -> _081DDBC4 */
#define M4A_CYC_NOTE_PCM_PATH      40    /* _081DDB9C -> _081DDBEC (+7 with a ROM sub-voice) */
#define M4A_CYC_NOTE_ROM_VOICE_PATH 7
#define M4A_CYC_NOTE_CGB_FREE      20    /* _081DDBC4 -> _081DDC40, channel status 0 */
#define M4A_CYC_NOTE_CGB_STOPPING  26    /* ... SF_STOP set */
#define M4A_CYC_NOTE_CGB_PRIO      32    /* ... taken from a lower priority (est.) */
#define M4A_CYC_NOTE_CGB_TRACK     48    /* ... taken by track order at equal priority (mt_chimney frame 348) */
#define M4A_CYC_NOTE_CGB_REJECT_PRIO  34 /* ... rejected: to _081DDCEA (est.) */
#define M4A_CYC_NOTE_CGB_REJECT_TRACK 42 /* ... rejected by track order (est.) */
#define M4A_CYC_NOTE_SCAN_ENTER    14    /* _081DDBEC -> _081DDBFA */
#define M4A_CYC_SCAN_FREE          14    /* a free channel: _081DDBFA -> _081DDC40 */
#define M4A_CYC_SCAN_FIRST_STOP    34    /* ON+STOP, first stopping candidate: -> _081DDC32 32 + 2 */
#define M4A_CYC_SCAN_STOP          24    /* ON+STOP with a stopping candidate: -> _081DDC18 */
#define M4A_CYC_SCAN_ON            20    /* ON, no STOP: -> _081DDC14 ... */
#define M4A_CYC_SCAN_ON_SKIP       10    /* ... -> _081DDC34, a stopping candidate exists */
#define M4A_CYC_SCAN_ON_CMP        4     /* ... -> _081DDC18 */
#define M4A_CYC_SCAN_LOWER         18    /* _081DDC18: strictly lower priority -> _081DDC32 (est. 16) + 2 */
#define M4A_CYC_SCAN_CMP           12    /* _081DDC18 -> _081DDC24 */
#define M4A_CYC_SCAN_HIGHER        8     /* _081DDC24 -> _081DDC34, higher priority (est.) */
#define M4A_CYC_SCAN_LATER_TRACK   20    /* equal priority, later track -> _081DDC32 18 + 2 */
#define M4A_CYC_SCAN_SAME_TRACK    18    /* equal priority, same track (the taken bls, bcc not taken) 16 + 2 */
#define M4A_CYC_SCAN_EARLIER_TRACK 22    /* equal priority, earlier track -> _081DDC34 */
#define M4A_CYC_SCAN_NEXT          12    /* _081DDC34 -> _081DDBFA */
#define M4A_CYC_SCAN_DONE          12    /* _081DDC34 -> _081DDC40 after the last channel */
#define M4A_CYC_SCAN_NONE          14    /* ... -> _081DDCEA, no candidate (est.) */
#define M4A_CYC_NOTE_TAKE          45    /* _081DDC40 -> ClearChain 12 -> RealClearChain 33 ... */
#define M4A_CYC_NOTE_UNLINK_NONE   44    /* ... -> _081DDC54, channel in no chain */
#define M4A_CYC_NOTE_UNLINK_00     68    /* ... chained, prev 0 / next 0 */
#define M4A_CYC_NOTE_UNLINK_01     64    /* ... prev 0 / next set */
#define M4A_CYC_NOTE_UNLINK_10     70    /* ... prev set / next 0 */
#define M4A_CYC_NOTE_UNLINK_11     66    /* ... both set */
#define M4A_CYC_NOTE_CHAIN_EMPTY   4     /* ... + when the track's chain is empty after the unlink */
#define M4A_CYC_NOTE_LFODL0        18    /* _081DDC54 -> _081DDC66, lfoDelay 0 */
#define M4A_CYC_NOTE_LFODL         60    /* ... 24 + clear_modM 36 */
#define M4A_CYC_NOTE_TVPS          114   /* _081DDC66 -> TrkVolPitSet 14 -> ChnVolSetAsm 100 (+ M4A_CYC_TVPS_*) */
#define M4A_CYC_TVPS_VOLSET        82    /* TrkVolPitSet with MPT_FLG_VOLSET ... */
#define M4A_CYC_TVPS_VOLSET_MODT1  4     /* ... + when modT == 1 */
#define M4A_CYC_TVPS_PITSET        50    /* ... with MPT_FLG_PITSET ... */
#define M4A_CYC_TVPS_PITSET_MODT   -2    /* ... when modT != 0 */
#define M4A_CYC_TVPS_ROM_VOICE     25    /* ... type/wav/attack read from a ROM sub-voice (ply_note only) */
#define M4A_CYC_NOTE_CHNVOL        78    /* ChnVolSetAsm -> _081DDCA0 */
#define M4A_CYC_NOTE_CGB_FREQ      248   /* _081DDCA0 -> _081DDCBC 28, MidiKeyToCgbFreq 30 + 190 */
#define M4A_CYC_NOTE_CGB_SWEEP     4     /* ... + a real sweep value (pan bit clear, time bits set): the taken bne */
#define M4A_CYC_CGB_FREQ_LOW_KEY   -10   /* MidiKeyToCgbFreq with key <= 35 (the key = 0 branch): 180 instead of 190 */
#define M4A_CYC_NOISE_FREQ_CLAMP   -4    /* ... noise with key > 80 (clamped to 59): 94 instead of 98 */
#define M4A_CYC_NOISE_FREQ_LOW_KEY -8    /* ... noise with key <= 20 (the key = 0 branch): est. 90 (one taken branch and the sub/clamp compare fewer) */
#define M4A_CYC_NOTE_NOISE_FREQ    156   /* ... noise: 28 + 30 + 98 */
#define M4A_CYC_NOTE_CGB_ROM_VOICE 16    /* ... length/sweep from a ROM sub-voice (est.) */
#define M4A_CYC_NOTE_PCM_FREQ      267   /* _081DDCA0 -> _081DDCCE 12, MidiKeyToFreq 20 + 235 */
#define M4A_CYC_NOTE_STORE         14    /* _081DDCDC -> _081DDCEA */
#define M4A_CYC_NOTE_EXIT          30    /* _081DDCEA -> _081DD938 */
/* Inside CgbSound (test/accuracy/tools/probe_cgb.py over the corpus): the
 * entry to channel 1's loop head, an inactive channel's pass (loop head to
 * the next loop head), and the segment costs of an active pass, which live
 * in m4a_channel.c next to the code they time. */
#define M4A_CYC_CGB_ENTRY_TO_LOOP  53    /* CgbSound -> the channel-1 loop head (c15 decremented) ... */
#define M4A_CYC_CGB_ENTRY_RELOAD   51    /* ... when c15 was 0 and reloads 14 */
#define M4A_CYC_CGB_INACTIVE       44    /* loop head -> next (28) -> next loop head (16), SF_ON clear */

/* Engine lifecycle */
void m4a_engine_init(M4AEngine *engine, float sampleRate);
void m4a_engine_destroy(M4AEngine *engine);

/* Replay the sound hardware's boot as Pokemon Emerald (and the accuracy test
 * ROM) perform it, from reset to the first VBlank interrupt the game catches:
 * the DMA/timer/FIFO register writes of m4aSoundInit at their measured cycle
 * offsets, and the idle first SoundMain.  Afterwards the engine is in the
 * steady state a game is in when it starts its first song, and the next
 * frame's SoundMain is the first that plays.  With emitAudio the boot's
 * output (silence, ~36 ms) is kept so a render lines up with an emulator
 * recording from reset; without it the output is discarded.
 * m4a_engine_process boots quietly on first use if this was not called;
 * call it explicitly, before starting a song, to keep the boot audio. */
void m4a_engine_boot(M4AEngine *engine, bool emitAudio);

/* Set the DirectSound (PCM) mixing rate: a sound-mode frequency index
 * (SOUND_MODE_FREQ_xxxxx >> 16, 1..12; M4A_PCM_FREQ_MODE_DEFAULT is the
 * game's 13379 Hz).  Out-of-range values select the default.  Reprograms
 * the timer and DMA period like m4aSoundMode -> SampleFreqSet; call while
 * stopped (the ring is restarted). */
#define M4A_PCM_FREQ_MODE_MIN     1
#define M4A_PCM_FREQ_MODE_MAX     12
#define M4A_PCM_FREQ_MODE_DEFAULT 4   /* SOUND_MODE_FREQ_13379 */
void m4a_engine_set_pcm_freq_mode(M4AEngine *engine, int mode);
/* The rate in Hz of a mode (SampleFreqSet's pcmFreq: 13379 for mode 4),
 * and the mode whose rate is nearest to `hz` (the default for hz <= 0). */
int m4a_pcm_freq_mode_hz(int mode);
int m4a_pcm_freq_mode_from_hz(double hz);

/* Set voicegroup (must be loaded by voicegroup_loader) */
void m4a_engine_set_voicegroup(M4AEngine *engine, ToneData *voiceGroup);

/* Re-copy voiceGroup[currentProgram] into each track's currentVoice.
 * Call after editing voicegroup entries to propagate changes to active tracks. */
void m4a_engine_refresh_voices(M4AEngine *engine);

/* MIDI event handling */

/* Start a note.  Returns a handle for the channel the note started on --
 * 0..TOTAL_PCM_CHANNELS-1 for pcmChannels[], TOTAL_PCM_CHANNELS + i for
 * cgbChannels[i] -- or M4A_CHANNEL_NONE when no channel was taken (the note
 * was dropped, or the voice could not be resolved). */
#define M4A_CHANNEL_NONE (-1)
/* ... or M4A_CHANNEL_QUEUED when the event latch holds the note for the next
 * tick (m4a_engine_set_quantize_events); the channel is taken then. */
#define M4A_CHANNEL_QUEUED (-2)
int m4a_engine_note_on(M4AEngine *engine, int trackIndex, uint8_t key, uint8_t velocity);
/* The voice ply_note would play for `key` on `voice` (keysplit / rhythm
 * tables resolved), or NULL when the sub-voice is itself a split/rhythm
 * voice (ply_note starts nothing).  For the sequencer's timing model. */
const ToneData *m4a_engine_resolve_voice(const ToneData *voice, uint8_t key);
void m4a_engine_note_off(M4AEngine *engine, int trackIndex, uint8_t key);
void m4a_engine_program_change(M4AEngine *engine, int trackIndex, uint8_t program);
/* The same from another 128-entry voicegroup (NULL: the engine's): a music
 * player whose song names its own voicegroup -- the second cry player's
 * song, whose VOICE 0 is the second cry's ToneData (m4a_sequencer.h,
 * m4a_sequencer_set_voicegroup). */
void m4a_engine_program_change_from(M4AEngine *engine, int trackIndex,
                                    const ToneData *voiceGroup, uint8_t program);
void m4a_engine_cc(M4AEngine *engine, int trackIndex, uint8_t cc, uint8_t value);
/* Extended command (XCMD), already resolved to its gXcmdTable index the way
 * mid2agb resolves a CC 0x1E + CC 0x1D/0x1F pair when it compiles a song.
 * Hosts that know the whole song should resolve the pairs themselves and call
 * this: the selector is compile-time state in mid2agb, so it doesn't rewind at
 * a loop or a seek the way the CC path's running selector does. */
#define M4A_XCMD_IECV 8 /* pseudo-echo volume */
#define M4A_XCMD_IECL 9 /* pseudo-echo length */
void m4a_engine_xcmd(M4AEngine *engine, int trackIndex, uint8_t command, uint8_t value);
void m4a_engine_pitch_bend(M4AEngine *engine, int trackIndex, int16_t bend);
/* ply_fine for a track the host knows has ended (poryaaaa_render: the end of
 * the MIDI track, where mid2agb writes FINE): every channel the track owns
 * gets SF_STOP and no longer belongs to it, its pending changes are dropped
 * and its LFO stops, until a later note on it.  Latched like the others. */
void m4a_engine_track_end(M4AEngine *engine, int trackIndex);
void m4a_engine_all_notes_off(M4AEngine *engine, int trackIndex);
/* Silences everything at once and drops the latched events still queued
 * (a host's panic / transport stop must not release queued note-ons at
 * the next tick).  Called from inside a tick (CC 120 draining) it only
 * silences: the events after it in the same frame keep their order. */
void m4a_engine_all_sound_off(M4AEngine *engine);

/* The MIDI event latch (plan, milestone 5): on, the event entry points
 * queue and the queue drains at the top of the next tick, after the gate
 * walk and before the CgbSound passes, the LFO and the mixer's envelope
 * step -- the frame edge a GBA note can start on.  A latched note-off that
 * finds its note still START (started in the same frame) sets the channel's
 * gateTime to 1 instead of STOP, so the next tick's gate walk stops it: the
 * GBA's one-tick note (mid2agb rounds a shorter note up to one clock) when
 * one frame is one song tick -- this gate counts frames, the GBA's counts
 * song ticks -- where START | STOP would be killed silently by
 * SoundMainRAM.  Turning the
 * latch off applies whatever is queued at once, in order. */
void m4a_engine_set_quantize_events(M4AEngine *engine, bool enabled);
/* Apply every queued event now, in arrival order (what the tick does). */
void m4a_engine_flush_events(M4AEngine *engine);
/* The host-side policy for the latch (the plugin's override, slice 2b-ii):
 * follow the host transport (latched while it plays), always on, or always
 * off.  0 is "transport" so a zeroed or absent setting means the default. */
typedef enum {
    M4A_LATCH_TRANSPORT = 0,
    M4A_LATCH_ON        = 1,
    M4A_LATCH_OFF       = 2
} M4ALatchMode;
/* Apply `mode` given the transport's last state and its new one (pass the
 * same value twice when the host gives no transport): the latch is set on
 * for ON, off for OFF, `playing` for TRANSPORT (turning it off applies what
 * it holds, in order), and on the playing -> stopped edge, in every mode,
 * whatever is still queued is applied before the portamento history is
 * forgotten -- so no note queued before the stop leaves a glide behind. */
void m4a_engine_apply_latch_mode(M4AEngine *engine, M4ALatchMode mode,
                                 bool wasPlaying, bool playing);
/* "transport" / "on" / "off" (also "1" / "0" for on / off; surrounding
 * blanks ignored, case-sensitive); anything else is transport.  For the
 * config file. */
M4ALatchMode m4a_latch_mode_from_name(const char *name);
/* The CLAP state byte: 0 / 1 / 2; anything else is transport. */
M4ALatchMode m4a_latch_mode_from_byte(uint8_t value);
/* A CLAP NOTE_CHOKE: cut `key` on `trackIndex` at once, latch or no latch
 * -- every matching channel killed (releasing or not, shadow pools
 * included) and the matching note-ons / note-offs removed from the queue,
 * the other queued events keeping their order.  A negative trackIndex or
 * key is a wildcard (all tracks / all keys).  Not a queued event, so it
 * never runs inside a drain. */
void m4a_engine_note_choke(M4AEngine *engine, int trackIndex, int key);
/* TrackStop's per-channel silence: PCM channels just stop; CGB channels also
 * get CgbOscOff on their PSG.  `handle` is a note-on return value. */
void m4a_engine_kill_channel(M4AEngine *engine, int handle);

/* Forget every track's portamento note history (the "previous note" a glide
 * would start from).  Called automatically by all_notes_off/all_sound_off;
 * also call on DAW transport stop so the first note after the playhead moves
 * doesn't glide from a note that was sounding before playback paused.  A
 * glide in progress ends at its target pitch on the track's live channels. */
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

/* poryaaaa_render --gba-tempo-grid (plan, milestone 5 slice 3b): the LFO
 * clock on the ROM's tick grid.  start_song_tempo is MPlayStart's seed
 * (tempoI 150, tempoC 0, nothing held), called at the song's origin before
 * frame 3's SoundMain; set_tick_tempo is a TEMPO played in the coming tick
 * (tempoI as ply_tempo stores it), held and stored after the next frame's
 * add, so it first counts on the frame after.  Both are no-ops while a
 * sequencer drives the engine; the plugin uses set_tempo_bpm. */
void m4a_engine_start_song_tempo(M4AEngine *engine);
void m4a_engine_set_tick_tempo(M4AEngine *engine, uint16_t tempoI);

/* Audio processing */
void m4a_engine_process(M4AEngine *engine, float *outL, float *outR, int numSamples);

/* Internal: engine tick (~60Hz) */
void m4a_engine_tick(M4AEngine *engine);

/* Internal: track volume/pitch calculation (matches TrkVolPitSet) */
void m4a_track_vol_pit_set(M4ATrack *track);

/* MPlayMain's end-of-frame pass for one track: TrkVolPitSet, then push the
 * track's volume (ChnVolSetAsm, MO_VOL on CGB) and/or pitch (MidiKeyToFreq /
 * MidiKeyToCgbFreq, MO_PIT) into every channel the track owns.  The song
 * sequencer writes track parameters directly and calls this once per frame
 * for tracks flagged VOLCHG / PITCHG. */
void m4a_engine_track_apply(M4AEngine *engine, int trackIndex, bool volume, bool pitch);

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
