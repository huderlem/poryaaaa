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

/* Resolve the effective PCM mixing rate: the configured pcmMixRate, or the host
 * sample rate when pcmMixRate is 0 ("follow host"). */
static inline float m4a_pcm_mix_rate(const M4AEngine *engine)
{
    return engine->pcmMixRate > 0.0f ? engine->pcmMixRate : engine->sampleRate;
}

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
 * Track volume and pitch calculation - matches TrkVolPitSet in m4a.c
 */
void m4a_track_vol_pit_set(M4ATrack *track)
{
    /* Volume calculation */
    int32_t x = ((uint32_t)track->volume * track->volX) >> 5;

    if (track->modT == 1)
        x = ((uint32_t)x * (track->modM + 128)) >> 7;

    int32_t y = 2 * track->pan + track->panX;

    if (track->modT == 2)
        y += track->modM;

    if (y < -128) y = -128;
    else if (y > 127) y = 127;

    track->volMR = (uint32_t)((y + 128) * x) >> 8;
    track->volML = (uint32_t)((127 - y) * x) >> 8;

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
    uint32_t velocity = ch->velocity;
    int32_t rhythmPan = ch->rhythmPan;
    uint32_t panR = (uint32_t)(0x80 + rhythmPan);
    uint32_t volR = panR * velocity;
    uint32_t result = (volR * track->volMR) >> 14;
    if (result > 0xFF) result = 0xFF;
    ch->rightVolume = (uint8_t)result;

    uint32_t panL = (uint32_t)(0x7F - rhythmPan);
    uint32_t volL = panL * velocity;
    result = (volL * track->volML) >> 14;
    if (result > 0xFF) result = 0xFF;
    ch->leftVolume = (uint8_t)result;
}

