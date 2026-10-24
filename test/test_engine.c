#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <math.h>
#include "m4a_engine.h"
#include "m4a_channel.h"
#include "m4a_tables.h"
#include "m4a_song.h"
#include "m4a_sequencer.h"

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
    ASSERT_EQ(engine.masterVolume, 12, "master volume");
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
    ASSERT_EQ(engine.polyEvents[0].tick, M4A_POLY_TICK_NONE,
              "poly: event tick defaults to none when host never sets the clock");
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
    engine.polyEventClock = 1234;  /* host timeline position of the thief's note-on */
    m4a_engine_note_on(&engine, 0, 67, 100);
    ASSERT_EQ(engine.polyStealCount[1], 1, "poly: steal counted for victim track");
    ASSERT_EQ(engine.polyEvents[0].type, M4A_POLY_STOLEN, "poly: event type stolen");
    ASSERT_EQ(engine.polyEvents[0].trackIndex, 1, "poly: steal victim track");
    ASSERT_EQ(engine.polyEvents[0].midiKey, 60, "poly: steal victim key");
    ASSERT_EQ(engine.polyEvents[0].byTrack, 0, "poly: steal attributed to new track");
    ASSERT_EQ(engine.polyEvents[0].tick, 1234, "poly: event stamped with host-set clock");
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

    /* ---- Invert mode: host audition notes stay audible ---- */
    m4a_engine_init(&engine, 44100.0f);
    m4a_engine_set_voicegroup(&engine, voices);
    engine.maxPcmChannels = 1;
    m4a_engine_set_poly_debug_invert(&engine, true);
    m4a_engine_program_change(&engine, 0, 0);
    m4a_engine_cc(&engine, 0, 7, 127);
    engine.auditionNote = true;
    m4a_engine_note_on(&engine, 0, 60, 100);
    ASSERT(engine.pcmChannels[0].audition, "poly invert: audition flag copied to channel");
    m4a_engine_process(&engine, outL, outR, 1024);
    {
        float maxVal = 0;
        for (int i = 0; i < 1024; i++) {
            if (fabs(outL[i]) > maxVal) maxVal = fabs(outL[i]);
            if (fabs(outR[i]) > maxVal) maxVal = fabs(outR[i]);
        }
        ASSERT(maxVal > 0.001f, "poly invert: audition note is audible");
    }
    engine.auditionNote = false;
    m4a_engine_note_on(&engine, 0, 62, 100); /* sequenced reuse of the channel */
    ASSERT(!engine.pcmChannels[0].audition,
           "poly invert: sequenced reuse clears the audition flag");
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

    /* Regression: PWMC alone must re-arm the engine's active flag when the
     * track's speed is still set.  The sequence "PWMS n ... PWMC 0 ... PWMC m"
     * previously left the effect permanently dead, because pwmActiveFlag
     * decays while all patterns are 0 and only the PWMS handler set it. */
    m4a_engine_cc(&engine, 0, 0x19, 2);  /* PWMS 2: speed set, effect armed */
    m4a_engine_cc(&engine, 0, 0x17, 0);  /* PWMC 0: pattern off */
    m4a_engine_process(&engine, outL, outR, SAMPLES_PER_TICK);
    ASSERT(!engine.pwmActiveFlag, "pwm: active flag decays while all patterns are 0");
    m4a_engine_cc(&engine, 0, 0x17, 2);  /* PWMC alone re-enables the effect */
    ASSERT(engine.pwmActiveFlag, "pwm: PWMC alone re-arms the active flag");
    m4a_engine_process(&engine, outL, outR, SAMPLES_PER_TICK);
    m4a_engine_process(&engine, outL, outR, SAMPLES_PER_TICK);
    ASSERT_EQ(sq->dutyCycle, 1, "pwm: modulation resumes after PWMC re-arm");

    m4a_engine_destroy(&engine);
}

/* TONEDATA_TYPE_FIX ("_alt" PSG voices): voice_square_1_alt and friends set
 * bit 3 of the voice type, which makes CgbSound round the 11-bit frequency
 * register to an even value -- (freq + 1) & 0x7fe under Emerald's 65536 Hz DAC
 * PWM rate -- so the oscillator period divides the PWM period evenly.  The
 * audible result is a slight detune that grows with pitch.  CgbSound guards
 * the rounding with `ch < 4`, so voice_noise_alt must behave like voice_noise. */
