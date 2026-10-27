/*
 * Bit-exactness test of plugin/m4a_resampler.c against mgba's output
 * resampler (blip_buf's fast linear delta path, blip_add_delta_fast), using
 * the copy vendored in an mgba source tree.  Built only when ACCURACY_MGBA_DIR
 * is set (CMake target poryaaaa_resampler_tests); the reference is linked
 * into this test only, never into the plugin.
 *
 * Feeds both buffers the same synthetic streams -- random deltas at random
 * clock offsets, GBA-shaped 65536 Hz steps, DC, clamping overload -- at the
 * rates the engine uses, and requires identical output samples, availability
 * counts and clocks_needed answers at every step.
 */
#include <mgba/core/blip_buf.h>
#include "m4a_resampler.h"
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

static int failures = 0;
static int checks = 0;

static uint32_t rng = 0x12345678u;
static uint32_t next_rand(void)
{
    rng ^= rng << 13;
    rng ^= rng >> 17;
    rng ^= rng << 5;
    return rng;
}

#define CHECK(cond, ...) do { checks++; if (!(cond)) { failures++; fprintf(stderr, "FAIL: "); fprintf(stderr, __VA_ARGS__); fprintf(stderr, "\n"); } } while (0)

static void compare_reads(blip_t *ref, M4AResampler *mine, int maxRead, int stereo, const char *what)
{
    CHECK(blip_samples_avail(ref) == m4a_resampler_samples_avail(mine),
          "%s: avail %d vs %d", what, blip_samples_avail(ref), m4a_resampler_samples_avail(mine));
    int n = blip_samples_avail(ref);
    if (n > maxRead) n = maxRead;
    short a[4096 * 2], b[4096 * 2];
    memset(a, 0x55, sizeof a);
    memset(b, 0x55, sizeof b);
    int ra = blip_read_samples(ref, a, n, stereo);
    int rb = m4a_resampler_read_samples(mine, b, n, stereo);
    CHECK(ra == rb, "%s: read count %d vs %d", what, ra, rb);
    CHECK(memcmp(a, b, sizeof a) == 0, "%s: samples differ", what);
}

/* One scenario: identical streams of (time, delta) events per frame. */
static void run_scenario(const char *name, double clockRate, double sampleRate,
                         int frames, unsigned frameClocks, int eventsPerFrame,
                         int deltaRange, int stereo, int readEvery)
{
    blip_t *ref = blip_new(8192);
    M4AResampler *mine = m4a_resampler_new(8192);
    blip_set_rates(ref, clockRate, sampleRate);
    m4a_resampler_set_rates(mine, clockRate, sampleRate);

    for (int f = 0; f < frames; f++) {
        for (int e = 0; e < eventsPerFrame; e++) {
            unsigned t = next_rand() % (frameClocks + 1);
            int d = (int)(next_rand() % (2 * deltaRange + 1)) - deltaRange;
            blip_add_delta_fast(ref, t, d);
            m4a_resampler_add_delta(mine, t, d);
        }
        for (int s = 1; s <= 8; s++) {
            CHECK(blip_clocks_needed(ref, s) == m4a_resampler_clocks_needed(mine, s),
                  "%s: clocks_needed(%d) %d vs %d", name, s,
                  blip_clocks_needed(ref, s), m4a_resampler_clocks_needed(mine, s));
        }
        blip_end_frame(ref, frameClocks);
        m4a_resampler_end_frame(mine, frameClocks);
        if (f % readEvery == readEvery - 1)
            compare_reads(ref, mine, 4096, stereo, name);
    }
    compare_reads(ref, mine, 4096, stereo, name);
    blip_delete(ref);
    m4a_resampler_delete(mine);
}

/* GBA-shaped stream: a 65536 Hz sample grid, one delta per 256-clock slot,
 * frames of 2048 clocks, as mgba's audio core produces them. */
static void run_gba_stream(double sampleRate, int frames, int amplitude, const char *name)
{
    blip_t *ref = blip_new(0x4000);
    M4AResampler *mine = m4a_resampler_new(0x4000);
    blip_set_rates(ref, 16777216.0, sampleRate);
    m4a_resampler_set_rates(mine, 16777216.0, sampleRate);
    int last = 0;
    unsigned phase = 0;
    for (int f = 0; f < frames; f++) {
        for (unsigned clock = 0; clock < 2048; clock += 256) {
            /* square wave at a period of 37 slots plus noise */
            int level = ((phase / 37) & 1) ? amplitude : -amplitude;
            level += (int)(next_rand() % 64) - 32;
            phase++;
            blip_add_delta_fast(ref, clock, level - last);
            m4a_resampler_add_delta(mine, clock, level - last);
            last = level;
        }
        blip_end_frame(ref, 2048);
        m4a_resampler_end_frame(mine, 2048);
        if (f % 97 == 96)
            compare_reads(ref, mine, 4096, 1, name);
    }
    compare_reads(ref, mine, 4096, 1, name);
    blip_delete(ref);
    m4a_resampler_delete(mine);
}

int main(void)
{
    printf("=== m4a_blip vs blip_buf (fast linear path) ===\n");
    static const double rates[] = { 48000.0, 44100.0, 96000.0, 22050.0, 192000.0 };
    for (unsigned i = 0; i < sizeof rates / sizeof *rates; i++) {
        char name[64];
        snprintf(name, sizeof name, "random@%g", rates[i]);
        run_scenario(name, 16777216.0, rates[i], 400, 2048, 7, 30000, 0, 3);
        snprintf(name, sizeof name, "random-stereo@%g", rates[i]);
        run_scenario(name, 16777216.0, rates[i], 400, 2048, 7, 30000, 1, 5);
        snprintf(name, sizeof name, "gba-square@%g", rates[i]);
        run_gba_stream(rates[i], 3000, 20000, name);
        snprintf(name, sizeof name, "gba-overload@%g", rates[i]);
        run_gba_stream(rates[i], 1500, 40000, name);   /* clamps every step */
    }
    /* Odd frame lengths and DC: deltas only at frame start. */
    run_scenario("dc-odd-frames", 16777216.0, 48000.0, 1000, 1023, 1, 200, 0, 7);
    /* Other source clocks (GB) for the rate math. */
    run_scenario("gb-clock", 4194304.0, 48000.0, 500, 4096, 5, 10000, 0, 2);
    printf("%d checks, %d failures\n", checks, failures);
    return failures ? 1 : 0;
}
