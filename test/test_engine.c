#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <math.h>
#include "m4a_engine.h"
#include "m4a_channel.h"
#include "m4a_tables.h"

/*
 * Unit tests for the m4a engine.
 * Tests key algorithms against known values from the GBA engine.
 */

static int tests_run = 0;
static int tests_passed = 0;

#define ASSERT(cond, msg) do { \
    tests_run++; \
    if (!(cond)) { \
        fprintf(stderr, "FAIL: %s (line %d)\n", msg, __LINE__); \
    } else { \
        tests_passed++; \
    } \
} while(0)

#define ASSERT_EQ(a, b, msg) do { \
    tests_run++; \
    if ((a) != (b)) { \
        fprintf(stderr, "FAIL: %s: expected %d, got %d (line %d)\n", \
                msg, (int)(b), (int)(a), __LINE__); \
    } else { \
        tests_passed++; \
    } \
} while(0)

#define ASSERT_NEAR(a, b, eps, msg) do { \
    tests_run++; \
    if (fabs((double)(a) - (double)(b)) > (eps)) { \
        fprintf(stderr, "FAIL: %s: expected %f, got %f (line %d)\n", \
                msg, (double)(b), (double)(a), __LINE__); \
    } else { \
        tests_passed++; \
    } \
} while(0)

/* Test umul3232H32 */
static void test_umul3232H32(void)
{
    printf("Testing umul3232H32...\n");

    /* Simple cases */
    ASSERT_EQ(umul3232H32(0, 0), 0, "0 * 0");
    ASSERT_EQ(umul3232H32(0xFFFFFFFF, 1), 0, "FFFFFFFF * 1 high word");
    ASSERT_EQ(umul3232H32(0x80000000, 2), 1, "0x80000000 * 2");
    ASSERT_EQ(umul3232H32(0x80000000, 0x80000000), 0x40000000, "0.5 * 0.5 in fixed point");
}

/* Test scale/freq table lookups */
static void test_scale_table(void)
{
    printf("Testing scale table...\n");

    /* Key 0 should map to 0xE0 */
    ASSERT_EQ(gScaleTable[0], 0xE0, "key 0");
    /* Key 12 should map to 0xD0 (one octave up) */
    ASSERT_EQ(gScaleTable[12], 0xD0, "key 12");
    /* Key 60 (middle C) should be in octave 5 */
    ASSERT_EQ(gScaleTable[60], 0x90, "key 60 octave");
    /* Key 168 (near end) */
    ASSERT_EQ(gScaleTable[168], 0x00, "key 168");
}

/* Test MidiKeyToFreq */
static void test_midi_key_to_freq(void)
{
    printf("Testing MidiKeyToFreq...\n");

    /* Create a fake WaveData with a known frequency */
    WaveData wd;
    memset(&wd, 0, sizeof(wd));
    wd.freq = 0x00800000;  /* 0.5 in 9.23 fixed point, ~4186 Hz */

    /* Middle C (key 60) with no fine adjust */
    uint32_t freq60 = m4a_midi_key_to_freq(&wd, 60, 0);

    /* One octave up (key 72) should be ~2x frequency */
    uint32_t freq72 = m4a_midi_key_to_freq(&wd, 72, 0);

    /* freq72 should be approximately 2 * freq60 */
    double ratio = (double)freq72 / (double)freq60;
    ASSERT_NEAR(ratio, 2.0, 0.01, "octave ratio 72/60");

    /* One semitone up should be ~1.0595x */
    uint32_t freq61 = m4a_midi_key_to_freq(&wd, 61, 0);
    ratio = (double)freq61 / (double)freq60;
    ASSERT_NEAR(ratio, 1.0595, 0.01, "semitone ratio 61/60");

    /* Key clamping at 178 */
    uint32_t freq178 = m4a_midi_key_to_freq(&wd, 178, 0);
    uint32_t freq200 = m4a_midi_key_to_freq(&wd, 200, 0);
    ASSERT_EQ(freq200, m4a_midi_key_to_freq(&wd, 178, 255),
              "key > 178 should clamp");
}

/* Test MidiKeyToCgbFreq */
static void test_midi_key_to_cgb_freq(void)
{
    printf("Testing MidiKeyToCgbFreq...\n");

    /* Noise channel: key 21 is the lowest valid */
    uint32_t noiseFreq = m4a_midi_key_to_cgb_freq(4, 21, 0);
    ASSERT_EQ(noiseFreq, gNoiseTable[0], "noise key 21");

    /* Noise channel: key 80 = table index 59 */
    noiseFreq = m4a_midi_key_to_cgb_freq(4, 80, 0);
    ASSERT_EQ(noiseFreq, gNoiseTable[59], "noise key 80");

    /* Square channel: very low key should clamp */
    uint32_t sqFreq = m4a_midi_key_to_cgb_freq(1, 20, 0);
    uint32_t sqFreqLow = m4a_midi_key_to_cgb_freq(1, 36, 0);
    ASSERT_EQ(sqFreq, sqFreqLow, "square low key clamp");

    /* Square channel: verify 2048 offset */
    sqFreq = m4a_midi_key_to_cgb_freq(1, 72, 0);
    ASSERT(sqFreq > 0, "square freq should be positive");
}

/* Test track volume/pitch calculation */
static void test_trk_vol_pit_set(void)
{
    printf("Testing TrkVolPitSet...\n");

    M4ATrack track;
    memset(&track, 0, sizeof(track));
    track.volume = 127;
    track.volX = 64;
    track.pan = 0;
    track.panX = 0;
    track.bendRange = 2;
    track.modT = 0;
    track.modM = 0;
    track.keyShift = 0;
    track.keyShiftX = 0;
    track.tune = 0;
    track.pitX = 0;
    track.bend = 0;

    m4a_track_vol_pit_set(&track);

    /* With center pan (0), volumes should be roughly equal */
    ASSERT(track.volMR > 0, "right volume should be positive");
    ASSERT(track.volML > 0, "left volume should be positive");

    /* With vol=127, volX=64, and center pan:
     * x = (127 * 64) >> 5 = 254
     * y = 0 (center)
     * volMR = (128 * 254) >> 8 = 127
     * volML = (127 * 254) >> 8 = 126 */
    ASSERT_EQ(track.volMR, 127, "right vol center");
    ASSERT_EQ(track.volML, 126, "left vol center");

    /* Test with full right pan */
    track.pan = 63;
    m4a_track_vol_pit_set(&track);
    ASSERT(track.volMR > track.volML, "right pan: R > L");

    /* Test with full left pan */
    track.pan = -64;
    m4a_track_vol_pit_set(&track);
    ASSERT(track.volML > track.volMR, "left pan: L > R");

    /* Test pitch: bend of +1 with range 2 = +2 semitones */
    track.pan = 0;
    track.bend = 64;
    m4a_track_vol_pit_set(&track);
    ASSERT_EQ(track.keyM, 2, "bend +64 range 2 = keyM 2");
}

/* Test engine initialization */
static void test_engine_init(void)
{
    printf("Testing engine init...\n");

    M4AEngine engine;
    m4a_engine_init(&engine, 44100.0f);

    ASSERT_NEAR(engine.sampleRate, 44100.0f, 0.1f, "sample rate");
    ASSERT_NEAR(engine.samplesPerTick, 44100.0f / 59.7275f, 1.0f, "samples per tick");
    ASSERT_EQ(engine.masterVolume, 15, "master volume");
    ASSERT_EQ(engine.songMasterVolume, MAX_SONG_VOLUME, "song master volume");
    ASSERT_EQ(engine.maxPcmChannels, 5, "max pcm channels");

    /* Verify CGB channel types */
    ASSERT_EQ(engine.cgbChannels[0].type, 1, "cgb ch0 type");
    ASSERT_EQ(engine.cgbChannels[1].type, 2, "cgb ch1 type");
    ASSERT_EQ(engine.cgbChannels[2].type, 3, "cgb ch2 type");
    ASSERT_EQ(engine.cgbChannels[3].type, 4, "cgb ch3 type");

    m4a_engine_destroy(&engine);
}

