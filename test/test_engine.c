#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <math.h>
#include "m4a_engine.h"
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

    printf("\n=== Results: %d/%d tests passed ===\n", tests_passed, tests_run);
    return (tests_passed == tests_run) ? 0 : 1;
}
