#ifndef M4A_RESAMPLER_H
#define M4A_RESAMPLER_H

/*
 * Delta-buffer resampler: brings a stream of level changes stamped in source
 * clocks to an output sample rate, the way mgba brings its GBA audio core to
 * the host rate.  Each change is split by two-tap linear interpolation between
 * the two output samples that straddle its instant and accumulated into an
 * integer buffer at fixed-point positions; the reader integrates the deltas
 * into samples, applies a gentle first-order high-pass (the "bass" filter)
 * and clamps to 16 bits.  Bit-exact with mgba's output resampler on that
 * path; test/test_resampler.c checks it against mgba when a source tree is
 * available.
 *
 * Time is in source clocks relative to the current frame; m4a_resampler_end_frame
 * ends the frame and makes the samples it covers readable.
 */

#include <stdint.h>

#ifdef __cplusplus
extern "C" {
#endif

/* Maximum source clocks per output sample the rate setter accepts, and the
 * maximum clocks per frame. */
#define M4A_RESAMPLER_MAX_RATIO 0x100000
#define M4A_RESAMPLER_MAX_FRAME 4000

typedef struct {
    uint64_t factor;    /* output samples per source clock, 52-bit fraction */
    uint64_t offset;    /* fractional position of the frame start */
    int avail;          /* whole samples ready to read */
    int size;           /* capacity in samples */
    int integrator;     /* running sum of deltas (15-bit fraction) */
    int32_t *buf;       /* size + extra delta slots */
} M4AResampler;

/* Allocate a buffer able to hold `size` output samples (plus the taps that
 * reach past the last sample).  Returns NULL on allocation failure. */
M4AResampler *m4a_resampler_new(int size);
void m4a_resampler_delete(M4AResampler *b);

/* Set the source clock rate and output sample rate.  clock_rate must not
 * exceed sample_rate * M4A_RESAMPLER_MAX_RATIO. */
void m4a_resampler_set_rates(M4AResampler *b, double clock_rate, double sample_rate);

/* Drop all buffered samples and pending deltas. */
void m4a_resampler_clear(M4AResampler *b);

/* Add a level change of `delta` at `time` clocks into the current frame.
 * Two-tap linear interpolation between the two output samples that straddle
 * the instant. */
void m4a_resampler_add_delta(M4AResampler *b, unsigned time, int delta);

/* Source clocks that must elapse before `samples` more samples are readable. */
int m4a_resampler_clocks_needed(const M4AResampler *b, int samples);

/* End the current frame after `clocks` source clocks. */
void m4a_resampler_end_frame(M4AResampler *b, unsigned clocks);

int m4a_resampler_samples_avail(const M4AResampler *b);

/* Read up to `count` samples into `out` (every other slot when `stereo`),
 * clamped to 16 bits.  Returns the number read. */
int m4a_resampler_read_samples(M4AResampler *b, short *out, int count, int stereo);

#ifdef __cplusplus
}
#endif

#endif /* M4A_RESAMPLER_H */