/* Test basic audio generation */
static void test_basic_audio(void)
{
    printf("Testing basic audio generation...\n");

    M4AEngine engine;
    m4a_engine_init(&engine, 44100.0f);

    /* Create a simple WaveData (sine-ish) */
    int dataSize = 64;
    WaveData *wd = calloc(1, sizeof(WaveData) + dataSize + 1);
    wd->type = 0;
    wd->freq = 0x01000000;  /* ~8372 Hz base */
    wd->loopStart = 0;
    wd->size = dataSize;
    wd->data = (int8_t *)((uint8_t *)wd + sizeof(WaveData));
    for (int i = 0; i < dataSize; i++) {
        wd->data[i] = (int8_t)(127.0 * sin(2.0 * 3.14159265 * i / dataSize));
    }
    wd->data[dataSize] = wd->data[0];  /* safety sample */

    /* Create a voicegroup with one DirectSound voice */
    ToneData voices[128];
    memset(voices, 0, sizeof(voices));
    voices[0].type = VOICE_DIRECTSOUND;
    voices[0].key = 60;
    voices[0].wav = wd;
    voices[0].attack = 0xFF;  /* instant attack */
    voices[0].decay = 0;
    voices[0].sustain = 0xFF;
    voices[0].release = 0;

    m4a_engine_set_voicegroup(&engine, voices);
    m4a_engine_program_change(&engine, 0, 0);
    m4a_engine_cc(&engine, 0, 7, 127);

    /* Play a note */
    m4a_engine_note_on(&engine, 0, 60, 100);

    /* Generate some audio */
    float outL[1024], outR[1024];
    m4a_engine_process(&engine, outL, outR, 1024);

    /* Verify we got non-zero audio */
    float maxVal = 0;
    for (int i = 0; i < 1024; i++) {
        if (fabs(outL[i]) > maxVal) maxVal = fabs(outL[i]);
        if (fabs(outR[i]) > maxVal) maxVal = fabs(outR[i]);
    }
    ASSERT(maxVal > 0.001f, "audio output should be non-zero");

    /* Note off */
    m4a_engine_note_off(&engine, 0, 60);

    m4a_engine_destroy(&engine);
    free(wd);
}

/* Test PCM channel stealing / polyphony behavior */
static void test_polyphony_stealing(void)
{
    printf("Testing polyphony channel stealing...\n");

    /* Minimal WaveData for PCM tests */
    int dataSize = 4;
    WaveData *wd = calloc(1, sizeof(WaveData) + dataSize + 1);
    wd->type = 0;
    wd->freq = 0x01000000;
    wd->loopStart = 0;
    wd->size = dataSize;
    wd->data = (int8_t *)((uint8_t *)wd + sizeof(WaveData));
    wd->data[dataSize] = 0;  /* safety sample */

    ToneData voices[128];
    memset(voices, 0, sizeof(voices));
    voices[0].type = VOICE_DIRECTSOUND;
    voices[0].key = 60;
    voices[0].wav = wd;
    voices[0].attack = 0xFF;
    voices[0].decay = 0;
    voices[0].sustain = 0xFF;
    voices[0].release = 0;

    /* Status value for an active (non-stopping) PCM channel */
    const uint8_t ACTIVE = CHN_START | CHN_ENV_SUSTAIN;
    /* Status value for a stopping PCM channel */
    const uint8_t STOPPING = CHN_STOP | CHN_START;

    M4AEngine engine;

    /* ---- Test 1: Free channel used immediately (baseline) ---- */
    m4a_engine_init(&engine, 44100.0f);
    m4a_engine_set_voicegroup(&engine, voices);
    m4a_engine_program_change(&engine, 0, 0);
    engine.tracks[0].priority = 5;
    m4a_engine_note_on(&engine, 0, 60, 100);
    ASSERT(engine.pcmChannels[0].status & CHN_ON, "free channel: ch0 allocated");
    ASSERT_EQ(engine.pcmChannels[0].trackIndex, 0, "free channel: ch0 trackIndex");
    ASSERT_EQ(engine.pcmChannels[0].priority, 5, "free channel: ch0 priority");
    m4a_engine_destroy(&engine);

    /* ---- Test 2: Higher-priority note steals lower-priority active channel ---- */
    m4a_engine_init(&engine, 44100.0f);
    m4a_engine_set_voicegroup(&engine, voices);
    m4a_engine_program_change(&engine, 1, 0);
    engine.tracks[1].priority = 7;
    for (int i = 0; i < 5; i++) {
        engine.pcmChannels[i].status = ACTIVE;
        engine.pcmChannels[i].priority = 3;
        engine.pcmChannels[i].trackIndex = 0;
    }
    m4a_engine_note_on(&engine, 1, 60, 100);
    {
        int stolen = 0;
        for (int i = 0; i < 5; i++)
            if (engine.pcmChannels[i].trackIndex == 1) { stolen = 1; break; }
        ASSERT(stolen, "higher priority: steals lower-priority channel");
    }
    m4a_engine_destroy(&engine);

    /* ---- Test 3: Lower-priority note cannot steal higher-priority channel ---- */
    m4a_engine_init(&engine, 44100.0f);
    m4a_engine_set_voicegroup(&engine, voices);
    m4a_engine_program_change(&engine, 2, 0);
    engine.tracks[2].priority = 3;
    for (int i = 0; i < 5; i++) {
        engine.pcmChannels[i].status = ACTIVE;
        engine.pcmChannels[i].priority = 7;
        engine.pcmChannels[i].trackIndex = 0;
    }
    m4a_engine_note_on(&engine, 2, 60, 100);
    for (int i = 0; i < 5; i++)
        ASSERT_EQ(engine.pcmChannels[i].trackIndex, 0, "lower priority: no steal");
    m4a_engine_destroy(&engine);

    /* ---- Test 4: Equal-priority note steals channel with higher trackIndex ---- */
    m4a_engine_init(&engine, 44100.0f);
    m4a_engine_set_voicegroup(&engine, voices);
    m4a_engine_program_change(&engine, 3, 0);
    engine.tracks[3].priority = 5;
    for (int i = 0; i < 5; i++) {
        engine.pcmChannels[i].status = ACTIVE;
        engine.pcmChannels[i].priority = 5;
        engine.pcmChannels[i].trackIndex = 7;
    }
    m4a_engine_note_on(&engine, 3, 60, 100);  /* trackIndex=3, priority=5 */
    /* ch.trackIndex=7 >= new.trackIndex=3 → qualifies → steal */
    {
        int stolen = 0;
        for (int i = 0; i < 5; i++)
            if (engine.pcmChannels[i].trackIndex == 3) { stolen = 1; break; }
        ASSERT(stolen, "equal priority: steals channel with higher trackIndex");
    }
    m4a_engine_destroy(&engine);

    /* ---- Test 5: Equal-priority note cannot steal channel with lower trackIndex ---- */
    m4a_engine_init(&engine, 44100.0f);
    m4a_engine_set_voicegroup(&engine, voices);
    m4a_engine_program_change(&engine, 5, 0);
    engine.tracks[5].priority = 5;
    for (int i = 0; i < 5; i++) {
        engine.pcmChannels[i].status = ACTIVE;
        engine.pcmChannels[i].priority = 5;
        engine.pcmChannels[i].trackIndex = 1;
    }
    m4a_engine_note_on(&engine, 5, 60, 100);  /* trackIndex=5, priority=5 */
    /* ch.trackIndex=1 < new.trackIndex=5 → does not qualify → dropped */
    for (int i = 0; i < 5; i++)
        ASSERT_EQ(engine.pcmChannels[i].trackIndex, 1, "equal priority: no steal when ch.trackIndex < new.trackIndex");
    m4a_engine_destroy(&engine);

    /* ---- Test 6: Stopping channel is always stolen regardless of priority ---- */
    m4a_engine_init(&engine, 44100.0f);
    m4a_engine_set_voicegroup(&engine, voices);
    m4a_engine_program_change(&engine, 0, 0);
    engine.tracks[0].priority = 1;  /* very low priority */
    /* 4 active channels at high priority */
    for (int i = 0; i < 4; i++) {
        engine.pcmChannels[i].status = ACTIVE;
        engine.pcmChannels[i].priority = 10;
        engine.pcmChannels[i].trackIndex = 9;
    }
    /* Channel 4 is stopping (releasing) */
    engine.pcmChannels[4].status = STOPPING;
    engine.pcmChannels[4].priority = 10;
    engine.pcmChannels[4].trackIndex = 9;
    m4a_engine_note_on(&engine, 0, 60, 100);  /* priority=1, trackIndex=0 */
    /* Stopping channel (index 4) should be stolen */
    ASSERT_EQ(engine.pcmChannels[4].trackIndex, 0, "stopping channel: always stolen");
    /* Active channels untouched */
    for (int i = 0; i < 4; i++)
        ASSERT_EQ(engine.pcmChannels[i].trackIndex, 9, "stopping channel: active channels untouched");
    m4a_engine_destroy(&engine);

    /* ---- Test 7: Concrete bug case — all 5 slots at equal priority, new note dropped ---- */
    /* All 5 channels at priority 5, owned by tracks 1–5.
     * New note from track 7 at priority 5.
     * ch.trackIndex (1..5) < new.trackIndex (7) for all → no victim → dropped. */
    m4a_engine_init(&engine, 44100.0f);
    m4a_engine_set_voicegroup(&engine, voices);
    m4a_engine_program_change(&engine, 7, 0);
    engine.tracks[7].priority = 5;
    for (int i = 0; i < 5; i++) {
        engine.pcmChannels[i].status = ACTIVE;
        engine.pcmChannels[i].priority = 5;
        engine.pcmChannels[i].trackIndex = i + 1;  /* tracks 1..5 */
    }
    m4a_engine_note_on(&engine, 7, 60, 100);
    for (int i = 0; i < 5; i++)
        ASSERT_EQ(engine.pcmChannels[i].trackIndex, i + 1, "all slots: note dropped when no valid victim");
    m4a_engine_destroy(&engine);

    free(wd);
}

