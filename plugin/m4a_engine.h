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
/* TONEDATA_TYPE_SPL: the HQ mixer's synth channel.  Its SF_START pass
 * replaces the channel type byte with this value for a zero-size sample
 * (m4a_hq_channel_init), so the voice's FIX / REV / CMP bits never reach
 * the synth paths.  Vanilla mode classifies synths by synthType alone. */
#define VOICE_TYPE_SPL              0x40

/* DPCM (aif2pcm/wav2agb `--compress`) sample layout: 64 samples per block,
 * 33 bytes each -- one raw sample, then 4-bit deltas (the second byte's low
 * nibble first, then high nibble before low nibble for the rest) indexed into
 * gDeltaEncodingTable.  Matches SoundMainRAM_Unk2 in m4a_1.s. */
#define M4A_DPCM_BLOCK_SAMPLES      64
/* The HQ mixer's frame buffer: the largest sound-mode frame (42048 Hz, 0x2C0 samples). */
#define M4A_HQ_STACK_SP_DEFAULT 0x03007DD0u
#define M4A_HQ_BUFFER_WORDS         0x2C0
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
    /* Host only: how many of the ROM's bytes that precede the sample's
     * data (this entry's 16-byte header, the padding and the previous
     * entry's tail) are kept contiguously in front of `data` (data[-1] ..
     * data[-romBefore]).  ipatix's HQ mixer reads them when a reversed
     * uncompressed sample runs out (docs/HQ-MIXER-ACCURACY.md, section 2);
     * 0 for a sample loaded from a .wav / .aif or built by hand. */
    uint32_t romBefore;
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
    /* Per-tick command batching on the MIDI path (docs/ACCURACY.md, "Rules for every source"): a VOL / PAN / BEND / BENDR / TUNE / MODT / clear_modM and an LFO
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
    /* HQ-mixer mode: CHN_SAMPLE_STOR (SoundChannel byte 0x3F), the base
     * sample of the linear interpolation carried across frames (the HQ
     * mixer points currentPointer at the *next* sample). */
    int8_t sampleStor;
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
    M4A_EV_TRACK_END,
    M4A_EV_TICK_BOUNDARY
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

/* SoundMain's CPU cost for one frame (docs/ACCURACY.md, "SoundMain cost
 * model"): cycles from m4aSoundMain's entry to its return, split as the
 * probe splits them.  sequencer: m4aSoundMain entry -> CgbSound entry
 * (MPlayMain); cgb: CgbSound entry -> SoundMainRAM entry (CgbSound, its
 * return and SoundMain_4/5); mixer: SoundMainRAM entry -> m4aSoundMain's
 * return (the mix and the 10-cycle epilogue after SoundMain returns).
 * total = sequencer + cgb + mixer.  valid is false until a complete
 * frame has been costed and in live mode. */
/* At most one DMA3 copy per real channel and frame (the HQ fast path). */
#define M4A_HQ_DMA3_MAX 16

typedef struct {
    int32_t sequencer, cgb, mixer, total;
    bool valid;
    /* sequencer is the MIDI-path estimate (no M4ASequencer drives the
     * engine and no timing-only one publishes the exact term; docs/ACCURACY.md,
     * "MIDI-path sequencer estimate"), not MPlayMain's exact cost. */
    bool sequencerEstimated;
} M4AFrameCost;

/* The MIDI-path sequencer estimate (docs/ACCURACY.md, "MIDI-path sequencer
 * estimate"; m4a_seq_estimate.c): what MPlayMain would have cost for the
 * events the engine applied, charged with the M4A_CYC_* constants.  Engine
 * state; a host only calls the m4a_engine_cost_* functions below. */
#define M4A_EST_CHANNELS (TOTAL_PCM_CHANNELS + TOTAL_CGB_CHANNELS)
#define M4A_EST_TICKS 8              /* ticks of one frame the estimate tells apart */
typedef struct {
    int32_t cycles;          /* MPlayMain's body since the last SoundMain (head / exit at publication) */
    int32_t held;            /* commands applied in frames without a tick: run at the next tick */
    uint32_t clock;          /* tempo ticks completed (the engine's tempoC / 150 accumulator) */
    uint8_t tickInFrame;     /* m4a_engine_tick_boundary calls since the last SoundMain */
    bool songDeclared;       /* m4a_engine_cost_song_start ran: the existing tracks are the host's */
    bool playing;            /* MusicPlayerInfo.status without PAUSE */
    bool allDone;            /* this frame's tick found no track (TICK_ALL_DONE, then PAUSE) */
    uint8_t trackCount;      /* the player's track count (MusicPlayerInfo.trackCount) */
    int16_t mpEntry, mpHead, mpPaused, mpExit;
    uint16_t declared;       /* the song's tracks (declared, or joined by a note) */
    uint16_t exist;          /* MPT_FLG_EXIST per engine track */
    uint16_t startPending;   /* MPT_FLG_START: the START init at the next tick */
    uint16_t ended;          /* FINE ran in this frame, on tick endTick[] */
    uint16_t firstNote;      /* a note has been seen since the start (mid2agb's implicit VOL) */
    uint16_t volSeen;        /* a VOL came before it */
    uint16_t barLen;         /* mid2agb's WholeNoteMark period in ticks (0: none) */
    uint32_t barStart;       /* the clock of the last time signature's mark */
    uint16_t loopBarLen;     /* the bar marks at the loop start ('['), restored by the GOTO */
    uint16_t loopBarPhase;
    bool loopSaved;
    uint32_t tempoClock;     /* the tick of the last TEMPO charged (UINT32_MAX: none) */
    /* per track: mid2agb's compression state and the W## bookkeeping */
    uint8_t lastOp[MAX_TRACKS];      /* s_lastOpName as the command byte (0: none) */
    uint8_t lastOpKey[MAX_TRACKS];   /* key + 1 of the note whose (predicted) command is lastOp, 0: none */
    uint8_t lastKey[MAX_TRACKS];     /* s_lastNote (0xFF: none) */
    uint8_t lastVel[MAX_TRACKS];     /* s_lastVelocity (0xFF: none) */
    uint8_t extraChg[MAX_TRACKS];    /* VOLCHG / PITCHG from commands the engine never sees (KEYSH, the implicit VOL) */
    uint8_t cmdTicks[MAX_TRACKS];    /* the ticks (bit = tick index in the frame) the track had an event on */
    uint8_t endTick[MAX_TRACKS];     /* the tick index of this frame's FINE */
    bool keep[MAX_TRACKS];           /* s_keepLastOpName: a wait since the last note */
    uint32_t lastEvClock[MAX_TRACKS];
    uint32_t lastWClock[MAX_TRACKS];
    uint16_t lastDur[MAX_TRACKS];    /* the last finished note's length in ticks (0: none) */
    uint32_t keyOn[MAX_TRACKS][128]; /* the clock of each key's note-on (UINT32_MAX: none): its length at the note-off */
    /* the walk steps of channels a command took from a chain after the
     * owner's walk of that tick had met them, per tick of the frame */
    uint8_t ghostWalked[MAX_TRACKS][M4A_EST_TICKS];
    int16_t ghostCycles[MAX_TRACKS][M4A_EST_TICKS];
    /* per channel handle (m4a_engine_note_on's): the track chains */
    uint8_t owner[M4A_EST_CHANNELS]; /* track + 1 while chained */
    bool tie[M4A_EST_CHANNELS];      /* a TIE (predicted, or ended by EOT): gate 0 */
    uint32_t chSeq[M4A_EST_CHANNELS];
    uint32_t chStart[M4A_EST_CHANNELS];  /* the tick of its note (walked from the next one) */
    uint32_t chExpire[M4A_EST_CHANNELS]; /* the tick its N## gate expires (UINT32_MAX: not known) */
    uint32_t seqCounter;
} M4ASeqEstimate;
/* The budgets a host shows the cost against: the VBlank window (68 lines
 * of 1232 cycles) and the whole frame (a SoundMain running past it makes
 * the next one skip). */