static void cgb_chn_vol_set(M4ACGBChannel *ch, M4ATrack *track)
{
    uint32_t velocity = ch->velocity;
    int32_t rhythmPan = ch->rhythmPan;
    uint32_t panR = (uint32_t)(0x80 + rhythmPan);
    uint32_t volR = panR * velocity;
    uint32_t result = (volR * track->volMR) >> 14;
    if (result > 0xFF) result = 0xFF;
    ch->rightVolume = (uint8_t)result;

    uint32_t panL = (uint32_t)(0x7F - rhythmPan);
    uint32_t volL = panL * velocity;
    result = (volL * track->volML) >> 14;
    if (result > 0xFF) result = 0xFF;
    ch->leftVolume = (uint8_t)result;
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

/* Initialize engine */
void m4a_engine_init(M4AEngine *engine, float sampleRate)
{
    memset(engine, 0, sizeof(M4AEngine));

    engine->sampleRate = sampleRate;
    engine->samplesPerTick = sampleRate / VBLANK_RATE;
    engine->tickAccumulator = 0.0f;
    /* Default to the GBA's hardware DirectSound mix rate so high notes alias
     * the way they do in-game.  Set to 0 (follow host rate) for clean mixing. */
    engine->pcmMixRate = 13379.0f;
    engine->pcmResampleAccum = 0.0f;
    engine->pcmPrevL = engine->pcmPrevR = 0;
    engine->pcmCurL = engine->pcmCurR = 0;
    engine->masterVolume = 15;
    engine->songMasterVolume = MAX_SONG_VOLUME;
    engine->maxPcmChannels = 5;  /* default, matches Pokemon Emerald init */
    engine->polyEventClock = M4A_POLY_TICK_NONE;
    engine->c15 = 14;
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

    /* Initialize reverb at the PCM mix rate (the GBA reverb is a DirectSound
     * buffer effect that runs at the mixing rate, one VBlank frame of delay). */
    m4a_reverb_init(&engine->reverb, m4a_pcm_mix_rate(engine), 0);
}

void m4a_engine_destroy(M4AEngine *engine)
{
    m4a_reverb_destroy(&engine->reverb);
}

void m4a_engine_set_pcm_mix_rate(M4AEngine *engine, float rate)
{
    /* 0 means "follow host rate"; otherwise clamp to a sane audio range. */
    if (rate != 0.0f) {
        if (rate < 1000.0f)   rate = 1000.0f;
        if (rate > 192000.0f) rate = 192000.0f;
    }
    engine->pcmMixRate = rate;

    /* The reverb delay line is sized for the mixing rate; rebuild it, keeping
     * the current amount.  Reset the resampler so it restarts cleanly. */
    uint8_t amount = engine->reverb.amount;
    m4a_reverb_destroy(&engine->reverb);
    m4a_reverb_init(&engine->reverb, m4a_pcm_mix_rate(engine), amount);

    engine->pcmResampleAccum = 0.0f;
    engine->pcmPrevL = engine->pcmPrevR = 0;
    engine->pcmCurL = engine->pcmCurR = 0;
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
    if (trackIndex < 0 || trackIndex >= MAX_TRACKS || !engine->voiceGroup)
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
    engine->cgbChannels[MAX_CGB_CHANNELS + cgbIdx] = *victim;
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
        int32_t pcmSamplesPerVBlank = 224;
        int32_t pcmFreq = (597275 * pcmSamplesPerVBlank + 5000) / 10000;
        int32_t divFreq = (16777216 / pcmFreq + 1) >> 1;
        float scale = (float)pcmFreq / m4a_pcm_mix_rate(engine);
        ch->frequency = (uint32_t)((uint64_t)freq * divFreq * scale);
    }
    for (int i = 0; i < TOTAL_CGB_CHANNELS; i++) {
        M4ACGBChannel *ch = &engine->cgbChannels[i];
        if (!(ch->status & CHN_ON) || (ch->status & CHN_STOP)
            || ch->trackIndex != trackIndex)
            continue;
        uint32_t newFreq = m4a_midi_key_to_cgb_freq(ch->type, (uint8_t)key, fine);
        /* Preserve NR43 bit 3 (7-bit LFSR mode) for the noise channel */
        if (ch->type == 4)
            newFreq |= ch->frequency & 0x08;
        ch->frequency = newFreq;
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
void m4a_engine_note_on(M4AEngine *engine, int trackIndex, uint8_t key, uint8_t velocity)
{
    if (trackIndex < 0 || trackIndex >= MAX_TRACKS)
        return;

    M4ATrack *track = &engine->tracks[trackIndex];
    ToneData *voice = resolve_voice(&track->currentVoice, key);
    if (!voice) return;

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
    int32_t finalKey = (int32_t)useKey + track->keyM;
    if (finalKey < 0) finalKey = 0;
    if (finalKey > 127) finalKey = 127;
    int32_t pcmKey = (int32_t)useKey + pcmBaseAdjust;
    if (pcmKey < 0) pcmKey = 0;
    if (pcmKey > 255) pcmKey = 255;
    int32_t pcmFinalKey = pcmKey + track->keyM;
    int32_t pcmFinalKeyMax = engine->respectBaseMidiKey ? 178 : 127;
    if (pcmFinalKey < 0) pcmFinalKey = 0;
    if (pcmFinalKey > pcmFinalKeyMax) pcmFinalKey = pcmFinalKeyMax;

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
                return;
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
        ch->rhythmPan = rhythmPan;
        ch->attack = voice->attack;
        ch->decay = voice->decay;
        ch->sustain = voice->sustain;
        ch->release = voice->release;
        ch->pseudoEchoVolume = track->pseudoEchoVolume;
        ch->pseudoEchoLength = track->pseudoEchoLength;
        ch->length = voice->length;
        ch->gateTime = 0;

        cgb_chn_vol_set(ch, track);
        m4a_cgb_mod_vol(ch);

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

        /* Calculate frequency */
        ch->frequency = m4a_midi_key_to_cgb_freq(voiceType, (uint8_t)finalKey, track->pitM);
        /* Noise channel: apply period bit (NR43 bit 3) from wavePointer.
         * period=0 → 15-bit LFSR, period=1 → 7-bit short-period LFSR. */
        if (voiceType == 4)
            ch->frequency |= ((uintptr_t)voice->wavePointer & 0x01) << 3;

        if (portamentoInherit)
            ch->status = CHN_ENV_SUSTAIN;
        else
            m4a_cgb_channel_start(ch);

        /* A dropped note never sounded on the GBA, so it must not become the
         * start key of a later portamento glide. */
        if (!shadow)
            portamento_note_started(engine, track, trackIndex, ch->key);
    } else {
        /* PCM DirectSound channel */
        if (!voice->wav) return;

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
                return;
            ch = allocate_pcm_channel(engine, combinedPriority, trackIndex,
                                      MAX_PCM_CHANNELS, MAX_PCM_CHANNELS);
            if (!ch) return;
            shadow = true;
        }

        ch->midiKey = key;
        ch->key = (uint8_t)pcmKey;
        ch->velocity = velocity;
        ch->priority = combinedPriority;
        ch->trackIndex = trackIndex;
        ch->rhythmPan = rhythmPan;
        ch->attack = voice->attack;
        ch->decay = voice->decay;
        ch->sustain = voice->sustain;
        ch->release = voice->release;
        ch->pseudoEchoVolume = track->pseudoEchoVolume;
        ch->pseudoEchoLength = track->pseudoEchoLength;
        ch->gateTime = 0;

        chn_vol_set(ch, track);

        /* Calculate frequency.
         * GBA freq index 4 = 13379 Hz, pcmSamplesPerVBlank = 224.
         * divFreq converts from MidiKeyToFreq units to source-samples-per-GBA-tick.
         * scale converts from GBA tick rate to DAW sample rate. */
        {
            int32_t pcmSamplesPerVBlank = 224;
            int32_t pcmFreq = (597275 * pcmSamplesPerVBlank + 5000) / 10000;
            float scale = (float)pcmFreq / m4a_pcm_mix_rate(engine);

            if (voice->type & VOICE_TYPE_FIX) {
                /* Fixed-frequency (no resample): ignore MIDI key, play at GBA PCM rate.
                 * On the GBA, SoundMainRAM uses fw advance = 0x800000 per PCM tick
                 * (i.e., exactly one source sample per GBA output sample).
                 * Scale that to the DAW sample rate. */
                ch->frequency = (uint32_t)(0x800000 * scale);
            } else {
                int32_t divFreq = (16777216 / pcmFreq + 1) >> 1;
                ch->frequency = m4a_midi_key_to_freq(voice->wav, (uint8_t)pcmFinalKey, track->pitM);
                ch->frequency = (uint32_t)((uint64_t)ch->frequency * divFreq * scale);
            }
        }

        m4a_pcm_channel_start(ch, voice->wav, voice->type);

        /* Portamento legato (opt-in): if the previous note on this track is
         * still in any envelope phase (zero gap between notes), the new note
         * inherits its sample position, envelope, and loop state instead of
         * triggering a fresh attack/sample start, making the glide perfectly
         * smooth.  The previous channel is silenced so the two don't
         * double-voice.  Disallowed if the voice changed (different wav).
         * A shadow (dropped) note must not inherit -- or silence -- a real
         * channel's state. */
        if (!shadow && engine->portamentoEnabled && track->portamentoDuration != 0) {
            for (int i = 0; i < engine->maxPcmChannels; i++) {
                M4APCMChannel *prev = &engine->pcmChannels[i];
                if (prev == ch || !(prev->status & CHN_ON)
                    || prev->trackIndex != trackIndex || prev->wav != voice->wav)
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
    }
}

/*
 * Note Off - transition matching channels to release
 */
void m4a_engine_note_off(M4AEngine *engine, int trackIndex, uint8_t key)
{
    if (trackIndex < 0 || trackIndex >= MAX_TRACKS)
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
        if ((ch->status & CHN_ON) && ch->trackIndex == trackIndex && ch->wav) {
            int32_t finalKey = (int32_t)ch->key + track->keyM;
            if (finalKey < 0) finalKey = 0;
            uint32_t freq = m4a_midi_key_to_freq(ch->wav, (uint8_t)finalKey, track->pitM);
            int32_t pcmSamplesPerVBlank = 224;
            int32_t pcmFreq = (597275 * pcmSamplesPerVBlank + 5000) / 10000;
            int32_t divFreq = (16777216 / pcmFreq + 1) >> 1;
            float scale = (float)pcmFreq / m4a_pcm_mix_rate(engine);
            ch->frequency = (uint32_t)((uint64_t)freq * divFreq * scale);
        }
    }
    for (int i = 0; i < TOTAL_CGB_CHANNELS; i++) {
        M4ACGBChannel *ch = &engine->cgbChannels[i];
        if ((ch->status & CHN_ON) && ch->trackIndex == trackIndex) {
            int32_t finalKey = (int32_t)ch->key + track->keyM;
            if (finalKey < 0) finalKey = 0;
            uint32_t newFreq = m4a_midi_key_to_cgb_freq(ch->type, (uint8_t)finalKey, track->pitM);
            /* Preserve NR43 bit 3 (7-bit LFSR mode) for noise channel.
             * gNoiseTable entries always have bit 3 = 0; the period bit is
             * ORed in at note-on time and must survive frequency updates. */
            if (ch->type == 4)
                newFreq |= ch->frequency & 0x08;
            ch->frequency = newFreq;
        }
    }
}

/* Recalculate track vol/pan and push updated rightVolume/leftVolume into
* all active channels on the track. Matches MPlayMain's behavior of calling
* ChnVolSetAsm on every active channel when MPT_FLG_VOLCHG is set. */
static inline void refresh_volumes(M4AEngine *engine, M4ATrack *track, int trackIndex)
{
    m4a_track_vol_pit_set(track);
    for (int i = 0; i < TOTAL_PCM_CHANNELS; i++) {
        M4APCMChannel *ch = &engine->pcmChannels[i];
        if ((ch->status & CHN_ON) && ch->trackIndex == trackIndex)
            chn_vol_set(ch, track);
    }
    for (int i = 0; i < TOTAL_CGB_CHANNELS; i++) {
        M4ACGBChannel *ch = &engine->cgbChannels[i];
        if ((ch->status & CHN_ON) && ch->trackIndex == trackIndex) {
            cgb_chn_vol_set(ch, track);
            m4a_cgb_mod_vol(ch);
            /* MPlayMain flags MO_VOL on CGB channels when the track volume
             * changes; the next CgbSound applies it with an NRx4 trigger
             * write, retriggering the square-1 sweep unit. */
            ch->modify |= 0x01;
        }
    }
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
        track->lfoSpeed = value;
        track->lfoSpeedC = 0;
        track->modM = 0;
        if (value == 0)
            clear_mod_m(engine, track, trackIndex);
        break;
    case 0x16: /* Modulation type (MODT) */
        // TODO: none of the pokemon emerald songs use MODT
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
                if (ch != NULL)
                    ch->dutyCycle = (uint8_t)(uintptr_t)track->currentVoice.wavePointer & 0x03;
            }
        }
        break;
    case 0x18: /* Micro tuning (TUNE) */
        // TODO: none of the pokemon emerald songs use TUNE
        break;
    case 0x1A: /* LFO delay (LFODL) */
        // TODO: none of the pokemon emerald songs use LFODL
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
        engine->cgbChannels[i].status = 0;
    m4a_engine_reset_portamento(engine);
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
                if (ch != NULL)
                    ch->dutyCycle = (uint8_t)(uintptr_t)track->currentVoice.wavePointer & 0x03;
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
        for (int i = MAX_CGB_CHANNELS; i < TOTAL_CGB_CHANNELS; i++) {
            engine->cgbChannels[i].status = 0;
            engine->cgbChannels[i].declickSamplesRemaining = 0;
        }
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
                    /* Recalculate frequency for pitch mod */
                    if (track->modT == 0 && ch->wav) {
                        int32_t finalKey = (int32_t)ch->key + track->keyM;
                        if (finalKey < 0) finalKey = 0;
                        uint32_t freq = m4a_midi_key_to_freq(ch->wav, (uint8_t)finalKey, track->pitM);
                        int32_t pcmSamplesPerVBlank = 224;
                        int32_t pcmFreq = (597275 * pcmSamplesPerVBlank + 5000) / 10000;
                        int32_t divFreq = (16777216 / pcmFreq + 1) >> 1;
                        float scale = (float)pcmFreq / m4a_pcm_mix_rate(engine);
                        ch->frequency = (uint32_t)((uint64_t)freq * divFreq * scale);
                    }
                }
            }
            for (int j = 0; j < TOTAL_CGB_CHANNELS; j++) {
                M4ACGBChannel *ch = &engine->cgbChannels[j];
                if ((ch->status & CHN_ON) && ch->trackIndex == i) {
                    cgb_chn_vol_set(ch, track);
                    m4a_cgb_mod_vol(ch);
                    /* Tremolo/autopan LFO steps are volume changes (VOLCHG ->
                     * MO_VOL on the GBA), so they retrigger the square-1
                     * sweep; vibrato steps are MO_PIT and do not. */
                    if (track->modT != 0)
                        ch->modify |= 0x01;
                    if (track->modT == 0) {
                        int32_t finalKey = (int32_t)ch->key + track->keyM;
                        if (finalKey < 0) finalKey = 0;
                        uint32_t newFreq = m4a_midi_key_to_cgb_freq(ch->type, (uint8_t)finalKey, track->pitM);
                        if (ch->type == 4)
                            newFreq |= ch->frequency & 0x08;
                        ch->frequency = newFreq;
                    }
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
            /* Decrement gate time */
            if (ch->gateTime > 0) {
                ch->gateTime--;
                if (ch->gateTime == 0)
                    ch->status |= CHN_STOP;
            }
            m4a_pcm_channel_tick(ch, engine->masterVolume);
        }
    }

    /* Process CGB channel envelopes (VBlank rate) */
    for (int i = 0; i < TOTAL_CGB_CHANNELS; i++) {
        M4ACGBChannel *ch = &engine->cgbChannels[i];
        if (ch->status & CHN_ON) {
            if (ch->gateTime > 0) {
                ch->gateTime--;
                if (ch->gateTime == 0)
                    ch->status |= CHN_STOP;
            }
            m4a_cgb_channel_tick(ch, engine->c15);
        }
    }

    /* Tempo accumulator drives LFO ticks, matching MPlayMain's tempo loop.
     * tempoC += tempoI each VBlank; fires one LFO tick per 150 accumulated. */
    engine->tempoC += engine->tempoI;
    while (engine->tempoC >= 150) {
        engine->tempoC -= 150;
        m4a_lfo_tick(engine);
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
 * Generates numSamples of stereo float output.
 */
void m4a_engine_process(M4AEngine *engine, float *outL, float *outR, int numSamples)
{
    /* Number of PCM-rate samples that elapse per host output sample.  When the
     * mix rate is below the host rate (the GBA-accurate case, e.g. 13379 vs
     * 44100), pcmStep < 1: each PCM sample spans several host samples and we
     * linearly interpolate between the two most recent PCM samples.  This
     * reproduces the hardware mixer's low-rate resampling -- including the
     * aliasing that high notes produce in-game.  A mix rate equal to the host
     * rate (pcmMixRate == 0) collapses to one PCM sample per host sample. */
    float pcmStep = m4a_pcm_mix_rate(engine) / engine->sampleRate;

    /* Polyphony-overflow debug: when inverted, the real channels still render
     * (into a discarded accumulator, so sample positions, loop ends, and
     * oscillator phases advance exactly as in normal playback) but only the
     * shadow channels -- the sounds lost to the polyphony limit -- are heard. */
    bool invert = engine->polyDebugInvert;

    for (int i = 0; i < numSamples; i++) {
        /* Check for engine tick (~60Hz) */
        engine->tickAccumulator += 1.0f;
        if (engine->tickAccumulator >= engine->samplesPerTick) {
            engine->tickAccumulator -= engine->samplesPerTick;
            m4a_engine_tick(engine);
        }

        /* Advance the PCM resample clock, generating a new PCM-rate sample
         * (all DirectSound channels mixed + reverb) each time it crosses a
         * whole sample boundary. */
        engine->pcmResampleAccum += pcmStep;
        while (engine->pcmResampleAccum >= 1.0f) {
            engine->pcmResampleAccum -= 1.0f;
            engine->pcmPrevL = engine->pcmCurL;
            engine->pcmPrevR = engine->pcmCurR;

            int32_t pcmL = 0, pcmR = 0;
            int32_t mutedL = 0, mutedR = 0;  /* discarded output of muted channels */
            for (int ch = 0; ch < TOTAL_PCM_CHANNELS; ch++) {
                if (!(engine->pcmChannels[ch].status & CHN_ON))
                    continue;
                bool audible = ((ch >= MAX_PCM_CHANNELS) == invert);
                m4a_pcm_channel_render(&engine->pcmChannels[ch],
                                       audible ? &pcmL : &mutedL,
                                       audible ? &pcmR : &mutedR);
            }
            /* Reverb is a GBA DirectSound-buffer effect: it runs at the PCM mix
             * rate, before the upsample and before CGB is added. */
            m4a_reverb_process(&engine->reverb, &pcmL, &pcmR);

            engine->pcmCurL = pcmL;
            engine->pcmCurR = pcmR;
        }

        /* Linear interpolation of the PCM mix at this host-sample instant. */
        float frac = engine->pcmResampleAccum;
        int32_t mixL = engine->pcmPrevL
                     + (int32_t)((float)(engine->pcmCurL - engine->pcmPrevL) * frac);
        int32_t mixR = engine->pcmPrevR
                     + (int32_t)((float)(engine->pcmCurR - engine->pcmPrevR) * frac);

        /* CGB channels are oscillators synthesized directly at the host rate,
         * so they are mixed in after the PCM upsample.  They are not reverbed,
         * matching the GBA where reverb only touches the DirectSound buffer. */
        {
            int32_t mutedL = 0, mutedR = 0;
            for (int ch = 0; ch < TOTAL_CGB_CHANNELS; ch++) {
                bool audible = ((ch >= MAX_CGB_CHANNELS) == invert);
                m4a_cgb_channel_render(&engine->cgbChannels[ch],
                                       audible ? &mixL : &mutedL,
                                       audible ? &mixR : &mutedR,
                                       engine->sampleRate);
            }
        }


        /* Normalize to float (-1.0 to 1.0)
         * The GBA mixer accumulates (int8_sample * uint8_envVol) >> 8 per channel,
         * giving ~±127 per channel. With maxPcmChannels typically 5-6, the sum
         * can reach ~±700. We use a divider that gives good headroom while
         * keeping CGB channels (which are quieter) audible. */
        outL[i] = (float)mixL / 256.0f;
        outR[i] = (float)mixR / 256.0f;

        /* GBA analog output emulation: single-pole IIR low-pass filter (6 dB/octave).
         * The GBA's PWM output circuit has a characteristic frequency rolloff due to
         * the output capacitor. Adapted from mGBA _audioLowPassFilter (libretro.c).
         * Coefficient 0.6/0.4 matches mGBA's default audioLowPassRange (60%). */
        if (engine->analogFilter) {
            engine->lowPassLeft  = engine->lowPassLeft  * 0.6f + outL[i] * 0.4f;
            engine->lowPassRight = engine->lowPassRight * 0.6f + outR[i] * 0.4f;
            outL[i] = engine->lowPassLeft;
            outR[i] = engine->lowPassRight;
        }
    }
}