/* Polyphony-overflow debugging: lost sounds are counted per track and logged
 * to the event ring; the invert mode mutes normal playback and plays the lost
 * sounds on the shadow channel pool instead. */
static void test_poly_overflow_debug(void)
{
    printf("Testing polyphony overflow debugging...\n");

    /* Looped WaveData so notes keep sounding until note-off */
    int dataSize = 64;
    WaveData *wd = calloc(1, sizeof(WaveData) + dataSize + 1);
    wd->type = 0;
    wd->status = 0xC000;  /* looped */
    wd->freq = 0x01000000;
    wd->loopStart = 0;
    wd->size = dataSize;
    wd->data = (int8_t *)((uint8_t *)wd + sizeof(WaveData));
    for (int i = 0; i < dataSize; i++)
        wd->data[i] = (int8_t)(127.0 * sin(2.0 * 3.14159265 * i / dataSize));
    wd->data[dataSize] = wd->data[0];

    ToneData voices[128];
    memset(voices, 0, sizeof(voices));
    voices[0].type = VOICE_DIRECTSOUND;
    voices[0].key = 60;
    voices[0].wav = wd;
    voices[0].attack = 0xFF;
    voices[0].decay = 0;
    voices[0].sustain = 0xFF;
    voices[0].release = 0;
    voices[1].type = VOICE_SQUARE_1;
    voices[1].key = 60;
    voices[1].wavePointer = (uint32_t *)(uintptr_t)2;
    voices[1].attack = 0;
    voices[1].decay = 0;
    voices[1].sustain = 15;
    voices[1].release = 3;
    voices[2] = voices[0];  /* distinct program for event-instrument tests */

    M4AEngine engine;
    float outL[1024], outR[1024];

    /* ---- PCM drop is counted and logged; no shadow start when invert off ---- */
    m4a_engine_init(&engine, 44100.0f);
    m4a_engine_set_voicegroup(&engine, voices);
    engine.maxPcmChannels = 1;
    m4a_engine_program_change(&engine, 0, 0);
    m4a_engine_program_change(&engine, 1, 2);
    m4a_engine_note_on(&engine, 0, 60, 100);
    /* Equal priority and the occupant's trackIndex (0) is lower than the new
     * note's (1), so the new note has no victim and is dropped. */
    m4a_engine_note_on(&engine, 1, 67, 100);
    ASSERT_EQ(engine.polyDropCount[1], 1, "poly: PCM drop counted for track");
    ASSERT_EQ(engine.polyEventTotal, 1, "poly: drop logged one event");
    ASSERT_EQ(engine.polyEvents[0].type, M4A_POLY_DROPPED, "poly: event type dropped");
    ASSERT_EQ(engine.polyEvents[0].trackIndex, 1, "poly: event track");
    ASSERT_EQ(engine.polyEvents[0].midiKey, 67, "poly: event key");
    ASSERT_EQ(engine.polyEvents[0].program, 2, "poly: event records losing track's program");
    {
        int shadowActive = 0;
        for (int i = MAX_PCM_CHANNELS; i < TOTAL_PCM_CHANNELS; i++)
            if (engine.pcmChannels[i].status & CHN_ON) shadowActive = 1;
        ASSERT(!shadowActive, "poly: no shadow channel without invert mode");
    }
    m4a_engine_destroy(&engine);

    /* ---- PCM steal is counted and attributed to the victim's track ---- */
    m4a_engine_init(&engine, 44100.0f);
    m4a_engine_set_voicegroup(&engine, voices);
    engine.maxPcmChannels = 1;
    m4a_engine_program_change(&engine, 0, 0);
    m4a_engine_program_change(&engine, 1, 0);
    m4a_engine_note_on(&engine, 1, 60, 100);
    /* Equal priority, occupant trackIndex (1) >= new (0): the channel is stolen. */
    m4a_engine_note_on(&engine, 0, 67, 100);
    ASSERT_EQ(engine.polyStealCount[1], 1, "poly: steal counted for victim track");
    ASSERT_EQ(engine.polyEvents[0].type, M4A_POLY_STOLEN, "poly: event type stolen");
    ASSERT_EQ(engine.polyEvents[0].trackIndex, 1, "poly: steal victim track");
    ASSERT_EQ(engine.polyEvents[0].midiKey, 60, "poly: steal victim key");
    ASSERT_EQ(engine.polyEvents[0].byTrack, 0, "poly: steal attributed to new track");
    m4a_engine_destroy(&engine);

    /* ---- Tail cut: reusing a releasing channel is logged separately ---- */
    m4a_engine_init(&engine, 44100.0f);
    m4a_engine_set_voicegroup(&engine, voices);
    engine.maxPcmChannels = 1;
    voices[0].release = 220;  /* slow release so the tail is still alive */
    m4a_engine_program_change(&engine, 0, 0);
    m4a_engine_program_change(&engine, 1, 0);
    m4a_engine_note_on(&engine, 1, 60, 100);
    m4a_engine_note_off(&engine, 1, 60);
    m4a_engine_note_on(&engine, 0, 67, 100);
    ASSERT_EQ(engine.polyTailCutCount[1], 1, "poly: tail cut counted");
    ASSERT_EQ(engine.polyEvents[0].type, M4A_POLY_TAIL_CUT, "poly: event type tail cut");
    voices[0].release = 0;
    m4a_engine_destroy(&engine);

    /* ---- Invert mode: normal audio is muted, dropped note is audible ---- */
    m4a_engine_init(&engine, 44100.0f);
    m4a_engine_set_voicegroup(&engine, voices);
    engine.maxPcmChannels = 1;
    m4a_engine_set_poly_debug_invert(&engine, true);
    m4a_engine_program_change(&engine, 0, 0);
    m4a_engine_program_change(&engine, 1, 0);
    m4a_engine_cc(&engine, 0, 7, 127);
    m4a_engine_cc(&engine, 1, 7, 127);
    m4a_engine_note_on(&engine, 0, 60, 100);
    /* Only a normally-playing note: inverted output must be silent. */
    m4a_engine_process(&engine, outL, outR, 1024);
    {
        float maxVal = 0;
        for (int i = 0; i < 1024; i++) {
            if (fabs(outL[i]) > maxVal) maxVal = fabs(outL[i]);
            if (fabs(outR[i]) > maxVal) maxVal = fabs(outR[i]);
        }
        ASSERT(maxVal < 1e-9f, "poly invert: normal playback is muted");
    }
    /* ...but the muted channel still advances through its sample. */
    ASSERT(engine.pcmChannels[0].currentPointer != wd->data
           || engine.pcmChannels[0].fw != 0,
           "poly invert: muted channel still advances");
    /* Now drop a note: it should start on a shadow channel and be audible. */
    m4a_engine_note_on(&engine, 1, 67, 100);
    {
        M4APCMChannel *shadow = NULL;
        for (int i = MAX_PCM_CHANNELS; i < TOTAL_PCM_CHANNELS; i++)
            if (engine.pcmChannels[i].status & CHN_ON) { shadow = &engine.pcmChannels[i]; break; }
        ASSERT(shadow != NULL, "poly invert: dropped note plays on shadow channel");
        ASSERT_EQ(shadow->trackIndex, 1, "poly invert: shadow channel track");
        ASSERT_EQ(shadow->midiKey, 67, "poly invert: shadow channel key");
    }
    m4a_engine_process(&engine, outL, outR, 1024);
    {
        float maxVal = 0;
        for (int i = 0; i < 1024; i++) {
            if (fabs(outL[i]) > maxVal) maxVal = fabs(outL[i]);
            if (fabs(outR[i]) > maxVal) maxVal = fabs(outR[i]);
        }
        ASSERT(maxVal > 0.001f, "poly invert: dropped note is audible");
    }
    /* Note-off releases the shadow note like a real one. */
    m4a_engine_note_off(&engine, 1, 67);
    {
        int stopped = 0;
        for (int i = MAX_PCM_CHANNELS; i < TOTAL_PCM_CHANNELS; i++)
            if (engine.pcmChannels[i].status & CHN_STOP) stopped = 1;
        ASSERT(stopped, "poly invert: note-off releases shadow channel");
    }
    /* Turning invert off kills the shadow pool. */
    m4a_engine_set_poly_debug_invert(&engine, false);
    {
        int shadowActive = 0;
        for (int i = MAX_PCM_CHANNELS; i < TOTAL_PCM_CHANNELS; i++)
            if (engine.pcmChannels[i].status & CHN_ON) shadowActive = 1;
        ASSERT(!shadowActive, "poly invert: disabling kills shadow channels");
    }
    m4a_engine_destroy(&engine);

    /* ---- Invert mode: a stolen note's remainder survives on a shadow channel ---- */
    m4a_engine_init(&engine, 44100.0f);
    m4a_engine_set_voicegroup(&engine, voices);
    engine.maxPcmChannels = 1;
    m4a_engine_set_poly_debug_invert(&engine, true);
    m4a_engine_program_change(&engine, 0, 0);
    m4a_engine_program_change(&engine, 1, 0);
    m4a_engine_note_on(&engine, 1, 60, 100);
    m4a_engine_note_on(&engine, 0, 67, 100);  /* steals track 1's channel */
    {
        M4APCMChannel *shadow = NULL;
        for (int i = MAX_PCM_CHANNELS; i < TOTAL_PCM_CHANNELS; i++)
            if (engine.pcmChannels[i].status & CHN_ON) { shadow = &engine.pcmChannels[i]; break; }
        ASSERT(shadow != NULL, "poly invert: stolen note preserved on shadow channel");
        ASSERT_EQ(shadow->trackIndex, 1, "poly invert: preserved victim track");
        ASSERT_EQ(shadow->midiKey, 60, "poly invert: preserved victim key");
    }
    m4a_engine_destroy(&engine);

    /* ---- CGB: drop counted; invert plays it on the shadow CGB channel ---- */
    m4a_engine_init(&engine, 44100.0f);
    m4a_engine_set_voicegroup(&engine, voices);
    m4a_engine_set_poly_debug_invert(&engine, true);
    m4a_engine_program_change(&engine, 0, 1);  /* square 1 */
    m4a_engine_program_change(&engine, 1, 1);
    engine.tracks[0].priority = 5;
    engine.tracks[1].priority = 3;
    m4a_engine_note_on(&engine, 0, 60, 100);
    m4a_engine_note_on(&engine, 1, 67, 100);   /* lower priority: dropped */
    ASSERT_EQ(engine.polyDropCount[1], 1, "poly cgb: drop counted");
    ASSERT(engine.cgbChannels[0].status & CHN_ON, "poly cgb: original note untouched");
    ASSERT_EQ(engine.cgbChannels[0].trackIndex, 0, "poly cgb: original note owner");
    ASSERT(engine.cgbChannels[MAX_CGB_CHANNELS].status & CHN_ON,
           "poly cgb: dropped note plays on shadow square channel");
    ASSERT_EQ(engine.cgbChannels[MAX_CGB_CHANNELS].trackIndex, 1,
              "poly cgb: shadow channel track");
    /* Cross-track CGB steal is recorded and the victim preserved. */
    engine.tracks[2].priority = 7;
    m4a_engine_program_change(&engine, 2, 1);
    m4a_engine_note_on(&engine, 2, 72, 100);   /* steals track 0's square */
    ASSERT_EQ(engine.polyStealCount[0], 1, "poly cgb: cross-track steal counted");
    ASSERT_EQ(engine.cgbChannels[MAX_CGB_CHANNELS].trackIndex, 0,
              "poly cgb: stolen victim replaces older shadow sound");
    ASSERT_EQ(engine.cgbChannels[MAX_CGB_CHANNELS].midiKey, 60,
              "poly cgb: preserved victim key");
    /* Reset clears counters and the event ring. */
    m4a_engine_reset_poly_stats(&engine);
    ASSERT_EQ(engine.polyDropCount[1], 0, "poly: reset clears drop counts");
    ASSERT_EQ(engine.polyStealCount[0], 0, "poly: reset clears steal counts");
    ASSERT_EQ(engine.polyEventTotal, 0, "poly: reset clears event ring");
    m4a_engine_destroy(&engine);

    free(wd);
}

