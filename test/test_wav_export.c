#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <math.h>
#include "m4a_engine.h"
#include "m4a_channel.h"
#include "m4a_reverb.h"
#include "voicegroup_loader.h"

/*
 * Standalone test program for the m4a engine.
 * Loads a voicegroup from the pokeemerald project, plays a sequence of notes,
 * and writes the output to a WAV file.
 *
 * Usage: poryaaaa_test <project_root> <voicegroup_name> [output.wav]
 *
 * Example:
 *   ./poryaaaa_test /path/to/pokeemerald petalburg output.wav
 */

#define SAMPLE_RATE 44100
#define DURATION_SECONDS 8
#define TOTAL_SAMPLES (SAMPLE_RATE * DURATION_SECONDS)

/* WAV file writing */
static void write_u16_le(FILE *f, uint16_t val) {
    uint8_t buf[2] = { val & 0xFF, (val >> 8) & 0xFF };
    fwrite(buf, 1, 2, f);
}

static void write_u32_le(FILE *f, uint32_t val) {
    uint8_t buf[4] = { val & 0xFF, (val >> 8) & 0xFF, (val >> 16) & 0xFF, (val >> 24) & 0xFF };
    fwrite(buf, 1, 4, f);
}

static int write_wav(const char *path, const float *left, const float *right,
                     int numSamples, int sampleRate)
{
    FILE *f = fopen(path, "wb");
    if (!f) {
        fprintf(stderr, "Cannot open %s for writing\n", path);
        return -1;
    }

    uint16_t numChannels = 2;
    uint16_t bitsPerSample = 16;
    uint32_t byteRate = sampleRate * numChannels * bitsPerSample / 8;
    uint16_t blockAlign = numChannels * bitsPerSample / 8;
    uint32_t dataSize = numSamples * numChannels * bitsPerSample / 8;

    /* RIFF header */
    fwrite("RIFF", 1, 4, f);
    write_u32_le(f, 36 + dataSize);
    fwrite("WAVE", 1, 4, f);

    /* fmt chunk */
    fwrite("fmt ", 1, 4, f);
    write_u32_le(f, 16);           /* chunk size */
    write_u16_le(f, 1);            /* PCM format */
    write_u16_le(f, numChannels);
    write_u32_le(f, sampleRate);
    write_u32_le(f, byteRate);
    write_u16_le(f, blockAlign);
    write_u16_le(f, bitsPerSample);

    /* data chunk */
    fwrite("data", 1, 4, f);
    write_u32_le(f, dataSize);

    for (int i = 0; i < numSamples; i++) {
        /* Convert float to 16-bit signed */
        int32_t l = (int32_t)(left[i] * 32767.0f);
        int32_t r = (int32_t)(right[i] * 32767.0f);
        if (l > 32767) l = 32767;
        if (l < -32768) l = -32768;
        if (r > 32767) r = 32767;
        if (r < -32768) r = -32768;
        write_u16_le(f, (uint16_t)(int16_t)l);
        write_u16_le(f, (uint16_t)(int16_t)r);
    }

    fclose(f);
    return 0;
}

/*
 * Play a C major scale using program 1 (typically piano on most voicegroups)
 */
static void play_test_sequence(M4AEngine *engine)
{
    /* Notes: C4, D4, E4, F4, G4, A4, B4, C5 */
    static const uint8_t scaleNotes[] = { 60, 62, 64, 65, 67, 69, 71, 72 };
    int numNotes = sizeof(scaleNotes) / sizeof(scaleNotes[0]);

    /* Set program 1 on channel 0 */
    m4a_engine_program_change(engine, 0, 1);

    /* Set volume and pan */
    m4a_engine_cc(engine, 0, 7, 127);  /* max volume */
    m4a_engine_cc(engine, 0, 10, 64);  /* center pan */

    /* Schedule notes: each note lasts ~0.5 seconds */
    int samplesPerNote = SAMPLE_RATE / 2;
    int noteGap = SAMPLE_RATE / 20;  /* 50ms gap between notes */

    float *outL = calloc(TOTAL_SAMPLES, sizeof(float));
    float *outR = calloc(TOTAL_SAMPLES, sizeof(float));

    int samplePos = 0;
    for (int n = 0; n < numNotes && samplePos < TOTAL_SAMPLES; n++) {
        /* Note on */
        m4a_engine_note_on(engine, 0, scaleNotes[n], 100);

        /* Render note duration */
        int renderLen = samplesPerNote;
        if (samplePos + renderLen > TOTAL_SAMPLES)
            renderLen = TOTAL_SAMPLES - samplePos;
        m4a_engine_process(engine, outL + samplePos, outR + samplePos, renderLen);
        samplePos += renderLen;

        /* Note off */
        m4a_engine_note_off(engine, 0, scaleNotes[n]);

        /* Render gap (release tail) */
        int gapLen = noteGap;
        if (samplePos + gapLen > TOTAL_SAMPLES)
            gapLen = TOTAL_SAMPLES - samplePos;
        if (gapLen > 0) {
            m4a_engine_process(engine, outL + samplePos, outR + samplePos, gapLen);
            samplePos += gapLen;
        }
    }

    /* Also test a chord: C major (C4, E4, G4) */
    if (samplePos < TOTAL_SAMPLES) {
        m4a_engine_note_on(engine, 0, 60, 100);
        m4a_engine_note_on(engine, 0, 64, 100);
        m4a_engine_note_on(engine, 0, 67, 100);

        int chordLen = SAMPLE_RATE;  /* 1 second */
        if (samplePos + chordLen > TOTAL_SAMPLES)
            chordLen = TOTAL_SAMPLES - samplePos;
        m4a_engine_process(engine, outL + samplePos, outR + samplePos, chordLen);
        samplePos += chordLen;

        m4a_engine_note_off(engine, 0, 60);
        m4a_engine_note_off(engine, 0, 64);
        m4a_engine_note_off(engine, 0, 67);

        /* Render release tail */
        int tailLen = TOTAL_SAMPLES - samplePos;
        if (tailLen > 0) {
            m4a_engine_process(engine, outL + samplePos, outR + samplePos, tailLen);
        }
    }

    /* Write WAV - handled by caller using returned buffers */
    /* For simplicity, write here */
    printf("Writing output.wav...\n");
    write_wav("output.wav", outL, outR, TOTAL_SAMPLES, SAMPLE_RATE);

    free(outL);
    free(outR);
}