#define M4A_VBLANK_CYCLES 83776
#define M4A_FRAME_CYCLES  280896

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
    /* HQ-mixer mode (ipatix's HQ-Mixer rev 4.0 as the pret wiki installs it;
     * docs/HQ-MIXER-ACCURACY.md).  Off (the default) the engine is the
     * vanilla pokeemerald engine.  On, m4a_engine_boot replays the HQ test
     * ROM's boot timeline (the larger IWRAM copy moves m4aSoundInit's
     * register writes, and the final timer-0 start by +4 cycles) and
     * sound_main mixes through m4a_hq_sound_main_ram (m4a_hq_mixer.c)
     * instead of m4a_sound_main_ram.  Set through m4a_engine_set_hq_mixer. */
    bool hqMixer;
    int32_t pcmSamplesPerVBlank; /* samples mixed per VBlank frame */
    int32_t pcmFreq;             /* SoundInfo.pcmFreq for that count */
    int32_t pcmDivFreq;          /* SoundInfo.divFreq */
    /* PCM_DMA_BUF_SIZE of the configuration: the game's 1584 (vanilla at any
     * rate, the HQ mixer at 13379 Hz) or, HQ at another rate, 7 frames as the
     * wiki resizes the define (docs/HQ-MIXER-ACCURACY.md, appendix C). */
    int32_t pcmDmaBufSize;
    uint8_t pcmDmaPeriod;        /* VBlanks per DMA restart (pcmDmaBufSize / spv) */
    int8_t pcmDmaCounter;        /* SoundInfo.pcmDmaCounter */

    /* Output stage: the GBA hardware the mixed PCM and the PSG go through
     * (timer-driven FIFOs, DAC sample grid, bias, host-rate resampler), the
     * PCM ring buffer SoundMain writes (A = right, then B = left, each
     * pcmDmaBufSize samples), and the scheduler state for the driver's two
     * per-frame interrupts.  See m4a_gba.h and the scheduler notes in
     * m4a_engine.c. */
    M4AGbaAudio gba;
    /* The PSG: [0] is the hardware the real CGB channels drive, [1] the
     * shadow pool's (polyphony-overflow debug); both see the boot writes. */
    M4APsg psg[2];
    /* SoundInfo.pcmBuffer: the DMA ring, A (right) then B (left), int8 as
     * the hardware FIFOs read it. */
    int8_t pcmBuffer[GBA_PCM_DMA_BUF_MAX * 2];
    /* sDecodingBuffer: the one DPCM block decoder shared by every channel. */
    int8_t decodingBuffer[M4A_DPCM_BLOCK_SAMPLES];
    /* A discarded frame the muted pool mixes into (polyphony debug). */
    int8_t mutedFrame[GBA_PCM_DMA_BUF_MAX * 2];
    /* HQ-mixer mode (m4a_hq_mixer.c): hq_buffer_ptr, the 32-bit mix buffer
     * (int16 left << 16 | int16 right per output sample) that carries the
     * next frame's reverb seed between frames; the muted pool's copy; the
     * downsampler's noise-shape residuals (SoundInfo bytes 0xE / 0xF); and
     * the count of channel-frames that hit a path the port does not have
     * yet (left untouched for the frame). */
    uint32_t hqBuffer[M4A_HQ_BUFFER_WORDS];
    uint32_t hqMutedBuffer[M4A_HQ_BUFFER_WORDS];
    uint8_t hqNoiseShapeLeft, hqNoiseShapeRight;
    uint32_t hqUnportedChannelFrames;
    /* sp at SoundMainRAM entry: the HQ mixer's pre-cached paths (CMP, REV)
     * decode onto the stack and skip the channel for the frame when
     * 0x03007900 + bytes >= sp - 12 (docs/HQ-MIXER-ACCURACY.md, section
     * 2).  A property of the game's call depth at the interrupt; the test
     * ROM's measured value (0x03007DD0, a 1220-byte budget) is the default. */
    uint32_t hqStackSp;
    /* The HQ fast loop's previous_fast_code (counting only: which of the
     * three step blocks is pasted into C_fast_mixing_loop; 0 none, as after
     * m4aSoundInit's copy): a different block costs the paste
     * (M4A_HQMIX_PASTE). */
    uint8_t hqFastCode;
    /* The HQ fast path's DMA3 copies this frame (counting only): each one's
     * start, cycles after SoundMainRAM's entry, and length. */
    int hqDma3Count;
    int32_t hqDma3Start[M4A_HQ_DMA3_MAX];
    int32_t hqDma3Len[M4A_HQ_DMA3_MAX];
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
    /* m4a_engine_tick_boundary: LFO steps of this frame's ticks already run
     * among its events; the frame's tick loop skips that many. */
    uint8_t  lfoTicksAhead;

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
     * song; without either the steady one-track value applies.  See docs/ACCURACY.md,
     * "SoundMain timing model". */
    int32_t cgbEntryCycles;
    int64_t soundMainStamp;      /* cycle of the current SoundMain (-1 outside one) */
    /* Live mode (m4a_engine_set_live_mode): the ring is mixed lazily just
     * ahead of the DMA's fetch position instead of a frame at a time at
     * SoundMain.  liveMixPos is the ring sample the lazy mix has reached
     * (the same index on both sides; circular). */
    bool liveMode;
    int liveMixPos;

    /* SoundMain cost model (docs/ACCURACY.md, "SoundMain cost model").
     * frameCost: the last completed SoundMain; frameCostMax: the one with
     * the largest total since m4a_engine_reset_frame_cost_max (invalid
     * frames ignored).  Written by plain struct assignment at the end of
     * every SoundMain (before frameEndHook), on the thread that calls
     * m4a_engine_process.  A host reads them on that thread -- right after
     * m4a_engine_process returns, or from frameEndHook -- and hands them on
     * itself (e.g. atomics per field, total written last); never read them
     * from another thread while the engine runs.  One m4a_engine_process
     * call can run several SoundMains: frameCost is then the last of them
     * and frameCostMax covers the others.  The live-mode, HQ-mixer and
     * sound-mode-rate setters and a new voicegroup clear both (frameCost
     * invalid, no peak); a host also resets the peak on play-from-stop
     * (porydaw docs/cpu-meter D8). */
    M4AFrameCost frameCost;
    M4AFrameCost frameCostMax;
    /* Set by m4a_engine_reset_frame_cost_max (any thread), consumed by the
     * next frame (the m4a_engine_process thread).  Only ever accessed
     * through m4a_engine.c's atomic flag helpers (compiler atomic builtins:
     * the header stays plain C a C++ host can include). */
    volatile int32_t frameCostMaxResetPending;
    /* Internal: CgbSound's cursor (real pool) after channel 4, where the
     * next loop head would be; and the vanilla mixer's cycles charged by
     * m4a_sound_main_ram this frame (M4A_MIX_* below, FIFO stall not yet
     * added). */
    int64_t cgbCursorEnd;
    int64_t mixCycles;
    /* The MIDI-path sequencer estimate (M4ASeqEstimate above), and
     * costEstimateForced: publish it even while a timing-only sequencer
     * (m4a_sequencer_init_timing) supplies the exact term (poryaaaa_render
     * --cost-estimate, to measure the estimate against it).  Never used
     * while an M4ASequencer drives the engine. */
    M4ASeqEstimate seqEst;
    bool costEstimateForced;
    };