/* Find the active, non-releasing PCM channel for a track, or NULL */
static M4APCMChannel *find_pcm_channel(M4AEngine *engine, int trackIndex)
{
    for (int i = 0; i < MAX_PCM_CHANNELS; i++) {
        M4APCMChannel *ch = &engine->pcmChannels[i];
        if ((ch->status & CHN_ON) && !(ch->status & CHN_STOP)
            && ch->trackIndex == trackIndex)
            return ch;
    }
    return NULL;
}

/* Test portamento (CC 5) glide behavior */
static void test_portamento(void)
{
    printf("Testing portamento...\n");

    /* Looped WaveData so legato notes keep playing indefinitely */
    int dataSize = 64;
    WaveData *wd = calloc(1, sizeof(WaveData) + dataSize + 1);
    wd->type = 0;
    wd->status = 0xC000;  /* looped */
    wd->freq = 0x01000000;
    wd->loopStart = 0;
    wd->size = dataSize;
    wd->data = (int8_t *)((uint8_t *)wd + sizeof(WaveData));
    for (int i = 0; i < dataSize; i++)
        wd->data[i] = (int8_t)(127.0 * sin(2.0 * 3.14159265 * i / dataSize));
    wd->data[dataSize] = wd->data[0];

    ToneData voices[128];
    memset(voices, 0, sizeof(voices));
    voices[0].type = VOICE_DIRECTSOUND;
    voices[0].key = 60;
    voices[0].wav = wd;
    voices[0].attack = 0xFF;
    voices[0].decay = 0;
    voices[0].sustain = 0xFF;
    voices[0].release = 220;  /* slow release so legato gap detection sees the old note */
    voices[1].type = VOICE_SQUARE_1;
    voices[1].key = 60;
    voices[1].wavePointer = (uint32_t *)(uintptr_t)2;  /* duty cycle */
    voices[1].attack = 0;
    voices[1].decay = 0;
    voices[1].sustain = 15;
    voices[1].release = 3;

    M4AEngine engine;
    float outL[1024], outR[1024];

    /* ---- PCM: legato glide ---- */
    m4a_engine_init(&engine, 44100.0f);
    engine.portamentoEnabled = true;
    m4a_engine_set_voicegroup(&engine, voices);
    m4a_engine_program_change(&engine, 0, 0);
    m4a_engine_cc(&engine, 0, 0x5, 2);  /* portamento: 2-tick glide */
    ASSERT_EQ(engine.tracks[0].portamentoDuration, 2, "portamento: duration set by CC 5");

    /* First note: no previous key, so no glide */
    m4a_engine_note_on(&engine, 0, 60, 100);
    ASSERT_EQ(engine.tracks[0].portamentoPrevKey, 60, "portamento: first note captures prev key");
    ASSERT(!engine.tracks[0].portamentoGliding, "portamento: first note does not glide");

    M4APCMChannel *first = find_pcm_channel(&engine, 0);
    ASSERT(first != NULL, "portamento: first note allocated a channel");
    uint32_t freqStart = first->frequency;

    /* Render a bit, then play a legato (zero-gap) note a fifth up */
    m4a_engine_process(&engine, outL, outR, 256);
    int8_t *posBefore = first->currentPointer;
    m4a_engine_note_off(&engine, 0, 60);
    m4a_engine_note_on(&engine, 0, 67, 100);

    M4APCMChannel *second = find_pcm_channel(&engine, 0);
    ASSERT(second != NULL, "portamento: legato note has a channel");
    ASSERT(engine.tracks[0].portamentoGliding, "portamento: legato note starts a glide");
    ASSERT_EQ(engine.tracks[0].portamentoTargetKey, 67, "portamento: glide target key");
    ASSERT_EQ(engine.tracks[0].portamentoPrevKey, 60, "portamento: glide start key");

    /* The new note inherits the old note's state: sustain envelope, sample
     * position, and the old channel is silenced (only one channel active) */
    ASSERT_EQ(second->status & ~CHN_LOOP, CHN_ENV_SUSTAIN, "portamento: inherited note is in sustain");
    ASSERT(second->currentPointer == posBefore, "portamento: inherited sample position");
    {
        int active = 0;
        for (int i = 0; i < MAX_PCM_CHANNELS; i++)
            if (engine.pcmChannels[i].status & CHN_ON) active++;
        ASSERT_EQ(active, 1, "portamento: previous channel silenced (no double-voice)");
    }

    /* Pitch starts at the old key, rises during the glide, and lands on the
     * target.  2 ticks at default tempo = 2 VBlanks ~= 1477 samples. */
    ASSERT_EQ(second->frequency, freqStart, "portamento: glide starts at previous pitch");
    m4a_engine_process(&engine, outL, outR, 745);  /* ~1 VBlank: mid-glide */
    uint32_t freqMid = second->frequency;
    ASSERT(freqMid > freqStart, "portamento: pitch rises during glide");
    m4a_engine_process(&engine, outL, outR, 1024);  /* finish the glide */
    uint32_t freqEnd = second->frequency;
    ASSERT(freqEnd > freqMid, "portamento: pitch keeps rising to target");
    ASSERT(!engine.tracks[0].portamentoGliding, "portamento: glide completes");
    ASSERT_EQ(engine.tracks[0].portamentoPrevKey, 67, "portamento: prev key updated on completion");

    /* Final pitch matches a plain note at the target key */
    {
        M4AEngine ref;
        m4a_engine_init(&ref, 44100.0f);
        m4a_engine_set_voicegroup(&ref, voices);
        m4a_engine_program_change(&ref, 0, 0);
        m4a_engine_note_on(&ref, 0, 67, 100);
        M4APCMChannel *refCh = find_pcm_channel(&ref, 0);
        ASSERT_EQ(freqEnd, refCh->frequency, "portamento: final pitch equals target note pitch");
        m4a_engine_destroy(&ref);
    }
    m4a_engine_destroy(&engine);

    /* ---- PCM: notes with a gap do not inherit channel state ---- */
    m4a_engine_init(&engine, 44100.0f);
    engine.portamentoEnabled = true;
    m4a_engine_set_voicegroup(&engine, voices);
    m4a_engine_program_change(&engine, 0, 0);
    m4a_engine_cc(&engine, 0, 0x5, 2);
    m4a_engine_note_on(&engine, 0, 60, 100);
    m4a_engine_note_off(&engine, 0, 60);
    /* Render long enough for the release to fully fade out */
    for (int i = 0; i < 200; i++)
        m4a_engine_process(&engine, outL, outR, 1024);
    ASSERT(find_pcm_channel(&engine, 0) == NULL, "portamento gap: old note fully released");
    m4a_engine_note_on(&engine, 0, 67, 100);
    M4APCMChannel *gapCh = find_pcm_channel(&engine, 0);
    ASSERT(gapCh != NULL, "portamento gap: new note allocated");
    ASSERT(gapCh->status & CHN_ENV_MASK, "portamento gap: new note triggers its own envelope");
    /* But the pitch still glides from the previous note's key */
    ASSERT(engine.tracks[0].portamentoGliding, "portamento gap: pitch still glides");
    m4a_engine_destroy(&engine);

    /* ---- CC 5 value 0 disables portamento ---- */
    m4a_engine_init(&engine, 44100.0f);
    engine.portamentoEnabled = true;
    m4a_engine_set_voicegroup(&engine, voices);
    m4a_engine_program_change(&engine, 0, 0);
    m4a_engine_cc(&engine, 0, 0x5, 2);
    m4a_engine_note_on(&engine, 0, 60, 100);
    m4a_engine_cc(&engine, 0, 0x5, 0);
    m4a_engine_note_off(&engine, 0, 60);
    m4a_engine_note_on(&engine, 0, 67, 100);
    ASSERT(!engine.tracks[0].portamentoGliding, "portamento off: no glide");
    M4APCMChannel *offCh = find_pcm_channel(&engine, 0);
    {
        M4AEngine ref;
        m4a_engine_init(&ref, 44100.0f);
        m4a_engine_set_voicegroup(&ref, voices);
        m4a_engine_program_change(&ref, 0, 0);
        m4a_engine_note_on(&ref, 0, 67, 100);
        ASSERT_EQ(offCh->frequency, find_pcm_channel(&ref, 0)->frequency,
                  "portamento off: note plays at its own pitch");
        m4a_engine_destroy(&ref);
    }
    m4a_engine_destroy(&engine);

    /* ---- CGB: legato note inherits the shared channel slot ---- */
    m4a_engine_init(&engine, 44100.0f);
    engine.portamentoEnabled = true;
    m4a_engine_set_voicegroup(&engine, voices);
    m4a_engine_program_change(&engine, 0, 1);  /* square 1 */
    m4a_engine_cc(&engine, 0, 0x5, 2);
    m4a_engine_note_on(&engine, 0, 60, 100);
    M4ACGBChannel *cgb = &engine.cgbChannels[0];
    ASSERT(cgb->status & CHN_ON, "cgb portamento: first note started");
    uint32_t cgbFreqStart = cgb->frequency;
    uint8_t cgbEnvBefore = cgb->envelopeVolume;
    ASSERT(cgbEnvBefore != 0, "cgb portamento: first note envelope is audible");
    m4a_engine_process(&engine, outL, outR, 256);
    m4a_engine_note_off(&engine, 0, 60);
    m4a_engine_note_on(&engine, 0, 67, 100);
    ASSERT_EQ(cgb->status, CHN_ENV_SUSTAIN, "cgb portamento: legato note sustains, no retrigger");
    ASSERT(engine.tracks[0].portamentoGliding, "cgb portamento: glide started");
    ASSERT_EQ(cgb->frequency, cgbFreqStart, "cgb portamento: glide starts at previous pitch");
    m4a_engine_process(&engine, outL, outR, 1024);  /* finish the glide... */
    m4a_engine_process(&engine, outL, outR, 1024);
    ASSERT(!engine.tracks[0].portamentoGliding, "cgb portamento: glide completes");
    {
        M4AEngine ref;
        m4a_engine_init(&ref, 44100.0f);
        m4a_engine_set_voicegroup(&ref, voices);
        m4a_engine_program_change(&ref, 0, 1);
        m4a_engine_note_on(&ref, 0, 67, 100);
        ASSERT_EQ(cgb->frequency, ref.cgbChannels[0].frequency,
                  "cgb portamento: final pitch equals target note pitch");
        m4a_engine_destroy(&ref);
    }
    m4a_engine_destroy(&engine);

    free(wd);
}

