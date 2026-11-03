/*
 * resample.c: the sample rate converter of resample.h, a windowed-sinc
 * filter whose coefficients are interpolated between the rows of one table,
 * after FreeBSD's feeder_rate, in integer arithmetic.
 */

#include "resample.h"

#include "../lib/kstring.h"
#include "resample_coef.h"

/* The table position past the last coefficient, in entries, 16.16. */
#define RESAMPLE_END ((uint32_t)RESAMPLE_ZEROS * RESAMPLE_STEPS << 16)

/* The header's reach is the table's. */
typedef char resample_reach_is_table[RESAMPLE_REACH == RESAMPLE_ZEROS ? 1 : -1];

/* The table's positions, 16.16, must stay within 32 bits. */
typedef char resample_end_fits[RESAMPLE_ZEROS * RESAMPLE_STEPS < 32768 ? 1 : -1];

/**
 * `n` over `d`, rounded down, where the quotient is below 2^32, and the
 * remainder in `*rest` unless it is NULL, by long division, so that no
 * 64-bit division reaches a helper of the compiler's. Only the setup
 * divides, never a frame's conversion.
 */
static uint32_t resample_div(uint64_t n, uint32_t d, uint32_t *rest)
{
    uint64_t rem = 0;
    uint32_t q = 0;
    int i;

    for (i = 63; i >= 0; i--) {
        rem = rem << 1 | ((n >> i) & 1u);
        q <<= 1;
        if (rem >= d) {
            rem -= d;
            q |= 1u;
        }
    }
    if (rest) {
        *rest = (uint32_t)rem;
    }
    return q;
}

/**
 * The table entries an input frame from `in` to `out` frames per second
 * moves, 16.16, and the filter's rate in `*low`: the lower of the two, or
 * the input's over RESAMPLE_RATIO when that is higher. An input frame is
 * low / in of one of the filter's frames.
 */
static uint32_t resample_step(uint32_t in, uint32_t out, uint32_t *low)
{
    *low = in < out ? in : out;
    if (*low * RESAMPLE_RATIO < in) {
        *low = (in + RESAMPLE_RATIO - 1u) / RESAMPLE_RATIO;
    }
    return resample_div((uint64_t)*low * RESAMPLE_STEPS << 16, in, NULL);
}

void resample_init(resampler_t *r, uint32_t in, uint32_t out)
{
    uint32_t low;

    k_memset(r->ring, 0, sizeof(r->ring));
    r->in = in;
    r->out = out;
    r->phase = r->at = r->at_rest = 0;
    r->head = 0;
    r->mults = 0;
    if (in == out) {
        r->step = r->gain = r->span = r->move = r->move_rest = 0;
        r->taps = r->owed = 1;
        return;
    }
    /* The taps sum to in / low before the gain; a conversion up has the
       gain 1, which `gain` 0 stands for. */
    r->step = resample_step(in, out, &low);
    r->gain = low < in ? resample_div((uint64_t)low << 32, in, NULL) : 0;
    r->move = resample_div((uint64_t)in * r->step, out, &r->move_rest);
    r->span = RESAMPLE_END / r->step + 1u;
    r->taps = 2u * r->span;
    r->owed = 1u;
}

void resample_delay(resampler_t *r, uint32_t delay)
{
    if (r->span == 0) {
        return;
    }
    r->owed += delay / r->out;
    r->phase = delay % r->out;
    r->at = resample_div((uint64_t)r->step * r->phase, r->out, &r->at_rest);
}

uint32_t resample_need(const resampler_t *r, uint32_t n)
{
    return n == 0 ? 0 : r->owed + (r->phase + (n - 1u) * r->in) / r->out;
}

uint32_t resample_held(const resampler_t *r)
{
    return r->span > r->owed ? r->span - r->owed : 0;
}

void resample_put(resampler_t *r, const int32_t *frame)
{
    uint32_t h = r->head;

    r->ring[h][0] = r->ring[h + r->taps][0] = frame[0];
    r->ring[h][1] = r->ring[h + r->taps][1] = frame[1];
    r->head = h + 1u == r->taps ? 0 : h + 1u;
    r->owed--;
}