/*
 * SoundMain timing model, measured in mgba 0.10.5 on the accuracy test ROM
 * (test/accuracy/ref/mgba_probe.c; docs/ACCURACY.md, "SoundMain timing
 * model" and appendix A).  Cycles relative to the start of VBlank line 160 unless noted.
 */
#define M4A_CYC_SOUNDMAIN_ENTRY    197   /* SoundMain entered (m4aSoundMain +185) */
#define M4A_CYC_M4A_SOUNDMAIN      185   /* m4aSoundMain entered (the cost model's zero) */
#define M4A_CYC_CGB_ENTRY_PAUSED   1007  /* CgbSound entry, BGM player paused */
#define M4A_CYC_CGB_ENTRY_BASE     1396  /* CgbSound entry, playing, no tempo tick ... */
#define M4A_CYC_PER_TRACK          6     /* ... + 6 per existing track */
/* cgbEntryCycles (from the SoundMain entry) without a sequencer: the steady one-track value, 1205 */
#define M4A_CYC_CGB_ENTRY_DEFAULT  (M4A_CYC_CGB_ENTRY_BASE + M4A_CYC_PER_TRACK - M4A_CYC_SOUNDMAIN_ENTRY)
/*
 * MPlayMain, segment by segment (docs/ACCURACY.md, appendix A: every number is the measured delta between two of
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
 * (docs/ACCURACY.md, appendix A: measured on the SE corpus,
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
 * 63 + 55 = 118, measured on four cries (docs/ACCURACY.md, appendix A); cry 1 (entry
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
 * cry (docs/ACCURACY.md, appendix A): the command fetch 21 / 14,
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
/* CgbSound's return and SoundMain_4/5 (docs/ACCURACY.md, "SoundMain cost
 * model"; probe columns tail and s2r).  The cost model's cgb term ends at
 * SoundMainRAM's entry, so it holds both. */
#define M4A_CYC_CGB_TAIL           20    /* the cursor after channel 4 (where the next loop head would be) -> CgbSound's return */
#define M4A_CYC_SM_TO_RAM          63    /* CgbSound's return -> SoundMainRAM_Buffer (SoundMain_4/5, bx r3) ... */
#define M4A_CYC_SM_TO_RAM_FIRST    59    /* ... pcmDmaCounter <= 1: the bls SoundMain_5 taken, the slot multiply skipped */

/*
 * The vanilla mixer's cost (SoundMainRAM, m4a_1.s; charged in m4a_mixer.c
 * at the translated labels).  Each value is the cycles between two labels
 * (or one block's instructions), derived from mgba 0.10.5's ARM7 timing
 * with SoundMainRAM, Unk1 and Unk2 running from IWRAM (SoundMainRAM_Buffer)
 * and SoundInfo / the channels / the ring / sDecodingBuffer in IWRAM:
 * an instruction 1, a taken branch or bx 3 (1 not taken), ldr/ldrb/ldrsb
 * from IWRAM 3, str 2, ldm n regs n + 2 (+2 loading pc), stm n + 1, mul
 * 1 + m (m 1..4 from the Rs operand's significant bytes), an 8/16-bit load
 * from the ROM (samples, WaveData's type, gDeltaEncodingTable) 6 (WS0 N 3 +
 * 2 + the fetch; the prefetch buffer does nothing for code in IWRAM), a
 * 32-bit ROM load (WaveData's size / loopStart) 8.  All are "derived" and
 * checked against the probe (`mix` = EXIT - RAM over the vanilla corpus at
 * 13379 and 21024 Hz, tools/cost_diff.py); none needed fitting except
 * where marked.  The FIFO DMA stall (M4A_CYC_FIFO_DMA_STALL) is added
 * across the mixer's span on top (m4a_engine.c, publish_frame_cost).
 */