/* Regression tests for intermittent gap-detection failures: the glide start
 * key must come from note history, not from whether the previous note's
 * channel happened to still be alive when CC 5 arrived. */
static void test_portamento_prev_key_tracking(void)
{
    printf("Testing portamento prev-key tracking...\n");

    int dataSize = 64;
    WaveData *wd = calloc(1, sizeof(WaveData) + dataSize + 1);
    wd->type = 0;
    wd->status = 0xC000;
    wd->freq = 0x01000000;
    wd->loopStart = 0;
    wd->size = dataSize;
    wd->data = (int8_t *)((uint8_t *)wd + sizeof(WaveData));
    for (int i = 0; i < dataSize; i++)
        wd->data[i] = (int8_t)(127.0 * sin(2.0 * 3.14159265 * i / dataSize));
    wd->data[dataSize] = wd->data[0];

    ToneData voices[128];
    memset(voices, 0, sizeof(voices));
    /* Decaying (piano-like) voice: envelope dies during a held note */
    voices[0].type = VOICE_DIRECTSOUND;
    voices[0].key = 60;
    voices[0].wav = wd;
    voices[0].attack = 0xFF;
    voices[0].decay = 200;
    voices[0].sustain = 0;
    voices[0].release = 150;

    M4AEngine engine;
    float outL[1024], outR[1024];

    /* ---- Symptom "no glide": envelope fully decayed before CC 5 arrives ---- */
    m4a_engine_init(&engine, 44100.0f);
    engine.portamentoEnabled = true;
    m4a_engine_set_voicegroup(&engine, voices);
    m4a_engine_program_change(&engine, 0, 0);
    m4a_engine_note_on(&engine, 0, 48, 0x60);
    /* Hold until the decay-to-zero envelope frees the channel */
    for (int i = 0; i < 100; i++)
        m4a_engine_process(&engine, outL, outR, 1024);
    {
        int alive = 0;
        for (int i = 0; i < MAX_PCM_CHANNELS; i++)
            if (engine.pcmChannels[i].status & CHN_ON) alive = 1;
        ASSERT(!alive, "prev-key: held note's channel fully decayed");
    }
    /* CC 5 + zero-gap pair, exactly like the user's minimal MIDI */
    m4a_engine_cc(&engine, 0, 0x5, 5);
    m4a_engine_note_off(&engine, 0, 48);
    m4a_engine_note_on(&engine, 0, 50, 0x60);
    ASSERT(engine.tracks[0].portamentoGliding, "prev-key: glide starts even after envelope died");
    ASSERT_EQ(engine.tracks[0].portamentoPrevKey, 48, "prev-key: glide starts from previous note");
    ASSERT_EQ(engine.tracks[0].portamentoTargetKey, 50, "prev-key: glide targets new note");
    m4a_engine_destroy(&engine);

    /* ---- Symptom "wrong frequency": notes played while portamento is off
     * must still update the glide start key ---- */
    m4a_engine_init(&engine, 44100.0f);
    engine.portamentoEnabled = true;
    m4a_engine_set_voicegroup(&engine, voices);
    m4a_engine_program_change(&engine, 0, 0);
    m4a_engine_cc(&engine, 0, 0x5, 5);
    m4a_engine_note_on(&engine, 0, 40, 0x60);   /* records key 40 */
    m4a_engine_note_off(&engine, 0, 40);
    m4a_engine_cc(&engine, 0, 0x5, 0);          /* portamento off */
    m4a_engine_note_on(&engine, 0, 60, 0x60);   /* must record key 60 */
    m4a_engine_note_off(&engine, 0, 60);
    m4a_engine_cc(&engine, 0, 0x5, 5);          /* portamento back on */
    m4a_engine_note_on(&engine, 0, 62, 0x60);
    ASSERT(engine.tracks[0].portamentoGliding, "prev-key: glide after re-enable");
    ASSERT_EQ(engine.tracks[0].portamentoPrevKey, 60,
              "prev-key: glide starts from last note, not stale pre-disable key");
    m4a_engine_destroy(&engine);

    /* ---- Transport stop / All Notes Off forgets note history: the first
     * note after resume must not glide, but CC 5 (a parameter) survives ---- */
    voices[0].decay = 0;
    voices[0].sustain = 0xFF;
    m4a_engine_init(&engine, 44100.0f);
    engine.portamentoEnabled = true;
    m4a_engine_set_voicegroup(&engine, voices);
    m4a_engine_program_change(&engine, 0, 0);
    m4a_engine_cc(&engine, 0, 0x5, 5);
    m4a_engine_note_on(&engine, 0, 48, 0x60);          /* records key 48 */
    m4a_engine_all_notes_off(&engine, 0);              /* DAW stop (CC 123) */
    m4a_engine_note_on(&engine, 0, 60, 0x60);
    ASSERT(!engine.tracks[0].portamentoGliding, "reset: no glide after All Notes Off");
    ASSERT_EQ(engine.tracks[0].portamentoDuration, 5, "reset: CC 5 duration survives");
    /* ...but a subsequent legato note glides again as usual */
    m4a_engine_note_off(&engine, 0, 60);
    m4a_engine_note_on(&engine, 0, 62, 0x60);
    ASSERT(engine.tracks[0].portamentoGliding, "reset: portamento works after resume");
    ASSERT_EQ(engine.tracks[0].portamentoPrevKey, 60, "reset: glide from post-resume note");
    /* All Sound Off / transport stop clears every track, even mid-glide */
    m4a_engine_all_sound_off(&engine);
    ASSERT(!engine.tracks[0].portamentoGliding, "reset: All Sound Off cancels glide");
    ASSERT_EQ(engine.tracks[0].portamentoPrevKey, 0, "reset: All Sound Off forgets prev key");
    m4a_engine_destroy(&engine);

    /* ---- Interrupted glide: a new note that interrupts an in-progress glide
     * restarts from the original previous-note key (snapping back to that
     * pitch), NOT the current interpolated position.  Matches the GBA, which
     * only advances portamentoPrevKey when a glide actually completes. ---- */
    voices[0].decay = 0;
    voices[0].sustain = 0xFF;  /* sustained so legato inheritance works */
    m4a_engine_init(&engine, 44100.0f);
    engine.portamentoEnabled = true;
    m4a_engine_set_voicegroup(&engine, voices);
    m4a_engine_program_change(&engine, 0, 0);
    m4a_engine_cc(&engine, 0, 0x5, 8);
    m4a_engine_note_on(&engine, 0, 48, 0x60);
    m4a_engine_process(&engine, outL, outR, 1024);
    m4a_engine_note_off(&engine, 0, 48);
    m4a_engine_note_on(&engine, 0, 60, 0x60);   /* glide 48 -> 60 */
    /* Let the glide get partway (8 ticks = 1200 units; ~3 VBlanks in) */
    m4a_engine_process(&engine, outL, outR, 1024);
    m4a_engine_process(&engine, outL, outR, 1024);
    ASSERT(engine.tracks[0].portamentoGliding, "interrupt: glide still in progress");
    m4a_engine_note_off(&engine, 0, 60);
    m4a_engine_note_on(&engine, 0, 50, 0x60);   /* interrupt mid-glide */
    ASSERT(engine.tracks[0].portamentoGliding, "interrupt: new glide starts");
    ASSERT_EQ(engine.tracks[0].portamentoPrevKey, 48,
              "interrupt: new glide restarts from original previous-note key");
    ASSERT_EQ(engine.tracks[0].portamentoTargetKey, 50, "interrupt: new glide targets new note");
    m4a_engine_destroy(&engine);

    free(wd);
}

