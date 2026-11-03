/*
 * resamplebench: time the kernel's resampler (rach2/sys/audio/resample.c)
 * per stereo frame at 96 to 48 kHz and at 44.1 to 48 kHz, beside FreeBSD's
 * feeder_rate's two sinc loops over the same filter, built as the kernel is
 * built and run on the host. tools/audio/resamplebench.sh builds and runs it.
 *
 * The program is freestanding, as the kernel is: it includes resample.c
 * whole and reaches Linux by its system calls alone, so that the cross
 * compilers that build the kernel, at no optimization and with the
 * kernel's code generation flags, build it too.
 *
 * The two other rows are feeder_rate.c's z_feed_sinc_polyphase and
 * z_feed_sinc (FreeBSD, sys/dev/sound/pcm/feeder_rate.c, BSD license),
 * written again here over this filter's table, window and gain, with
 * FreeBSD's order of work: each channel in turn, a sample times its
 * coefficient into 64 bits, and the sum scaled once. The polyphase loop
 * reads each phase's coefficients from a table made at the start, as
 * FreeBSD does whenever the phases times the taps fit its limit, which
 * both of these ratios do; the other interpolates each coefficient between
 * two of the table's entries with a 32-bit product of their difference,
 * which FreeBSD keeps in a second table.
 */
#include "audio/resample.c"

#if defined(__x86_64__)
#define SYS_WRITE 1
#define SYS_EXIT  60
#define SYS_CLOCK 228
#else
#define SYS_WRITE 4
#define SYS_EXIT  1
#define SYS_CLOCK 265
#endif
#define CLOCK_MONOTONIC 1

#define SECONDS 2u     /* seconds of output a pass makes */
#define PASSES  5u     /* passes of a row, of which the fastest counts */
#define INPUT   96000u /* input frames, played round as often as needed */
#define BLOCK   256u   /* output frames made at a time, as the mix makes */
#define PHASES  320u   /* the most phases the polyphase row takes */
#define SHIFT   16     /* the interpolating row's table position, 16.16 */

static int32_t input[INPUT][2];
static int32_t sink;
static resampler_t conv;

/* The polyphase row's coefficients, each phase's taps of the window, oldest
 * frame first, and the interpolating row's differences of the table. */
static int32_t poly[PHASES][2 * (2 * RESAMPLE_ZEROS + 1)];
static int32_t dcoeff[RESAMPLE_ZEROS * RESAMPLE_STEPS + 1];

/** System call `n` of Linux with three arguments. */
static long sys3(long n, long a, long b, long c)
{
    long r;

#if defined(__x86_64__)
    __asm__ volatile("syscall" : "=a"(r) : "a"(n), "D"(a), "S"(b), "d"(c) : "rcx", "r11", "memory");
#else
    __asm__ volatile("int $0x80" : "=a"(r) : "a"(n), "b"(a), "c"(b), "d"(c) : "memory");
#endif
    return r;
}

/** Fill `n` bytes at `p` with `c`, for resample.c. */
void *k_memset(void *p, int c, size_t n)
{
    uint8_t *b = p;

    while (n-- > 0) {
        *b++ = (uint8_t)c;
    }
    return p;
}

/** The monotonic clock, in nanoseconds. */
static uint64_t now_ns(void)
{
    long ts[2];

    sys3(SYS_CLOCK, CLOCK_MONOTONIC, (long)ts, 0);
    return (uint64_t)ts[0] * 1000000000u + (uint64_t)ts[1];
}

/** Write the string `s` to standard output. */
static void put(const char *s)
{
    const char *e = s;

    while (*e) {
        e++;
    }
    sys3(SYS_WRITE, 1, (long)s, e - s);
}

/** Write `v` in decimal, with a point before its last `dec` digits. */
static void putnum(uint32_t v, int dec)
{
    char buf[16];
    int i = 15, n = 0;

    buf[i] = 0;
    do {
        if (n == dec && dec > 0) {
            buf[--i] = '.';
        }
        buf[--i] = (char)('0' + v % 10u);
        v /= 10u;
        n++;
    } while (v != 0 || n <= dec);
    put(buf + i);
}

/** The table's coefficient at position `pos`, 16.16 in entries,
 * interpolated as resample.c interpolates it, and 0 past its end. */
static int32_t coef_at(uint32_t pos)
{
    const int32_t *t;

    if (pos >= RESAMPLE_END) {
        return 0;
    }
    t = resample_coef + (pos >> 16);
    return t[0] + (int32_t)((int64_t)(t[1] - t[0]) * (int32_t)(pos & 0xFFFFu) / 65536);
}

/**
 * Make the polyphase row's table for `r`: for each `phase` of the output
 * time, a multiple of `g` below `out`, the coefficient of each frame of the
 * window, as resample_get weighs the frames for that phase.
 */
static void make_poly(const resampler_t *r, uint32_t g)
{
    uint32_t p, k, at, rest, span = r->span;

    for (p = 0; p * g < r->out; p++) {
        at = resample_div((uint64_t)r->step * (p * g), r->out, &rest);
        for (k = 0; k < span; k++) {
            poly[p][span - 1u - k] = coef_at(at + k * r->step);
            poly[p][span + k] = coef_at(r->step - at + k * r->step);
        }
    }
}

/** The difference of each entry of the table from the next, FreeBSD's
 * z_dcoeff. */
static void make_dcoeff(void)
{
    uint32_t i;

    for (i = 0; i < RESAMPLE_ZEROS * RESAMPLE_STEPS; i++) {
        dcoeff[i] = resample_coef[i + 1] - resample_coef[i];
    }
    dcoeff[i] = 0;
}