/* frame */
#define M4A_MIX_ENTRY              7     /* SoundMainRAM -> SoundMainRAM_NoReverb (ldrb reverb, cmp, beq) */
#define M4A_MIX_NOREVERB           11    /* SoundMainRAM_NoReverb -> _Loop, both bcc taken (spv & 4, spv & 8 clear) */
#define M4A_MIX_NOREVERB_4         2     /* ... + spv & 4: two stm instead of the bcc */
#define M4A_MIX_NOREVERB_8         6     /* ... + spv & 8: four stm instead of the bcc */
#define M4A_MIX_NOREVERB_LOOP      20    /* one _Loop pass (16 samples: eight stm, subs, bgt) */
#define M4A_MIX_LOOP_LAST          (-2)  /* any loop's last pass: its backward branch not taken */
#define M4A_MIX_REVERB             15    /* SoundMainRAM -> _081DCEC4 and the exit to _081DCF36 (bx to ARM and back; the loop's last bgt included) */
#define M4A_MIX_REVERB_SAMPLE      28    /* one _081DCEC4 pass: four ldrsb, mul, two strb (26.8 measured incl. the stall) */
#define M4A_MIX_CHAN_SETUP         11    /* _081DCF36 -> SoundMainRAM_ChanLoop */
#define M4A_MIX_SLOT_OFF           30    /* ChanLoop -> _081DCF60 -> _081DD240 -> ChanLoop, SF_ON clear */
#define M4A_MIX_SLOT_LAST          (-2)  /* the last slot's _081DD240: ble taken, no adds / b */
#define M4A_MIX_EXIT               33    /* _081DD24A -> RET (pops; the bx r3 refills from ROM, 4 more than from IWRAM) */
#define M4A_MIX_EPILOGUE           10    /* RET -> EXIT, m4aSoundMain's tail (measured) */
#define M4A_MIX_EXIT_BX            8     /* ... a FIFO stall due in its last 8 cycles (the bx r0 refilling from ROM, and the cycle before) lands after the return (measured: RET + 3 never counted, RET + 2 in 3 frames of 4) */
/* a channel with SF_ON: the envelope (_081DCF6A .. _081DD006) */
#define M4A_MIX_CHAN_ON            20    /* ChanLoop -> _081DCF6A */
#define M4A_MIX_CHAN_NEXT          9     /* _081DD240 -> ChanLoop */
#define M4A_MIX_ENV_START          44    /* SF_START: _081DCF6A -> _081DCFF8 (count / pointer / size from ROM), no loop ... */
#define M4A_MIX_ENV_START_LOOP     5     /* ... + WAVE_DATA_FLAG_LOOP (orrs, strb, b instead of the beq) */
#define M4A_MIX_ENV_START_STOP     8     /* SF_START | SF_STOP: -> _081DCFB0 */
#define M4A_MIX_ENV_END            6     /* _081DCFB0 -> _081DD240: the channel ends */
#define M4A_MIX_ENV_HEAD           10    /* not SF_START: _081DCF6A -> _081DCFA0, the IEC test */
#define M4A_MIX_ENV_IEC            10    /* SF_IEC, echo continues: -> _081DD006 */
#define M4A_MIX_ENV_IEC_END        8     /* SF_IEC, echo length out: -> _081DCFB0 */
#define M4A_MIX_ENV_NOT_IEC        5     /* -> _081DCFB6, the STOP test */
#define M4A_MIX_ENV_RELEASE        14    /* SF_STOP, above the echo volume: -> _081DD006 */
#define M4A_MIX_ENV_RELEASE_LOW    12    /* SF_STOP, at or below it: -> _081DCFC8 */
#define M4A_MIX_ENV_ECHO_START     12    /* _081DCFC8, echo volume set: SF_IEC -> _081DD006 */
#define M4A_MIX_ENV_ECHO_NONE      7     /* _081DCFC8, no echo -> _081DCFB0 */
#define M4A_MIX_ENV_NOT_STOP       6     /* -> _081DCFD6, the envelope state */
#define M4A_MIX_ENV_DECAY          14    /* decay, above sustain: -> _081DD006 */
#define M4A_MIX_ENV_DECAY_SUSTAIN  20    /* decay reaching sustain (nonzero): -> _081DD006 */
#define M4A_MIX_ENV_DECAY_ZERO     16    /* decay reaching sustain 0: -> _081DCFC8 */
#define M4A_MIX_ENV_SUSTAIN        7     /* sustain: _081DCFD6's bne -> _081DCFF4 -> _081DD006 */
#define M4A_MIX_ENV_TO_ATTACK      5     /* attack: -> _081DCFF8 */
#define M4A_MIX_ENV_ATTACK         8     /* _081DCFF8 -> _081DD006, below 0xFF */
#define M4A_MIX_ENV_ATTACK_PEAK    10    /* ... reaching 0xFF (state -> decay) */
#define M4A_MIX_VOL                63    /* _081DD006 -> _081DD03A -> _081DD044's type test (volumes; no loop) ... */
#define M4A_MIX_VOL_LOOP           22    /* ... + SF_LOOP: loop start / length from ROM */
#define M4A_MIX_TO_FIX             10    /* -> _081DD068 -> _081DD07C */
#define M4A_MIX_TO_INTERP          12    /* -> _081DD068 -> _081DD19C */
#define M4A_MIX_TO_UNK1            4     /* the bl SoundMainRAM_Unk1 */
#define M4A_MIX_STORE              11    /* _081DD22C -> _081DD234 -> _081DD240 (count, pointer) */
#define M4A_MIX_STORE_FW           13    /* _081DD228 -> _081DD240 (fw too) */
#define M4A_MIX_RETURN_UNK1        16    /* Unk1's return: b _081DD228 -> _081DD240 */
/* the per-sample work shared by the loops */
#define M4A_MIX_WORD_LOAD          6     /* a word's two ldr from the ring */
#define M4A_MIX_NEXT_SAMPLE        4     /* adds r5 / bcc taken: the word's next sample */
#define M4A_MIX_WORD_STORE         7     /* adds r5 / bcc not taken, two str, subs */
#define M4A_MIX_WORD_STORE_SP      12    /* ... Unk1's: two str and the frame count on the stack (ldr, subs, str) */
#define M4A_MIX_NEXT_WORD          3     /* the bgt back to the word loop (1 when it falls through) */
/* the interpolating loop (_081DD19C) */
#define M4A_MIX_INTERP_SETUP       20    /* _081DD19C -> _081DD1B4: push, the step multiply (+ its m), two ldrsb from ROM */
#define M4A_MIX_INTERP_SAMPLE      13    /* _081DD1BC: the interpolation and both accumulates, to the movs */
#define M4A_MIX_HOLD               3     /* no source advance: beq _081DD208 taken */
#define M4A_MIX_INTERP_STEP1       14    /* advance by one source sample: the next one ldrsb'd from ROM */
#define M4A_MIX_INTERP_STEPN       19    /* advance by more: two ldrsb */
#define M4A_MIX_INTERP_WRAP        32    /* past the end, looping: _081DD134 -> _081DD148 -> _081DD1FC -> _081DD208 */
#define M4A_MIX_WRAP_ITER          6     /* another _081DD148 / _081DD3B0 pass */
#define M4A_MIX_INTERP_END         45    /* past the end, no loop: _081DD134 -> _081DD158 -> _081DD174 -> _081DD240 */
#define M4A_MIX_INTERP_DONE        18    /* the frame done after the bgt: sub, pop, _081DD228 -> _081DD240 */
/* the fixed-frequency loops (_081DD07C) */
#define M4A_MIX_FIX_SHORT          4     /* _081DD07C, count <= 4: -> _081DD0EC */
#define M4A_MIX_FIX_BLOCK          7     /* ... the frame inside the sample: -> _081DD0A8 */
#define M4A_MIX_FIX_BLOCK_END      11    /* ... the sample ends in the frame: the block to its last word */
#define M4A_MIX_FIX_SAMPLE         14    /* _081DD0B0 / _081DD0F4: ldrsb from ROM and both accumulates, to add r7 */
#define M4A_MIX_FIX_BLOCK_DONE     4     /* after the block: adds / beq _081DD22C taken */
#define M4A_MIX_FIX_BLOCK_TAIL     2     /* ... not taken: -> _081DD0EC */
#define M4A_MIX_FIX_TAIL           2     /* _081DD0F4: subs r2 / beq not taken (count left) */
#define M4A_MIX_FIX_WRAP           14    /* count out, looping: _081DD164 -> _081DD118 */
#define M4A_MIX_FIX_END            34    /* count out, no loop: _081DD164 -> _081DD174 -> _081DD240 */
#define M4A_MIX_FIX_TAIL_DONE      4     /* the tail word's bgt not taken, b _081DD22C */
/* SoundMainRAM_Unk1 (TONEDATA_TYPE_CMP / _REV) and Unk2 (the DPCM block) */
#define M4A_MIX_UNK1_HEAD          10    /* Unk1 -> _081DD2B4, SF_SPECIAL set */
#define M4A_MIX_UNK1_FIRST         18    /* ... first frame, forward: SF_SPECIAL set, -> _081DD29C */
#define M4A_MIX_UNK1_FIRST_REV     29    /* ... first frame, reverse (the start from the end, size from ROM) */
#define M4A_MIX_UNK1_FIRST_RAW     10    /* _081DD29C -> _081DD2B4, uncompressed */
#define M4A_MIX_UNK1_FIRST_CMP     12    /* ... compressed (the pointer made an index) */
#define M4A_MIX_UNK1_SETUP         22    /* _081DD2B4 -> the WaveData type test (push, the step: 0x800000, or the multiply + its m) */
#define M4A_MIX_UNK1_RAW           3     /* -> _081DD468 */
#define M4A_MIX_UNK1_CMP           8     /* -> the REV test (xpi = 0xFF000000) */
#define M4A_MIX_UNK1_CMP_FWD       4     /* ... forward: -> _081DD308, around its two Unk2 calls */
#define M4A_MIX_UNK1_CMP_REV       7     /* ... reverse: -> _081DD3C0 -> _081DD3D8, around its two Unk2 calls */
#define M4A_MIX_UNK1_RAW_FWD       14    /* _081DD468, not REV: -> _081DD4F0 and the pop */
#define M4A_MIX_UNK1_RAW_REV       18    /* _081DD468 -> _081DD480 (two ldrsb from ROM) */
#define M4A_MIX_CMP_STEP1          12    /* CMP advance by one: -> _081DD364 / _081DD434, + one Unk2 call */
#define M4A_MIX_CMP_STEPN          12    /* CMP advance by more: -> _081DD358 / _081DD428, + two Unk2 calls */
#define M4A_MIX_CMP_WRAP           31    /* CMP past the end, looping: _081DD398 -> _081DD3B0 -> _081DD358, + two Unk2 calls */
#define M4A_MIX_CMP_END            35    /* CMP forward past the end, no loop: _081DD398 -> _081DD4F4 -> the pop */
#define M4A_MIX_REV_STEP           17    /* raw REV advance: two ldrsb from ROM */
#define M4A_MIX_REV_END            28    /* REV (raw or CMP) past the start: -> _081DD4F4 -> the pop */
#define M4A_MIX_UNK1_DONE          11    /* the frame done after the bgt: sub / add r3, b _081DD4F0, the pop */
#define M4A_MIX_UNK1_DONE_RAW      8     /* raw REV: add r3, the pop (no branch) */
#define M4A_MIX_UNK2_CALL          3     /* a bl SoundMainRAM_Unk2 */
#define M4A_MIX_UNK2_HIT           32    /* Unk2, the block already decoded (push, xpi test, ldrsb, pop pc) */
#define M4A_MIX_UNK2_DECODE        1005  /* Unk2 decoding a 64-sample block: the loop 32 x 14 + 31 x 16 - 2, 46 + 17 around it (+ the block-index multiply's m) */
#define M4A_MIX_UNK2_UNMAPPED      (-99) /* ... a block before the sample (REV past its start): the address wraps to region 0x8C, GBALoad8's default case, 33 byte loads 3 instead of 6 */
/* poryaaaa's extension: a Golden Sun synth voice under the vanilla
 * accumulate (no ROM routine to measure; est. as the interpolating loop
 * with no source fetch). */