/* Pulse-width modulation: PWMC (CC 0x17) selects a duty-cycle pattern and PWMS
 * (CC 0x19) sets the speed; the engine then cycles a square channel's duty
 * cycle through the pattern at VBlank rate. */
static void test_pwm(void)
{
    printf("Testing pulse-width modulation...\n");

    /* Square voice with a default 50% (duty 2) duty cycle. */
    ToneData voices[128];
    memset(voices, 0, sizeof(voices));
    voices[0].type = VOICE_SQUARE_1;
    voices[0].key = 60;
    voices[0].wavePointer = (uint32_t *)(uintptr_t)2;  /* default duty: 50% */
    voices[0].attack = 0;
    voices[0].decay = 0;
    voices[0].sustain = 15;
    voices[0].release = 3;

    M4AEngine engine;
    float outL[1024], outR[1024];

    /* At 44100 Hz one VBlank tick is ~738.4 samples; 739 fires exactly one. */
    const int SAMPLES_PER_TICK = 739;

    m4a_engine_init(&engine, 44100.0f);
    engine.pwmEnabled = true;
    m4a_engine_set_voicegroup(&engine, voices);
    m4a_engine_program_change(&engine, 0, 0);

    M4ACGBChannel *sq = &engine.cgbChannels[0];  /* SQUARE_1 -> cgb ch 0 */

    /* Enable PWM with pattern 2 (ascending {0,1,2}) at speed 2. */
    m4a_engine_cc(&engine, 0, 0x17, 2);  /* PWMC: pattern 2 */
    m4a_engine_cc(&engine, 0, 0x19, 2);  /* PWMS: speed 2 */
    ASSERT_EQ(engine.tracks[0].pwmPattern, 2, "pwm: PWMC sets pattern");
    ASSERT_EQ(engine.tracks[0].pwmSpeed, 2, "pwm: PWMS sets speed");
    ASSERT(engine.pwmActiveFlag, "pwm: enabling PWMS marks engine active");

    /* A new note starts on the pattern's first duty cycle (step 0 = 0), not the
     * voice's default duty.  Mirrors the SF_START branch on the GBA. */
    m4a_engine_note_on(&engine, 0, 60, 100);
    ASSERT_EQ(sq->dutyCycle, 0, "pwm: note starts on pattern step 0");
    ASSERT_EQ(engine.tracks[0].pwmStep, 0, "pwm: note resets pattern step");

    /* speed=2: duty holds for one tick, then advances each second tick. */
    m4a_engine_process(&engine, outL, outR, SAMPLES_PER_TICK);  /* tick 1 */
    ASSERT_EQ(sq->dutyCycle, 0, "pwm: duty holds during speed countdown");
    m4a_engine_process(&engine, outL, outR, SAMPLES_PER_TICK);  /* tick 2 */
    ASSERT_EQ(sq->dutyCycle, 1, "pwm: duty advances to step 1");
    m4a_engine_process(&engine, outL, outR, SAMPLES_PER_TICK);  /* tick 3 */
    m4a_engine_process(&engine, outL, outR, SAMPLES_PER_TICK);  /* tick 4 */
    ASSERT_EQ(sq->dutyCycle, 2, "pwm: duty advances to step 2");
    m4a_engine_process(&engine, outL, outR, SAMPLES_PER_TICK);  /* tick 5 */
    m4a_engine_process(&engine, outL, outR, SAMPLES_PER_TICK);  /* tick 6 */
    ASSERT_EQ(sq->dutyCycle, 0, "pwm: pattern wraps back to step 0");

    /* Disabling the effect (PWMS 0) restores the voice's default duty cycle. */
    m4a_engine_cc(&engine, 0, 0x19, 0);
    ASSERT_EQ(engine.tracks[0].pwmSpeed, 0, "pwm: PWMS 0 disables");
    ASSERT_EQ(sq->dutyCycle, 2, "pwm: disabling restores voice default duty");

    /* The disabled effect no longer modulates the duty. */
    for (int i = 0; i < 4; i++)
        m4a_engine_process(&engine, outL, outR, SAMPLES_PER_TICK);
    ASSERT_EQ(sq->dutyCycle, 2, "pwm: duty stays fixed once disabled");

    /* An out-of-range pattern index falls back to 0 (no effect). */
    m4a_engine_cc(&engine, 0, 0x17, 200);
    ASSERT_EQ(engine.tracks[0].pwmPattern, 0, "pwm: out-of-range pattern clamps to 0");

    m4a_engine_destroy(&engine);
}