static void test_cgb_fixed_freq(void)
{
    printf("Testing CGB fixed-frequency (_alt) voices...\n");

    ToneData voices[128];
    memset(voices, 0, sizeof(voices));
    /* 0/1: square-1 plain and _alt.  2/3: wave plain and _alt.
     * 4/5: noise plain and _alt (the bit must be inert there). */
    static const uint8_t types[6] = {
        VOICE_SQUARE_1, VOICE_SQUARE_1_ALT,
        VOICE_PROGRAMMABLE_WAVE, VOICE_PROGRAMMABLE_WAVE_ALT,
        VOICE_NOISE, VOICE_NOISE_ALT,
    };
    static uint32_t waveTable[4] = { 0x89ABCDEF, 0x01234567, 0xFEDCBA98, 0x76543210 };
    for (int i = 0; i < 6; i++) {
        voices[i].type = types[i];
        voices[i].key = 60;
        voices[i].attack = 0;
        voices[i].decay = 0;
        voices[i].sustain = 15;
        voices[i].release = 3;
        if (types[i] == VOICE_PROGRAMMABLE_WAVE || types[i] == VOICE_PROGRAMMABLE_WAVE_ALT)
            voices[i].wavePointer = waveTable;
    }

    M4AEngine engine;
    float outL[1024], outR[1024];
    m4a_engine_init(&engine, 44100.0f);
    m4a_engine_set_voicegroup(&engine, voices);

    /* Square-1 (cgb channel 0) and wave (cgb channel 2). */
    const int progs[2] = { 0, 2 };
    const int chans[2] = { 0, 2 };
    const char *names[2] = { "square-1", "wave" };

    for (int v = 0; v < 2; v++) {
        M4ACGBChannel *ch = &engine.cgbChannels[chans[v]];
        int oddSeen = 0, differed = 0;

        for (int key = 36; key <= 120; key++) {
            m4a_engine_program_change(&engine, 0, progs[v]);
            m4a_engine_note_on(&engine, 0, (uint8_t)key, 100);
            uint32_t plain = ch->frequency;
            ASSERT(!ch->fixedFreq, "fixfreq: plain voice does not set fixedFreq");

            m4a_engine_program_change(&engine, 0, progs[v] + 1);
            m4a_engine_note_on(&engine, 0, (uint8_t)key, 100);
            uint32_t alt = ch->frequency;
            ASSERT(ch->fixedFreq, "fixfreq: _alt voice sets fixedFreq");

            if (alt != ((plain + 1) & 0x7FE)) {
                fprintf(stderr, "  %s key %d: plain %u, alt %u\n",
                        names[v], key, plain, alt);
                differed = -1;  /* poisoned: reported once below */
            }
            if (plain & 1) oddSeen = 1;
            if (alt != plain && differed == 0) differed = 1;

            m4a_engine_note_off(&engine, 0, (uint8_t)key);
        }

        ASSERT(differed >= 0, "fixfreq: _alt frequency is (plain + 1) & 0x7fe");
        /* Guards against a vacuous pass: if no key in the sweep produced an odd
         * register value, the rounding would be a no-op everywhere. */
        ASSERT(oddSeen, "fixfreq: sweep covers keys with an odd frequency register");
        ASSERT(differed == 1, "fixfreq: rounding actually shifts some frequencies");
    }

    /* Noise: the FIX bit is inert (CgbSound's `ch < 4` guard), so the _alt and
     * plain voices must produce identical frequency registers. */
    M4ACGBChannel *noise = &engine.cgbChannels[3];
    int noiseDiffered = 0;
    for (int key = 36; key <= 120; key++) {
        m4a_engine_program_change(&engine, 0, 4);
        m4a_engine_note_on(&engine, 0, (uint8_t)key, 100);
        uint32_t plain = noise->frequency;

        m4a_engine_program_change(&engine, 0, 5);
        m4a_engine_note_on(&engine, 0, (uint8_t)key, 100);
        ASSERT(!noise->fixedFreq, "fixfreq: voice_noise_alt leaves fixedFreq clear");
        if (noise->frequency != plain) noiseDiffered = 1;

        m4a_engine_note_off(&engine, 0, (uint8_t)key);
    }
    ASSERT(!noiseDiffered, "fixfreq: voice_noise_alt matches voice_noise");

    /* The rounding also applies on the tick path (MO_PIT: pitch bend, vibrato),
     * not just at note-on. */
    M4ACGBChannel *sq = &engine.cgbChannels[0];
    m4a_engine_program_change(&engine, 0, 1);   /* square-1 _alt */
    m4a_engine_note_on(&engine, 0, 90, 100);
    int benta = 0;
    for (int bend = -8000; bend <= 8000; bend += 250) {
        m4a_engine_pitch_bend(&engine, 0, (int16_t)bend);
        m4a_engine_process(&engine, outL, outR, 739);  /* one VBlank tick */
        if (sq->frequency & 1) benta = 1;
    }
    ASSERT(!benta, "fixfreq: bent _alt frequency stays quantized on the tick path");

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

/*
 * ply_lfos only stores the speed; clear_modM (phase and offset back to 0) runs
 * only for a speed of 0.  So repeating LFOS, or turning a knob routed to it,
 * keeps the vibrato going instead of restarting its phase each time.
 */
static void test_lfos_keeps_phase(void)
{
    printf("Testing LFOS keeps the LFO phase...\n");

    M4AEngine engine;
    m4a_engine_init(&engine, 44100.0f);
    m4a_engine_set_tempo_bpm(&engine, 150.0);
    m4a_engine_cc(&engine, 0, 0x15, 1);  /* LFOS */
    m4a_engine_cc(&engine, 0, 0x1, 64);  /* MOD */
    for (int i = 0; i < 10; i++)
        m4a_engine_tick(&engine);
    ASSERT_EQ(engine.tracks[0].lfoSpeedC, 10, "lfos: 10 steps at speed 1");
    int8_t modM = engine.tracks[0].modM;
    ASSERT(modM != 0, "lfos: the LFO has an offset");

    /* The same speed again, then a faster one: the phase carries on. */
    m4a_engine_cc(&engine, 0, 0x15, 1);
    ASSERT_EQ(engine.tracks[0].lfoSpeedC, 10, "lfos: a repeat keeps the phase");
    ASSERT_EQ(engine.tracks[0].modM, modM, "lfos: a repeat keeps the offset");
    m4a_engine_cc(&engine, 0, 0x15, 2);
    ASSERT_EQ(engine.tracks[0].lfoSpeedC, 10, "lfos: a new speed keeps the phase");
    ASSERT_EQ(engine.tracks[0].modM, modM, "lfos: a new speed keeps the offset");
    m4a_engine_tick(&engine);
    ASSERT_EQ(engine.tracks[0].lfoSpeedC, 12, "lfos: the phase advances at the new speed");

    /* Speed 0 is clear_modM. */
    m4a_engine_cc(&engine, 0, 0x15, 0);
    ASSERT_EQ(engine.tracks[0].lfoSpeedC, 0, "lfos: 0 resets the phase");
    ASSERT_EQ(engine.tracks[0].modM, 0, "lfos: 0 clears the offset");

    m4a_engine_destroy(&engine);
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

/* The audible CGB volume is the hardware envelope unit (NRx2 low bits +
 * trigger), reloaded by CgbSound's MO_VOL writes and stepping on its own
 * free-running 64 Hz clock -- not the ~60 Hz software envelope, which during
 * decay/release is silent bookkeeping.  A percussive noise voice (attack 0,
 * decay 1, sustain 0) at envelope goal 2 must drop to level 1 at the first
 * 64 Hz clock (~15.6 ms) and be silent by ~31 ms; the software envelope alone
 * holds the goal for two engine frames and would still be sounding at 40 ms. */
static void test_cgb_hw_envelope(void)
{
    printf("Testing CGB hardware envelope...\n");

    M4AEngine engine;
    m4a_engine_init(&engine, 48000.0f);

    ToneData voices[128];
    memset(voices, 0, sizeof(voices));
    voices[0].type = VOICE_NOISE;
    voices[0].key = 60;
    voices[0].attack = 0;
    voices[0].decay = 1;
    voices[0].sustain = 0;
    voices[0].release = 1;
    /* wavePointer NULL: NR43 period bit 0 = 15-bit LFSR */

    m4a_engine_set_voicegroup(&engine, voices);
    m4a_engine_program_change(&engine, 0, 0);
    m4a_engine_cc(&engine, 0, 7, 127);

    /* Velocity 20 at full track volume: left+right = 38 -> envelope goal 2 */
    m4a_engine_note_on(&engine, 0, 80, 20);

    /* Render 60 ms and record the peak level of three windows:
     * W0 = 0-14 ms (goal level 2), W1 = 17-30 ms (level 1 after the first
     * 64 Hz step), W2 = 35-45 ms (hardware envelope dead at 31.25 ms). */
    float peakW0 = 0, peakW1 = 0, peakW2 = 0;
    float outL[480], outR[480];
    for (int block = 0; block < 6; block++) {
        m4a_engine_process(&engine, outL, outR, 480);
        for (int i = 0; i < 480; i++) {
            int s = block * 480 + i;
            float v = fabsf(outL[i]);
            if (fabsf(outR[i]) > v) v = fabsf(outR[i]);
            if (s < 672) {
                if (v > peakW0) peakW0 = v;
            } else if (s >= 816 && s < 1440) {
                if (v > peakW1) peakW1 = v;
            } else if (s >= 1680 && s < 2160) {
                if (v > peakW2) peakW2 = v;
            }
        }
    }

    ASSERT(peakW0 > 0.01f, "hw env: goal level audible at note start");
    ASSERT(peakW1 > 0.005f, "hw env: level 1 audible after first 64 Hz step");
    ASSERT(peakW1 < peakW0 * 0.75f, "hw env: level dropped at ~15.6 ms");
    ASSERT(peakW2 == 0.0f, "hw env: silent once the unit reaches zero (~31 ms)");

    m4a_engine_destroy(&engine);
}

/* The square channels' duty position is never reset by an NRx4 trigger (mGBA
 * GBAudioWriteNR14/NR24 leave the duty index and its timer alone; the counter
 * only advances with time, and keeps advancing while the channel is silent).
 * So retriggering a square -- back-to-back notes, or a note after a rest --
 * must continue the free-running waveform exactly where a single long note
 * would be, with no phase restart, and two squares that share a frequency
 * must keep their relative phase across one of them being retriggered. */
static void test_cgb_square_phase_continuity(void)
{
    printf("Testing CGB square phase continuity across retriggers...\n");

    ToneData voices[128];
    memset(voices, 0, sizeof(voices));
    for (int i = 0; i < 2; i++) {
        voices[i].type = i == 0 ? VOICE_SQUARE_1 : VOICE_SQUARE_2;
        voices[i].key = 60;
        voices[i].attack = 0;
        voices[i].decay = 0;
        voices[i].sustain = 15;
        voices[i].release = 0;   /* note-off silences the channel at once */
        voices[i].wavePointer = (uint32_t *)(uintptr_t)2;  /* 50% duty */
    }

    enum { BLOCK = 512, BLOCKS = 6, REST = 300 };
    static float refL[BLOCK * BLOCKS], refR[BLOCK * BLOCKS];
    static float outL[BLOCK], outR[BLOCK];

    /* Reference: one long square-1 note. */
    M4AEngine ref;
    m4a_engine_init(&ref, 44100.0f);
    m4a_engine_set_voicegroup(&ref, voices);
    m4a_engine_program_change(&ref, 0, 0);
    m4a_engine_note_on(&ref, 0, 69, 100);
    for (int b = 0; b < BLOCKS; b++)
        m4a_engine_process(&ref, refL + b * BLOCK, refR + b * BLOCK, BLOCK);

    /* Same key retriggered back-to-back after one block, then again after a
     * 300-sample rest.  Every rendered sample of a sounding stretch must equal
     * the long note's sample at the same absolute position. */
    M4AEngine eng;
    m4a_engine_init(&eng, 44100.0f);
    m4a_engine_set_voicegroup(&eng, voices);
    m4a_engine_program_change(&eng, 0, 0);
    m4a_engine_note_on(&eng, 0, 69, 100);
    m4a_engine_process(&eng, outL, outR, BLOCK);
    ASSERT(memcmp(outL, refL, sizeof(outL)) == 0, "sq phase: first note matches reference");

    /* Back-to-back retrigger. */
    m4a_engine_note_off(&eng, 0, 69);
    m4a_engine_note_on(&eng, 0, 69, 100);
    m4a_engine_process(&eng, outL, outR, BLOCK);
    int mism = 0;
    for (int i = 0; i < BLOCK; i++)
        if (outL[i] != refL[BLOCK + i] || outR[i] != refR[BLOCK + i]) mism++;
    ASSERT_EQ(mism, 0, "sq phase: back-to-back retrigger continues the waveform sample-exactly");
    /* Guard against a vacuous pass: the reference stretch is not silence. */
    float energy = 0;
    for (int i = 0; i < BLOCK; i++) energy += fabsf(refL[BLOCK + i]);
    ASSERT(energy > 1.0f, "sq phase: reference note is audible");

    /* Rest, then retrigger: the phase kept running through the silence.
     * Note-off takes effect at the next engine tick (CgbSound runs once per
     * VBlank), so render one block for it to land, then a REST of silence. */
    int pos = 2 * BLOCK;
    m4a_engine_note_off(&eng, 0, 69);
    m4a_engine_process(&eng, outL, outR, BLOCK);
    pos += BLOCK;
    ASSERT(!(eng.cgbChannels[0].status & CHN_ON), "sq phase: note-off landed within a block");
    m4a_engine_process(&eng, outL, outR, REST);
    energy = 0;
    for (int i = 0; i < REST; i++) energy += fabsf(outL[i]) + fabsf(outR[i]);
    ASSERT(energy == 0.0f, "sq phase: silent during the rest");
    pos += REST;
    m4a_engine_note_on(&eng, 0, 69, 100);
    m4a_engine_process(&eng, outL, outR, BLOCK);
    mism = 0;
    for (int i = 0; i < BLOCK; i++)
        if (outL[i] != refL[pos + i] || outR[i] != refR[pos + i]) mism++;
    ASSERT_EQ(mism, 0, "sq phase: retrigger after a rest resumes at the free-running phase");

    /* And a retrigger must not simply happen to land on phase 0: the
     * reference itself is not periodic in BLOCK samples at this key. */
    mism = 0;
    for (int i = 0; i < BLOCK; i++)
        if (refL[i] != refL[BLOCK + i]) mism++;
    ASSERT(mism > 0, "sq phase: reference waveform is not BLOCK-periodic (test is not vacuous)");

    /* Two squares in unison: identical from engine reset, and still identical
     * after square 2 alone is released, rests, and retriggers. */
    M4AEngine duo;
    m4a_engine_init(&duo, 44100.0f);
    m4a_engine_set_voicegroup(&duo, voices);
    m4a_engine_program_change(&duo, 0, 0);
    m4a_engine_program_change(&duo, 1, 1);
    m4a_engine_note_on(&duo, 0, 69, 100);
    m4a_engine_note_on(&duo, 1, 69, 100);
    m4a_engine_process(&duo, outL, outR, BLOCK);
    ASSERT_EQ(duo.cgbChannels[0].frequency, duo.cgbChannels[1].frequency, "sq phase: unison squares share a frequency");
    ASSERT_EQ(duo.cgbChannels[0].phase, duo.cgbChannels[1].phase, "sq phase: unison squares are phase-locked");
    m4a_engine_note_off(&duo, 1, 69);
    m4a_engine_process(&duo, outL, outR, BLOCK);
    m4a_engine_process(&duo, outL, outR, REST);
    ASSERT(!(duo.cgbChannels[1].status & CHN_ON), "sq phase: square 2 went silent");
    m4a_engine_note_on(&duo, 1, 69, 100);
    m4a_engine_process(&duo, outL, outR, BLOCK);
    ASSERT_EQ(duo.cgbChannels[0].phase, duo.cgbChannels[1].phase, "sq phase: unison holds across a rest and retrigger of one square");
    ASSERT(duo.cgbChannels[1].status & CHN_ON, "sq phase: retriggered square 2 is sounding");

    m4a_engine_destroy(&ref);
    m4a_engine_destroy(&eng);
    m4a_engine_destroy(&duo);
}

/* The wave channel's position also survives a retrigger: on the GBA the wave
 * RAM is a rotating shift register (mGBA rotates it a nibble per step and the
 * trigger's window reset is DMG-only), and CgbSound rewrites the RAM at note
 * start only when the voice's table differs from the loaded one.  So a
 * repeated note on the same wave voice keeps its position (the trigger only
 * re-arms the step timer, restarting the current step), while a note on a
 * voice with a different table starts from position 0 -- as recorded
 * in-game. */
static void test_cgb_wave_phase_continuity(void)
{
    printf("Testing CGB wave position continuity across retriggers...\n");

    static uint32_t tableA[4] = { 0x89ABCDEF, 0x01234567, 0xFEDCBA98, 0x76543210 };
    static uint32_t tableB[4] = { 0x00112233, 0x44556677, 0x8899AABB, 0xCCDDEEFF };
    ToneData voices[128];
    memset(voices, 0, sizeof(voices));
    for (int i = 0; i < 2; i++) {
        voices[i].type = VOICE_PROGRAMMABLE_WAVE;
        voices[i].key = 60;
        voices[i].attack = 0;
        voices[i].decay = 0;
        voices[i].sustain = 15;
        voices[i].release = 0;
        voices[i].wavePointer = i == 0 ? tableA : tableB;
    }

    M4AEngine eng;
    float outL[64], outR[64];
    m4a_engine_init(&eng, 44100.0f);
    m4a_engine_set_voicegroup(&eng, voices);
    M4ACGBChannel *ch = &eng.cgbChannels[2];

    m4a_engine_program_change(&eng, 0, 0);
    m4a_engine_note_on(&eng, 0, 60, 100);
    ASSERT_EQ(ch->phase, 0u, "wave phase: first note on a fresh table starts at position 0");
    ASSERT(ch->currentWavePointer == tableA, "wave phase: table A loaded");

    /* Run into the note, stopping at a sample where the position is mid-step
     * and not 0, so a reset to 0 and a kept position are distinguishable. */
    for (int i = 0; i < 700; i++)
        m4a_engine_process(&eng, outL, outR, 1);
    while (((ch->phase >> 27) & 0x1F) == 0 || (ch->phase & 0x07FFFFFFu) == 0)
        m4a_engine_process(&eng, outL, outR, 1);
    uint32_t posBefore = (ch->phase >> 27) & 0x1F;

    /* Same voice, back-to-back: position kept, current step restarted. */
    m4a_engine_note_off(&eng, 0, 60);
    m4a_engine_note_on(&eng, 0, 60, 100);
    ASSERT_EQ((ch->phase >> 27) & 0x1F, posBefore, "wave phase: same-table retrigger keeps the wave position");
    ASSERT_EQ(ch->phase & 0x07FFFFFFu, 0u, "wave phase: retrigger re-arms the step timer (step restarts)");
    ASSERT(ch->currentWavePointer == tableA, "wave phase: same table not re-uploaded");

    /* Position keeps advancing through the retriggered note. */
    for (int i = 0; i < 300; i++)
        m4a_engine_process(&eng, outL, outR, 1);
    ASSERT(((ch->phase >> 27) & 0x1F) != posBefore, "wave phase: wave keeps running after the retrigger");

    /* A different table is uploaded fresh: playback restarts at position 0. */
    m4a_engine_program_change(&eng, 0, 1);
    m4a_engine_note_off(&eng, 0, 60);
    m4a_engine_note_on(&eng, 0, 60, 100);
    ASSERT_EQ(ch->phase, 0u, "wave phase: new table starts at position 0");
    ASSERT(ch->currentWavePointer == tableB, "wave phase: table B loaded");

    /* And that table's position now persists across its own retriggers. */
    for (int i = 0; i < 500; i++)
        m4a_engine_process(&eng, outL, outR, 1);
    while (((ch->phase >> 27) & 0x1F) == 0)
        m4a_engine_process(&eng, outL, outR, 1);
    posBefore = (ch->phase >> 27) & 0x1F;
    m4a_engine_note_off(&eng, 0, 60);
    m4a_engine_note_on(&eng, 0, 60, 100);
    ASSERT_EQ((ch->phase >> 27) & 0x1F, posBefore, "wave phase: table B retrigger keeps position");

    m4a_engine_destroy(&eng);
}

/* The noise LFSR clocks at up to 524288 Hz -- ~11 steps per 48 kHz output
 * sample at the highest keys -- so each output sample must be the
 * time-weighted average of every LFSR level inside its window (mGBA's
 * _coalesceNoiseChannel).  Point-sampling the LFSR instead aliases the fast
 * clock into a rail-to-rail two-level square: every sample sits at +/-peak.
 * A sustained key-80 noise note (NR43 0x00, 524288 Hz) must therefore
 * produce mostly INTERMEDIATE values, well inside the rails. */
static void test_cgb_noise_band_limiting(void)
{
    printf("Testing CGB noise band-limiting...\n");

    M4AEngine engine;
    m4a_engine_init(&engine, 48000.0f);

    ToneData voices[128];
    memset(voices, 0, sizeof(voices));
    voices[0].type = VOICE_NOISE;
    voices[0].key = 60;
    voices[0].attack = 0;
    voices[0].decay = 0;
    voices[0].sustain = 15;  /* hold at full level: isolate the LFSR shape */
    voices[0].release = 0;
    /* wavePointer NULL: NR43 period bit 0 = 15-bit LFSR */

    m4a_engine_set_voicegroup(&engine, voices);
    m4a_engine_program_change(&engine, 0, 0);
    m4a_engine_cc(&engine, 0, 7, 127);

    /* Key 80 = gNoiseTable[59] = 0x00: the fastest LFSR clock (524288 Hz). */
    m4a_engine_note_on(&engine, 0, 80, 127);

    float outL[480], outR[480];
    /* Skip the first block (note-start ramp), then gather statistics. */
    m4a_engine_process(&engine, outL, outR, 480);
    float peak = 0;
    int total = 0, interior = 0, positive = 0, negative = 0;
    float samples[1920];
    for (int block = 0; block < 4; block++) {
        m4a_engine_process(&engine, outL, outR, 480);
        for (int i = 0; i < 480; i++)
            samples[total++] = outL[i];
    }
    for (int i = 0; i < total; i++)
        if (fabsf(samples[i]) > peak) peak = fabsf(samples[i]);
    for (int i = 0; i < total; i++) {
        if (fabsf(samples[i]) < peak * 0.7f) interior++;
        if (samples[i] > 0) positive++;
        if (samples[i] < 0) negative++;
    }

    ASSERT(peak > 0.001f, "noise blimit: note is audible");
    ASSERT(positive > total / 10 && negative > total / 10,
           "noise blimit: both polarities present");
    /* Averaging ~11 random LFSR levels leaves nearly every sample far from
     * the rails; the aliasing point-sampler leaves interior == 0. */
    ASSERT(interior > total / 2,
           "noise blimit: fast-clock noise is mostly intermediate values");

    m4a_engine_destroy(&engine);
}


/* Reverse (TONEDATA_TYPE_REV / voice_directsound_alt) samples play from the
 * last sample to the first, interpolating toward the earlier sample, ignore
 * loops, and end when they run off the front -- the SoundMainRAM_Unk1
 * reverse path.  Driven directly with a hand-built channel at unity volume
 * (256 -> value >> 8 is the sample itself). */
/* What a hand-driven PCM channel at envelope volume 128 outputs for sample v. */
#define PCM_OUT(v) (((int32_t)(v) * 128) >> 8)

static void test_pcm_reverse(void)
{
    printf("Testing reverse-played samples...\n");

    enum { N = 8 };
    WaveData *wd = calloc(1, sizeof(WaveData) + N + 1);
    wd->type = 0;
    wd->status = 0x4000;  /* looped -- must be ignored in reverse */
    wd->loopStart = 2;
    wd->size = N;
    wd->data = (int8_t *)((uint8_t *)wd + sizeof(WaveData));
    for (int i = 0; i < N; i++)
        wd->data[i] = (int8_t)(10 * (i + 1));  /* 10, 20, ..., 80 */
    wd->data[N] = wd->data[N - 1];

    /* One source sample per output sample. */
    {
        M4APCMChannel ch;
        memset(&ch, 0, sizeof(ch));
        m4a_pcm_channel_start(&ch, wd, VOICE_DIRECTSOUND_ALT);
        ch.status = CHN_ENV_SUSTAIN;
        ch.frequency = 0x800000;
        ch.envelopeVolumeLeft = 128;
        ch.envelopeVolumeRight = 128;
        ASSERT(ch.currentPointer == wd->data + N, "reverse: starts at the end of the data");

        int ok = 1;
        for (int i = 0; i < N; i++) {
            int32_t l = 0, r = 0;
            m4a_pcm_channel_render(&ch, &l, &r);
            if (l != PCM_OUT(10 * (N - i)) || r != l) ok = 0;
        }
        ASSERT(ok, "reverse: plays the samples last-to-first");
        ASSERT_EQ(ch.status, 0, "reverse: stops at the front (loop ignored)");
        int32_t l = 0, r = 0;
        m4a_pcm_channel_render(&ch, &l, &r);
        ASSERT_EQ(l, 0, "reverse: silent once stopped");
    }

    /* Half speed: interpolates toward the previous sample. */
    {
        M4APCMChannel ch;
        memset(&ch, 0, sizeof(ch));
        m4a_pcm_channel_start(&ch, wd, VOICE_DIRECTSOUND_ALT);
        ch.status = CHN_ENV_SUSTAIN;
        ch.frequency = 0x400000;
        ch.envelopeVolumeLeft = 128;
        ch.envelopeVolumeRight = 128;
        static const int32_t expect[6] = { 80, 75, 70, 65, 60, 55 };
        int ok = 1;
        for (int i = 0; i < 6; i++) {
            int32_t l = 0, r = 0;
            m4a_pcm_channel_render(&ch, &l, &r);
            if (l != PCM_OUT(expect[i])) ok = 0;
        }
        ASSERT(ok, "reverse: half speed interpolates toward the earlier sample");
        ASSERT_EQ(ch.count, 5, "reverse: count tracks the samples left");
        ASSERT(ch.currentPointer == wd->data + 5, "reverse: pointer stays one past the current sample");
    }

    /* Half speed with the FIX bit (0x18): sample-and-hold like the main
     * mixer, no interpolation.  (A FIX voice only steps exactly one source
     * sample per output at the GBA's mix rate; at other rates, or after a
     * pitch refresh, the special path must honour the flag itself.) */
    {
        M4APCMChannel ch;
        memset(&ch, 0, sizeof(ch));
        m4a_pcm_channel_start(&ch, wd, VOICE_DIRECTSOUND_ALT | VOICE_TYPE_FIX);
        ch.status = CHN_ENV_SUSTAIN;
        ch.frequency = 0x400000;
        ch.envelopeVolumeLeft = 128;
        ch.envelopeVolumeRight = 128;
        static const int32_t expect[6] = { 80, 80, 70, 70, 60, 60 };
        int ok = 1;
        for (int i = 0; i < 6; i++) {
            int32_t l = 0, r = 0;
            m4a_pcm_channel_render(&ch, &l, &r);
            if (l != PCM_OUT(expect[i])) ok = 0;
        }
        ASSERT(ok, "reverse fix: half speed holds each sample (no interpolation)");
    }

    /* Through the engine: a voice_directsound_alt voice on a rising ramp
     * comes out falling, and the FIX+REV combination (0x18) still runs
     * backwards. */
    {
        enum { RN = 4096 };
        WaveData *ramp = calloc(1, sizeof(WaveData) + RN + 1);
        ramp->freq = 13379 << 10;
        ramp->size = RN;
        ramp->data = (int8_t *)((uint8_t *)ramp + sizeof(WaveData));
        for (int i = 0; i < RN; i++)
            ramp->data[i] = (int8_t)(-100 + (200 * i) / RN);
        ramp->data[RN] = ramp->data[RN - 1];

        ToneData voices[128];
        memset(voices, 0, sizeof(voices));
        for (int v = 0; v < 3; v++) {
            voices[v].key = 60;
            voices[v].wav = ramp;
            voices[v].attack = 0xFF;
            voices[v].sustain = 0xFF;
        }
        voices[0].type = VOICE_DIRECTSOUND;
        voices[1].type = VOICE_DIRECTSOUND_ALT;
        voices[2].type = VOICE_DIRECTSOUND_ALT | VOICE_TYPE_FIX;

        for (int v = 0; v < 3; v++) {
            M4AEngine engine;
            m4a_engine_init(&engine, 44100.0f);
            m4a_engine_set_voicegroup(&engine, voices);
            m4a_engine_program_change(&engine, 0, (uint8_t)v);
            m4a_engine_cc(&engine, 0, 7, 127);
            m4a_engine_note_on(&engine, 0, 60, 127);
            float outL[512], outR[512];
            m4a_engine_process(&engine, outL, outR, 512);
            /* Compare an early and a late output of the block (the very
             * first output precedes the channel's first mix and is 0):
             * the ramp covers ~4096 source samples, so 512 outputs never
             * reach the other end at any of these rates. */
            ASSERT(fabsf(outL[8]) > 0.01f, "reverse engine: audible");
            if (v == 0)
                ASSERT(outL[504] > outL[8], "engine: plain sample rises with the ramp");
            else
                ASSERT(outL[504] < outL[8], "reverse engine: alt voice falls (plays backwards)");
            m4a_engine_destroy(&engine);
        }
        free(ramp);
    }

    free(wd);
}

/* Portamento legato inheritance across a reverse voice and a forward voice
 * that share the same WaveData.  The two mixers keep different pointer
 * conventions (reverse: one past the current sample; forward: at it), so a
 * legato note must only inherit from a channel playing in the same
 * direction -- otherwise a forward channel would start at data + count and
 * read `count` samples past the end of the data (heap over-read). */
static void test_pcm_reverse_legato_inherit(void)
{
    printf("Testing reverse/forward legato inheritance...\n");

    enum { N = 1000 };
    WaveData *wd = calloc(1, sizeof(WaveData) + N + 1);
    wd->freq = 13379 << 10;
    wd->size = N;
    wd->data = (int8_t *)((uint8_t *)wd + sizeof(WaveData));
    for (int i = 0; i < N; i++)
        wd->data[i] = (int8_t)(127.0 * sin(2.0 * 3.14159265 * i / 50.0));
    wd->data[N] = wd->data[N - 1];

    ToneData voices[128];
    memset(voices, 0, sizeof(voices));
    for (int v = 0; v < 2; v++) {
        voices[v].key = 60;
        voices[v].wav = wd;
        voices[v].attack = 0xFF;
        voices[v].sustain = 0xFF;
        voices[v].release = 220;
    }
    voices[0].type = VOICE_DIRECTSOUND_ALT;  /* reverse */
    voices[1].type = VOICE_DIRECTSOUND;      /* forward, same wav */

    static const struct { uint8_t from, to; const char *what; } cases[] = {
        { 0, 1, "reverse -> forward" },
        { 1, 0, "forward -> reverse" },
    };
    for (size_t c = 0; c < sizeof(cases) / sizeof(cases[0]); c++) {
        M4AEngine engine;
        float outL[256], outR[256];
        m4a_engine_init(&engine, 44100.0f);
        engine.portamentoEnabled = true;
        m4a_engine_set_voicegroup(&engine, voices);
        m4a_engine_cc(&engine, 0, 0x5, 2);  /* portamento on: 2-tick glide */

        m4a_engine_program_change(&engine, 0, cases[c].from);
        m4a_engine_note_on(&engine, 0, 60, 100);
        m4a_engine_process(&engine, outL, outR, 32);
        M4APCMChannel *first = find_pcm_channel(&engine, 0);
        ASSERT(first != NULL, "rev legato: first note has a channel");
        ASSERT(first->count < N && first->count > 0, "rev legato: first note is mid-sample");

        /* Zero-gap note on the other voice: must NOT inherit the position. */
        m4a_engine_program_change(&engine, 0, cases[c].to);
        m4a_engine_note_off(&engine, 0, 60);
        m4a_engine_note_on(&engine, 0, 67, 100);
        M4APCMChannel *second = find_pcm_channel(&engine, 0);
        ASSERT(second != NULL, "rev legato: second note has a channel");
        ASSERT(second != first, "rev legato: opposite direction gets a fresh channel");
        ASSERT_EQ(second->count, N, "rev legato: opposite direction starts the sample over");
        int8_t *expectPtr = (voices[cases[c].to].type & VOICE_TYPE_REV) ? wd->data + N : wd->data;
        ASSERT(second->currentPointer == expectPtr, "rev legato: pointer uses the new direction's convention");
        ASSERT((first->status & CHN_ON) != 0, "rev legato: previous channel keeps releasing (not silenced)");

        /* Render well past the point where a wrongly-inherited forward
         * channel would have run off the end of the data. */
        for (int i = 0; i < 40; i++)
            m4a_engine_process(&engine, outL, outR, 256);
        ASSERT(second->currentPointer >= wd->data && second->currentPointer <= wd->data + N,
               "rev legato: position stays inside the data");
        m4a_engine_destroy(&engine);
    }

    /* Same direction (reverse -> reverse) still inherits as usual. */
    {
        M4AEngine engine;
        float outL[256], outR[256];
        m4a_engine_init(&engine, 44100.0f);
        engine.portamentoEnabled = true;
        m4a_engine_set_voicegroup(&engine, voices);
        m4a_engine_cc(&engine, 0, 0x5, 2);
        m4a_engine_program_change(&engine, 0, 0);
        m4a_engine_note_on(&engine, 0, 60, 100);
        m4a_engine_process(&engine, outL, outR, 32);
        M4APCMChannel *first = find_pcm_channel(&engine, 0);
        int8_t *posBefore = first->currentPointer;
        int32_t countBefore = first->count;
        m4a_engine_note_off(&engine, 0, 60);
        m4a_engine_note_on(&engine, 0, 67, 100);
        M4APCMChannel *second = find_pcm_channel(&engine, 0);
        ASSERT(second != NULL, "rev legato: same-direction note has a channel");
        ASSERT(second->currentPointer == posBefore, "rev legato: same direction inherits the position");
        ASSERT_EQ(second->count, countBefore, "rev legato: same direction inherits the count");
        ASSERT_EQ(second->status & ~CHN_LOOP, CHN_ENV_SUSTAIN, "rev legato: same direction sustains");
        m4a_engine_destroy(&engine);
    }

    free(wd);
}

/* Build a DPCM-compressed WaveData whose samples follow the given nibble
 * stream: block k starts with rawStart[k], every later sample adds
 * gDeltaEncodingTable[nibble].  Returns the WaveData and fills `decoded`
 * with the samples the GBA decoder must produce. */
static WaveData *make_dpcm_wave(uint32_t size, uint32_t loopStart, int looped,
                                int8_t *decoded)
{
    uint32_t blocks = size / M4A_DPCM_BLOCK_SAMPLES + 1;
    uint32_t bytes = blocks * M4A_DPCM_BLOCK_BYTES;
    WaveData *wd = calloc(1, sizeof(WaveData) + bytes + 1);
    wd->type = 1;
    wd->status = looped ? 0x4000 : 0;
    wd->loopStart = loopStart;
    wd->size = size;
    wd->data = (int8_t *)((uint8_t *)wd + sizeof(WaveData));
    uint8_t *out = (uint8_t *)wd->data;
    uint32_t seed = 12345;
    for (uint32_t b = 0; b < blocks; b++) {
        uint8_t *blk = out + b * M4A_DPCM_BLOCK_BYTES;
        uint8_t level = (uint8_t)(int8_t)(-40 + (int)(b * 25));
        blk[0] = level;
        uint32_t idx = b * M4A_DPCM_BLOCK_SAMPLES;
        if (idx < size + 1) decoded[idx] = (int8_t)level;
        for (int i = 1; i < M4A_DPCM_BLOCK_SAMPLES; i++) {
            seed = seed * 1103515245u + 12345u;
            /* Small steps (nibbles 0-2 up, 13-15 down) keep the level in range. */
            uint8_t nib = (uint8_t)((seed >> 16) % 6);
            if (nib >= 3) nib = (uint8_t)(nib + 10);
            level = (uint8_t)(level + gDeltaEncodingTable[nib]);
            if (i == 1)
                blk[1] = nib;                     /* low nibble only */
            else if (i % 2 == 0)
                blk[i / 2 + 1] = (uint8_t)(nib << 4);   /* high nibble first */
            else
                blk[i / 2 + 1] |= nib;            /* then low nibble */
            if (idx + i < size + 1) decoded[idx + i] = (int8_t)level;
        }
    }
    return wd;
}

/* DPCM-compressed samples (WaveData.type != 0, the `cry` voices) decode
 * block by block through the delta table -- SoundMainRAM_Unk2 -- and play
 * forward with loops or in reverse without. */
static void test_pcm_dpcm(void)
{
    printf("Testing DPCM-compressed samples...\n");

    enum { SIZE = 100, LOOP = 40 };
    int8_t decoded[SIZE + 1];
    WaveData *wd = make_dpcm_wave(SIZE, LOOP, 1, decoded);

    /* Forward at one sample per output: exact decode, then the loop wraps
     * back to loopStart. */
    {
        M4APCMChannel ch;
        memset(&ch, 0, sizeof(ch));
        m4a_pcm_channel_start(&ch, wd, VOICE_CRY);
        ch.status = CHN_ENV_SUSTAIN | CHN_LOOP;
        ch.frequency = 0x800000;
        ch.envelopeVolumeLeft = 128;
        ch.envelopeVolumeRight = 128;
        int ok = 1, wrapOk = 1;
        for (int i = 0; i < SIZE; i++) {
            int32_t l = 0, r = 0;
            m4a_pcm_channel_render(&ch, &l, &r);
            if (l != PCM_OUT(decoded[i])) ok = 0;
        }
        ASSERT(ok, "dpcm: forward decode matches the delta table");
        for (int i = 0; i < 2 * (SIZE - LOOP); i++) {
            int32_t l = 0, r = 0;
            m4a_pcm_channel_render(&ch, &l, &r);
            if (l != PCM_OUT(decoded[LOOP + i % (SIZE - LOOP)])) wrapOk = 0;
        }
        ASSERT(wrapOk, "dpcm: forward loop wraps to loopStart");
        ASSERT((ch.status & CHN_ON) != 0, "dpcm: looped channel keeps playing");
    }

    /* Forward at half speed: interpolates between decoded neighbours, and
     * the look-ahead at index `size` reads the extra block the encoders
     * emit. */
    {
        M4APCMChannel ch;
        memset(&ch, 0, sizeof(ch));
        m4a_pcm_channel_start(&ch, wd, VOICE_CRY);
        ch.status = CHN_ENV_SUSTAIN;
        ch.frequency = 0x400000;
        ch.envelopeVolumeLeft = 128;
        ch.envelopeVolumeRight = 128;
        int ok = 1;
        for (int i = 0; i < 2 * SIZE - 2; i++) {
            int32_t l = 0, r = 0;
            m4a_pcm_channel_render(&ch, &l, &r);
            int32_t s0 = decoded[i / 2], s1 = decoded[i / 2 + 1];
            int32_t expect = (i % 2 == 0) ? s0 : s0 + (((s1 - s0) * 0x400000) >> 23);
            if (l != PCM_OUT(expect)) ok = 0;
        }
        ASSERT(ok, "dpcm: half speed interpolates between decoded samples");
    }

    /* Forward at half speed with the FIX bit (0x28): sample-and-hold, as
     * the main mixer does for FIX voices. */
    {
        M4APCMChannel ch;
        memset(&ch, 0, sizeof(ch));
        m4a_pcm_channel_start(&ch, wd, VOICE_CRY | VOICE_TYPE_FIX);
        ch.status = CHN_ENV_SUSTAIN;
        ch.frequency = 0x400000;
        ch.envelopeVolumeLeft = 128;
        ch.envelopeVolumeRight = 128;
        int ok = 1;
        for (int i = 0; i < 2 * SIZE - 2; i++) {
            int32_t l = 0, r = 0;
            m4a_pcm_channel_render(&ch, &l, &r);
            if (l != PCM_OUT(decoded[i / 2])) ok = 0;
        }
        ASSERT(ok, "dpcm fix: half speed holds each decoded sample");
    }

    /* Reverse (cry_reverse): last sample first, loop ignored. */
    {
        M4APCMChannel ch;
        memset(&ch, 0, sizeof(ch));
        m4a_pcm_channel_start(&ch, wd, VOICE_CRY_REVERSE);
        ch.status = CHN_ENV_SUSTAIN | CHN_LOOP;
        ch.frequency = 0x800000;
        ch.envelopeVolumeLeft = 128;
        ch.envelopeVolumeRight = 128;
        int ok = 1;
        for (int i = 0; i < SIZE; i++) {
            int32_t l = 0, r = 0;
            m4a_pcm_channel_render(&ch, &l, &r);
            if (l != PCM_OUT(decoded[SIZE - 1 - i])) ok = 0;
        }
        ASSERT(ok, "dpcm reverse: plays the decoded samples last-to-first");
        ASSERT_EQ(ch.status, 0, "dpcm reverse: stops at the front (loop ignored)");
    }

    /* Through the engine, a cry voice is audible (uncompressed treatment
     * of DPCM bytes would still be "audible", so also check the first
     * output against the first decoded sample scaled by the volume). */
    {
        ToneData voices[128];
        memset(voices, 0, sizeof(voices));
        voices[0].type = VOICE_CRY;
        voices[0].key = 60;
        voices[0].wav = wd;
        voices[0].attack = 0xFF;
        voices[0].sustain = 0xFF;
        M4AEngine engine;
        m4a_engine_init(&engine, 44100.0f);
        m4a_engine_set_voicegroup(&engine, voices);
        m4a_engine_program_change(&engine, 0, 0);
        m4a_engine_cc(&engine, 0, 7, 127);
        m4a_engine_note_on(&engine, 0, 60, 127);
        int32_t l = 0, r = 0;
        M4APCMChannel *ch = &engine.pcmChannels[0];
        m4a_pcm_channel_render(ch, &l, &r);
        ASSERT_EQ(l, (decoded[0] * ch->envelopeVolumeLeft) >> 8,
                  "dpcm engine: first output is the first decoded sample");
        m4a_engine_destroy(&engine);
    }

    free(wd);
}


/* ------------------------------------------------------------------------
 * Song parser and sequencer (milestone 1 of docs/ACCURACY_PLAN.md)
 * ------------------------------------------------------------------------ */

static M4ASong *parse_song_text(const char *text)
{
    char err[256];
    M4ASong *song = m4a_song_parse(text, strlen(text), err, sizeof(err));
    if (!song)
        fprintf(stderr, "  parse error: %s\n", err);
    return song;
}

static void test_song_parser(void)
{
    printf("Testing .s song parser...\n");

    /* mid2agb's output shapes: .equ header, running-status note forms
     * (a bare note name, note + velocity), expressions, labels, GOTO/PATT
     * targets, the header with the voicegroup word. */
    const char *text =
        "\t.include \"MPlayDef.s\"\n"
        "\n"
        "\t.equ\tt_grp, voicegroup_littleroot\n"
        "\t.equ\tt_pri, 0\n"
        "\t.equ\tt_rev, reverb_set+50\n"
        "\t.equ\tt_mvl, 100\n"
        "\t.equ\tt_key, 0\n"
        "\t.equ\tt_tbs, 1\n"
        "\n"
        "\t.section .rodata\n"
        "\t.global\tt\n"
        "\t.align\t2\n"
        "\n"
        "@**************** Track 1 (Midi-Chn.1) ****************@\n"
        "\n"
        "t_1:\n"
        "\t.byte\tKEYSH , t_key+0\n"
        "@ 000   ----------------------------------------\n"
        "\t.byte\tTEMPO , 120*t_tbs/2\n"
        "\t.byte\t\tVOICE , 48\n"
        "\t.byte\t\tVOL   , 127*t_mvl/mxv\n"
        "\t.byte\t\tPAN   , c_v-16\n"
        "\t.byte\t\tBEND  , c_v+2\n"
        "t_1_B1:\n"
        "\t.byte\t\tN24   , Cn3 , v127\n"
        "\t.byte\tW36\n"
        "\t.byte\t\t        Gn3 \n"
        "\t.byte\tW12\n"
        "\t.byte\t\t        An3 , v064\n"
        "\t.byte\tW24\n"
        "\t.byte\t\tTIE   , Cn4 , v100\n"
        "\t.byte\tW96\n"
        "\t.byte\t\tEOT   \n"
        "\t.byte\tPATT\n"
        "\t .word\tt_1_000\n"
        "\t.byte\tGOTO\n"
        "\t .word\tt_1_B1\n"
        "\t.byte\tFINE\n"
        "\n"
        "t_1_000:\n"
        "\t.byte\tMEMACC, mem_set, 0x00, 117\n"
        "\t.byte\t\tXCMD  , xIECV , 18\n"
        "\t.byte\tPEND\n"
        "\n"
        "@******************************************************@\n"
        "\t.align\t2\n"
        "\n"
        "t:\n"
        "\t.byte\t1\t@ NumTrks\n"
        "\t.byte\t0\t@ NumBlks\n"
        "\t.byte\tt_pri\t@ Priority\n"
        "\t.byte\tt_rev\t@ Reverb.\n"
        "\n"
        "\t.word\tt_grp\n"
        "\n"
        "\t.word\tt_1\n"
        "\n"
        "\t.end\n";

    M4ASong *song = parse_song_text(text);
    ASSERT(song != NULL, "song parses");
    if (!song) return;

    static const uint8_t expected[] = {
        0xBC, 0,                /* KEYSH 0 */
        0xBB, 60,               /* TEMPO 120*1/2 */
        0xBD, 48,               /* VOICE 48 */
        0xBE, 100,              /* VOL 127*100/127 */
        0xBF, 0x40 - 16,        /* PAN */
        0xC0, 0x40 + 2,         /* BEND */
        /* t_1_B1 = 12 */
        0xCF + 24, 60, 127,     /* N24 Cn3 v127 (clock index 24) */
        0x80 + 28,              /* W36 */
        67,                     /* Gn3 (running status) */
        0x80 + 12,              /* W12 */
        69, 64,                 /* An3 v064 */
        0x80 + 24,              /* W24 */
        0xCF, 72, 100,          /* TIE Cn4 v100 */
        0x80 + 48,              /* W96 */
        0xCE,                   /* EOT */
        0xB3, 0, 0, 0, 0,       /* PATT t_1_000 (offset 37) */
        0xB2, 0, 0, 0, 0,       /* GOTO t_1_B1 (offset 12) */
        0xB1,                   /* FINE */
        /* t_1_000 = 37 */
        0xB9, 0, 0x00, 117,     /* MEMACC mem_set 0 117 */
        0xCD, 0x08, 18,         /* XCMD xIECV 18 */
        0xB4,                   /* PEND */
        0, 0, 0,                /* .align 2 padding: 45 -> 48 */
        /* header t = 48 */
        1, 0, 0, 0x80 + 50,
        0, 0, 0, 0,             /* voicegroup (external) */
        0, 0, 0, 0,             /* t_1 = offset 0 */
    };
    ASSERT_EQ(song->size, (uint32_t)sizeof(expected), "song size");
    if (song->size == sizeof(expected)) {
        /* Patch the expected words that are relocations with their offsets. */
        uint8_t exp[sizeof(expected)];
        memcpy(exp, expected, sizeof(exp));
        exp[27] = 37;   /* PATT target */
        exp[32] = 12;   /* GOTO target */
        ASSERT(memcmp(song->data, exp, sizeof(exp)) == 0, "song bytes");
        if (memcmp(song->data, exp, sizeof(exp)) != 0)
            for (uint32_t i = 0; i < song->size; i++)
                if (song->data[i] != exp[i])
                    fprintf(stderr, "  byte %u: got %02x want %02x\n", i, song->data[i], exp[i]);
    }
    ASSERT_EQ(song->headerOffset, 48, "header offset");
    ASSERT_EQ(song->trackCount, 1, "track count");
    ASSERT_EQ(song->reverb, 0x80 + 50, "reverb byte");
    ASSERT_EQ(song->priority, 0, "priority");
    ASSERT_EQ(song->trackOffsets[0], 0, "track 1 offset");
    ASSERT(strcmp(song->voicegroup, "littleroot") == 0, "voicegroup from _grp");
    ASSERT(strcmp(song->name, "t") == 0, "song name from .global");
    ASSERT_EQ(song->relocCount, 4, "relocation count");

    /* Relocation to a ROM image: label words get base + offset, the
     * voicegroup word its own address. */
    uint8_t img[sizeof(expected)];
    m4a_song_relocate(song, 0x08001000, 0x08ABCDEF, img);
    ASSERT_EQ(img[27] | (img[28] << 8) | (img[29] << 16) | ((uint32_t)img[30] << 24), 0x08001000 + 37, "PATT relocated");
    ASSERT_EQ(img[32] | (img[33] << 8) | (img[34] << 16) | ((uint32_t)img[35] << 24), 0x08001000 + 12, "GOTO relocated");
    ASSERT_EQ(img[52] | (img[53] << 8) | (img[54] << 16) | ((uint32_t)img[55] << 24), 0x08ABCDEF, "voicegroup relocated");
    ASSERT_EQ(img[56] | (img[57] << 8) | (img[58] << 16) | ((uint32_t)img[59] << 24), 0x08001000, "track relocated");
    m4a_song_free(song);

    /* Errors are reported, not crashed on. */
    char err[128];
    ASSERT(m4a_song_parse("\t.byte\tBOGUS\n", 13, err, sizeof(err)) == NULL, "undefined symbol in .byte fails");
    ASSERT(strstr(err, "line 1") != NULL, "error names the line");
    ASSERT(m4a_song_parse("x:\n\t.byte 1\n", 12, err, sizeof(err)) == NULL, "missing .global fails");
}

/* A one-voice square-wave voicegroup for sequencer tests (CGB channel 1,
 * so there is no sample data to build). */
static void seq_test_voices(ToneData *voices)
{
    memset(voices, 0, 128 * sizeof(ToneData));
    voices[0].type = VOICE_SQUARE_1;
    voices[0].key = 60;
    voices[0].wavePointer = (uint32_t *)(uintptr_t)2; /* 50% duty */
    voices[0].attack = 0;
    voices[0].decay = 0;
    voices[0].sustain = 15;
    voices[0].release = 0;
}

static const char *seq_song_header =
    "\t.equ\tt_grp, voicegroup_test\n"
    "\t.equ\tt_pri, 0\n"
    "\t.equ\tt_rev, 0\n"
    "\t.equ\tt_mvl, 127\n"
    "\t.equ\tt_key, 0\n"
    "\t.equ\tt_tbs, 1\n"
    "\t.section .rodata\n"
    "\t.global\tt\n"
    "\t.align\t2\n"
    "t_1:\n";

static const char *seq_song_footer =
    "\t.align\t2\n"
    "t:\n"
    "\t.byte\t1, 0, t_pri, t_rev\n"
    "\t.word\tt_grp\n"
    "\t.word\tt_1\n";

static M4ASong *seq_song(const char *body)
{
    char buf[4096];
    snprintf(buf, sizeof(buf), "%s%s%s", seq_song_header, body, seq_song_footer);
    return parse_song_text(buf);
}

static void test_sequencer_tempo(void)
{
    printf("Testing sequencer tempo accumulator...\n");

    /* MPlayMain: tempoC += tempoI per frame; one tick per 150.  TEMPO 60
     * (byte 60 -> tempoD 120 -> tempoI 120) gives 4 ticks in 5 frames; the
     * clock must follow the GBA's integer arithmetic exactly. */
    ToneData voices[128];
    seq_test_voices(voices);
    M4ASong *song = seq_song(
        "\t.byte\tTEMPO , 120*t_tbs/2\n"
        "\t.byte\tW96\n\t.byte\tW96\n\t.byte\tW96\n\t.byte\tW96\n\t.byte\tW96\n"
        "\t.byte\tFINE\n");
    ASSERT(song != NULL, "tempo song parses");
    if (!song) return;

    M4AEngine engine;
    m4a_engine_init(&engine, 48000.0f);
    m4a_engine_set_voicegroup(&engine, voices);
    M4ASequencer seq;
    m4a_sequencer_init(&seq, &engine);
    m4a_sequencer_start(&seq, song);
    ASSERT_EQ(seq.tempoI, 150, "tempo defaults to 150 (one tick per frame)");

    uint16_t tempoC = 0;
    uint32_t clock = 0;
    for (int frame = 1; frame <= 400; frame++) {
        m4a_sequencer_frame(&seq);
        /* Reference: the first tick runs TEMPO 60 (tempoI 120) before any
         * further accumulation. */
        uint16_t tempoI = (frame == 1) ? 150 : 120;
        tempoC += tempoI;
        while (tempoC >= 150) {
            tempoC -= 150;
            clock++;
        }
        if (seq.clock != clock) {
            ASSERT_EQ(seq.clock, clock, "clock matches the per-frame accumulator");
            break;
        }
    }
    ASSERT_EQ(seq.tempoD, 120, "TEMPO byte doubled");
    ASSERT_EQ(seq.tempoI, 120, "tempoI = tempoD * tempoU >> 8");
    ASSERT_EQ(seq.clock, clock, "clock after 400 frames");
    ASSERT(!m4a_sequencer_finished(&seq), "still waiting after 400 frames");
    /* 5 x W96 = 480 ticks, then FINE: the player pauses on the tick after
     * the last track ends.  480 ticks + 1 FINE tick + 1 empty tick = 482
     * ticks; at 120/150 ticks per frame that is frame ceil(482*150/120)
     * (first frame counts 150). */
    int finishedFrame = 0;
    for (int frame = 401; frame <= 1000 && !finishedFrame; frame++) {
        m4a_sequencer_frame(&seq);
        if (m4a_sequencer_finished(&seq))
            finishedFrame = frame;
    }
    ASSERT_EQ(seq.clock, 482, "ticks to finish");
    /* tempoC after frame 1: 0 (150 consumed).  Ticks 2..482 need 481 more
     * ticks = 72150 units at 120 per frame = 601.25 frames -> frame 1 + 602. */
    ASSERT_EQ(finishedFrame, 603, "frame on which the song finished");

    m4a_sequencer_detach(&seq);
    m4a_engine_destroy(&engine);
    m4a_song_free(song);
}

static void test_sequencer_notes(void)
{
    printf("Testing sequencer note timing and gate times...\n");

    ToneData voices[128];
    seq_test_voices(voices);
    /* TEMPO 75 -> tempoI 150: one tick per frame. */
    M4ASong *song = seq_song(
        "\t.byte\tTEMPO , 150*t_tbs/2\n"
        "\t.byte\t\tVOICE , 0\n"
        "\t.byte\t\tVOL   , 127*t_mvl/mxv\n"
        "\t.byte\t\tN24   , Cn3 , v127\n"
        "\t.byte\tW24\n"
        "\t.byte\t\t        En3 \n"       /* running status: N24 En3 v127 */
        "\t.byte\tW12\n"
        "\t.byte\t\tTIE   , Gn3 , v100\n"
        "\t.byte\tW24\n"
        "\t.byte\t\tEOT   , Gn3\n"
        "\t.byte\tW12\n"
        "\t.byte\tFINE\n");
    ASSERT(song != NULL, "note song parses");
    if (!song) return;

    M4AEngine engine;
    m4a_engine_init(&engine, 48000.0f);
    m4a_engine_set_voicegroup(&engine, voices);
    M4ASequencer seq;
    m4a_sequencer_init(&seq, &engine);
    m4a_sequencer_start(&seq, song);
    M4ACGBChannel *sq = &engine.cgbChannels[0];

    /* Frame 1 (tick 1): track init, commands up to W24, the note starts. */
    m4a_sequencer_frame(&seq);
    ASSERT_EQ(seq.clock, 1, "one tick on the first frame");
    ASSERT(sq->status & CHN_ON, "note started on tick 1");
    ASSERT_EQ(sq->midiKey, 60, "note key");
    ASSERT_EQ(sq->velocity, 127, "note velocity");
    ASSERT_EQ(sq->gateTime, 24, "gate time from N24");
    ASSERT_EQ(engine.tracks[0].volume, 127, "VOL applied to the engine track");
    ASSERT_EQ(engine.tracks[0].bendRange, 2, "track init: bend range 2");
    ASSERT_EQ(engine.tracks[0].lfoSpeed, 22, "track init: LFO speed 22");
    ASSERT_EQ(engine.tracks[0].volX, 64, "track init: volX 64");

    /* The gate counts down one per tick; the channel stops when it hits 0,
     * on the same tick the next note starts (W24 later). */
    for (int i = 0; i < 23; i++)
        m4a_sequencer_frame(&seq);
    ASSERT_EQ(sq->gateTime, 1, "gate about to expire");
    ASSERT(!(sq->status & CHN_STOP), "not stopped yet");
    m4a_sequencer_frame(&seq);  /* tick 25: gate -> 0 (STOP), then N24 En3 retriggers the channel */
    ASSERT_EQ(sq->midiKey, 64, "running-status note En3 started on tick 25");
    ASSERT_EQ(sq->velocity, 127, "running-status note keeps the velocity");
    ASSERT_EQ(sq->gateTime, 24, "running-status note keeps the N24 length");
    ASSERT_EQ(seq.noteCount, 2, "two notes so far");

    /* W12 later a TIE starts: gate 0 (never auto-stops) until EOT. */
    for (int i = 0; i < 12; i++)
        m4a_sequencer_frame(&seq);
    ASSERT_EQ(sq->midiKey, 67, "TIE Gn3 started");
    ASSERT_EQ(sq->gateTime, 0, "TIE has no gate");
    for (int i = 0; i < 23; i++)
        m4a_sequencer_frame(&seq);
    ASSERT(!(sq->status & CHN_STOP), "tied note still held");
    m4a_sequencer_frame(&seq);  /* EOT */
    ASSERT(sq->status & CHN_STOP, "EOT releases the tied note");
    ASSERT(!m4a_sequencer_finished(&seq), "not finished before FINE");
    for (int i = 0; i < 12; i++)
        m4a_sequencer_frame(&seq);  /* W12, FINE */
    m4a_sequencer_frame(&seq);      /* empty tick -> pause */
    ASSERT(m4a_sequencer_finished(&seq), "finished after FINE");
    ASSERT_EQ(seq.noteCount, 3, "three notes in total");

    m4a_sequencer_detach(&seq);
    m4a_engine_destroy(&engine);
    m4a_song_free(song);
}

static void test_sequencer_deferred_volume(void)
{
    printf("Testing sequencer deferred volume (MPlayMain VOLCHG pass)...\n");

    /* Two tracks so two notes can sound at once (square 1 and square 2). */
    ToneData voices[128];
    seq_test_voices(voices);
    voices[1] = voices[0];
    voices[1].type = VOICE_SQUARE_2;
    const char *text =
        "\t.equ\tt_grp, voicegroup_test\n"
        "\t.section .rodata\n"
        "\t.global\tt\n"
        "t_1:\n"
        "\t.byte\tTEMPO , 75\n"
        "\t.byte\t\tVOICE , 0\n"
        "\t.byte\t\tVOL   , 100\n"
        "\t.byte\t\tTIE   , Cn3 , v127\n"
        "\t.byte\tW24\n"
        "\t.byte\t\tVOL   , 50\n"       /* same tick as a note on track 2: see below */
        "\t.byte\tW24\n"
        "\t.byte\t\tPAN   , c_v+0\n"    /* a later change re-applies everything */
        "\t.byte\tW24\n"
        "\t.byte\tFINE\n"
        "t_2:\n"
        "\t.byte\t\tVOICE , 1\n"
        "\t.byte\t\tVOL   , 100\n"
        "\t.byte\t\tTIE   , Cn3 , v127\n"
        "\t.byte\tW24\n"
        "\t.byte\t\tVOL   , 50\n"
        "\t.byte\t\tTIE   , En3 , v127\n"  /* VOL then a note in the same tick */
        "\t.byte\tW48\n"
        "\t.byte\tFINE\n"
        "\t.align\t2\n"
        "t:\n"
        "\t.byte\t2, 0, 0, 0\n"
        "\t.word\tt_grp\n"
        "\t.word\tt_1\n"
        "\t.word\tt_2\n";
    M4ASong *song = parse_song_text(text);
    ASSERT(song != NULL, "deferred-volume song parses");
    if (!song) return;

    M4AEngine engine;
    m4a_engine_init(&engine, 48000.0f);
    m4a_engine_set_voicegroup(&engine, voices);
    M4ASequencer seq;
    m4a_sequencer_init(&seq, &engine);
    m4a_sequencer_start(&seq, song);
    M4ACGBChannel *sq1 = &engine.cgbChannels[0];
    M4ACGBChannel *sq2 = &engine.cgbChannels[1];

    m4a_sequencer_frame(&seq);
    ASSERT(sq1->status & CHN_ON, "track 1 note sounding");
    ASSERT(sq2->status & CHN_ON, "track 2 note sounding");
    uint8_t vol100 = sq1->leftVolume;
    ASSERT(vol100 > 0, "VOL 100 note has volume");
    ASSERT_EQ(sq2->leftVolume, vol100, "both tracks start at VOL 100");

    for (int i = 0; i < 24; i++)
        m4a_sequencer_frame(&seq);
    /* Track 1: VOL 50 alone -> applied at the end of the frame to the
     * sounding channel.  Track 2: VOL 50 then a note on the same tick -> the
     * new note has VOL 50 but the quirk is that nothing else would have been
     * re-applied; the square-2 channel IS the new note here, so check the
     * track-1 channel got its change and the track-2 note has the new VOL. */
    ASSERT(sq1->leftVolume < vol100, "track 1: VOL 50 applied at frame end");
    uint8_t vol50 = sq1->leftVolume;
    ASSERT_EQ(sq2->leftVolume, vol50, "track 2: new note keyed at VOL 50");
    ASSERT_EQ(seq.tracks[0].flags & 0x0F, 0, "track 1 flags cleared after the pass");
    ASSERT_EQ(seq.tracks[1].flags & 0x0F, 0, "track 2 flags cleared by the note");

    m4a_sequencer_detach(&seq);
    m4a_engine_destroy(&engine);
    m4a_song_free(song);
}

static void test_sequencer_flow(void)
{
    printf("Testing sequencer PATT/PEND/GOTO/REPT flow...\n");

    ToneData voices[128];
    seq_test_voices(voices);
    M4ASong *song = seq_song(
        "\t.byte\tTEMPO , 75\n"
        "\t.byte\t\tVOICE , 0\n"
        "\t.byte\t\tVOL   , 100\n"
        "t_1_B1:\n"
        "\t.byte\tPATT\n"
        "\t .word\tt_1_000\n"
        "\t.byte\tPATT\n"
        "\t .word\tt_1_000\n"
        "\t.byte\tREPT , 3\n"           /* repeat the following GOTO target 3 times */
        "\t .word\tt_1_R\n"
        "\t.byte\tGOTO\n"
        "\t .word\tt_1_B1\n"
        "t_1_000:\n"
        "\t.byte\t\tN12   , Cn3 , v127\n"
        "\t.byte\tW12\n"
        "\t.byte\tPEND\n"
        "t_1_R:\n"
        "\t.byte\t\tN06   , En3 , v127\n"
        "\t.byte\tW06\n"
        "\t.byte\tFINE\n");
    ASSERT(song != NULL, "flow song parses");
    if (!song) return;

    M4AEngine engine;
    m4a_engine_init(&engine, 48000.0f);
    m4a_engine_set_voicegroup(&engine, voices);
    M4ASequencer seq;
    m4a_sequencer_init(&seq, &engine);
    m4a_sequencer_start(&seq, song);

    /* Tick 1: two patterns are called (first plays, second queued): the
     * first note.  After 2 x 12 ticks both patterns are done; REPT 3 jumps
     * to t_1_R and the En3 plays, W06, FINE.  REPT's counter means the
     * jump happens while repN < 3, so t_1_R runs once and FINE ends it
     * (REPT jumps only when the target is reached again, which FINE
     * prevents): expect 2 Cn3 + 1 En3 notes. */
    for (int i = 0; i < 40 && !m4a_sequencer_finished(&seq); i++)
        m4a_sequencer_frame(&seq);
    ASSERT(m4a_sequencer_finished(&seq), "flow song finishes");
    ASSERT_EQ(seq.noteCount, 3, "two pattern notes plus one after REPT");
    ASSERT_EQ(seq.loopPasses, 0, "REPT is not counted as a GOTO loop pass");
    m4a_song_free(song);

    /* A GOTO loop: the loop pass counter advances once per backward GOTO. */
    song = seq_song(
        "\t.byte\tTEMPO , 75\n"
        "\t.byte\t\tVOICE , 0\n"
        "\t.byte\t\tVOL   , 100\n"
        "t_1_B1:\n"
        "\t.byte\t\tN06   , Cn3 , v127\n"
        "\t.byte\tW12\n"
        "\t.byte\tGOTO\n"
        "\t .word\tt_1_B1\n"
        "\t.byte\tFINE\n");
    ASSERT(song != NULL, "loop song parses");
    if (song) {
        m4a_sequencer_start(&seq, song);
        for (int i = 0; i < 12 * 5 + 1; i++)
            m4a_sequencer_frame(&seq);
        ASSERT_EQ(seq.loopPasses, 5, "five GOTO passes in 61 ticks");
        ASSERT_EQ(seq.noteCount, 6, "six notes in 61 ticks");
        ASSERT(!m4a_sequencer_finished(&seq), "a looping song never finishes");
        m4a_song_free(song);
    }

    m4a_sequencer_detach(&seq);
    m4a_engine_destroy(&engine);
}

static void test_sequencer_lfo_and_engine_tick(void)
{
    printf("Testing sequencer LFO on the frame hook...\n");

    /* Through the engine's own tick: the hook runs the sequencer, and the
     * engine's internal tempo/LFO loop stays off while attached. */
    ToneData voices[128];
    seq_test_voices(voices);
    M4ASong *song = seq_song(
        "\t.byte\tTEMPO , 75\n"
        "\t.byte\t\tVOICE , 0\n"
        "\t.byte\t\tVOL   , 100\n"
        "\t.byte\t\tLFOS  , 1\n"
        "\t.byte\t\tMOD   , 64\n"
        "\t.byte\t\tTIE   , Cn3 , v127\n"
        "\t.byte\tW96\n"
        "\t.byte\tW96\n"
        "\t.byte\tEOT\n"
        "\t.byte\tFINE\n");
    ASSERT(song != NULL, "lfo song parses");
    if (!song) return;

    M4AEngine engine;
    m4a_engine_init(&engine, 48000.0f);
    m4a_engine_set_voicegroup(&engine, voices);
    M4ASequencer seq;
    m4a_sequencer_init(&seq, &engine);
    ASSERT(engine.externalSequencer, "engine knows a sequencer is attached");
    m4a_sequencer_start(&seq, song);

    for (int i = 0; i < 100; i++)
        m4a_engine_tick(&engine);
    ASSERT_EQ(seq.clock, 100, "engine ticks drive the sequencer");
    /* lfoSpeed 1: lfoSpeedC counts the LFO steps.  The LFO steps once per
     * tick after the wait is consumed: 100 ticks, the first one starting
     * the note (its W96 is consumed on that tick too, so it steps). */
    ASSERT_EQ(engine.tracks[0].lfoSpeedC, 100, "one LFO step per song tick");
    ASSERT(engine.cgbChannels[0].status & CHN_ON, "tied note still sounding");
    ASSERT_EQ(engine.cgbChannels[0].gateTime, 0, "engine tick did not touch the TIE gate");

    m4a_sequencer_detach(&seq);
    ASSERT(!engine.externalSequencer, "detach restores the engine's own tick");
    m4a_engine_destroy(&engine);
    m4a_song_free(song);
}


/* A looped sample so PCM voices can be used in sequencer tests. */
static WaveData *seq_test_wave(void)
{
    int n = 64;
    WaveData *wd = calloc(1, sizeof(WaveData) + n + 1);
    wd->type = 0;
    wd->status = 0x4000; /* loop */
    wd->freq = 0x01000000;
    wd->loopStart = 0;
    wd->size = n;
    wd->data = (int8_t *)((uint8_t *)wd + sizeof(WaveData));
    for (int i = 0; i < n; i++)
        wd->data[i] = (int8_t)(100.0 * sin(2.0 * 3.14159265 * i / n));
    return wd;
}

static void test_sequencer_lfo_delay_note_on(void)
{
    printf("Testing sequencer: LFODL note start does not touch older notes...\n");

    /* ply_note with lfoDelay != 0: clear_modM flags the track, TrkVolPitSet
     * recomputes keyM/pitM for the NEW note, then flags &= 0xF0.  Older
     * channels keep their LFO-offset pitch and a VOL pending on the same tick
     * is not applied to them until the next change. */
    WaveData *wd = seq_test_wave();
    ToneData voices[128];
    memset(voices, 0, sizeof(voices));
    voices[0].type = VOICE_DIRECTSOUND;
    voices[0].key = 60;
    voices[0].wav = wd;
    voices[0].attack = 0xFF;
    voices[0].sustain = 0xFF;
    M4ASong *song = seq_song(
        "\t.byte\tTEMPO , 75\n"
        "\t.byte\t\tVOICE , 0\n"
        "\t.byte\t\tVOL   , 100\n"
        "\t.byte\t\tLFOS  , 40\n"
        "\t.byte\t\tLFODL , 8\n"
        "\t.byte\t\tMOD   , 20\n"
        "\t.byte\t\tTIE   , Cn3 , v127\n"
        "\t.byte\tW48\n"
        "\t.byte\t\tVOL   , 40\n"
        "\t.byte\t\tTIE   , Gn3 , v127\n"
        "\t.byte\tW48\n"
        "\t.byte\t\tPAN   , c_v+0\n"      /* a later change applies everything */
        "\t.byte\tW12\n"
        "\t.byte\t\tEOT   , Cn3\n"
        "\t.byte\t\tEOT   , Gn3\n"
        "\t.byte\tFINE\n");
    ASSERT(song != NULL, "lfodl song parses");
    if (!song) { free(wd); return; }

    M4AEngine engine;
    m4a_engine_init(&engine, 48000.0f);
    m4a_engine_set_voicegroup(&engine, voices);
    M4ASequencer seq;
    m4a_sequencer_init(&seq, &engine);
    m4a_sequencer_start(&seq, song);

    M4APCMChannel *first = NULL;
    for (int i = 0; i < 48; i++)
        m4a_sequencer_frame(&seq);
    for (int i = 0; i < MAX_PCM_CHANNELS; i++)
        if ((engine.pcmChannels[i].status & CHN_ON) && engine.pcmChannels[i].midiKey == 60)
            first = &engine.pcmChannels[i];
    ASSERT(first != NULL, "first note sounding");
    if (!first) goto done;
    ASSERT(engine.tracks[0].modM != 0, "LFO is offsetting the pitch after 48 ticks");
    uint32_t freqBefore = first->frequency;
    uint8_t volBefore = first->leftVolume;

    m4a_sequencer_frame(&seq);  /* tick 49: VOL 40 + TIE Gn3 (LFO restart) */
    ASSERT_EQ(engine.tracks[0].modM, 0, "LFO output recentred for the new note");
    /* Reloaded to 8 at note start, then the same tick's LFO step (after the
     * W48 is consumed) counts it down once, as on the GBA. */
    ASSERT_EQ(engine.tracks[0].lfoDelayC, 7, "LFO delay reloaded and stepped once");
    ASSERT_EQ(first->frequency, freqBefore, "older note keeps its LFO-offset pitch");
    ASSERT_EQ(first->leftVolume, volBefore, "older note keeps the old volume (flags dropped by ply_note)");
    M4APCMChannel *second = NULL;
    for (int i = 0; i < MAX_PCM_CHANNELS; i++)
        if ((engine.pcmChannels[i].status & CHN_ON) && engine.pcmChannels[i].midiKey == 67)
            second = &engine.pcmChannels[i];
    ASSERT(second != NULL, "second note sounding");
    if (second)
        ASSERT(second->leftVolume < volBefore, "new note keyed at VOL 40");

    for (int i = 0; i < 48; i++)
        m4a_sequencer_frame(&seq);  /* PAN on tick 97 flags VOLCHG: now everything updates */
    ASSERT(first->leftVolume < volBefore, "older note picks up VOL 40 at the next volume change");

done:
    m4a_sequencer_detach(&seq);
    m4a_engine_destroy(&engine);
    m4a_song_free(song);
    free(wd);
}

static void test_fixed_rate_voice_ignores_pitch(void)
{
    printf("Testing fixed-rate (no-resample) voices ignore pitch changes...\n");

    WaveData *wd = seq_test_wave();
    ToneData voices[128];
    memset(voices, 0, sizeof(voices));
    voices[0].type = VOICE_DIRECTSOUND_NO_RESAMPLE;
    voices[0].key = 60;
    voices[0].wav = wd;
    voices[0].attack = 0xFF;
    voices[0].sustain = 0xFF;

    M4AEngine engine;
    m4a_engine_init(&engine, 48000.0f);
    m4a_engine_set_voicegroup(&engine, voices);
    m4a_engine_program_change(&engine, 0, 0);
    m4a_engine_cc(&engine, 0, 7, 127);
    int h = m4a_engine_note_on(&engine, 0, 60, 127);
    ASSERT(h >= 0 && h < TOTAL_PCM_CHANNELS, "fixed-rate note started on a PCM channel");
    if (h < 0) { m4a_engine_destroy(&engine); free(wd); return; }
    uint32_t f0 = engine.pcmChannels[h].frequency;
    m4a_engine_pitch_bend(&engine, 0, 4000);
    ASSERT_EQ(engine.pcmChannels[h].frequency, f0, "pitch bend leaves a fixed-rate voice alone");
    m4a_engine_cc(&engine, 0, 0x15, 40);
    m4a_engine_cc(&engine, 0, 0x1, 64);
    for (int i = 0; i < 20; i++)
        m4a_engine_tick(&engine);
    ASSERT_EQ(engine.pcmChannels[h].frequency, f0, "vibrato leaves a fixed-rate voice alone");
    m4a_engine_destroy(&engine);
    free(wd);
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
    test_cgb_fixed_freq();
    test_sweep();
    test_lfo_tempo_scaling();
    test_lfos_keeps_phase();
    test_golden_sun_synth();
    test_golden_sun_synth_waveforms();
    test_cgb_hw_envelope();
    test_cgb_square_phase_continuity();
    test_cgb_wave_phase_continuity();
    test_cgb_noise_band_limiting();
    test_pcm_reverse();
    test_pcm_reverse_legato_inherit();
    test_pcm_dpcm();
    test_song_parser();
    test_sequencer_tempo();
    test_sequencer_notes();
    test_sequencer_deferred_volume();
    test_sequencer_flow();
    test_sequencer_lfo_and_engine_tick();
    test_sequencer_lfo_delay_note_on();
    test_fixed_rate_voice_ignores_pitch();

    printf("\n=== Results: %d/%d tests passed ===\n", tests_passed, tests_run);
    return (tests_passed == tests_run) ? 0 : 1;
}