#define M4A_MIX_SYNTH_SETUP        20    /* est. */
#define M4A_MIX_SYNTH_SAMPLE       16    /* est. */

/*
 * The HQ mixer's cost (ipatix's HQ-Mixer rev 4.0, test/accuracy/rom/hq/m4a_1.s;
 * charged in m4a_hq_mixer.c at the ported labels).  The same rules as
 * M4A_MIX_*: the whole mixer runs from its IWRAM copy (SoundMainRAM_Buffer),
 * Thumb (the channel walk, the envelope) and ARM (the mixing, the
 * downsampler); SoundInfo, the channels, hq_buffer and the stack (the
 * DMA3 / pre-cache copies of the sample bytes) are IWRAM; the samples, the
 * WaveData headers and the synth descriptors are ROM.  An instruction 1, a
 * taken branch / bx / write to pc 3, ldr/ldrb/ldrsb/ldrsh from IWRAM 3, str
 * 2, ldm n regs n + 2 (+2 loading pc), stm n + 1, mul 1 + m and mla 2 + m
 * (m 1..4 from Rs, mgba's ARM_WAIT_MUL, charged where the operand is
 * known: the mixing loops' mul / mla, the downsampler's reverb products),
 * umlal 3 + m, a ROM byte load 6, a ROM word load 8, an ldm of ROM words
 * 4n (mgba's GBALoadMultiple: S32 - N32 + n (S32 + 1) + 1, WS0 3/1), a
 * failed condition 1, the DMA3 copy 5 per word + 4 (mgba's GBADMAService:
 * 2 + N32 ROM 5 for the first word, 2 + S32 3 for the others, 2 more at
 * the end into IWRAM).  Every value is a label pair counted from the
 * source and checked against the probe (each pair's delta over a probe
 * run breaking on every C_ / F_ label); "est." marks the one with no ROM
 * code.  The FIFO DMA stall is added across the span in m4a_engine.c.
 */
/* frame */
#define M4A_HQMIX_ENTRY            16    /* SoundMainRAM -> C_channel_state_loop (stores, SoundInfo loads) */
#define M4A_HQMIX_SKIP             16    /* C_channel_state_loop -> C_skip_channel (13) -> C_end_channel_state_loop (3): status & 0xC7 clear */
#define M4A_HQMIX_LOOP_NEXT        9     /* C_end_channel_state_loop -> C_channel_state_loop (ldr, sub, ble, add, b) */
#define M4A_HQMIX_LOOP_LAST        (-2)  /* ... the last slot: ble taken -> C_main_mixer_return (7) */
#define M4A_HQMIX_MAIN_RETURN      23    /* C_main_mixer_return -> C_downsampler (noise-shape bytes, reverb, the hint, bx) */
#define M4A_HQMIX_DOWNSAMPLE       17    /* C_downsampler -> C_downsampler_loop (8 + the literal) and the exit (adr, bx; the loop's last bgt included) */
#define M4A_HQMIX_DOWNSAMPLE_PAIR  67    /* one C_downsampler_loop pass (two output samples: 22 noise shaping, ldm/stm, the four ring taps, two mul at m 1) */
#define M4A_HQMIX_RETURN           39    /* C_downsampler_return -> RET (noise-shape stores, ident, the pops, bx r3 to ROM; measured) */
/* the channel walk and the envelope, C_channel_state_loop -> C_channel_vol_calc (Thumb) */
#define M4A_HQMIX_ENV_HEAD         15    /* not SF_START: -> C_adsr_echo_check */
#define M4A_HQMIX_ENV_IEC          14    /* SF_IEC, echo continues: -> C_channel_vol_calc */
#define M4A_HQMIX_ENV_IEC_END      12    /* SF_IEC, echo length out: -> C_stop_channel */
#define M4A_HQMIX_ENV_NOT_IEC      7     /* -> C_adsr_release_check */
#define M4A_HQMIX_ENV_RELEASE      16    /* SF_STOP, above the echo volume: -> C_channel_vol_calc */
#define M4A_HQMIX_ENV_RELEASE_ZERO 11    /* SF_STOP, released to 0 (ble): -> C_adsr_released */
#define M4A_HQMIX_ENV_RELEASE_LOW  14    /* SF_STOP, at or below the echo volume: -> C_adsr_released */
#define M4A_HQMIX_ENV_ECHO_NONE    7     /* C_adsr_released, no echo: -> C_stop_channel */
#define M4A_HQMIX_ENV_ECHO_START   12    /* C_adsr_released, echo: SF_IEC -> C_adsr_save_and_finalize -> C_channel_vol_calc */
#define M4A_HQMIX_ENV_NOT_STOP     4     /* -> C_adsr_decay_check */
#define M4A_HQMIX_ENV_DECAY        17    /* decay, above sustain: -> C_channel_vol_calc */
#define M4A_HQMIX_ENV_DECAY_SUSTAIN 23   /* decay reaching sustain (nonzero): -> C_adsr_next_state -> C_channel_vol_calc */
#define M4A_HQMIX_ENV_DECAY_ZERO   19    /* decay reaching sustain 0: -> C_adsr_released */
#define M4A_HQMIX_ENV_NOT_DECAY    6     /* -> C_adsr_attack_check */
#define M4A_HQMIX_ENV_SUSTAIN      4     /* sustain (or release state without SF_STOP): -> C_channel_vol_calc */
#define M4A_HQMIX_ENV_TO_ATTACK    2     /* attack: -> C_adsr_attack */
#define M4A_HQMIX_ENV_ATTACK       10    /* C_adsr_attack -> C_adsr_save_and_finalize -> C_channel_vol_calc, below 0xFF */
#define M4A_HQMIX_ENV_ATTACK_PEAK  10    /* ... reaching 0xFF: through C_adsr_next_state */
#define M4A_HQMIX_STOP             6     /* C_stop_channel -> C_skip_channel -> C_end_channel_state_loop */
#define M4A_HQMIX_START_STOP       16    /* SF_START | SF_STOP: C_channel_state_loop -> C_stop_channel */
#define M4A_HQMIX_INIT_FWD         48    /* SF_START, uncompressed forward: -> C_channel_init_noncomp_forward (WaveData size and type from ROM) -> C_channel_init_check_loop */
#define M4A_HQMIX_INIT_REV         51    /* ... uncompressed reverse: via C_channel_init_noncomp_reverse */
#define M4A_HQMIX_INIT_CMP_FWD     53    /* ... compressed forward: C_channel_init_comp -> _comp_forward */
#define M4A_HQMIX_INIT_CMP_REV     53    /* ... compressed reverse: C_channel_init_comp -> _comp_reverse */
#define M4A_HQMIX_INIT_SYNTH       42    /* ... size 0: C_channel_init_synth (the type byte from ROM) -> C_channel_init_check_loop */
#define M4A_HQMIX_INIT_SYNTH_TRI   48    /* ... the triangle (type 2): -> C_channel_init_check_loop_no_fine_pos directly (fw = 0x40000000) */
#define M4A_HQMIX_INIT_CHECK       3     /* C_channel_init_check_loop -> _no_fine_pos (fw = 0) */
#define M4A_HQMIX_INIT_FLAGS       19    /* _no_fine_pos -> C_adsr_attack: the stores, WaveData.flags from ROM, no loop ... */
#define M4A_HQMIX_INIT_FLAGS_LOOP  2     /* ... + the loop flag (add, b instead of the beq) */
#define M4A_HQMIX_VOL              48    /* C_channel_vol_calc -> C_mixing_setup: the volume products (mul m 1: env, side <= 0xFF), no loop, the bx to ARM ... */
#define M4A_HQMIX_VOL_LOOP         19    /* ... + SF_LOOP: loop start / end from ROM (ldmia of two ROM words 8), uncompressed */
#define M4A_HQMIX_VOL_LOOP_CMP     18    /* ... + SF_LOOP, compressed (bcs taken, mov) */
/* C_mixing_setup (ARM) */
#define M4A_HQMIX_VOL_ZERO         14    /* both volumes 0: C_mixing_setup -> C_mixing_epilogue (10) -> C_end_channel_state_loop (4) */
#define M4A_HQMIX_TO_COMP_REV      12    /* -> C_mixing_setup_comp_rev (any path but FIX) */
#define M4A_HQMIX_TO_FIX           14    /* -> C_setup_fixed_freq_mixing */
#define M4A_HQMIX_SETUP            30    /* C_mixing_setup_comp_rev -> C_data_load_uncomp_for (+ the step mul's m by divFreq, + umlal's m by the frame length) ... */
#define M4A_HQMIX_SETUP_CMP        (-2)  /* ... -> C_data_load_comp (bcs taken, no b) */
#define M4A_HQMIX_SETUP_UREV       (-1)  /* ... -> C_data_load_uncomp_rev (bmi taken) */
#define M4A_HQMIX_SETUP_SYNTH      14    /* C_mixing_setup_comp_rev -> C_setup_synth (+ the step mul's m) */
#define M4A_HQMIX_END              20    /* C_end_mixing -> C_mixing_end_store (12) -> C_mixing_epilogue (4) -> C_end_channel_state_loop (4) */
#define M4A_HQMIX_END_STORE        8     /* C_mixing_end_store -> C_end_channel_state_loop (FIX) */
#define M4A_HQMIX_END_STOP         9     /* C_mixing_end_and_stop_channel -> C_mixing_epilogue (5) -> C_end_channel_state_loop (4) */
/* the interpolating path, uncompressed forward (C_data_load_uncomp_for) */
#define M4A_HQMIX_COPY             25    /* count > advance, the copy: -> C_select_highspeed_codepath_vla_r3_and3 (the stmia that starts DMA3 included) */
#define M4A_HQMIX_DMA3             4     /* the DMA3 copy onto the stack: 4 + ... */
#define M4A_HQMIX_DMA3_WORD        5     /* ... 5 per word, (advance + 7) >> 2 words */
/* A FIFO refill (both DMAs, M4A_CYC_FIFO_DMA_STALL elsewhere) due in or
 * around the DMA3 copy costs the CPU less.  By p, the refill's due cycle
 * less the copy's start (the stmia's end on mixCycles' clock, moved on by
 * the span's earlier stalls), L = M4A_HQMIX_DMA3 + 5n the copy's halt.
 * Derived from mgba 0.10.5's dma.c / timing.c (a simulation of
 * GBADMAService / GBADMAUpdate / _dmaEvent and the timer event: the stmia's
 * IO store schedules DMA3 at its own start + 3 = p -1, the events due
 * inside the stmia run at its end in (when, priority) order -- DMA3's
 * before a timer at p 0, a timer at p -3..-1 first -- _dmaEvent re-anchors
 * the first word to now, DMA1 / DMA2 preempt the copy between words);
 * every value but L - 1 also seen in the corpus.  p <= -4 or >= L: 18. */
