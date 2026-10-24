#include "m4a_blip.h"
#include <stdlib.h>
#include <string.h>

/* Fixed-point layout.  A sample period is 2^TIME_BITS; the top 32 bits are
 * discarded before indexing so the per-frame products fit a 64-bit word, and
 * the remaining FRAC_BITS select the sample slot and the interpolation. */
#define PRE_SHIFT   32
#define TIME_BITS   (PRE_SHIFT + 20)
#define FRAC_BITS   (TIME_BITS - PRE_SHIFT)
#define TIME_UNIT   ((uint64_t)1 << TIME_BITS)
#define DELTA_BITS  15
#define DELTA_UNIT  (1 << DELTA_BITS)
#define BASS_SHIFT  9       /* high-pass corner */
#define HALF_WIDTH  8       /* taps reaching past the last sample */
#define END_FRAME_EXTRA 2   /* deltas slightly after the frame end */
#define BUF_EXTRA   (HALF_WIDTH * 2 + END_FRAME_EXTRA)

M4ABlip *m4a_blip_new(int size)
{
    if (size < 0)
        return NULL;
    M4ABlip *b = (M4ABlip *)malloc(sizeof(*b));
    if (!b)
        return NULL;
    b->buf = (int32_t *)malloc(((size_t)size + BUF_EXTRA) * sizeof(int32_t));
    if (!b->buf) {
        free(b);
        return NULL;
    }
    b->factor = TIME_UNIT / M4A_BLIP_MAX_RATIO;
    b->size = size;
    m4a_blip_clear(b);
    return b;
}

void m4a_blip_delete(M4ABlip *b)
{
    if (!b)
        return;
    free(b->buf);
    memset(b, 0, sizeof(*b));
    free(b);
}

void m4a_blip_set_rates(M4ABlip *b, double clock_rate, double sample_rate)
{
    double factor = (double)TIME_UNIT * sample_rate / clock_rate;
    b->factor = (uint64_t)factor;
    /* Round up so that the whole-sample count never runs ahead of the clock. */
    if ((double)b->factor < factor)
        b->factor++;
}

void m4a_blip_clear(M4ABlip *b)
{
    b->offset = b->factor / 2;
    b->avail = 0;
    b->integrator = 0;
    memset(b->buf, 0, ((size_t)b->size + BUF_EXTRA) * sizeof(int32_t));
}

void m4a_blip_add_delta(M4ABlip *b, unsigned time, int delta)
{
    uint32_t fixed = (uint32_t)(((uint64_t)time * b->factor + b->offset) >> PRE_SHIFT);
    int32_t *out = b->buf + b->avail + (fixed >> FRAC_BITS);

    int interp = (int)((fixed >> (FRAC_BITS - DELTA_BITS)) & (DELTA_UNIT - 1));
    int delta2 = delta * interp;

    out[HALF_WIDTH - 1] += delta * DELTA_UNIT - delta2;
    out[HALF_WIDTH] += delta2;
}

int m4a_blip_clocks_needed(const M4ABlip *b, int samples)
{
    uint64_t needed = (uint64_t)samples * TIME_UNIT;
    if (needed < b->offset)
        return 0;
    return (int)((needed - b->offset + b->factor - 1) / b->factor);
}

void m4a_blip_end_frame(M4ABlip *b, unsigned clocks)
{
    uint64_t off = (uint64_t)clocks * b->factor + b->offset;
    b->avail += (int)(off >> TIME_BITS);
    b->offset = off & (TIME_UNIT - 1);
}

int m4a_blip_samples_avail(const M4ABlip *b)
{
    return b->avail;
}

static void remove_samples(M4ABlip *b, int count)
{
    int remain = b->avail + BUF_EXTRA - count;
    b->avail -= count;
    memmove(&b->buf[0], &b->buf[count], (size_t)remain * sizeof(int32_t));
    memset(&b->buf[remain], 0, (size_t)count * sizeof(int32_t));
}

int m4a_blip_read_samples(M4ABlip *b, short *out, int count, int stereo)
{
    if (count < 0)
        count = 0;
    if (count > b->avail)
        count = b->avail;
    if (count) {
        const int step = stereo ? 2 : 1;
        const int32_t *in = b->buf;
        const int32_t *end = in + count;
        int sum = b->integrator;
        do {
            int s = sum >> DELTA_BITS;      /* drop the fraction */
            sum += *in++;
            if ((short)s != s)              /* clamp to 16 bits */
                s = (s >> 16) ^ 0x7FFF;
            *out = (short)s;
            out += step;
            sum -= s << (DELTA_BITS - BASS_SHIFT);   /* high-pass */
        } while (in != end);
        b->integrator = sum;
        remove_samples(b, count);
    }
    return count;
}