/*
 * Play a multi-program test using various instruments
 */
static void play_multi_program_test(M4AEngine *engine, const char *outputPath)
{
    int totalSamples = SAMPLE_RATE * 12;
    float *outL = calloc(totalSamples, sizeof(float));
    float *outR = calloc(totalSamples, sizeof(float));
    int pos = 0;

    /* Test programs 0-7 with a short melody each */
    uint8_t programs[] = { 0, 1, 35, 45, 56, 125, 126, 127 };
    for (int i = 0; i < 8 && pos < totalSamples; i++) {
        uint8_t prog = programs[i];
        m4a_engine_program_change(engine, 0, prog);
        m4a_engine_cc(engine, 0, 7, 127);
        m4a_engine_cc(engine, 0, 10, 64);

        /* Play 3 notes */
        uint8_t notesInstrument[] = { 60, 64, 67 };
        uint8_t notesPercussion[] = { 38, 39, 40 };
        for (int n = 0; n < 3 && pos < totalSamples; n++) {
            uint8_t *notes = prog == 0 ? notesPercussion : notesInstrument;
            m4a_engine_note_on(engine, 0, notes[n], 100);

            int len = SAMPLE_RATE / 3;
            if (pos + len > totalSamples) len = totalSamples - pos;
            m4a_engine_process(engine, outL + pos, outR + pos, len);
            pos += len;

            m4a_engine_note_off(engine, 0, notes[n]);
        }

        /* Small gap */
        int gap = SAMPLE_RATE / 8;
        if (pos + gap > totalSamples) gap = totalSamples - pos;
        if (gap > 0) {
            m4a_engine_process(engine, outL + pos, outR + pos, gap);
            pos += gap;
        }
    }

    /* Fill remaining */
    if (pos < totalSamples) {
        m4a_engine_process(engine, outL + pos, outR + pos, totalSamples - pos);
    }

    printf("Writing %s...\n", outputPath);
    write_wav(outputPath, outL, outR, totalSamples, SAMPLE_RATE);

    free(outL);
    free(outR);
}

int main(int argc, char *argv[])
{
    if (argc < 3) {
        fprintf(stderr, "Usage: %s <project_root> <voicegroup_name> [output.wav]\n", argv[0]);
        fprintf(stderr, "\nExample:\n");
        fprintf(stderr, "  %s /path/to/pokeemerald petalburg output.wav\n", argv[0]);
        return 1;
    }

    const char *projectRoot = argv[1];
    const char *vgName = argv[2];
    const char *outputPath = argc > 3 ? argv[3] : "output.wav";

    printf("Loading voicegroup '%s' from %s...\n", vgName, projectRoot);
    fflush(stdout);

    LoadedVoiceGroup *vg = voicegroup_load(projectRoot, vgName, NULL);
    if (!vg) {
        fprintf(stderr, "Failed to load voicegroup '%s'\n", vgName);
        return 1;
    }

    printf("Voicegroup loaded successfully.\n");

    /* Count loaded voices */
    int dsCount = 0, sq1Count = 0, sq2Count = 0, pwCount = 0, noiseCount = 0;
    int ksCount = 0, ksaCount = 0;
    for (int i = 0; i < VOICEGROUP_SIZE; i++) {
        switch (vg->voices[i].type & 0xC7) {
        case VOICE_DIRECTSOUND: if (vg->voices[i].wav) dsCount++; break;
        case VOICE_SQUARE_1: sq1Count++; break;
        case VOICE_SQUARE_2: sq2Count++; break;
        case VOICE_PROGRAMMABLE_WAVE: pwCount++; break;
        case VOICE_NOISE: noiseCount++; break;
        case VOICE_KEYSPLIT: ksCount++; break;
        case VOICE_KEYSPLIT_ALL: ksaCount++; break;
        }
    }
    printf("Voices: %d DirectSound, %d Square1, %d Square2, %d ProgWave, %d Noise\n",
           dsCount, sq1Count, sq2Count, pwCount, noiseCount);
    printf("        %d Keysplit, %d KeysplitAll\n", ksCount, ksaCount);
    printf("Loaded %d samples, %d prog waves, %d sub-voicegroups\n",
           vg->waveDataCount, vg->progWaveCount, vg->subGroupCount);

    /* Initialize engine */
    M4AEngine engine;
    m4a_engine_init(&engine, SAMPLE_RATE);
    m4a_engine_set_voicegroup(&engine, vg->voices);

    /* Run test */
    play_multi_program_test(&engine, outputPath);

    printf("Done! Output written to %s\n", outputPath);

    /* Cleanup */
    m4a_engine_destroy(&engine);
    voicegroup_free(vg);

    return 0;
}