/** A channel's sum `v` of the window, Q30, scaled by `gain` as
 * resample_out scales it, and held to 24 bits, as FreeBSD's Z_SCALE_24 and
 * pcm_clamp do. */
static int32_t scale_out(int64_t v, uint32_t gain)
{
    if (gain != 0) {
        v = (v >> 16) * (int64_t)(gain >> 16) >> 30;
    } else {
        v >>= 30;
    }
    return v < -8388608 ? -8388608 : v > 8388607 ? 8388607 : (int32_t)v;
}

/** One output frame of the polyphase row from the window `w`, as
 * z_feed_sinc_polyphase makes it, a channel at a time. */
static void get_poly(resampler_t *r, uint32_t g, int32_t *frame)
{
    const int32_t *w = r->ring[r->head], *p, *c;
    int64_t v;
    uint32_t i, ch;

    for (ch = 0; ch < 2; ch++) {
        v = 0;
        p = w + ch;
        c = poly[r->phase / g];
        for (i = r->span; i != 0; i--) {
            v += (int64_t)*p * *c;
            c++;
            p += 2;
            v += (int64_t)*p * *c;
            c++;
            p += 2;
        }
        frame[ch] = scale_out(v, r->gain);
    }
}

/** One side of a channel's window for the interpolating row, as
 * z_feed_sinc's _Z_SINC_ACCUMULATE goes: the frames from `p` by `dir`,
 * the position `z` in table entries from 0 by `dy`, 16.16. */
static int64_t side_interp(const int32_t *p, int dir, uint32_t z, uint32_t dy, uint32_t n)
{
    int64_t v = 0;
    uint32_t c = 0;
    int32_t coeff;

    for (; n != 0; n--) {
        c += z >> SHIFT;
        z &= (1u << SHIFT) - 1u;
        coeff = resample_coef[c] + (int32_t)((z >> 9) * dcoeff[c] >> 7);
        v += (int64_t)*p * coeff;
        z += dy;
        p += 2 * dir;
    }
    return v;
}

/** One output frame of the interpolating row from the window, as
 * z_feed_sinc makes it, a channel at a time. */
static void get_interp(resampler_t *r, int32_t *frame)
{
    const int32_t *w = r->ring[r->head + r->span - 1u];
    uint32_t ch, n = r->span - 1u;
    int64_t v;

    for (ch = 0; ch < 2; ch++) {
        v = side_interp(w + ch, -1, r->at, r->step, n);
        v += side_interp(w + 2 + ch, 1, r->step - r->at, r->step, n);
        frame[ch] = scale_out(v, r->gain);
    }
}

/** Move `r` on by one output frame, as resample_get does after its sum. */
static void advance(resampler_t *r)
{
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

/**
 * Convert SECONDS of output from `in` to `out` frames per second through
 * the loop `how`, 0 for the kernel's, 1 for the polyphase row and 2 for the
 * interpolating one, PASSES times, and give the fastest pass's nanoseconds
 * a stereo frame, in tenths.
 */
static uint32_t row(uint32_t in, uint32_t out, uint32_t g, int how)
{
    uint64_t best = ~(uint64_t)0, t;
    uint32_t pass, made, i, at = 0;
    int32_t frame[2];

    for (pass = 0; pass < PASSES; pass++) {
        resample_init(&conv, in, out);
        if (how == 1) {
            make_poly(&conv, g);
        }
        t = now_ns();
        for (made = 0; made < SECONDS * out; made += BLOCK) {
            (void)resample_need(&conv, BLOCK);
            for (i = 0; i < BLOCK; i++) {
                while (conv.owed > 0) {
                    resample_put(&conv, input[at]);
                    at = at + 1u == INPUT ? 0 : at + 1u;
                }
                if (how == 0) {
                    resample_get(&conv, frame);
                } else {
                    if (how == 1) {
                        get_poly(&conv, g, frame);
                    } else {
                        get_interp(&conv, frame);
                    }
                    advance(&conv);
                }
                sink += frame[0] ^ frame[1];
            }
        }
        t = now_ns() - t;
        if (t < best) {
            best = t;
        }
    }
    return resample_div(best * 10u, SECONDS * out, NULL);
}

/** Time the three loops at `in` to `out`, whose rates share `g`. */
static void ratio(uint32_t in, uint32_t out, uint32_t g)
{
    static const char *names[] = {"rach2", "feeder_rate polyphase", "feeder_rate interpolating"};
    uint32_t ns, base = 0;
    int how;

    for (how = 0; how < 3; how++) {
        ns = row(in, out, g, how);
        if (how == 0) {
            base = ns;
        }
        put("resamplebench: ");
        putnum(in, 0);
        put(" to ");
        putnum(out, 0);
        put(" Hz: ");
        put(names[how]);
        put(": ");
        putnum(ns, 1);
        put(" ns a frame, ");
        putnum(ns * (out / 1000u) / 1000u, 1);
        put(" ms a second");
        if (how > 0) {
            put(", rach2 ");
            putnum(base * 100u / ns, 2);
            put(" times");
        }
        put("\n");
    }
}

/** Fill the input with noise of 24 bits, time both ratios and exit 0. */
static int bench(void)
{
    uint32_t i, x = 1;

    for (i = 0; i < INPUT; i++) {
        x = x * 1664525u + 1013904223u;
        input[i][0] = (int32_t)x >> 9;
        x = x * 1664525u + 1013904223u;
        input[i][1] = (int32_t)x >> 9;
    }
    make_dcoeff();
    ratio(96000u, 48000u, 48000u);
    ratio(44100u, 48000u, 300u);
    return 0;
}

/** The program's entry, which Linux enters with no C runtime. */
void _start(void)
{
    sys3(SYS_EXIT, bench(), 0, 0);
}
