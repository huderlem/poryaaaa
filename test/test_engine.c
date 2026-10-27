#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <math.h>
#include "m4a_engine.h"
#include "m4a_channel.h"
#include "m4a_mixer.h"
#include "m4a_hq_mixer.h"
#include "m4a_tables.h"
#include "m4a_song.h"
#include "m4a_sequencer.h"
#include "gba_tempo_grid.h"
#include "m4a_cry.h"
#include "voicegroup_loader.h"
#ifndef _WIN32
#include <sys/stat.h>
#include <unistd.h>
#endif

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

/* Most tests count output samples from a note-on; boot the engine first so
 * the ~36 ms of hardware boot (m4a_engine_boot) is out of the way and the
 * next SoundMain frame is exactly one VBlank away. */
static void engine_init_booted(M4AEngine *e, float rate)
{
    m4a_engine_init(e, rate);
    m4a_engine_boot(e, false);
}

/* Render until `frames` SoundMain frames (engine ticks) have run. */
static void advance_frames(M4AEngine *e, int frames, float *outL, float *outR)
{
    int64_t target = e->nextSoundMain + (int64_t)(frames - 1) * GBA_CYCLES_PER_FRAME;
    while (e->gba.now < target)
        m4a_engine_process(e, outL, outR, 64);
}

/* Mix one SoundMainRAM frame of a hand-built channel through the real
 * mixer into a zeroed ring-shaped buffer (A = right at 0, B = left at
 * GBA_PCM_DMA_BUF_SIZE) and hand back the right side's bytes.  The engine
 * only supplies pcmSamplesPerVBlank / pcmDivFreq and the DPCM decoding
 * buffer; tests set pcmDivFreq to 1 so `frequency` is the 23-bit step. */
static void mix_frame(M4AEngine *e, M4APCMChannel *ch, int8_t *frame)
{
    memset(frame, 0, GBA_PCM_DMA_BUF_SIZE * 2);
    m4a_mixer_mix_channel(e, ch, frame);
}

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
    engine_init_booted(&engine, 44100.0f);

    ASSERT_NEAR(engine.sampleRate, 44100.0f, 0.1f, "sample rate");
    ASSERT_EQ(engine.pcmSamplesPerVBlank, 224, "PCM samples per VBlank at 13379 Hz");
    ASSERT_EQ(engine.masterVolume, 12, "master volume");
    ASSERT_EQ(engine.songMasterVolume, MAX_SONG_VOLUME, "song master volume");
    ASSERT_EQ(engine.maxPcmChannels, 5, "max pcm channels");

    /* The sound-mode frequency table (SampleFreqSet): twelve rates, the
     * game's 13379 Hz at index 4; any Hz maps to the nearest one, 0 and
     * out-of-range indices to the default. */
    static const int hz[12] = { 5734, 7884, 10512, 13379, 15768, 18157,
                                21024, 26758, 31536, 36314, 40137, 42048 };
    int tableOk = 1;
    for (int m = 1; m <= 12; m++)
        if (m4a_pcm_freq_mode_hz(m) != hz[m - 1] || m4a_pcm_freq_mode_from_hz(hz[m - 1]) != m)
            tableOk = 0;
    ASSERT(tableOk, "pcm freq modes: SampleFreqSet's pcmFreq for every table entry");
    ASSERT_EQ(m4a_pcm_freq_mode_from_hz(0), M4A_PCM_FREQ_MODE_DEFAULT, "pcm freq modes: 0 Hz is the default");
    ASSERT_EQ(m4a_pcm_freq_mode_from_hz(44100), 12, "pcm freq modes: 44100 snaps to 42048");
    ASSERT_EQ(m4a_pcm_freq_mode_from_hz(14000), 4, "pcm freq modes: 14000 snaps to 13379");
    m4a_engine_set_pcm_freq_mode(&engine, 12);
    ASSERT_EQ(engine.pcmSamplesPerVBlank, 704, "pcm freq modes: 42048 Hz mixes 704 samples per VBlank");
    ASSERT_EQ(engine.pcmDmaPeriod, 2, "pcm freq modes: DMA period 1584 / 704");
    ASSERT_EQ(engine.pcmFreq, 42048, "pcm freq modes: pcmFreq");
    ASSERT_EQ(engine.pcmDivFreq, (16777216 / 42048 + 1) >> 1, "pcm freq modes: divFreq");
    ASSERT_EQ(engine.gba.timerPeriodDen, 704, "pcm freq modes: timer reprogrammed");
    m4a_engine_set_pcm_freq_mode(&engine, 99);
    ASSERT_EQ(engine.pcmSamplesPerVBlank, 224, "pcm freq modes: out of range selects the default");

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
    engine_init_booted(&engine, 44100.0f);

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

    /* Generate some audio: the note starts at the next frame and reaches the
     * output through the DMA buffer, so look across a few frames. */
    float outL[1024], outR[1024];
    float maxVal = 0;
    for (int k = 0; k < 4; k++) {
        m4a_engine_process(&engine, outL, outR, 1024);
        for (int i = 0; i < 1024; i++) {
            if (fabs(outL[i]) > maxVal) maxVal = fabs(outL[i]);
            if (fabs(outR[i]) > maxVal) maxVal = fabs(outR[i]);
        }
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
    engine_init_booted(&engine, 44100.0f);
    m4a_engine_set_voicegroup(&engine, voices);
    m4a_engine_program_change(&engine, 0, 0);
    engine.tracks[0].priority = 5;
    m4a_engine_note_on(&engine, 0, 60, 100);
    ASSERT(engine.pcmChannels[0].status & CHN_ON, "free channel: ch0 allocated");
    ASSERT_EQ(engine.pcmChannels[0].trackIndex, 0, "free channel: ch0 trackIndex");
    ASSERT_EQ(engine.pcmChannels[0].priority, 5, "free channel: ch0 priority");
    m4a_engine_destroy(&engine);

    /* ---- Test 2: Higher-priority note steals lower-priority active channel ---- */
    engine_init_booted(&engine, 44100.0f);
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
    engine_init_booted(&engine, 44100.0f);
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
    engine_init_booted(&engine, 44100.0f);
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
    engine_init_booted(&engine, 44100.0f);
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
    engine_init_booted(&engine, 44100.0f);
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
    engine_init_booted(&engine, 44100.0f);
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
    engine_init_booted(&engine, 44100.0f);
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
    engine_init_booted(&engine, 44100.0f);
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
    engine_init_booted(&engine, 44100.0f);
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
    engine_init_booted(&engine, 44100.0f);
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
    {
        float maxVal = 0;
        /* The note starts at the next frame and reaches the output after
         * the DMA buffer latency: look across a few frames. */
        for (int k = 0; k < 4; k++) {
            m4a_engine_process(&engine, outL, outR, 1024);
            for (int i = 0; i < 1024; i++) {
                if (fabs(outL[i]) > maxVal) maxVal = fabs(outL[i]);
                if (fabs(outR[i]) > maxVal) maxVal = fabs(outR[i]);
            }
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
    engine_init_booted(&engine, 44100.0f);
    m4a_engine_set_voicegroup(&engine, voices);
    engine.maxPcmChannels = 1;
    m4a_engine_set_poly_debug_invert(&engine, true);
    m4a_engine_program_change(&engine, 0, 0);
    m4a_engine_cc(&engine, 0, 7, 127);
    engine.auditionNote = true;
    m4a_engine_note_on(&engine, 0, 60, 100);
    ASSERT(engine.pcmChannels[0].audition, "poly invert: audition flag copied to channel");
    {
        float maxVal = 0;
        for (int k = 0; k < 4; k++) {
            m4a_engine_process(&engine, outL, outR, 1024);
            for (int i = 0; i < 1024; i++) {
                if (fabs(outL[i]) > maxVal) maxVal = fabs(outL[i]);
                if (fabs(outR[i]) > maxVal) maxVal = fabs(outR[i]);
            }
        }
        ASSERT(maxVal > 0.001f, "poly invert: audition note is audible");
    }
    engine.auditionNote = false;
    m4a_engine_note_on(&engine, 0, 62, 100); /* sequenced reuse of the channel */
    ASSERT(!engine.pcmChannels[0].audition,
           "poly invert: sequenced reuse clears the audition flag");
    m4a_engine_destroy(&engine);

    /* ---- Invert mode: a stolen note's remainder survives on a shadow channel ---- */
    engine_init_booted(&engine, 44100.0f);
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
    engine_init_booted(&engine, 44100.0f);
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
    engine_init_booted(&engine, 44100.0f);
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
        engine_init_booted(&ref, 44100.0f);
        m4a_engine_set_voicegroup(&ref, voices);
        m4a_engine_program_change(&ref, 0, 0);
        m4a_engine_note_on(&ref, 0, 67, 100);
        M4APCMChannel *refCh = find_pcm_channel(&ref, 0);
        ASSERT_EQ(freqEnd, refCh->frequency, "portamento: final pitch equals target note pitch");
        m4a_engine_destroy(&ref);
    }
    m4a_engine_destroy(&engine);

    /* ---- PCM: notes with a gap do not inherit channel state ---- */
    engine_init_booted(&engine, 44100.0f);
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
    ASSERT(gapCh->status & CHN_START, "portamento gap: new note triggers its own envelope");
    /* But the pitch still glides from the previous note's key */
    ASSERT(engine.tracks[0].portamentoGliding, "portamento gap: pitch still glides");
    m4a_engine_destroy(&engine);

    /* ---- CC 5 value 0 disables portamento ---- */
    engine_init_booted(&engine, 44100.0f);
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
        engine_init_booted(&ref, 44100.0f);
        m4a_engine_set_voicegroup(&ref, voices);
        m4a_engine_program_change(&ref, 0, 0);
        m4a_engine_note_on(&ref, 0, 67, 100);
        ASSERT_EQ(offCh->frequency, find_pcm_channel(&ref, 0)->frequency,
                  "portamento off: note plays at its own pitch");
        m4a_engine_destroy(&ref);
    }
    m4a_engine_destroy(&engine);

    /* ---- CGB: legato note inherits the shared channel slot ---- */
    engine_init_booted(&engine, 44100.0f);
    engine.portamentoEnabled = true;
    m4a_engine_set_voicegroup(&engine, voices);
    m4a_engine_program_change(&engine, 0, 1);  /* square 1 */
    m4a_engine_cc(&engine, 0, 0x5, 2);
    m4a_engine_note_on(&engine, 0, 60, 100);
    M4ACGBChannel *cgb = &engine.cgbChannels[0];
    ASSERT(cgb->status & CHN_ON, "cgb portamento: first note started");
    uint32_t cgbFreqStart = cgb->frequency;
    advance_frames(&engine, 1, outL, outR);   /* CgbSound starts the note */
    uint8_t cgbEnvBefore = cgb->envelopeVolume;
    ASSERT(cgbEnvBefore != 0, "cgb portamento: first note envelope is audible");
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
        engine_init_booted(&ref, 44100.0f);
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
    engine_init_booted(&engine, 44100.0f);
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
    engine_init_booted(&engine, 44100.0f);
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
    engine_init_booted(&engine, 44100.0f);
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
    engine_init_booted(&engine, 44100.0f);
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

    engine_init_booted(&engine, 44100.0f);
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

    /* speed=2: the start frame (tick 1) only starts the effect (the SF_START
     * branch, no countdown); then the duty advances every second tick, before
     * that tick's CgbSound, so step k is written on tick 1 + 2k. */
    advance_frames(&engine, 1, outL, outR);  /* tick 1: the note's start frame */
    ASSERT_EQ(sq->dutyCycle, 0, "pwm: the start frame keeps step 0");
    advance_frames(&engine, 1, outL, outR);  /* tick 2 */
    ASSERT_EQ(sq->dutyCycle, 0, "pwm: duty holds during speed countdown");
    advance_frames(&engine, 1, outL, outR);  /* tick 3 */
    ASSERT_EQ(sq->dutyCycle, 1, "pwm: duty advances to step 1");
    advance_frames(&engine, 1, outL, outR);  /* tick 4 */
    advance_frames(&engine, 1, outL, outR);  /* tick 5 */
    ASSERT_EQ(sq->dutyCycle, 2, "pwm: duty advances to step 2");
    advance_frames(&engine, 1, outL, outR);  /* tick 6 */
    advance_frames(&engine, 1, outL, outR);  /* tick 7 */
    ASSERT_EQ(sq->dutyCycle, 0, "pwm: pattern wraps back to step 0");

    /* Disabling the effect (PWMS 0) restores the voice's default duty cycle. */
    m4a_engine_cc(&engine, 0, 0x19, 0);
    ASSERT_EQ(engine.tracks[0].pwmSpeed, 0, "pwm: PWMS 0 disables");
    ASSERT_EQ(sq->dutyCycle, 2, "pwm: disabling restores voice default duty");

    /* The disabled effect no longer modulates the duty. */
    for (int i = 0; i < 4; i++)
        advance_frames(&engine, 1, outL, outR);
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
    advance_frames(&engine, 1, outL, outR);
    ASSERT(!engine.pwmActiveFlag, "pwm: active flag decays while all patterns are 0");
    m4a_engine_cc(&engine, 0, 0x17, 2);  /* PWMC alone re-enables the effect */
    ASSERT(engine.pwmActiveFlag, "pwm: PWMC alone re-arms the active flag");
    advance_frames(&engine, 1, outL, outR);
    advance_frames(&engine, 1, outL, outR);
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
    engine_init_booted(&engine, 44100.0f);
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

/* ---- PSG model (plugin/m4a_psg.c) against hand-computed mgba behavior ----
 *
 * Each expectation below is worked out from mgba 0.10.5 src/gb/audio.c in its
 * GB_AUDIO_GBA style (timing factor 4), not from running the model. */

/* Register-write semantics (GBAIOWrite8 / GBAIOWrite), the frame sequencer
 * restart on NR52, and the square channel's duty index catch-up. */
static void test_psg_square(void)
{
    printf("Testing PSG square channel vs mgba...\n");
    M4APsg p;
    memset(&p, 0, sizeof(p));
    m4a_psg_reset(&p);
    int64_t t = 0;

    /* Unit off: NR10 goes through the merged 16-bit path, which ignores
     * writes to 0x60..0x80 while NR52 is off; NR12 is a byte register and
     * lands anyway.  Reads of 0x60..0x80 return 0 while off. */
    m4a_psg_write8(&p, PSG_REG_NR10, 0x1B, t);
    ASSERT_EQ(p.ch1.sweep.shift, 0, "psg: NR10 write ignored while the unit is off");
    m4a_psg_write8(&p, PSG_REG_NR12, 0xF3, t);
    ASSERT_EQ(p.ch1.envelope.initialVolume, 15, "psg: byte NR12 write lands while off");
    ASSERT_EQ(m4a_psg_read8(&p, PSG_REG_NR12), 0, "psg: channel registers read 0 while off");

    m4a_psg_write8(&p, PSG_REG_NR52, 0x8F, t);
    ASSERT(p.enable, "psg: NR52 bit 7 enables the unit");
    ASSERT_EQ(p.frame, 7, "psg: enabling restarts the frame sequencer at frame 7");
    ASSERT_EQ(m4a_psg_read8(&p, PSG_REG_NR52), 0x80, "psg: NR52 image = enable bit, no channel playing");
    ASSERT_EQ(m4a_psg_read8(&p, PSG_REG_NR12), 0xF3, "psg: NRx2 reads back whole");

    m4a_psg_write8(&p, PSG_REG_NR50, 0x77, t);
    m4a_psg_write8(&p, PSG_REG_NR51, 0x11, t);
    ASSERT_EQ(m4a_psg_read8(&p, PSG_REG_NR51), 0x11, "psg: NR51 reads back");
    ASSERT_EQ(m4a_psg_read8(&p, PSG_REG_NR50), 0x77, "psg: NR50 reads back (stored & 0x77)");
    m4a_psg_write8(&p, PSG_REG_NR11, 0x80 | 5, t);          /* duty 2, length 5 */
    ASSERT_EQ(m4a_psg_read8(&p, PSG_REG_NR11), 0x80, "psg: NRx1 image keeps only the duty bits");
    ASSERT_EQ(p.ch1.length, 59, "psg: length counter = 64 - length");
    m4a_psg_write8(&p, PSG_REG_NR13, 0x00, t);
    m4a_psg_write8(&p, PSG_REG_NR14, 0x87, t);              /* freq 0x700, trigger */
    ASSERT_EQ(p.ch1.frequency, 0x700, "psg: NR13/NR14 assemble the 11-bit frequency");
    ASSERT(p.playingCh1, "psg: trigger with a nonzero volume starts the channel");
    ASSERT_EQ(m4a_psg_read8(&p, PSG_REG_NR13), 0, "psg: NRx3 is write-only");
    ASSERT_EQ(m4a_psg_read8(&p, PSG_REG_NR14), 0, "psg: NRx4 image keeps only the length-enable bit");
    ASSERT_EQ(m4a_psg_read8(&p, PSG_REG_NR52), 0x81, "psg: NR52 playing bit set by the trigger");
    ASSERT_EQ(p.ch1.length, 59, "psg: trigger leaves a nonzero length counter alone");

    /* Duty index: period 4 * (2048 - 0x700) * 4 = 4096 cycles per step,
     * lastUpdate 0 from reset.  Duty 2 = {1,0,0,0,0,1,1,1}. */
    ASSERT_EQ(p.ch1.sample, 15, "psg: sample = duty bit x volume at the trigger (index 0)");
    m4a_psg_run(&p, 4095, 0xF);
    ASSERT_EQ(p.ch1.index, 0, "psg: no step before one period has elapsed");
    m4a_psg_run(&p, 4096, 0xF);
    ASSERT_EQ(p.ch1.index, 1, "psg: index steps once per period");
    ASSERT_EQ(p.ch1.sample, 0, "psg: duty 2 is low at index 1");
    m4a_psg_run(&p, 5 * 4096 + 100, 0xF);
    ASSERT_EQ(p.ch1.index, 5, "psg: catch-up by whole periods");
    ASSERT_EQ(p.ch1.lastUpdate, 5 * 4096, "psg: lastUpdate stays on the period grid");
    ASSERT_EQ(p.ch1.sample, 15, "psg: duty 2 is high at index 5");

    /* GBAudioSamplePSG scale: 15 << 3, x (1 + 7) per side, both sides on. */
    int l = 0, r = 0;
    m4a_psg_sample(&p, 0xF, &l, &r);
    ASSERT_EQ(l, 960, "psg: left = (15 << 3) * (1 + NR50 left volume)");
    ASSERT_EQ(r, 960, "psg: right likewise");
    m4a_psg_sample(&p, 0xE, &l, &r);
    ASSERT_EQ(l, 0, "psg: channel mask excludes ch1");

    /* 64 Hz envelope: NR12 = 0xF3 (15, decrease, step 3) steps at every third
     * frame 7; the frame sequencer is at 7, so 8 steps bring it back to 7. */
    int64_t ft = 32768;
    for (int i = 0; i < 8; i++, ft += 32768)
        m4a_psg_frame(&p, ft);
    ASSERT_EQ(p.frame, 7, "psg: eight frame steps wrap to frame 7");
    ASSERT_EQ(p.ch1.envelope.nextStep, 2, "psg: envelope counter ticks at frame 7");
    ASSERT_EQ(p.ch1.envelope.currentVolume, 15, "psg: no volume step before the counter expires");
    for (int i = 0; i < 16; i++, ft += 32768)
        m4a_psg_frame(&p, ft);
    ASSERT_EQ(p.ch1.envelope.currentVolume, 14, "psg: volume steps down after 3 x 64 Hz clocks");
    ASSERT_EQ(p.ch1.envelope.nextStep, 3, "psg: step counter reloads");
    ASSERT_EQ(p.ch1.length, 59, "psg: length counter idle without the length-enable bit");

    /* Catch-up cases while the channel is not sounding.  NR12 = 0 (volume 0,
     * decrease) stops the channel; run(0xF) then leaves the index alone, but
     * channels == 0x1 (a register write to this channel) catches it up. */
    int64_t now = ft;
    m4a_psg_run(&p, now, 0xF);
    m4a_psg_write8(&p, PSG_REG_NR12, 0x00, now);
    ASSERT(!p.playingCh1, "psg: NRx2 = 0 (volume 0, decreasing) stops the channel");
    ASSERT_EQ(m4a_psg_read8(&p, PSG_REG_NR52) & 1, 0, "psg: NR52 playing bit cleared");
    uint8_t idx = p.ch1.index;
    int64_t lu = p.ch1.lastUpdate;
    m4a_psg_run(&p, now + 3 * 4096, 0xF);
    ASSERT_EQ(p.ch1.index, idx, "psg: a stopped channel does not step on run(0xF)");
    m4a_psg_run(&p, now + 3 * 4096, 0x1);
    ASSERT_EQ(p.ch1.index, (idx + 3) & 7, "psg: channels == 0x1 catches a stopped channel up");
    ASSERT_EQ(p.ch1.lastUpdate, lu + 3 * 4096, "psg: and advances lastUpdate");
    m4a_psg_run(&p, p.ch1.lastUpdate + 0x40000000 + 4096 * 3, 0xF);
    ASSERT_EQ(p.ch1.lastUpdate, lu + 3 * 4096 + 0x40000000 + 4096 * 3 - ((0x40000000 + 4096 * 3) % 4096),
              "psg: a lastUpdate over 0x40000000 behind is caught up by run(0xF)");

    /* Dead envelope: NR12 = 8 (volume 0, increase, step 0) keeps the channel
     * "playing" with dead = 2 and a zero sample -- the MPlayExtender state. */
    m4a_psg_write8(&p, PSG_REG_NR12, 0x08, now);
    m4a_psg_write8(&p, PSG_REG_NR14, 0x80, now);
    ASSERT(p.playingCh1, "psg: trigger with increase direction counts as playing");
    ASSERT_EQ(p.ch1.envelope.dead, 2, "psg: step 0 with volume 0 is dead = 2");
    ASSERT_EQ(p.ch1.sample, 0, "psg: dead channel outputs 0");
    ASSERT_EQ(p.ch1.frequency, 0, "psg: NRx4 = 0x80 clears the frequency high bits");

    /* NR52 off: merged-path registers are zeroed; byte image of NR11 too. */
    m4a_psg_write8(&p, PSG_REG_NR52, 0x00, now);
    ASSERT(!p.enable && !p.playingCh1, "psg: NR52 off stops everything");
    ASSERT_EQ(p.io[(PSG_REG_NR11 - 0x60) >> 1], 0, "psg: NR52 off clears the register images");
}

/* Square-1 sweep unit: shadow register, 128 Hz steps, overflow, and the
 * `occurred` direction-change quirk. */
static void test_psg_sweep(void)
{
    printf("Testing PSG sweep unit vs mgba...\n");
    M4APsg p;
    memset(&p, 0, sizeof(p));
    m4a_psg_reset(&p);
    int64_t t = 0;
    m4a_psg_write8(&p, PSG_REG_NR52, 0x8F, t);
    m4a_psg_write8(&p, PSG_REG_NR10, 0x1B, t);  /* time 1, down, shift 3 */
    ASSERT_EQ(p.ch1.sweep.time, 1, "sweep: NR10 time");
    ASSERT(p.ch1.sweep.direction, "sweep: NR10 direction = down");
    ASSERT_EQ(p.ch1.sweep.shift, 3, "sweep: NR10 shift");
    m4a_psg_write8(&p, PSG_REG_NR12, 0xF0, t);
    m4a_psg_write8(&p, PSG_REG_NR13, 0x00, t);
    m4a_psg_write8(&p, PSG_REG_NR14, 0x86, t);  /* 0x600 = 1536, trigger */
    ASSERT(p.playingCh1, "sweep: downward trigger passes the initial check");
    ASSERT_EQ(p.ch1.sweep.realFrequency, 1536, "sweep: shadow loaded at the trigger");
    ASSERT_EQ(p.ch1.frequency, 1536, "sweep: the initial calculation never writes the frequency");
    ASSERT(p.ch1.sweep.occurred, "sweep: initial calculation sets `occurred`");
    ASSERT(p.ch1.sweep.enable, "sweep: time != 8 enables the unit");

    /* Frames 0, 1 do nothing for the sweep; frame 2 decrements step 1 -> 0
     * and recalculates: 1536 - (1536 >> 3) = 1344. */
    int64_t ft = 32768;
    for (int i = 0; i < 2; i++, ft += 32768)
        m4a_psg_frame(&p, ft);
    ASSERT_EQ(p.ch1.frequency, 1536, "sweep: no change before frame 2");
    m4a_psg_frame(&p, ft); ft += 32768;
    ASSERT_EQ(p.frame, 2, "sweep: at frame 2");
    ASSERT_EQ(p.ch1.frequency, 1344, "sweep: f' = f - (f >> 3) at frame 2");
    ASSERT_EQ(p.ch1.sweep.realFrequency, 1344, "sweep: shadow follows");
    for (int i = 0; i < 4; i++, ft += 32768)
        m4a_psg_frame(&p, ft);
    ASSERT_EQ(p.frame, 6, "sweep: at frame 6");
    ASSERT_EQ(p.ch1.frequency, 1344 - (1344 >> 3), "sweep: second step at frame 6 compounds");
    ASSERT(p.playingCh1, "sweep: a downward sweep keeps sounding");

    /* Direction change down -> up after a calculation occurred turns the
     * channel off (mgba _writeSweep). */
    m4a_psg_write8(&p, PSG_REG_NR10, 0x11, ft);
    ASSERT(!p.playingCh1, "sweep: down->up NR10 write after `occurred` disables the channel");

    /* Upward overflow at the trigger: 1792 + (1792 >> 1) >= 2048. */
    m4a_psg_write8(&p, PSG_REG_NR13, 0x00, ft);
    m4a_psg_write8(&p, PSG_REG_NR14, 0x87, ft);
    ASSERT(!p.playingCh1, "sweep: upward overflow at the trigger silences the channel");
    ASSERT_EQ(m4a_psg_read8(&p, PSG_REG_NR52) & 1, 0, "sweep: NR52 bit cleared on overflow");

    /* Inert NR10 (time 0 -> 8, shift 0): the unit stays disabled. */
    m4a_psg_write8(&p, PSG_REG_NR10, 0x08, ft);
    m4a_psg_write8(&p, PSG_REG_NR14, 0x86, ft);
    ASSERT(p.playingCh1, "sweep: inert NR10 lets the trigger start the channel");
    ASSERT(!p.ch1.sweep.enable, "sweep: time 8 with shift 0 disables the unit");
    ASSERT_EQ(p.ch1.sweep.time, 8, "sweep: time 0 reads as 8");
    for (int i = 0; i < 16; i++, ft += 32768)
        m4a_psg_frame(&p, ft);
    ASSERT_EQ(p.ch1.frequency, 1536, "sweep: no sweeping with the inert value");
}

/* Wave channel: banked rotating wave RAM, step timing, volume shifts. */
static void test_psg_wave(void)
{
    printf("Testing PSG wave channel vs mgba...\n");
    M4APsg p;
    memset(&p, 0, sizeof(p));
    m4a_psg_reset(&p);
    int64_t t = 0;
    m4a_psg_write8(&p, PSG_REG_NR52, 0x8F, t);
    m4a_psg_write8(&p, PSG_REG_NR51, 0x44, t);
    m4a_psg_write8(&p, PSG_REG_NR50, 0x77, t);

    /* CgbSound's upload: select bank 1 (NR30 = 0x40) so the words land in
     * bank 0 (writes go to the bank not selected for playback). */
    m4a_psg_write8(&p, PSG_REG_NR30, 0x40, t);
    ASSERT(p.ch3.bank, "wave: NR30 bit 6 selects bank 1");
    m4a_psg_write_wave_ram(&p, 0, 0x01234567, t);
    m4a_psg_write_wave_ram(&p, 1, 0x89ABCDEF, t);
    m4a_psg_write_wave_ram(&p, 2, 0, t);
    m4a_psg_write_wave_ram(&p, 3, 0, t);
    ASSERT_EQ(p.ch3.wavedata32[0], 0x01234567u, "wave: upload lands in the other bank (words 0..3)");
    ASSERT_EQ(p.ch3.wavedata32[4], 0u, "wave: playing bank untouched");
    m4a_psg_write8(&p, PSG_REG_NR30, 0x00, t);
    ASSERT(!p.ch3.enable && !p.playingCh3, "wave: NR30 = 0 disables the DAC");
    m4a_psg_write8(&p, PSG_REG_NR31, 0x00, t);
    m4a_psg_write8(&p, PSG_REG_NR32, 0x20, t);      /* volume field 1 = 100% */
    ASSERT_EQ(p.ch3.volume, 1, "wave: NR32 bits 5-7");
    ASSERT_EQ(m4a_psg_read8(&p, PSG_REG_NR32), 0x20, "wave: NR32 image keeps bits 5-7");
    m4a_psg_write8(&p, PSG_REG_NR33, 0xF8, t);      /* rate 0x7F8 = 2040 */
    m4a_psg_write8(&p, PSG_REG_NR34, 0x87, t);      /* trigger while disabled */
    ASSERT(!p.playingCh3, "wave: a trigger with NR30 bit 7 clear does not start the channel");
    m4a_psg_write8(&p, PSG_REG_NR30, 0x80, t);
    m4a_psg_write8(&p, PSG_REG_NR34, 0x87, t);
    ASSERT(p.playingCh3, "wave: trigger after NR30 = 0x80 starts the channel");
    ASSERT_EQ(p.ch3.length, 256, "wave: zero length reloads to 256");
    /* First step after (6 + 2 * (2048 - 2040)) * 4 = 88 cycles, then every
     * 2 * 8 * 4 = 64.  Nibble order: byte 0 high nibble first. */
    ASSERT_EQ(p.ch3.nextUpdate, 88, "wave: first step delay (6 + 2 * (2048 - rate)) * 4");
    m4a_psg_run(&p, 87, 0xF);
    ASSERT_EQ(p.ch3.sample, 0, "wave: no sample before the first step");
    m4a_psg_run(&p, 88, 0xF);
    ASSERT_EQ(p.ch3.sample, 6, "wave: first nibble = high nibble of byte 0 (0x67 -> 6)");
    m4a_psg_run(&p, 88 + 64, 0xF);
    ASSERT_EQ(p.ch3.sample, 7, "wave: second nibble = low nibble of byte 0");
    m4a_psg_run(&p, 88 + 2 * 64, 0xF);
    ASSERT_EQ(p.ch3.sample, 4, "wave: third nibble = high nibble of byte 1");
    m4a_psg_run(&p, 88 + 7 * 64, 0xF);
    ASSERT_EQ(p.ch3.sample, 1, "wave: eighth nibble = low nibble of byte 3");
    m4a_psg_run(&p, 88 + 8 * 64, 0xF);
    ASSERT_EQ(p.ch3.sample, 0xE, "wave: ninth nibble comes from word 1 (0xEF -> E)");
    int l = 0, r = 0;
    m4a_psg_sample(&p, 0xF, &l, &r);
    ASSERT_EQ(l, (0xE << 3) * 8, "wave: output scale like the squares");
    m4a_psg_run(&p, 88 + 31 * 64, 0xF);
    m4a_psg_run(&p, 88 + 32 * 64, 0xF);
    ASSERT_EQ(p.ch3.sample, 6, "wave: 32 steps rotate the 4-word bank back to the start");

    /* Volume shifts: field 3 = 25% (>> 2), field 4 = 75% (x3 >> 2). */
    m4a_psg_write8(&p, PSG_REG_NR32, 0x60, 88 + 32 * 64);
    m4a_psg_run(&p, 88 + 33 * 64, 0xF);
    ASSERT_EQ(p.ch3.sample, 7 >> 2, "wave: 25% volume = nibble >> 2");
    m4a_psg_write8(&p, PSG_REG_NR32, 0x80, 88 + 33 * 64);
    m4a_psg_run(&p, 88 + 34 * 64, 0xF);
    ASSERT_EQ(p.ch3.sample, (4 * 3) >> 2, "wave: GBA 75% volume = (nibble * 3) >> 2");
    m4a_psg_write8(&p, PSG_REG_NR32, 0x00, 88 + 34 * 64);
    m4a_psg_run(&p, 88 + 35 * 64, 0xF);
    ASSERT_EQ(p.ch3.sample, 0, "wave: volume field 0 mutes");

    /* CgbOscOff(3): NR30 = 0 stops it. */
    m4a_psg_write8(&p, PSG_REG_NR30, 0x00, 88 + 35 * 64);
    ASSERT(!p.playingCh3, "wave: NR30 = 0 stops the channel");
}

/* Noise channel: LFSR period, 15/7-bit taps, point sampling, envelope step. */
static void test_psg_noise(void)
{
    printf("Testing PSG noise channel vs mgba...\n");
    M4APsg p;
    memset(&p, 0, sizeof(p));
    m4a_psg_reset(&p);
    int64_t t = 1000;
    m4a_psg_write8(&p, PSG_REG_NR52, 0x8F, t);
    m4a_psg_write8(&p, PSG_REG_NR51, 0x88, t);
    m4a_psg_write8(&p, PSG_REG_NR50, 0x77, t);
    m4a_psg_write8(&p, PSG_REG_NR41, 0x00, t);
    m4a_psg_write8(&p, PSG_REG_NR42, 0xF0, t);
    m4a_psg_write8(&p, PSG_REG_NR43, 0x00, t);      /* ratio 0, 15-bit, shift 0 */
    ASSERT_EQ(m4a_psg_read8(&p, PSG_REG_NR43), 0x00, "noise: NR43 reads back");
    m4a_psg_write8(&p, PSG_REG_NR44, 0x80, t);
    ASSERT(p.playingCh4, "noise: trigger starts the channel");
    ASSERT_EQ(p.ch4.lfsr, 0x7FFFu, "noise: 15-bit LFSR seeded at the trigger");
    ASSERT_EQ(p.ch4.lastEvent, t, "noise: LFSR clock starts at the trigger");
    /* Period: (ratio ? 2 * ratio : 1) << shift, x 8 x 4 = 32 cycles.  Each
     * step: lsb out, lfsr >>= 1, lfsr ^= lsb * 0x6000. */
    m4a_psg_run(&p, t + 31, 0xF);
    ASSERT_EQ(p.ch4.sample, 0, "noise: no output before the first LFSR step");
    m4a_psg_run(&p, t + 32, 0xF);
    ASSERT_EQ(p.ch4.lfsr, 0x5FFFu, "noise: first step 0x7FFF -> 0x5FFF");
    ASSERT_EQ(p.ch4.sample, 15, "noise: sample = lsb x volume (point-sampled)");
    m4a_psg_run(&p, t + 3 * 32 + 5, 0xF);
    ASSERT_EQ(p.ch4.lfsr, 0x47FFu, "noise: three steps 0x7FFF -> 0x47FF");
    ASSERT_EQ(p.ch4.lastEvent, t + 3 * 32, "noise: lastEvent stays on the LFSR grid");
    int l = 0, r = 0;
    m4a_psg_sample(&p, 0xF, &l, &r);
    ASSERT_EQ(l, (15 << 3) * 8, "noise: output = (sample << 3) x (1 + NR50)");
    /* 14 ones shift out before the first zero: step 15 outputs 0. */
    m4a_psg_run(&p, t + 14 * 32, 0xF);
    ASSERT_EQ(p.ch4.lfsr, 0x4000u, "noise: after 14 steps only bit 14 is set");
    m4a_psg_run(&p, t + 15 * 32, 0xF);
    ASSERT_EQ(p.ch4.lfsr, 0x2000u, "noise: step 15 shifts a 0 out");
    ASSERT_EQ(p.ch4.sample, 0, "noise: a 0 bit outputs 0");

    /* Envelope step at frame 7 rescales the held sample. */
    m4a_psg_run(&p, t + 16 * 32, 0xF);
    ASSERT_EQ(p.ch4.sample, 0, "noise: step 16 (lfsr 0x1000) outputs 0");
    m4a_psg_run(&p, t + 28 * 32, 0xF);
    ASSERT_EQ(p.ch4.lfsr, 0x0001u, "noise: bit 14 walks down to bit 0 in 13 more steps");
    m4a_psg_run(&p, t + 29 * 32, 0xF);
    ASSERT_EQ(p.ch4.lfsr, 0x6000u, "noise: a 1 shifts out and taps bits 13-14");
    ASSERT_EQ(p.ch4.sample, 15, "noise: output follows the shifted-out bit");

    /* 7-bit mode and a slow ratio/shift. */
    m4a_psg_write8(&p, PSG_REG_NR43, 0x2B, t + 30 * 32);   /* ratio 3, 7-bit, shift 2 */
    m4a_psg_write8(&p, PSG_REG_NR44, 0x80, t + 30 * 32);
    ASSERT_EQ(p.ch4.lfsr, 0x7Fu, "noise: 7-bit LFSR seed");
    int64_t t2 = t + 30 * 32;
    int period = (2 * 3) << 2;
    period *= 32;   /* 768 cycles */
    m4a_psg_run(&p, t2 + period - 1, 0xF);
    ASSERT_EQ(p.ch4.lfsr, 0x7Fu, "noise: no step before (2 x ratio) << shift x 32 cycles");
    m4a_psg_run(&p, t2 + period, 0xF);
    ASSERT_EQ(p.ch4.lfsr, 0x5Fu, "noise: 7-bit taps bits 5-6 (0x7F -> 0x5F)");

    /* Envelope: NR42 = 0xF1 (step 1): the frame-7 step drops the volume and
     * rescales the held sample. */
    m4a_psg_write8(&p, PSG_REG_NR42, 0xF1, t2 + period);
    m4a_psg_write8(&p, PSG_REG_NR44, 0x80, t2 + period);
    m4a_psg_run(&p, t2 + 2 * period, 0xF);
    ASSERT_EQ(p.ch4.sample, 15, "noise: fresh trigger at volume 15");
    for (int i = 0; i < 8; i++)
        m4a_psg_frame(&p, t2 + 2 * period);
    ASSERT_EQ(p.frame, 7, "noise: frame sequencer at 7");
    ASSERT_EQ(p.ch4.envelope.currentVolume, 14, "noise: envelope step at frame 7");
    ASSERT_EQ(p.ch4.sample, 14, "noise: held sample rescaled to the new volume");
}

/* ---- CgbSound driver (plugin/m4a_channel.c): register sequences ---- */

typedef struct {
    uint32_t reg[64];
    uint32_t val[64];
    int64_t when[64];
    int n;
    const M4AEngine *engine;
} WriteLog;

static void write_log_cb(void *user, uint32_t reg, uint32_t value)
{
    WriteLog *log = (WriteLog *)user;
    if (log->n < 64) {
        log->reg[log->n] = reg;
        log->val[log->n] = value;
        log->when[log->n] = log->engine->gba.now;
        log->n++;
    }
}

static int log_matches(const WriteLog *log, const uint32_t *expect, int n)
{
    if (log->n != n)
        return 0;
    for (int i = 0; i < n; i++)
        if (log->reg[i] != expect[2 * i] || log->val[i] != expect[2 * i + 1])
            return 0;
    return 1;
}

static void log_dump(const WriteLog *log)
{
    for (int i = 0; i < log->n; i++)
        fprintf(stderr, "    [%d] reg 0x%02X = 0x%02X\n", i, log->reg[i], log->val[i]);
}

#define EXPECT_LOG(log, msg, ...) do { \
    const uint32_t _e[] = { __VA_ARGS__ }; \
    int _ok = log_matches((log), _e, (int)(sizeof(_e) / sizeof(_e[0]) / 2)); \
    if (!_ok) log_dump(log); \
    ASSERT(_ok, msg); \
} while (0)

/* Boot: the SoundInit/MPlayExtender register writes reach both PSGs. */
static void test_cgb_driver_boot(void)
{
    printf("Testing CgbSound driver: boot writes...\n");
    M4AEngine engine;
    m4a_engine_init(&engine, 48000.0f);
    WriteLog log = { .n = 0, .engine = &engine };
    engine.psg[0].writeLog = write_log_cb;
    engine.psg[0].writeLogUser = &log;
    m4a_engine_boot(&engine, false);
    EXPECT_LOG(&log, "boot: SoundInit NR52, then MPlayExtender's PSG setup, in order",
               PSG_REG_NR52, 0x8F,
               PSG_REG_NR52, 0x8F, PSG_REG_NR50, 0x00, PSG_REG_NR51, 0x00,
               PSG_REG_NR12, 0x08, PSG_REG_NR22, 0x08, PSG_REG_NR42, 0x08,
               PSG_REG_NR14, 0x80, PSG_REG_NR24, 0x80, PSG_REG_NR44, 0x80,
               PSG_REG_NR30, 0x00, PSG_REG_NR50, 0x77);
    ASSERT_EQ(log.when[0], 5720, "boot: SOUNDCNT_X at cycle 5720");
    ASSERT_EQ(log.when[1], 39875, "boot: MPlayExtender starts at cycle 39875");
    ASSERT(log.when[11] <= 39935, "boot: MPlayExtender ends by cycle 39935");
    for (int i = 0; i < 2; i++) {
        M4APsg *p = &engine.psg[i];
        ASSERT(p->enable, "boot: PSG enabled");
        ASSERT(p->playingCh1 && p->playingCh2 && p->playingCh4, "boot: squares/noise triggered at volume 0");
        ASSERT_EQ(p->ch1.envelope.dead, 2, "boot: dead envelopes");
        ASSERT_EQ(p->volumeLeft, 7, "boot: NR50 = 0x77");
        ASSERT_EQ(m4a_psg_read8(p, PSG_REG_NR12), 0x08, "boot: NR12 image = 8");
    }
    m4a_engine_destroy(&engine);
}

/* Square 1 note: the CgbSound start-frame sequence, the MO_PIT update, the
 * instant-release CgbOscOff, and the write timestamp. */
static void test_cgb_driver_square(void)
{
    printf("Testing CgbSound driver: square register sequences...\n");
    ToneData voices[128];
    memset(voices, 0, sizeof(voices));
    /* voice 0: sq1, 50% duty, attack 0, decay 0, sustain 15, release 0, no sweep */
    voices[0].type = VOICE_SQUARE_1;
    voices[0].key = 60;
    voices[0].wavePointer = (uint32_t *)(uintptr_t)2;
    voices[0].sustain = 15;
    /* voice 1: sq1 with an upward sweep (NR10 bit 3 clear -> double NR14) and a real envelope */
    voices[1] = voices[0];
    voices[1].panSweep = 0x11;
    voices[1].attack = 2;
    voices[1].decay = 3;
    voices[1].sustain = 8;
    voices[1].release = 1;
    /* voice 2: sq2, 25% duty */
    voices[2].type = VOICE_SQUARE_2;
    voices[2].key = 60;
    voices[2].wavePointer = (uint32_t *)(uintptr_t)1;
    voices[2].sustain = 15;
    /* voice 3: sq2 with attack 1, decay 0: the attack's goal frame falls through to sustain */
    voices[3] = voices[2];
    voices[3].attack = 1;

    M4AEngine engine;
    float outL[1024], outR[1024];
    engine_init_booted(&engine, 48000.0f);
    m4a_engine_set_voicegroup(&engine, voices);
    WriteLog log = { .n = 0, .engine = &engine };
    engine.psg[0].writeLog = write_log_cb;
    engine.psg[0].writeLogUser = &log;
    M4ACGBChannel *sq = &engine.cgbChannels[0];

    /* --- voice 0: attack 0 / decay 0 -> straight to sustain --- */
    m4a_engine_program_change(&engine, 0, 0);
    m4a_engine_note_on(&engine, 0, 60, 127);
    ASSERT_EQ(sq->status, CHN_START, "driver: note-on only flags CHN_START");
    ASSERT_EQ(log.n, 0, "driver: no register writes until the next SoundMain");
    int64_t soundMain = engine.nextSoundMain;
    advance_frames(&engine, 1, outL, outR);
    uint32_t f = sq->frequency;
    uint8_t nr51 = 0x11 & sq->pan;      /* pan 0xFF & panMask 0x11 */
    uint8_t vol = sq->envelopeVolume;
    ASSERT_EQ(sq->status, CHN_ENV_SUSTAIN, "driver: attack 0, decay 0 -> sustain in the start frame");
    EXPECT_LOG(&log, "driver: square-1 start frame: NR10, NR11, NR13, NR14 (no trigger), NR51, NR12, NR14 | 0x80",
               PSG_REG_NR10, 0x08,
               PSG_REG_NR11, (2u << 6),
               PSG_REG_NR13, f & 0xFF,
               PSG_REG_NR14, (f >> 8) & 0x07,
               PSG_REG_NR51, nr51,
               PSG_REG_NR12, 0x08 | (vol << 4),
               PSG_REG_NR14, 0x80 | ((f >> 8) & 0x07));
    /* SoundMain timing model (MIDI path: steady one-track MPlayMain): the
     * first write lands at the CgbSound entry + prologue + square-1 start
     * offset, and the rest follow at the gaps measured in mgba. */
    int64_t cgbEntry = soundMain + M4A_CYC_CGB_ENTRY_BASE + M4A_CYC_PER_TRACK - M4A_CYC_SOUNDMAIN_ENTRY;
    ASSERT_EQ(log.when[0], cgbEntry + 351, "driver: NR10 at CgbSound entry + 351 (loop head 53, active 26, switch 69, start 24, CgbModVol 44, NR10 135)");
    ASSERT_EQ(log.when[1] - log.when[0], 10, "driver: NR11 10 cycles after NR10");
    ASSERT_EQ(log.when[2] - log.when[0], 212, "driver: NR13 212 after NR10 (envelope setup between)");
    ASSERT_EQ(log.when[3] - log.when[0], 244, "driver: NR14 32 after NR13");
    ASSERT_EQ(log.when[4] - log.when[0], 277, "driver: NR51 33 after NR14");
    ASSERT_EQ(log.when[5] - log.when[0], 305, "driver: NR12 28 after NR51");
    ASSERT_EQ(log.when[6] - log.when[0], 315, "driver: NR14 10 after NR12");
    ASSERT_EQ((soundMain - engine.nextSoundMain + GBA_CYCLES_PER_FRAME) % GBA_CYCLES_PER_FRAME, 0,
              "driver: one SoundMain per frame");
    ASSERT_EQ(engine.psg[0].ch1.frequency, (int)f, "driver: PSG frequency follows NR13/NR14");
    ASSERT(engine.psg[0].playingCh1 && engine.psg[0].ch1.envelope.currentVolume == (vol & 0xF),
           "driver: PSG channel 1 sounding at the software envelope volume");
    ASSERT(!engine.psg[1].playingCh1 || engine.psg[1].ch1.envelope.currentVolume == 0,
           "driver: the shadow PSG hears nothing for a real note");

    /* Steady frame: no writes (sustain holds, no modify flags). */
    log.n = 0;
    advance_frames(&engine, 1, outL, outR);
    ASSERT_EQ(log.n, 0, "driver: a sustaining channel writes nothing");

    /* Pitch bend -> MO_PIT: NRx3 then NRx4 without the trigger bit. */
    m4a_engine_pitch_bend(&engine, 0, 4096);
    log.n = 0;
    advance_frames(&engine, 1, outL, outR);
    uint32_t f2 = sq->frequency;
    ASSERT(f2 != f, "driver: bend changed the frequency");
    EXPECT_LOG(&log, "driver: MO_PIT writes NR13, NR14 = n4 (no trigger)",
               PSG_REG_NR13, f2 & 0xFF,
               PSG_REG_NR14, (f2 >> 8) & 0x07);

    /* Note-off with release 0: pseudo-echo volume 0 -> CgbOscOff in the next frame. */
    m4a_engine_note_off(&engine, 0, 60);
    log.n = 0;
    advance_frames(&engine, 1, outL, outR);
    EXPECT_LOG(&log, "driver: release 0 -> CgbOscOff(1): NR12 = 8, NR14 = 0x80",
               PSG_REG_NR12, 0x08, PSG_REG_NR14, 0x80);
    ASSERT_EQ(sq->status, 0, "driver: channel freed");
    ASSERT_EQ(engine.psg[0].ch1.envelope.currentVolume, 0, "driver: PSG silenced by the trigger at volume 0");

    /* --- voice 1: sweep + attack: envelopeStepTimeAndDir and the double NR14 --- */
    m4a_engine_pitch_bend(&engine, 0, 0);
    m4a_engine_program_change(&engine, 0, 1);
    m4a_engine_note_on(&engine, 0, 60, 127);
    log.n = 0;
    advance_frames(&engine, 1, outL, outR);
    f = sq->frequency;
    ASSERT_EQ(sq->status, CHN_ENV_ATTACK, "driver: attack 2 starts in the attack phase");
    ASSERT_EQ(sq->envelopeVolume, 0, "driver: attack starts from volume 0");
    EXPECT_LOG(&log, "driver: sweep voice: NR10 = sweep, NR12 = attack | inc, two NR14 trigger writes",
               PSG_REG_NR10, 0x11,
               PSG_REG_NR11, (2u << 6),
               PSG_REG_NR13, f & 0xFF,
               PSG_REG_NR14, (f >> 8) & 0x07,
               PSG_REG_NR51, nr51,
               PSG_REG_NR12, (0x08 | 2) | (0u << 4),
               PSG_REG_NR14, 0x80 | ((f >> 8) & 0x07),
               PSG_REG_NR14, 0x80 | ((f >> 8) & 0x07));
    ASSERT_EQ(engine.psg[0].ch1.sweep.time, 1, "driver: PSG sweep programmed from the voice");
    /* Software attack: one step per frame (two on c15 frames) until the goal,
     * then decay: NR12 = decay | dec with the goal volume. */
    int frames = 0;
    log.n = 0;
    while (sq->status == CHN_ENV_ATTACK && frames < 40) {
        log.n = 0;
        advance_frames(&engine, 1, outL, outR);
        frames++;
    }
    ASSERT_EQ(sq->status, CHN_ENV_DECAY, "driver: attack reaches the goal and enters decay");
    ASSERT(frames >= 1 && frames <= 32, "driver: attack 2 takes a plausible number of frames");
    /* NR10 bit 3 is clear for this voice, so every MO_VOL ends with two NR14 writes. */
    ASSERT(log.n >= 4, "driver: the decay transition writes registers");
    ASSERT_EQ(log.reg[log.n - 4], PSG_REG_NR51, "driver: MO_VOL starts with NR51");
    ASSERT_EQ(log.val[log.n - 3], (uint32_t)((3 | 0x00) | (sq->envelopeGoal << 4)),
              "driver: decay start writes NR12 = decay | dec, goal volume");
    ASSERT_EQ(log.reg[log.n - 2], PSG_REG_NR14, "driver: followed by the NR14 trigger");
    ASSERT_EQ(log.reg[log.n - 1], PSG_REG_NR14, "driver: written twice when NR10 bit 3 is clear");

    /* The envelopeStepTimeAndDir fallback: a frame with MO_VOL but no new
     * step/dir (track volume change) reads the NR12 image back. */
    while (sq->status == CHN_ENV_DECAY) advance_frames(&engine, 1, outL, outR);
    ASSERT_EQ(sq->status, CHN_ENV_SUSTAIN, "driver: decay settles into sustain");
    m4a_engine_cc(&engine, 0, 7, 100);
    log.n = 0;
    advance_frames(&engine, 1, outL, outR);
    ASSERT(log.n >= 4, "driver: a volume change writes MO_VOL registers");
    ASSERT_EQ(log.reg[log.n - 4], PSG_REG_NR51, "driver: NR51 first");
    ASSERT_EQ(log.val[log.n - 3] & 0x0F, 0x08u, "driver: NR12 low nibble read back from the register image (sustain: 8)");
    ASSERT_EQ(log.val[log.n - 3] >> 4, (uint32_t)(sq->envelopeVolume & 0xF), "driver: NR12 high nibble = software volume");

    /* --- square 2 start: no NR10, single trigger --- */
    m4a_engine_all_sound_off(&engine);
    m4a_engine_program_change(&engine, 1, 2);
    m4a_engine_note_on(&engine, 1, 60, 127);
    log.n = 0;
    uint8_t nr51Before = m4a_psg_read8(&engine.psg[0], PSG_REG_NR51);
    advance_frames(&engine, 1, outL, outR);
    M4ACGBChannel *sq2 = &engine.cgbChannels[1];
    f = sq2->frequency;
    EXPECT_LOG(&log, "driver: square-2 start frame: NR21, NR23, NR24, NR51, NR22, NR24 | 0x80",
               PSG_REG_NR21, (1u << 6),
               PSG_REG_NR23, f & 0xFF,
               PSG_REG_NR24, (f >> 8) & 0x07,
               PSG_REG_NR51, (nr51Before & ~0x22u) | (0x22 & sq2->pan),
               PSG_REG_NR22, 0x08 | (sq2->envelopeVolume << 4),
               PSG_REG_NR24, 0x80 | ((f >> 8) & 0x07));
    /* Channel 1 is inactive (44), then square 2's pass: loop 26, switch 61,
     * start 24, CgbModVol 44, NR21 131 (centre pan); the decay -> sustain
     * start block (70 + 76), envelope_complete 18, pitch 10 + 28. */
    {
        int64_t entry2 = log.when[0] - (53 + 44 + 26 + 61 + 24 + 44 + 131);
        ASSERT_EQ((entry2 - soundMain) % GBA_CYCLES_PER_FRAME,
                  (int64_t)(M4A_CYC_CGB_ENTRY_BASE + M4A_CYC_PER_TRACK - M4A_CYC_SOUNDMAIN_ENTRY),
                  "driver: square-2 NR21 at CgbSound entry + 383 (an inactive channel 1 before it)");
        ASSERT_EQ(log.when[1] - log.when[0], 146 + 18 + 10 + 28, "driver: NR23 202 after NR21 (sustain start block)");
        ASSERT_EQ(log.when[3] - log.when[2], 12 + 21, "driver: NR51 33 after NR24");
    }

    /* TrackStop / all-sound-off: CgbOscOff immediately. */
    log.n = 0;
    m4a_engine_all_sound_off(&engine);
    EXPECT_LOG(&log, "driver: all-sound-off -> CgbOscOff(2)", PSG_REG_NR22, 0x08, PSG_REG_NR24, 0x80);
    ASSERT_EQ(sq2->status, 0, "driver: channel freed by all-sound-off");

    /* --- square 2, attack 1 / decay 0: the goal frame is one envelope pass
     * (attack step -> envelope_decay_start -> envelope_sustain_start ->
     * envelope_sustain), charged once, not as attack->decay plus
     * decay->sustain. --- */
    m4a_engine_program_change(&engine, 1, 3);
    m4a_engine_note_on(&engine, 1, 60, 127);
    advance_frames(&engine, 1, outL, outR);
    ASSERT_EQ(sq2->status, CHN_ENV_ATTACK, "driver: attack 1 starts in the attack phase");
    frames = 0;
    while (sq2->status == CHN_ENV_ATTACK && frames < 40) {
        soundMain = engine.nextSoundMain;
        log.n = 0;
        advance_frames(&engine, 1, outL, outR);
        frames++;
    }
    ASSERT_EQ(sq2->status, CHN_ENV_SUSTAIN, "driver: attack 1 / decay 0 reaches sustain in the goal frame");
    ASSERT_EQ(sq2->envelopeVolume, sq2->sustainGoal, "driver: volume = sustainGoal");
    EXPECT_LOG(&log, "driver: the goal frame writes NR51, NR22 = sustain | inc, NR24 trigger",
               PSG_REG_NR51, (uint8_t)((nr51Before & ~0x22u) | (0x22 & sq2->pan)),
               PSG_REG_NR22, (uint32_t)(0x08 | (sq2->envelopeVolume << 4)),
               PSG_REG_NR24, 0x80 | ((sq2->frequency >> 8) & 0x07));
    /* channel 1 inactive 44, loop 26, switch 61, no-start 30 + 38, step ->
     * CgbModVol 18 + 10, attack -> sustain 229 (centre pan), envelope_complete
     * 18, -> volume block 26, NR51 21; the start frame's SoundMain is one
     * track's MPlayMain before CgbSound as in the first note. */
    {
        int64_t entry3 = log.when[0] - (53 + 44 + 26 + 61 + 30 + 38 + 18 + 10 + 229 + 18 + 26 + 21);
        ASSERT_EQ((entry3 - soundMain) % GBA_CYCLES_PER_FRAME,
                  (int64_t)(M4A_CYC_CGB_ENTRY_BASE + M4A_CYC_PER_TRACK - M4A_CYC_SOUNDMAIN_ENTRY),
                  "driver: attack->sustain goal frame: NR51 at CgbSound entry + 574 (one pass, 229)");
        ASSERT_EQ(log.when[1] - log.when[0], 28, "driver: NR22 28 after NR51");
        ASSERT_EQ(log.when[2] - log.when[1], 10, "driver: NR24 10 after NR22");
    }
    log.n = 0;
    m4a_engine_all_sound_off(&engine);

    m4a_engine_destroy(&engine);
}

/* Wave and noise notes: the upload-once wave RAM sequence with the n4
 * trigger handling, and the noise period/frequency writes. */
static void test_cgb_driver_wave_noise(void)
{
    printf("Testing CgbSound driver: wave and noise register sequences...\n");
    static uint32_t tableA[4] = { 0x01234567, 0x89ABCDEF, 0xFEDCBA98, 0x76543210 };
    static uint32_t tableB[4] = { 0x00000000, 0xFFFFFFFF, 0x00000000, 0xFFFFFFFF };
    ToneData voices[128];
    memset(voices, 0, sizeof(voices));
    voices[0].type = VOICE_PROGRAMMABLE_WAVE;
    voices[0].key = 60;
    voices[0].wavePointer = tableA;
    voices[0].sustain = 15;
    voices[1] = voices[0];
    voices[1].wavePointer = tableB;
    voices[2].type = VOICE_NOISE;
    voices[2].key = 60;
    voices[2].wavePointer = (uint32_t *)(uintptr_t)1;   /* 7-bit */
    voices[2].sustain = 15;
    voices[2].release = 2;

    M4AEngine engine;
    float outL[1024], outR[1024];
    engine_init_booted(&engine, 48000.0f);
    m4a_engine_set_voicegroup(&engine, voices);
    WriteLog log = { .n = 0, .engine = &engine };
    engine.psg[0].writeLog = write_log_cb;
    engine.psg[0].writeLogUser = &log;
    M4ACGBChannel *wv = &engine.cgbChannels[2];
    M4ACGBChannel *nz = &engine.cgbChannels[3];

    m4a_engine_program_change(&engine, 0, 0);
    m4a_engine_note_on(&engine, 0, 60, 127);
    advance_frames(&engine, 1, outL, outR);
    uint32_t f = wv->frequency;
    uint8_t nr32 = wv->envelopeVolume < 16 ? gCgb3Vol[wv->envelopeVolume] : 0;
    EXPECT_LOG(&log, "driver: wave start: NR30 = 0x40, wave RAM x4, NR30 = 0, NR31, NR33, NR34 = 0x80 | hi, NR51, NR32, NR30 = 0x80, NR34 = n4",
               PSG_REG_NR30, 0x40,
               PSG_REG_WAVE_RAM + 0, 0x01234567, PSG_REG_WAVE_RAM + 4, 0x89ABCDEF,
               PSG_REG_WAVE_RAM + 8, 0xFEDCBA98, PSG_REG_WAVE_RAM + 12, 0x76543210,
               PSG_REG_NR30, 0x00,
               PSG_REG_NR31, 0x00,
               PSG_REG_NR33, f & 0xFF,
               PSG_REG_NR34, 0x80 | ((f >> 8) & 0x07),
               PSG_REG_NR51, 0x44 & wv->pan,
               PSG_REG_NR32, nr32,
               PSG_REG_NR30, 0x80,
               PSG_REG_NR34, 0x80 | ((f >> 8) & 0x07));
    ASSERT_EQ(wv->n4, (uint8_t)((f >> 8) & 0x07), "driver: n4 trigger bit cleared after the first MO_VOL");
    ASSERT(engine.psg[0].playingCh3, "driver: PSG wave channel sounding");
    ASSERT(wv->currentWavePointer == tableA, "driver: currentPointer = uploaded table");

    /* Same table again: no upload, no NR30 = 0x40. */
    m4a_engine_note_off(&engine, 0, 60);
    advance_frames(&engine, 1, outL, outR);
    m4a_engine_note_on(&engine, 0, 72, 127);
    log.n = 0;
    advance_frames(&engine, 1, outL, outR);
    f = wv->frequency;
    EXPECT_LOG(&log, "driver: same-table wave note skips the wave RAM upload",
               PSG_REG_NR30, 0x00,
               PSG_REG_NR31, 0x00,
               PSG_REG_NR33, f & 0xFF,
               PSG_REG_NR34, 0x80 | ((f >> 8) & 0x07),
               PSG_REG_NR51, 0x44 & wv->pan,
               PSG_REG_NR32, nr32,
               PSG_REG_NR30, 0x80,
               PSG_REG_NR34, 0x80 | ((f >> 8) & 0x07));

    /* Different table: uploaded. Note-off with release 0 -> NR30 = 0 first. */
    m4a_engine_note_off(&engine, 0, 72);
    log.n = 0;
    advance_frames(&engine, 1, outL, outR);
    EXPECT_LOG(&log, "driver: wave CgbOscOff is NR30 = 0", PSG_REG_NR30, 0x00);
    m4a_engine_program_change(&engine, 0, 1);
    m4a_engine_note_on(&engine, 0, 60, 127);
    log.n = 0;
    advance_frames(&engine, 1, outL, outR);
    ASSERT(log.n == 13 && log.reg[0] == PSG_REG_NR30 && log.val[0] == 0x40
           && log.reg[1] == PSG_REG_WAVE_RAM && log.val[1] == 0x00000000
           && log.reg[2] == PSG_REG_WAVE_RAM + 4 && log.val[2] == 0xFFFFFFFF,
           "driver: a new table is uploaded");

    /* Noise: NR41, NR43 = period << 3, NR43 = (old & 8) | freq, NR44 = n4, NR51, NR42, NR44 | 0x80. */
    m4a_engine_all_sound_off(&engine);
    m4a_engine_program_change(&engine, 1, 2);
    m4a_engine_note_on(&engine, 1, 60, 127);
    log.n = 0;
    uint8_t nr51Before = m4a_psg_read8(&engine.psg[0], PSG_REG_NR51);
    advance_frames(&engine, 1, outL, outR);
    f = nz->frequency;
    ASSERT_EQ(f & 0x08, 0x08u, "driver: noise period bit kept in the frequency word");
    uint8_t nr51Noise = (uint8_t)((nr51Before & ~0x88u) | (0x88 & nz->pan));
    EXPECT_LOG(&log, "driver: noise start sequence",
               PSG_REG_NR41, 0x00,
               PSG_REG_NR43, 0x08,
               PSG_REG_NR43, f & 0xFF,
               PSG_REG_NR44, 0x00,
               PSG_REG_NR51, nr51Noise,
               PSG_REG_NR42, 0x08 | (nz->envelopeVolume << 4),
               PSG_REG_NR44, 0x80);
    ASSERT(engine.psg[0].playingCh4 && engine.psg[0].ch4.power, "driver: PSG noise in 7-bit mode");
    ASSERT_EQ(engine.psg[0].ch4.ratio, (int)(f & 7), "driver: NR43 ratio from gNoiseTable");

    /* Release 2: MO_VOL with NR42 = release | dec at the software volume, then
     * the software envelope counts down and CgbOscOff ends it. */
    m4a_engine_note_off(&engine, 1, 60);
    log.n = 0;
    advance_frames(&engine, 1, outL, outR);
    EXPECT_LOG(&log, "driver: release start writes NR51, NR42 = release | dec, NR44 trigger",
               PSG_REG_NR51, nr51Noise,
               PSG_REG_NR42, 0x02 | (nz->envelopeVolume << 4),
               PSG_REG_NR44, 0x80);
    int frames = 0;
    while ((nz->status & CHN_ON) && frames < 200) {
        advance_frames(&engine, 1, outL, outR);
        frames++;
    }
    ASSERT(!(nz->status & CHN_ON), "driver: release completes");
    ASSERT(log.n >= 2 && log.reg[log.n - 2] == PSG_REG_NR42 && log.val[log.n - 2] == 0x08
           && log.reg[log.n - 1] == PSG_REG_NR44 && log.val[log.n - 1] == 0x80,
           "driver: the release ends with CgbOscOff(4)");

    m4a_engine_destroy(&engine);
}

/* Square-1 frequency sweep through the whole stack: a sweep voice's
 * frequency glides in the PSG and the channel keeps sounding (the sq1_sweep
 * micro-test used to render silence). */
static void test_sweep(void)
{
    printf("Testing square-1 frequency sweep (driver + PSG)...\n");
    ToneData voices[128];
    memset(voices, 0, sizeof(voices));
    voices[0].type = VOICE_SQUARE_1;
    voices[0].key = 60;
    voices[0].wavePointer = (uint32_t *)(uintptr_t)2;
    voices[0].sustain = 15;
    voices[0].panSweep = 0x3A;   /* time 3, down, shift 2 (the micro-test voice) */
    voices[1] = voices[0];
    voices[1].panSweep = 0x11;   /* time 1, up, shift 1 */
    voices[2] = voices[0];
    voices[2].panSweep = 0xC0;   /* a pan, not a sweep */

    M4AEngine engine;
    float outL[1024], outR[1024];
    engine_init_booted(&engine, 44100.0f);
    m4a_engine_set_voicegroup(&engine, voices);
    M4APsg *p = &engine.psg[0];
    M4ACGBChannel *sq = &engine.cgbChannels[0];

    m4a_engine_program_change(&engine, 0, 0);
    m4a_engine_note_on(&engine, 0, 60, 100);
    advance_frames(&engine, 1, outL, outR);
    ASSERT_EQ(sq->sweep, 0x3A, "sweep: NR10 byte captured at note-on");
    ASSERT_EQ(p->ch1.sweep.time, 3, "sweep: PSG sweep time");
    ASSERT(p->ch1.sweep.direction, "sweep: PSG sweep down");
    ASSERT_EQ(p->ch1.sweep.shift, 2, "sweep: PSG sweep shift");
    ASSERT(p->playingCh1, "sweep: channel sounding");
    int f0 = p->ch1.frequency;
    ASSERT_EQ(f0, (int)sq->frequency, "sweep: PSG frequency from the driver");
    /* 3 sweep clocks at 128 Hz = 15.6..23.4 ms after the trigger: exactly
     * one step has happened two frames (33 ms) later. */
    advance_frames(&engine, 2, outL, outR);
    ASSERT_EQ(p->ch1.frequency, f0 - (f0 >> 2), "sweep: first step f - (f >> 2)");
    for (int i = 0; i < 120; i++)
        advance_frames(&engine, 1, outL, outR);
    ASSERT(p->ch1.frequency < 16, "sweep: downward sweep decays toward register 0");
    ASSERT(p->playingCh1, "sweep: downward sweep stays audible");
    ASSERT(sq->status & CHN_ON, "sweep: software envelope unaffected");
    float energy = 0.0f;
    m4a_engine_process(&engine, outL, outR, 1024);
    for (int i = 0; i < 1024; i++) energy += fabsf(outL[i]);
    ASSERT(energy > 1.0f, "sweep: still producing sound (the sq1_sweep silence bug)");

    /* Upward overflow silences the channel in the PSG. */
    m4a_engine_all_sound_off(&engine);
    m4a_engine_program_change(&engine, 0, 1);
    m4a_engine_note_on(&engine, 0, 36, 100);
    advance_frames(&engine, 1, outL, outR);
    ASSERT(p->playingCh1, "sweep: low-key upward sweep passes the initial check");
    int fu0 = p->ch1.frequency;
    for (int i = 0; i < 120 && p->playingCh1; i++)
        advance_frames(&engine, 1, outL, outR);
    ASSERT(!p->playingCh1, "sweep: upward overflow silences the channel");
    ASSERT(p->ch1.frequency > fu0 && p->ch1.frequency < 2048, "sweep: frequency rose, stayed 11-bit");
    ASSERT(sq->status & CHN_ON, "sweep: software envelope keeps running while muted");

    /* pan_sweep with the pan bit is not a sweep. */
    m4a_engine_all_sound_off(&engine);
    m4a_engine_program_change(&engine, 0, 2);
    m4a_engine_note_on(&engine, 0, 60, 100);
    advance_frames(&engine, 1, outL, outR);
    ASSERT_EQ(sq->sweep, 0x08, "sweep: pan-bearing pan_sweep maps to inert NR10 value 8");
    ASSERT(!p->ch1.sweep.enable, "sweep: inert value disables the unit");
    int fi = p->ch1.frequency;
    advance_frames(&engine, 20, outL, outR);
    ASSERT_EQ(p->ch1.frequency, fi, "sweep: no sweeping on a pan-bearing voice");

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
    engine_init_booted(&engine, 44100.0f);
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
        engine_init_booted(&engine, 44100.0f);
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
        if (expectType[v] == M4A_SYNTH_TRIANGLE) {
            /* The first mixed frame keeps the 90-degree start: the first
             * output is the zero crossing, not the negative peak. */
            advance_frames(&engine, 1, outL, outR);
            int off = (engine.pcmDmaPeriod - (engine.pcmDmaCounter - 1)) * engine.pcmSamplesPerVBlank;
            if (engine.pcmDmaCounter - 1 <= 0) off = 0;
            ASSERT(engine.pcmBuffer[off] >= 0 && engine.pcmBuffer[off] < 8,
                   "gs synth: triangle's first mixed sample is near the zero crossing");
            ASSERT(engine.pcmBuffer[off + 8] > engine.pcmBuffer[off], "gs synth: triangle rises from the zero crossing");
        }
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

/* Golden Sun synth waveform shapes: drive the mixer frame by frame with a
 * hand-built channel so the generated waveforms can be checked
 * sample-by-sample against the GBA algorithm. */
static void test_golden_sun_synth_waveforms(void)
{
    printf("Testing Golden Sun synth waveform shapes...\n");

    M4AEngine e;
    m4a_engine_init(&e, 44100.0f);
    e.pcmDivFreq = 1;   /* frequency is the sample step; the phase step is step << 3 */
    const int spv = e.pcmSamplesPerVBlank;
    static int8_t frame[GBA_PCM_DMA_BUF_SIZE * 2];

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
        int32_t out[1024 + 256];
        int32_t maxV = -1000, minV = 1000;
        int n = 0;
        while (n < 1024) {
            mix_frame(&e, &ch, frame);
            for (int i = 0; i < spv && n < 1024 + 256; i++, n++) {
                out[n] = frame[i];
                if (out[n] > maxV) maxV = out[n];
                if (out[n] < minV) minV = out[n];
            }
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
        int high = 0, low = 0, other = 0, n = 0;
        while (n < 512) {
            mix_frame(&e, &ch, frame);
            for (int i = 0; i < spv && n < 512; i++, n++) {
                if (frame[i] == 32) high++;
                else if (frame[i] == -32) low++;
                else other++;
            }
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
        for (int f = 0; f < 10; f++) {
            mix_frame(&e, &ch, frame);
            for (int i = 0; i < spv; i++) {
                if (frame[i] > maxV) maxV = frame[i];
                if (frame[i] < minV) minV = frame[i];
            }
        }
        ASSERT(maxV > 30 && maxV <= 56, "gs saw: positive range");
        ASSERT(minV < -30 && minV >= -56, "gs saw: negative range");
    }
    m4a_engine_destroy(&e);
}

/* Reverse (TONEDATA_TYPE_REV / voice_directsound_alt) samples play from the
 * last sample to the first, interpolating toward the earlier sample, ignore
 * loops, and end when they run off the front -- the SoundMainRAM_Unk1
 * reverse path.  Driven frame by frame with a hand-built channel at
 * envelope volume 128 (value >> 1). */
/* What a hand-driven PCM channel at envelope volume 128 outputs for sample v. */
#define PCM_OUT(v) (((int32_t)(v) * 128) >> 8)

static void test_pcm_reverse(void)
{
    printf("Testing reverse-played samples...\n");

    M4AEngine e;
    m4a_engine_init(&e, 44100.0f);
    e.pcmDivFreq = 1;
    static int8_t frame[GBA_PCM_DMA_BUF_SIZE * 2];

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
        mix_frame(&e, &ch, frame);
        /* Unk1's first-mix setup: the channel is flagged and plays from
         * data + size backwards (one source sample per output). */
        ASSERT((ch.status & CHN_SPECIAL) != 0 || ch.status == 0, "reverse: Unk1 set the channel up");
        int ok = 1;
        for (int i = 0; i < N; i++)
            if (frame[i] != PCM_OUT(10 * (N - i)) || frame[GBA_PCM_DMA_BUF_SIZE + i] != frame[i]) ok = 0;
        ASSERT(ok, "reverse: plays the samples last-to-first");
        ASSERT_EQ(ch.status, 0, "reverse: stops at the front (loop ignored)");
        ASSERT_EQ(frame[N], 0, "reverse: silent once stopped (rest of the frame unmixed)");
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
        mix_frame(&e, &ch, frame);
        static const int32_t expect[6] = { 80, 75, 70, 65, 60, 55 };
        int ok = 1;
        for (int i = 0; i < 6; i++)
            if (frame[i] != PCM_OUT(expect[i])) ok = 0;
        ASSERT(ok, "reverse: half speed interpolates toward the earlier sample");
        /* 16 outputs cover the 8 samples; the 16th interpolates toward the
         * byte before the data (0 on the GBA: the header's last byte). */
        ASSERT_EQ(frame[14], PCM_OUT(10), "reverse: last sample");
        ASSERT_EQ(frame[15], PCM_OUT(5), "reverse: look-behind before the data reads 0");
        ASSERT_EQ(ch.status, 0, "reverse: stops after running off the front");
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
        mix_frame(&e, &ch, frame);
        /* Unk1 ignores the frequency for FIX voices: step 0x800000. */
        static const int32_t expect[6] = { 80, 70, 60, 50, 40, 30 };
        int ok = 1;
        for (int i = 0; i < 6; i++)
            if (frame[i] != PCM_OUT(expect[i])) ok = 0;
        ASSERT(ok, "reverse fix: one sample per output regardless of frequency");
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
            engine_init_booted(&engine, 44100.0f);
            m4a_engine_set_voicegroup(&engine, voices);
            m4a_engine_program_change(&engine, 0, (uint8_t)v);
            m4a_engine_cc(&engine, 0, 7, 127);
            m4a_engine_note_on(&engine, 0, 60, 127);
            float outL[512], outR[512];
            /* Let the note start (next frame) and reach the output through
             * the DMA buffer, then compare an early and a late output of a
             * block: the ramp covers ~4096 source samples, so 512 outputs
             * never reach the other end at any of these rates. */
            for (int k = 0; k < 4; k++)
                m4a_engine_process(&engine, outL, outR, 512);
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
    m4a_engine_destroy(&e);
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
        engine_init_booted(&engine, 44100.0f);
        engine.portamentoEnabled = true;
        m4a_engine_set_voicegroup(&engine, voices);
        m4a_engine_cc(&engine, 0, 0x5, 2);  /* portamento on: 2-tick glide */

        m4a_engine_program_change(&engine, 0, cases[c].from);
        m4a_engine_note_on(&engine, 0, 60, 100);
        advance_frames(&engine, 1, outL, outR);  /* the note's first mixed frame */
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
        ASSERT(second->currentPointer == wd->data, "rev legato: pointer starts at the data (Unk1 flips it on the first mix)");
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
        engine_init_booted(&engine, 44100.0f);
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
    wd->freq = 13379 << 10;
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
    M4AEngine e;
    m4a_engine_init(&e, 44100.0f);
    e.pcmDivFreq = 1;
    static int8_t frame[GBA_PCM_DMA_BUF_SIZE * 2];

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
        mix_frame(&e, &ch, frame);
        int ok = 1, wrapOk = 1;
        for (int i = 0; i < SIZE; i++)
            if (frame[i] != PCM_OUT(decoded[i])) ok = 0;
        ASSERT(ok, "dpcm: forward decode matches the delta table");
        for (int i = 0; i < 2 * (SIZE - LOOP); i++)
            if (frame[SIZE + i] != PCM_OUT(decoded[LOOP + i % (SIZE - LOOP)])) wrapOk = 0;
        ASSERT(wrapOk, "dpcm: forward loop wraps to loopStart");
        ASSERT((ch.status & CHN_ON) != 0, "dpcm: looped channel keeps playing");
        ASSERT((ch.status & CHN_SPECIAL) != 0, "dpcm: Unk1 flagged the channel");
        ASSERT_EQ((int)(ch.currentPointer - wd->data), (int)(LOOP + (224 - SIZE) % (SIZE - LOOP)),
                  "dpcm: currentPointer is a sample index after the frame");
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
        mix_frame(&e, &ch, frame);
        int ok = 1;
        for (int i = 0; i < 2 * SIZE - 2; i++) {
            int32_t s0 = decoded[i / 2], s1 = decoded[i / 2 + 1];
            int32_t expect = (i % 2 == 0) ? s0 : s0 + (((s1 - s0) * 0x400000) >> 23);
            if (frame[i] != PCM_OUT(expect)) ok = 0;
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
        mix_frame(&e, &ch, frame);
        /* Unk1's FIX step is 0x800000 whatever the frequency. */
        int ok = 1;
        for (int i = 0; i < SIZE; i++)
            if (frame[i] != PCM_OUT(decoded[i])) ok = 0;
        ASSERT(ok, "dpcm fix: one decoded sample per output regardless of frequency");
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
        mix_frame(&e, &ch, frame);
        int ok = 1;
        for (int i = 0; i < SIZE; i++)
            if (frame[i] != PCM_OUT(decoded[SIZE - 1 - i])) ok = 0;
        ASSERT(ok, "dpcm reverse: plays the decoded samples last-to-first");
        ASSERT_EQ(ch.status, 0, "dpcm reverse: stops at the front (loop ignored)");
    }

    /* The decoding buffer is shared: a second compressed channel decoding
     * a different block replaces it, and the first channel's cached block
     * tag (xpi) does not notice -- exactly the GBA's sDecodingBuffer. */
    {
        M4APCMChannel a, b;
        memset(&a, 0, sizeof(a));
        memset(&b, 0, sizeof(b));
        m4a_pcm_channel_start(&a, wd, VOICE_CRY);
        m4a_pcm_channel_start(&b, wd, VOICE_CRY);
        a.status = b.status = CHN_ENV_SUSTAIN;
        a.frequency = b.frequency = 0x800000;
        a.envelopeVolumeLeft = a.envelopeVolumeRight = 128;
        b.envelopeVolumeLeft = b.envelopeVolumeRight = 128;
        b.currentPointer = wd->data + 64;   /* start offset: block 1 */
        b.count = SIZE - 64;
        mix_frame(&e, &a, frame);          /* leaves block 1 (xpi 1) in the buffer */
        ASSERT_EQ((int)a.xpi, 1, "dpcm shared: channel a ends in block 1");
        mix_frame(&e, &b, frame);          /* decodes block 1 too -- no change */
        ASSERT_EQ((int)b.xpi, 1, "dpcm shared: channel b decoded block 1");
        ASSERT_EQ(frame[0], PCM_OUT(decoded[64]), "dpcm shared: b plays block 1 from its start");
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
        engine_init_booted(&engine, 44100.0f);
        m4a_engine_set_voicegroup(&engine, voices);
        m4a_engine_program_change(&engine, 0, 0);
        m4a_engine_cc(&engine, 0, 7, 127);
        m4a_engine_note_on(&engine, 0, 60, 127);
        float outL[256], outR[256];
        advance_frames(&engine, 1, outL, outR);   /* the note's first mixed frame */
        M4APCMChannel *ch = &engine.pcmChannels[0];
        ASSERT((ch->status & CHN_SPECIAL) != 0, "dpcm engine: the channel went through Unk1");
        ASSERT((int)(ch->currentPointer - wd->data) > 0 && (int)(ch->currentPointer - wd->data) < SIZE,
               "dpcm engine: position is a sample index inside the data");
        m4a_engine_destroy(&engine);
    }

    free(wd);
    m4a_engine_destroy(&e);
}

/* The reversed DPCM walk (SoundMainRAM_Unk1 _081DD3C0, cry_reverse /
 * voice_directsound_compressed_reverse, type 0x30 = CMP | REV) against
 * make_dpcm_wave's decoded array played backwards: the first frame starts
 * at the last sample (Unk1's CHN_SPECIAL setup flips currentPointer to
 * data + size and reads it back as the index), a frame boundary that
 * falls on a DPCM block boundary resumes in the block below, the
 * interpolation at half speed looks behind (toward the earlier sample,
 * across the block boundary), and the walk ends through the count path
 * at sample 0 whose look-behind is "block -1" (the port decodes a zero
 * block there; the GBA's address wraps out of the cartridge space -- see
 * docs/ACCURACY.md, "Known residuals and limitations"). */
static void test_pcm_dpcm_reverse_walk(void)
{
    printf("Testing the reversed DPCM walk (REV | CMP)...\n");
    M4AEngine e;
    m4a_engine_init(&e, 44100.0f);
    e.pcmDivFreq = 1;
    static int8_t frame[GBA_PCM_DMA_BUF_SIZE * 2];

    /* 288 = 224 + 64 samples at one sample per output: the first frame
     * plays decoded[287 .. 64] and the second frame starts exactly at the
     * block-1 / block-0 boundary, decoded[63 .. 0], then stops. */
    {
        enum { SIZE = 288 };
        static int8_t decoded[SIZE + 1 + M4A_DPCM_BLOCK_SAMPLES];
        WaveData *wd = make_dpcm_wave(SIZE, 0, 0, decoded);
        M4APCMChannel ch;
        memset(&ch, 0, sizeof(ch));
        m4a_pcm_channel_start(&ch, wd, VOICE_CRY_REVERSE);
        ch.status = CHN_ENV_SUSTAIN;
        ch.frequency = 0x800000;
        ch.envelopeVolumeLeft = 128;
        ch.envelopeVolumeRight = 128;
        ASSERT_EQ((int)(ch.currentPointer - wd->data), 0, "dpcm reverse walk: ply_note leaves the pointer at the data start");
        ASSERT_EQ(ch.count, SIZE, "dpcm reverse walk: count = size");
        mix_frame(&e, &ch, frame);
        int ok = 1, first = -1;
        const int spv = e.pcmSamplesPerVBlank;   /* 224 at the GBA's 13379 Hz */
        ASSERT_EQ(spv, 224, "dpcm reverse walk: 224 samples per frame");
        for (int i = 0; i < spv; i++)
            if (frame[i] != PCM_OUT(decoded[SIZE - 1 - i]) || frame[GBA_PCM_DMA_BUF_SIZE + i] != frame[i]) { ok = 0; if (first < 0) first = i; }
        if (!ok) printf("  frame 1 first mismatch at output %d: %d, expected %d\n", first, frame[first], PCM_OUT(decoded[SIZE - 1 - first]));
        ASSERT(ok, "dpcm reverse walk: frame 1 plays decoded[287 .. 64], the last block first");
        ASSERT((ch.status & CHN_SPECIAL) != 0, "dpcm reverse walk: Unk1 flagged the channel");
        ASSERT_EQ(ch.count, SIZE - spv, "dpcm reverse walk: 64 samples left after frame 1");
        ASSERT_EQ((int)(ch.currentPointer - wd->data), 64, "dpcm reverse walk: the stored index after frame 1 is 64 (i3 + 2 of the walk's 62: the next frame re-reads 63 and 62)");
        ASSERT_EQ((int)ch.xpi, 0, "dpcm reverse walk: block 0 is the decoded one after frame 1 (the look-behind of sample 64 is 63)");
        mix_frame(&e, &ch, frame);
        ok = 1; first = -1;
        for (int i = 0; i < 64; i++)
            if (frame[i] != PCM_OUT(decoded[63 - i])) { ok = 0; if (first < 0) first = i; }
        if (!ok) printf("  frame 2 first mismatch at output %d: %d, expected %d\n", first, frame[first], PCM_OUT(decoded[63 - first]));
        ASSERT(ok, "dpcm reverse walk: frame 2 resumes at decoded[63] across the frame / block boundary and reaches decoded[0]");
        ASSERT_EQ(ch.status, 0, "dpcm reverse walk: the count path stops the channel after sample 0");
        ASSERT_EQ(frame[64], 0, "dpcm reverse walk: nothing mixed past the stop");
        ASSERT_EQ(frame[GBA_PCM_DMA_BUF_SIZE + 64], 0, "dpcm reverse walk: nothing mixed past the stop (left)");
        free(wd);
    }

    /* Half speed on 100 samples: even outputs are decoded[99 - i/2], odd
     * ones interpolate toward the earlier sample (r1 = earlier - current,
     * (r9 * r1) >> 23 with r9 = 0x400000), the pairs crossing the block
     * boundary at 64 / 63 like any other; output 199 interpolates from
     * decoded[0] toward the look-behind of sample 0, which the port reads
     * as 0 (a zero block for index -1).  The channel stops on the next
     * crossing. */
    {
        enum { SIZE = 100 };
        static int8_t decoded[SIZE + 1 + M4A_DPCM_BLOCK_SAMPLES];
        WaveData *wd = make_dpcm_wave(SIZE, 0, 0, decoded);
        M4APCMChannel ch;
        memset(&ch, 0, sizeof(ch));
        m4a_pcm_channel_start(&ch, wd, VOICE_CRY_REVERSE);
        ch.status = CHN_ENV_SUSTAIN;
        ch.frequency = 0x400000;
        ch.envelopeVolumeLeft = 128;
        ch.envelopeVolumeRight = 128;
        mix_frame(&e, &ch, frame);
        int ok = 1, first = -1, boundaryOk = 1;
        for (int i = 0; i < 2 * SIZE - 2; i++) {
            int32_t s0 = decoded[SIZE - 1 - i / 2], s1 = decoded[SIZE - 2 - i / 2];
            int32_t expect = (i % 2 == 0) ? s0 : s0 + (((s1 - s0) * 0x400000) >> 23);
            if (frame[i] != PCM_OUT(expect)) { ok = 0; if (first < 0) first = i; if (i / 2 == SIZE - 1 - 64 || i / 2 == SIZE - 1 - 63) boundaryOk = 0; }
        }
        if (!ok) printf("  half speed first mismatch at output %d\n", first);
        ASSERT(ok, "dpcm reverse walk: half speed interpolates toward the earlier decoded sample");
        ASSERT(boundaryOk, "dpcm reverse walk: the pair across the block boundary (64 -> 63) interpolates like any other");
        ASSERT_EQ(frame[2 * SIZE - 2], PCM_OUT(decoded[0]), "dpcm reverse walk: output 198 is decoded[0]");
        ASSERT_EQ(frame[2 * SIZE - 1], PCM_OUT(decoded[0] + (((0 - decoded[0]) * 0x400000) >> 23)),
                  "dpcm reverse walk: output 199 interpolates from decoded[0] toward the zero look-behind of block -1");
        ASSERT_EQ(ch.status, 0, "dpcm reverse walk: stops on the crossing after sample 0");
        ASSERT_EQ(frame[2 * SIZE], 0, "dpcm reverse walk: silent after the stop");
        free(wd);
    }
    m4a_engine_destroy(&e);
}

/* ------------------------------------------------------------------------
 * The SoundMainRAM mixer against hand-computed arithmetic (m4a_mixer.c)
 * ------------------------------------------------------------------------ */

static WaveData *ramp_wave(int n, int looped, uint32_t loopStart)
{
    WaveData *wd = calloc(1, sizeof(WaveData) + n + 1);
    wd->status = looped ? 0x4000 : 0;
    wd->loopStart = loopStart;
    wd->size = (uint32_t)n;
    wd->freq = 13379 << 10;
    wd->data = (int8_t *)((uint8_t *)wd + sizeof(WaveData));
    for (int i = 0; i < n; i++)
        wd->data[i] = (int8_t)(10 * (i + 1));
    wd->data[n] = 0;   /* the byte after the data: wav2agb's padding */
    return wd;
}

/* The HQ mixer (plugin/m4a_hq_mixer.c, docs/HQ-MIXER-ACCURACY.md): each
 * expected value is worked by hand from the assembly in the comments. */
static void test_hq_mixer(void)
{
    printf("Testing the HQ mixer (ipatix rev 4.0) arithmetic...\n");
    M4AEngine e;
    m4a_engine_init(&e, 44100.0f);
    m4a_engine_set_hq_mixer(&e, true);
    e.pcmDivFreq = 1;
    e.pcmSamplesPerVBlank = 8;
    static uint32_t buf[64];

    /* C_channel_vol_calc: v = env * (mv + 1) = 255 * 13 = 3315;
     * L = 127 * 3315 >> 13 = 51, R = 64 * 3315 >> 13 = 25. */
    {
        M4APCMChannel ch;
        memset(&ch, 0, sizeof(ch));
        ch.envelopeVolume = 255; ch.leftVolume = 127; ch.rightVolume = 64;
        m4a_hq_channel_volumes(&ch, 12);
        ASSERT_EQ(ch.envelopeVolumeLeft, 51, "hq vol: left = (leftVolume * env * (mv + 1)) >> 13");
        ASSERT_EQ(ch.envelopeVolumeRight, 25, "hq vol: right likewise");
    }

    /* C_channel_init: a REV voice's pointer becomes data + count (size -
     * offset); a sample whose WaveData.type bit 0 is set OR-s CMP into the
     * voice type; sampleStor is cleared. */
    {
        WaveData *wd = ramp_wave(12, 0, 0);
        M4APCMChannel ch;
        memset(&ch, 0, sizeof(ch));
        m4a_pcm_channel_start_at(&ch, wd, VOICE_DIRECTSOUND | VOICE_TYPE_REV, 2);
        ch.sampleStor = 7;
        m4a_hq_channel_init(&ch);
        ASSERT(ch.currentPointer == wd->data + 10, "hq init: reverse pointer = data + (size - offset)");
        ASSERT_EQ(ch.count, 10, "hq init: count = size - offset");
        ASSERT_EQ(ch.sampleStor, 0, "hq init: sampleStor cleared");
        wd->type = 1;
        m4a_pcm_channel_start(&ch, wd, VOICE_DIRECTSOUND);
        m4a_hq_channel_init(&ch);
        ASSERT(ch.type & VOICE_TYPE_CMP, "hq init: compression follows the sample's flag");
        free(wd);
    }

    /* The fast loop's < 1.0 block at half speed on an unlooped 12-sample
     * ramp 10..120 (count 12 > advance 4), sampleStor 0 from init, volumes
     * 2 / 2 halved to 1 / 1 (ceil): sample 0 interpolates from the stored
     * base 0 (output 0, not mixed), then 10, 20, 30, ... at double scale;
     * the state after: fw 0, count 8, pointer data + 4, base 40. */
    {
        WaveData *wd = ramp_wave(12, 0, 0);
        M4APCMChannel ch;
        memset(&ch, 0, sizeof(ch));
        m4a_pcm_channel_start(&ch, wd, VOICE_DIRECTSOUND);
        m4a_hq_channel_init(&ch);
        ch.status = CHN_ENV_SUSTAIN;
        ch.frequency = 0x400000;
        ch.envelopeVolumeLeft = ch.envelopeVolumeRight = 2;
        memset(buf, 0, sizeof(buf));
        ASSERT(m4a_hq_mix_channel(&e, &ch, buf), "hq fast: the path is ported");
        int ok = 1;
        for (int i = 0; i < 8; i++)
            if (buf[i] != ((uint32_t)(10 * i) << 16 | (uint32_t)(10 * i))) ok = 0;
        ASSERT(ok, "hq fast: 0, 10, 20 .. 70 at double scale, packed L << 16 | R");
        ASSERT_EQ(ch.fw, 0, "hq fast: fw after the frame");
        ASSERT_EQ(ch.count, 8, "hq fast: count -= advance");
        ASSERT(ch.currentPointer == wd->data + 4, "hq fast: pointer += advance");
        ASSERT_EQ(ch.sampleStor, 40, "hq fast: the base sample carried to the next frame");
        free(wd);
    }

    /* The mulne quirk: a flat run of -10 at half speed with the base already
     * -10 (diff 0).  Each advance lands on diff 0 and skips the next
     * sample's multiply, so r9 keeps the previous output (-20), whose asr
     * #22 is -1: -21 instead of -20.  The first sample of each 8-sample
     * iteration always multiplies (the loop's subs r8).  Two iterations:
     * -20 -20 -21 -20 -21 -20 -21 -20 | -20 -20 -21 -20 -21 -20 -21 -20.
     * Packed at volume 1 / 1 the negative product borrows: -20 ->
     * 0xFFEBFFEC, -21 -> 0xFFEAFFEB. */
    {
        static WaveData wd;
        static int8_t flat[40];
        memset(flat, -10, sizeof(flat));
        wd.size = 32; wd.data = flat;
        M4APCMChannel ch;
        memset(&ch, 0, sizeof(ch));
        ch.wav = &wd; ch.currentPointer = flat; ch.count = 32; ch.sampleStor = -10;
        ch.status = CHN_ENV_SUSTAIN; ch.frequency = 0x400000;
        ch.envelopeVolumeLeft = ch.envelopeVolumeRight = 2;
        e.pcmSamplesPerVBlank = 16;
        memset(buf, 0, sizeof(buf));
        m4a_hq_mix_channel(&e, &ch, buf);
        static const uint32_t want[16] = {
            0xFFEBFFECu, 0xFFEBFFECu, 0xFFEAFFEBu, 0xFFEBFFECu, 0xFFEAFFEBu, 0xFFEBFFECu, 0xFFEAFFEBu, 0xFFEBFFECu,
            0xFFEBFFECu, 0xFFEBFFECu, 0xFFEAFFEBu, 0xFFEBFFECu, 0xFFEAFFEBu, 0xFFEBFFECu, 0xFFEAFFEBu, 0xFFEBFFECu };
        int ok = 1;
        for (int i = 0; i < 16; i++) if (buf[i] != want[i]) ok = 0;
        ASSERT(ok, "hq fast: diff-0 skip gives -21 after an advance, never at an iteration's first sample");
        e.pcmSamplesPerVBlank = 8;
    }

    /* Volume zero: the channel is skipped for the frame, nothing advances. */
    {
        WaveData *wd = ramp_wave(12, 0, 0);
        M4APCMChannel ch;
        memset(&ch, 0, sizeof(ch));
        m4a_pcm_channel_start(&ch, wd, VOICE_DIRECTSOUND);
        m4a_hq_channel_init(&ch);
        ch.status = CHN_ENV_SUSTAIN; ch.frequency = 0x400000; ch.fw = 123; ch.sampleStor = 9;
        memset(buf, 0x11, sizeof(buf));
        ASSERT(m4a_hq_mix_channel(&e, &ch, buf), "hq vol0: skipped, not unported");
        ASSERT(buf[0] == 0x11111111u && ch.fw == 123 && ch.count == 12 && ch.currentPointer == wd->data
               && ch.sampleStor == 9, "hq vol0: buffer and position untouched");
        free(wd);
    }

    /* The unbuffered loop at step 1.0 on an unlooped 4-sample ramp (count 4
     * <= advance 8): 0 (from the stored base 0), 20, 40, 60, then count
     * reaches 0 at the fourth advance: status 0, the rest of the frame
     * untouched, nothing stored back. */
    {
        WaveData *wd = ramp_wave(4, 0, 0);
        M4APCMChannel ch;
        memset(&ch, 0, sizeof(ch));
        m4a_pcm_channel_start(&ch, wd, VOICE_DIRECTSOUND);
        m4a_hq_channel_init(&ch);
        ch.status = CHN_ENV_SUSTAIN; ch.frequency = 0x800000;
        ch.envelopeVolumeLeft = ch.envelopeVolumeRight = 2;
        memset(buf, 0, sizeof(buf));
        m4a_hq_mix_channel(&e, &ch, buf);
        ASSERT(buf[0] == 0 && buf[1] == 0x00140014u && buf[2] == 0x00280028u && buf[3] == 0x003C003Cu
               && buf[4] == 0, "hq unbuffered: 0, 20, 40, 60 then the end");
        ASSERT_EQ(ch.status, 0, "hq unbuffered: the channel stops at the sample's end");
        ASSERT(ch.count == 4 && ch.currentPointer == wd->data && ch.fw == 0, "hq unbuffered: no state stored at the end");
        free(wd);
    }

    /* The same ramp looped over all 4 samples: at count 0 the pointer steps
     * back by the loop length and count += 4, the base continuing from the
     * last sample (40 -> 80 at double scale), then 20, 40, 60 again; after
     * the frame fw 0, base 40, count 4, pointer data. */
    {
        WaveData *wd = ramp_wave(4, 1, 0);
        M4APCMChannel ch;
        memset(&ch, 0, sizeof(ch));
        m4a_pcm_channel_start(&ch, wd, VOICE_DIRECTSOUND);
        m4a_hq_channel_init(&ch);
        ch.status = CHN_ENV_SUSTAIN | CHN_LOOP; ch.frequency = 0x800000;
        ch.envelopeVolumeLeft = ch.envelopeVolumeRight = 2;
        memset(buf, 0, sizeof(buf));
        m4a_hq_mix_channel(&e, &ch, buf);
        static const uint32_t want[8] = { 0, 0x00140014u, 0x00280028u, 0x003C003Cu, 0x00500050u, 0x00140014u, 0x00280028u, 0x003C003Cu };
        int ok = 1;
        for (int i = 0; i < 8; i++) if (buf[i] != want[i]) ok = 0;
        ASSERT(ok, "hq unbuffered: the loop wraps relative to the pointer");
        ASSERT(ch.status == (CHN_ENV_SUSTAIN | CHN_LOOP) && ch.fw == 0 && ch.sampleStor == 40 && ch.count == 4
               && ch.currentPointer == wd->data, "hq unbuffered: state after a wrapped frame");
        free(wd);
    }

    /* The downsampler: int16 halves to int8 (>> 7) with the 7-bit residual
     * carried (0x400000 twice -> 0, 1), saturation to 31 bits on either
     * side (0x7FFF0000 -> 0x7F with residual 0x7F; 0x80010000 -> 0x80), the
     * right half from w << 16, the residuals kept across frames. */
    {
        memset(e.pcmBuffer, 0, sizeof(e.pcmBuffer));
        e.hqNoiseShapeLeft = e.hqNoiseShapeRight = 0;
        e.reverb.amount = 0;
        static const uint32_t words[8] = { 0x00400000u, 0x00400000u, 0x7FFF0000u, 0x00800000u,
                                           0x80010000u, 0x00000040u, 0x0000FFC0u, 0x00800000u };
        memcpy(e.hqBuffer, words, sizeof(words));
        m4a_hq_downsample(&e, 0, 7);
        const int8_t *A = e.pcmBuffer, *B = e.pcmBuffer + GBA_PCM_DMA_BUF_SIZE;
        ASSERT(B[0] == 0 && B[1] == 1, "hq ds: left 64 + 64 carries the residual into a 1");
        ASSERT_EQ(B[2], 127, "hq ds: positive saturation");
        ASSERT_EQ(B[3], 1, "hq ds: 0x7F residual + 128 -> 1 (0x7F0000 + 0x800000 = 0xFF0000: bits 23..30 = 1)");
        ASSERT_EQ(B[4], -128, "hq ds: negative saturation");
        ASSERT(A[5] == 0 && A[6] == 0, "hq ds: right 64 then -64: 0x400000 + 0xFFC00000 = 0, byte 0");
        ASSERT(A[0] == 0 && A[2] == 0 && A[3] == 0, "hq ds: the right side ignores the left half");
        ASSERT_EQ(e.hqNoiseShapeLeft, 0x00, "hq ds: left residual after the frame");
        ASSERT_EQ(e.hqNoiseShapeRight, 0x00, "hq ds: right residual after the frame");
        int ok = 1;
        for (int i = 0; i < 8; i++) if (e.hqBuffer[i] != 0) ok = 0;
        ASSERT(ok, "hq ds: reverb 0 seeds the next frame with zeros");
        /* residual across frames: 0x200000 per sample reaches 0x800000 every 4th */
        for (int i = 0; i < 8; i++) e.hqBuffer[i] = 0x00200000u;
        m4a_hq_downsample(&e, 8, 6);
        ASSERT(B[8] == 0 && B[9] == 0 && B[10] == 0 && B[11] == 1 && B[15] == 1, "hq ds: a 1/4 LSB signal yields a 1 every 4th sample");
        for (int i = 0; i < 8; i++) e.hqBuffer[i] = 0x00200000u;
        e.hqNoiseShapeLeft = 0x60;    /* three quarters already accumulated */
        m4a_hq_downsample(&e, 16, 5);
        ASSERT(B[16] == 1 && B[17] == 0, "hq ds: the residual byte carries the fraction into the next frame");
    }

    /* The reverb seed: taps next A, next B, this slot's old B, old A per
     * sample, times reverb >> 2 in both halves (12 for amount 50): a sum
     * of 10 -> 0x00780078; a sum of -1 -> 0xFFF3FFF4 (the borrow).  With
     * the buffer-position hint 2 the next slot is the ring start. */
    {
        int8_t *A = e.pcmBuffer, *B = e.pcmBuffer + GBA_PCM_DMA_BUF_SIZE;
        memset(e.pcmBuffer, 0, sizeof(e.pcmBuffer));
        memset(e.hqBuffer, 0, sizeof(e.hqBuffer));
        e.hqNoiseShapeLeft = e.hqNoiseShapeRight = 0;
        e.reverb.amount = 50;
        /* the slot at 48 (counter 2: the last one), next = ring start */
        A[0] = 3; B[0] = 4; B[48] = 5; A[48] = -2;        /* sample 0: 10 */
        A[1] = 0; B[1] = 0; B[49] = -1; A[49] = 0;        /* sample 1: -1 */
        m4a_hq_downsample(&e, 48, 2);
        ASSERT_EQ(e.hqBuffer[0], 0x00780078u, "hq seed: sum 10 * 12 in both halves");
        ASSERT_EQ(e.hqBuffer[1], 0xFFF3FFF4u, "hq seed: a negative sum borrows from the high half");
        ASSERT(A[48] == 0 && B[48] == 0 && B[49] == 0, "hq seed: the slot was overwritten with the (zero) output after the taps were read");
        ASSERT(A[0] == 3, "hq seed: the next slot is only read");
    }

    /* The frame: an unported path (FIX) leaves the channel untouched and
     * is counted; a ported one advances. */
    {
        WaveData *wd = ramp_wave(12, 0, 0);
        m4a_engine_set_hq_mixer(&e, true);
        e.pcmDivFreq = 1; e.pcmSamplesPerVBlank = 8;
        M4APCMChannel *ch = &e.pcmChannels[0];
        memset(ch, 0, sizeof(*ch));
        m4a_pcm_channel_start(ch, wd, VOICE_DIRECTSOUND | VOICE_TYPE_FIX);
        ch->frequency = 0x400000; ch->leftVolume = ch->rightVolume = 127;
        ch->attack = 255; ch->decay = 255; ch->sustain = 255;
        m4a_hq_sound_main_ram(&e, 0, 7);
        ASSERT_EQ(e.hqUnportedChannelFrames, 1, "hq frame: a FIX channel is counted as unported");
        ASSERT(ch->count == 12 && ch->currentPointer == wd->data, "hq frame: and left untouched");
        ch->type = VOICE_DIRECTSOUND;
        m4a_hq_sound_main_ram(&e, 8, 6);
        ASSERT_EQ(e.hqUnportedChannelFrames, 1, "hq frame: a ported channel is not counted");
        ASSERT_EQ(ch->count, 8, "hq frame: and it advanced (envelope 255, volumes 51 / 51)");
        ASSERT_EQ(ch->envelopeVolumeLeft, 51, "hq frame: 127 * 255 * 13 >> 13 = 51");
        free(wd);
    }
}

static void test_mixer_arithmetic(void)
{
    printf("Testing the SoundMainRAM mixer arithmetic...\n");

    /* The packed word: mul / bic 0xFF0000 / add ror 8.  Four samples at
     * volume 128 land little-endian as (s * 128) >> 8; a pre-loaded word
     * (the reverb base) adds lane by lane; the lanes wrap at 8 bits. */
    {
        uint32_t w = 0;
        static const int32_t s[4] = { 10, -20, 30, -40 };
        for (int i = 0; i < 4; i++)
            w = m4a_mixer_acc(w, 128u << 16, s[i]);
        ASSERT_EQ(w, 0xEC0FF605u, "ror8: four lanes little-endian, floor(s*v/256)");
        w = 0x01020304u;
        for (int i = 0; i < 4; i++)
            w = m4a_mixer_acc(w, 128u << 16, s[i]);
        ASSERT_EQ(w, 0xED11F909u, "ror8: adds onto the loaded word lane by lane");
        /* 127 * 255 >> 8 = 126; twice in one lane = 252 = -4 as int8. */
        w = 0;
        for (int pass = 0; pass < 2; pass++)
            for (int i = 0; i < 4; i++)
                w = m4a_mixer_acc(w, 255u << 16, 127);
        ASSERT_EQ(w, 0xFCFCFCFCu, "ror8: lanes wrap at 8 bits (126 + 126 = -4)");
        /* bic 0xFF0000: the product's low byte never carries into the lane. */
        w = m4a_mixer_acc(0xFF000000u, 1u << 16, 1);   /* 1*1 = 1 -> high byte 0 */
        ASSERT_EQ(w, 0x00FF0000u, "ror8: a product below 256 contributes 0 to its lane");
    }

    M4AEngine e;
    m4a_engine_init(&e, 44100.0f);
    e.pcmDivFreq = 1;
    static int8_t frame[GBA_PCM_DMA_BUF_SIZE * 2];
    const int spv = e.pcmSamplesPerVBlank;

    /* One looped 8-sample ramp, FIX type: the frame is the ramp repeated,
     * both sides at their own volume, count carried across the frame. */
    {
        WaveData *wd = ramp_wave(8, 1, 0);
        M4APCMChannel ch;
        memset(&ch, 0, sizeof(ch));
        m4a_pcm_channel_start(&ch, wd, VOICE_DIRECTSOUND | VOICE_TYPE_FIX);
        ch.status = CHN_ENV_SUSTAIN | CHN_LOOP;
        ch.frequency = 12345;   /* ignored by the FIX loop */
        ch.envelopeVolumeRight = 128;
        ch.envelopeVolumeLeft = 64;
        mix_frame(&e, &ch, frame);
        int ok = 1;
        for (int i = 0; i < spv; i++) {
            int v = 10 * (i % 8 + 1);
            if (frame[i] != (v * 128) >> 8 || frame[GBA_PCM_DMA_BUF_SIZE + i] != (v * 64) >> 8)
                ok = 0;
        }
        ASSERT(ok, "fix loop: the ramp repeats through the frame at each side's volume");
        ASSERT_EQ(ch.count, 8 - spv % 8, "fix loop: count after the frame");
        ASSERT(ch.currentPointer == wd->data + spv % 8, "fix loop: pointer after the frame");
        free(wd);
    }

    /* The interpolating loop at half speed on an unlooped 8-sample ramp:
     * 10, 15, 20, ... 80, then the look-ahead toward the byte after the
     * data (0): 40; the channel stops there, mid-word, and the rest of the
     * frame is left untouched (nothing else was mixed). */
    {
        WaveData *wd = ramp_wave(8, 0, 0);
        M4APCMChannel ch;
        memset(&ch, 0, sizeof(ch));
        m4a_pcm_channel_start(&ch, wd, VOICE_DIRECTSOUND);
        ch.status = CHN_ENV_SUSTAIN;
        ch.frequency = 0x400000;
        ch.envelopeVolumeRight = ch.envelopeVolumeLeft = 128;
        memset(frame, 3, sizeof(frame));
        m4a_mixer_mix_channel(&e, &ch, frame);
        int ok = 1;
        for (int i = 0; i < 15; i++)
            if (frame[i] != 3 + ((10 + 5 * i) * 128 >> 8)) ok = 0;
        ASSERT(ok, "interp: half speed interpolates between neighbours on top of the base");
        ASSERT_EQ(frame[15], 3 + 20, "interp: last output looks ahead to the padding byte (0)");
        ASSERT_EQ(frame[16], 3, "interp: the frame past the stop is untouched");
        ASSERT_EQ(frame[GBA_PCM_DMA_BUF_SIZE + 15], 3 + 20, "interp: left side mixed alike");
        ASSERT_EQ(ch.status, 0, "interp: channel off when count runs out");
        free(wd);
    }

    /* The interpolating loop crossing a loop end: 8-sample ramp looping
     * from 6 at 1.5 samples per output.  Outputs: pos 0 (10), 1.5 (25),
     * 3 (40), 4.5 (55), 6 (70), 7.5 (80 + (0-80)/2 = 40, the look-ahead
     * reads the padding byte at the loop end), then the wrap: count 8 -
     * 1 - 1 - 2 - 1 - 2 ... let the mixer count; the position after the
     * wrap is loopStart + (advance - count). */
    {
        WaveData *wd = ramp_wave(8, 1, 6);
        M4APCMChannel ch;
        memset(&ch, 0, sizeof(ch));
        m4a_pcm_channel_start(&ch, wd, VOICE_DIRECTSOUND);
        ch.status = CHN_ENV_SUSTAIN | CHN_LOOP;
        ch.frequency = 0xC00000;
        ch.envelopeVolumeRight = ch.envelopeVolumeLeft = 128;
        mix_frame(&e, &ch, frame);
        static const int expect[8] = { 10, 25, 40, 55, 70, 40, /* wrap: 7.5 + 1.5 = 9 -> 6 + 1 */ 80, /* 10.5 -> 6 + 0.5 */ 75 };
        int ok = 1;
        for (int i = 0; i < 8; i++)
            if (frame[i] != (expect[i] * 128) >> 8) ok = 0;
        ASSERT(ok, "interp loop: wraps to loopStart + overshoot and keeps interpolating");
        ASSERT((ch.status & CHN_ON) != 0, "interp loop: still playing");
        free(wd);
    }

    /* Two full-volume channels on one frame wrap the lane sum: 100 and
     * 100 at volume 255 give 99 + 99 = 198 = -58. */
    {
        WaveData *wd = calloc(1, sizeof(WaveData) + 8 + 1);
        wd->status = 0x4000; wd->size = 8; wd->freq = 13379 << 10;
        wd->data = (int8_t *)((uint8_t *)wd + sizeof(WaveData));
        memset(wd->data, 100, 8);
        M4APCMChannel a, b;
        memset(&a, 0, sizeof(a));
        m4a_pcm_channel_start(&a, wd, VOICE_DIRECTSOUND | VOICE_TYPE_FIX);
        a.status = CHN_ENV_SUSTAIN | CHN_LOOP;
        a.envelopeVolumeRight = a.envelopeVolumeLeft = 255;
        b = a;
        mix_frame(&e, &a, frame);
        ASSERT_EQ(frame[0], 99, "wrap: one channel at 100 * 255 >> 8");
        m4a_mixer_mix_channel(&e, &b, frame);
        ASSERT_EQ(frame[0], -58, "wrap: the second channel wraps the 8-bit lane");
        ASSERT_EQ(frame[GBA_PCM_DMA_BUF_SIZE + 5], -58, "wrap: both sides");
        free(wd);
    }

    /* SoundMainRAM_Reverb: (a + b + c + d) * reverb >> 9, +1 when bit 7 of
     * the result is set, written to both sides of the current slot; the
     * taps are the slot's previous contents and the next slot's (the
     * buffer start when pcmDmaCounter == 2). */
    {
        M4AEngine r;
        m4a_engine_init(&r, 44100.0f);
        r.reverb.amount = 64;
        int off = 2 * r.pcmSamplesPerVBlank;
        int8_t *A = r.pcmBuffer, *B = r.pcmBuffer + GBA_PCM_DMA_BUF_SIZE;
        A[off] = -10; B[off] = -10; A[off + 224] = -10; B[off + 224] = -10;   /* -40 * 64 >> 9 = -5 -> -4 */
        A[off + 1] = 20; B[off + 1] = 20; A[off + 225] = 0; B[off + 225] = 0;  /* 40 * 64 >> 9 = 5 */
        A[off + 2] = -1; B[off + 2] = 0;  A[off + 226] = 0; B[off + 226] = 0;  /* -64 >> 9 = -1 -> 0 */
        A[off + 3] = 3;  B[off + 3] = 3;  A[off + 227] = 1; B[off + 227] = 1;  /* 8 * 64 >> 9 = 1 */
        m4a_sound_main_ram(&r, off, 5);
        ASSERT_EQ(A[off], -4, "reverb: negative result rounds toward zero (+1 on bit 7)");
        ASSERT_EQ(B[off], -4, "reverb: mono, written to both sides");
        ASSERT_EQ(A[off + 1], 5, "reverb: positive result floors");
        ASSERT_EQ(A[off + 2], 0, "reverb: -1 >> 9 becomes 0");
        ASSERT_EQ(A[off + 3], 1, "reverb: small positive");
        ASSERT_EQ(A[off + 4], 0, "reverb: empty taps give 0");
        /* pcmDmaCounter == 2: the second tap is the buffer start. */
        memset(r.pcmBuffer, 0, sizeof(r.pcmBuffer));
        int last = 6 * r.pcmSamplesPerVBlank;
        A[0] = 100; B[0] = 100;
        m4a_sound_main_ram(&r, last, 2);
        ASSERT_EQ(A[last], (200 * 64) >> 9, "reverb: counter 2 taps the buffer start");
        /* No reverb: the slot is zero-filled. */
        r.reverb.amount = 0;
        A[off] = 50; B[off] = 50;
        m4a_sound_main_ram(&r, off, 5);
        ASSERT_EQ(A[off], 0, "no reverb: slot zero-filled");
        ASSERT_EQ(B[off], 0, "no reverb: both sides");
        m4a_engine_destroy(&r);
    }

    m4a_engine_destroy(&e);
}


/* ------------------------------------------------------------------------
 * Song parser and sequencer (docs/ACCURACY.md, "Sequencer (MPlayMain)")
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

/* SoundMain timing model: the sequencer's per-frame MPlayMain cycle sum
 * against mgba's CgbSound entry for the sq1 micro-test (test/accuracy/rom/
 * micro/sq1.s; build/probe/sq1.txt.frames from mgba_probe, cycles from
 * VBlank line 160).  Frame 3 is the song's first MPlayMain: the START init,
 * KEYSH, TEMPO, VOICE, VOL, PAN, the note and the wait; then no-tick frames
 * (tempo 120: 1402 = 566 + 150 + 16 + 30 x 10 - 6 + 6 + 370), tick frames
 * with the gate counting down (1866), the gate expiry (+8, frame 33), the
 * dead channel's ClearChain (frame 35), the empty-chain wait (1840) and the
 * second note under running status (frame 48). */
static void test_sequencer_cycles(void)
{
    printf("Testing sequencer cycle accounting against the sq1 probe...\n");
    ToneData voices[128];
    seq_test_voices(voices);
    M4ASong *song = seq_song(
        "\t.byte\tKEYSH , t_key+0\n"
        "\t.byte\tTEMPO , 120*t_tbs/2\n"
        "\t.byte\t\tVOICE , 0\n"
        "\t.byte\t\tVOL   , 127*t_mvl/mxv\n"
        "\t.byte\t\tPAN   , c_v+0\n"
        "\t.byte\t\tN24   , Cn3 , v127\n"
        "\t.byte\tW36\n"
        "\t.byte\t\t        Cn4 \n"
        "\t.byte\tW36\n"
        "\t.byte\t\tN48   , Cn2 \n"
        "\t.byte\tW24\n"
        "\t.byte\tW24\n"
        "\t.byte\tFINE\n");
    ASSERT(song != NULL, "sq1 song parses");
    if (!song) return;

    M4AEngine engine;
    m4a_engine_init(&engine, 48000.0f);
    m4a_engine_set_voicegroup(&engine, voices);
    M4ASequencer seq;
    m4a_sequencer_init(&seq, &engine);
    m4a_sequencer_start(&seq, song);

    /* mgba's CgbSound entry for engine frames 3..52 (the probe's column). */
    static const int expected[50] = {
        4069, 1402, 1866, 1866, 1866, 1866, 1402, 1866, 1866, 1866,
        1866, 1402, 1866, 1866, 1866, 1866, 1402, 1866, 1866, 1866,
        1866, 1402, 1866, 1866, 1866, 1866, 1402, 1866, 1866, 1866,
        1874, 1402, 1953, 1840, 1840, 1840, 1402, 1840, 1840, 1840,
        1840, 1402, 1840, 1840, 1840, 2790, 1402, 1866, 1866, 1866 };
    int mismatches = 0;
    for (int k = 3; k <= 52; k++) {
        m4a_sequencer_frame(&seq);
        if (seq.frameCycles != expected[k - 3]) {
            if (mismatches++ < 5)
                printf("  frame %d: engine %d, mgba %d\n", k, seq.frameCycles, expected[k - 3]);
        }
        /* The frame is driven without the engine's tick, so stand in for
         * CgbSound on the note's channel: the start frame settles it in
         * sustain (attack 0, decay 0) and the gate expiry's SF_STOP turns it
         * off on the next pass (release 0, no echo), which is what the
         * sequencer's chain walk sees a frame later. */
        M4ACGBChannel *ch = &engine.cgbChannels[0];
        if (ch->status & CHN_START)
            ch->status = CHN_ENV_SUSTAIN;
        else if ((ch->status & CHN_STOP) && (ch->status & CHN_ON))
            ch->status = 0;           /* release 0, no echo: CgbOscOff */
    }
    ASSERT_EQ(mismatches, 0, "sequencer: CgbSound entry matches the sq1 probe on frames 3..52");
    ASSERT_EQ(seq.frameCycles, 1866, "sequencer: steady tick frame after the second note");

    m4a_sequencer_detach(&seq);
    m4a_engine_destroy(&engine);
    m4a_song_free(song);
}

/* MPlayMain's LFO (m4a_1.s _081DD95A): the accumulator is stored as a
 * byte, but the rising half computes 0x80 - the untruncated sum.  With
 * LFOS 0x50 and MOD 1 the fourth step sums to 0x140: the GBA gets modM =
 * (0x80 - 0x140) >> 6 = -3, a byte-truncated port would get (0x80 -
 * 0x40) >> 6 = 1. */
static void test_sequencer_lfo_untruncated_sum(void)
{
    printf("Testing sequencer LFO: the rising half uses the untruncated accumulator...\n");
    ToneData voices[128];
    seq_test_voices(voices);
    M4ASong *song = seq_song(
        "\t.byte\tLFOS , 0x50\n"
        "\t.byte\tMOD , 1\n"
        "\t.byte\t\tN96   , Cn3 , v127\n"
        "\t.byte\tW96\n"
        "\t.byte\tFINE\n");
    ASSERT(song != NULL, "lfo song parses");
    if (!song) return;
    M4AEngine engine;
    m4a_engine_init(&engine, 48000.0f);
    m4a_engine_set_voicegroup(&engine, voices);
    M4ASequencer seq;
    m4a_sequencer_init(&seq, &engine);
    m4a_sequencer_start(&seq, song);
    /* frame 1: the commands and the note (no LFO step on the note's tick);
     * frames 2..5: four LFO steps on the wait. */
    /* One LFO step per tick, the first on the note's own tick (its W96 is
     * read in the same tick).  Each step adds lfoSpeed to the stored byte:
     * the sum exceeds 255 on the fourth step (0xF0 + 0x50 = 0x140), where
     * the rising half gives (0x80 - 0x140) >> 6 = -3; a byte-truncated
     * port would give (0x80 - 0x40) >> 6 = 1. */
    int8_t expected[6] = { 0, 0, 0, 0, 0, 0 };
    uint8_t c = 0;
    for (int i = 1; i <= 5; i++) {
        int sum = c + 0x50;
        c = (uint8_t)sum;
        int r = ((int8_t)(c - 0x40) < 0) ? (int8_t)c : 0x80 - sum;
        expected[i] = (int8_t)((1 * r) >> 6);
    }
    for (int i = 1; i <= 5; i++) {
        m4a_sequencer_frame(&seq);
        ASSERT_EQ(engine.tracks[0].modM, expected[i], "modM after an LFO step follows the asm's arithmetic");
    }
    ASSERT_EQ(expected[4], -3, "the fourth step crosses 256: modM -3, not 1");
    m4a_sequencer_detach(&seq);
    m4a_engine_destroy(&engine);
    m4a_song_free(song);
}

/* Handlers whose cost depends on their operand: MOD 0 and LFOS 0 branch
 * to clear_modM (51 + 58 instead of 69); the frame sum must differ by 40
 * from the nonzero form and nothing else. */
static void test_sequencer_cycles_lfos_zero(void)
{
    printf("Testing sequencer cycle accounting: LFOS / MOD 0 take the clear_modM path...\n");
    ToneData voices[128];
    seq_test_voices(voices);
    static const char *forms[4] = {
        "\t.byte\tLFOS , 44\n", "\t.byte\tLFOS , 0\n",
        "\t.byte\tMOD , 1\n", "\t.byte\tMOD , 0\n" };
    int cycles[4] = { 0, 0, 0, 0 };
    for (int f = 0; f < 4; f++) {
        char body[256];
        snprintf(body, sizeof(body), "%s\t.byte\tW24\n\t.byte\tFINE\n", forms[f]);
        M4ASong *song = seq_song(body);
        ASSERT(song != NULL, "lfos/mod song parses");
        if (!song) return;
        M4AEngine engine;
        m4a_engine_init(&engine, 48000.0f);
        m4a_engine_set_voicegroup(&engine, voices);
        M4ASequencer seq;
        m4a_sequencer_init(&seq, &engine);
        m4a_sequencer_start(&seq, song);
        m4a_sequencer_frame(&seq);
        cycles[f] = seq.frameCycles;
        m4a_sequencer_detach(&seq);
        m4a_engine_destroy(&engine);
        m4a_song_free(song);
    }
    /* clear_modM also flags PITCHG, so the frame gains the pitch pass
     * (30 + 62 + 50, empty chain 6, track end 10, less the no-flags 20 =
     * 138); LFOS 0 then takes the wait path with lfoSpeed 0 (23 vs 28). */
    ASSERT_EQ(cycles[1] - cycles[0], 40 + 138 - 5, "LFOS 0: clear_modM 40, the pitch pass 138, lfoSpeed-0 wait -5");
    /* MOD 1 with the default lfoSpeed 0x16 runs an LFO step on the wait
     * (34 + 26 + 20 = 80 instead of 28): MOD 0 saves 52 there. */
    ASSERT_EQ(cycles[3] - cycles[2], 40 + 138 - 52, "MOD 0: clear_modM 40, the pitch pass 138, no LFO step -52");
    ASSERT_EQ(cycles[3] - cycles[1], 5, "MOD 0 and LFOS 0 differ only by the wait path's lfoSpeed test");
}

/* A note the GBA rejects (same priority, an earlier track holds the CGB
 * channel) costs the rejection path only; the polyphony-debug invert mode
 * plays it on a shadow channel but must not add the take/chain charges. */
static void test_sequencer_cycles_poly_invert(void)
{
    printf("Testing sequencer cycle accounting: a rejected note costs the same in poly-debug invert mode...\n");
    ToneData voices[128];
    seq_test_voices(voices);
    static const char *text =
        "\t.equ\tt_grp, voicegroup_test\n"
        "\t.equ\tt_pri, 0\n\t.equ\tt_rev, 0\n\t.equ\tt_mvl, 127\n\t.equ\tt_key, 0\n\t.equ\tt_tbs, 1\n"
        "\t.section .rodata\n\t.global\tt\n\t.align\t2\n"
        "t_1:\n\t.byte\tVOICE , 0\n\t.byte\tN96 , Cn3 , v127\n\t.byte\tW96\n\t.byte\tFINE\n"
        "t_2:\n\t.byte\tVOICE , 0\n\t.byte\tW02\n\t.byte\tN48 , En3 , v127\n\t.byte\tW72\n\t.byte\tFINE\n"
        "\t.align\t2\nt:\n\t.byte\t2, 0, t_pri, t_rev\n\t.word\tt_grp\n\t.word\tt_1\n\t.word\tt_2\n";
    int cycles[2][4];
    for (int inv = 0; inv < 2; inv++) {
        M4ASong *song = parse_song_text(text);
        ASSERT(song != NULL, "poly-invert song parses");
        if (!song) return;
        M4AEngine engine;
        m4a_engine_init(&engine, 48000.0f);
        m4a_engine_set_voicegroup(&engine, voices);
        m4a_engine_set_poly_debug_invert(&engine, inv == 1);
        M4ASequencer seq;
        m4a_sequencer_init(&seq, &engine);
        m4a_sequencer_start(&seq, song);
        for (int f = 0; f < 4; f++) {
            m4a_sequencer_frame(&seq);
            cycles[inv][f] = seq.frameCycles;
        }
        /* frame 1 starts both tracks, frame 2.. ticks; track 2's note lands
         * on the tick where its W02 expires, and track 1 still holds square 1. */
        ASSERT(engine.cgbChannels[0].status != 0 && engine.cgbChannels[0].trackIndex == 0,
               "poly-invert: track 1 keeps square 1");
        if (inv == 1)
            ASSERT(engine.cgbChannels[MAX_CGB_CHANNELS].status != 0, "poly-invert: the rejected note plays on the shadow channel");
        else
            ASSERT_EQ(engine.cgbChannels[MAX_CGB_CHANNELS].status, 0, "poly-invert off: the rejected note is dropped");
        m4a_sequencer_detach(&seq);
        m4a_engine_destroy(&engine);
        m4a_song_free(song);
    }
    for (int f = 0; f < 4; f++)
        ASSERT_EQ(cycles[1][f], cycles[0][f], "poly-invert: the frame's MPlayMain cost is the GBA's either way");
}

/* TrackStop (m4aMPlayStart / Stop) walks the track's channel chain: a
 * chained, sounding channel is cut (CgbOscOff); a channel ply_fine
 * unchained keeps releasing through the next song's start (ply_fine also
 * clears the track's EXIST flag, which TrackStop tests first, so both the
 * chain walk and that flag protect it). */
static void test_sequencer_track_stop_chain(void)
{
    printf("Testing sequencer TrackStop: only chained channels are killed...\n");
    ToneData voices[128];
    seq_test_voices(voices);
    voices[0].release = 1;                     /* a long software release */
    M4ASong *fineSong = seq_song("\t.byte\tVOICE , 0\n\t.byte\tVOL , 127*t_mvl/mxv\n\t.byte\tN02 , Cn3 , v127\n\t.byte\tW02\n\t.byte\tFINE\n");
    M4ASong *holdSong = seq_song("\t.byte\tVOICE , 0\n\t.byte\tVOL , 127*t_mvl/mxv\n\t.byte\tN96 , Cn3 , v127\n\t.byte\tW96\n\t.byte\tFINE\n");
    M4ASong *restSong = seq_song("\t.byte\tW96\n\t.byte\tFINE\n");
    ASSERT(fineSong && holdSong && restSong, "track-stop songs parse");
    if (!fineSong || !holdSong || !restSong) return;

    M4AEngine engine;
    float outL[1024], outR[1024];
    engine_init_booted(&engine, 48000.0f);
    m4a_engine_set_voicegroup(&engine, voices);
    M4ASequencer seq;
    m4a_sequencer_init(&seq, &engine);      /* the frame hook runs MPlayMain, then CgbSound */
    M4ACGBChannel *sq = &engine.cgbChannels[0];

    /* FINE after the note: ply_fine flags CHN_STOP and unchains; the
     * release runs on. */
    m4a_sequencer_start(&seq, fineSong);
    advance_frames(&engine, 6, outL, outR);
    ASSERT(sq->status & CHN_ON, "track-stop: the channel is still releasing after FINE");
    ASSERT_EQ(seq.chainOwner[TOTAL_PCM_CHANNELS], 0, "track-stop: ply_fine unchained it");
    m4a_sequencer_start(&seq, restSong);
    ASSERT(sq->status & CHN_ON, "track-stop: a new song's TrackStop leaves the unchained release alone");

    /* A chained, sounding channel is cut by the next start. */
    m4a_sequencer_start(&seq, holdSong);
    advance_frames(&engine, 3, outL, outR);
    ASSERT(sq->status & CHN_ON, "track-stop: the held note sounds");
    ASSERT_EQ(seq.chainOwner[TOTAL_PCM_CHANNELS], 1, "track-stop: the held note is chained to track 1");
    m4a_sequencer_start(&seq, restSong);
    ASSERT_EQ(sq->status, 0, "track-stop: the chained channel is killed by the next start");
    ASSERT_EQ(seq.chainOwner[TOTAL_PCM_CHANNELS], 0, "track-stop: and unchained");

    m4a_sequencer_detach(&seq);
    m4a_engine_destroy(&engine);
    m4a_song_free(fineSong);
    m4a_song_free(holdSong);
    m4a_song_free(restSong);
}

/* ply_fine's RealClearChain nulls channel->track (m4a_1.s): a channel
 * still releasing after FINE belongs to no track, so a song restarted on
 * the same track index must not rewrite its volume / pitch in its VOLCHG /
 * PITCHG pass (which walks the new track's chain on the GBA) and ply_fine
 * must not re-flag it.  Slice 4's review found the port keeping
 * trackIndex; a cry with a release tail followed by another cry on the
 * same player is the game case. */
static void test_sequencer_fine_orphan(void)
{
    printf("Testing sequencer FINE: the released channel leaves its track...\n");
    ToneData voices[128];
    seq_test_voices(voices);
    voices[0].release = 1;                     /* a long software release */
    M4ASong *fineSong = seq_song("\t.byte\tVOICE , 0\n\t.byte\tVOL , 127*t_mvl/mxv\n\t.byte\tN02 , Cn3 , v127\n\t.byte\tW02\n\t.byte\tFINE\n");
    M4ASong *quietSong = seq_song("\t.byte\tVOICE , 0\n\t.byte\tVOL , 10*t_mvl/mxv\n\t.byte\tPAN , c_v-40\n\t.byte\tW96\n\t.byte\tFINE\n");
    ASSERT(fineSong && quietSong, "fine-orphan songs parse");
    if (!fineSong || !quietSong) return;

    M4AEngine engine;
    float outL[1024], outR[1024];
    engine_init_booted(&engine, 48000.0f);
    m4a_engine_set_voicegroup(&engine, voices);
    M4ASequencer seq;
    m4a_sequencer_init(&seq, &engine);
    M4ACGBChannel *sq = &engine.cgbChannels[0];

    m4a_sequencer_start(&seq, fineSong);
    advance_frames(&engine, 2, outL, outR);
    ASSERT((sq->status & CHN_ON) && !(sq->status & CHN_STOP), "fine-orphan: the note sounds on track 0");
    ASSERT_EQ(sq->trackIndex, 0, "fine-orphan: owned by track 0 while chained");
    advance_frames(&engine, 4, outL, outR);
    ASSERT((sq->status & CHN_ON) && (sq->status & CHN_STOP), "fine-orphan: releasing after FINE");
    ASSERT_EQ(seq.chainOwner[TOTAL_PCM_CHANNELS], 0, "fine-orphan: unchained");
    ASSERT_EQ(sq->trackIndex, M4A_NO_TRACK, "fine-orphan: RealClearChain nulled channel->track");
    uint8_t right = sq->rightVolume, left = sq->leftVolume;
    ASSERT(right != 0 || left != 0, "fine-orphan: the release still carries the note's volume");

    /* The same track index restarted while the orphan releases: its VOL /
     * PAN pass walks its own (empty) chain and leaves the orphan alone. */
    m4a_sequencer_start(&seq, quietSong);
    advance_frames(&engine, 2, outL, outR);
    ASSERT((sq->status & CHN_ON) && (sq->status & CHN_STOP), "fine-orphan: still releasing after the restart (TrackStop kills chained channels only)");
    ASSERT(sq->rightVolume == right && sq->leftVolume == left, "fine-orphan: the new track's VOL 10 / PAN pass did not rewrite the orphan's volumes");
    ASSERT_EQ(sq->trackIndex, M4A_NO_TRACK, "fine-orphan: still no track");

    m4a_sequencer_detach(&seq);
    m4a_engine_destroy(&engine);
    m4a_song_free(fineSong);
    m4a_song_free(quietSong);
}

/* The sound FIFO's refill DMA stalls the CPU 18 cycles every 16 samples
 * (20064 cycles at 13379 Hz), counted on the cycle spans the timing model
 * charges. */
static void test_fifo_dma_stall(void)
{
    printf("Testing the FIFO DMA stall accounting...\n");
    M4AGbaAudio g;
    memset(&g, 0, sizeof(g));
    g.timerPeriodNum = 280896;
    g.timerPeriodDen = 224;
    g.dmaEnabled = true;
    g.fifoDmaCycle = -1;
    ASSERT_EQ((int)m4a_gba_fifo_dma_stall(&g, 0, 100000), 0, "stall: nothing before the first refill");
    g.fifoDmaCycle = 100;
    ASSERT_EQ((int)m4a_gba_fifo_dma_stall(&g, 0, 99), 0, "stall: span ending before the refill");
    ASSERT_EQ((int)m4a_gba_fifo_dma_stall(&g, 50, 100), 18, "stall: span ending on the refill cycle");
    ASSERT_EQ((int)m4a_gba_fifo_dma_stall(&g, 100, 200), 0, "stall: span starting on the refill cycle");
    ASSERT_EQ((int)m4a_gba_fifo_dma_stall(&g, 20000, 20200), 18, "stall: the next refill, 20064 later");
    ASSERT_EQ((int)m4a_gba_fifo_dma_stall(&g, 0, 60000), 54, "stall: three refills in 60000 cycles");
    ASSERT_EQ((int)m4a_gba_fifo_dma_stall(&g, 40300, 40200), 0, "stall: empty span");
    g.dmaEnabled = false;
    ASSERT_EQ((int)m4a_gba_fifo_dma_stall(&g, 50, 100), 0, "stall: none while the DMA is off");
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


static void test_midi_path_modt_tune_lfodl(void)
{
    printf("Testing MIDI-path MODT / TUNE / LFODL controls...\n");

    WaveData *wd = seq_test_wave();
    ToneData voices[128];
    memset(voices, 0, sizeof(voices));
    voices[0].type = VOICE_DIRECTSOUND;
    voices[0].key = 60;
    voices[0].wav = wd;
    voices[0].attack = 0xFF;
    voices[0].sustain = 0xFF;

    M4AEngine engine;
    m4a_engine_init(&engine, 48000.0f);
    m4a_engine_set_voicegroup(&engine, voices);
    m4a_engine_program_change(&engine, 0, 0);
    m4a_engine_cc(&engine, 0, 7, 127);

    /* TUNE is c_v-centred: 64 -> 0, 127 -> +63, 0 -> -64 (one semitone down). */
    m4a_engine_cc(&engine, 0, 0x18, 127);
    ASSERT_EQ(engine.tracks[0].tune, 63, "TUNE 127 -> +63");
    m4a_engine_cc(&engine, 0, 0x18, 0);
    ASSERT_EQ(engine.tracks[0].tune, -64, "TUNE 0 -> -64");
    m4a_engine_tick(&engine);             /* the frame's PITCHG pass computes keyM */
    ASSERT_EQ(engine.tracks[0].keyM, -1, "TUNE -64 is one semitone down in keyM");
    ASSERT_EQ(engine.tracks[0].pitM, 0, "TUNE -64 leaves no fine remainder");
    m4a_engine_cc(&engine, 0, 0x18, 64);
    m4a_engine_tick(&engine);
    ASSERT_EQ(engine.tracks[0].keyM, 0, "TUNE centre");

    /* MODT moves the LFO output between pitch and volume. */
    m4a_engine_cc(&engine, 0, 0x15, 1);   /* LFOS */
    m4a_engine_cc(&engine, 0, 0x1, 64);   /* MOD */
    int h = m4a_engine_note_on(&engine, 0, 60, 127);
    ASSERT(h >= 0, "note started");
    if (h < 0) { m4a_engine_destroy(&engine); free(wd); return; }
    M4APCMChannel *ch = &engine.pcmChannels[h];
    uint32_t f0 = ch->frequency;
    uint8_t v0 = ch->leftVolume;
    for (int i = 0; i < 20; i++)
        m4a_engine_tick(&engine);
    ASSERT(ch->frequency != f0, "MODT 0: vibrato moves the pitch");
    ASSERT_EQ(ch->leftVolume, v0, "MODT 0: vibrato leaves the volume alone");
    m4a_engine_cc(&engine, 0, 0x16, 1);   /* MODT tremolo */
    m4a_engine_tick(&engine);             /* applied by the frame's pass */
    ASSERT_EQ(ch->frequency, f0, "MODT change recomputes pitch without the LFO offset");
    for (int i = 0; i < 20; i++)
        m4a_engine_tick(&engine);
    ASSERT_EQ(ch->frequency, f0, "MODT 1: pitch untouched");
    ASSERT(ch->leftVolume != v0, "MODT 1: tremolo moves the volume");
    m4a_engine_cc(&engine, 0, 0x16, 0);
    m4a_engine_note_off(&engine, 0, 60);
    for (int i = 0; i < 60; i++)
        m4a_engine_tick(&engine);

    /* LFODL: a note start reloads the delay and recentres the LFO; the LFO
     * holds for lfoDelay ticks before stepping again. */
    m4a_engine_cc(&engine, 0, 0x1A, 10);
    ASSERT_EQ(engine.tracks[0].lfoDelay, 10, "LFODL stored");
    engine.tracks[0].modM = 17;     /* pretend the LFO is mid-swing */
    engine.tracks[0].lfoSpeedC = 99;
    h = m4a_engine_note_on(&engine, 0, 62, 127);
    ASSERT(h >= 0, "second note started");
    ASSERT_EQ(engine.tracks[0].modM, 0, "note start with LFODL recentres the LFO");
    ASSERT_EQ(engine.tracks[0].lfoSpeedC, 0, "note start with LFODL resets the LFO phase");
    ASSERT_EQ(engine.tracks[0].lfoDelayC, 10, "delay counter reloaded");
    for (int i = 0; i < 10; i++)
        m4a_engine_tick(&engine);
    ASSERT_EQ(engine.tracks[0].lfoDelayC, 0, "delay counted down");
    ASSERT_EQ(engine.tracks[0].lfoSpeedC, 0, "LFO held during the delay");
    m4a_engine_tick(&engine);
    ASSERT_EQ(engine.tracks[0].lfoSpeedC, 1, "LFO steps after the delay");

    m4a_engine_destroy(&engine);
    free(wd);
}

/* The music players' positions in SoundMain's MPlayMain chain and their
 * track counts (m4a_sequencer_set_player), against the SE probes of
 * milestone 4 slice 1 (build/probe/se_*.txt; docs/ACCURACY.md, appendix A).  With
 * the other players paused, mgba puts _081DD9BC at 716 / 779 / 842 / 905
 * and _081DDA6C -> CgbSound at 370 / 307 / 244 / 181 for BGM / SE1 / SE2 /
 * SE3, and CgbSound at 1007 on every frame where the selected player is
 * paused too.  A no-tick frame with every player track existing and
 * unflagged: SE1 with its 3 tracks 1204 (se_faint frame 4), SE2 with 4 of
 * its 9 tracks 1390 (se_contest_place frame 4). */
static M4ASong *seq_song_tracks(int n)
{
    char buf[4096];
    int p = snprintf(buf, sizeof(buf), "%s", seq_song_header);
    for (int i = 1; i <= n; i++) {
        if (i > 1)
            p += snprintf(buf + p, sizeof(buf) - p, "t_%d:\n", i);
        p += snprintf(buf + p, sizeof(buf) - p, "%s\t.byte\tW96\n\t.byte\tFINE\n",
                      i == 1 ? "\t.byte\tTEMPO , 90*t_tbs/2\n" : "");
    }
    p += snprintf(buf + p, sizeof(buf) - p, "\t.align\t2\nt:\n\t.byte\t%d, 0, t_pri, t_rev\n\t.word\tt_grp\n", n);
    for (int i = 1; i <= n; i++)
        p += snprintf(buf + p, sizeof(buf) - p, "\t.word\tt_%d\n", i);
    return parse_song_text(buf);
}

static void test_sequencer_players(void)
{
    printf("Testing sequencer music players (chain position, track counts)...\n");
    ToneData voices[128];
    seq_test_voices(voices);
    /* BGM, SE1, SE2, SE3 from the SE probes; cry 0 from the four cry probes
     * (step 5: _081DD9BC 968, exit 118); cry 1 by the chain rule (not
     * measured: no run starts two cries). */
    static const int tracks[M4A_PLAYER_COUNT] = { 10, 3, 9, 1, 2, 2 };
    static const int bc[M4A_PLAYER_COUNT] = { 716, 779, 842, 905, 968, 1031 };      /* _081DD9BC */
    static const int exits[M4A_PLAYER_COUNT] = { 370, 307, 244, 181, 118, 55 };     /* _081DDA6C -> CgbSound */
    ASSERT_EQ(M4A_PLAYER_COUNT, 6, "sequencer: six players in the chain");
    for (int p = 0; p < M4A_PLAYER_COUNT; p++) {
        M4AEngine engine;
        m4a_engine_init(&engine, 48000.0f);
        m4a_engine_set_voicegroup(&engine, voices);
        M4ASequencer seq;
        m4a_sequencer_init(&seq, &engine);
        ASSERT_EQ(seq.maxTracks, 10, "sequencer: init selects the BGM player");
        ASSERT_EQ(m4a_sequencer_set_player(&seq, p), 0, "sequencer: a valid player is accepted");
        ASSERT_EQ(seq.player, p, "sequencer: player selected");
        ASSERT_EQ(seq.maxTracks, tracks[p], "sequencer: the player's track count");
        ASSERT_EQ(seq.mpEntry + seq.mpHead, bc[p], "sequencer: _081DD9BC position of the player");
        ASSERT_EQ(seq.mpExit, exits[p], "sequencer: _081DDA6C -> CgbSound of the player");
        m4a_sequencer_frame(&seq);
        ASSERT_EQ(seq.frameCycles, 1007, "sequencer: every player paused -> CgbSound at 1007");
        m4a_sequencer_detach(&seq);
        m4a_engine_destroy(&engine);
    }
    {
        M4AEngine engine;
        m4a_engine_init(&engine, 48000.0f);
        m4a_engine_set_voicegroup(&engine, voices);
        M4ASequencer seq;
        m4a_sequencer_init(&seq, &engine);
        m4a_sequencer_set_player(&seq, M4A_PLAYER_SE2);
        ASSERT_EQ(m4a_sequencer_set_player(&seq, 99), -1, "sequencer: out-of-range player is an error");
        ASSERT_EQ(m4a_sequencer_set_player(&seq, -1), -1, "sequencer: negative player is an error");
        ASSERT_EQ(seq.player, M4A_PLAYER_SE2, "sequencer: an out-of-range player changes nothing");
        ASSERT_EQ(seq.maxTracks, 9, "sequencer: ... nor the track count");
        m4a_sequencer_detach(&seq);
        m4a_engine_destroy(&engine);
    }

    /* SE1, three waiting tracks: start frame, then the no-tick frame. */
    M4ASong *song3 = seq_song_tracks(3);
    ASSERT(song3 != NULL, "3-track song parses");
    M4ASong *song4 = seq_song_tracks(4);
    ASSERT(song4 != NULL, "4-track song parses");
    if (song3 && song4) {
        M4AEngine engine;
        m4a_engine_init(&engine, 48000.0f);
        m4a_engine_set_voicegroup(&engine, voices);
        M4ASequencer seq;
        m4a_sequencer_init(&seq, &engine);
        m4a_sequencer_set_player(&seq, M4A_PLAYER_SE1);
        m4a_sequencer_start(&seq, song3);
        ASSERT_EQ(seq.trackCount, 3, "SE1: three tracks play");
        m4a_sequencer_frame(&seq);      /* frame 3: the start tick */
        m4a_sequencer_frame(&seq);      /* frame 4: tempoC 90, no tick */
        ASSERT_EQ(seq.frameCycles, 1204, "SE1 no-tick frame, 3 tracks existing (se_faint frame 4)");

        /* A fourth track does not fit the SE1 player: MPlayStart clears it. */
        m4a_sequencer_set_player(&seq, M4A_PLAYER_SE1);
        m4a_sequencer_start(&seq, song4);
        ASSERT_EQ(seq.trackCount, 3, "SE1: a 4-track song plays 3 tracks");
        ASSERT_EQ(seq.tracks[3].flags, 0, "SE1: the fourth track does not exist");
        m4a_sequencer_frame(&seq);
        m4a_sequencer_frame(&seq);
        ASSERT_EQ(seq.frameCycles, 1204, "SE1 no-tick frame with a 4-track song: the player's 3 tracks");

        /* SE2: four of nine tracks. */
        m4a_sequencer_set_player(&seq, M4A_PLAYER_SE2);
        m4a_sequencer_start(&seq, song4);
        ASSERT_EQ(seq.trackCount, 4, "SE2: four tracks play");
        m4a_sequencer_frame(&seq);
        m4a_sequencer_frame(&seq);
        ASSERT_EQ(seq.frameCycles, 1390, "SE2 no-tick frame, 4 of 9 tracks existing (se_contest_place frame 4)");

        /* BGM, for the record: the sq1 probe's one-track no-tick frame
         * (1402) plus 6 for each further existing track (20 instead of 14
         * in the apply pass). */
        m4a_sequencer_set_player(&seq, M4A_PLAYER_BGM);
        m4a_sequencer_start(&seq, song3);
        m4a_sequencer_frame(&seq);
        m4a_sequencer_frame(&seq);
        ASSERT_EQ(seq.frameCycles, 1402 + 2 * 6, "BGM no-tick frame, 3 of 10 tracks existing");
        m4a_sequencer_detach(&seq);
        m4a_engine_destroy(&engine);
    }
    m4a_song_free(song3);
    m4a_song_free(song4);
}

/* A whole sound effect on the SE2 player against its probe: se_truck_unload
 * (sound/songs/midi/se_truck_unload.mid, voicegroup rs_sfx_1 voice 120 =
 * voice_noise 60, 0, 0, 2, 7, 15, 0; song table player SE2, id 51).
 * build/probe/se_truck_unload.txt.frames, CgbSound entry on engine frames
 * 3..32: the start frame (KEYSH, TEMPO, VOICE, VOL, the note), the W03
 * waits read every third tick (1895 = 1802 + the 93 of an explicit wait
 * byte), the second note on the channel the gate just stopped (frame 9),
 * FINE with that channel chained (frame 30, ply_fine 114), the tick with no
 * track left (frame 31: _081DD9A4 -> _081DDA6C 24), then paused (1007). */
static void test_sequencer_cycles_se2(void)
{
    printf("Testing sequencer cycle accounting on the SE2 player against the se_truck_unload probe...\n");
    ToneData voices[128];
    seq_test_voices(voices);
    voices[120].type = VOICE_NOISE;
    voices[120].key = 60;
    voices[120].wavePointer = NULL;     /* noise period select 0 */
    voices[120].attack = 2;
    voices[120].decay = 7;
    voices[120].sustain = 15;
    voices[120].release = 0;
    M4ASong *song = seq_song(
        "\t.byte\tKEYSH , t_key+0\n"
        "\t.byte\tTEMPO , 150*t_tbs/2\n"
        "\t.byte\t\tVOICE , 120\n"
        "\t.byte\t\tVOL   , 80*t_mvl/mxv\n"
        "\t.byte\t\tN06   , Bn3 , v028\n"
        "\t.byte\tW03\n"
        "\t.byte\tW03\n"
        "\t.byte\t\tN21   , Gs4 \n"
        "\t.byte\tW03\n\t.byte\tW03\n\t.byte\tW03\n\t.byte\tW03\n\t.byte\tW03\n\t.byte\tW03\n\t.byte\tW03\n"
        "\t.byte\tFINE\n");
    ASSERT(song != NULL, "se_truck_unload song parses");
    if (!song) return;

    M4AEngine engine;
    m4a_engine_init(&engine, 48000.0f);
    m4a_engine_set_voicegroup(&engine, voices);
    M4ASequencer seq;
    m4a_sequencer_init(&seq, &engine);
    m4a_sequencer_set_player(&seq, M4A_PLAYER_SE2);
    m4a_sequencer_start(&seq, song);

    static const int expected[30] = {
        3729, 1802, 1802, 1895, 1802, 1802, 2704, 1802, 1802, 1895,
        1802, 1802, 1895, 1802, 1802, 1895, 1802, 1802, 1895, 1802,
        1802, 1895, 1802, 1802, 1895, 1802, 1802, 1987, 1430, 1007 };
    int mismatches = 0;
    for (int k = 3; k <= 32; k++) {
        m4a_sequencer_frame(&seq);
        if (seq.frameCycles != expected[k - 3]) {
            if (mismatches++ < 5)
                printf("  frame %d: engine %d, mgba %d\n", k, seq.frameCycles, expected[k - 3]);
        }
        /* Stand in for CgbSound on the noise channel (see test_sequencer_cycles). */
        M4ACGBChannel *ch = &engine.cgbChannels[3];
        if (ch->status & CHN_START)
            ch->status = CHN_ENV_SUSTAIN;
        else if ((ch->status & CHN_STOP) && (ch->status & CHN_ON))
            ch->status = 0;           /* release 0, no echo: CgbOscOff */
    }
    ASSERT_EQ(mismatches, 0, "sequencer: CgbSound entry matches the se_truck_unload probe on frames 3..32 (SE2)");
    ASSERT(m4a_sequencer_finished(&seq), "sequencer: the SE finished");

    m4a_sequencer_detach(&seq);
    m4a_engine_destroy(&engine);
    m4a_song_free(song);
}

/* The cry song (m4a_cry.c) against the ROM: gPokemonCrySongTemplate's bytes
 * (0x08002F20 in the 2026-10-24 accuracy ROM) with the setters applied as
 * the ROM's StartCry / PlayCryInternal apply them (volume 120, pan 0,
 * pitch 15360, length 140, progress 0, release 0, chorus 0, priority 10),
 * the pointer words as SetPokemonCryTone patches them (offsets here). */
static void test_cry_song(void)
{
    printf("Testing the cry song template...\n");
    /* The template as the ROM holds it (tone word = voicegroup_dummy, zeroed here). */
    static const uint8_t romTemplate[M4A_CRY_SONG_SIZE] = {
        0x01, 0x00, 0xFF, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00,
        0x00, 0xC8, 0x40, 0xB2, 0x00, 0x00, 0x00, 0x00, 0xC8, 0x50, 0xBD, 0x00, 0xBE, 0x7F, 0xCD, 0x0D,
        0x00, 0x00, 0x00, 0x00, 0xCD, 0x07, 0x00, 0xBF, 0x40, 0xCF, 0x3C, 0x7F, 0xCD, 0x0C, 0x3C, 0x00,
        0xCE, 0xB1, 0x00, 0x00 };
    ASSERT_EQ(memcmp(gM4ACrySongTemplate, romTemplate, M4A_CRY_SONG_SIZE), 0,
              "cry: the template equals the ROM's gPokemonCrySongTemplate");

    /* After StartCry's setters and SetPokemonCryTone (hand-applied to the
     * ROM bytes): trackCount 1, priority 10, part0 at 0x11, part1 at 0x18,
     * TUNE c_v, GOTO cont (0x1A), TUNE c_v+16, VOICE 0, VOL 120, XCMD 0D 0,
     * XCMD RELE 0, PAN c_v, TIE Cn3 v127, XCMD WAIT 140, EOT, FINE. */
    static const uint8_t expected[M4A_CRY_SONG_SIZE] = {
        0x01, 0x00, 0x0A, 0x00, 0x00, 0x00, 0x00, 0x00, 0x11, 0x00, 0x00, 0x00, 0x18, 0x00, 0x00, 0x00,
        0x00, 0xC8, 0x40, 0xB2, 0x1A, 0x00, 0x00, 0x00, 0xC8, 0x50, 0xBD, 0x00, 0xBE, 0x78, 0xCD, 0x0D,
        0x00, 0x00, 0x00, 0x00, 0xCD, 0x07, 0x00, 0xBF, 0x40, 0xCF, 0x3C, 0x7F, 0xCD, 0x0C, 0x8C, 0x00,
        0xCE, 0xB1, 0x00, 0x00 };
    M4ACryParams p;
    m4a_cry_params_default(&p);
    ASSERT_EQ(p.volume, 120, "cry: default volume CRY_VOLUME");
    ASSERT_EQ(p.pitch, 15360, "cry: default pitch (PlayCryInternal, CRY_MODE_NORMAL)");
    ASSERT_EQ(p.length, 140, "cry: default length");
    ASSERT_EQ(p.priority, 10, "cry: default priority CRY_PRIORITY_NORMAL");
    M4ASong *song = m4a_cry_song_new(&p);
    ASSERT(song != NULL, "cry: song built");
    if (!song) return;
    ASSERT_EQ(song->size, M4A_CRY_SONG_SIZE, "cry: sizeof(PokemonCrySong) bytes");
    int bad = -1;
    for (int i = 0; i < M4A_CRY_SONG_SIZE; i++)
        if (song->data[i] != expected[i]) { bad = i; break; }
    if (bad >= 0)
        printf("  byte 0x%02X: engine %02X, ROM %02X\n", bad, song->data[bad], expected[bad]);
    ASSERT_EQ(bad, -1, "cry: the defaults give the ROM's gPokemonCrySongs[0] bytes");
    ASSERT_EQ(song->trackCount, 1, "cry: one track without chorus");
    ASSERT_EQ(song->priority, 10, "cry: header priority");
    ASSERT_EQ(song->reverb, 0, "cry: no reverb bit");
    ASSERT_EQ(song->trackOffsets[0], 0x11, "cry: part0 at 0x11");
    ASSERT_EQ(song->trackOffsets[1], 0x18, "cry: part1 at 0x18");
    ASSERT_EQ(song->dataInRam, 1, "cry: the data lives in IWRAM (gPokemonCrySongs)");
    ASSERT_EQ(song->relocCount, 4, "cry: tone, part0, part1 and the GOTO target are pointers");
    /* The ROM image: gPokemonCrySongs[0] at 0x03001F20 with gCryTable[0] as the tone. */
    uint8_t img[M4A_CRY_SONG_SIZE];
    m4a_song_relocate(song, 0x03001F20, 0x0802AF7C, img);
    ASSERT_EQ(img[4] | (img[5] << 8) | (img[6] << 16) | ((uint32_t)img[7] << 24), 0x0802AF7Cu, "cry: tone word");
    ASSERT_EQ(img[8] | (img[9] << 8) | (img[10] << 16) | ((uint32_t)img[11] << 24), 0x03001F31u, "cry: part[0] = &part0");
    ASSERT_EQ(img[12] | (img[13] << 8) | (img[14] << 16) | ((uint32_t)img[15] << 24), 0x03001F38u, "cry: part[1] = &part1");
    ASSERT_EQ(img[0x14] | (img[0x15] << 8) | (img[0x16] << 16) | ((uint32_t)img[0x17] << 24), 0x03001F3Au, "cry: gotoTarget = &cont");
    m4a_song_free(song);

    /* The setters' arithmetic (m4a.c): pitch 15600 (CRY_MODE_DOUBLES's
     * first), pan -32, chorus 4 (two tracks, the second TUNE c_v+chorus). */
    p.pitch = 15600; p.pan = -32; p.chorus = 4; p.volume = 0xFF; p.length = 0x1234; p.release = 7; p.progress = 0x11223344;
    song = m4a_cry_song_new(&p);
    ASSERT(song != NULL, "cry: song built (chorus)");
    if (!song) return;
    ASSERT_EQ(song->data[0x2A], (15600 + 0x80) >> 8, "cry: pitch -> TIE key 61");
    ASSERT_EQ(song->data[0x12], ((15600 + 0x80) >> 1) & 0x7F, "cry: pitch -> TUNE");
    ASSERT_EQ(song->data[0x19], (4 + (((15600 + 0x80) >> 1) & 0x7F)) & 0x7F, "cry: chorus -> second TUNE = chorus + tune");
    ASSERT_EQ(song->data[0x00], 2, "cry: chorus -> two tracks");
    ASSERT_EQ(song->trackCount, 2, "cry: header track count follows");
    ASSERT_EQ(song->data[0x28], (-32 + 0x40) & 0x7F, "cry: pan -> PAN c_v + pan");
    ASSERT_EQ(song->data[0x1D], 0x7F, "cry: volume & 0x7F");
    ASSERT_EQ(song->data[0x2E], 0x34, "cry: length low byte");
    ASSERT_EQ(song->data[0x2F], 0x12, "cry: length high byte");
    ASSERT_EQ(song->data[0x26], 7, "cry: release");
    ASSERT_EQ(song->data[0x20], 0x44, "cry: progress word (little-endian)");
    ASSERT_EQ(song->data[0x23], 0x11, "cry: progress word high byte");
    m4a_song_free(song);

    /* The corpus variants (milestone 4 slice 3), the setters hand-applied
     * to the ROM template with the test ROM's StartCry (length 140,
     * progress 0, release 0, chorus 0, priority 10): every byte but the
     * named ones equals the default song's. */
    M4ACryParams d;
    m4a_cry_params_default(&d);
    M4ASong *base = m4a_cry_song_new(&d);
    ASSERT(base != NULL, "cry: default song built (variants)");
    if (!base) return;
    static const struct {
        const char *name; int16_t pitch; int8_t pan; uint8_t volume; uint16_t length; uint8_t release; int8_t chorus;
        uint8_t trackCount, key, tune, tune2, panByte, volByte, waitLo, waitHi, releaseByte;
    } variants[14] = {
        /* Cry_Jynx at CRY_MODE_FAINT's 14440: b = 0x38E8 -> key 0x38, tune 0x74, tune2 (16 + 0x74) & 0x7F */
        { "cry_jynx_faint", 14440, 0, 120, 140, 0, 0, 1, 0x38, 0x74, 0x04, 0x40, 0x78, 0x8C, 0x00, 0x00 },
        /* Cry_Bulbasaur at CRY_MODE_HIGH_PITCH's 15800, pan 63: b = 0x3E38 -> key 0x3E, tune 0x1C, tune2 0x2C; PAN (63 + 64) & 0x7F */
        { "cry_bulbasaur_howl", 15800, 63, 120, 140, 0, 0, 1, 0x3E, 0x1C, 0x2C, 0x7F, 0x78, 0x8C, 0x00, 0x00 },
        /* Cry_Pidgey at volume 90 (CRY_MODE_ENCOUNTER's): VOL 0x5A */
        { "cry_pidgey_quiet", 15360, 0, 90, 140, 0, 0, 1, 0x3C, 0x40, 0x50, 0x40, 0x5A, 0x8C, 0x00, 0x00 },
        /* Milestone 4 slice 4, whole modes (src/sound.c PlayCryInternal; none sets chorus):
         * CRY_MODE_FAINT on Cry_Jynx: pitch 14440, length 140 (xWAIT 0x8C 0x00), release 200 (xRELE 0xC8) */
        { "cry_jynx_faint_full", 14440, 0, 120, 140, 200, 0, 1, 0x38, 0x74, 0x04, 0x40, 0x78, 0x8C, 0x00, 0xC8 },
        /* CRY_MODE_DOUBLES on Cry_Wailord: pitch 15360, length 20 (0x14 0x00), release 225 (0xE1) */
        { "cry_wailord_doubles", 15360, 0, 120, 20, 225, 0, 1, 0x3C, 0x40, 0x50, 0x40, 0x78, 0x14, 0x00, 0xE1 },
        /* CRY_MODE_ROAR_2 on Cry_Pidgey: pitch 15616 (b = 0x3D80 -> key 0x3D, tune 0x40, tune2 0x50), length 60 (0x3C 0x00), release 225 */
        { "cry_pidgey_roar2", 15616, 0, 120, 60, 225, 0, 1, 0x3D, 0x40, 0x50, 0x40, 0x78, 0x3C, 0x00, 0xE1 },
        /* Milestone 4 slice 5, the reverse cries (gCryTable_Reverse's entries; the type byte is the
         * voice's, the template does not change): cry_reverse Cry_Bulbasaur at CRY_MODE_NORMAL's defaults */
        { "cry_bulbasaur_reverse", 15360, 0, 120, 140, 0, 0, 1, 0x3C, 0x40, 0x50, 0x40, 0x78, 0x8C, 0x00, 0x00 },
        /* CRY_MODE_GROWL_1 on cry_reverse Cry_Jynx: pitch 15200 (b = 0x3BE0 -> key 0x3B, tune 0x70, tune2 (16 + 0x70) & 0x7F = 0x00),
         * length 15 (0x0F 0x00), release 125 (0x7D) */
        { "cry_jynx_growl1", 15200, 0, 120, 15, 125, 0, 1, 0x3B, 0x70, 0x00, 0x40, 0x78, 0x0F, 0x00, 0x7D },
        /* CRY_MODE_ECHO_START on cry_reverse Cry_Pidgey: pitch 15600 (b = 0x3D70 -> key 0x3D, tune 0x38, tune2 0x48),
         * volume 90 (VOL 0x5A), length 25 (0x19 0x00), release 100 (0x64); the one-track form with chorus 0 (the complete mode is the slice 6 row below) */
        { "cry_pidgey_echo_start", 15600, 0, 90, 25, 100, 0, 1, 0x3D, 0x38, 0x48, 0x40, 0x5A, 0x19, 0x00, 0x64 },
        /* Milestone 4 slice 6, the chorus modes (SetPokemonCryChorus: a nonzero value sets trackCount 2 and
         * overwrites TUNE2 with (chorus + TUNE) & 0x7F, discarding the pitch setter's +16 carry):
         * CRY_MODE_ENCOUNTER on Cry_Bulbasaur: release 225, pitch 15600 (key 0x3D, tune 0x38), chorus 20 -> TUNE2 (20 + 56) & 0x7F = 0x4C, volume 90 */
        { "cry_bulbasaur_encounter", 15600, 0, 90, 140, 225, 20, 2, 0x3D, 0x38, 0x4C, 0x40, 0x5A, 0x8C, 0x00, 0xE1 },
        /* CRY_MODE_ECHO_END on Cry_Jynx: release 220, pitch 15555 (b = 0x3D43 -> key 0x3D, tune 0x21), chorus 192 (the s8 -64)
         * -> TUNE2 (192 + 33) & 0x7F = 0x61, volume 70 (VOL 0x46) */
        { "cry_jynx_echo_end", 15555, 0, 70, 140, 220, (int8_t)192, 2, 0x3D, 0x21, 0x61, 0x40, 0x46, 0x8C, 0x00, 0xDC },
        /* CRY_MODE_ECHO_START complete on cry_reverse Cry_Pidgey: cry_pidgey_echo_start's bytes plus chorus 192
         * -> trackCount 2, TUNE2 (192 + 56) & 0x7F = 0x78 */
        { "cry_pidgey_echo_start_chorus", 15600, 0, 90, 25, 100, (int8_t)192, 2, 0x3D, 0x38, 0x78, 0x40, 0x5A, 0x19, 0x00, 0x64 },
        /* Milestone 4 slice 7, CRY_MODE_HIGH_PITCH whole (length 50, release 200, pitch 15800, chorus 20, volume 90):
         * b = 0x3E38 -> key 0x3E, tune 0x1C (cry_bulbasaur_howl's), the chorus overwriting TUNE2 with (20 + 28) & 0x7F = 0x30;
         * xWAIT 0x32 0x00, xRELE 0xC8, VOL 0x5A; the same bytes on Cry_Jynx and Cry_Wailord (the sample is the tone word) */
        { "cry_jynx_high_pitch", 15800, 0, 90, 50, 200, 20, 2, 0x3E, 0x1C, 0x30, 0x40, 0x5A, 0x32, 0x00, 0xC8 },
        { "cry_wailord_high_pitch", 15800, 0, 90, 50, 200, 20, 2, 0x3E, 0x1C, 0x30, 0x40, 0x5A, 0x32, 0x00, 0xC8 },
    };
    for (int k = 0; k < 14; k++) {
        M4ACryParams v = d;
        v.pitch = variants[k].pitch; v.pan = variants[k].pan; v.volume = variants[k].volume;
        v.length = variants[k].length; v.release = variants[k].release; v.chorus = variants[k].chorus;
        M4ASong *vs = m4a_cry_song_new(&v);
        ASSERT(vs != NULL, "cry: variant song built");
        if (!vs) continue;
        char msg[96];
        snprintf(msg, sizeof(msg), "cry: %s TIE key / TUNE / TUNE2 / PAN / VOL bytes", variants[k].name);
        ASSERT(vs->data[0x2A] == variants[k].key && vs->data[0x12] == variants[k].tune
               && vs->data[0x19] == variants[k].tune2 && vs->data[0x28] == variants[k].panByte
               && vs->data[0x1D] == variants[k].volByte, msg);
        snprintf(msg, sizeof(msg), "cry: %s trackCount byte (0x00) and M4ASong.trackCount", variants[k].name);
        ASSERT(vs->data[0x00] == variants[k].trackCount && vs->trackCount == variants[k].trackCount, msg);
        snprintf(msg, sizeof(msg), "cry: %s xWAIT u16 (0x2E, 0x2F) / xRELE (0x26) bytes", variants[k].name);
        ASSERT(vs->data[0x2E] == variants[k].waitLo && vs->data[0x2F] == variants[k].waitHi
               && vs->data[0x26] == variants[k].releaseByte, msg);
        int same = 1;
        for (int i = 0; i < M4A_CRY_SONG_SIZE; i++)
            if (i != 0x00 && i != 0x2A && i != 0x12 && i != 0x19 && i != 0x28 && i != 0x1D && i != 0x2E && i != 0x2F && i != 0x26
                && vs->data[i] != base->data[i])
                same = 0;
        snprintf(msg, sizeof(msg), "cry: %s differs from the default song in those bytes only", variants[k].name);
        ASSERT(same, msg);
        m4a_song_free(vs);
    }
    m4a_song_free(base);
}

/* gCryTable's index space (voicegroup_cry_table_entry): the `cry` lines
 * after `gCryTable::` and, continuing the count, the `cry_reverse` lines
 * after `gCryTable_Reverse::` -- the two labels assemble into one block, so
 * the ROM's gCryTable[388] is gCryTable_Reverse[0] (type 0x30, the same
 * sample).  A miniature table in a temporary project root. */
static void test_cry_table_index(void)
{
    printf("Testing the gCryTable index space (cry / cry_reverse)...\n");
#ifdef _WIN32
    printf("  (skipped: needs mkdtemp)\n");
#else
    char root[] = "/tmp/poryaaaa_cry_tables_XXXXXX";
    if (!mkdtemp(root)) { ASSERT(0, "cry table: mkdtemp"); return; }
    char dir[320], path[320];
    snprintf(dir, sizeof(dir), "%s/sound", root);
    mkdir(dir, 0700);
    snprintf(path, sizeof(path), "%s/sound/cry_tables.inc", root);
    FILE *f = fopen(path, "w");
    ASSERT(f != NULL, "cry table: wrote the test table");
    if (!f) { rmdir(dir); rmdir(root); return; }
    fputs("\t.align 2\n"
          "gCryTable::\n"
          "\tcry Cry_Bulbasaur\n"
          "\tcry Cry_Ivysaur\n"
          "\t@ a comment line\n"
          "\tcry\tCry_Pidgey\n"
          "\n"
          "\t.align 2\n"
          "gCryTable_Reverse::\n"
          "\tcry_reverse Cry_Bulbasaur\n"
          "\tcry_reverse Cry_Ivysaur\n"
          "\tcry_reverse Cry_Pidgey\n"
          "\n"
          "\t.align 2\n"
          "gOtherTable::\n"
          "\tcry Cry_Venusaur\n", f);
    fclose(f);

    static const struct { int index; const char *symbol; uint8_t type; } expect[] = {
        { 0, "Cry_Bulbasaur", VOICE_CRY }, { 1, "Cry_Ivysaur", VOICE_CRY }, { 2, "Cry_Pidgey", VOICE_CRY },
        { 3, "Cry_Bulbasaur", VOICE_CRY_REVERSE }, { 4, "Cry_Ivysaur", VOICE_CRY_REVERSE },
        { 5, "Cry_Pidgey", VOICE_CRY_REVERSE },
    };
    for (size_t k = 0; k < sizeof(expect) / sizeof(expect[0]); k++) {
        char sym[64] = "";
        uint8_t type = 0xFF;
        int rc = voicegroup_cry_table_entry(root, expect[k].index, sym, sizeof(sym), &type);
        char msg[128];
        snprintf(msg, sizeof(msg), "cry table: index %d is %s %s (type 0x%02X)", expect[k].index,
                 (expect[k].type & VOICE_TYPE_REV) ? "cry_reverse" : "cry", expect[k].symbol, expect[k].type);
        ASSERT(rc == 0 && strcmp(sym, expect[k].symbol) == 0 && type == expect[k].type, msg);
        if (rc != 0 || strcmp(sym, expect[k].symbol) != 0 || type != expect[k].type)
            printf("  got rc %d '%s' type 0x%02X\n", rc, sym, type);
    }
    char sym[64];
    ASSERT_EQ(voicegroup_cry_table_entry(root, 6, sym, sizeof(sym), NULL), -1,
              "cry table: the next label's entries are not in the table (index 6 missing)");
    ASSERT_EQ(voicegroup_cry_table_entry(root, -1, sym, sizeof(sym), NULL), -1, "cry table: negative index");
    ASSERT_EQ(voicegroup_cry_table_symbol(root, 4, sym, sizeof(sym)), 0, "cry table: the symbol-only form reaches the reverse entries");
    ASSERT_EQ(strcmp(sym, "Cry_Ivysaur"), 0, "cry table: symbol-only form, index 4");
    ASSERT_EQ(voicegroup_cry_table_entry("/nonexistent/poryaaaa", 0, sym, sizeof(sym), NULL), -1, "cry table: missing file");

    /* The vanilla layout when the project is at hand: 388 `cry` lines, so
     * 388 / 403 / 511 are the reversed Bulbasaur / Pidgey / Jynx (the
     * ROM's gCryTable_Reverse at gCryTable + 388 * 12, type 0x30). */
    const char *home = getenv("HOME");
    char vanilla[320];
    snprintf(vanilla, sizeof(vanilla), "%s/pokeemerald", home ? home : "");
    uint8_t type = 0;
    if (home && voicegroup_cry_table_entry(vanilla, 388, sym, sizeof(sym), &type) == 0) {
        ASSERT(strcmp(sym, "Cry_Bulbasaur") == 0 && type == VOICE_CRY_REVERSE, "cry table: pokeemerald index 388 = cry_reverse Cry_Bulbasaur");
        ASSERT(voicegroup_cry_table_entry(vanilla, 403, sym, sizeof(sym), &type) == 0
               && strcmp(sym, "Cry_Pidgey") == 0 && type == VOICE_CRY_REVERSE, "cry table: pokeemerald index 403 = cry_reverse Cry_Pidgey");
        ASSERT(voicegroup_cry_table_entry(vanilla, 511, sym, sizeof(sym), &type) == 0
               && strcmp(sym, "Cry_Jynx") == 0 && type == VOICE_CRY_REVERSE, "cry table: pokeemerald index 511 = cry_reverse Cry_Jynx");
        ASSERT(voicegroup_cry_table_entry(vanilla, 123, sym, sizeof(sym), &type) == 0
               && strcmp(sym, "Cry_Jynx") == 0 && type == VOICE_CRY, "cry table: pokeemerald index 123 = cry Cry_Jynx");
        ASSERT_EQ(voicegroup_cry_table_entry(vanilla, 776, sym, sizeof(sym), &type), -1, "cry table: pokeemerald has 776 entries");
    } else {
        printf("  (no ~/pokeemerald: vanilla indices not checked)\n");
    }

    unlink(path);
    rmdir(dir);
    rmdir(root);
#endif
}

/* XCMD xRELE (ply_xrele, m4a.c): the byte becomes the track tone's release,
 * so a note started after it releases at that rate instead of the voice's.
 * The cry song carries SetPokemonCryRelease's value this way (0 at the
 * defaults; the game's other cry modes use 200 / 225). */
static void test_sequencer_xrele(void)
{
    printf("Testing XCMD xRELE...\n");
    ToneData voices[128];
    seq_test_voices(voices);           /* voice 0: square 1, release 0 */
    M4ASong *song = parse_song_text(
        "\t.equ\tt_grp, voicegroup_test\n\t.equ\tt_pri, 0\n\t.equ\tt_rev, 0\n\t.equ\tt_mvl, 127\n"
        "\t.equ\tt_key, 0\n\t.equ\tt_tbs, 1\n\t.section .rodata\n\t.global\tt\n\t.align\t2\n"
        "t_1:\n\t.byte\tVOICE , 0\n\t.byte\tXCMD , xRELE , 5\n\t.byte\tN04 , Cn3 , v127\n"
        "\t.byte\tW24\n\t.byte\tFINE\n\t.align\t2\nt:\n\t.byte\t1, 0, t_pri, t_rev\n\t.word\tt_grp\n\t.word\tt_1\n");
    ASSERT(song != NULL, "xrele: song parses");
    if (!song) return;
    M4AEngine engine;
    m4a_engine_init(&engine, 48000.0f);
    m4a_engine_set_voicegroup(&engine, voices);
    M4ASequencer seq;
    m4a_sequencer_init(&seq, &engine);
    m4a_sequencer_start(&seq, song);
    m4a_sequencer_frame(&seq);         /* the start tick: VOICE, xRELE 5, the note */
    ASSERT_EQ(engine.tracks[0].currentVoice.release, 5, "xrele: the track tone's release");
    ASSERT_EQ(voices[0].release, 0, "xrele: the voicegroup's voice is untouched");
    ASSERT_EQ(engine.cgbChannels[0].release, 5, "xrele: the note started after it carries the release");
    ASSERT_EQ(seq.tracks[0].cmdPtr, song->trackOffsets[0] + 2 + 3 + 3 + 1, "xrele: one operand byte consumed (VOICE, XCMD xRELE n, the note, then the W24 that ends the tick)");
    m4a_sequencer_detach(&seq);
    m4a_engine_destroy(&engine);
    m4a_song_free(song);
}

/* One cry, frame by frame, against its probe: Cry_Pidgey (gCryTable 15,
 * 1902 DPCM samples at 10512 Hz) on cry player 0, build/probe/cry_pidgey.txt
 * (docs/ACCURACY.md, appendix A).  The engine's mixer plays a
 * stand-in sample of the same length and rate, so the channel dies on the
 * same frame as mgba's (14) and the tick's chain walk, the EOT at tick 141
 * (frame 143, the sample long gone) and the finished tick follow. */
static void test_sequencer_cycles_cry(void)
{
    printf("Testing sequencer CgbSound entry on a cry (cry_pidgey probe)...\n");
    static int8_t decoded[1902 + 2 * M4A_DPCM_BLOCK_SAMPLES];
    WaveData *wd = make_dpcm_wave(1902, 0, 0, decoded);
    wd->freq = 10512u << 10;            /* every cry sample's rate */
    static ToneData voices[128];
    memset(voices, 0, sizeof(voices));
    voices[0].type = VOICE_CRY;         /* `cry` macro: compressed, key 60, 255 / 0 / 255 / 0 */
    voices[0].key = 60;
    voices[0].wav = wd;
    voices[0].attack = 0xFF;
    voices[0].decay = 0;
    voices[0].sustain = 0xFF;
    voices[0].release = 0;

    M4ACryParams p;
    m4a_cry_params_default(&p);
    M4ASong *song = m4a_cry_song_new(&p);
    ASSERT(song != NULL, "cry cycles: song built");
    if (!song) { free(wd); return; }

    M4AEngine engine;
    m4a_engine_init(&engine, 48000.0f);
    m4a_engine_set_voicegroup(&engine, voices);
    M4ASequencer seq;
    m4a_sequencer_init(&seq, &engine);
    ASSERT_EQ(m4a_sequencer_set_player(&seq, M4A_PLAYER_CRY0), 0, "cry cycles: cry player 0");
    m4a_engine_boot(&engine, false);
    m4a_sequencer_start(&seq, song);
    ASSERT_EQ(seq.trackCount, 1, "cry cycles: one track on the two-track player");

    /* mgba's CgbSound entry, frames 3..145 (cry_pidgey.txt.frames). */
    static float outL[4096], outR[4096];
    int mismatches = 0;
    for (int k = 3; k <= 145; k++) {
        int expected = k == 3 ? 4077 : k < 14 ? 1596 : k == 14 ? 1689 : k < 143 ? 1576
                     : k == 143 ? 1810 : k == 144 ? 1192 : 1007;
        advance_frames(&engine, 1, outL, outR);
        if (seq.frameCycles != expected) {
            if (mismatches++ < 5)
                printf("  frame %d: engine %d, mgba %d\n", k, seq.frameCycles, expected);
        }
    }
    ASSERT_EQ(mismatches, 0, "sequencer: CgbSound entry matches the cry_pidgey probe on frames 3..145 (cry 0)");
    ASSERT(m4a_sequencer_finished(&seq), "sequencer: the cry finished");
    ASSERT_EQ(seq.noteCount, 1, "sequencer: one TIE");
    ASSERT_EQ(seq.clock, 142, "sequencer: 142 ticks (141 command ticks: xwait waits on 3..142 and expires with EOT / FINE on 143; the finished tick on 144)");

    m4a_sequencer_detach(&seq);
    m4a_engine_destroy(&engine);
    m4a_song_free(song);
    free(wd);
}

/* XCMD 0x0D (ply_xcmd_0D, m4a.c): the word becomes track->unk_3C, which
 * ply_note stores as the new DirectSound channel's count (m4a_1.s
 * _081DDCCE) and SoundMainRAM's SF_START pass turns into
 * currentPointer = data + count, count = size - count: the sample starts
 * that many samples in.  The cry song carries SetPokemonCryProgress's
 * value this way (0 in every PlayCryInternal mode), so the harness never
 * exercises it; these are its gate.  First the mixer on hand-built
 * channels, then the sequencer's entry point. */
static void test_pcm_start_offset(void)
{
    printf("Testing the sample start offset (XCMD 0x0D / SoundChannel.count at SF_START)...\n");
    M4AEngine e;
    m4a_engine_init(&e, 44100.0f);
    e.pcmDivFreq = 1;
    static int8_t frame[GBA_PCM_DMA_BUF_SIZE * 2];
    const int spv = e.pcmSamplesPerVBlank;

    /* Uncompressed, looped 8-sample ramp, FIX: the frame starts at sample
     * 3 (40, 50, ...) and wraps as usual. */
    {
        WaveData *wd = ramp_wave(8, 1, 0);
        M4APCMChannel ch;
        memset(&ch, 0, sizeof(ch));
        m4a_pcm_channel_start_at(&ch, wd, VOICE_DIRECTSOUND | VOICE_TYPE_FIX, 3);
        ASSERT(ch.currentPointer == wd->data + 3, "offset: SF_START's currentPointer = data + count");
        ASSERT_EQ(ch.count, 8 - 3, "offset: SF_START's count = size - count");
        ch.status = CHN_ENV_SUSTAIN | CHN_LOOP;
        ch.envelopeVolumeRight = ch.envelopeVolumeLeft = 128;
        mix_frame(&e, &ch, frame);
        int ok = 1;
        for (int i = 0; i < spv; i++)
            if (frame[i] != PCM_OUT(10 * ((3 + i) % 8 + 1))) ok = 0;
        ASSERT(ok, "offset: the fix loop plays from sample 3 and wraps");
        free(wd);
    }
    /* m4a_pcm_channel_start is the offset-0 case. */
    {
        WaveData *wd = ramp_wave(8, 1, 0);
        M4APCMChannel ch;
        memset(&ch, 0, sizeof(ch));
        m4a_pcm_channel_start(&ch, wd, VOICE_DIRECTSOUND);
        ASSERT(ch.currentPointer == wd->data && ch.count == 8, "offset: start without an offset is offset 0");
        m4a_pcm_channel_start_at(&ch, wd, VOICE_DIRECTSOUND, 1000);
        ASSERT(ch.currentPointer == wd->data + 8 && ch.count == 0, "offset: an offset past the sample is clamped to its end (no host read past the data)");
        free(wd);
    }

    /* DPCM (the cry samples' type): Unk1 reads the pointer back as a
     * sample index, so a block-aligned offset (64) starts at block 1's
     * first sample and an offset inside a block (37) decodes block 0 and
     * starts at its 38th; the channel stops after size - offset samples
     * and leaves the rest of the frame alone. */
    {
        enum { SIZE = 200 };
        static int8_t decoded[SIZE + 2 * M4A_DPCM_BLOCK_SAMPLES];
        WaveData *wd = make_dpcm_wave(SIZE, 0, 0, decoded);
        static const int offsets[2] = { 64, 37 };
        for (int k = 0; k < 2; k++) {
            int off = offsets[k];
            M4APCMChannel ch;
            memset(&ch, 0, sizeof(ch));
            m4a_pcm_channel_start_at(&ch, wd, VOICE_CRY, (uint32_t)off);
            ch.status = CHN_ENV_SUSTAIN;
            ch.frequency = 0x800000;
            ch.envelopeVolumeLeft = ch.envelopeVolumeRight = 128;
            mix_frame(&e, &ch, frame);
            int n = SIZE - off, ok = 1, rest = 1;
            for (int i = 0; i < n; i++)
                if (frame[i] != PCM_OUT(decoded[off + i])) ok = 0;
            for (int i = n; i < spv; i++)
                if (frame[i] != 0) rest = 0;
            char msg[96];
            snprintf(msg, sizeof(msg), "offset: dpcm at offset %d plays decoded[%d..%d]", off, off, SIZE - 1);
            ASSERT(ok, msg);
            snprintf(msg, sizeof(msg), "offset: dpcm at offset %d stops after size - offset samples", off);
            ASSERT(rest && ch.status == 0, msg);
        }
        free(wd);
    }
}

static void test_sequencer_xcmd_0D(void)
{
    printf("Testing XCMD 0x0D through the sequencer...\n");
    static ToneData voices[128];
    memset(voices, 0, sizeof(voices));
    WaveData *wd = ramp_wave(64, 1, 0);
    voices[0].type = VOICE_DIRECTSOUND;
    voices[0].key = 60;
    voices[0].wav = wd;
    voices[0].attack = 0xFF; voices[0].decay = 0; voices[0].sustain = 0xFF; voices[0].release = 0;
    /* XCMD 0x0D 0x00000025 (little-endian 37), then a TIE; the second
     * start of the song must come up with unk_3C cleared (Clear64byte). */
    M4ASong *song = parse_song_text(
        "\t.equ\tt_grp, voicegroup_test\n\t.equ\tt_pri, 0\n\t.equ\tt_rev, 0\n\t.equ\tt_mvl, 127\n"
        "\t.equ\tt_key, 0\n\t.equ\tt_tbs, 1\n\t.section .rodata\n\t.global\tt\n\t.align\t2\n"
        "t_1:\n\t.byte\tVOICE , 0\n\t.byte\tXCMD , 13 , 37 , 0 , 0 , 0\n\t.byte\tTIE , Cn3 , v127\n"
        "\t.byte\tW24\n\t.byte\tFINE\n\t.align\t2\nt:\n\t.byte\t1, 0, t_pri, t_rev\n\t.word\tt_grp\n\t.word\tt_1\n");
    ASSERT(song != NULL, "xcmd_0D: song parses");
    if (!song) { free(wd); return; }
    M4AEngine engine;
    m4a_engine_init(&engine, 48000.0f);
    m4a_engine_set_voicegroup(&engine, voices);
    M4ASequencer seq;
    m4a_sequencer_init(&seq, &engine);
    m4a_sequencer_start(&seq, song);
    m4a_sequencer_frame(&seq);         /* the start tick: VOICE, XCMD 0D, the TIE */
    ASSERT_EQ(engine.tracks[0].sampleStart, 37u, "xcmd_0D: the word lands in the track's unk_3C");
    ASSERT_EQ(seq.tracks[0].cmdPtr, song->trackOffsets[0] + 2 + 6 + 3 + 1, "xcmd_0D: four operand bytes consumed");
    M4APCMChannel *ch = &engine.pcmChannels[0];
    ASSERT((ch->status & CHN_ON) != 0, "xcmd_0D: the TIE took a DirectSound channel");
    ASSERT(ch->currentPointer == wd->data + 37, "xcmd_0D: ply_note's channel starts 37 samples in");
    ASSERT_EQ(ch->count, 64 - 37, "xcmd_0D: its count is size - 37");
    ASSERT_EQ(engine.tracks[0].sampleStart, 37u, "xcmd_0D: unk_3C persists for the next note (ply_note copies, never clears)");
    m4a_sequencer_start(&seq, song);   /* MPlayStart again: Clear64byte at the track's start tick */
    ASSERT_EQ(engine.tracks[0].sampleStart, 37u, "xcmd_0D: untouched until the track's start tick runs");
    m4a_sequencer_frame(&seq);
    ASSERT_EQ(engine.tracks[0].sampleStart, 37u, "xcmd_0D: the restarted track read the XCMD again");
    m4a_sequencer_detach(&seq);
    m4a_engine_destroy(&engine);
    m4a_song_free(song);
    free(wd);

    /* The reset itself: a song without the XCMD after one with it. */
    wd = ramp_wave(64, 1, 0);
    voices[0].wav = wd;
    M4ASong *plain = parse_song_text(
        "\t.equ\tt_grp, voicegroup_test\n\t.equ\tt_pri, 0\n\t.equ\tt_rev, 0\n\t.equ\tt_mvl, 127\n"
        "\t.equ\tt_key, 0\n\t.equ\tt_tbs, 1\n\t.section .rodata\n\t.global\tt\n\t.align\t2\n"
        "t_1:\n\t.byte\tVOICE , 0\n\t.byte\tTIE , Cn3 , v127\n\t.byte\tW24\n\t.byte\tFINE\n"
        "\t.align\t2\nt:\n\t.byte\t1, 0, t_pri, t_rev\n\t.word\tt_grp\n\t.word\tt_1\n");
    ASSERT(plain != NULL, "xcmd_0D: plain song parses");
    if (!plain) { free(wd); return; }
    m4a_engine_init(&engine, 48000.0f);
    m4a_engine_set_voicegroup(&engine, voices);
    m4a_sequencer_init(&seq, &engine);
    engine.tracks[0].sampleStart = 99;  /* left over from an earlier song */
    m4a_sequencer_start(&seq, plain);
    m4a_sequencer_frame(&seq);
    ASSERT_EQ(engine.tracks[0].sampleStart, 0u, "xcmd_0D: the track's start tick clears unk_3C (Clear64byte)");
    ASSERT(engine.pcmChannels[0].currentPointer == wd->data, "xcmd_0D: the note starts at sample 0");
    m4a_sequencer_detach(&seq);
    m4a_engine_destroy(&engine);
    m4a_song_free(plain);
    free(wd);
}

/* ply_endtie's walk cost by chain state (docs/ACCURACY.md, appendix A): +28 when the
 * first chained channel holds the key (measured on cry_jynx_faint's EOT
 * frame: 82 against the empty chain's 54), +18 est. per channel passed
 * over.  Three songs that differ only in the EOT's key, two live channels
 * on the track (newest first: Dn3, Cn3). */
static void test_sequencer_endtie_walk(void)
{
    printf("Testing ply_endtie's walk cost...\n");
    static ToneData voices[128];
    memset(voices, 0, sizeof(voices));
    WaveData *wd = ramp_wave(64, 1, 0);
    voices[0].type = VOICE_DIRECTSOUND;
    voices[0].key = 60;
    voices[0].wav = wd;
    voices[0].attack = 0xFF; voices[0].decay = 0; voices[0].sustain = 0xFF; voices[0].release = 0;
    static const char *keys[3] = { "Dn3", "Cn3", "En3" };   /* match first; skip then match; skip twice */
    int32_t eotFrame[3] = { 0, 0, 0 };
    bool live[3] = { false, false, false };
    for (int k = 0; k < 3; k++) {
        char text[1024];
        snprintf(text, sizeof(text),
            "\t.equ\tt_grp, voicegroup_test\n\t.equ\tt_pri, 0\n\t.equ\tt_rev, 0\n\t.equ\tt_mvl, 127\n"
            "\t.equ\tt_key, 0\n\t.equ\tt_tbs, 1\n\t.section .rodata\n\t.global\tt\n\t.align\t2\n"
            "t_1:\n\t.byte\tVOICE , 0\n\t.byte\tTIE , Cn3 , v127\n\t.byte\tTIE , Dn3\n\t.byte\tW01\n"
            "\t.byte\tEOT , %s\n\t.byte\tW01\n\t.byte\tFINE\n"
            "\t.align\t2\nt:\n\t.byte\t1, 0, t_pri, t_rev\n\t.word\tt_grp\n\t.word\tt_1\n", keys[k]);
        M4ASong *song = parse_song_text(text);
        ASSERT(song != NULL, "endtie walk: song parses");
        if (!song) continue;
        M4AEngine engine;
        m4a_engine_init(&engine, 48000.0f);
        m4a_engine_set_voicegroup(&engine, voices);
        M4ASequencer seq;
        m4a_sequencer_init(&seq, &engine);
        m4a_engine_boot(&engine, false);
        m4a_sequencer_start(&seq, song);
        static float outL[4096], outR[4096];
        advance_frames(&engine, 1, outL, outR);     /* frame 3 (the start tick): VOICE, the two TIEs, W01 */
        live[k] = (engine.pcmChannels[0].status & CHN_ON) && (engine.pcmChannels[1].status & CHN_ON);
        advance_frames(&engine, 1, outL, outR);     /* frame 4: EOT key, W01 */
        eotFrame[k] = seq.frameCycles;
        m4a_sequencer_detach(&seq);
        m4a_engine_destroy(&engine);
        m4a_song_free(song);
    }
    ASSERT(live[0] && live[1] && live[2], "endtie walk: both channels were live at the EOT (the walk saw two)");
    ASSERT_EQ(eotFrame[1] - eotFrame[0], M4A_CYC_ENDTIE_SKIP, "endtie walk: a channel passed over before the match costs the skip");
    ASSERT_EQ(eotFrame[2] - eotFrame[0], 2 * M4A_CYC_ENDTIE_SKIP - M4A_CYC_ENDTIE_MATCH, "endtie walk: no match walks the whole chain and skips the match cost");
    ASSERT_EQ(M4A_CYC_ENDTIE_MATCH, 28, "endtie walk: the match cost is the probe's 82 - 54 (cry_jynx_faint frame 143)");
    free(wd);
}

/* Variant (a) of the cry corpus, frame by frame against its probe:
 * Cry_Jynx (23524 DPCM samples at 10512 Hz) at CRY_MODE_FAINT's pitch
 * 14440, build/probe/cry_jynx_faint.txt.  The sample outlives the 140-tick
 * EOT, so frame 143 is the first measured EOT with a chained channel
 * holding the key (ply_endtie +28) and ply_fine then unlinks a live
 * channel (+70); the xwait ticks keep the live-channel walk (1596) to the
 * end. */
static void test_sequencer_cycles_cry_faint(void)
{
    printf("Testing sequencer CgbSound entry on a cry variant (cry_jynx_faint probe)...\n");
    static int8_t decoded[23524 + 2 * M4A_DPCM_BLOCK_SAMPLES];
    WaveData *wd = make_dpcm_wave(23524, 0, 0, decoded);
    wd->freq = 10512u << 10;
    static ToneData voices[128];
    memset(voices, 0, sizeof(voices));
    voices[0].type = VOICE_CRY;
    voices[0].key = 60;
    voices[0].wav = wd;
    voices[0].attack = 0xFF; voices[0].decay = 0; voices[0].sustain = 0xFF; voices[0].release = 0;

    M4ACryParams p;
    m4a_cry_params_default(&p);
    p.pitch = 14440;                    /* CRY_MODE_FAINT */
    M4ASong *song = m4a_cry_song_new(&p);
    ASSERT(song != NULL, "cry faint cycles: song built");
    if (!song) { free(wd); return; }
    ASSERT_EQ(song->data[0x2A], 0x38, "cry faint cycles: TIE key 56");
    ASSERT_EQ(song->data[0x12], 0x74, "cry faint cycles: TUNE 116");

    M4AEngine engine;
    m4a_engine_init(&engine, 48000.0f);
    m4a_engine_set_voicegroup(&engine, voices);
    M4ASequencer seq;
    m4a_sequencer_init(&seq, &engine);
    ASSERT_EQ(m4a_sequencer_set_player(&seq, M4A_PLAYER_CRY0), 0, "cry faint cycles: cry player 0");
    m4a_engine_boot(&engine, false);
    m4a_sequencer_start(&seq, song);

    static float outL[4096], outR[4096];
    int mismatches = 0;
    bool liveAtEot = false;
    for (int k = 3; k <= 145; k++) {
        int expected = k == 3 ? 4077 : k < 143 ? 1596 : k == 143 ? 1928 : k == 144 ? 1192 : 1007;
        if (k == 143)
            liveAtEot = (engine.pcmChannels[0].status & CHN_ON) != 0;
        advance_frames(&engine, 1, outL, outR);
        if (seq.frameCycles != expected) {
            if (mismatches++ < 5)
                printf("  frame %d: engine %d, mgba %d\n", k, seq.frameCycles, expected);
        }
    }
    ASSERT(liveAtEot, "cry faint cycles: the channel is still sounding when the EOT tick runs");
    ASSERT_EQ(mismatches, 0, "sequencer: CgbSound entry matches the cry_jynx_faint probe on frames 3..145 (cry 0)");
    ASSERT(m4a_sequencer_finished(&seq), "sequencer: the cry finished");
    ASSERT((engine.pcmChannels[0].status & CHN_ON) == 0, "cry faint cycles: the release-0 cut at EOT ended the channel");

    m4a_sequencer_detach(&seq);
    m4a_engine_destroy(&engine);
    m4a_song_free(song);
    free(wd);
}

/* The PCM release envelope on a hand-built channel (SoundMainRAM
 * _081DCFB6.._081DCFC8 / _081DD006, m4a_1.s): once per pass with SF_STOP
 * set, envelopeVolume = envelopeVolume * release >> 8; with pseudoEchoVolume
 * 0 the pass whose product is 0 stores statusFlags 0 and mixes nothing (the
 * `cmp r5, echo; bhi` falls through to `_081DCFC8` -> `_081DCFB0`); the
 * per-channel volume is (masterVolume + 1) * env >> 4, then * rightVolume /
 * leftVolume >> 8.  The game's cry modes use releases 100..225 (slice 4's
 * corpus: 200 and 225, the probe's death frames 161 and 53). */
static void test_pcm_release_envelope(void)
{
    printf("Testing the PCM release envelope (release 225 / 200 against the GBA's arithmetic)...\n");
    static const uint8_t seq225[31] = { 224, 196, 172, 151, 132, 116, 101, 88, 77, 67, 58, 50, 43, 37, 32, 28,
                                        24, 21, 18, 15, 13, 11, 9, 7, 6, 5, 4, 3, 2, 1, 0 };
    static const uint8_t seq200[19] = { 199, 155, 121, 94, 73, 57, 44, 34, 26, 20, 15, 11, 8, 6, 4, 3, 2, 1, 0 };
    const struct { uint8_t release; const uint8_t *seq; int n; } cases[2] = {
        { 225, seq225, 31 }, { 200, seq200, 19 } };

    for (int c = 0; c < 2; c++) {
        /* The tick alone: the sequence, the volumes and the stop pass. */
        M4APCMChannel ch;
        memset(&ch, 0, sizeof(ch));
        ch.status = CHN_ENV_SUSTAIN | CHN_STOP;          /* ply_endtie's SF_STOP on a sustaining channel */
        ch.envelopeVolume = 0xFF;                        /* attack 255 reached it; decay 0 / sustain 255 hold it */
        ch.release = cases[c].release;
        ch.rightVolume = 0xEE; ch.leftVolume = 0x77;     /* arbitrary ChnVolSetAsm results */
        int bad = -1, volBad = -1, stoppedAt = -1;
        for (int k = 0; k < cases[c].n; k++) {
            m4a_pcm_channel_tick(&ch, 12);
            uint8_t want = cases[c].seq[k];
            if (want == 0) {
                if ((ch.status & CHN_ON) == 0 && stoppedAt < 0) stoppedAt = k;
                break;
            }
            if (!(ch.status & CHN_ON) || ch.envelopeVolume != want) { if (bad < 0) bad = k; }
            uint32_t vol = (13u * want) >> 4;
            if (ch.envelopeVolumeRight != ((0xEEu * vol) >> 8) || ch.envelopeVolumeLeft != ((0x77u * vol) >> 8)) { if (volBad < 0) volBad = k; }
        }
        char msg[128];
        if (bad >= 0) printf("  release %d pass %d: engine %d, GBA %d\n", cases[c].release, bad, ch.envelopeVolume, cases[c].seq[bad]);
        snprintf(msg, sizeof(msg), "release %d: envelopeVolume follows env * release >> 8 on every mixed pass (%d passes)", cases[c].release, cases[c].n - 1);
        ASSERT_EQ(bad, -1, msg);
        snprintf(msg, sizeof(msg), "release %d: the per-side volumes are rightVolume * ((13 * env) >> 4) >> 8 on every pass", cases[c].release);
        ASSERT_EQ(volBad, -1, msg);
        snprintf(msg, sizeof(msg), "release %d: the channel stops on pass %d, the first whose product is 0", cases[c].release, cases[c].n - 1);
        ASSERT_EQ(stoppedAt, cases[c].n - 1, msg);
        ASSERT_EQ(ch.status, 0, "release: the stop clears every status flag");
    }

    /* Through the mixer, as m4a_sound_main_ram runs a channel (the tick,
     * then the mix if the channel is still on): a constant +100 sample at
     * FIX, release 200 from 255 -- the mixed byte is (100 *
     * envelopeVolumeRight) >> 8 per pass, env 1 (volume 0) is already
     * silent, and the stop pass writes nothing. */
    {
        M4AEngine e;
        m4a_engine_init(&e, 44100.0f);
        e.pcmDivFreq = 1;
        ASSERT_EQ(e.masterVolume, 12, "release: the engine's masterVolume is m4aSoundInit's 12");
        static int8_t frame[GBA_PCM_DMA_BUF_SIZE * 2];
        WaveData *wd = calloc(1, sizeof(WaveData) + 8192 + 1);   /* 20 passes of 224 samples fit */
        wd->size = 8192; wd->freq = 13379 << 10;
        wd->data = (int8_t *)((uint8_t *)wd + sizeof(WaveData));
        memset(wd->data, 100, 8192);
        M4APCMChannel ch;
        memset(&ch, 0, sizeof(ch));
        m4a_pcm_channel_start(&ch, wd, VOICE_DIRECTSOUND | VOICE_TYPE_FIX);
        ch.status = CHN_START;                           /* ply_note's SF_START */
        ch.rightVolume = ch.leftVolume = 0xFF;
        ch.attack = 0xFF; ch.decay = 0; ch.sustain = 0xFF; ch.release = 200;
        m4a_pcm_channel_tick(&ch, e.masterVolume);       /* SF_START: attack to 255 */
        mix_frame(&e, &ch, frame);
        ASSERT_EQ(ch.envelopeVolume, 0xFF, "release: attack 255 reaches 255 on the first pass");
        ASSERT_EQ(frame[0], (int8_t)((100u * ((0xFFu * ((13u * 0xFFu) >> 4)) >> 8)) >> 8), "release: the sustained byte is 100 * (255 * 207 >> 8) >> 8");
        ch.status |= CHN_STOP;                           /* the EOT */
        int bad = -1;
        for (int k = 0; k < 18; k++) {
            m4a_pcm_channel_tick(&ch, e.masterVolume);
            mix_frame(&e, &ch, frame);
            uint32_t vol = (13u * seq200[k]) >> 4;
            int8_t want = (int8_t)((100u * ((0xFFu * vol) >> 8)) >> 8);
            if (ch.envelopeVolume != seq200[k] || frame[0] != want || frame[GBA_PCM_DMA_BUF_SIZE] != want || frame[e.pcmSamplesPerVBlank - 1] != want) {
                if (bad < 0) { bad = k; printf("  pass %d: env %d (want %d), byte %d (want %d)\n", k, ch.envelopeVolume, seq200[k], frame[0], want); }
            }
        }
        ASSERT_EQ(bad, -1, "release 200 through the mixer: 18 release passes at the GBA's envelope and volume");
        ASSERT_EQ(frame[0], 0, "release: the pass at env 1 ((13 * 1) >> 4 = 0) is already silent");
        ASSERT(ch.status & CHN_ON, "release: the channel is still on after the env-1 pass");
        m4a_pcm_channel_tick(&ch, e.masterVolume);
        ASSERT_EQ(ch.status, 0, "release: the 19th pass (product 0) stops the channel before its mix (m4a_sound_main_ram skips it)");
        free(wd);
        m4a_engine_destroy(&e);
    }
}

/* cry_jynx_faint_full (gCryTable 123 with CRY_MODE_FAINT complete: pitch
 * 14440, length 140, release 200) against build/probe/cry_jynx_faint_full.txt
 * (docs/ACCURACY.md, "Cries"): the sequencer frames are
 * cry_jynx_faint's (the EOT at 143 finds the chained channel, 1928; FINE
 * unlinks it), and the channel then releases on its own, 18 mixed passes
 * on frames 143..160 and the stop on 161 (the probe's _081DCFB0 stamp). */
static void test_sequencer_cycles_cry_faint_full(void)
{
    printf("Testing sequencer CgbSound entry and the release tail on a cry mode (cry_jynx_faint_full probe)...\n");
    static int8_t decoded[23524 + 2 * M4A_DPCM_BLOCK_SAMPLES];
    WaveData *wd = make_dpcm_wave(23524, 0, 0, decoded);
    wd->freq = 10512u << 10;
    static ToneData voices[128];
    memset(voices, 0, sizeof(voices));
    voices[0].type = VOICE_CRY;
    voices[0].key = 60;
    voices[0].wav = wd;
    voices[0].attack = 0xFF; voices[0].decay = 0; voices[0].sustain = 0xFF; voices[0].release = 0;

    M4ACryParams p;
    m4a_cry_params_default(&p);
    p.pitch = 14440; p.length = 140; p.release = 200;      /* CRY_MODE_FAINT */
    M4ASong *song = m4a_cry_song_new(&p);
    ASSERT(song != NULL, "cry faint full cycles: song built");
    if (!song) { free(wd); return; }
    ASSERT(song->data[0x2A] == 0x38 && song->data[0x12] == 0x74 && song->data[0x2E] == 0x8C
           && song->data[0x2F] == 0x00 && song->data[0x26] == 0xC8, "cry faint full cycles: TIE key 56, TUNE 116, xWAIT 140, xRELE 200");

    M4AEngine engine;
    m4a_engine_init(&engine, 48000.0f);
    m4a_engine_set_voicegroup(&engine, voices);
    M4ASequencer seq;
    m4a_sequencer_init(&seq, &engine);
    ASSERT_EQ(m4a_sequencer_set_player(&seq, M4A_PLAYER_CRY0), 0, "cry faint full cycles: cry player 0");
    m4a_engine_boot(&engine, false);
    m4a_sequencer_start(&seq, song);

    static float outL[4096], outR[4096];
    static const uint8_t seq200[19] = { 199, 155, 121, 94, 73, 57, 44, 34, 26, 20, 15, 11, 8, 6, 4, 3, 2, 1, 0 };
    int mismatches = 0, envBad = -1, deathFrame = -1;
    bool liveAtEot = false, releaseAtEot = false;
    for (int k = 3; k <= 165; k++) {
        int expected = k == 3 ? 4077 : k < 143 ? 1596 : k == 143 ? 1928 : k == 144 ? 1192 : 1007;
        if (k == 143)
            liveAtEot = (engine.pcmChannels[0].status & CHN_ON) != 0;
        advance_frames(&engine, 1, outL, outR);
        if (seq.frameCycles != expected) {
            if (mismatches++ < 5)
                printf("  frame %d: engine %d, mgba %d\n", k, seq.frameCycles, expected);
        }
        if (k == 143)
            releaseAtEot = engine.pcmChannels[0].release == 200;
        if (k >= 143 && k <= 160) {
            if (!(engine.pcmChannels[0].status & CHN_ON) || engine.pcmChannels[0].envelopeVolume != seq200[k - 143]) {
                if (envBad < 0) { envBad = k; printf("  frame %d: env %d status %02X, GBA %d\n", k, engine.pcmChannels[0].envelopeVolume, engine.pcmChannels[0].status, seq200[k - 143]); }
            }
        }
        if (deathFrame < 0 && k >= 143 && !(engine.pcmChannels[0].status & CHN_ON))
            deathFrame = k;
    }
    ASSERT(liveAtEot, "cry faint full cycles: the channel is still sounding when the EOT tick runs");
    ASSERT(releaseAtEot, "cry faint full cycles: the channel carries xRELE's 200 (ply_xrele -> the track tone -> ply_note)");
    ASSERT_EQ(mismatches, 0, "sequencer: CgbSound entry matches the cry_jynx_faint_full probe on frames 3..165 (cry 0)");
    ASSERT_EQ(envBad, -1, "cry faint full cycles: the release envelope on frames 143..160 is 255 * 200 >> 8 per pass (199 .. 1)");
    ASSERT_EQ(deathFrame, 161, "cry faint full cycles: the channel dies on frame 161, the probe's _081DCFB0 stamp (f162)");
    ASSERT(m4a_sequencer_finished(&seq), "sequencer: the cry finished (the track gone at 143, the channel outliving it)");

    m4a_sequencer_detach(&seq);
    m4a_engine_destroy(&engine);
    m4a_song_free(song);
    free(wd);
}

/* cry_jynx_growl1 (milestone 4 slice 5): cry_reverse Cry_Jynx with
 * CRY_MODE_GROWL_1 (pitch 15200, length 15, release 125) against its probe:
 * the start tick 4077 on frame 3, 1596 to the EOT on frame 18 (1928, the
 * reversed channel still sounding and holding the key), 1192 on 19, 1007
 * after; the release envelope 255 * 125 >> 8 per pass from the EOT frame
 * (124, 60, 29, 14, 6, 2 on frames 18..23) and the channel's death on
 * frame 24, the probe's _081DCFB0 stamp (raw f25).  The channel walks the
 * sample backwards from its end: its index goes down from 23524. */
static void test_sequencer_cycles_cry_growl1_reverse(void)
{
    printf("Testing sequencer CgbSound entry and the release tail on a reversed cry (cry_jynx_growl1 probe)...\n");
    static int8_t decoded[23524 + 2 * M4A_DPCM_BLOCK_SAMPLES];
    WaveData *wd = make_dpcm_wave(23524, 0, 0, decoded);
    wd->freq = 10512u << 10;
    static ToneData voices[128];
    memset(voices, 0, sizeof(voices));
    voices[0].type = VOICE_CRY_REVERSE;
    voices[0].key = 60;
    voices[0].wav = wd;
    voices[0].attack = 0xFF; voices[0].decay = 0; voices[0].sustain = 0xFF; voices[0].release = 0;

    M4ACryParams p;
    m4a_cry_params_default(&p);
    p.pitch = 15200; p.length = 15; p.release = 125;      /* CRY_MODE_GROWL_1 (its reverse flag is the table entry's type) */
    M4ASong *song = m4a_cry_song_new(&p);
    ASSERT(song != NULL, "cry growl1 cycles: song built");
    if (!song) { free(wd); return; }
    ASSERT(song->data[0x2A] == 0x3B && song->data[0x12] == 0x70 && song->data[0x19] == 0x00 && song->data[0x2E] == 0x0F
           && song->data[0x2F] == 0x00 && song->data[0x26] == 0x7D, "cry growl1 cycles: TIE key 59, TUNE 112, TUNE2 0, xWAIT 15, xRELE 125");

    M4AEngine engine;
    m4a_engine_init(&engine, 48000.0f);
    m4a_engine_set_voicegroup(&engine, voices);
    M4ASequencer seq;
    m4a_sequencer_init(&seq, &engine);
    ASSERT_EQ(m4a_sequencer_set_player(&seq, M4A_PLAYER_CRY0), 0, "cry growl1 cycles: cry player 0");
    m4a_engine_boot(&engine, false);
    m4a_sequencer_start(&seq, song);

    static float outL[4096], outR[4096];
    static const uint8_t seq125[7] = { 124, 60, 29, 14, 6, 2, 0 };
    int mismatches = 0, envBad = -1, deathFrame = -1;
    bool liveAtEot = false, releaseAtEot = false, reversed = false, walkingDown = false;
    int idxAfter4 = -1, idxAfter10 = -1;
    for (int k = 3; k <= 30; k++) {
        int expected = k == 3 ? 4077 : k < 18 ? 1596 : k == 18 ? 1928 : k == 19 ? 1192 : 1007;
        if (k == 18)
            liveAtEot = (engine.pcmChannels[0].status & CHN_ON) != 0;
        advance_frames(&engine, 1, outL, outR);
        if (seq.frameCycles != expected) {
            if (mismatches++ < 5)
                printf("  frame %d: engine %d, mgba %d\n", k, seq.frameCycles, expected);
        }
        if (k == 4) {
            reversed = engine.pcmChannels[0].type == VOICE_CRY_REVERSE && (engine.pcmChannels[0].status & CHN_SPECIAL) != 0;
            idxAfter4 = (int)(engine.pcmChannels[0].currentPointer - wd->data);
        }
        if (k == 10)
            idxAfter10 = (int)(engine.pcmChannels[0].currentPointer - wd->data);
        if (k == 18)
            releaseAtEot = engine.pcmChannels[0].release == 125;
        if (k >= 18 && k <= 23) {
            if (!(engine.pcmChannels[0].status & CHN_ON) || engine.pcmChannels[0].envelopeVolume != seq125[k - 18]) {
                if (envBad < 0) { envBad = k; printf("  frame %d: env %d status %02X, GBA %d\n", k, engine.pcmChannels[0].envelopeVolume, engine.pcmChannels[0].status, seq125[k - 18]); }
            }
        }
        if (deathFrame < 0 && k >= 18 && !(engine.pcmChannels[0].status & CHN_ON))
            deathFrame = k;
    }
    walkingDown = idxAfter4 > 0 && idxAfter4 < 23524 && idxAfter10 > 0 && idxAfter10 < idxAfter4;
    ASSERT(reversed, "cry growl1 cycles: the channel carries type 0x30 (CMP | REV) and went through Unk1");
    ASSERT(walkingDown, "cry growl1 cycles: the sample index walks down from the end (frame 10 below frame 4, both inside the data)");
    ASSERT(liveAtEot, "cry growl1 cycles: the reversed channel is still sounding when the EOT tick runs (frame 18)");
    ASSERT(releaseAtEot, "cry growl1 cycles: the channel carries xRELE's 125");
    ASSERT_EQ(mismatches, 0, "sequencer: CgbSound entry matches the cry_jynx_growl1 probe on frames 3..30 (cry 0)");
    ASSERT_EQ(envBad, -1, "cry growl1 cycles: the release envelope on frames 18..23 is 255 * 125 >> 8 per pass (124 .. 2)");
    ASSERT_EQ(deathFrame, 24, "cry growl1 cycles: the channel dies on frame 24, the probe's _081DCFB0 stamp (f25)");
    ASSERT(m4a_sequencer_finished(&seq), "sequencer: the cry finished (the track gone at 18, the channel outliving it)");

    m4a_sequencer_detach(&seq);
    m4a_engine_destroy(&engine);
    m4a_song_free(song);
    free(wd);
}


/* A chorus cry on cry player 0: SetPokemonCryChorus makes the song two
 * tracks (part0: TUNE, GOTO cont; part1: TUNE2, falling into cont), MPlayStart
 * takes min(songHeader->trackCount, mplayInfo->trackCount) = 2 of them, and
 * the start tick runs ply_note on both, so two PCM channels are keyed on the
 * same key from the same sample, a TUNE step apart (cry_bulbasaur_encounter's
 * values: TUNE 56 and 76). */
static void test_sequencer_cry_two_tracks(void)
{
    printf("Testing the sequencer running a two-track (chorus) cry on cry player 0...\n");
    static int8_t decoded[8183 + 2 * M4A_DPCM_BLOCK_SAMPLES];
    WaveData *wd = make_dpcm_wave(8183, 0, 0, decoded);
    wd->freq = 10512u << 10;
    static ToneData voices[128];
    memset(voices, 0, sizeof(voices));
    voices[0].type = VOICE_CRY;
    voices[0].key = 60;
    voices[0].wav = wd;
    voices[0].attack = 0xFF; voices[0].decay = 0; voices[0].sustain = 0xFF; voices[0].release = 0;

    M4ACryParams p;
    m4a_cry_params_default(&p);
    p.pitch = 15600; p.volume = 90; p.release = 225; p.chorus = 20;      /* CRY_MODE_ENCOUNTER */
    M4ASong *song = m4a_cry_song_new(&p);
    ASSERT(song != NULL, "cry two tracks: song built");
    if (!song) { free(wd); return; }
    ASSERT_EQ(song->trackCount, 2, "cry two tracks: the song header says two tracks");

    M4AEngine engine;
    m4a_engine_init(&engine, 48000.0f);
    m4a_engine_set_voicegroup(&engine, voices);
    M4ASequencer seq;
    m4a_sequencer_init(&seq, &engine);
    ASSERT_EQ(m4a_sequencer_set_player(&seq, M4A_PLAYER_CRY0), 0, "cry two tracks: cry player 0");
    m4a_engine_boot(&engine, false);
    m4a_sequencer_start(&seq, song);
    ASSERT_EQ(seq.trackCount, 2, "cry two tracks: MPlayStart takes both tracks (the player has two)");
    ASSERT(seq.tracks[0].cmdPtr == 0x11 && seq.tracks[1].cmdPtr == 0x18, "cry two tracks: track 0 at part0 (0x11), track 1 at part1 (0x18)");

    static float outL[4096], outR[4096];
    advance_frames(&engine, 4, outL, outR);              /* through the start tick on frame 3 */
    int on = 0, c0 = -1, c1 = -1;
    for (int i = 0; i < TOTAL_PCM_CHANNELS; i++) {
        if (engine.pcmChannels[i].status & CHN_ON) {
            on++;
            if (engine.pcmChannels[i].trackIndex == 0) c0 = i;
            if (engine.pcmChannels[i].trackIndex == 1) c1 = i;
        }
    }
    ASSERT_EQ(on, 2, "cry two tracks: two PCM channels are sounding after the start tick");
    ASSERT(c0 >= 0 && c1 >= 0, "cry two tracks: one channel on each of the two tracks");
    if (c0 >= 0 && c1 >= 0) {
        ASSERT(engine.pcmChannels[c0].key == 0x3D && engine.pcmChannels[c1].key == 0x3D, "cry two tracks: both channels keyed on the TIE key 61");
        ASSERT(engine.tracks[0].tune == 56 - 64 && engine.tracks[1].tune == 76 - 64, "cry two tracks: track TUNE 56 and 76 (c_v-relative -8 and +12), a 20-step chorus apart");
        ASSERT(engine.pcmChannels[c1].frequency > engine.pcmChannels[c0].frequency, "cry two tracks: the second channel runs faster (TUNE2 above TUNE)");
        ASSERT(engine.pcmChannels[c0].envelopeVolume == 255 && engine.pcmChannels[c1].envelopeVolume == 255, "cry two tracks: both envelopes at 255 (attack 255, sustain 255)");
        ASSERT(engine.pcmChannels[c0].release == 225 && engine.pcmChannels[c1].release == 225, "cry two tracks: both channels carry xRELE's 225");
    }

    m4a_sequencer_detach(&seq);
    m4a_engine_destroy(&engine);
    m4a_song_free(song);
    free(wd);
}

/* cry_bulbasaur_encounter's probe (docs/ACCURACY.md, "Cries"):
 * the two-track cry on cry player 0.  CgbSound entry (cycles from VBlank
 * line 160): 6792 on frame 3 (the start tick with two tracks and two
 * ply_notes), 1932 with both channels alive, 2025 on frame 47 (the faster
 * track's channel ran out of data: its unlink tick), 2005 on 48 (the other
 * track's unlink, the first track already dead), 1892 with both dead, 2360
 * on frame 143 (the EOT on two empty chains; the mode's release 225 reaches
 * no channel), 1192 on 144, 1007 from 145.  Both envelopes hold 255 for the
 * channels' whole lives (attack 255, sustain 255, no release ever
 * applied); the deaths are the count path's. */
static void test_sequencer_cycles_cry_encounter_chorus(void)
{
    printf("Testing sequencer CgbSound entry on a two-track cry (cry_bulbasaur_encounter probe)...\n");
    static int8_t decoded[8183 + 2 * M4A_DPCM_BLOCK_SAMPLES];
    WaveData *wd = make_dpcm_wave(8183, 0, 0, decoded);
    wd->freq = 10512u << 10;
    static ToneData voices[128];
    memset(voices, 0, sizeof(voices));
    voices[0].type = VOICE_CRY;
    voices[0].key = 60;
    voices[0].wav = wd;
    voices[0].attack = 0xFF; voices[0].decay = 0; voices[0].sustain = 0xFF; voices[0].release = 0;

    M4ACryParams p;
    m4a_cry_params_default(&p);
    p.pitch = 15600; p.volume = 90; p.release = 225; p.chorus = 20;      /* CRY_MODE_ENCOUNTER */
    M4ASong *song = m4a_cry_song_new(&p);
    ASSERT(song != NULL, "cry encounter cycles: song built");
    if (!song) { free(wd); return; }
    ASSERT(song->data[0x00] == 2 && song->data[0x2A] == 0x3D && song->data[0x12] == 0x38 && song->data[0x19] == 0x4C
           && song->data[0x1D] == 0x5A && song->data[0x2E] == 0x8C && song->data[0x2F] == 0x00 && song->data[0x26] == 0xE1,
           "cry encounter cycles: trackCount 2, TIE key 61, TUNE 56, TUNE2 76, VOL 90, xWAIT 140, xRELE 225");

    M4AEngine engine;
    m4a_engine_init(&engine, 48000.0f);
    m4a_engine_set_voicegroup(&engine, voices);
    M4ASequencer seq;
    m4a_sequencer_init(&seq, &engine);
    ASSERT_EQ(m4a_sequencer_set_player(&seq, M4A_PLAYER_CRY0), 0, "cry encounter cycles: cry player 0");
    m4a_engine_boot(&engine, false);
    m4a_sequencer_start(&seq, song);

    static float outL[4096], outR[4096];
    int mismatches = 0, envBad = -1;
    int death[2] = { -1, -1 };          /* the frame after whose processing each track's channel is no longer CHN_ON */
    int chan[2] = { -1, -1 };
    for (int k = 3; k <= 150; k++) {
        int expected = k == 3 ? 6792 : k < 47 ? 1932 : k == 47 ? 2025 : k == 48 ? 2005 : k < 143 ? 1892
                     : k == 143 ? 2360 : k == 144 ? 1192 : 1007;
        advance_frames(&engine, 1, outL, outR);
        if (seq.frameCycles != expected) {
            if (mismatches++ < 5)
                printf("  frame %d: engine %d, mgba %d\n", k, seq.frameCycles, expected);
        }
        if (k == 3) {
            for (int i = 0; i < TOTAL_PCM_CHANNELS; i++)
                if ((engine.pcmChannels[i].status & CHN_ON) && engine.pcmChannels[i].trackIndex >= 0 && engine.pcmChannels[i].trackIndex < 2)
                    chan[engine.pcmChannels[i].trackIndex] = i;
        }
        for (int t = 0; t < 2; t++) {
            if (chan[t] < 0) continue;
            const M4APCMChannel *ch = &engine.pcmChannels[chan[t]];
            if (death[t] < 0 && !(ch->status & CHN_ON))
                death[t] = k;
            if (death[t] < 0 && ch->envelopeVolume != 255 && envBad < 0) {
                envBad = k;
                printf("  frame %d: track %d env %d\n", k, t, ch->envelopeVolume);
            }
        }
    }
    ASSERT(chan[0] >= 0 && chan[1] >= 0, "cry encounter cycles: both tracks keyed a channel on the start tick (frame 3)");
    ASSERT_EQ(mismatches, 0, "sequencer: CgbSound entry matches the cry_bulbasaur_encounter probe on frames 3..150 (cry 0, two tracks)");
    ASSERT_EQ(envBad, -1, "cry encounter cycles: both envelopes hold 255 while the channels live (no release is ever applied)");
    /* The probe's unlink ticks are on frames 47 (track 1, TUNE 76, the faster
     * one) and 48 (track 0, TUNE 56): the channel ran out during the
     * previous frame's SoundMainRAM, so it is dead after frames 46 / 47. */
    ASSERT_EQ(death[1], 46, "cry encounter cycles: track 1's channel (TUNE 76) ends through the count path on frame 46, its unlink tick on 47");
    ASSERT_EQ(death[0], 47, "cry encounter cycles: track 0's channel (TUNE 56) ends through the count path on frame 47, its unlink tick on 48");
    ASSERT(m4a_sequencer_finished(&seq), "sequencer: the cry finished (both tracks gone at the EOT on 143)");

    m4a_sequencer_detach(&seq);
    m4a_engine_destroy(&engine);
    m4a_song_free(song);
    free(wd);
}

/* cry_jynx_high_pitch's probe (docs/ACCURACY.md, "Cries"):
 * CRY_MODE_HIGH_PITCH whole (length 50, release 200, pitch 15800, chorus
 * 20, volume 90) on Cry_Jynx, the first EOT that finds a live channel on
 * each of the cry player's two chains.  CgbSound entry (cycles from VBlank
 * line 160): 6792 on frame 3, 1932 with both channels alive, 2596 on frame
 * 53 (the EOT: two ply_endtie -> _081DD938 walks of 82 (61 - 7 for the RAM
 * byte + the match 28), each chain's one channel holding the key, then
 * two ply_fine -> _loop 10 -> _ok 14 ->
 * RealClearChain 12 -> _done 50 unlinks of a live channel; the empty-chain
 * value was 2360), 1192 on 54, 1007 from 55.  Both channels carry xRELE's
 * 200 and run the same tail from the EOT frame, 255 * 200 >> 8 per pass
 * (199 .. 1 on frames 53..70), and both die on frame 71, the probe's two
 * _081DCFB0 stamps on raw f72 (channels 0x03000FC0 and 0x03001000). */
static void test_sequencer_cycles_cry_high_pitch_two_chain(void)
{
    printf("Testing sequencer CgbSound entry and the two-channel release on a two-track cry (cry_jynx_high_pitch probe)...\n");
    static int8_t decoded[23524 + 2 * M4A_DPCM_BLOCK_SAMPLES];
    WaveData *wd = make_dpcm_wave(23524, 0, 0, decoded);
    wd->freq = 10512u << 10;
    static ToneData voices[128];
    memset(voices, 0, sizeof(voices));
    voices[0].type = VOICE_CRY;
    voices[0].key = 60;
    voices[0].wav = wd;
    voices[0].attack = 0xFF; voices[0].decay = 0; voices[0].sustain = 0xFF; voices[0].release = 0;

    M4ACryParams p;
    m4a_cry_params_default(&p);
    p.pitch = 15800; p.volume = 90; p.length = 50; p.release = 200; p.chorus = 20;      /* CRY_MODE_HIGH_PITCH */
    M4ASong *song = m4a_cry_song_new(&p);
    ASSERT(song != NULL, "cry high pitch cycles: song built");
    if (!song) { free(wd); return; }
    ASSERT(song->data[0x00] == 2 && song->data[0x2A] == 0x3E && song->data[0x12] == 0x1C && song->data[0x19] == 0x30
           && song->data[0x1D] == 0x5A && song->data[0x2E] == 0x32 && song->data[0x2F] == 0x00 && song->data[0x26] == 0xC8,
           "cry high pitch cycles: trackCount 2, TIE key 62, TUNE 28, TUNE2 48, VOL 90, xWAIT 50, xRELE 200");

    M4AEngine engine;
    m4a_engine_init(&engine, 48000.0f);
    m4a_engine_set_voicegroup(&engine, voices);
    M4ASequencer seq;
    m4a_sequencer_init(&seq, &engine);
    ASSERT_EQ(m4a_sequencer_set_player(&seq, M4A_PLAYER_CRY0), 0, "cry high pitch cycles: cry player 0");
    m4a_engine_boot(&engine, false);
    m4a_sequencer_start(&seq, song);

    static float outL[4096], outR[4096];
    static const uint8_t seq200[18] = { 199, 155, 121, 94, 73, 57, 44, 34, 26, 20, 15, 11, 8, 6, 4, 3, 2, 1 };
    int mismatches = 0, envBad = -1;
    int chan[2] = { -1, -1 }, death[2] = { -1, -1 };
    bool liveAtEot[2] = { false, false }, releaseAtEot[2] = { false, false };
    for (int k = 3; k <= 80; k++) {
        int expected = k == 3 ? 6792 : k < 53 ? 1932 : k == 53 ? 2596 : k == 54 ? 1192 : 1007;
        if (k == 53)
            for (int t = 0; t < 2; t++)
                if (chan[t] >= 0) liveAtEot[t] = (engine.pcmChannels[chan[t]].status & CHN_ON) != 0;
        advance_frames(&engine, 1, outL, outR);
        if (seq.frameCycles != expected) {
            if (mismatches++ < 5)
                printf("  frame %d: engine %d, mgba %d\n", k, seq.frameCycles, expected);
        }
        if (k == 3) {
            for (int i = 0; i < TOTAL_PCM_CHANNELS; i++)
                if ((engine.pcmChannels[i].status & CHN_ON) && engine.pcmChannels[i].trackIndex >= 0 && engine.pcmChannels[i].trackIndex < 2)
                    chan[engine.pcmChannels[i].trackIndex] = i;
        }
        for (int t = 0; t < 2; t++) {
            if (chan[t] < 0) continue;
            const M4APCMChannel *ch = &engine.pcmChannels[chan[t]];
            if (k == 53)
                releaseAtEot[t] = ch->release == 200;
            if (k >= 53 && k <= 70 && (!(ch->status & CHN_ON) || !(ch->status & CHN_STOP) || (ch->status & CHN_IEC)
                                       || ch->envelopeVolume != seq200[k - 53]) && envBad < 0) {
                envBad = k;
                printf("  frame %d: track %d env %d status %02X, GBA %d\n", k, t, ch->envelopeVolume, ch->status, seq200[k - 53]);
            }
            if (death[t] < 0 && k >= 53 && !(ch->status & CHN_ON))
                death[t] = k;
        }
    }
    ASSERT(chan[0] >= 0 && chan[1] >= 0, "cry high pitch cycles: both tracks keyed a channel on the start tick (frame 3)");
    ASSERT(chan[0] == 0 && chan[1] == 1, "cry high pitch cycles: track 0 took channel 0 and track 1 channel 1 (ply_note's first-free scan in index order: the probe's 0x03000FC0 then 0x03001000, SoundMainRAM stopping the lower index first)");
    ASSERT(liveAtEot[0] && liveAtEot[1], "cry high pitch cycles: both channels are still sounding when the EOT tick runs (frame 53)");
    ASSERT(releaseAtEot[0] && releaseAtEot[1], "cry high pitch cycles: both channels carry xRELE's 200");
    ASSERT_EQ(mismatches, 0, "sequencer: CgbSound entry matches the cry_jynx_high_pitch probe on frames 3..80 (cry 0, two live chains at the EOT: 2596)");
    ASSERT_EQ(envBad, -1, "cry high pitch cycles: both release envelopes on frames 53..70 are 255 * 200 >> 8 per pass (199 .. 1), ON | STOP without IEC (the release path, not pseudo-echo)");
    ASSERT_EQ(death[0], 71, "cry high pitch cycles: track 0's channel dies on frame 71, the probe's first _081DCFB0 stamp (f72, 0x03000FC0)");
    ASSERT_EQ(death[1], 71, "cry high pitch cycles: track 1's channel dies on frame 71, the probe's second _081DCFB0 stamp (f72, 0x03001000)");
    ASSERT(m4a_sequencer_finished(&seq), "sequencer: the cry finished (both tracks gone at the EOT on 53, the channels outliving them)");

    m4a_sequencer_detach(&seq);
    m4a_engine_destroy(&engine);
    m4a_song_free(song);
    free(wd);
}

/* Two cry players on one frame (docs/ACCURACY.md, "Players and chaining"): the first cry on cry player 0 from frame 3,
 * a second at the defaults started on VBlank F (its tick on frame F + 2)
 * on cry player 1 -- SetPokemonCryTone's first free player -- whose
 * MPlayMain body runs after cry 0's on the same frame.  The engine's
 * stand-in samples have the cries' lengths (Bulbasaur 8183, Pidgey 1902,
 * Jynx 23524, Wailord 20846 DPCM samples at 10512 Hz), so the channels
 * die on mgba's frames.  The expected CgbSound entries are the probes'
 * (build/probe/cry_second_bulbasaur_pidgey.txt.frames, frames 3..178, the
 * 180-frame probe's complete frames; cry_second_jynx_wailord.txt.frames,
 * 3..238). */
static int two_cries_expect_a(int k)      /* Cry_Bulbasaur, Cry_Pidgey from VBlank 20 */
{
    return k == 3 ? 4077 : k < 22 ? 1596 : k == 22 ? 4736 : k < 33 ? 2185 : k == 33 ? 2278
         : k < 50 ? 2165 : k == 50 ? 2258 : k < 143 ? 2145 : k == 143 ? 2379 : k == 144 ? 1761
         : k < 162 ? 1576 : k == 162 ? 1810 : k == 163 ? 1192 : 1007;
}
static int two_cries_expect_b(int k)      /* Cry_Jynx, Cry_Wailord from VBlank 60 */
{
    return k == 3 ? 4077 : k < 62 ? 1596 : k == 62 ? 4736 : k < 137 ? 2185 : k == 137 ? 2278
         : k < 143 ? 2165 : k == 143 ? 2399 : k == 144 ? 1781 : k < 181 ? 1596 : k == 181 ? 1689
         : k < 202 ? 1576 : k == 202 ? 1810 : k == 203 ? 1192 : 1007;
}
static int one_cry_expect_pidgey(int k)   /* test_sequencer_cycles_cry's values: cry 1 attached, never started */
{
    return k == 3 ? 4077 : k < 14 ? 1596 : k == 14 ? 1689 : k < 143 ? 1576 : k == 143 ? 1810 : k == 144 ? 1192 : 1007;
}

typedef struct {
    const char *name;
    uint32_t size0, size1;      /* the two cries' sample counts */
    int startFrame2;            /* VBlank of the second StartCry; 0 = none */
    int lastFrame;
    int (*expect)(int k);
    int death0, death1;         /* the frame after whose mix the channel's CHN_ON is clear (the unlink tick is the next frame) */
    int eot0, eot1;             /* the EOT ticks' frames */
} TwoCriesRun;

static void run_two_cries(const TwoCriesRun *r)
{
    static int8_t decoded0[23524 + 2 * M4A_DPCM_BLOCK_SAMPLES], decoded1[23524 + 2 * M4A_DPCM_BLOCK_SAMPLES];
    WaveData *wd0 = make_dpcm_wave(r->size0, 0, 0, decoded0);
    WaveData *wd1 = make_dpcm_wave(r->size1, 0, 0, decoded1);
    wd0->freq = wd1->freq = 10512u << 10;
    static ToneData voices0[128], voices1[128];
    memset(voices0, 0, sizeof(voices0));
    memset(voices1, 0, sizeof(voices1));
    for (int v = 0; v < 2; v++) {
        ToneData *t = v ? voices1 : voices0;
        t[0].type = VOICE_CRY; t[0].key = 60; t[0].wav = v ? wd1 : wd0;
        t[0].attack = 0xFF; t[0].decay = 0; t[0].sustain = 0xFF; t[0].release = 0;
    }
    M4ACryParams p;
    m4a_cry_params_default(&p);
    M4ASong *song0 = m4a_cry_song_new(&p);
    M4ASong *song1 = m4a_cry_song_new(&p);      /* gPokemonCrySongs[1]: its own copy of the same bytes */
    ASSERT(song0 && song1, "two cries: both template copies built");
    if (!song0 || !song1) { free(wd0); free(wd1); return; }

    M4AEngine engine;
    m4a_engine_init(&engine, 48000.0f);
    m4a_engine_set_voicegroup(&engine, voices0);
    M4ASequencer seq0, seq1;
    m4a_sequencer_init(&seq0, &engine);
    ASSERT_EQ(m4a_sequencer_set_player(&seq0, M4A_PLAYER_CRY0), 0, "two cries: cry player 0");
    ASSERT_EQ(m4a_sequencer_init_chained(&seq1, &seq0, seq0.trackBase + seq0.maxTracks), 0, "two cries: cry player 1 chained after cry 0 on engine tracks 2..3");
    ASSERT_EQ(seq1.trackBase, 2, "two cries: cry 1's track 0 is engine track 2 (above cry 0's 0..1, as gPokemonCryTracks[2] lies above [0])");
    ASSERT_EQ(m4a_sequencer_set_player(&seq1, M4A_PLAYER_CRY1), 0, "two cries: cry player 1 selected");
    ASSERT_EQ(m4a_sequencer_set_player(&seq1, M4A_PLAYER_CRY0), -1, "two cries: a chained player below its inner is refused");
    ASSERT_EQ(seq1.player, M4A_PLAYER_CRY1, "two cries: ... and the player is unchanged");
    ASSERT(engine.frameHookUser == &seq0 && seq0.outer == &seq1 && seq1.inner == &seq0, "two cries: one hook (cry 0's) runs the chain");
    m4a_sequencer_set_voicegroup(&seq1, voices1);
    M4ASequencer *players[2] = { &seq0, &seq1 };
    ASSERT_EQ(m4a_cry_choose_player(players), 0, "two cries: before anything plays SetPokemonCryTone takes cry player 0");
    m4a_engine_boot(&engine, false);
    m4a_sequencer_start(&seq0, song0);
    ASSERT_EQ(m4a_cry_choose_player(players), 1, "two cries: with cry 0's track 0 EXIST | START, the next cry takes cry player 1");

    static float outL[4096], outR[4096];
    int mismatches = 0, chan1 = -1, started = -1;
    int death[2] = { -1, -1 };
    bool stopSeen[2] = { false, false };
    bool chainEmptyAtEot[2] = { false, false };
    for (int k = 3; k <= r->lastFrame; k++) {
        if (r->startFrame2 && k == r->startFrame2 + 2) {
            /* StartCry on VBlank startFrame2: MPlayStart between the two
             * SoundMains, as poryaaaa_render's cry2 hook issues it */
            started = m4a_cry_choose_player(players);
            if (started >= 0)
                m4a_sequencer_start(players[started], song1);
        }
        for (int t = 0; t < 2; t++) {
            int c = t == 0 ? 0 : chan1;
            if (c < 0) continue;
            int eot = t == 0 ? r->eot0 : r->eot1;
            if (k == eot)           /* before the EOT tick runs: the chain it will walk */
                chainEmptyAtEot[t] = players[t]->chainOwner[c] == 0 && !(engine.pcmChannels[c].status & CHN_ON);
        }
        advance_frames(&engine, 1, outL, outR);
        if (seq1.frameCycles != r->expect(k)) {
            if (mismatches++ < 5)
                printf("  %s frame %d: engine %d, mgba %d\n", r->name, k, seq1.frameCycles, r->expect(k));
        }
        if (r->startFrame2 && k == r->startFrame2 + 2)
            for (int i = 0; i < TOTAL_PCM_CHANNELS; i++)
                if ((engine.pcmChannels[i].status & CHN_ON) && engine.pcmChannels[i].trackIndex == 2)
                    chan1 = i;
        for (int t = 0; t < 2; t++) {
            int c = t == 0 ? 0 : chan1;
            if (c < 0) continue;
            uint8_t st = engine.pcmChannels[c].status;
            if ((st & CHN_ON) && (st & CHN_STOP)) stopSeen[t] = true;
            if (death[t] < 0 && !(st & CHN_ON)) death[t] = k;
        }
    }
    char msg[256];
    snprintf(msg, sizeof(msg), "sequencer: CgbSound entry matches the %s probe on frames 3..%d (both players)", r->name, r->lastFrame);
    ASSERT_EQ(mismatches, 0, msg);
    ASSERT_EQ(engine.cgbEntryCycles, seq1.frameCycles - M4A_CYC_SOUNDMAIN_ENTRY, "two cries: the chain's last member publishes the entry");
    ASSERT_EQ(seq0.frameCount, seq1.frameCount, "two cries: both players count the same frames");
    ASSERT_EQ(engine.pcmChannels[0].trackIndex, 0, "two cries: the first cry took channel 0");
    ASSERT_EQ(death[0], r->death0, "two cries: the first cry's channel ends on mgba's frame through the count path");
    ASSERT(!stopSeen[0], "two cries: the first cry's channel was never releasing (no _081DCFB0 stamp: the sample ran out)");
    ASSERT(chainEmptyAtEot[0], "two cries: cry 0's EOT finds an empty chain");
    if (r->startFrame2) {
        ASSERT_EQ(started, 1, "two cries: the second cry took cry player 1 (SetPokemonCryTone's first free player)");
        ASSERT_EQ(chan1, 1, "two cries: the second cry took channel 1 (ply_note's index-order scan passing over the first cry's channel 0)");
        ASSERT_EQ(seq1.noteCount, 1, "two cries: one TIE on cry player 1");
        ASSERT_EQ(seq0.noteCount, 1, "two cries: one TIE on cry player 0");
        ASSERT_EQ(death[1], r->death1, "two cries: the second cry's channel ends on mgba's frame through the count path");
        ASSERT(!stopSeen[1], "two cries: the second cry's channel was never releasing");
        ASSERT(chainEmptyAtEot[1], "two cries: cry 1's EOT finds an empty chain");
        ASSERT(m4a_sequencer_finished(&seq0) && m4a_sequencer_finished(&seq1), "two cries: both players finished");
        ASSERT_EQ(seq1.clock, 142, "two cries: cry 1's 142 ticks");
        ASSERT_EQ(m4a_cry_choose_player(players), 0, "two cries: both finished, cry player 0 is free again");
    } else {
        ASSERT_EQ(seq1.noteCount, 0, "lone cry with cry 1 attached: cry 1 never played");
        ASSERT(m4a_sequencer_finished(&seq0) && seq1.song == NULL, "lone cry with cry 1 attached: cry 0 finished, cry 1 idle");
    }

    m4a_sequencer_detach(&seq1);
    ASSERT(seq0.outer == NULL && engine.frameHookUser == &seq0, "two cries: detaching the outer player leaves cry 0's hook");
    m4a_sequencer_detach(&seq0);
    m4a_engine_destroy(&engine);
    m4a_song_free(song0);
    m4a_song_free(song1);
    free(wd0);
    free(wd1);
}

static void test_sequencer_cycles_two_cries(void)
{
    printf("Testing two cry players on one frame (cry_second_bulbasaur_pidgey / cry_second_jynx_wailord probes)...\n");
    static const TwoCriesRun runs[] = {
        /* (a): Pidgey (cry 1, channel 1) dies after frame 32's mix, unlinked on 33 = 22 + 11; Bulbasaur after 49, unlinked on 50;
         *      cry 0's EOT on 143 with cry 1's track dead (2379), cry 1's on 162 = 22 + 140. */
        { "cry_second_bulbasaur_pidgey", 8183, 1902, 20, 178, two_cries_expect_a, 49, 32, 143, 162 },   /* the 180-frame probe's last complete frame is 178 */
        /* (b): Jynx (cry 0) dies after 136, unlinked on 137; Wailord (cry 1) after 180, unlinked on 181 = 62 + 119;
         *      cry 0's EOT on 143 with cry 1's channel alive (2399), cry 1's on 202 = 62 + 140. */
        { "cry_second_jynx_wailord", 23524, 20846, 60, 238, two_cries_expect_b, 136, 180, 143, 202 },
        /* a single cry with the second player attached and inactive: the one-cry skeleton, unchanged */
        { "cry_pidgey (cry 1 attached, inactive)", 1902, 1902, 0, 145, one_cry_expect_pidgey, 13, -1, 143, 0 },
    };
    for (size_t i = 0; i < sizeof(runs) / sizeof(runs[0]); i++)
        run_two_cries(&runs[i]);
}

/* A cry over a sound effect and over music (docs/ACCURACY.md, "Players and chaining"): cry player 0 chained after the song's player -- SE1 (3 tracks) ->
 * cry 0 on engine tracks 3..4, BGM (10) -> cry 0 on 10..11 -- with the
 * paused players between them (SE2 / SE3; SE1 / SE2 / SE3) charged by the
 * chain, so every frame is 1007 + (79 + body) per playing player.  The
 * songs are the ROM's compiled se_exp.s / mus_littleroot.s with their
 * vanilla voicegroups (skipped with a message when build/accuracy or
 * ~/pokeemerald is absent); the cry is a stand-in DPCM sample of
 * Cry_Bulbasaur's 8183 samples at 10512 Hz (the count path ends it on
 * the probe's frame).  Expected: the CgbSound entry of
 * build/probe/cry_over_se.txt.frames / cry_over_bgm.txt.frames on frames
 * 3..178 (the 180-frame probes' last complete frame).  Run (a): the cry's
 * start tick on frame 32 takes channel 0 free (no DirectSound channel ON:
 * the SE's note is a square voice).  Run (b): on frame 62 all five
 * DirectSound channels are ON and the cry's ply_note steals channel 1, a
 * releasing (ON | STOP) note of BGM track 1, by the stopping-candidate
 * rule; the GBA's RealClearChain unlinks it from BGM track 1's chain at
 * once through channel->track (NOTE_UNLINK_11 66 + CHAIN_EMPTY 4 = 70
 * inside the 7136), so the BGM's later walks see one channel fewer (-32
 * on its tick frames) and never meet it as dead. */
typedef struct {
    const char *name;
    const char *songFile;       /* under build/accuracy/rom/songs/ */
    const char *voicegroup;     /* the song's, as m4a_song names it (no voicegroup_ prefix), loaded from ~/pokeemerald */
    int player;                 /* the song's M4A_PLAYER_* */
    int trackBase;              /* cry 0's engine track 0 */
    int startFrame2;            /* VBlank of the StartCry: the start tick on frame + 2 */
    int cryChan;                /* the channel the cry takes */
    int stolenFromTrack;        /* the BGM track (from 0) whose releasing channel it was; -1 = a free take */
    int unlinkFrame;            /* the cry's unlink tick (its channel's CHN_ON clear after the frame before) */
    int eot;                    /* the cry's EOT tick frame, or 0 when past the run */
    const int *expect;          /* frames 3..178 */
} CryOverRun;

static const int sCryOverSeExpect[176] = {
        3987, 2117, 2602, 2111, 2331, 2950, 2111, 2331, 2111, 2950,   /* 3..12 */
        2602, 1412, 2950, 2111, 2602, 2679, 2111, 2602, 2459, 2331,   /* 13..22 */
        2602, 2459, 2331, 2111, 2950, 2602, 1412, 2950, 2111, 5672,   /* 23..32 */
        3268, 2700, 3191, 3048, 2920, 3191, 3048, 2920, 2700, 3539,   /* 33..42 */
        3191, 2001, 3539, 2700, 3191, 3268, 2700, 3191, 3048, 2920,   /* 43..52 */
        3191, 3048, 2920, 2700, 3539, 3191, 2001, 3539, 2700, 3191,   /* 53..62 */
        3268, 2700, 3191, 3048, 2920, 3191, 3048, 2920, 2700, 3539,   /* 63..72 */
        3191, 2001, 3539, 2700, 3191, 3268, 2793, 3171, 3028, 2900,   /* 73..82 */
        3171, 3028, 2900, 2680, 3519, 3171, 1981, 3519, 2680, 3171,   /* 83..92 */
        3248, 2680, 3171, 3028, 2900, 3171, 3028, 2900, 2340, 1795,   /* 93..102 */
        1576, 1576, 1576, 1576, 1576, 1576, 1576, 1576, 1576, 1576,   /* 103..112 */
        1576, 1576, 1576, 1576, 1576, 1576, 1576, 1576, 1576, 1576,   /* 113..122 */
        1576, 1576, 1576, 1576, 1576, 1576, 1576, 1576, 1576, 1576,   /* 123..132 */
        1576, 1576, 1576, 1576, 1576, 1576, 1576, 1576, 1576, 1576,   /* 133..142 */
        1576, 1576, 1576, 1576, 1576, 1576, 1576, 1576, 1576, 1576,   /* 143..152 */
        1576, 1576, 1576, 1576, 1576, 1576, 1576, 1576, 1576, 1576,   /* 153..162 */
        1576, 1576, 1576, 1576, 1576, 1576, 1576, 1576, 1576, 1810,   /* 163..172 */
        1192, 1007, 1007, 1007, 1007, 1007,   /* 173..178 */
};
static const int sCryOverBgmExpect[176] = {
        22695, 1450, 2462, 2462, 1450, 2462, 2462, 2462, 1450, 2470,   /* 3..12 */
        2456, 1450, 2456, 2456, 2456, 1450, 2456, 6233, 1450, 2526,   /* 13..22 */
        2526, 2526, 1450, 2526, 2621, 2494, 1450, 2494, 2589, 1450,   /* 23..32 */
        2462, 2462, 2462, 1450, 4423, 2494, 1450, 2494, 2494, 2494,   /* 33..42 */
        1450, 2494, 2494, 1450, 2494, 2589, 2462, 1450, 2462, 2462,   /* 43..52 */
        9321, 1450, 2552, 2552, 1450, 4769, 2552, 2552, 1450, 7136,   /* 53..62 */
        3109, 2039, 3204, 3861, 3127, 2039, 3604, 5027, 2039, 3638,   /* 63..72 */
        3638, 4065, 2039, 3731, 3604, 5193, 2039, 3638, 3638, 2039,   /* 73..82 */
        4160, 3604, 3604, 2039, 9384, 4126, 2039, 4982, 4126, 4130,   /* 83..92 */
        2039, 4986, 6712, 2039, 4126, 4555, 4553, 2039, 4130, 4146,   /* 93..102 */
        8931, 2039, 3103, 3103, 2039, 3103, 3196, 3083, 2019, 5397,   /* 103..112 */
        3109, 2019, 3109, 3202, 3089, 2019, 3089, 7695, 2019, 3121,   /* 113..122 */
        3121, 3121, 2019, 3121, 3216, 5385, 2019, 3121, 3216, 2019,   /* 123..132 */
        3089, 3089, 3105, 2019, 7773, 3121, 2019, 3121, 3121, 3121,   /* 133..142 */
        2019, 3129, 4385, 2019, 3216, 3089, 3089, 2019, 3184, 3065,   /* 143..152 */
        7470, 2019, 3115, 3115, 2019, 3115, 3115, 3131, 2019, 6957,   /* 153..162 */
        3210, 2019, 3083, 3083, 3083, 2019, 3099, 7663, 2019, 3115,   /* 163..172 */
        3115, 3115, 2019, 3115, 3131, 5655,   /* 173..178 */
};

static void run_cry_over_song(const CryOverRun *r)
{
    const char *home = getenv("HOME");
    char songPath[320], root[320], err[256];
    snprintf(songPath, sizeof(songPath), "build/accuracy/rom/songs/%s", r->songFile);
    snprintf(root, sizeof(root), "%s/pokeemerald", home ? home : "");
    M4ASong *song = m4a_song_load(songPath, err, sizeof(err));
    if (!song) {
        printf("  (%s: no %s, run not checked)\n", r->name, songPath);
        return;
    }
    ASSERT_EQ(strcmp(song->voicegroup, r->voicegroup), 0, "cry over song: the song names its vanilla voicegroup");
    LoadedVoiceGroup *vg = voicegroup_load(root, r->voicegroup, NULL);
    if (!vg) {
        printf("  (%s: no voicegroup_%s in %s, run not checked)\n", r->name, r->voicegroup, root);
        m4a_song_free(song);
        return;
    }
    static int8_t decoded[8183 + 2 * M4A_DPCM_BLOCK_SAMPLES];
    WaveData *wd = make_dpcm_wave(8183, 0, 0, decoded);
    wd->freq = 10512u << 10;
    static ToneData cryVoices[128];
    memset(cryVoices, 0, sizeof(cryVoices));
    cryVoices[0].type = VOICE_CRY; cryVoices[0].key = 60; cryVoices[0].wav = wd;
    cryVoices[0].attack = 0xFF; cryVoices[0].decay = 0; cryVoices[0].sustain = 0xFF; cryVoices[0].release = 0;
    M4ACryParams p;
    m4a_cry_params_default(&p);
    M4ASong *cry = m4a_cry_song_new(&p);
    ASSERT(cry != NULL, "cry over song: the template copy built");
    if (!cry) { free(wd); voicegroup_free(vg); m4a_song_free(song); return; }

    M4AEngine engine;
    m4a_engine_init(&engine, 48000.0f);
    m4a_engine_set_voicegroup(&engine, vg->voices);
    M4ASequencer seq, cry0;
    m4a_sequencer_init(&seq, &engine);
    ASSERT_EQ(m4a_sequencer_set_player(&seq, r->player), 0, "cry over song: the song's player");
    ASSERT_EQ(m4a_sequencer_init_chained_player(&cry0, &seq, seq.trackBase + seq.maxTracks, M4A_PLAYER_CRY0), 0,
              "cry over song: cry player 0 chained after the song's player");
    ASSERT_EQ(cry0.trackBase, r->trackBase, "cry over song: cry 0's track 0 is the engine track above the song player's tracks");
    ASSERT_EQ(cry0.player, M4A_PLAYER_CRY0, "cry over song: the chained player is cry 0 (the SE players between stay paused)");
    ASSERT(engine.frameHookUser == &seq && seq.outer == &cry0 && cry0.inner == &seq, "cry over song: one hook (the song's) runs the chain");
    m4a_sequencer_set_voicegroup(&cry0, cryVoices);
    M4ASequencer *players[2] = { &cry0, NULL };
    ASSERT_EQ(m4a_cry_choose_player(players), 0, "cry over song: SetPokemonCryTone from silence takes cry player 0");
    m4a_engine_boot(&engine, false);
    m4a_sequencer_start(&seq, song);

    static float outL[4096], outR[4096];
    int mismatches = 0, started = -1, death = -1;
    bool stopSeen = false, chainEmptyAtEot = false, preStealOk = false, postStealOk = false;
    int startTick = r->startFrame2 + 2;
    for (int k = 3; k <= 178; k++) {
        if (k == startTick) {
            /* the state the cry's ply_note scans: before the frame's MPlayMain */
            if (r->stolenFromTrack >= 0) {
                int on = 0;
                for (int c = 0; c < 5; c++)
                    on += (engine.pcmChannels[c].status & CHN_ON) != 0;
                uint8_t st = engine.pcmChannels[r->cryChan].status;
                preStealOk = on == 5 && (st & CHN_ON) && (st & CHN_STOP)
                          && engine.pcmChannels[r->cryChan].trackIndex == seq.trackBase + r->stolenFromTrack
                          && seq.chainOwner[r->cryChan] == r->stolenFromTrack + 1;
            } else {
                preStealOk = true;
                for (int c = 0; c < 5; c++)
                    if (engine.pcmChannels[c].status & CHN_ON) preStealOk = false;
            }
            started = m4a_cry_choose_player(players);
            if (started >= 0)
                m4a_sequencer_start(players[started], cry);
        }
        if (k == r->eot)
            chainEmptyAtEot = cry0.chainOwner[r->cryChan] == 0 && !(engine.pcmChannels[r->cryChan].status & CHN_ON);
        advance_frames(&engine, 1, outL, outR);
        if (cry0.frameCycles != r->expect[k - 3]) {
            if (mismatches++ < 5)
                printf("  %s frame %d: engine %d, mgba %d\n", r->name, k, cry0.frameCycles, r->expect[k - 3]);
        }
        if (k == startTick) {
            /* the cry owns its channel; the owner's chain dropped it at once */
            postStealOk = engine.pcmChannels[r->cryChan].trackIndex == cry0.trackBase
                       && cry0.chainOwner[r->cryChan] == 1
                       && seq.chainOwner[r->cryChan] == 0
                       && (engine.pcmChannels[r->cryChan].status & CHN_ON)
                       && !(engine.pcmChannels[r->cryChan].status & CHN_STOP);
            if (r->stolenFromTrack >= 0)   /* the other releasing channel of the same track stays chained */
                postStealOk = postStealOk && seq.chainOwner[0] == r->stolenFromTrack + 1
                           && (engine.pcmChannels[0].status & CHN_STOP);
        }
        if (k > startTick) {
            uint8_t st = engine.pcmChannels[r->cryChan].status;
            if (engine.pcmChannels[r->cryChan].trackIndex == cry0.trackBase && (st & CHN_ON) && (st & CHN_STOP))
                stopSeen = true;
            if (death < 0 && !(st & CHN_ON)) death = k;
        }
    }
    char msg[256];
    snprintf(msg, sizeof(msg), "sequencer: CgbSound entry matches the %s probe on frames 3..178 (both players)", r->name);
    ASSERT_EQ(mismatches, 0, msg);
    ASSERT_EQ(engine.cgbEntryCycles, cry0.frameCycles - M4A_CYC_SOUNDMAIN_ENTRY, "cry over song: the chain's last member (cry 0) publishes the entry");
    ASSERT_EQ(seq.frameCount, cry0.frameCount, "cry over song: both players count the same frames");
    ASSERT_EQ(started, 0, "cry over song: the cry took cry player 0");
    snprintf(msg, sizeof(msg), "%s: the state ply_note scanned on frame %d (%s)", r->name, startTick,
             r->stolenFromTrack >= 0 ? "five channels ON, the target ON | STOP on the BGM track" : "no DirectSound channel ON");
    ASSERT(preStealOk, msg);
    snprintf(msg, sizeof(msg), "%s: the cry took channel %d%s", r->name, r->cryChan,
             r->stolenFromTrack >= 0 ? " and the owner's chain dropped it at once (chainOwner cleared, the other releasing channel kept)" : " (a free take)");
    ASSERT(postStealOk, msg);
    ASSERT_EQ(death, r->unlinkFrame - 1, "cry over song: the cry's channel ends through the count path after the frame before its unlink tick");
    ASSERT(!stopSeen, "cry over song: the cry's channel was never releasing (no _081DCFB0 stamp)");
    if (r->eot)
        ASSERT(chainEmptyAtEot, "cry over song: the cry's EOT finds an empty chain");
    ASSERT_EQ(cry0.noteCount, 1, "cry over song: one TIE on cry player 0");

    m4a_sequencer_detach(&cry0);
    ASSERT(seq.outer == NULL && engine.frameHookUser == &seq, "cry over song: detaching cry 0 leaves the song's hook");
    m4a_sequencer_detach(&seq);
    m4a_engine_destroy(&engine);
    m4a_song_free(cry);
    m4a_song_free(song);
    voicegroup_free(vg);
    free(wd);
}

static void test_sequencer_cycles_cry_over_song(void)
{
    printf("Testing a cry over a sound effect and over music (cry_over_se / cry_over_bgm probes)...\n");
    static const CryOverRun runs[] = {
        /* (a): Cry_Bulbasaur from VBlank 30 over se_exp on SE1: start tick 32 on channel 0 (free), the unlink tick 79 = 32 + 47, EOT 172 = 32 + 140 */
        { "cry_over_se", "se_exp.s", "rs_sfx_1", M4A_PLAYER_SE1, 3, 30, 0, -1, 79, 172, sCryOverSeExpect },
        /* (b): Cry_Bulbasaur from VBlank 60 over mus_littleroot on BGM: start tick 62 steals channel 1 from BGM track 1 (from 0), the unlink tick 109 = 62 + 47; the EOT 202 is past the run */
        { "cry_over_bgm", "mus_littleroot.s", "littleroot", M4A_PLAYER_BGM, 10, 60, 1, 1, 109, 0, sCryOverBgmExpect },
    };
    for (size_t i = 0; i < sizeof(runs) / sizeof(runs[0]); i++)
        run_cry_over_song(&runs[i]);

    /* The chain constructors: the next-player form unchanged (cry 1 after
     * cry 0), the explicit form refusing a player out of order or range and
     * accepting a chain that skips players -- BGM -> SE1 -> cry 0, 15 of the
     * engine's 16 tracks. */
    M4AEngine engine;
    m4a_engine_init(&engine, 48000.0f);
    M4ASequencer a, b, c;
    m4a_sequencer_init(&a, &engine);
    ASSERT_EQ(m4a_sequencer_set_player(&a, M4A_PLAYER_CRY0), 0, "chain: cry 0 alone");
    ASSERT_EQ(m4a_sequencer_init_chained(&b, &a, 2), 0, "chain: the next-player form after cry 0");
    ASSERT_EQ(b.player, M4A_PLAYER_CRY1, "chain: ... selects cry 1, as before");
    m4a_sequencer_detach(&b);
    ASSERT_EQ(m4a_sequencer_init_chained(&b, &a, 2), 0, "chain: re-attached");
    m4a_sequencer_detach(&b);
    ASSERT_EQ(m4a_sequencer_set_player(&a, M4A_PLAYER_BGM), 0, "chain: the BGM player alone");
    ASSERT_EQ(m4a_sequencer_init_chained_player(&b, &a, 10, M4A_PLAYER_BGM), -1, "chain: a player not above its inner's is refused");
    ASSERT(a.outer == NULL, "chain: ... and nothing is attached");
    ASSERT_EQ(m4a_sequencer_init_chained_player(&b, &a, 9, M4A_PLAYER_CRY0), -1, "chain: a trackBase inside the inner's tracks is refused");
    ASSERT_EQ(m4a_sequencer_init_chained_player(&b, &a, 10, M4A_PLAYER_SE2), -1, "chain: SE2's 9 tracks at 10 do not fit the engine's 16");
    ASSERT_EQ(m4a_sequencer_init_chained_player(&b, &a, 10, M4A_PLAYER_SE1), 0, "chain: BGM -> SE1 on tracks 10..12");
    ASSERT_EQ(m4a_sequencer_init_chained_player(&c, &b, 13, M4A_PLAYER_CRY0), 0, "chain: BGM -> SE1 -> cry 0 on tracks 13..14 (15 tracks fit)");
    ASSERT(a.outer == &b && b.outer == &c && c.inner == &b, "chain: three members in MPlayOpen order");
    ASSERT_EQ(m4a_sequencer_init_chained_player(&c, &b, 13, M4A_PLAYER_CRY0), -1, "chain: an inner that already has an outer is refused");
    m4a_sequencer_detach(&c);
    m4a_sequencer_detach(&b);
    m4a_sequencer_detach(&a);
    m4a_engine_destroy(&engine);
}

/* The MIDI event latch (docs/ACCURACY.md, "Entry points and the frame latch"): with quantizeEvents the
 * entry points queue and the queue drains at the top of the next tick after
 * the gate walk; off, they apply at once. */
static int latch_hook_started = -1;
static void latch_hook(M4AEngine *e, void *user)
{
    (void)user;
    /* The frame hook runs inside the tick: a program change and a note-on
     * from here (the sequencer's ply_voice / ply_note) apply at once even
     * with the latch on. */
    m4a_engine_program_change(e, 3, 0);
    latch_hook_started = m4a_engine_note_on(e, 3, 60, 127);
}

/*
 * poryaaaa_render --gba-start-delay puts MIDI time 0 at output sample
 * O = floor((883882 - GBA_BLIP_FRAME) * rate / 2^24): the last sample a blip
 * frame before frame 3's SoundMain, the ROM's first song tick.  That rests on
 * the dispatch granularity: after the kept boot and O samples the machine
 * must be past frame 2's SoundMain and short of frame 3's, so an event
 * dispatched there is applied by the first tick, latched or not (docs/ACCURACY.md,
 * the renderer's "as the game" mode).
 */
static void test_gba_start_delay_origin(void)
{
    printf("Testing the --gba-start-delay origin (dispatch before frame 3's SoundMain)...\n");

    const int64_t soundMain2 = 124773 + 1 * GBA_CYCLES_PER_FRAME + 160 * GBA_CYCLES_PER_LINE + M4A_CYC_SOUNDMAIN_ENTRY;
    const int64_t soundMain3 = soundMain2 + GBA_CYCLES_PER_FRAME;
    ASSERT_EQ(soundMain3, 883882, "frame 3's SoundMain at 883882 cycles");

    WaveData *wd = seq_test_wave();
    ToneData voices[128];
    memset(voices, 0, sizeof(voices));
    voices[0].type = VOICE_DIRECTSOUND;
    voices[0].key = 60;
    voices[0].wav = wd;
    voices[0].attack = 0xFF;
    voices[0].sustain = 0xFF;

    static const int rates[] = { 22050, 44100, 48000, 96000 };
    for (int q = 0; q < 2; q++) {
        for (size_t r = 0; r < sizeof(rates) / sizeof(rates[0]); r++) {
            char msg[128];
            M4AEngine engine;
            m4a_engine_init(&engine, (float)rates[r]);
            m4a_engine_set_voicegroup(&engine, voices);
            m4a_engine_boot(&engine, true);
            m4a_engine_set_quantize_events(&engine, q == 1);
            m4a_engine_program_change(&engine, 0, 0);

            int origin = (int)((double)(soundMain3 - GBA_BLIP_FRAME) * rates[r] / (double)GBA_CPU_HZ);
            float *buf = malloc(sizeof(float) * (size_t)(origin + 4096) * 2);
            m4a_engine_process(&engine, buf, buf + origin + 4096, origin);
            snprintf(msg, sizeof(msg), "%d Hz%s: sample %d dispatched after frame 2's SoundMain",
                     rates[r], q ? " latched" : "", origin);
            ASSERT(engine.gba.now > soundMain2, msg);
            snprintf(msg, sizeof(msg), "%d Hz%s: sample %d dispatched before frame 3's SoundMain",
                     rates[r], q ? " latched" : "", origin);
            ASSERT(engine.gba.now < soundMain3 && engine.nextSoundMain == soundMain3, msg);

            m4a_engine_note_on(&engine, 0, 60, 127);
            /* Run to just past frame 3's SoundMain: the note's channel has
             * been through SoundMainRAM (START taken), i.e. it plays on the
             * ROM's first tick frame. */
            int more = (int)((double)(soundMain3 - engine.gba.now + 2 * GBA_BLIP_FRAME) * rates[r] / GBA_CPU_HZ) + 1;
            m4a_engine_process(&engine, buf, buf + origin + 4096, more);
            snprintf(msg, sizeof(msg), "%d Hz%s: frame 3's SoundMain has run", rates[r], q ? " latched" : "");
            ASSERT(engine.gba.now >= soundMain3 && engine.nextSoundMain == soundMain3 + GBA_CYCLES_PER_FRAME, msg);
            bool started = false;
            for (int i = 0; i < TOTAL_PCM_CHANNELS; i++)
                if ((engine.pcmChannels[i].status & CHN_ON) && !(engine.pcmChannels[i].status & CHN_START))
                    started = true;
            snprintf(msg, sizeof(msg), "%d Hz%s: the note started on frame 3", rates[r], q ? " latched" : "");
            ASSERT(started, msg);
            free(buf);
            m4a_engine_destroy(&engine);
        }
    }
    free(wd);
}

/*
 * poryaaaa_render --gba-tempo-grid places MIDI tick n on the frame MPlayMain
 * plays it: tempoC += tempoI once per frame (MPlayStart's 150 before the first
 * TEMPO), a tick each time tempoC reaches 150, a TEMPO changing only the next
 * frame's add; grid frame 0 is frame 3.  The event is dispatched at the last
 * sample a blip frame before that frame's SoundMain, the --gba-start-delay
 * origin's rule at every frame (docs/ACCURACY.md, the renderer's "as the game" mode).  Without the
 * flag the renderer passes the grid NULL and every exact sample through.
 */
static void test_gba_tempo_grid(void)
{
    printf("Testing the --gba-tempo-grid tick -> frame mapping...\n");
    char msg[160];

    /* mid2agb's TEMPO byte, doubled by ply_tempo */
    ASSERT_EQ(gba_tempo_grid_tempo_i(500000, 24), 120, "120 BPM -> tempoI 120");
    ASSERT_EQ(gba_tempo_grid_tempo_i(500000, 48), 240, "120 BPM with -X -> tempoI 240");
    ASSERT_EQ(gba_tempo_grid_tempo_i(800000, 24), 74, "75 BPM -> tempoI 74 (the byte is 75/2)");
    ASSERT_EQ(gba_tempo_grid_tempo_i(250000, 24), 240, "240 BPM -> tempoI 240");
    ASSERT_EQ(gba_tempo_grid_tempo_i(461539, 24), 130, "129.9998 BPM rounds to 130 (truncation would give 128)");

    /* mid2agb's clocks: 24 * tbs * tick / division, truncated */
    GbaTempoGrid g;
    gba_tempo_grid_init(&g, 96, 24);
    ASSERT_EQ((int)gba_tempo_grid_clock(&g, 3), 0, "division 96: tick 3 -> clock 0");
    ASSERT_EQ((int)gba_tempo_grid_clock(&g, 5), 1, "division 96: tick 5 -> clock 1");
    gba_tempo_grid_init(&g, 24, 48);
    ASSERT_EQ((int)gba_tempo_grid_clock(&g, 3), 6, "-X, division 24: tick 3 -> clock 6");
    /* A loop pass adds whole clocks: loop [2, 388] at division 96 is clock
     * 0 .. 97, so pass 2 of tick 2 is clock 194 (tick 2 + 2 x 386 = 774
     * would truncate to 193). */
    gba_tempo_grid_init(&g, 96, 24);
    ASSERT_EQ((int)gba_tempo_grid_pass_clock(&g, 2, 2, 2, 388), 194, "loop pass 2 of tick 2 at division 96 -> clock 194");
    ASSERT_EQ((int)gba_tempo_grid_pass_clock(&g, 5, 0, 2, 388), 1, "outside the loop: the tick's own clock");

    /* No TEMPO: MPlayStart's 150, a tick every frame */
    gba_tempo_grid_init(&g, 24, 24);
    int ok = 1;
    for (int n = 0; n <= 100; n++)
        ok &= gba_tempo_grid_frame(&g, (uint64_t)n) == n;
    ASSERT(ok, "no TEMPO: tick n on grid frame n");

    /* TEMPO 120 at tick 0: tick n on frame ceil(1.25 n) (frame 3 + that) */
    gba_tempo_grid_init(&g, 24, 24);
    ASSERT_EQ((int)gba_tempo_grid_frame(&g, 0), 0, "120: tick 0 on grid frame 0 (the add of 150)");
    gba_tempo_grid_set_tempo(&g, 500000);
    ok = 1;
    for (int n = 1; n <= 400; n++)
        ok &= gba_tempo_grid_frame(&g, (uint64_t)n) == (5 * n + 3) / 4;
    ASSERT(ok, "120: tick n on grid frame ceil(1.25 n), n = 1 .. 400");

    /* TEMPO 240 at tick 0: two ticks in one frame on three frames in five */
    static const int f240[] = { 0, 1, 2, 2, 3, 4, 4, 5, 5, 6, 7 };
    gba_tempo_grid_init(&g, 24, 24);
    gba_tempo_grid_frame(&g, 0);
    gba_tempo_grid_set_tempo(&g, 250000);
    for (int n = 1; n < (int)(sizeof(f240) / sizeof(f240[0])); n++) {
        snprintf(msg, sizeof(msg), "240: tick %d on grid frame %d", n, f240[n]);
        ASSERT_EQ((int)gba_tempo_grid_frame(&g, (uint64_t)n), f240[n], msg);
    }

    /* A tempo change inside a two-tick frame: 240, then 60 at tick 2.  Tick 3
     * still plays on tick 2's frame (the frame's add was 240), the new add
     * starts on the next frame: tick 4 on frame 4, tick 5 on frame 7. */
    gba_tempo_grid_init(&g, 24, 24);
    gba_tempo_grid_frame(&g, 0);
    gba_tempo_grid_set_tempo(&g, 250000);
    ASSERT_EQ((int)gba_tempo_grid_frame(&g, 2), 2, "240 -> 60: tick 2 on frame 2");
    gba_tempo_grid_set_tempo(&g, 1000000);
    ASSERT_EQ((int)gba_tempo_grid_frame(&g, 3), 2, "240 -> 60: tick 3 still on frame 2");
    ASSERT_EQ((int)gba_tempo_grid_frame(&g, 4), 4, "240 -> 60: tick 4 on frame 4");
    ASSERT_EQ((int)gba_tempo_grid_frame(&g, 5), 7, "240 -> 60: tick 5 on frame 7");
    ASSERT_EQ((int)gba_tempo_grid_frame(&g, 5), 7, "a placed tick queried again keeps its frame");

    /* A TEMPO byte of 0 (512 BPM: 512 / 2 = 256 wraps to 0 in the .byte)
     * stops the ROM's accumulator: no later tick is placed, and a later
     * TEMPO, inside one of those ticks, never runs. */
    ASSERT_EQ(gba_tempo_grid_tempo_i(117188, 24), 0, "512 BPM -> TEMPO byte 0 -> tempoI 0");
    gba_tempo_grid_init(&g, 24, 24);
    ASSERT_EQ((int)gba_tempo_grid_dispatch_sample(&g, 0, 0, 117188, 48000.0), 0, "tempo 0: tick 0 plays");
    ASSERT(gba_tempo_grid_dispatch_sample(&g, 0, 1, 500000, 48000.0) >= UINT64_MAX / 8,
           "tempo 0: tick 1 (and its TEMPO) never plays");
    ASSERT(gba_tempo_grid_frame(&g, 2) == GBA_TEMPO_GRID_NEVER && g.frozen,
           "tempo 0: the later TEMPO did not restart the accumulator");

    /* The dispatch samples: grid frame 0 at the origin (slice 2a's O), the
     * events of one frame on one sample, relative to grid frame 0. */
    static const int rates[] = { 22050, 44100, 48000, 96000 };
    static const int origins[] = { 1158, 2317, 2522, 5045 };
    for (size_t r = 0; r < 4; r++) {
        snprintf(msg, sizeof(msg), "%d Hz: grid frame 0 at the --gba-start-delay origin %d", rates[r], origins[r]);
        ASSERT_EQ((int)gba_tempo_grid_sample(0, rates[r]), origins[r], msg);
    }
    gba_tempo_grid_init(&g, 24, 24);
    ASSERT_EQ((int)gba_tempo_grid_dispatch_sample(&g, 0, 0, 250000, 48000.0), 0,
              "dispatch: tick 0 (the TEMPO) at MIDI time 0");
    uint64_t s2 = gba_tempo_grid_dispatch_sample(&g, 999, 2, 0, 48000.0);
    uint64_t s3 = gba_tempo_grid_dispatch_sample(&g, 999, 3, 0, 48000.0);
    ASSERT(s2 == s3 && s2 == gba_tempo_grid_sample(2, 48000.0) - gba_tempo_grid_sample(0, 48000.0),
           "dispatch: ticks 2 and 3 (one frame) on grid frame 2's sample");

    /* Off: the exact sample, unchanged, whatever the tick or tempo */
    ok = 1;
    for (uint64_t x = 0; x < 200000; x += 977)
        ok &= gba_tempo_grid_dispatch_sample(NULL, x, x / 7, (x & 1) ? 250000 : 0, 48000.0) == x;
    ASSERT(ok, "no grid: every event keeps its exact-tempo sample");

    /* The machine at a grid frame's dispatch: past the previous frame's
     * SoundMain, short of its own; a note-on there starts on that frame. */
    WaveData *wd = seq_test_wave();
    ToneData voices[128];
    memset(voices, 0, sizeof(voices));
    voices[0].type = VOICE_DIRECTSOUND;
    voices[0].key = 60;
    voices[0].wav = wd;
    voices[0].attack = 0xFF;
    voices[0].sustain = 0xFF;
    static const int frames[] = { 1, 5, 37 };
    static const int erates[] = { 22050, 48000, 96000 };
    for (int q = 0; q < 2; q++) {
        for (size_t r = 0; r < 3; r++) {
            for (size_t j = 0; j < 3; j++) {
                M4AEngine engine;
                m4a_engine_init(&engine, (float)erates[r]);
                m4a_engine_set_voicegroup(&engine, voices);
                m4a_engine_boot(&engine, true);
                m4a_engine_set_quantize_events(&engine, q == 1);
                m4a_engine_program_change(&engine, 0, 0);
                const int64_t soundMain = ROM_FIRST_TICK_CYCLE + (int64_t)frames[j] * GBA_CYCLES_PER_FRAME;
                int s = (int)gba_tempo_grid_sample(frames[j], erates[r]);
                float *buf = malloc(sizeof(float) * (size_t)(s + 8192) * 2);
                m4a_engine_process(&engine, buf, buf + s + 8192, s);
                snprintf(msg, sizeof(msg), "%d Hz%s: grid frame %d dispatched after the previous SoundMain and before its own",
                         erates[r], q ? " latched" : "", frames[j]);
                ASSERT(engine.gba.now > soundMain - GBA_CYCLES_PER_FRAME && engine.gba.now < soundMain
                       && engine.nextSoundMain == soundMain, msg);
                m4a_engine_note_on(&engine, 0, 60, 127);
                int more = (int)((double)(soundMain - engine.gba.now + 2 * GBA_BLIP_FRAME) * erates[r] / GBA_CPU_HZ) + 1;
                m4a_engine_process(&engine, buf, buf + s + 8192, more);
                bool started = false;
                for (int i = 0; i < TOTAL_PCM_CHANNELS; i++)
                    if ((engine.pcmChannels[i].status & CHN_ON) && !(engine.pcmChannels[i].status & CHN_START))
                        started = true;
                snprintf(msg, sizeof(msg), "%d Hz%s: a note dispatched on grid frame %d starts on frame %d",
                         erates[r], q ? " latched" : "", frames[j], frames[j] + 3);
                ASSERT(engine.nextSoundMain == soundMain + GBA_CYCLES_PER_FRAME && started, msg);
                free(buf);
                m4a_engine_destroy(&engine);
            }
        }
    }
    free(wd);
}

/* One render of the MIDI path's LFO clock: an engine with no notes, track 0
 * modulated (MOD 1, LFOS 1, so lfoSpeedC counts its LFO steps), the tempo
 * events dispatched as poryaaaa_render dispatches them on the grid (each on
 * its tick's grid frame's sample); `mechanism` is --gba-tempo-grid's
 * m4a_engine_start_song_tempo at the origin and m4a_engine_set_tick_tempo,
 * otherwise m4a_engine_set_tempo_bpm (the flag-off call).  steps[j] = the
 * LFO steps on grid frame j (frame j + 3). */
static int lfo_grid_steps(bool mechanism, bool latch, const int *tempoTick, const uint32_t *tempoUs,
                          int nTempo, int nFrames, int *steps)
{
    int offFrame = 0;   /* frames whose count did not end right after frame j + 3's SoundMain */
    const double rate = 48000.0;
    M4AEngine engine;
    m4a_engine_init(&engine, (float)rate);
    m4a_engine_boot(&engine, true);
    m4a_engine_set_quantize_events(&engine, latch);
    m4a_engine_cc(&engine, 0, 0x15, 1);
    m4a_engine_cc(&engine, 0, 0x1, 1);

    float buf[2 * 4096];
    uint64_t pos = 0;
    uint64_t s0 = gba_tempo_grid_sample(0, rate);
    m4a_engine_process(&engine, buf, buf + 4096, (int)s0);
    pos = s0;
    if (mechanism)
        m4a_engine_start_song_tempo(&engine);

    GbaTempoGrid g;
    gba_tempo_grid_init(&g, 24, 24);
    int k = 0;
    uint64_t tick = 0;
    for (int j = 0; j < nFrames; j++) {
        /* the TEMPOs of grid frame j's ticks, in order */
        while (k < nTempo) {
            while (tick < (uint64_t)tempoTick[k])
                gba_tempo_grid_frame(&g, tick++);
            if (gba_tempo_grid_frame(&g, (uint64_t)tempoTick[k]) != j)
                break;
            gba_tempo_grid_set_tempo(&g, tempoUs[k]);
            if (mechanism)
                m4a_engine_set_tick_tempo(&engine, (uint16_t)gba_tempo_grid_tempo_i(tempoUs[k], 24));
            else
                m4a_engine_set_tempo_bpm(&engine, 60000000.0 / tempoUs[k]);
            k++;
        }
        uint8_t before = engine.tracks[0].lfoSpeedC;
        uint64_t next = s0 + gba_tempo_grid_sample(j + 1, rate) - gba_tempo_grid_sample(0, rate);
        m4a_engine_process(&engine, buf, buf + 4096, (int)(next - pos));
        pos = next;
        steps[j] = (uint8_t)(engine.tracks[0].lfoSpeedC - before);
        /* grid frame j is ROM frame j + 3: its SoundMain has run, the next not */
        if (engine.nextSoundMain != ROM_FIRST_TICK_CYCLE + (int64_t)(j + 1) * GBA_CYCLES_PER_FRAME)
            offFrame++;
    }
    m4a_engine_destroy(&engine);
    return offFrame;
}

/* The walker's ticks per grid frame for the same tempo list. */
static void lfo_grid_walker(const int *tempoTick, const uint32_t *tempoUs, int nTempo, int nFrames, int *ticks)
{
    GbaTempoGrid g;
    gba_tempo_grid_init(&g, 24, 24);
    memset(ticks, 0, sizeof(int) * (size_t)nFrames);
    int k = 0;
    for (uint64_t n = 0;; n++) {
        int64_t f = gba_tempo_grid_frame(&g, n);
        if (f >= nFrames)
            break;
        ticks[f]++;
        while (k < nTempo && (uint64_t)tempoTick[k] == n)
            gba_tempo_grid_set_tempo(&g, tempoUs[k++]);
    }
}

/*
 * poryaaaa_render --gba-tempo-grid also puts the engine's own tempo
 * accumulator (the MIDI path's LFO clock) on the walker's frames: MPlayStart's
 * tempoI 150 / tempoC 0 at the origin, and a TEMPO dispatched on its tick's
 * frame counting from the next frame's add (ply_tempo runs after
 * _081DD86C's add).  Without the two calls the accumulator runs as before:
 * from the boot's tick, the tempo added on the frame it is dispatched
 * before.  A sequencer-driven engine ignores both (docs/ACCURACY.md, the renderer's "as the game" mode).
 */
static void test_engine_lfo_grid(void)
{
    printf("Testing the engine's LFO clock on the --gba-tempo-grid frames...\n");
    char msg[160];
    enum { NF = 400 };
    int steps[NF], ticks[NF];

    static const int t0[] = { 0 };
    static const uint32_t us120[] = { 500000 }, us240[] = { 250000 };
    static const int tChange[] = { 0, 2 };
    static const uint32_t usChange[] = { 250000, 1000000 };
    static const uint32_t us512[] = { 117188 };   /* TEMPO byte 0 */
    struct { const char *name; const int *tick; const uint32_t *us; int n; } cases[] = {
        { "120", t0, us120, 1 },
        { "240", t0, us240, 1 },
        { "240 -> 60 at tick 2", tChange, usChange, 2 },
        { "512 (tempo 0)", t0, us512, 1 },
    };
    for (int q = 0; q < 2; q++) {
        for (size_t c = 0; c < sizeof(cases) / sizeof(cases[0]); c++) {
            int off = lfo_grid_steps(true, q == 1, cases[c].tick, cases[c].us, cases[c].n, NF, steps);
            lfo_grid_walker(cases[c].tick, cases[c].us, cases[c].n, NF, ticks);
            int bad = -1;
            for (int j = 0; j < NF && bad < 0; j++)
                if (steps[j] != ticks[j])
                    bad = j;
            snprintf(msg, sizeof(msg), "%s%s: the LFO steps on the walker's frames, grid frames 0 .. %d = ROM frames 3 .. %d"
                     " (first mismatch %d, %d frames off the ROM's SoundMain)",
                     cases[c].name, q ? " latched" : "", NF - 1, NF + 2, bad, off);
            ASSERT(bad < 0 && off == 0, msg);
        }
    }

    /* The frames, spelled out (grid frame j = frame j + 3) */
    lfo_grid_steps(true, false, t0, us120, 1, NF, steps);
    static const int on120[] = { 0, 2, 3, 4, 5, 7, 8, 9, 10, 12 };
    int ok = 1, n = 0;
    for (int j = 0; j < 13; j++)
        if (steps[j]) ok &= n < 10 && on120[n++] == j && steps[j] == 1;
    ASSERT(ok && n == 10, "120: LFO steps on frames 3, 5, 6, 7, 8, 10, ... (3 + ceil(1.25 n))");
    lfo_grid_steps(true, false, t0, us240, 1, NF, steps);
    static const int per240[] = { 1, 1, 2, 1, 2, 2, 1, 2 };
    ok = 1;
    for (int j = 0; j < 8; j++) ok &= steps[j] == per240[j];
    ASSERT(ok, "240: 1, 1, 2, 1, 2, 2, 1, 2 steps on frames 3 .. 10");
    lfo_grid_steps(true, false, tChange, usChange, 2, NF, steps);
    static const int perChange[] = { 1, 1, 2, 0, 1, 0, 0, 1, 0, 1 };
    ok = 1;
    for (int j = 0; j < 10; j++) ok &= steps[j] == perChange[j];
    ASSERT(ok, "240 -> 60 at tick 2: tick 3 still on frame 5 (two steps), then frames 7, 10, 12");
    lfo_grid_steps(true, false, t0, us512, 1, NF, steps);
    ok = steps[0] == 1;
    for (int j = 1; j < NF; j++) ok &= steps[j] == 0;
    ASSERT(ok, "tempo 0: the step of tick 0, then none (the accumulator stops, as the walker)");

    /* Without the calls (set_tempo_bpm on the same grid samples, slice 3a's
     * default): as before the change.  The boot's frame-2 tick has taken
     * MPlayStart's 150, the tempo dispatched before frame 3 is added on
     * frame 3. */
    for (int q = 0; q < 2; q++) {
        lfo_grid_steps(false, q == 1, t0, us120, 1, NF, steps);
        ok = 1; n = 0;
        for (int j = 0; j < NF; j++) {
            if (!steps[j]) continue;
            ok &= steps[j] == 1 && j == 1 + n + n / 4;   /* frame 4 + n + floor(n / 4) */
            n++;
        }
        snprintf(msg, sizeof(msg), "without the calls%s, 120: LFO step n on frame 4 + n + floor(n / 4) (%d steps)", q ? " latched" : "", n);
        ASSERT(ok && n == 320, msg);
        lfo_grid_steps(false, q == 1, t0, us240, 1, NF, steps);
        static const int old240[] = { 1, 2, 1, 2, 2, 1, 2, 1 };
        ok = 1;
        for (int j = 0; j < 8; j++) ok &= steps[j] == old240[j];
        snprintf(msg, sizeof(msg), "without the calls%s, 240: 1, 2, 1, 2, 2, 1, 2, 1 steps on frames 3 .. 10", q ? " latched" : "");
        ASSERT(ok, msg);
        lfo_grid_steps(false, q == 1, tChange, usChange, 2, NF, steps);
        static const int oldChange[] = { 1, 2, 0, 1, 0, 0, 1, 0, 1 };
        ok = 1;
        for (int j = 0; j < 9; j++) ok &= steps[j] == oldChange[j];
        snprintf(msg, sizeof(msg), "without the calls%s, 240 -> 60 at tick 2: 1, 2, 0, 1, 0, 0, 1, 0, 1 (60 added on its own frame)",
                 q ? " latched" : "");
        ASSERT(ok, msg);
    }

    /* A sequencer-driven engine: both calls are no-ops, its LFO is the
     * sequencer's (one step per song tick) and the engine's accumulator
     * stays where it was. */
    ToneData voices[128];
    seq_test_voices(voices);
    M4ASong *song = seq_song(
        "\t.byte\tTEMPO , 75\n"
        "\t.byte\t\tVOICE , 0\n"
        "\t.byte\t\tLFOS  , 1\n"
        "\t.byte\t\tMOD   , 64\n"
        "\t.byte\t\tTIE   , Cn3 , v127\n"
        "\t.byte\tW96\n"
        "\t.byte\tW96\n"
        "\t.byte\tEOT\n"
        "\t.byte\tFINE\n");
    ASSERT(song != NULL, "sequencer song parses");
    if (!song) return;
    static M4AEngine se[2];
    M4ASequencer seq[2];
    for (int k = 0; k < 2; k++) {
        m4a_engine_init(&se[k], 48000.0f);
        m4a_engine_set_voicegroup(&se[k], voices);
        m4a_sequencer_init(&seq[k], &se[k]);
        m4a_sequencer_start(&seq[k], song);
        se[k].tempoI = 77;
        se[k].tempoC = 33;
        for (int i = 0; i < 100; i++) {
            if (k == 1 && i == 10) m4a_engine_start_song_tempo(&se[k]);
            if (k == 1 && i == 20) m4a_engine_set_tick_tempo(&se[k], 60);
            m4a_engine_tick(&se[k]);
        }
    }
    ASSERT(se[1].tempoI == 77 && se[1].tempoC == 33 && !se[1].tempoINextSet,
           "sequencer: start_song_tempo / set_tick_tempo leave the engine's accumulator alone");
    ASSERT(se[1].tracks[0].lfoSpeedC == se[0].tracks[0].lfoSpeedC && se[0].tracks[0].lfoSpeedC == 100
           && memcmp(se[0].cgbChannels, se[1].cgbChannels, sizeof(se[0].cgbChannels)) == 0,
           "sequencer: with the calls the LFO (100 steps in 100 ticks) and the channels equal a run without them");
    for (int k = 0; k < 2; k++) {
        m4a_sequencer_detach(&seq[k]);
        m4a_engine_destroy(&se[k]);
    }
    m4a_song_free(song);
}

/* The last NR12 / NR13 / NR14 values a PSG got, and in which frame. */
typedef struct {
    uint32_t nr12, nr13, nr14;
    int nr12Writes;
} LfoOrderLog;

static void lfo_order_log_cb(void *user, uint32_t reg, uint32_t value)
{
    LfoOrderLog *log = (LfoOrderLog *)user;
    if (reg == PSG_REG_NR12) { log->nr12 = value; log->nr12Writes++; }
    if (reg == PSG_REG_NR13) log->nr13 = value;
    if (reg == PSG_REG_NR14) log->nr14 = value;
}

static void lfo_order_nr11_cb(void *user, uint32_t reg, uint32_t value)
{
    if (reg == PSG_REG_NR11)
        *(uint32_t *)user = value;
}

/* One frame: past the next SoundMain and its CgbSound writes (well inside
 * the frame: CgbSound's writes end a few thousand cycles after its entry). */
static void lfo_order_frame(M4AEngine *e, float *outL, float *outR)
{
    int64_t target = e->nextSoundMain + 60000;
    while (e->gba.now < target)
        m4a_engine_process(e, outL, outR, 64);
}

/*
 * Without a sequencer the frame's LFO steps (and portamento, PWM) run before
 * CgbSound, as MPlayMain does in SoundMain: a square's vibrato pitch set by a
 * frame's LFO step is in NR13 / NR14 after that frame's CgbSound, and a
 * tremolo step's volume change is written (NR12) in the same frame (docs/ACCURACY.md,
 * "Rules for every source").  The sequencer path,
 * whose MPlayMain runs in the frame hook, was already in this order.
 */
static void test_lfo_cgbsound_order(void)
{
    printf("Testing the LFO before CgbSound on the MIDI path...\n");
    char msg[160];
    ToneData voices[128];
    memset(voices, 0, sizeof(voices));
    voices[0].type = VOICE_SQUARE_1;     /* sq1, 50 % duty, sustain 15, no sweep */
    voices[0].key = 60;
    voices[0].wavePointer = (uint32_t *)(uintptr_t)2;
    voices[0].sustain = 15;
    float outL[1024], outR[1024];

    for (int modT = 0; modT < 2; modT++) {
        for (int q = 0; q < 2; q++) {
            M4AEngine engine;
            engine_init_booted(&engine, 48000.0f);
            m4a_engine_set_voicegroup(&engine, voices);
            m4a_engine_set_quantize_events(&engine, q == 1);
            LfoOrderLog log = { 0 };
            engine.psg[0].writeLog = lfo_order_log_cb;
            engine.psg[0].writeLogUser = &log;
            m4a_engine_set_tempo_bpm(&engine, 120.0);     /* the plugin's call */
            m4a_engine_program_change(&engine, 0, 0);
            m4a_engine_cc(&engine, 0, 0x16, (uint8_t)modT);
            m4a_engine_cc(&engine, 0, 0x15, 40);
            m4a_engine_cc(&engine, 0, 0x1, 24);
            m4a_engine_note_on(&engine, 0, 60, 127);
            M4ACGBChannel *sq = &engine.cgbChannels[0];
            int bad = 0, changes = 0;
            for (int f = 0; f < 90; f++) {
                int8_t modM = engine.tracks[0].modM;
                int writes = log.nr12Writes;
                lfo_order_frame(&engine, outL, outR);
                bool stepped = engine.tracks[0].modM != modM;
                changes += stepped;
                if (modT == 0) {
                    uint32_t written = log.nr13 | ((log.nr14 & 7) << 8);
                    bad += written != (sq->frequency & 0x7FF);
                } else if (f > 0 && stepped) {
                    bad += log.nr12Writes == writes;    /* the tremolo step written this frame */
                }
            }
            snprintf(msg, sizeof(msg), modT ? "tremolo%s: an NR12 write in every frame whose LFO step changes modM"
                                            " (%d modM changes in 90 frames, %d frames without)"
                                          : "vibrato%s: NR13 / NR14 hold the channel frequency after every frame"
                                            " (%d modM changes in 90 frames, %d frames late)",
                     q ? " latched" : "", changes, bad);
            ASSERT(bad == 0 && changes > 20, msg);
            m4a_engine_destroy(&engine);
        }
    }

    /* Against the sequencer path (the ROM's order, byte-checked by the
     * harness): the same note through the MIDI path on the grid (MPlayStart's
     * seed at the origin, the TEMPO held to the next add) and through a song
     * starting on frame 3 leave the same NR12 / NR13 / NR14 values after every
     * frame -- a tremolo step's volume and a vibrato step's pitch in the frame
     * of the step. */
    for (int modT = 0; modT < 2; modT++) {
        char text[512];
        snprintf(text, sizeof(text),
                 "\t.byte\tTEMPO , 60\n\t.byte\t\tVOICE , 0\n\t.byte\t\tVOL   , 127\n"
                 "\t.byte\t\tMODT  , %d\n\t.byte\t\tLFOS  , 40\n\t.byte\t\tMOD   , 24\n"
                 "\t.byte\t\tTIE   , Cn3 , v127\n\t.byte\tW96\n\t.byte\tEOT\n\t.byte\tFINE\n", modT);
        M4ASong *refSong = seq_song(text);
        ASSERT(refSong != NULL, "reference song parses");
        if (!refSong) continue;
        static M4AEngine pair[2];
        M4ASequencer refSeq;
        LfoOrderLog plog[2] = { { 0 }, { 0 } };
        for (int k = 0; k < 2; k++) {
            m4a_engine_init(&pair[k], 48000.0f);
            m4a_engine_set_voicegroup(&pair[k], voices);
            pair[k].psg[0].writeLog = lfo_order_log_cb;
            pair[k].psg[0].writeLogUser = &plog[k];
            m4a_engine_boot(&pair[k], true);   /* output kept: sample s is the renderer's sample s */
        }
        m4a_sequencer_init(&refSeq, &pair[1]);
        m4a_sequencer_start(&refSeq, refSong);                /* first tick on frame 3 */
        int s0 = (int)gba_tempo_grid_sample(0, 48000.0);       /* the origin, before frame 3 */
        float *obuf = malloc(sizeof(float) * (size_t)s0 * 2);
        m4a_engine_process(&pair[0], obuf, obuf + s0, s0);
        free(obuf);
        m4a_engine_start_song_tempo(&pair[0]);
        m4a_engine_set_tick_tempo(&pair[0], 120);
        m4a_engine_program_change(&pair[0], 0, 0);
        m4a_engine_cc(&pair[0], 0, 0x16, (uint8_t)modT);
        m4a_engine_cc(&pair[0], 0, 0x15, 40);
        m4a_engine_cc(&pair[0], 0, 0x1, 24);
        m4a_engine_note_on(&pair[0], 0, 60, 127);
        int bad = 0, changes = 0;
        for (int f = 0; f < 90; f++) {
            uint32_t before = plog[1].nr12 | plog[1].nr13 << 8 | plog[1].nr14 << 16;
            for (int k = 0; k < 2; k++)
                lfo_order_frame(&pair[k], outL, outR);
            changes += (plog[1].nr12 | plog[1].nr13 << 8 | plog[1].nr14 << 16) != before;
            bad += plog[0].nr12 != plog[1].nr12 || plog[0].nr13 != plog[1].nr13 || plog[0].nr14 != plog[1].nr14;
        }
        snprintf(msg, sizeof(msg), "%s: the MIDI path's NR12 / NR13 / NR14 after each frame are the sequencer's"
                 " (%d frames changed, %d differ)", modT ? "tremolo" : "vibrato", changes, bad);
        ASSERT(bad == 0 && changes > 8, msg);
        m4a_sequencer_detach(&refSeq);
        for (int k = 0; k < 2; k++)
            m4a_engine_destroy(&pair[k]);
        m4a_song_free(refSong);
    }

    /* PWM before CgbSound (opt-in): pattern 1 (duty 2, 1, 0) at speed S --
     * MPlayProcessPulseWidthMod skips the start frame, so step k is in NR11
     * from frame F + k * S, F the note's start frame (S = 1: duty 2 still
     * sounds in the start frame). */
    for (int speed = 1; speed <= 2; speed++) {
        M4AEngine engine;
        engine_init_booted(&engine, 48000.0f);
        m4a_engine_set_voicegroup(&engine, voices);
        m4a_engine_set_pwm_enabled(&engine, true);
        uint32_t nr11 = 0xFFFF;
        engine.psg[0].writeLog = lfo_order_nr11_cb;
        engine.psg[0].writeLogUser = &nr11;
        m4a_engine_program_change(&engine, 0, 0);
        m4a_engine_cc(&engine, 0, 0x19, (uint8_t)speed);
        m4a_engine_cc(&engine, 0, 0x17, 1);
        m4a_engine_note_on(&engine, 0, 60, 127);
        static const uint8_t duty1[3] = { 2, 1, 0 };
        int bad = 0;
        for (int f = 0; f < 12; f++) {
            lfo_order_frame(&engine, outL, outR);
            bad += (nr11 >> 6) != duty1[(f / speed) % 3];
        }
        snprintf(msg, sizeof(msg), "PWM speed %d: step k in NR11 from frame F + k x %d (%d frames off)", speed, speed, bad);
        ASSERT(bad == 0, msg);
        m4a_engine_destroy(&engine);
    }

    /* The sequencer path: the same vibrato from a song, unchanged order. */
    M4ASong *song = seq_song(
        "\t.byte\tTEMPO , 120/2\n"
        "\t.byte\t\tVOICE , 0\n"
        "\t.byte\t\tMODT  , 0\n"
        "\t.byte\t\tLFOS  , 40\n"
        "\t.byte\t\tMOD   , 24\n"
        "\t.byte\t\tTIE   , Cn3 , v127\n"
        "\t.byte\tW96\n"
        "\t.byte\tEOT\n"
        "\t.byte\tFINE\n");
    ASSERT(song != NULL, "vibrato song parses");
    if (!song) return;
    M4AEngine engine;
    engine_init_booted(&engine, 48000.0f);
    m4a_engine_set_voicegroup(&engine, voices);
    LfoOrderLog log = { 0 };
    engine.psg[0].writeLog = lfo_order_log_cb;
    engine.psg[0].writeLogUser = &log;
    M4ASequencer seq;
    m4a_sequencer_init(&seq, &engine);
    m4a_sequencer_start(&seq, song);
    int bad = 0, changes = 0;
    for (int f = 0; f < 90; f++) {
        uint32_t freq = engine.cgbChannels[0].frequency;
        lfo_order_frame(&engine, outL, outR);
        changes += engine.cgbChannels[0].frequency != freq;
        bad += (log.nr13 | ((log.nr14 & 7) << 8)) != (engine.cgbChannels[0].frequency & 0x7FF);
    }
    snprintf(msg, sizeof(msg), "sequencer: the vibrato pitch in NR13 / NR14 every frame (%d changes, %d late)", changes, bad);
    ASSERT(bad == 0 && changes > 20, msg);
    m4a_sequencer_detach(&seq);
    m4a_engine_destroy(&engine);
    m4a_song_free(song);
}

/* Every PSG write of one engine, with its cycle (the renderer's --psg-log). */
#define CGB_ENTRY_LOG_MAX 4096
typedef struct {
    const M4AEngine *engine;
    int n;
    int64_t cycle[CGB_ENTRY_LOG_MAX];
    uint16_t regVal[CGB_ENTRY_LOG_MAX];
} CgbEntryLog;

static void cgb_entry_log_cb(void *user, uint32_t reg, uint32_t value)
{
    CgbEntryLog *log = (CgbEntryLog *)user;
    if (log->n >= CGB_ENTRY_LOG_MAX)
        return;
    log->cycle[log->n] = log->engine->gba.now;
    log->regVal[log->n] = (uint16_t)((reg & 0xFF) << 8 | (value & 0xFF));
    log->n++;
}

/* A MIDI-path event for test_cgb_entry_midi_path: at song tick `tick`, on
 * engine track `track`: a note-on (b > 0) / note-off (b == 0) of key a, a CC
 * a = b (kind 'c'), a program change a (kind 'p'), a bend of c_v+a (kind
 * 'b') or the track's end, mid2agb's FINE (kind 'e'). */
typedef struct { int tick; char kind; int track, a, b; } CgbEntryEvent;

static void cgb_entry_dispatch(M4AEngine *e, const CgbEntryEvent *ev)
{
    switch (ev->kind) {
    case 'n':
        if (ev->b > 0) m4a_engine_note_on(e, ev->track, (uint8_t)ev->a, (uint8_t)ev->b);
        else m4a_engine_note_off(e, ev->track, (uint8_t)ev->a);
        break;
    case 'c': m4a_engine_cc(e, ev->track, (uint8_t)ev->a, (uint8_t)ev->b); break;
    case 'p': m4a_engine_program_change(e, ev->track, (uint8_t)ev->a); break;
    case 'b': m4a_engine_pitch_bend(e, ev->track, (int16_t)(ev->a * 128)); break;
    case 'e': m4a_engine_track_end(e, ev->track); break;
    }
}

/* The material through both paths: engine 0 the MIDI path (the renderer's
 * calls with --gba-start-delay --gba-tempo-grid: MPlayStart's seed at the
 * origin, the TEMPO held to the next add, each event before the SoundMain
 * of its tick's frame) with a timing-only sequencer over `text`; engine 1 a
 * sequencer playing `text`.  Both booted with their output, as the renderer
 * does.  Returns the number of frames 3 .. 2 + nFrames whose CgbSound entry
 * or PSG writes (register, value and cycle) differ; entry[] gets engine 0's
 * entries from line 160 (SoundMain + 197).  `tempoI`: the song's. */
static int cgb_entry_pair(const char *text, const ToneData *voices, const CgbEntryEvent *evs, int nEvs,
                          int tempoI, bool latch, int nFrames, int *entry, int *writes, int *firstBad)
{
    M4ASong *song = parse_song_text(text);
    if (!song)
        return -1;
    static M4AEngine pair[2];
    static CgbEntryLog log[2];
    M4ASequencer seq[2];
    for (int k = 0; k < 2; k++) {
        m4a_engine_init(&pair[k], 48000.0f);
        m4a_engine_set_voicegroup(&pair[k], (ToneData *)voices);
        log[k].engine = &pair[k];
        log[k].n = 0;
        pair[k].psg[0].writeLog = cgb_entry_log_cb;
        pair[k].psg[0].writeLogUser = &log[k];
        m4a_engine_boot(&pair[k], true);
    }
    m4a_engine_set_quantize_events(&pair[0], latch);
    m4a_sequencer_init_timing(&seq[0], &pair[0]);
    m4a_sequencer_set_voicegroup(&seq[0], voices);
    m4a_sequencer_init(&seq[1], &pair[1]);
    m4a_sequencer_start(&seq[1], song);                    /* first tick on frame 3 */
    int s0 = (int)gba_tempo_grid_sample(0, 48000.0);       /* the origin, before frame 3 */
    float *obuf = malloc(sizeof(float) * (size_t)s0 * 2);
    m4a_engine_process(&pair[0], obuf, obuf + s0, s0);
    free(obuf);
    m4a_engine_start_song_tempo(&pair[0]);
    m4a_sequencer_start(&seq[0], song);
    m4a_engine_set_tick_tempo(&pair[0], (uint16_t)tempoI);

    /* The song's tick frames: MPlayStart's tempoI 150 on frame 3 (tick 0,
     * where the TEMPO is read), the song's from frame 4. */
    int tickFrame[512], nTicks = 0, acc = 0;
    for (int f = 3; f < 3 + nFrames && nTicks < 512; f++) {
        acc += f == 3 ? 150 : tempoI;
        while (acc >= 150 && nTicks < 512) {
            tickFrame[nTicks++] = f;
            acc -= 150;
        }
    }
    float outL[1024], outR[1024];
    int bad = 0, ev = 0, w0 = 0, w1 = 0;
    *firstBad = -1;
    *writes = 0;
    for (int f = 3; f < 3 + nFrames; f++) {
        while (ev < nEvs && evs[ev].tick < nTicks && tickFrame[evs[ev].tick] == f)
            cgb_entry_dispatch(&pair[0], &evs[ev++]);
        for (int k = 0; k < 2; k++)
            lfo_order_frame(&pair[k], outL, outR);   /* past frame f's SoundMain and CgbSound */
        entry[f - 3] = pair[0].cgbEntryCycles + M4A_CYC_SOUNDMAIN_ENTRY;
        bool same = pair[0].cgbEntryCycles == pair[1].cgbEntryCycles
                 && log[0].n - w0 == log[1].n - w1;
        for (int i = 0; same && i < log[0].n - w0; i++)
            same = log[0].cycle[w0 + i] == log[1].cycle[w1 + i] && log[0].regVal[w0 + i] == log[1].regVal[w1 + i];
        *writes += log[0].n - w0;
        if (!same && (*firstBad < 0))
            *firstBad = f;
        bad += !same;
        w0 = log[0].n;
        w1 = log[1].n;
    }
    for (int k = 0; k < 2; k++) {
        m4a_sequencer_detach(&seq[k]);
        m4a_engine_destroy(&pair[k]);
    }
    m4a_song_free(song);
    return bad;
}

/*
 * The CgbSound entry on the MIDI path (docs/ACCURACY.md, the renderer's "as the game" mode): with a
 * timing-only sequencer over the song mid2agb makes of the MIDI (here the
 * song's text itself), the MIDI path's CgbSound starts on the cycle the
 * sequencer path's does and its PSG writes land on the same cycles, frame
 * for frame -- tick and no-tick frames, note-on and gate-expiry frames, LFO
 * frames, two tracks -- latched and not; on the sq1 song both are mgba's
 * measured entries.  A timing-only sequencer drives nothing.
 */
static void test_cgb_entry_midi_path(void)
{
    printf("Testing the CgbSound entry on the MIDI path (timing-only sequencer)...\n");
    char msg[200];
    ToneData voices[128];
    seq_test_voices(voices);                 /* 0: square 1, sustain 15 */
    voices[1] = voices[0];
    voices[1].type = VOICE_SQUARE_2;         /* 1: square 2 */
    int entry[160], writes, firstBad;

    /* sq1 (test/accuracy/rom/micro/sq1.s): the probe's entries, frames 3 .. 52
     * (test_sequencer_cycles' column), now through the MIDI path. */
    char text[4096];
    snprintf(text, sizeof(text), "%s%s%s", seq_song_header,
             "\t.byte\tKEYSH , t_key+0\n"
             "\t.byte\tTEMPO , 120*t_tbs/2\n"
             "\t.byte\t\tVOICE , 0\n"
             "\t.byte\t\tVOL   , 127*t_mvl/mxv\n"
             "\t.byte\t\tPAN   , c_v+0\n"
             "\t.byte\t\tN24   , Cn3 , v127\n"
             "\t.byte\tW36\n"
             "\t.byte\t\t        Cn4 \n"
             "\t.byte\tW36\n"
             "\t.byte\t\tN48   , Cn2 \n"
             "\t.byte\tW24\n"
             "\t.byte\tW24\n"
             "\t.byte\tFINE\n", seq_song_footer);
    static const CgbEntryEvent sq1[] = {
        { 0, 'p', 1, 0, 0 }, { 0, 'c', 1, 7, 127 }, { 0, 'c', 1, 10, 64 }, { 0, 'n', 1, 60, 127 },
        { 24, 'n', 1, 60, 0 }, { 36, 'n', 1, 72, 127 }, { 60, 'n', 1, 72, 0 },
        { 72, 'n', 1, 48, 127 }, { 120, 'n', 1, 48, 0 },
    };
    static const int probe[50] = {
        4069, 1402, 1866, 1866, 1866, 1866, 1402, 1866, 1866, 1866,
        1866, 1402, 1866, 1866, 1866, 1866, 1402, 1866, 1866, 1866,
        1866, 1402, 1866, 1866, 1866, 1866, 1402, 1866, 1866, 1866,
        1874, 1402, 1953, 1840, 1840, 1840, 1402, 1840, 1840, 1840,
        1840, 1402, 1840, 1840, 1840, 2790, 1402, 1866, 1866, 1866 };
    for (int q = 0; q < 2; q++) {
        int bad = cgb_entry_pair(text, voices, sq1, (int)(sizeof(sq1) / sizeof(sq1[0])), 120, q == 1, 160,
                                 entry, &writes, &firstBad);
        snprintf(msg, sizeof(msg), "sq1%s: the MIDI path's CgbSound entry and PSG write cycles are the sequencer's,"
                 " frames 3 .. 162 (%d differ, first %d; %d writes)", q ? " latched" : "", bad, firstBad, writes);
        ASSERT(bad == 0 && writes > 20, msg);
        int off = 0;
        for (int f = 0; f < 50; f++)
            off += entry[f] != probe[f];
        snprintf(msg, sizeof(msg), "sq1%s: the MIDI path's entries on frames 3 .. 52 are mgba's (%d differ)",
                 q ? " latched" : "", off);
        ASSERT_EQ(off, 0, msg);
    }

    /* Without the timing-only sequencer: the fixed one-track no-tick value on
     * every frame (the plugin's), 2667 early on frame 3 as measured. */
    {
        M4AEngine e;
        engine_init_booted(&e, 48000.0f);
        ASSERT_EQ(e.cgbEntryCycles + M4A_CYC_SOUNDMAIN_ENTRY, 1402,
                  "no timing-only sequencer: the fixed entry, 1402 from line 160");
        ASSERT(e.frameHook == NULL && e.frameEndHook == NULL, "no hooks on a plain engine");
        m4a_engine_destroy(&e);
    }

    /* Two tracks: track 1 square 1 notes under running status, track 2 a
     * square-2 TIE with a delayed vibrato (the delay countdown, then LFO
     * frames and the VOLCHG / PITCHG pass) ended by EOT. */
    snprintf(text, sizeof(text),
             "%s"
             "\t.byte\tKEYSH , t_key+0\n"
             "\t.byte\tTEMPO , 120*t_tbs/2\n"
             "\t.byte\t\tVOICE , 0\n"
             "\t.byte\t\tVOL   , 100*t_mvl/mxv\n"
             "\t.byte\t\tN24   , Cn3 , v127\n"
             "\t.byte\tW24\n"
             "\t.byte\t\t        Dn3 \n"
             "\t.byte\tW24\n"
             "\t.byte\t\tN12   , En3 , v100\n"
             "\t.byte\tW48\n"
             "\t.byte\tFINE\n"
             "t_2:\n"
             "\t.byte\tKEYSH , t_key+0\n"
             "\t.byte\t\tVOICE , 1\n"
             "\t.byte\t\tVOL   , 90*t_mvl/mxv\n"
             "\t.byte\t\tLFOS  , 40\n"
             "\t.byte\t\tLFODL , 6\n"
             "\t.byte\t\tMOD   , 24\n"
             "\t.byte\t\tTIE   , Gn3 , v127\n"
             "\t.byte\tW96\n"
             "\t.byte\tW12\n"
             "\t.byte\t\tEOT   \n"
             "\t.byte\tW12\n"
             "\t.byte\tFINE\n"
             "\t.align\t2\n"
             "t:\n"
             "\t.byte\t2, 0, t_pri, t_rev\n"
             "\t.word\tt_grp\n"
             "\t.word\tt_1\n"
             "\t.word\tt_2\n", seq_song_header);
    static const CgbEntryEvent two[] = {
        { 0, 'p', 1, 0, 0 }, { 0, 'c', 1, 7, 100 }, { 0, 'n', 1, 60, 127 },
        { 0, 'p', 2, 1, 0 }, { 0, 'c', 2, 7, 90 }, { 0, 'c', 2, 0x15, 40 }, { 0, 'c', 2, 0x1A, 6 }, { 0, 'c', 2, 1, 24 },
        { 0, 'n', 2, 67, 127 },
        { 24, 'n', 1, 60, 0 }, { 24, 'n', 1, 62, 127 }, { 48, 'n', 1, 62, 0 }, { 48, 'n', 1, 64, 100 },
        { 60, 'n', 1, 64, 0 }, { 108, 'n', 2, 67, 0 },
    };
    for (int q = 0; q < 2; q++) {
        int bad = cgb_entry_pair(text, voices, two, (int)(sizeof(two) / sizeof(two[0])), 120, q == 1, 150,
                                 entry, &writes, &firstBad);
        int tick = 0, ticks = 0;
        for (int f = 1; f < 150; f++) {
            tick += entry[f] > 1600;        /* a tick frame costs well over a no-tick one */
            ticks++;
        }
        snprintf(msg, sizeof(msg), "two tracks with vibrato%s: entry and PSG write cycles are the sequencer's,"
                 " frames 3 .. 152 (%d differ, first %d; %d writes; %d of %d frames tick)",
                 q ? " latched" : "", bad, firstBad, writes, tick, ticks);
        ASSERT(bad == 0 && writes > 100 && tick > 50 && tick < ticks, msg);
    }

    /* DirectSound: ply_note's channel scan, channel by channel -- a note
     * under running status, then a three-note chord in one tick while the
     * earlier notes release (ON + STOP candidates, equal-priority compares,
     * the unlinks). */
    WaveData *wd = seq_test_wave();
    voices[2].type = VOICE_DIRECTSOUND;
    voices[2].key = 60;
    voices[2].wav = wd;
    voices[2].attack = 0xFF;
    voices[2].sustain = 0xFF;
    voices[2].release = 0xE0;
    snprintf(text, sizeof(text), "%s%s%s", seq_song_header,
             "\t.byte\tKEYSH , t_key+0\n"
             "\t.byte\tTEMPO , 120*t_tbs/2\n"
             "\t.byte\t\tVOICE , 2\n"
             "\t.byte\t\tVOL   , 127*t_mvl/mxv\n"
             "\t.byte\t\tN12   , Cn3 , v127\n"
             "\t.byte\tW12\n"
             "\t.byte\t\t        Dn3 \n"
             "\t.byte\tW12\n"
             "\t.byte\t\tN24   , Cn3 \n"
             "\t.byte\t\tN24   , En3 \n"     /* mid2agb repeats the op inside a tick */
             "\t.byte\t\tN24   , Gn3 \n"
             "\t.byte\tW24\n"
             "\t.byte\t\tN12   , An3 \n"
             "\t.byte\tW24\n"
             "\t.byte\tFINE\n", seq_song_footer);
    static const CgbEntryEvent pcm[] = {
        { 0, 'p', 1, 2, 0 }, { 0, 'c', 1, 7, 127 }, { 0, 'n', 1, 60, 127 },
        { 12, 'n', 1, 60, 0 }, { 12, 'n', 1, 62, 127 },
        { 24, 'n', 1, 62, 0 }, { 24, 'n', 1, 60, 127 }, { 24, 'n', 1, 64, 127 }, { 24, 'n', 1, 67, 127 },
        { 48, 'n', 1, 60, 0 }, { 48, 'n', 1, 64, 0 }, { 48, 'n', 1, 67, 0 }, { 48, 'n', 1, 69, 127 },
        { 60, 'n', 1, 69, 0 },
    };
    for (int q = 0; q < 2; q++) {
        int bad = cgb_entry_pair(text, voices, pcm, (int)(sizeof(pcm) / sizeof(pcm[0])), 120, q == 1, 100,
                                 entry, &writes, &firstBad);
        /* tick 12 (frame 18) one note, tick 24 (frame 33) the chord: three
         * notes, each a scan past the channels before it */
        int one = entry[18 - 3] - entry[17 - 3], chord = entry[33 - 3] - entry[32 - 3];
        snprintf(msg, sizeof(msg), "DirectSound chord%s: the entry is the sequencer's, frames 3 .. 102"
                 " (%d differ, first %d; one note +%d, the chord +%d)", q ? " latched" : "", bad, firstBad, one, chord);
        ASSERT(bad == 0 && one > 800 && chord > 3 * one, msg);
    }
    free(wd);

    /* A timing-only sequencer drives nothing: with no MIDI events its engine's
     * channels never start, its tracks keep their state, the song header's
     * reverb is not applied -- and its entries are still the song's. */
    {
        /* every command that sets track state on the --song path */
        M4ASong *song = seq_song(
            "\t.byte\tKEYSH , t_key+2\n"
            "\t.byte\tTEMPO , 120*t_tbs/2\n"
            "\t.byte\t\tVOICE , 1\n"
            "\t.byte\t\tVOL   , 90*t_mvl/mxv\n"
            "\t.byte\t\tPAN   , c_v+20\n"
            "\t.byte\t\tBENDR , 12\n"
            "\t.byte\t\tBEND  , c_v+30\n"
            "\t.byte\t\tTUNE  , c_v+5\n"
            "\t.byte\t\tLFOS  , 40\n"
            "\t.byte\t\tLFODL , 2\n"
            "\t.byte\t\tMODT  , 1\n"
            "\t.byte\t\tMOD   , 24\n"
            "\t.byte\t\tXCMD  , xIECV , 9\n"
            "\t.byte\t\tXCMD  , xIECL , 8\n"
            "\t.byte\t\tPRIO  , 5\n"
            "\t.byte\t\tN24   , Cn3 , v127\n"
            "\t.byte\tW24\n"
            "\t.byte\t\tN24   , Dn3 , v100\n"
            "\t.byte\tW24\n"
            "\t.byte\tFINE\n");
        ASSERT(song != NULL, "timing song parses");
        if (song) {
            song->reverb = 0x80 | 40;
            static M4AEngine e, fresh;
            M4ASequencer seq;
            engine_init_booted(&e, 48000.0f);
            engine_init_booted(&fresh, 48000.0f);
            m4a_engine_set_voicegroup(&e, voices);
            m4a_engine_set_voicegroup(&fresh, voices);
            m4a_sequencer_init_timing(&seq, &e);
            m4a_sequencer_start(&seq, song);
            ASSERT(!e.externalSequencer, "timing-only: the engine keeps its own per-tick work");
            float outL[1024], outR[1024];
            int on = 0, first = 0;
            for (int f = 0; f < 70; f++) {
                lfo_order_frame(&e, outL, outR);
                if (f == 0) first = e.cgbEntryCycles + M4A_CYC_SOUNDMAIN_ENTRY;
                for (int c = 0; c < TOTAL_PCM_CHANNELS; c++) on += (e.pcmChannels[c].status & CHN_ON) != 0;
                for (int c = 0; c < TOTAL_CGB_CHANNELS; c++) on += (e.cgbChannels[c].status & CHN_ON) != 0;
            }
            ASSERT_EQ(on, 0, "timing-only: no engine channel ever starts");
            ASSERT(memcmp(e.tracks, fresh.tracks, sizeof(e.tracks)) == 0, "timing-only: the engine's tracks untouched");
            ASSERT_EQ(e.reverb.amount, 0, "timing-only: the header's reverb not applied");
            ASSERT(first > 3000, "timing-only: the start frame's entry is the song's (start tick, commands, note)");
            m4a_sequencer_detach(&seq);
            ASSERT(e.frameHook == NULL && e.frameEndHook == NULL, "timing-only: detach removes both hooks");
            m4a_engine_destroy(&e);
            m4a_engine_destroy(&fresh);
            m4a_song_free(song);
        }
    }
}

/* The material through both paths, as cgb_entry_pair, compared on the
 * channels instead: after each of frames 3 .. 2 + nFrames, every real PCM
 * and CGB channel's status, right / left volume (ChnVolSetAsm), frequency,
 * envelope and whether it is owned by no track (ply_fine's unlink).  Returns the differing frames; *firstBad the first, *sounding
 * the frames with more than one PCM channel on (a releasing note beside a
 * new one), *seqClean whether the sequencer engine's tracks never took a
 * pending change or the finished mark. */
static int batch_pair(const char *text, const ToneData *voices, const CgbEntryEvent *evs, int nEvs,
                      int tempoI, bool latch, int nFrames, int *firstBad, int *sounding, bool *seqClean)
{
    M4ASong *song = parse_song_text(text);
    if (!song)
        return -1;
    static M4AEngine pair[2];
    M4ASequencer seq;
    for (int k = 0; k < 2; k++) {
        m4a_engine_init(&pair[k], 48000.0f);
        m4a_engine_set_voicegroup(&pair[k], (ToneData *)voices);
        m4a_engine_boot(&pair[k], true);
    }
    m4a_engine_set_quantize_events(&pair[0], latch);
    m4a_sequencer_init(&seq, &pair[1]);
    m4a_sequencer_start(&seq, song);                       /* first tick on frame 3 */
    int s0 = (int)gba_tempo_grid_sample(0, 48000.0);       /* the origin, before frame 3 */
    float *obuf = malloc(sizeof(float) * (size_t)s0 * 2);
    m4a_engine_process(&pair[0], obuf, obuf + s0, s0);
    free(obuf);
    m4a_engine_start_song_tempo(&pair[0]);
    m4a_engine_set_tick_tempo(&pair[0], (uint16_t)tempoI);

    int tickFrame[512], nTicks = 0, acc = 0;
    for (int f = 3; f < 3 + nFrames && nTicks < 512; f++) {
        acc += f == 3 ? 150 : tempoI;
        while (acc >= 150 && nTicks < 512) {
            tickFrame[nTicks++] = f;
            acc -= 150;
        }
    }
    float outL[1024], outR[1024];
    int bad = 0, ev = 0;
    *firstBad = -1;
    *sounding = 0;
    *seqClean = true;
    for (int f = 3; f < 3 + nFrames; f++) {
        /* The renderer's tick boundaries: before an event of the frame's
         * k-th tick, k boundaries in all (slice 7). */
        int firstTick = -1, sent = 0;
        for (int k = 0; k < nTicks && firstTick < 0; k++)
            if (tickFrame[k] == f)
                firstTick = k;
        while (ev < nEvs && evs[ev].tick < nTicks && tickFrame[evs[ev].tick] == f) {
            for (; sent < evs[ev].tick - firstTick; sent++)
                m4a_engine_tick_boundary(&pair[0]);
            cgb_entry_dispatch(&pair[0], &evs[ev++]);
        }
        for (int k = 0; k < 2; k++)
            lfo_order_frame(&pair[k], outL, outR);   /* past frame f's SoundMainRAM */
        bool same = true;
        int on = 0;
        for (int c = 0; c < MAX_PCM_CHANNELS; c++) {
            const M4APCMChannel *a = &pair[0].pcmChannels[c], *b = &pair[1].pcmChannels[c];
            same = same && a->status == b->status && a->rightVolume == b->rightVolume
                && a->leftVolume == b->leftVolume && a->frequency == b->frequency
                && a->envelopeVolume == b->envelopeVolume
                && a->envelopeVolumeRight == b->envelopeVolumeRight
                && a->envelopeVolumeLeft == b->envelopeVolumeLeft
                /* ply_fine's unlink: owned by no track on both (the paths
                 * number their tracks differently) */
                && (a->trackIndex == M4A_NO_TRACK) == (b->trackIndex == M4A_NO_TRACK);
            on += (a->status & CHN_ON) != 0;
        }
        for (int c = 0; c < MAX_CGB_CHANNELS; c++) {
            const M4ACGBChannel *a = &pair[0].cgbChannels[c], *b = &pair[1].cgbChannels[c];
            same = same && a->status == b->status && a->rightVolume == b->rightVolume
                && a->leftVolume == b->leftVolume && a->frequency == b->frequency
                && a->envelopeVolume == b->envelopeVolume
                && (a->trackIndex == M4A_NO_TRACK) == (b->trackIndex == M4A_NO_TRACK);
        }
        for (int t = 0; t < MAX_TRACKS; t++)
            *seqClean = *seqClean && pair[1].tracks[t].chgFlags == 0 && !pair[1].tracks[t].finished;
        *sounding += on > 1;
        if (!same && *firstBad < 0)
            *firstBad = f;
        bad += !same;
    }
    m4a_sequencer_detach(&seq);
    for (int k = 0; k < 2; k++)
        m4a_engine_destroy(&pair[k]);
    m4a_song_free(song);
    return bad;
}

/*
 * Per-tick command batching and ply_fine on the MIDI path (docs/ACCURACY.md, "Rules for every source"): a VOL / PAN / BEND before a note in the same tick reaches only
 * the new note (ply_note drops the track's pending VOLCHG / PITCHG), a
 * releasing note keeps its old value until the next change; a VOL on a tick
 * without a note reaches every channel of the track (the frame's pass); a
 * FINE unlinks the releasing note, so neither the FINE tick's VOL nor the
 * LFO steps after it reach it.  The same material through the MIDI path and
 * a sequencer song leaves every channel the same after every frame, latched
 * and not; the sequencer engine never enters the MIDI path's batching state.
 */
static void test_tick_batching_midi_path(void)
{
    printf("Testing per-tick command batching and ply_fine on the MIDI path...\n");
    char msg[200];
    ToneData voices[128];
    memset(voices, 0, sizeof(voices));
    WaveData *wd = seq_test_wave();
    voices[2].type = VOICE_DIRECTSOUND;
    voices[2].key = 60;
    voices[2].wav = wd;
    voices[2].attack = 0xFF;
    voices[2].sustain = 0xFF;
    voices[2].release = 0xF0;               /* a release long enough to overlap the next note */

    char text[4096];
    snprintf(text, sizeof(text), "%s%s%s", seq_song_header,
             "\t.byte\tKEYSH , t_key+0\n"
             "\t.byte\tTEMPO , 120*t_tbs/2\n"
             "\t.byte\t\tVOICE , 2\n"
             "\t.byte\t\tVOL   , 127*t_mvl/mxv\n"
             "\t.byte\t\tPAN   , c_v+0\n"
             "\t.byte\t\tN12   , Cn3 , v127\n"
             "\t.byte\tW12\n"
             "\t.byte\t\tVOL   , 60*t_mvl/mxv\n"     /* tick 12: VOL, then a note, Cn3 releasing */
             "\t.byte\t\tN12   , Dn3 \n"
             "\t.byte\tW12\n"
             "\t.byte\t\tPAN   , c_v+30\n"           /* tick 24: PAN, then a note */
             "\t.byte\t\tN12   , En3 \n"
             "\t.byte\tW12\n"
             "\t.byte\t\tBEND  , c_v+20\n"           /* tick 36: BEND, then a note */
             "\t.byte\t\tN12   , Fn3 \n"
             "\t.byte\tW12\n"
             "\t.byte\t\tVOL   , 100*t_mvl/mxv\n"    /* tick 48: VOL, no note: every channel */
             "\t.byte\tW12\n"
             "\t.byte\t\tLFOS  , 40\n"               /* tick 60: vibrato on a new note */
             "\t.byte\t\tMOD   , 30\n"
             "\t.byte\t\tN24   , Gn3 \n"
             "\t.byte\tW24\n"
             "\t.byte\t\tVOL   , 127*t_mvl/mxv\n"    /* tick 84: the note's end, VOL, FINE */
             "\t.byte\tFINE\n", seq_song_footer);
    static const CgbEntryEvent evs[] = {
        { 0, 'p', 1, 2, 0 }, { 0, 'c', 1, 7, 127 }, { 0, 'c', 1, 10, 64 }, { 0, 'n', 1, 60, 127 },
        { 12, 'n', 1, 60, 0 }, { 12, 'c', 1, 7, 60 }, { 12, 'n', 1, 62, 127 },
        { 24, 'n', 1, 62, 0 }, { 24, 'c', 1, 10, 94 }, { 24, 'n', 1, 64, 127 },
        { 36, 'n', 1, 64, 0 }, { 36, 'b', 1, 20, 0 }, { 36, 'n', 1, 65, 127 },
        { 48, 'n', 1, 65, 0 }, { 48, 'c', 1, 7, 100 },
        { 60, 'c', 1, 0x15, 40 }, { 60, 'c', 1, 1, 30 }, { 60, 'n', 1, 67, 127 },
        { 84, 'n', 1, 67, 0 }, { 84, 'c', 1, 7, 127 }, { 84, 'e', 1, 0, 0 },
    };
    const int nEvs = (int)(sizeof(evs) / sizeof(evs[0]));
    for (int q = 0; q < 2; q++) {
        int firstBad, sounding;
        bool seqClean;
        /* tick 84 is frame 3 + 84 / 0.8 = 108; 80 frames of release after it */
        int bad = batch_pair(text, voices, evs, nEvs, 120, q == 1, 190, &firstBad, &sounding, &seqClean);
        snprintf(msg, sizeof(msg), "batching%s: the MIDI path's channels are the sequencer's after every frame"
                 " 3 .. 192 (%d differ, first %d; %d frames with two notes sounding)",
                 q ? " latched" : "", bad, firstBad, sounding);
        ASSERT(bad == 0 && sounding > 40, msg);
        snprintf(msg, sizeof(msg), "batching%s: the sequencer engine's tracks never take a pending change",
                 q ? " latched" : "");
        ASSERT(seqClean, msg);
    }

    /* Each case alone, latched and not: a note (Cn3, N12), then on tick 12,
     * while it releases, the case's commands.  A..C and E..F fail on the old
     * rule (every controller on every channel at once, no FINE); D holds on
     * both and guards the pass reaching every channel. */
    static const struct {
        const char *what, *pre, *cmds;
        CgbEntryEvent ev[4];
        int nEv;
    } cases[] = {
        { "A: a VOL before a note keeps the releasing note's volume", "",
          "\t.byte\t\tVOL   , 60*t_mvl/mxv\n\t.byte\t\tN12   , Dn3 \n\t.byte\tW12\n\t.byte\tW48\n\t.byte\tFINE\n",
          { { 12, 'c', 1, 7, 60 }, { 12, 'n', 1, 62, 127 }, { 24, 'n', 1, 62, 0 }, { 72, 'e', 1, 0, 0 } }, 4 },
        { "B: a PAN before a note keeps the releasing note's pan", "",
          "\t.byte\t\tPAN   , c_v+30\n\t.byte\t\tN12   , Dn3 \n\t.byte\tW12\n\t.byte\tW48\n\t.byte\tFINE\n",
          { { 12, 'c', 1, 10, 94 }, { 12, 'n', 1, 62, 127 }, { 24, 'n', 1, 62, 0 }, { 72, 'e', 1, 0, 0 } }, 4 },
        { "C: a BEND before a note keeps the releasing note's pitch", "",
          "\t.byte\t\tBEND  , c_v+20\n\t.byte\t\tN12   , Dn3 \n\t.byte\tW12\n\t.byte\tW48\n\t.byte\tFINE\n",
          { { 12, 'b', 1, 20, 0 }, { 12, 'n', 1, 62, 127 }, { 24, 'n', 1, 62, 0 }, { 72, 'e', 1, 0, 0 } }, 4 },
        { "D: a VOL on a tick without a note reaches the releasing note", "",
          "\t.byte\t\tVOL   , 60*t_mvl/mxv\n\t.byte\tW48\n\t.byte\tFINE\n",
          { { 12, 'c', 1, 7, 60 }, { 60, 'e', 1, 0, 0 } }, 2 },
        { "E: the FINE tick's VOL does not reach the releasing note", "",
          "\t.byte\t\tVOL   , 60*t_mvl/mxv\n\t.byte\tFINE\n",
          { { 12, 'c', 1, 7, 60 }, { 12, 'e', 1, 0, 0 } }, 2 },
        { "F: no LFO step reaches the releasing note after FINE",
          "\t.byte\t\tLFOS  , 40\n\t.byte\t\tMOD   , 30\n",
          "\t.byte\tFINE\n",
          { { 12, 'e', 1, 0, 0 } }, 1 },
    };
    for (int c = 0; c < (int)(sizeof(cases) / sizeof(cases[0])); c++) {
        char one[2048];
        snprintf(one, sizeof(one),
                 "%s\t.byte\tKEYSH , t_key+0\n"
                 "\t.byte\tTEMPO , 120*t_tbs/2\n"
                 "\t.byte\t\tVOICE , 2\n"
                 "\t.byte\t\tVOL   , 127*t_mvl/mxv\n"
                 "\t.byte\t\tPAN   , c_v+0\n"
                 "%s"
                 "\t.byte\t\tN12   , Cn3 , v127\n"
                 "\t.byte\tW12\n"
                 "%s%s", seq_song_header, cases[c].pre, cases[c].cmds, seq_song_footer);
        CgbEntryEvent ev[12];
        int n = 0;
        ev[n++] = (CgbEntryEvent){ 0, 'p', 1, 2, 0 };
        ev[n++] = (CgbEntryEvent){ 0, 'c', 1, 7, 127 };
        ev[n++] = (CgbEntryEvent){ 0, 'c', 1, 10, 64 };
        if (cases[c].pre[0]) {
            ev[n++] = (CgbEntryEvent){ 0, 'c', 1, 0x15, 40 };
            ev[n++] = (CgbEntryEvent){ 0, 'c', 1, 1, 30 };
        }
        ev[n++] = (CgbEntryEvent){ 0, 'n', 1, 60, 127 };
        ev[n++] = (CgbEntryEvent){ 12, 'n', 1, 60, 0 };
        for (int k = 0; k < cases[c].nEv; k++)
            ev[n++] = cases[c].ev[k];
        for (int q = 0; q < 2; q++) {
            int firstBad, sounding;
            bool seqClean;
            int bad = batch_pair(one, voices, ev, n, 120, q == 1, 100, &firstBad, &sounding, &seqClean);
            snprintf(msg, sizeof(msg), "%s%s (%d frames differ, first %d)", cases[c].what,
                     q ? ", latched" : "", bad, firstBad);
            ASSERT(bad == 0 && seqClean, msg);
        }
    }
    free(wd);
}

/*
 * The two-tick-frame LFO interleave on the MIDI path (docs/ACCURACY.md, the renderer's "as the game" mode): in a frame with two song ticks MPlayMain runs tick n's commands
 * and each track's LFO step before tick n + 1's commands.  With the
 * renderer's tick boundaries (m4a_engine_tick_boundary, batch_pair) the same
 * material through the MIDI path and a sequencer song leaves every channel
 * the same after every frame at 240 (two ticks on three frames in five),
 * latched and not: a MOD from 0 and an LFOS change in a second tick, a note
 * restarting an LFO delay there, notes there on a vibrato track while an
 * older note sounds.  A sequencer-driven engine ignores the boundary; the
 * grid walker gives one-tick frames no boundary.
 */
static void test_two_tick_interleave_midi_path(void)
{
    printf("Testing the two-tick-frame LFO interleave on the MIDI path...\n");
    char msg[200];
    ToneData voices[128];
    memset(voices, 0, sizeof(voices));
    WaveData *wd = seq_test_wave();
    voices[2].type = VOICE_DIRECTSOUND;
    voices[2].key = 60;
    voices[2].wav = wd;
    voices[2].attack = 0xFF;
    voices[2].sustain = 0xFF;
    voices[2].release = 0xF0;

    /* The grid walker: at 240 ticks 0 .. 11 on frames 0 1 2 2 3 4 4 5 5 6 7
     * 7, so ticks 3, 6, 8 and 11 are second ticks; at 120 no frame has two. */
    {
        static const uint32_t want240[12] = { 0, 0, 0, 1, 0, 0, 1, 0, 1, 0, 0, 1 };
        GbaTempoGrid g;
        gba_tempo_grid_init(&g, 24, 24);
        bool ok = true;
        for (uint64_t c = 0; c < 12; c++) {
            (void)gba_tempo_grid_frame(&g, c);
            ok = ok && gba_tempo_grid_tick_in_frame(&g, c) == want240[c];
            if (c == 0)
                gba_tempo_grid_set_tempo(&g, 250000);   /* 240 BPM from the next frame */
        }
        ASSERT(ok, "grid walker at 240: second ticks 3, 6, 8, 11 get index 1, the others 0");
        gba_tempo_grid_init(&g, 24, 24);
        ok = true;
        for (uint64_t c = 0; c < 400; c++) {
            (void)gba_tempo_grid_frame(&g, c);
            ok = ok && gba_tempo_grid_tick_in_frame(&g, c) == 0;
            if (c == 0)
                gba_tempo_grid_set_tempo(&g, 500000);   /* 120 BPM */
        }
        ASSERT(ok, "grid walker at 120: every tick first in its frame (no boundary)");
    }

    /* Each case: KEYSH, TEMPO 240, VOICE 2, VOL, PAN, the case's setup at
     * tick 0, an N96 Cn3, then from tick 24 the case's commands (A .. C at
     * second tick 24, frame 18 with tick 23; D at 43, 72, 78), the note's
     * end and FINE at tick 96; 120 frames.  Every case fails on an engine
     * whose boundary does nothing (all of a frame's events before its
     * steps): A .. C from their frame on, D on 3 frames. */
    static const struct {
        const char *what, *pre, *at24;
        CgbEntryEvent pre_ev[3], ev[10];
        int nPre, nEv;
    } cases[] = {
        { "A: a MOD from 0 in the second tick takes one step that frame",
          "\t.byte\t\tLFOS  , 40\n",
          "\t.byte\t\tMOD   , 30\n\t.byte\tW72\n\t.byte\tFINE\n",
          { { 0, 'c', 1, 0x15, 40 } }, { { 24, 'c', 1, 1, 30 }, { 96, 'n', 1, 60, 0 }, { 96, 'e', 1, 0, 0 } }, 1, 3 },
        { "B: an LFOS change in the second tick after the first tick's step",
          "\t.byte\t\tLFOS  , 40\n\t.byte\t\tMOD   , 30\n",
          "\t.byte\t\tLFOS  , 20\n\t.byte\tW72\n\t.byte\tFINE\n",
          { { 0, 'c', 1, 0x15, 40 }, { 0, 'c', 1, 1, 30 } },
          { { 24, 'c', 1, 0x15, 20 }, { 96, 'n', 1, 60, 0 }, { 96, 'e', 1, 0, 0 } }, 2, 3 },
        { "C: an LFO delay restarted in the second tick counts once that frame",
          "\t.byte\t\tLFOS  , 40\n\t.byte\t\tLFODL , 3\n\t.byte\t\tMOD   , 30\n",
          "\t.byte\t\tN24   , Dn3 , v127\n\t.byte\tW24\n\t.byte\tW48\n\t.byte\tFINE\n",
          { { 0, 'c', 1, 0x15, 40 }, { 0, 'c', 1, 0x1A, 3 }, { 0, 'c', 1, 1, 30 } },
          { { 24, 'n', 1, 62, 127 }, { 48, 'n', 1, 62, 0 }, { 96, 'n', 1, 60, 0 }, { 96, 'e', 1, 0, 0 } }, 3, 4 },
        { "D: notes in second ticks on a vibrato track while an older note sounds",
          "\t.byte\t\tLFOS  , 22\n\t.byte\t\tMOD   , 4\n",
          /* ticks 43, 72, 78: second ticks whose step leaves tick n's modM
           * unchanged, so the dropped flag keeps the older note's pitch */
          "\t.byte\tW19\n\t.byte\t\tN03   , Dn3 , v127\n\t.byte\tW03\n"   /* tick 43 */
          "\t.byte\tW24\n\t.byte\tW02\n\t.byte\t\tN03   , En3 \n\t.byte\tW03\n"   /* tick 72 */
          "\t.byte\tW03\n\t.byte\t\tN03   , Fn3 \n\t.byte\tW03\n"   /* tick 78 */
          "\t.byte\tW15\n\t.byte\tFINE\n",
          { { 0, 'c', 1, 0x15, 22 }, { 0, 'c', 1, 1, 4 } },
          { { 43, 'n', 1, 62, 127 }, { 46, 'n', 1, 62, 0 }, { 72, 'n', 1, 64, 127 }, { 75, 'n', 1, 64, 0 },
            { 78, 'n', 1, 65, 127 }, { 81, 'n', 1, 65, 0 }, { 96, 'n', 1, 60, 0 }, { 96, 'e', 1, 0, 0 } }, 2, 8 },
    };
    for (int c = 0; c < (int)(sizeof(cases) / sizeof(cases[0])); c++) {
        char one[2048];
        snprintf(one, sizeof(one),
                 "%s\t.byte\tKEYSH , t_key+0\n"
                 "\t.byte\tTEMPO , 240*t_tbs/2\n"
                 "\t.byte\t\tVOICE , 2\n"
                 "\t.byte\t\tVOL   , 127*t_mvl/mxv\n"
                 "\t.byte\t\tPAN   , c_v+0\n"
                 "%s"
                 "\t.byte\t\tN96   , Cn3 , v127\n"
                 "\t.byte\tW24\n"
                 "%s%s", seq_song_header, cases[c].pre, cases[c].at24, seq_song_footer);
        CgbEntryEvent ev[16];
        int n = 0;
        ev[n++] = (CgbEntryEvent){ 0, 'p', 1, 2, 0 };
        ev[n++] = (CgbEntryEvent){ 0, 'c', 1, 7, 127 };
        ev[n++] = (CgbEntryEvent){ 0, 'c', 1, 10, 64 };
        for (int k = 0; k < cases[c].nPre; k++)
            ev[n++] = cases[c].pre_ev[k];
        ev[n++] = (CgbEntryEvent){ 0, 'n', 1, 60, 127 };
        for (int k = 0; k < cases[c].nEv; k++)
            ev[n++] = cases[c].ev[k];
        /* the events in tick order (D's note-offs interleave with its notes) */
        for (int i = 1; i < n; i++)
            for (int j = i; j > 0 && ev[j - 1].tick > ev[j].tick; j--) {
                CgbEntryEvent tmp = ev[j]; ev[j] = ev[j - 1]; ev[j - 1] = tmp;
            }
        for (int q = 0; q < 2; q++) {
            int firstBad, sounding;
            bool seqClean;
            int bad = batch_pair(one, voices, ev, n, 240, q == 1, 120, &firstBad, &sounding, &seqClean);
            snprintf(msg, sizeof(msg), "%s%s (%d frames differ, first %d)", cases[c].what,
                     q ? ", latched" : "", bad, firstBad);
            ASSERT(bad == 0 && seqClean, msg);
        }
    }

    /* E: two tracks, events of both in both ticks of a frame (the ROM runs
     * each track's tick, LFO step included, before the next track's; the
     * engine every track's tick-n events, then every step): track 0's MOD
     * from 0 at second tick 24 after track 1's note at tick 23; track 1's
     * note at second tick 43 after track 0's at 42.  F: a frame of three
     * ticks (TEMPO 320, ticks 30 .. 32 on frame 18), a MOD from 0 in the
     * third: two boundaries before it. */
    {
        static const char *two =
            "\t.byte\tKEYSH , t_key+0\n\t.byte\tTEMPO , 240*t_tbs/2\n\t.byte\t\tVOICE , 2\n"
            "\t.byte\t\tVOL   , 127*t_mvl/mxv\n\t.byte\t\tPAN   , c_v+0\n\t.byte\t\tLFOS  , 40\n"
            "\t.byte\t\tN96   , Cn3 , v127\n\t.byte\tW24\n"
            "\t.byte\t\tMOD   , 30\n\t.byte\tW18\n"                         /* tick 24 */
            "\t.byte\t\tN03   , Dn3 \n\t.byte\tW03\n"                       /* tick 42 */
            "\t.byte\tW48\n\t.byte\tW03\n\t.byte\tFINE\n"
            "t_2:\n\t.byte\t\tVOICE , 2\n\t.byte\t\tVOL   , 127*t_mvl/mxv\n\t.byte\t\tPAN   , c_v+0\n"
            "\t.byte\t\tLFOS  , 22\n\t.byte\t\tMOD   , 4\n\t.byte\t\tN96   , En3 , v127\n\t.byte\tW23\n"
            "\t.byte\t\tN03   , Fn3 \n\t.byte\tW03\n"                       /* tick 23 */
            "\t.byte\tW17\n\t.byte\t\tN03   , Gn3 \n\t.byte\tW03\n"        /* tick 43 */
            "\t.byte\tW48\n\t.byte\tW02\n\t.byte\tFINE\n"
            "\t.align\t2\nt:\n\t.byte\t2, 0, t_pri, t_rev\n\t.word\tt_grp\n\t.word\tt_1\n\t.word\tt_2\n";
        static const CgbEntryEvent evTwo[] = {
            { 0, 'p', 1, 2, 0 }, { 0, 'c', 1, 7, 127 }, { 0, 'c', 1, 10, 64 }, { 0, 'c', 1, 0x15, 40 },
            { 0, 'n', 1, 60, 127 },
            { 0, 'p', 2, 2, 0 }, { 0, 'c', 2, 7, 127 }, { 0, 'c', 2, 10, 64 }, { 0, 'c', 2, 0x15, 22 },
            { 0, 'c', 2, 1, 4 }, { 0, 'n', 2, 64, 127 },
            { 23, 'n', 2, 65, 127 }, { 24, 'c', 1, 1, 30 }, { 26, 'n', 2, 65, 0 },
            { 42, 'n', 1, 62, 127 }, { 43, 'n', 2, 67, 127 }, { 45, 'n', 1, 62, 0 }, { 46, 'n', 2, 67, 0 },
            { 96, 'n', 1, 60, 0 }, { 96, 'e', 1, 0, 0 }, { 96, 'n', 2, 64, 0 }, { 96, 'e', 2, 0, 0 },
        };
        char three[2048];
        snprintf(three, sizeof(three),
                 "%s\t.byte\tKEYSH , t_key+0\n\t.byte\tTEMPO , 320*t_tbs/2\n\t.byte\t\tVOICE , 2\n"
                 "\t.byte\t\tVOL   , 127*t_mvl/mxv\n\t.byte\t\tPAN   , c_v+0\n\t.byte\t\tLFOS  , 40\n"
                 "\t.byte\t\tN96   , Cn3 , v127\n\t.byte\tW24\n\t.byte\tW08\n"
                 "\t.byte\t\tMOD   , 30\n\t.byte\tW64\n\t.byte\tFINE\n%s",   /* tick 32 */
                 seq_song_header, seq_song_footer);
        static const CgbEntryEvent evThree[] = {
            { 0, 'p', 1, 2, 0 }, { 0, 'c', 1, 7, 127 }, { 0, 'c', 1, 10, 64 }, { 0, 'c', 1, 0x15, 40 },
            { 0, 'n', 1, 60, 127 }, { 32, 'c', 1, 1, 30 }, { 96, 'n', 1, 60, 0 }, { 96, 'e', 1, 0, 0 },
        };
        char twoText[4096];
        snprintf(twoText, sizeof(twoText), "%s%s", seq_song_header, two);
        for (int q = 0; q < 2; q++) {
            int firstBad, sounding;
            bool seqClean;
            int bad = batch_pair(twoText, voices, evTwo, (int)(sizeof(evTwo) / sizeof(evTwo[0])), 240, q == 1,
                                 120, &firstBad, &sounding, &seqClean);
            snprintf(msg, sizeof(msg), "E: two tracks' events in both ticks of a frame%s (%d frames differ, first %d)",
                     q ? ", latched" : "", bad, firstBad);
            ASSERT(bad == 0 && seqClean, msg);
            bad = batch_pair(three, voices, evThree, (int)(sizeof(evThree) / sizeof(evThree[0])), 320, q == 1,
                             100, &firstBad, &sounding, &seqClean);
            snprintf(msg, sizeof(msg), "F: a MOD from 0 in the third tick of a three-tick frame%s (%d frames differ, first %d)",
                     q ? ", latched" : "", bad, firstBad);
            ASSERT(bad == 0 && seqClean, msg);
        }
    }

    /* A sequencer-driven engine ignores the boundary, latched or not. */
    {
        char one[2048];
        snprintf(one, sizeof(one),
                 "%s\t.byte\tKEYSH , t_key+0\n\t.byte\tTEMPO , 240*t_tbs/2\n\t.byte\t\tVOICE , 2\n"
                 "\t.byte\t\tLFOS  , 40\n\t.byte\t\tMOD   , 30\n\t.byte\t\tN96   , Cn3 , v127\n"
                 "\t.byte\tW96\n\t.byte\tFINE\n%s", seq_song_header, seq_song_footer);
        M4ASong *song = parse_song_text(one);
        ASSERT(song != NULL, "sequencer song parses");
        if (song) {
            static M4AEngine e, ref;
            M4ASequencer seq, seqRef;
            engine_init_booted(&e, 48000.0f);
            engine_init_booted(&ref, 48000.0f);
            m4a_engine_set_voicegroup(&e, voices);
            m4a_engine_set_voicegroup(&ref, voices);
            m4a_sequencer_init(&seq, &e);
            m4a_sequencer_init(&seqRef, &ref);
            m4a_sequencer_start(&seq, song);
            m4a_sequencer_start(&seqRef, song);
            float outL[1024], outR[1024];
            bool same = true;
            for (int f = 0; f < 40; f++) {
                m4a_engine_set_quantize_events(&e, f >= 20);
                m4a_engine_tick_boundary(&e);
                m4a_engine_tick_boundary(&e);
                same = same && e.lfoTicksAhead == 0 && e.eventQueueCount == 0;
                lfo_order_frame(&e, outL, outR);
                lfo_order_frame(&ref, outL, outR);
                same = same && memcmp(e.tracks, ref.tracks, sizeof(e.tracks)) == 0
                     && memcmp(e.pcmChannels, ref.pcmChannels, sizeof(e.pcmChannels)) == 0;
            }
            ASSERT(same, "a sequencer-driven engine ignores the tick boundary (tracks and channels as without it)");
            m4a_sequencer_detach(&seq);
            m4a_sequencer_detach(&seqRef);
            m4a_engine_destroy(&e);
            m4a_engine_destroy(&ref);
            m4a_song_free(song);
        }
    }
    free(wd);
}

static void test_event_latch(void)
{
    printf("Testing the MIDI event latch (quantizeEvents)...\n");

    WaveData *wd = seq_test_wave();
    ToneData voices[128];
    memset(voices, 0, sizeof(voices));
    voices[0].type = VOICE_DIRECTSOUND;
    voices[0].key = 60;
    voices[0].wav = wd;
    voices[0].attack = 0xFF;
    voices[0].sustain = 0xFF;
    voices[0].release = 0x80;
    voices[1] = voices[0];
    voices[1].attack = 0x7F;            /* tells a program-1 channel from a program-0 one */
    voices[2].type = VOICE_SQUARE_1;    /* a CGB voice for the PSG side of the same rules */
    voices[2].key = 60;
    voices[2].attack = 0;
    voices[2].decay = 0;
    voices[2].sustain = 15;
    voices[2].release = 0;
    voices[2].wavePointer = (void *)(uintptr_t)2;

    M4AEngine engine;
    m4a_engine_init(&engine, 48000.0f);
    m4a_engine_set_voicegroup(&engine, voices);
    ASSERT(!engine.quantizeEvents, "latch off by default");
    m4a_engine_program_change(&engine, 0, 0);
    m4a_engine_cc(&engine, 0, 7, 127);

    /* --- off: a note-on applies at once --- */
    int h = m4a_engine_note_on(&engine, 0, 60, 127);
    ASSERT(h >= 0 && h < TOTAL_PCM_CHANNELS, "latch off: note starts at once");
    if (h < 0) { m4a_engine_destroy(&engine); free(wd); return; }
    ASSERT(engine.pcmChannels[h].status & CHN_START, "latch off: channel START at once");
    m4a_engine_note_off(&engine, 0, 60);
    ASSERT_EQ(engine.pcmChannels[h].status & (CHN_START | CHN_STOP), CHN_START | CHN_STOP,
              "latch off: a note-off in the same frame leaves START | STOP (SoundMainRAM kills it)");
    engine.pcmChannels[h].status = 0;

    /* --- on: a note-on mid-frame waits for the tick --- */
    m4a_engine_set_quantize_events(&engine, true);
    h = m4a_engine_note_on(&engine, 0, 60, 127);
    ASSERT_EQ(h, M4A_CHANNEL_QUEUED, "latch on: note-on reports queued");
    ASSERT_EQ(engine.eventQueueCount, 1, "latch on: one event queued");
    int on = 0;
    for (int i = 0; i < TOTAL_PCM_CHANNELS; i++)
        on += (engine.pcmChannels[i].status & CHN_ON) != 0;
    ASSERT_EQ(on, 0, "latch on: no channel started before the tick");
    m4a_engine_tick(&engine);
    ASSERT_EQ(engine.eventQueueCount, 0, "tick drained the queue");
    ASSERT(!engine.inTick, "inTick cleared after the tick");
    M4APCMChannel *ch = NULL;
    for (int i = 0; i < TOTAL_PCM_CHANNELS; i++)
        if (engine.pcmChannels[i].status & CHN_ON) { ch = &engine.pcmChannels[i]; break; }
    ASSERT(ch != NULL, "latch on: the note started at the tick");
    if (!ch) { m4a_engine_destroy(&engine); free(wd); return; }
    ASSERT(ch->status & CHN_START, "latch on: channel START at the tick");
    ASSERT_EQ(ch->midiKey, 60, "latch on: the queued key");
    ASSERT_EQ(ch->gateTime, 0, "latch on: no gate (the note-off is explicit)");

    /* --- a note-off a frame later stops it as before --- */
    m4a_engine_note_off(&engine, 0, 60);
    ASSERT(!(ch->status & CHN_STOP), "latched note-off not applied before the tick");
    ch->status = CHN_ENV_SUSTAIN;      /* the mixer's envelope step would have run by now */
    m4a_engine_tick(&engine);
    ASSERT(ch->status & CHN_STOP, "latched note-off applied at the tick");
    ch->status = 0;

    /* --- order preserved: program, note, program, note --- */
    m4a_engine_program_change(&engine, 0, 0);
    m4a_engine_note_on(&engine, 0, 60, 127);
    m4a_engine_program_change(&engine, 0, 1);
    m4a_engine_note_on(&engine, 0, 62, 127);
    ASSERT_EQ(engine.eventQueueCount, 4, "four events queued");
    ASSERT_EQ(engine.tracks[0].currentProgram, 0, "program change not applied before the tick");
    m4a_engine_tick(&engine);
    M4APCMChannel *c60 = NULL, *c62 = NULL;
    for (int i = 0; i < TOTAL_PCM_CHANNELS; i++) {
        M4APCMChannel *c = &engine.pcmChannels[i];
        if (!(c->status & CHN_ON)) continue;
        if (c->midiKey == 60) c60 = c;
        if (c->midiKey == 62) c62 = c;
    }
    ASSERT(c60 && c62, "both latched notes started at the tick");
    if (c60 && c62) {
        ASSERT_EQ(c60->attack, 0xFF, "order: key 60 took program 0 (queued before the program change)");
        ASSERT_EQ(c62->attack, 0x7F, "order: key 62 took program 1 (queued after it)");
        ASSERT_EQ(engine.tracks[0].currentProgram, 1, "the last program change stands");
        c60->status = 0;
        c62->status = 0;
    }

    /* --- note-off in the same frame as its note-on: the GBA's one-tick note --- */
    m4a_engine_program_change(&engine, 0, 0);
    m4a_engine_note_on(&engine, 0, 64, 127);
    m4a_engine_note_off(&engine, 0, 64);
    m4a_engine_tick(&engine);
    ch = NULL;
    for (int i = 0; i < TOTAL_PCM_CHANNELS; i++)
        if ((engine.pcmChannels[i].status & CHN_ON) && engine.pcmChannels[i].midiKey == 64)
            ch = &engine.pcmChannels[i];
    ASSERT(ch != NULL, "one-tick note: the channel started");
    if (ch) {
        ASSERT_EQ(ch->status & (CHN_START | CHN_STOP), CHN_START, "one-tick note: START without STOP at its tick");
        ASSERT_EQ(ch->gateTime, 1, "one-tick note: gate 1, as mid2agb's one-clock minimum");
        ch->status = CHN_ENV_SUSTAIN;  /* the mixer's envelope step: the frame sounds */
        m4a_engine_tick(&engine);
        ASSERT(ch->status & CHN_STOP, "one-tick note: the next tick's gate walk stops it");
        ASSERT_EQ(ch->gateTime, 0, "one-tick note: gate expired");
        ch->status = 0;
    }

    /* --- the same on a CGB channel --- */
    m4a_engine_program_change(&engine, 0, 2);
    m4a_engine_note_on(&engine, 0, 60, 127);
    m4a_engine_note_off(&engine, 0, 60);
    M4ACGBChannel *sq = &engine.cgbChannels[0];
    ASSERT_EQ(sq->status, 0, "CGB: nothing before the tick");
    m4a_engine_tick(&engine);
    /* The tick's CgbSound pass ran after the drain: the start block took the
     * channel from START into the attack (not killed, as START | STOP would
     * have been), and the gate is still armed for the next walk. */
    ASSERT(sq->status & CHN_ON, "CGB one-tick note: the same tick's CgbSound started the note");
    ASSERT(!(sq->status & (CHN_START | CHN_STOP)), "CGB one-tick note: attack, no STOP at its tick");
    ASSERT_EQ(sq->gateTime, 1, "CGB one-tick note: gate 1");
    m4a_engine_tick(&engine);
    /* The gate walk set STOP before this tick's CgbSound pass, which took
     * the channel into its release (release 0: straight off). */
    ASSERT_EQ(sq->gateTime, 0, "CGB one-tick note: gate expired on the next tick");
    ASSERT(!(sq->status & CHN_ON) || (sq->status & CHN_ENV_MASK) == CHN_ENV_RELEASE,
           "CGB one-tick note: released by the next tick's gate walk");
    for (int i = 0; i < 8; i++)
        m4a_engine_tick(&engine);

    /* --- CC and bend latched the same way --- */
    m4a_engine_program_change(&engine, 0, 0);
    m4a_engine_note_on(&engine, 0, 60, 127);
    m4a_engine_tick(&engine);
    ch = NULL;
    for (int i = 0; i < TOTAL_PCM_CHANNELS; i++)
        if ((engine.pcmChannels[i].status & CHN_ON) && engine.pcmChannels[i].midiKey == 60)
            ch = &engine.pcmChannels[i];
    ASSERT(ch != NULL, "CC test note started");
    uint8_t rawVol = engine.tracks[0].rawVolume;
    m4a_engine_cc(&engine, 0, 7, 40);
    ASSERT_EQ(engine.tracks[0].rawVolume, rawVol, "CC 7 not applied before the tick");
    m4a_engine_pitch_bend(&engine, 0, 4096);
    ASSERT_EQ(engine.tracks[0].bend, 0, "bend not applied before the tick");
    m4a_engine_cc(&engine, 0, 0x14, 12);
    ASSERT_EQ(engine.tracks[0].bendRange, 2, "BENDR not applied before the tick");
    m4a_engine_xcmd(&engine, 0, M4A_XCMD_IECV, 9);
    ASSERT_EQ(engine.tracks[0].pseudoEchoVolume, 0, "XCMD not applied before the tick");
    ASSERT_EQ(engine.eventQueueCount, 4, "CC, bend, BENDR, XCMD queued");
    uint32_t f0 = ch ? ch->frequency : 0;
    m4a_engine_tick(&engine);
    ASSERT_EQ(engine.tracks[0].rawVolume, 40, "CC 7 applied at the tick");
    ASSERT_EQ(engine.tracks[0].bend, 32, "bend applied at the tick (4096 >> 7)");
    ASSERT_EQ(engine.tracks[0].bendRange, 12, "BENDR applied at the tick");
    ASSERT_EQ(engine.tracks[0].pseudoEchoVolume, 9, "XCMD applied at the tick");
    if (ch)
        ASSERT(ch->frequency != f0, "the bend reached the sounding channel at the tick");
    if (ch) ch->status = 0;

    /* --- a panic drops what the latch holds --- */
    m4a_engine_note_on(&engine, 0, 60, 127);
    m4a_engine_note_on(&engine, 0, 62, 127);
    ASSERT_EQ(engine.eventQueueCount, 2, "two note-ons queued");
    m4a_engine_all_sound_off(&engine);
    ASSERT_EQ(engine.eventQueueCount, 0, "all sound off drops the queued events");
    m4a_engine_tick(&engine);
    on = 0;
    for (int i = 0; i < TOTAL_PCM_CHANNELS; i++)
        on += (engine.pcmChannels[i].status & CHN_ON) != 0;
    ASSERT_EQ(on, 0, "nothing starts after the panic");

    /* --- turning the latch off applies the queue at once, in order --- */
    m4a_engine_cc(&engine, 0, 7, 100);
    m4a_engine_cc(&engine, 0, 7, 90);
    ASSERT_EQ(engine.tracks[0].rawVolume, 40, "still latched");
    m4a_engine_set_quantize_events(&engine, false);
    ASSERT_EQ(engine.tracks[0].rawVolume, 90, "disabling the latch flushed both CCs in order");
    ASSERT_EQ(engine.eventQueueCount, 0, "queue empty after the flush");
    m4a_engine_cc(&engine, 0, 7, 127);
    ASSERT_EQ(engine.tracks[0].rawVolume, 127, "latch off: CC applies at once again");

    /* --- a full queue is applied in order before the next event queues:
     * a note-off that overflows still follows its own queued note-on --- */
    m4a_engine_set_quantize_events(&engine, true);
    m4a_engine_note_on(&engine, 0, 65, 127);
    for (int i = 1; i < M4A_EVENT_QUEUE_CAPACITY; i++)
        m4a_engine_cc(&engine, 0, 10, 64);
    ASSERT_EQ(engine.eventQueueCount, M4A_EVENT_QUEUE_CAPACITY, "queue full");
    ASSERT_EQ(engine.eventQueueOverflow, 0, "no overflow yet");
    m4a_engine_note_off(&engine, 0, 65);
    ASSERT_EQ(engine.eventQueueOverflow, 1, "overflow counted");
    ASSERT_EQ(engine.eventQueueCount, 1, "overflow: the full queue applied, the note-off queued after it");
    ch = NULL;
    for (int i = 0; i < TOTAL_PCM_CHANNELS; i++)
        if ((engine.pcmChannels[i].status & CHN_ON) && engine.pcmChannels[i].midiKey == 65)
            ch = &engine.pcmChannels[i];
    ASSERT(ch != NULL && !(ch->status & CHN_STOP), "overflow: the note-on applied before its note-off");
    m4a_engine_tick(&engine);
    ASSERT_EQ(engine.eventQueueCount, 0, "the queue drained");
    ASSERT(ch != NULL && ((ch->status & CHN_STOP) || ch->gateTime == 1),
           "overflow: the note-off reached its note (not stuck)");
    if (ch) ch->status = 0;

    /* --- the frame hook (the sequencer) bypasses the latch --- */
    engine.frameHook = latch_hook;
    latch_hook_started = -1;
    m4a_engine_tick(&engine);
    engine.frameHook = NULL;
    ASSERT(latch_hook_started >= 0, "a note-on from inside the tick applies at once with the latch on");
    ASSERT_EQ(engine.eventQueueCount, 0, "and queues nothing");

    m4a_engine_destroy(&engine);
    free(wd);
}

/* The PCM channel sounding (track, key), or NULL. */
static M4APCMChannel *find_pcm_note(M4AEngine *e, int track, int key)
{
    for (int i = 0; i < TOTAL_PCM_CHANNELS; i++) {
        M4APCMChannel *c = &e->pcmChannels[i];
        if ((c->status & CHN_ON) && c->trackIndex == track && c->midiKey == key)
            return c;
    }
    return NULL;
}

static void test_note_choke(void)
{
    printf("Testing NOTE_CHOKE (m4a_engine_note_choke)...\n");

    WaveData *wd = seq_test_wave();
    ToneData voices[128];
    memset(voices, 0, sizeof(voices));
    voices[0].type = VOICE_DIRECTSOUND;
    voices[0].key = 60;
    voices[0].wav = wd;
    voices[0].attack = 0xFF;
    voices[0].sustain = 0xFF;
    voices[0].release = 0x80;
    voices[2].type = VOICE_SQUARE_1;
    voices[2].key = 60;
    voices[2].sustain = 15;
    voices[2].wavePointer = (void *)(uintptr_t)2;

    M4AEngine engine;
    m4a_engine_init(&engine, 48000.0f);
    m4a_engine_set_voicegroup(&engine, voices);
    for (int t = 0; t < 2; t++) {
        m4a_engine_program_change(&engine, t, 0);
        m4a_engine_cc(&engine, t, 7, 127);
    }

    /* --- latch off: a cut at once, where a note-off releases --- */
    m4a_engine_note_on(&engine, 0, 60, 127);
    m4a_engine_note_on(&engine, 0, 64, 127);
    m4a_engine_note_on(&engine, 1, 60, 127);
    m4a_engine_tick(&engine);
    M4APCMChannel *a = find_pcm_note(&engine, 0, 60);
    M4APCMChannel *b = find_pcm_note(&engine, 0, 64);
    M4APCMChannel *c = find_pcm_note(&engine, 1, 60);
    ASSERT(a && b && c, "choke off: three notes sounding");
    if (!a || !b || !c) { m4a_engine_destroy(&engine); free(wd); return; }
    m4a_engine_note_choke(&engine, 0, 60);
    ASSERT_EQ(a->status, 0, "choke off: the key's channel cut at once (status 0, no release)");
    ASSERT(b->status & CHN_ON, "choke off: another key on the track untouched");
    ASSERT(!(b->status & CHN_STOP), "choke off: another key not released");
    ASSERT(c->status & CHN_ON, "choke off: the same key on another track untouched");
    m4a_engine_note_off(&engine, 0, 64);
    ASSERT(b->status & CHN_STOP, "choke off: note_off still releases (STOP, not a cut)");
    b->status = CHN_STOP;              /* the mixer's release state (STOP, envelope bits 0) */
    m4a_engine_note_choke(&engine, 0, 64);
    ASSERT_EQ(b->status, 0, "choke off: a releasing channel is cut too");
    m4a_engine_note_choke(&engine, 1, 60);
    ASSERT_EQ(c->status, 0, "choke off: track 1's key cut");

    /* --- latch on: a sounding note, a note queued in the same frame, another key --- */
    m4a_engine_note_on(&engine, 0, 60, 127);
    m4a_engine_note_on(&engine, 0, 64, 127);
    m4a_engine_tick(&engine);
    a = find_pcm_note(&engine, 0, 60);
    b = find_pcm_note(&engine, 0, 64);
    ASSERT(a && b, "choke on: two notes sounding");
    if (!a || !b) { m4a_engine_destroy(&engine); free(wd); return; }
    a->status = b->status = CHN_ENV_SUSTAIN;  /* the mixer's envelope step would have run by now */
    m4a_engine_set_quantize_events(&engine, true);
    m4a_engine_note_off(&engine, 0, 60);              /* queued */
    m4a_engine_note_on(&engine, 0, 62, 127);           /* queued */
    m4a_engine_note_on(&engine, 0, 60, 127);           /* queued: the key retriggered */
    m4a_engine_cc(&engine, 0, 7, 100);                 /* queued */
    m4a_engine_note_off(&engine, 0, 64);               /* queued */
    ASSERT_EQ(engine.eventQueueCount, 5, "choke on: five events queued");
    m4a_engine_note_choke(&engine, 0, 60);
    ASSERT_EQ(a->status, 0, "choke on: the sounding note cut at once, not at the tick");
    ASSERT(!(b->status & CHN_STOP), "choke on: another key's queued note-off still waits");
    ASSERT_EQ(engine.eventQueueCount, 3, "choke on: the key's queued note-off and note-on removed");
    ASSERT_EQ(engine.eventQueue[0].kind, M4A_EV_NOTE_ON, "choke on: queue[0] is note-on 62");
    ASSERT_EQ(engine.eventQueue[0].a, 62, "choke on: queue[0] key 62");
    ASSERT_EQ(engine.eventQueue[1].kind, M4A_EV_CC, "choke on: queue[1] is the CC, order kept");
    ASSERT_EQ(engine.eventQueue[2].kind, M4A_EV_NOTE_OFF, "choke on: queue[2] is note-off 64");
    ASSERT_EQ(engine.eventQueue[2].a, 64, "choke on: queue[2] key 64");
    ASSERT(engine.quantizeEvents, "choke on: the latch stays on");
    m4a_engine_tick(&engine);
    ASSERT(find_pcm_note(&engine, 0, 60) == NULL, "choke on: the choked note-on never starts");
    ASSERT(find_pcm_note(&engine, 0, 62) != NULL, "choke on: the other queued note-on starts at the tick");
    ASSERT(b->status & CHN_STOP, "choke on: key 64's note-off applied at the tick");
    ASSERT_EQ(engine.tracks[0].rawVolume, 100, "choke on: the queued CC applied at the tick");

    /* --- latch on: a note queued and released in the same frame, choked
     * before its tick: nothing starts and nothing is left queued --- */
    m4a_engine_note_on(&engine, 0, 67, 127);
    m4a_engine_note_off(&engine, 0, 67);
    m4a_engine_note_choke(&engine, 0, 67);
    ASSERT_EQ(engine.eventQueueCount, 0, "choke on: a same-frame note and its note-off both removed");
    m4a_engine_tick(&engine);
    ASSERT(find_pcm_note(&engine, 0, 67) == NULL, "choke on: the same-frame note never starts");

    /* --- wildcards: key -1 on one track, track -1 for one key, both --- */
    for (int i = 0; i < TOTAL_PCM_CHANNELS; i++)
        engine.pcmChannels[i].status = 0;
    m4a_engine_set_quantize_events(&engine, false);
    m4a_engine_note_on(&engine, 0, 48, 127);
    m4a_engine_note_on(&engine, 0, 50, 127);
    m4a_engine_note_on(&engine, 1, 48, 127);
    m4a_engine_set_quantize_events(&engine, true);
    m4a_engine_note_on(&engine, 0, 52, 127);           /* queued on track 0 */
    m4a_engine_note_on(&engine, 1, 52, 127);           /* queued on track 1 */
    m4a_engine_cc(&engine, 0, 7, 90);                  /* queued, not a note */
    m4a_engine_note_choke(&engine, 0, -1);
    ASSERT(find_pcm_note(&engine, 0, 48) == NULL && find_pcm_note(&engine, 0, 50) == NULL,
           "choke key -1: every key on track 0 cut");
    ASSERT(find_pcm_note(&engine, 1, 48) != NULL, "choke key -1: track 1 untouched");
    ASSERT_EQ(engine.eventQueueCount, 2, "choke key -1: track 0's queued note removed, the rest kept");
    ASSERT(engine.eventQueue[0].kind == M4A_EV_NOTE_ON && engine.eventQueue[0].track == 1,
           "choke key -1: track 1's note-on kept first");
    ASSERT_EQ(engine.eventQueue[1].kind, M4A_EV_CC, "choke key -1: the CC kept");
    m4a_engine_note_on(&engine, 0, 48, 127);           /* queued */
    M4APCMChannel *k49 = &engine.pcmChannels[10];      /* another key sounding on track 1 */
    k49->status = CHN_ENV_SUSTAIN;
    k49->trackIndex = 1;
    k49->midiKey = 49;
    m4a_engine_note_choke(&engine, -1, 48);
    ASSERT(find_pcm_note(&engine, 1, 48) == NULL, "choke track -1: key 48 on track 1 cut");
    ASSERT(find_pcm_note(&engine, 1, 49) == k49, "choke track -1: key 49 on track 1 untouched");
    ASSERT_EQ(engine.eventQueueCount, 2, "choke track -1: key 48's queued note-on removed");
    m4a_engine_note_choke(&engine, -1, -1);
    ASSERT_EQ(engine.eventQueueCount, 1, "choke -1 / -1: every queued note removed, the CC kept");
    ASSERT_EQ(engine.eventQueue[0].kind, M4A_EV_CC, "choke -1 / -1: only the CC left");
    int on = 0;
    for (int i = 0; i < TOTAL_PCM_CHANNELS; i++)
        on += (engine.pcmChannels[i].status & CHN_ON) != 0;
    ASSERT_EQ(on, 0, "choke -1 / -1: nothing sounding (key 49 on track 1 cut too)");

    /* --- out-of-range arguments are ignored --- */
    m4a_engine_note_on(&engine, 0, 60, 127);           /* queued */
    M4APCMChannel *k60 = &engine.pcmChannels[10];      /* key 60 sounding on track 0 */
    k60->status = CHN_ENV_SUSTAIN;
    k60->trackIndex = 0;
    k60->midiKey = 60;
    m4a_engine_note_choke(&engine, MAX_TRACKS, 60);
    m4a_engine_note_choke(&engine, 0, 128);
    ASSERT_EQ(engine.eventQueueCount, 2, "choke: track 16 / key 128 leave the queue alone");
    ASSERT_EQ(k60->status, CHN_ENV_SUSTAIN, "choke: track 16 / key 128 cut nothing");
    k60->status = 0;
    m4a_engine_flush_events(&engine);
    m4a_engine_set_quantize_events(&engine, false);

    /* --- the shadow pool (poly debug's lost sounds) is cut too --- */
    M4APCMChannel *sh = &engine.pcmChannels[MAX_PCM_CHANNELS];
    sh->status = CHN_ENV_SUSTAIN;
    sh->trackIndex = 0;
    sh->midiKey = 60;
    m4a_engine_note_choke(&engine, 0, 60);
    ASSERT_EQ(sh->status, 0, "choke: the shadow channel cut");
    ASSERT(find_pcm_note(&engine, 0, 60) == NULL, "choke: the real channel cut");

    /* --- a CGB channel: cut at once, the PSG oscillator off --- */
    m4a_engine_program_change(&engine, 0, 2);
    m4a_engine_set_quantize_events(&engine, true);
    m4a_engine_note_on(&engine, 0, 60, 127);
    m4a_engine_tick(&engine);
    M4ACGBChannel *sq = &engine.cgbChannels[0];
    ASSERT(sq->status & CHN_ON, "choke CGB: the square note sounding");
    WriteLog log = { .n = 0, .engine = &engine };
    engine.psg[0].writeLog = write_log_cb;
    engine.psg[0].writeLogUser = &log;
    m4a_engine_note_choke(&engine, 0, 60);
    ASSERT_EQ(sq->status, 0, "choke CGB: the channel cut at once with the latch on");
    EXPECT_LOG(&log, "choke CGB: CgbOscOff on square 1 (NR12 = 8, NR14 = 0x80)",
               PSG_REG_NR12, 0x08, PSG_REG_NR14, 0x80);
    engine.psg[0].writeLog = NULL;

    m4a_engine_destroy(&engine);
    free(wd);
}

/* The plugin's playing -> stopped edge (m4a_plugin.c plugin_process): the
 * latch is turned off -- applying what it holds -- before the portamento
 * reset, so a note queued before the stop cannot leave glide history. */
static void test_transport_stop_edge(void)
{
    printf("Testing the transport stop edge (latch flush, then portamento reset)...\n");

    WaveData *wd = seq_test_wave();
    ToneData voices[128];
    memset(voices, 0, sizeof(voices));
    voices[0].type = VOICE_DIRECTSOUND;
    voices[0].key = 60;
    voices[0].wav = wd;
    voices[0].attack = 0xFF;
    voices[0].sustain = 0xFF;
    voices[0].release = 0x80;

    M4AEngine engine;
    m4a_engine_init(&engine, 48000.0f);
    engine.portamentoEnabled = true;
    m4a_engine_set_voicegroup(&engine, voices);
    m4a_engine_program_change(&engine, 0, 0);
    m4a_engine_cc(&engine, 0, 0x5, 5);

    /* Playing: a note sounds and its note-off is queued; then the stop. */
    m4a_engine_set_quantize_events(&engine, true);
    m4a_engine_note_on(&engine, 0, 50, 127);
    m4a_engine_tick(&engine);
    M4APCMChannel *n50 = find_pcm_note(&engine, 0, 50);
    ASSERT(n50 != NULL, "stop edge: key 50 sounding while playing");
    if (!n50) { m4a_engine_destroy(&engine); free(wd); return; }
    n50->status = CHN_ENV_SUSTAIN;      /* the mixer's envelope step would have run by now */
    m4a_engine_note_off(&engine, 0, 50);
    ASSERT(!(n50->status & CHN_STOP), "stop edge: the note-off queued, not applied");
    m4a_engine_set_quantize_events(&engine, false);
    ASSERT(n50->status & CHN_STOP, "stop edge: the queued note-off reached its note at the flush (no hang)");
    m4a_engine_reset_portamento(&engine);

    /* A note-on and its note-off both queued in the last playing frame: the
     * flush runs with the latch still on, so the note gets the one-tick
     * gate and the next tick stops it -- it does not hang.  (Track 1: no
     * CC 5, so the note is not a legato inherit.) */
    m4a_engine_program_change(&engine, 1, 0);
    m4a_engine_set_quantize_events(&engine, true);
    m4a_engine_note_on(&engine, 1, 72, 127);
    m4a_engine_note_off(&engine, 1, 72);
    m4a_engine_set_quantize_events(&engine, false);
    M4APCMChannel *n72 = find_pcm_note(&engine, 1, 72);
    ASSERT(n72 != NULL, "stop edge: a same-frame note started at the flush");
    if (n72) {
        ASSERT_EQ(n72->status & (CHN_START | CHN_STOP), CHN_START, "stop edge: same-frame note START without STOP");
        ASSERT_EQ(n72->gateTime, 1, "stop edge: same-frame note gets the one-tick gate");
        n72->status = CHN_ENV_SUSTAIN;
        m4a_engine_tick(&engine);
        ASSERT(n72->status & CHN_STOP, "stop edge: the next tick's gate walk stops it with the latch off");
        n72->status = 0;
    }
    m4a_engine_reset_portamento(&engine);

    /* Playing again (key 50 is the history): a note-on queued, then the
     * stop edge in the plugin's order. */
    m4a_engine_set_quantize_events(&engine, true);
    m4a_engine_note_on(&engine, 0, 50, 127);
    m4a_engine_tick(&engine);
    m4a_engine_note_on(&engine, 0, 48, 127);
    ASSERT_EQ(engine.eventQueueCount, 1, "stop edge: a note-on queued before the stop");
    m4a_engine_set_quantize_events(&engine, false);
    ASSERT_EQ(engine.eventQueueCount, 0, "stop edge: the latch's flush emptied the queue");
    ASSERT(find_pcm_note(&engine, 0, 48) != NULL, "stop edge: the queued note-on applied at the flush");
    ASSERT(engine.tracks[0].portamentoGliding, "stop edge: the flushed note-on glides from 50, as at its tick");
    ASSERT_EQ(engine.tracks[0].portamentoTargetKey, 48, "stop edge: the flushed note-on is the glide target");
    m4a_engine_reset_portamento(&engine);
    ASSERT_EQ(engine.tracks[0].portamentoPrevKey, 0, "stop edge: the reset after the flush forgets the history");
    ASSERT(!engine.tracks[0].portamentoGliding, "stop edge: the reset ends the glide");
    {
        /* The glide the flush started is not left frozen at its start key:
         * the reset ends it at its target, the note's own pitch. */
        M4APCMChannel *n48 = find_pcm_note(&engine, 0, 48);
        ASSERT(n48 != NULL, "stop edge: the flushed key 48 sounding");
        if (n48)
            ASSERT_EQ(n48->frequency, m4a_midi_key_to_freq(n48->wav, 48, 0),
                      "stop edge: the reset leaves the flushed note at key 48's pitch, not key 50's");
    }

    /* Stopped: the first live note does not glide, and applies at once. */
    int h = m4a_engine_note_on(&engine, 0, 60, 127);
    ASSERT(h >= 0, "stop edge: the live note applies at once (latch off)");
    ASSERT(!engine.tracks[0].portamentoGliding, "stop edge: no glide from a note queued before the stop");
    ASSERT_EQ(engine.tracks[0].portamentoPrevKey, 60, "stop edge: the live note is the new history");
    ASSERT_EQ(engine.tracks[0].portamentoDuration, 5, "stop edge: CC 5 survives");
    m4a_engine_destroy(&engine);

    /* The hazard the order prevents: reset first, then the flush. */
    m4a_engine_init(&engine, 48000.0f);
    engine.portamentoEnabled = true;
    m4a_engine_set_voicegroup(&engine, voices);
    m4a_engine_program_change(&engine, 0, 0);
    m4a_engine_cc(&engine, 0, 0x5, 5);
    m4a_engine_set_quantize_events(&engine, true);
    m4a_engine_note_on(&engine, 0, 48, 127);
    m4a_engine_reset_portamento(&engine);
    m4a_engine_set_quantize_events(&engine, false);
    m4a_engine_note_on(&engine, 0, 60, 127);
    ASSERT(engine.tracks[0].portamentoGliding,
           "stop edge, reversed order: the live note glides from the queued one (the hazard)");
    m4a_engine_destroy(&engine);

    /* Stopped -> playing: nothing was queued; the block's events queue. */
    m4a_engine_init(&engine, 48000.0f);
    m4a_engine_set_voicegroup(&engine, voices);
    m4a_engine_program_change(&engine, 0, 0);
    ASSERT(m4a_engine_note_on(&engine, 0, 64, 127) >= 0, "start edge: stopped, a note applies at once");
    m4a_engine_set_quantize_events(&engine, true);
    ASSERT_EQ(engine.eventQueueCount, 0, "start edge: the flip queues nothing from the stopped block");
    ASSERT_EQ(m4a_engine_note_on(&engine, 0, 60, 127), M4A_CHANNEL_QUEUED, "start edge: the next event queues");
    m4a_engine_set_quantize_events(&engine, true);
    ASSERT_EQ(engine.eventQueueCount, 1, "start edge: a repeated 'playing' block keeps the queue");
    m4a_engine_destroy(&engine);
    free(wd);
}

/* The plugin's latch override (m4a_engine_apply_latch_mode, called by
 * plugin_process at every block and transport event) and its parsers. */
static void test_latch_mode(void)
{
    printf("Testing the latch override (m4a_engine_apply_latch_mode)...\n");

    /* --- parsers --- */
    ASSERT_EQ(m4a_latch_mode_from_name("transport"), M4A_LATCH_TRANSPORT, "name: transport");
    ASSERT_EQ(m4a_latch_mode_from_name("on"), M4A_LATCH_ON, "name: on");
    ASSERT_EQ(m4a_latch_mode_from_name("off"), M4A_LATCH_OFF, "name: off");
    ASSERT_EQ(m4a_latch_mode_from_name("1"), M4A_LATCH_ON, "name: 1 = on");
    ASSERT_EQ(m4a_latch_mode_from_name("0"), M4A_LATCH_OFF, "name: 0 = off");
    ASSERT_EQ(m4a_latch_mode_from_name(""), M4A_LATCH_TRANSPORT, "name: empty = transport");
    ASSERT_EQ(m4a_latch_mode_from_name("bogus"), M4A_LATCH_TRANSPORT, "name: unknown = transport");
    ASSERT_EQ(m4a_latch_mode_from_name(NULL), M4A_LATCH_TRANSPORT, "name: NULL = transport");
    ASSERT_EQ(m4a_latch_mode_from_name(" on "), M4A_LATCH_ON, "name: surrounding blanks ignored (on)");
    ASSERT_EQ(m4a_latch_mode_from_name("off\t"), M4A_LATCH_OFF, "name: trailing tab ignored (off)");
    ASSERT_EQ(m4a_latch_mode_from_name("On"), M4A_LATCH_TRANSPORT, "name: case-sensitive (On = transport)");
    ASSERT_EQ(m4a_latch_mode_from_name("onx"), M4A_LATCH_TRANSPORT, "name: a longer word is not on");
    ASSERT_EQ(m4a_latch_mode_from_name("10"), M4A_LATCH_TRANSPORT, "name: 10 is not 1");
    ASSERT_EQ(m4a_latch_mode_from_byte(0), M4A_LATCH_TRANSPORT, "byte 0 = transport (an old state's default)");
    ASSERT_EQ(m4a_latch_mode_from_byte(1), M4A_LATCH_ON, "byte 1 = on");
    ASSERT_EQ(m4a_latch_mode_from_byte(2), M4A_LATCH_OFF, "byte 2 = off");
    ASSERT_EQ(m4a_latch_mode_from_byte(3), M4A_LATCH_TRANSPORT, "byte 3 = transport");
    ASSERT_EQ(m4a_latch_mode_from_byte(255), M4A_LATCH_TRANSPORT, "byte 255 = transport");

    WaveData *wd = seq_test_wave();
    ToneData voices[128];
    memset(voices, 0, sizeof(voices));
    voices[0].type = VOICE_DIRECTSOUND;
    voices[0].key = 60;
    voices[0].wav = wd;
    voices[0].attack = 0xFF;
    voices[0].sustain = 0xFF;
    voices[0].release = 0x80;

    /* --- the latch each mode asks for, per transport state --- */
    {
        static const struct { M4ALatchMode mode; bool playing; bool latch; } cases[] = {
            { M4A_LATCH_TRANSPORT, true,  true  }, { M4A_LATCH_TRANSPORT, false, false },
            { M4A_LATCH_ON,        true,  true  }, { M4A_LATCH_ON,        false, true  },
            { M4A_LATCH_OFF,       true,  false }, { M4A_LATCH_OFF,       false, false },
            { (M4ALatchMode)7,     true,  true  }, { (M4ALatchMode)7,     false, false },
        };
        for (size_t c = 0; c < sizeof(cases) / sizeof(cases[0]); c++) {
            char msg[96];
            M4AEngine engine;
            m4a_engine_init(&engine, 48000.0f);
            m4a_engine_set_quantize_events(&engine, !cases[c].latch);
            m4a_engine_apply_latch_mode(&engine, cases[c].mode, cases[c].playing, cases[c].playing);
            snprintf(msg, sizeof(msg), "mode %d, %s: latch %s", (int)cases[c].mode,
                     cases[c].playing ? "playing" : "stopped", cases[c].latch ? "on" : "off");
            ASSERT_EQ(engine.quantizeEvents, cases[c].latch, msg);
            m4a_engine_destroy(&engine);
        }
    }

    /* --- the stop edge through the policy, per mode: queued note-off and
     * note-on applied, the glide history forgotten, the latch per mode --- */
    for (int m = 0; m < 2; m++) {
        M4ALatchMode mode = m == 0 ? M4A_LATCH_TRANSPORT : M4A_LATCH_ON;
        const char *name = m == 0 ? "transport" : "on";
        char msg[128];
        M4AEngine engine;
        m4a_engine_init(&engine, 48000.0f);
        engine.portamentoEnabled = true;
        m4a_engine_set_voicegroup(&engine, voices);
        m4a_engine_program_change(&engine, 0, 0);
        m4a_engine_program_change(&engine, 1, 0);
        m4a_engine_cc(&engine, 0, 0x5, 5);
        m4a_engine_apply_latch_mode(&engine, mode, true, true);       /* playing */
        m4a_engine_note_on(&engine, 0, 50, 127);
        m4a_engine_tick(&engine);
        M4APCMChannel *n50 = find_pcm_note(&engine, 0, 50);
        snprintf(msg, sizeof(msg), "%s stop edge: key 50 sounding", name);
        ASSERT(n50 != NULL, msg);
        if (!n50) { m4a_engine_destroy(&engine); continue; }
        m4a_engine_note_on(&engine, 1, 40, 127);        /* track 1: a queued note to see applied */
        m4a_engine_note_on(&engine, 0, 48, 127);        /* glides from 50 when applied */
        snprintf(msg, sizeof(msg), "%s stop edge: two note-ons queued", name);
        ASSERT_EQ(engine.eventQueueCount, 2, msg);

        m4a_engine_apply_latch_mode(&engine, mode, true, false);      /* the stop */
        snprintf(msg, sizeof(msg), "%s stop edge: the queue applied", name);
        ASSERT_EQ(engine.eventQueueCount, 0, msg);
        snprintf(msg, sizeof(msg), "%s stop edge: track 1's queued note started", name);
        ASSERT(find_pcm_note(&engine, 1, 40) != NULL, msg);
        M4APCMChannel *n48 = find_pcm_note(&engine, 0, 48);
        snprintf(msg, sizeof(msg), "%s stop edge: key 48 sounding", name);
        ASSERT(n48 != NULL, msg);
        if (n48) {
            snprintf(msg, sizeof(msg), "%s stop edge: key 48 at its own pitch (glide ended)", name);
            ASSERT_EQ(n48->frequency, m4a_midi_key_to_freq(n48->wav, 48, 0), msg);
        }
        snprintf(msg, sizeof(msg), "%s stop edge: glide history forgotten", name);
        ASSERT(engine.tracks[0].portamentoPrevKey == 0 && !engine.tracks[0].portamentoGliding, msg);
        snprintf(msg, sizeof(msg), "%s stop edge: latch %s after the stop", name, m == 0 ? "off" : "on");
        ASSERT_EQ(engine.quantizeEvents, m == 1, msg);

        /* the first note after the stop: no glide; at once (transport) or
         * at the next tick (on) */
        int h = m4a_engine_note_on(&engine, 0, 60, 127);
        if (m == 1) {
            snprintf(msg, sizeof(msg), "%s: after the stop a note still queues", name);
            ASSERT_EQ(h, M4A_CHANNEL_QUEUED, msg);
            m4a_engine_tick(&engine);
        } else {
            snprintf(msg, sizeof(msg), "%s: after the stop a note applies at once", name);
            ASSERT(h >= 0, msg);
        }
        snprintf(msg, sizeof(msg), "%s: the first note after the stop does not glide", name);
        ASSERT(!engine.tracks[0].portamentoGliding && engine.tracks[0].portamentoPrevKey == 60, msg);
        m4a_engine_destroy(&engine);
    }

    /* --- no edge, no reset: a mode change while stopped flushes (on ->
     * off / transport) but keeps the glide history --- */
    {
        M4AEngine engine;
        m4a_engine_init(&engine, 48000.0f);
        engine.portamentoEnabled = true;
        m4a_engine_set_voicegroup(&engine, voices);
        m4a_engine_program_change(&engine, 0, 0);
        m4a_engine_cc(&engine, 0, 0x5, 5);
        m4a_engine_apply_latch_mode(&engine, M4A_LATCH_ON, false, false);
        ASSERT(engine.quantizeEvents, "on while stopped: latched");
        m4a_engine_note_on(&engine, 0, 50, 127);
        ASSERT_EQ(engine.eventQueueCount, 1, "on while stopped: queued");
        m4a_engine_apply_latch_mode(&engine, M4A_LATCH_ON, false, false);
        ASSERT_EQ(engine.eventQueueCount, 1, "on, a block later: the queue kept for the tick");
        m4a_engine_apply_latch_mode(&engine, M4A_LATCH_TRANSPORT, false, false);
        ASSERT(!engine.quantizeEvents, "on -> transport while stopped: latch off");
        ASSERT_EQ(engine.eventQueueCount, 0, "on -> transport while stopped: the queue applied");
        ASSERT(find_pcm_note(&engine, 0, 50) != NULL, "on -> transport while stopped: the note started");
        ASSERT_EQ(engine.tracks[0].portamentoPrevKey, 50, "no edge: the glide history kept");
        m4a_engine_apply_latch_mode(&engine, M4A_LATCH_ON, true, true);
        m4a_engine_note_on(&engine, 0, 52, 127);
        m4a_engine_apply_latch_mode(&engine, M4A_LATCH_OFF, true, true);
        ASSERT(!engine.quantizeEvents, "on -> off while playing: latch off");
        ASSERT(find_pcm_note(&engine, 0, 52) != NULL, "on -> off while playing: the queued note applied");
        ASSERT(engine.tracks[0].portamentoGliding, "on -> off while playing: no reset (the note glides from 50)");
        m4a_engine_apply_latch_mode(&engine, M4A_LATCH_OFF, true, false);
        ASSERT_EQ(engine.tracks[0].portamentoPrevKey, 0, "off: the stop edge still resets the history");
        m4a_engine_destroy(&engine);
    }

    /* --- off at the stop edge: the latch stays off, a glide in progress
     * ends at its target --- */
    {
        M4AEngine engine;
        m4a_engine_init(&engine, 48000.0f);
        engine.portamentoEnabled = true;
        m4a_engine_set_voicegroup(&engine, voices);
        m4a_engine_program_change(&engine, 0, 0);
        m4a_engine_cc(&engine, 0, 0x5, 5);
        m4a_engine_apply_latch_mode(&engine, M4A_LATCH_OFF, true, true);
        m4a_engine_note_on(&engine, 0, 50, 127);
        m4a_engine_note_on(&engine, 0, 48, 127);     /* glides from 50, at once */
        ASSERT(engine.tracks[0].portamentoGliding, "off: the second note glides");
        m4a_engine_apply_latch_mode(&engine, M4A_LATCH_OFF, true, false);
        ASSERT(!engine.quantizeEvents, "off, stop edge: the latch stays off");
        M4APCMChannel *n48 = find_pcm_note(&engine, 0, 48);
        ASSERT(n48 != NULL, "off, stop edge: key 48 sounding");
        if (n48)
            ASSERT_EQ(n48->frequency, m4a_midi_key_to_freq(n48->wav, 48, 0),
                      "off, stop edge: the glide ends at its target");
        m4a_engine_destroy(&engine);
    }

    /* --- the start edge through the policy: transport turns the latch on
     * and resets nothing --- */
    {
        M4AEngine engine;
        m4a_engine_init(&engine, 48000.0f);
        engine.portamentoEnabled = true;
        m4a_engine_set_voicegroup(&engine, voices);
        m4a_engine_program_change(&engine, 0, 0);
        m4a_engine_cc(&engine, 0, 0x5, 5);
        m4a_engine_note_on(&engine, 0, 50, 127);     /* stopped: at once */
        m4a_engine_apply_latch_mode(&engine, M4A_LATCH_TRANSPORT, false, true);
        ASSERT(engine.quantizeEvents, "start edge: transport turns the latch on");
        ASSERT_EQ(engine.tracks[0].portamentoPrevKey, 50, "start edge: the glide history kept");
        ASSERT_EQ(m4a_engine_note_on(&engine, 0, 52, 127), M4A_CHANNEL_QUEUED, "start edge: the next note queues");
        m4a_engine_destroy(&engine);
    }

    /* --- on at the stop edge with a note-on and its note-off queued in the
     * last frame: the drain runs with the latch on, so the note gets the
     * one-tick gate and the next tick stops it (no hang) --- */
    {
        M4AEngine engine;
        m4a_engine_init(&engine, 48000.0f);
        m4a_engine_set_voicegroup(&engine, voices);
        m4a_engine_program_change(&engine, 0, 0);
        m4a_engine_apply_latch_mode(&engine, M4A_LATCH_ON, true, true);
        m4a_engine_note_on(&engine, 0, 72, 127);
        m4a_engine_note_off(&engine, 0, 72);
        m4a_engine_apply_latch_mode(&engine, M4A_LATCH_ON, true, false);
        ASSERT_EQ(engine.eventQueueCount, 0, "on, stop edge: note-on and note-off applied");
        M4APCMChannel *n72 = find_pcm_note(&engine, 0, 72);
        ASSERT(n72 != NULL, "on, stop edge: the same-frame note started");
        if (n72) {
            ASSERT_EQ(n72->gateTime, 1, "on, stop edge: the one-tick gate");
            n72->status = CHN_ENV_SUSTAIN;           /* the mixer's envelope step */
            m4a_engine_tick(&engine);
            ASSERT(n72->status & CHN_STOP, "on, stop edge: stopped by the next tick");
        }
        m4a_engine_destroy(&engine);
    }
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
    test_psg_square();
    test_psg_sweep();
    test_psg_wave();
    test_psg_noise();
    test_cgb_driver_boot();
    test_cgb_driver_square();
    test_cgb_driver_wave_noise();
    test_sweep();
    test_lfo_tempo_scaling();
    test_lfos_keeps_phase();
    test_golden_sun_synth();
    test_golden_sun_synth_waveforms();
    test_pcm_reverse();
    test_pcm_reverse_legato_inherit();
    test_pcm_dpcm();
    test_pcm_dpcm_reverse_walk();
    test_mixer_arithmetic();
    test_hq_mixer();
    test_song_parser();
    test_sequencer_tempo();
    test_sequencer_notes();
    test_sequencer_deferred_volume();
    test_sequencer_flow();
    test_sequencer_lfo_and_engine_tick();
    test_sequencer_lfo_delay_note_on();
    test_sequencer_cycles();
    test_sequencer_cycles_lfos_zero();
    test_sequencer_lfo_untruncated_sum();
    test_sequencer_cycles_poly_invert();
    test_sequencer_track_stop_chain();
    test_sequencer_fine_orphan();
    test_sequencer_players();
    test_sequencer_cycles_se2();
    test_cry_song();
    test_cry_table_index();
    test_sequencer_xrele();
    test_sequencer_cycles_cry();
    test_pcm_start_offset();
    test_sequencer_xcmd_0D();
    test_sequencer_endtie_walk();
    test_sequencer_cycles_cry_faint();
    test_pcm_release_envelope();
    test_sequencer_cycles_cry_faint_full();
    test_sequencer_cycles_cry_growl1_reverse();
    test_sequencer_cry_two_tracks();
    test_sequencer_cycles_cry_encounter_chorus();
    test_sequencer_cycles_cry_high_pitch_two_chain();
    test_sequencer_cycles_two_cries();
    test_sequencer_cycles_cry_over_song();
    test_fifo_dma_stall();
    test_fixed_rate_voice_ignores_pitch();
    test_midi_path_modt_tune_lfodl();
    test_event_latch();
    test_note_choke();
    test_transport_stop_edge();
    test_latch_mode();
    test_gba_start_delay_origin();
    test_gba_tempo_grid();
    test_engine_lfo_grid();
    test_lfo_cgbsound_order();
    test_cgb_entry_midi_path();
    test_tick_batching_midi_path();
    test_two_tick_interleave_midi_path();

    printf("\n=== Results: %d/%d tests passed ===\n", tests_passed, tests_run);
    return (tests_passed == tests_run) ? 0 : 1;
}