#define M4A_HQMIX_FIFO_PRE         16    /* p -3 .. -1: the refill runs first, then the whole copy (corpus: p -3, -2, -1) */
#define M4A_HQMIX_FIFO_FIRST       9     /* p 0 .. 7: 9 + p, inside the first word's 7 cycles (corpus: p 0, 2, 4, 7) */
#define M4A_HQMIX_FIFO_WORD        12    /* p 8 .. L - 2: 12 + (p - 8) mod 5, the 5-cycle words (corpus: every residue) */
#define M4A_HQMIX_FIFO_LAST        17    /* p L - 1: the copy's last cycle (the simulation only; no corpus frame) */
#define M4A_HQMIX_VLA              2     /* C_select_highspeed_codepath_vla_r3_and3 -> _vla_r3 -> C_select_highspeed_codepath (and, add) */
#define M4A_HQMIX_DIRECT           16    /* count > advance, advance >= 0x400 or no stack: -> C_select_highspeed_codepath (samples from ROM) */
#define M4A_HQMIX_SELECT           16    /* C_select_highspeed_codepath -> C_skip_fast_mixing_creation, the block in place ... */
#define M4A_HQMIX_PASTE            103   /* ... + the block pasted (previous_fast_code differs): 14 + 4 C_fast_mixing_creation_loop passes of 24 - 2 + 3 */
#define M4A_HQMIX_FAST_ENTRY       9     /* C_skip_fast_mixing_creation -> C_fast_mixing_loop (+ the first ldrsb's LOAD_*) */
#define M4A_HQMIX_FAST_ITER        26    /* one C_fast_mixing_loop pass outside its samples: two ldm / stm of 4 words, subs, bgt */
#define M4A_HQMIX_FAST_SAMPLE      9     /* a sample's nine instructions, mulne / mlane / the conditional load not executed ... */
#define M4A_HQMIX_LOAD_STACK       2     /* ... + an executed ldrsb from the stack (IWRAM, 3) */
#define M4A_HQMIX_LOAD_ROM         5     /* ... + an executed ldrsb from ROM (6) */
#define M4A_HQMIX_MLA              1     /* ... + an executed mlane (2 + m in place of 1; + m by the output) */
#define M4A_HQMIX_FAST_EXIT        10    /* after the last bgt: -> C_skip_fast_mixing -> C_end_mixing (ldmfd sp, ldmfd, b) */
#define M4A_HQMIX_UNBUF            12    /* count <= advance: -> C_unbuffered_mixing -> C_unbuffered_mixing_loop (ldrsb from ROM) */
#define M4A_HQMIX_UNBUF_SAMPLE     15    /* one C_unbuffered_mixing_loop pass, no advance, output 0 (+ mul's m by the diff) ... */
#define M4A_HQMIX_UNBUF_MIX        4     /* ... + the output not 0: ldrne, mlane, strne executed (+ the mla's m) */
#define M4A_HQMIX_UNBUF_ADVANCE    11    /* ... + a source advance by one (ldrsb from ROM) */
#define M4A_HQMIX_UNBUF_ADVANCE_N  5     /* ... + by more (ldrnesb from ROM) */
#define M4A_HQMIX_UNBUF_WRAP       11    /* ... + past the end, looping: C_unbuffered_mixing_loop_or_end -> _continue */
#define M4A_HQMIX_UNBUF_END        13    /* ... past the end, no loop (in place of _ADVANCE): -> _loop_or_end -> C_mixing_end_and_stop_channel, the pass's tail not run */
/* the fixed-frequency path (C_setup_fixed_freq_mixing) */
#define M4A_HQMIX_FIX_SETUP        3     /* stmfd {r4, r9} */
#define M4A_HQMIX_FIX_CHECK        42    /* C_fixed_mixing_length_check -> C_fixed_mixing_loop: blocks to mix (the code paste, the first word from ROM) */
#define M4A_HQMIX_FIX_NOBLOCK      7     /* ... -> C_fixed_mixing_process_rest: none (beq taken) */
#define M4A_HQMIX_FIX_BLOCK        30    /* one C_fixed_mixing_loop pass (4 samples: ldm, seven movs, ldmia of one ROM word, four mlane not executed, stm, subs, bne) ... */
#define M4A_HQMIX_FIX_NONZERO      2     /* ... + a nonzero sample's mlane (2 + m 1) */
#define M4A_HQMIX_FIX_REWIND       1     /* after the blocks: sub r3 */
#define M4A_HQMIX_FIX_REST         1     /* C_fixed_mixing_process_rest: mov r1, #4 */
#define M4A_HQMIX_FIX_SAMPLE       20    /* one C_fixed_mixing_unaligned_loop pass (ldr, ldrsb from ROM, mla, str, subs, beq, subs, bgt) */
#define M4A_HQMIX_FIX_WRAP         12    /* ... count out, looping: C_fixed_mixing_loop_or_end -> _continue */
#define M4A_HQMIX_FIX_END          8     /* ... count out, no loop: _loop_or_end (10) -> C_mixing_end_and_stop_channel, the pass's tail not run */
#define M4A_HQMIX_FIX_NEXT         4     /* subs r8, bgt -> C_fixed_mixing_length_check */
#define M4A_HQMIX_FIX_DONE         6     /* subs r8, bgt not taken, ldmfd -> C_mixing_end_store */
/* the pre-cached paths (C_data_load_comp / C_data_load_uncomp_rev) */
#define M4A_HQMIX_COMP             12    /* C_data_load_comp -> C_data_load_comp_for (the store code pasted) */
#define M4A_HQMIX_COMP_REV         14    /* ... -> C_data_load_comp_rev */
#define M4A_HQMIX_COMP_FOR         22    /* C_data_load_comp_for -> _calc_pos (20) -> C_data_load_comp_decode (2) */
#define M4A_HQMIX_COMP_FOR_END     29    /* ... the sample ends in the frame: -> F_clear_mem (27; it returns to _calc_pos) and _calc_pos -> _decode (2) */
#define M4A_HQMIX_COMP_REV_SETUP   27    /* C_data_load_comp_rev -> _rev_calc_pos (22) -> C_data_load_comp_decode (5) */
#define M4A_HQMIX_COMP_REV_END     32    /* ... the sample ends in the frame: -> F_clear_mem (27; it returns to _rev_calc_pos) and _rev_calc_pos -> _decode (5) */
#define M4A_HQMIX_STACK_SKIP       13    /* the stack check fails: C_data_load_comp_for -> C_end_mixing (bhs) ... */
#define M4A_HQMIX_STACK_SKIP_REV   14    /* ... C_data_load_comp_rev / C_data_load_uncomp_rev -> C_end_mixing */
#define M4A_HQMIX_DECODE_SETUP     11    /* C_data_load_comp_decode -> C_data_load_comp_loop (+ the block-address mla's m by the block index; + 1 when the streqb stores: the end frame) */
#define M4A_HQMIX_BLOCK            755   /* one C_data_load_comp_loop pass: bl, F_decode_compressed (19 + 32 x 12 + 31 x 11 + bgt not taken - 2 + ldmfd with pc 6; 33 ROM byte loads), subs, bgt */
#define M4A_HQMIX_BLOCK_OPENBUS    (-99) /* ... a block read from open bus (the reversed end frame's wrapped address): 33 byte loads of mgba's GBALoad8 default case, 3 instead of 6 */
#define M4A_HQMIX_DECODE_DONE      4     /* b C_select_highspeed_codepath_vla_r3 -> C_select_highspeed_codepath (add) */
#define M4A_HQMIX_UREV             22    /* C_data_load_uncomp_rev -> C_data_load_uncomp_rev_loop */
#define M4A_HQMIX_UREV_END         32    /* ... the sample ends in the frame: -> F_clear_mem (27), then ldr, strb (5) */
#define M4A_HQMIX_UREV_WORD        14    /* one C_data_load_uncomp_rev_loop pass (ldmia of one ROM word 4, the byte swap, stmdb, subs, bgt) */
#define M4A_HQMIX_UREV_DONE        6     /* rsb, b -> C_select_highspeed_codepath_vla_r3_and3 -> C_select_highspeed_codepath (and, add) */
#define M4A_HQMIX_CLEAR            16    /* F_clear_mem -> the computed jump into C_clear_loop (stmfd, movs, add pc) */
#define M4A_HQMIX_CLEAR_STORE      5     /* one stmia of 4 words */
#define M4A_HQMIX_CLEAR_RET        11    /* ands, ldmeqfd with pc (no remainder) */
#define M4A_HQMIX_CLEAR_WORD       6     /* C_clear_loop_rest: one word (stmia 2, subs, bgt) */
/* the synth paths (C_setup_synth) */
#define M4A_HQMIX_SYNTH            8     /* C_setup_synth: the type byte from ROM, pulse (bne not taken) */
#define M4A_HQMIX_PULSE_SETUP      37    /* the duty from the descriptor (four ROM bytes, mla m 1), stmfd -> C_synth_pulse_loop */
#define M4A_HQMIX_PULSE_ITER       55    /* one C_synth_pulse_loop pass (8 samples: ldm / stm of 8 words, 4 instructions a sample, subs, bgt) */
#define M4A_HQMIX_PULSE_EXIT       9     /* ldmfd, b C_end_mixing */
#define M4A_HQMIX_SAW              16    /* C_setup_synth -> C_check_synth_saw (10) -> C_synth_saw_loop (subs, bne, four instructions) */
#define M4A_HQMIX_SAW_ITER         15    /* one C_synth_saw_loop pass outside its samples (ldm, stm, subs, bgt) ... */
#define M4A_HQMIX_SAW_SAMPLE       6     /* ... + 6 a sample (mlane not executed: the state 0; + M4A_HQMIX_MLA + m executed) */
#define M4A_HQMIX_TRI              16    /* C_setup_synth -> C_check_synth_saw -> C_synth_triangle -> C_synth_triangle_loop */
#define M4A_HQMIX_TRI_ITER         15    /* one C_synth_triangle_loop pass outside its samples ... */
#define M4A_HQMIX_TRI_SAMPLE       5     /* ... + 5 + m a sample (adds, rsbpl, submi, mla by the wave value) */
#define M4A_HQMIX_SYNTH_EXIT       3     /* the saw / triangle loop's b C_end_mixing */