/* Square-1 hardware frequency sweep (NR10): a square-1 voice whose pan_sweep
 * byte holds a sweep value glides its frequency in "hardware" at 128 Hz,
 * f' = f +/- (f >> shift) every `time` clocks; an upward sweep that overflows
 * the 11-bit register silences the channel.  Matches ply_note/CgbSound in
 * pokeemerald and the sweep unit in mGBA's gb/audio.c. */
static void test_sweep(void)
{
    printf("Testing square-1 frequency sweep...\n");

    ToneData voices[128];
    memset(voices, 0, sizeof(voices));
    /* voice 0: sweep down -- time=1, dir=down, shift=3 (NR10 = 0x1B) */
    voices[0].type = VOICE_SQUARE_1;
    voices[0].key = 60;
    voices[0].wavePointer = (uint32_t *)(uintptr_t)2;  /* 50% duty */
    voices[0].sustain = 15;
    voices[0].release = 3;
    voices[0].panSweep = 0x1B;
    /* voice 1: sweep up -- time=1, dir=up, shift=1 (NR10 = 0x11) */
    voices[1] = voices[0];
    voices[1].panSweep = 0x11;
    /* voice 2: pan bit set -- pan_sweep is a pan, not a sweep */
    voices[2] = voices[0];
    voices[2].panSweep = 0xC0;

    M4AEngine engine;
    float outL[1024], outR[1024];

    m4a_engine_init(&engine, 44100.0f);
    m4a_engine_set_voicegroup(&engine, voices);
    M4ACGBChannel *sq = &engine.cgbChannels[0];  /* SQUARE_1 -> cgb ch 0 */

    /* --- Downward sweep: f' = f - (f >> 3) every 1/128 s --- */
    m4a_engine_program_change(&engine, 0, 0);
    m4a_engine_note_on(&engine, 0, 60, 100);
    ASSERT_EQ(sq->sweep, 0x1B, "sweep: NR10 byte captured at note-on");
    ASSERT(sq->sweepEnabled, "sweep: unit armed at note-on");
    ASSERT(!sq->sweepMuted, "sweep: downward sweep never mutes");
    uint32_t f0 = sq->frequency;
    ASSERT_EQ(sq->sweepShadowFreq, f0, "sweep: shadow loaded from frequency register");

    /* The 128 Hz sweep clock first fires after 44100/128 = ~344.5 samples. */
    m4a_engine_process(&engine, outL, outR, 300);
    ASSERT_EQ(sq->frequency, f0, "sweep: no change before the first 128 Hz clock");
    m4a_engine_process(&engine, outL, outR, 100);
    uint32_t f1 = f0 - (f0 >> 3);
    ASSERT_EQ(sq->frequency, f1, "sweep: first step lowers frequency by f>>3");
    m4a_engine_process(&engine, outL, outR, 345);
    ASSERT_EQ(sq->frequency, f1 - (f1 >> 3), "sweep: second step compounds");

    /* A long downward sweep decays toward register 0 and keeps sounding. */
    for (int i = 0; i < 44; i++)
        m4a_engine_process(&engine, outL, outR, 1000);
    ASSERT(sq->frequency < 16, "sweep: downward sweep decays toward register 0");
    ASSERT(!sq->sweepMuted, "sweep: downward sweep stays audible");
    ASSERT(sq->status & CHN_ON, "sweep: software envelope unaffected by sweeping");

    /* --- Upward sweep: overflow past 2047 silences the channel --- */
    m4a_engine_all_sound_off(&engine);
    m4a_engine_program_change(&engine, 0, 1);
    m4a_engine_note_on(&engine, 0, 36, 100);  /* low key: passes the initial check */
    ASSERT(!sq->sweepMuted, "sweep: low-key upward sweep passes initial overflow check");
    uint32_t fu0 = sq->frequency;
    for (int i = 0; i < 44 && !sq->sweepMuted; i++)
        m4a_engine_process(&engine, outL, outR, 1000);
    ASSERT(sq->sweepMuted, "sweep: upward overflow silences the channel");
    ASSERT(sq->frequency > fu0, "sweep: frequency rose before the overflow");
    ASSERT(sq->frequency < 2048, "sweep: register never exceeds 11 bits");
    ASSERT(sq->status & CHN_ON, "sweep: software envelope keeps running while muted");
    m4a_engine_process(&engine, outL, outR, 1024);
    float energy = 0.0f;
    for (int i = 0; i < 1024; i++)
        energy += fabsf(outL[i]) + fabsf(outR[i]);
    ASSERT(energy == 0.0f, "sweep: overflowed channel renders silence");

    /* --- pan_sweep with the pan bit (or zero time bits) is not a sweep --- */
    m4a_engine_all_sound_off(&engine);
    m4a_engine_program_change(&engine, 0, 2);
    m4a_engine_note_on(&engine, 0, 60, 100);
    ASSERT_EQ(sq->sweep, 0x08, "sweep: pan-bearing pan_sweep maps to inert NR10 value 8");
    ASSERT(!sq->sweepEnabled, "sweep: inert value disables the unit");
    uint32_t fi = sq->frequency;
    m4a_engine_process(&engine, outL, outR, 1000);
    ASSERT_EQ(sq->frequency, fi, "sweep: no sweeping on a pan-bearing voice");

    m4a_engine_destroy(&engine);
}

/*
 * The LFO (mod-wheel vibrato) advances inside the engine's tempo loop, so its
 * rate is proportional to the tempo set via m4a_engine_set_tempo_bpm: each
 * VBlank tick adds tempoI to an accumulator and the LFO steps once per 150
 * accumulated.  This is the contract poryaaaa_render relies on — it must feed
 * the MIDI tempo into the engine, otherwise the LFO runs at the init-default
 * tempo and ends up the wrong speed (the bug where a 76 BPM song's vibrato ran
 * ~2x too fast because the renderer left the engine at its 150 default).
 */
static void test_lfo_tempo_scaling(void)
{
    printf("Testing LFO speed scales with tempo...\n");

    /* lfoSpeed = 1 means the phase accumulator advances by exactly one LFO
     * step per LFO tick, so lfoSpeedC directly counts the steps taken (as long
     * as it stays below 256 and so does not wrap). */
    const int TICKS = 100;

    /* Fast: 150 BPM -> one LFO step per VBlank tick -> 100 steps. */
    M4AEngine fast;
    m4a_engine_init(&fast, 44100.0f);
    m4a_engine_cc(&fast, 0, 0x15, 1);   /* LFOS: LFO speed */
    m4a_engine_cc(&fast, 0, 0x1, 64);   /* mod wheel: LFO depth (must be nonzero) */
    m4a_engine_set_tempo_bpm(&fast, 150.0);
    for (int i = 0; i < TICKS; i++)
        m4a_engine_tick(&fast);
    int fastSteps = fast.tracks[0].lfoSpeedC;
    ASSERT_EQ(fastSteps, 100, "lfo: 150 BPM steps once per VBlank");
    m4a_engine_destroy(&fast);

    /* Slow: 75 BPM -> one LFO step every other VBlank tick -> 50 steps. */
    M4AEngine slow;
    m4a_engine_init(&slow, 44100.0f);
    m4a_engine_cc(&slow, 0, 0x15, 1);
    m4a_engine_cc(&slow, 0, 0x1, 64);
    m4a_engine_set_tempo_bpm(&slow, 75.0);
    for (int i = 0; i < TICKS; i++)
        m4a_engine_tick(&slow);
    int slowSteps = slow.tracks[0].lfoSpeedC;
    ASSERT_EQ(slowSteps, 50, "lfo: 75 BPM steps once per two VBlanks");
    ASSERT_EQ(fastSteps, slowSteps * 2, "lfo: half tempo = half LFO rate");
    m4a_engine_destroy(&slow);

    /* With no mod depth the LFO stays put regardless of tempo. */
    M4AEngine off;
    m4a_engine_init(&off, 44100.0f);
    m4a_engine_cc(&off, 0, 0x15, 1);    /* speed set... */
    m4a_engine_cc(&off, 0, 0x1, 0);     /* ...but depth zero */
    m4a_engine_set_tempo_bpm(&off, 150.0);
    for (int i = 0; i < TICKS; i++)
        m4a_engine_tick(&off);
    ASSERT_EQ(off.tracks[0].lfoSpeedC, 0, "lfo: zero depth never advances");
    m4a_engine_destroy(&off);
}

/* Build a Golden Sun synth WaveData: a header with size == 0 followed by the
 * waveform descriptor bytes (same layout as the duty/saw/triangle .bin
 * samples used by ipatix's improved mixer). */
