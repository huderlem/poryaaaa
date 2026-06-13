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
    test_portamento();
    test_portamento_prev_key_tracking();
    test_pwm();

    printf("\n=== Results: %d/%d tests passed ===\n", tests_passed, tests_run);
    return (tests_passed == tests_run) ? 0 : 1;
}