/**
 * Sum the taps of one side of the window into `acc`, left and right: each
 * frame's difference from `centre` times the coefficient at table position
 * `pos`, 16.16 in entries, interpolated linearly between the two entries
 * about it, the frames from `w` on by `dir` and the positions from `pos`
 * on by r->step, until a position passes the table or r->span taps are
 * summed. The products are of 30 and 24 bits, and the coefficients of a
 * window sum in magnitude to under 2^35 at the largest ratio, so the sum
 * stays under 2^60. Gives the taps summed.
 *
 * When `pos` and the step are whole entries, as every position of a
 * conversion by a whole ratio down is, the coefficients are the entries
 * themselves and the loop reads them without the interpolation's product.
 * The kernel builds without the compiler's optimizer, which keeps every
 * local in memory, so that each step of a loop waits for the store of the
 * step before; GCC keeps a `register` local in a register even so.
 */
static uint32_t resample_side(const resampler_t *r, const int32_t (*w)[2], int dir, uint32_t pos,
                              const int32_t *centre, int64_t *acc)
{
    register int64_t left = acc[0], right = acc[1];
    register const int32_t *x = w[0];
    register uint32_t z = pos;
    register int32_t c;
    register const int32_t *t;
    int32_t cl = centre[0], cr = centre[1];
    int stride = 2 * dir;
    uint32_t step = r->step, n, k;

    n = pos >= RESAMPLE_END ? 0 : (RESAMPLE_END - 1u - pos) / step + 1u;
    if (n > r->span) {
        n = r->span;
    }
    if (((z | step) & 0xFFFFu) == 0) {
        t = resample_coef + (z >> 16);
        for (k = n; k > 0; k--) {
            c = *t;
            left += (int64_t)c * (x[0] - cl);
            right += (int64_t)c * (x[1] - cr);
            t += step >> 16;
            x += stride;
        }
    } else {
        for (k = n; k > 0; k--) {
            t = resample_coef + (z >> 16);
            c = t[0] + (int32_t)((int64_t)(t[1] - t[0]) * (int32_t)(z & 0xFFFFu) / 65536);
            left += (int64_t)c * (x[0] - cl);
            right += (int64_t)c * (x[1] - cr);
            z += step;
            x += stride;
        }
    }
    acc[0] = left;
    acc[1] = right;
    return n;
}

/**
 * `centre` plus `acc` in Q30 times `gain`, 0.32, or once for a gain of 0,
 * rounded to the nearest, a half away from zero, and held to 24 bits. The
 * product of the magnitude, under 2^60, and the gain is made in two halves
 * of the magnitude, the kernel having no 128-bit product; the magnitude is
 * taken since C89 leaves a negative number's right shift to the compiler.
 */
static int32_t resample_out(int32_t centre, int64_t acc, uint32_t gain)
{
    uint64_t a = (uint64_t)(acc < 0 ? -acc : acc);
    int32_t d, v;

    if (gain != 0) {
        a = (a >> 32) * gain + ((a & 0xFFFFFFFFu) * gain >> 32);
    }
    d = (int32_t)((a + (1u << 29)) >> 30);
    v = centre + (acc < 0 ? -d : d);
    return v < -8388608 ? -8388608 : v > 8388607 ? 8388607 : v;
}

/*
 * The output time lies `phase` / out past the window's centre frame,
 * w[span - 1]. A frame m frames before the centre is m + phase / out input
 * frames from that time, one m frames after it m - phase / out, each in the
 * table's entries `step` times that; `at` is step * phase / out, which
 * moves with `phase` exactly, with its remainder in `at_rest`. The filter
 * is applied to each frame's difference from the centre frame and the
 * centre frame added back, which gives the centre frame whatever the
 * coefficients of a phase sum to, so that a constant input comes out
 * unchanged.
 */
void resample_get(resampler_t *r, int32_t *frame)
{
    const int32_t (*w)[2] = (const int32_t (*)[2])(r->ring + r->head);
    int64_t acc[2];
    uint32_t fs, n;

    if (r->span == 0) {
        frame[0] = w[0][0];
        frame[1] = w[0][1];
    } else {
        fs = r->at;
        acc[0] = acc[1] = 0;
        w += r->span - 1u;
        n = resample_side(r, w, -1, fs, w[0], acc);
        n += resample_side(r, w + 1, 1, r->step - fs, w[0], acc);
        frame[0] = resample_out(w[0][0], acc[0], r->gain);
        frame[1] = resample_out(w[0][1], acc[1], r->gain);
        r->mults += (((fs | r->step) & 0xFFFFu) == 0 ? 2u : 3u) * n + 2u;
    }
    r->phase += r->in;
    r->at += r->move;
    r->at_rest += r->move_rest;
    if (r->at_rest >= r->out) {
        r->at_rest -= r->out;
        r->at++;
    }
    while (r->phase >= r->out) {
        r->phase -= r->out;
        r->at -= r->step;
        r->owed++;
    }
}