static WaveData *make_synth_wave(const uint8_t *desc, int descLen)
{
    WaveData *wd = calloc(1, sizeof(WaveData) + 16 + 1);
    wd->type = 0;
    wd->status = 0x4000;      /* loop flag, as in the real synth samples */
    wd->freq = 0x01058920;    /* 64-sample wave period = middle C at key 60 */
    wd->loopStart = 0;
    wd->size = 0;
    wd->data = (int8_t *)((uint8_t *)wd + sizeof(WaveData));
    memcpy(wd->data, desc, descLen);
    return wd;
}

/* Golden Sun synth instruments: end-to-end engine behavior. */
static void test_golden_sun_synth(void)
{
    printf("Testing Golden Sun synth instruments...\n");

    static const uint8_t descTriangle[] = { 0x80, 0x02, 0x00, 0x00 };
    static const uint8_t descSaw[]      = { 0x80, 0x01, 0x00, 0x00 };
    static const uint8_t descPulse[]    = { 0x80, 0x00, 0x00, 0x20, 0x80, 0x10 };

    WaveData *wdTri   = make_synth_wave(descTriangle, sizeof(descTriangle));
    WaveData *wdSaw   = make_synth_wave(descSaw,      sizeof(descSaw));
    WaveData *wdPulse = make_synth_wave(descPulse,    sizeof(descPulse));

    ToneData voices[128];
    memset(voices, 0, sizeof(voices));
    for (int v = 0; v < 3; v++) {
        voices[v].type = VOICE_DIRECTSOUND;
        voices[v].key = 60;
        voices[v].attack = 0xFF;   /* instant attack */
        voices[v].sustain = 0xFF;
    }
    voices[0].wav = wdTri;
    voices[1].wav = wdSaw;
    voices[2].wav = wdPulse;

    /* Each descriptor is classified and audible, and the channel never
     * exhausts (a synth has no sample end). */
    static const uint8_t expectType[3] =
        { M4A_SYNTH_TRIANGLE, M4A_SYNTH_SAW, M4A_SYNTH_PULSE };
    for (int v = 0; v < 3; v++) {
        M4AEngine engine;
        m4a_engine_init(&engine, 44100.0f);
        m4a_engine_set_voicegroup(&engine, voices);
        m4a_engine_program_change(&engine, 0, (uint8_t)v);
        m4a_engine_cc(&engine, 0, 7, 127);
        m4a_engine_note_on(&engine, 0, 60, 127);
        ASSERT_EQ(engine.pcmChannels[0].synthType, expectType[v],
                  "gs synth: descriptor classification");
        if (expectType[v] == M4A_SYNTH_TRIANGLE) {
            /* Triangle starts at 90 degrees phase to avoid a pop. */
            ASSERT_EQ(engine.pcmChannels[0].fw, 0x40000000u,
                      "gs synth: triangle 90-degree phase start");
        }
        if (expectType[v] == M4A_SYNTH_PULSE) {
            /* First duty threshold from duty.bin's descriptor: the LFO
             * accumulator advances one step (0x20 << 24), the offset
             * (0x10 << 24) is added, and the positive result scaled by the
             * mod amount 0x80: (0x30000000 >> 8) * 0x80 = 0x18000000. */
            ASSERT_EQ(engine.pcmChannels[0].synthPulseDuty, 0x18000000u,
                      "gs synth: initial pulse duty threshold");
        }
        float outL[8192], outR[8192];
        m4a_engine_process(&engine, outL, outR, 8192);
        float maxVal = 0;
        for (int i = 0; i < 8192; i++)
            if (fabsf(outL[i]) > maxVal) maxVal = fabsf(outL[i]);
        ASSERT(maxVal > 0.01f, "gs synth: produces audio");
        ASSERT((engine.pcmChannels[0].status & CHN_ON) != 0,
               "gs synth: channel keeps playing (no sample end)");
        m4a_engine_destroy(&engine);
    }

    free(wdTri);
    free(wdSaw);
    free(wdPulse);
}

/* Golden Sun synth waveform shapes: drive m4a_pcm_channel_render directly
 * with a hand-built channel so the generated waveforms can be checked
 * sample-by-sample against the GBA algorithm. */
static void test_golden_sun_synth_waveforms(void)
{
    printf("Testing Golden Sun synth waveform shapes...\n");

    /* Channel template: sustain state, unity-ish volume (128 -> value >> 1),
     * frequency chosen so phase advances 0x800000 per sample = 512-sample
     * wave period. */
    M4APCMChannel base;
    memset(&base, 0, sizeof(base));
    base.status = CHN_ENV_SUSTAIN;
    base.frequency = 0x100000;
    base.envelopeVolumeLeft = 128;
    base.envelopeVolumeRight = 128;

    /* Triangle: equivalent amplitude is exactly a full-scale sample
     * (+/-128); with the volume of 128 the output is value >> 1 = +/-64.
     * From the 90-degree start it must begin at the zero crossing, rise
     * monotonically to +64, and repeat with a 512-sample period. */
    {
        M4APCMChannel ch = base;
        ch.synthType = M4A_SYNTH_TRIANGLE;
        ch.fw = 0x40000000u;
        int32_t out[1024];
        int32_t maxV = -1000, minV = 1000;
        for (int i = 0; i < 1024; i++) {
            int32_t l = 0, r = 0;
            m4a_pcm_channel_render(&ch, &l, &r);
            out[i] = l;
            if (l > maxV) maxV = l;
            if (l < minV) minV = l;
        }
        ASSERT_EQ(out[0], 0, "gs triangle: starts at the zero crossing");
        ASSERT_EQ(maxV, 64, "gs triangle: positive peak (full-scale equivalent)");
        ASSERT_EQ(minV, -64, "gs triangle: negative peak");
        int rising = 1;
        for (int i = 1; i < 127; i++)
            if (out[i] < out[i - 1]) rising = 0;
        ASSERT(rising, "gs triangle: monotonic first quarter");
        int periodic = 1;
        for (int i = 0; i < 512; i++)
            if (out[i] != out[i + 512]) periodic = 0;
        ASSERT(periodic, "gs triangle: 512-sample period");
    }

    /* Pulse with a fixed 50% duty (base duty 0x80, no modulation): half the
     * period high, half low, at +/-32 ((+/-64 * 128) >> 8) -- half the
     * equivalent amplitude of a full-scale sample, matching agbplay. */
    {
        static const uint8_t desc[] = { 0x80, 0x00, 0x80, 0x00, 0x00, 0x00 };
        WaveData *wd = make_synth_wave(desc, sizeof(desc));
        M4APCMChannel ch = base;
        ch.synthType = M4A_SYNTH_PULSE;
        ch.wav = wd;
        ch.synthPulseDuty = 0x80000000u;
        int high = 0, low = 0, other = 0;
        for (int i = 0; i < 512; i++) {
            int32_t l = 0, r = 0;
            m4a_pcm_channel_render(&ch, &l, &r);
            if (l == 32) high++;
            else if (l == -32) low++;
            else other++;
        }
        ASSERT_EQ(high, 256, "gs pulse: 50 percent duty high samples");
        ASSERT_EQ(low, 256, "gs pulse: 50 percent duty low samples");
        ASSERT_EQ(other, 0, "gs pulse: output is strictly two-level");
        free(wd);
    }

    /* Pseudo sawtooth: filter state settles to ~2x the raw wave (-112..112),
     * mixed at halved volume for an equivalent amplitude of ~+/-112; with
     * the volume of 128 the output lands in ~+/-56. */
    {
        M4APCMChannel ch = base;
        ch.synthType = M4A_SYNTH_SAW;
        int32_t maxV = -100000, minV = 100000;
        for (int i = 0; i < 2048; i++) {
            int32_t l = 0, r = 0;
            m4a_pcm_channel_render(&ch, &l, &r);
            if (l > maxV) maxV = l;
            if (l < minV) minV = l;
        }
        ASSERT(maxV > 30 && maxV <= 56, "gs saw: positive range");
        ASSERT(minV < -30 && minV >= -56, "gs saw: negative range");
    }
}

int main(void)
{
    printf("=== M4A Engine Unit Tests ===\n\n");

    test_umul3232H32();
    test_scale_table();
    test_midi_key_to_freq();
    test_midi_key_to_cgb_freq();
    test_trk_vol_pit_set();
    test_engine_init();
    test_basic_audio();
    test_polyphony_stealing();
    test_poly_overflow_debug();
    test_portamento();
    test_portamento_prev_key_tracking();
    test_pwm();
    test_sweep();
    test_lfo_tempo_scaling();
    test_golden_sun_synth();
    test_golden_sun_synth_waveforms();

    printf("\n=== Results: %d/%d tests passed ===\n", tests_passed, tests_run);
    return (tests_passed == tests_run) ? 0 : 1;
}