/* Engine lifecycle */
void m4a_engine_init(M4AEngine *engine, float sampleRate);
/* Internal (exposed for the unit tests): the FIFO refill stalls the
 * mixer's span adds, SoundMainRAM's entry at `ram` and `mix` cycles of
 * charges (docs/ACCURACY.md, "SoundMain cost model"). */
int64_t m4a_engine_mixer_stall(const M4AEngine *engine, int64_t ram, int64_t mix);
/* mul / mla's extra cycles (mgba's ARM_WAIT_SMUL): 1..4 by the significant
 * bytes of the Rs operand, the sign bits not counted; branchless. */
static inline int m4a_arm_mul_m(uint32_t rs)
{
    uint32_t x = rs ^ (uint32_t)((int32_t)rs >> 31);
    return 1 + (x > 0xFFu) + (x > 0xFFFFu) + (x > 0xFFFFFFu);
}
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

/* HQ-mixer mode (docs/HQ-MIXER-ACCURACY.md, section 5): the HQ ROM's boot
 * timeline (call before m4a_engine_boot; the boot replays the table once)
 * and ipatix's mixer in place of SoundMainRAM.  Resets the HQ-only state;
 * the 7884 Hz PCM rate (mode 2) is not supported by the HQ mixer (its
 * frame is not a multiple of 8) and snaps to the default.  Off by default. */
