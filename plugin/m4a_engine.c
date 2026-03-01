#include "m4a_engine.h"
#include "m4a_channel.h"
#include "m4a_reverb.h"
#include "m4a_tables.h"
#include <string.h>
#include <stdlib.h>

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
    engine->masterVolume = 15;
    engine->songMasterVolume = MAX_SONG_VOLUME;
    engine->maxPcmChannels = 5;  /* default, matches Pokemon Emerald init */
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

    /* Initialize CGB channels with proper types and pan masks */
    engine->cgbChannels[0].type = 1;
    engine->cgbChannels[0].panMask = 0x11;
    engine->cgbChannels[1].type = 2;
    engine->cgbChannels[1].panMask = 0x22;
    engine->cgbChannels[2].type = 3;
    engine->cgbChannels[2].panMask = 0x44;
    engine->cgbChannels[3].type = 4;
    engine->cgbChannels[3].panMask = 0x88;

    /* Initialize reverb */
    m4a_reverb_init(&engine->reverb, sampleRate, 0);
}

void m4a_engine_destroy(M4AEngine *engine)
{
    m4a_reverb_destroy(&engine->reverb);
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
 * Allocate a PCM channel for a new note.
 * Matches the channel allocation logic in ply_note (m4a_1.s).
 */
static M4APCMChannel *allocate_pcm_channel(M4AEngine *engine, uint8_t priority,
                                            int trackIndex)
{
    M4APCMChannel *best = NULL;
    uint8_t bestPriority = priority;
    int bestTrackIndex = trackIndex;
    int bestIsStopping = 0;

    for (int i = 0; i < engine->maxPcmChannels; i++) {
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

    /* For rhythm (keysplit_all) voices: the MIDI note selects which drum voice
     * to play, but the playback pitch is fixed to the drum voice's own key --
     * not the note the player pressed.  Apply per-note pan while we're here. */
    if (track->currentVoice.type & VOICE_KEYSPLIT_ALL) {
        useKey = voice->key;
        if (voice->panSweep & 0x80) {
            rhythmPan = (int8_t)((voice->panSweep - 0xC0) * 2);
        }
    }

    /* Calculate combined priority */
    uint8_t combinedPriority = track->priority;

    /* Calculate track volumes */
    m4a_track_vol_pit_set(track);

    /* Calculate final key with transposition */
    int32_t finalKey = (int32_t)useKey + track->keyM;
    if (finalKey < 0) finalKey = 0;
    if (finalKey > 127) finalKey = 127;

    if (voiceType >= 1 && voiceType <= 4) {
        /* CGB channel */
        int cgbIdx = voiceType - 1;
        M4ACGBChannel *ch = &engine->cgbChannels[cgbIdx];

        /* Check if we can steal this channel */
        if ((ch->status & CHN_ON) && !(ch->status & CHN_STOP)) {
            if (ch->priority > combinedPriority)
                return;  /* can't steal */
            if (ch->priority == combinedPriority && ch->trackIndex < trackIndex)
                return;
        }

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
            if (voiceType == 1)
                ch->sweep = (voice->panSweep & 0x70) ? voice->panSweep : 0x08;
        } else if (voiceType == 3) {
            ch->wavePointer = voice->wavePointer;
        }

        /* Calculate frequency */
        ch->frequency = m4a_midi_key_to_cgb_freq(voiceType, (uint8_t)finalKey, track->pitM);
        /* Noise channel: apply period bit (NR43 bit 3) from wavePointer.
         * period=0 → 15-bit LFSR, period=1 → 7-bit short-period LFSR. */
        if (voiceType == 4)
            ch->frequency |= ((uintptr_t)voice->wavePointer & 0x01) << 3;

        m4a_cgb_channel_start(ch);
    } else {
        /* PCM DirectSound channel */
        if (!voice->wav) return;

        M4APCMChannel *ch = allocate_pcm_channel(engine, combinedPriority, trackIndex);
        if (!ch) return;

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
        ch->gateTime = 0;

        chn_vol_set(ch, track);

        /* Calculate frequency.
         * GBA freq index 4 = 13379 Hz, pcmSamplesPerVBlank = 224.
         * divFreq converts from MidiKeyToFreq units to source-samples-per-GBA-tick.
         * scale converts from GBA tick rate to DAW sample rate. */
        {
            int32_t pcmSamplesPerVBlank = 224;
            int32_t pcmFreq = (597275 * pcmSamplesPerVBlank + 5000) / 10000;
            float scale = (float)pcmFreq / engine->sampleRate;

            if (voice->type & VOICE_TYPE_FIX) {
                /* Fixed-frequency (no resample): ignore MIDI key, play at GBA PCM rate.
                 * On the GBA, SoundMainRAM uses fw advance = 0x800000 per PCM tick
                 * (i.e., exactly one source sample per GBA output sample).
                 * Scale that to the DAW sample rate. */
                ch->frequency = (uint32_t)(0x800000 * scale);
            } else {
                int32_t divFreq = (16777216 / pcmFreq + 1) >> 1;
                ch->frequency = m4a_midi_key_to_freq(voice->wav, (uint8_t)finalKey, track->pitM);
                ch->frequency = (uint32_t)((uint64_t)ch->frequency * divFreq * scale);
            }
        }

        m4a_pcm_channel_start(ch, voice->wav, voice->type);

        /* Compute initial envelope volumes so the channel produces sound
         * before the first engine tick (~60Hz). On the GBA, SoundMainRAM
         * handles this every frame, but our render loop runs at DAW rate. */
        {
            uint32_t vol = ((uint32_t)(engine->masterVolume + 1) * ch->envelopeVolume) >> 4;
            ch->envelopeVolumeRight = ((uint32_t)ch->rightVolume * vol) >> 8;
            ch->envelopeVolumeLeft = ((uint32_t)ch->leftVolume * vol) >> 8;
        }
    }
}

/*
 * Note Off - transition matching channels to release
 */
void m4a_engine_note_off(M4AEngine *engine, int trackIndex, uint8_t key)
{
    if (trackIndex < 0 || trackIndex >= MAX_TRACKS)
        return;

    /* Stop matching PCM channels */
    for (int i = 0; i < MAX_PCM_CHANNELS; i++) {
        M4APCMChannel *ch = &engine->pcmChannels[i];
        if ((ch->status & CHN_ON) && !(ch->status & CHN_STOP)
            && ch->trackIndex == trackIndex && ch->midiKey == key) {
            ch->status |= CHN_STOP;
        }
    }

    /* Stop matching CGB channels */
    for (int i = 0; i < MAX_CGB_CHANNELS; i++) {
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
    for (int i = 0; i < MAX_PCM_CHANNELS; i++) {
        M4APCMChannel *ch = &engine->pcmChannels[i];
        if ((ch->status & CHN_ON) && ch->trackIndex == trackIndex && ch->wav) {
            int32_t finalKey = (int32_t)ch->key + track->keyM;
            if (finalKey < 0) finalKey = 0;
            uint32_t freq = m4a_midi_key_to_freq(ch->wav, (uint8_t)finalKey, track->pitM);
            int32_t pcmSamplesPerVBlank = 224;
            int32_t pcmFreq = (597275 * pcmSamplesPerVBlank + 5000) / 10000;
            int32_t divFreq = (16777216 / pcmFreq + 1) >> 1;
            float scale = (float)pcmFreq / engine->sampleRate;
            ch->frequency = (uint32_t)((uint64_t)freq * divFreq * scale);
        }
    }
    for (int i = 0; i < MAX_CGB_CHANNELS; i++) {
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
    for (int i = 0; i < MAX_PCM_CHANNELS; i++) {
        M4APCMChannel *ch = &engine->pcmChannels[i];
        if ((ch->status & CHN_ON) && ch->trackIndex == trackIndex)
            chn_vol_set(ch, track);
    }
    for (int i = 0; i < MAX_CGB_CHANNELS; i++) {
        M4ACGBChannel *ch = &engine->cgbChannels[i];
        if ((ch->status & CHN_ON) && ch->trackIndex == trackIndex) {
            cgb_chn_vol_set(ch, track);
            m4a_cgb_mod_vol(ch);
        }
    }
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
        if (value == 0) {
            track->lfoSpeedC = 0;
            track->modM = 0;
        }
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
        break;
    case 0x16: /* Modulation type (MODT) */
        // TODO: none of the pokemon emerald songs use MODT
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
 * All Notes Off for a channel
 */
void m4a_engine_all_notes_off(M4AEngine *engine, int trackIndex)
{
    for (int i = 0; i < MAX_PCM_CHANNELS; i++) {
        M4APCMChannel *ch = &engine->pcmChannels[i];
        if ((ch->status & CHN_ON) && ch->trackIndex == trackIndex)
            ch->status |= CHN_STOP;
    }
    for (int i = 0; i < MAX_CGB_CHANNELS; i++) {
        M4ACGBChannel *ch = &engine->cgbChannels[i];
        if ((ch->status & CHN_ON) && ch->trackIndex == trackIndex)
            ch->status |= CHN_STOP;
    }
}

/*
 * All Sound Off - immediately silence everything
 */
void m4a_engine_all_sound_off(M4AEngine *engine)
{
    for (int i = 0; i < MAX_PCM_CHANNELS; i++)
        engine->pcmChannels[i].status = 0;
    for (int i = 0; i < MAX_CGB_CHANNELS; i++)
        engine->cgbChannels[i].status = 0;
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
            for (int j = 0; j < MAX_PCM_CHANNELS; j++) {
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
                        float scale = (float)pcmFreq / engine->sampleRate;
                        ch->frequency = (uint32_t)((uint64_t)freq * divFreq * scale);
                    }
                }
            }
            for (int j = 0; j < MAX_CGB_CHANNELS; j++) {
                M4ACGBChannel *ch = &engine->cgbChannels[j];
                if ((ch->status & CHN_ON) && ch->trackIndex == i) {
                    cgb_chn_vol_set(ch, track);
                    m4a_cgb_mod_vol(ch);
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

    /* Process PCM channel envelopes (VBlank rate) */
    for (int i = 0; i < MAX_PCM_CHANNELS; i++) {
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
    for (int i = 0; i < MAX_CGB_CHANNELS; i++) {
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
}

/*
 * Main audio processing function.
 * Generates numSamples of stereo float output.
 */
void m4a_engine_process(M4AEngine *engine, float *outL, float *outR, int numSamples)
{
    for (int i = 0; i < numSamples; i++) {
        /* Check for engine tick (~60Hz) */
        engine->tickAccumulator += 1.0f;
        if (engine->tickAccumulator >= engine->samplesPerTick) {
            engine->tickAccumulator -= engine->samplesPerTick;
            m4a_engine_tick(engine);
        }

        /* Mix all active channels */
        int32_t mixL = 0, mixR = 0;

        for (int ch = 0; ch < MAX_PCM_CHANNELS; ch++) {
            if (engine->pcmChannels[ch].status & CHN_ON)
                m4a_pcm_channel_render(&engine->pcmChannels[ch], &mixL, &mixR);
        }

        /* Apply reverb */
        m4a_reverb_process(&engine->reverb, &mixL, &mixR);

        for (int ch = 0; ch < MAX_CGB_CHANNELS; ch++) {
            m4a_cgb_channel_render(&engine->cgbChannels[ch], &mixL, &mixR,
                                   engine->sampleRate);
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