void m4a_engine_set_hq_mixer(M4AEngine *engine, bool on);

/* Live mode, for a host monitoring a keyboard: a note keyed now sounds
 * within a few milliseconds instead of one to two VBlank frames later.
 * The frame tick (MPlayMain, CgbSound, the LFO, the PCM envelope step) still
 * runs once per VBlank; what moves is the PCM mix.  SoundMainRAM mixes a
 * whole frame into the ring slot the DMA reads one frame later; in live
 * mode m4a_engine_process mixes the ring lazily, in M4A_LIVE_PIECE-sample
 * pieces, only M4A_LIVE_AHEAD samples ahead of the DMA's fetch position
 * (the FIFO's prefetch is 16..32 samples), and a PCM channel keyed between
 * ticks gets its start envelope step at the next piece.  A PSG channel
 * keyed between ticks gets its CgbSound start pass at the next scheduler
 * step (1024 cycles) instead of the next tick.  PCM latency is about
 * M4A_LIVE_AHEAD + 32 samples of the PCM rate (~4 ms at 13379 Hz); PSG
 * ~0.1 ms.  Vanilla mixer only: the HQ mixer is turned off while live and
 * cannot be turned on.  Not the hardware's timing: a frame's envelope and
 * LFO step applies from the tick on, not one frame later.  Call while
 * stopped, as the mixer setters. */
#define M4A_LIVE_PIECE 16
#define M4A_LIVE_AHEAD 32
void m4a_engine_set_live_mode(M4AEngine *engine, bool on);

/* Clear frameCostMax (a host's peak reset: play from stop, a Reset button).
 * Safe to call from any thread: it only raises a flag, and the next
 * SoundMain (on the m4a_engine_process thread) clears the peak before
 * publishing its own cost, so frameCostMax is that frame's afterwards. */
void m4a_engine_reset_frame_cost_max(M4AEngine *engine);

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

/* The MIDI event latch (docs/ACCURACY.md, "Entry points and the frame latch"): on, the event entry points
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

/* poryaaaa_render --gba-tempo-grid (docs/ACCURACY.md, the renderer's "as the game" mode): the LFO
 * clock on the ROM's tick grid.  start_song_tempo is MPlayStart's seed
 * (tempoI 150, tempoC 0, nothing held), called at the song's origin before
 * frame 3's SoundMain; set_tick_tempo is a TEMPO played in the coming tick
 * (tempoI as ply_tempo stores it), held and stored after the next frame's
 * add, so it first counts on the frame after.  Both are no-ops while a
 * sequencer drives the engine; the plugin uses set_tempo_bpm. */
void m4a_engine_start_song_tempo(M4AEngine *engine);

/* The MIDI-path sequencer estimate (docs/ACCURACY.md, "MIDI-path sequencer
 * estimate").  Without an M4ASequencer the published frameCost.sequencer is
 * an estimate of MPlayMain's cost from the events the engine applies
 * (frameCost.sequencerEstimated).  What the host can tell it:
 *
 * m4a_engine_cost_song_start: the song starts at the next tempo tick
 * (MPlayStart) with the tracks in trackMask (bit = engine track index: the
 * tracks mid2agb would emit, i.e. those with notes; at most the player's
 * track count are counted) on music player `player` (M4A_PLAYER_*, BGM for
 * a song); bars every 96 ticks from that tick until
 * m4a_engine_cost_time_signature says otherwise.  Without it the estimate
 * infers: a track exists from its first note, the player is BGM, no bar
 * marks.  m4a_engine_cost_song_stop: the player is paused (nothing
 * playing; also the state before any note).
 *
 * m4a_engine_cost_time_signature: a time signature at the coming tick:
 * mid2agb's bar marks restart there, every clocksPerBar ticks
 * (96 * numerator * clocksPerBeat / 2^denominator; 0 = no marks).
 *
 * Without them (the inferred form) m4a_engine_all_sound_off ends the
 * inferred song (the paused player again); in both forms a new voicegroup
 * and m4a_engine_cost_song_stop reset the estimate, and all sound off drops
 * its chains.  m4a_engine_set_tempo_bpm with a changed tempo while a song
 * plays is a TEMPO command.  A host should declare its song (song_start /
 * time signatures / loop points / song_stop) for the lead-in, the bar
 * marks and the GOTOs; events of a frame without a tempo tick count on the
 * next tick.
 *
 * Call them where the events go (the thread that drives the engine,
 * between m4a_engine_process calls). */
void m4a_engine_cost_song_start(M4AEngine *engine, uint32_t trackMask, int player);
/* m4a_engine_cost_song_continue: the song resumes at the next tempo tick
 * part-way through (m4aMPlayContinue after a pause, or a host's seek within
 * the song) with the tracks in trackMask: as song_start, but without the
 * start-of-song work (the START init, KEYSH, mid2agb's implicit VOL) and
 * with every track's compression state unknown.  Bars every 96 ticks from
 * that tick until a time signature says otherwise; declare the meter in
 * force with m4a_engine_cost_time_signature_phase. */
void m4a_engine_cost_song_continue(M4AEngine *engine, uint32_t trackMask, int player);
void m4a_engine_cost_song_stop(M4AEngine *engine);
void m4a_engine_cost_time_signature(M4AEngine *engine, uint32_t clocksPerBar);
/* m4a_engine_cost_time_signature_phase: the meter in force at the coming
 * tick, which lies ticksIntoBar ticks past one of its bar lines (a start or
 * a seek mid-bar): the bar marks fall every clocksPerBar ticks in the
 * song's phase.  time_signature(n) is time_signature_phase(n, 0). */
void m4a_engine_cost_time_signature_phase(M4AEngine *engine, uint32_t clocksPerBar,
                                          uint32_t ticksIntoBar);
/* m4a_engine_cost_loop_start_meter: the meter the song's loop start ('[')
 * has -- clocksPerBar, with the loop start ticksIntoBar ticks past one of
 * its bar lines -- for the GOTO to restore, without the label itself (no
 * command, no compression reset): for a host whose playback starts after
 * the loop start, so loop_point(false) never runs.  Call it after
 * song_start / song_continue, which forget it. */
void m4a_engine_cost_loop_start_meter(M4AEngine *engine, uint32_t clocksPerBar,
                                      uint32_t ticksIntoBar);
/* m4a_engine_cost_loop_point: the song's loop start ('[': mid2agb's label,
 * loopEnd false) or loop end (']': every track's GOTO back to the start,
 * loopEnd true) at the coming tick; call the end before the next pass's
 * events of that tick. */
void m4a_engine_cost_loop_point(M4AEngine *engine, bool loopEnd);
/* Publish the estimate even while a timing-only sequencer supplies the exact
 * term (measurement only: poryaaaa_render --cost-estimate). */
void m4a_engine_set_cost_estimate_forced(M4AEngine *engine, bool forced);
void m4a_engine_set_tick_tempo(M4AEngine *engine, uint16_t tempoI);
/* poryaaaa_render --gba-tempo-grid (docs/ACCURACY.md, the renderer's "as the game" mode): the frame's
 * next song tick starts here.  In a frame with two ticks MPlayMain runs tick
 * n's commands and each track's LFO step before tick n + 1's commands; the
 * host calls this between the two ticks' events, and the LFO step of the
 * tick before it runs now (latched like the other entry points), the
 * frame's tick loop then skipping it.  A no-op while a sequencer drives the
 * engine; the plugin never calls it (it cannot know a frame's ticks). */
void m4a_engine_tick_boundary(M4AEngine *engine);

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
