/**
 * The resampler (audio/resample.c): sines in the passband kept within 0.1 dB
 * and their images and aliases 80 dB down, sines above the lower rate's
 * Nyquist frequency attenuated by 80 dB, a constant kept exactly, a full
 * scale step held to 24 bits, the fixed-point output against the filter
 * computed in double, the history across blocks, equal rates copied, and the cost.
 *
 * host-deps: rach2/sys/lib/kstring.c rach2/sys/lib/kctypes.c
 *
 * The test includes resample.c whole. Output frame k lies at input time
 * k * in / out - span, so a sine put at the input comes out as the same
 * sine sampled at the output rate, later by the span; a row fits a sine of
 * the frequency put, at any phase, to the output. The host's libm is not
 * linked, so the sine is a series here.
 */
#include "audio/resample.c"

extern int printf(const char *format, ...);

#define FULL   8388607.0 /* 2^23 - 1, full scale of 24 bits */
#define LEN    48000     /* input frames of a row, at most */
#define SETTLE 1200      /* output frames left out at each end of a row */
#define PI     3.14159265358979323846

static int failures;
static const char *row; /* the row under way, named in a failure */
static int32_t in[LEN][2];
static int32_t out[LEN * RESAMPLE_RATIO][2];
static resampler_t conv;

/** Count and name a failure when `ok` is false. */
static void check(int ok, const char *what)
{
    if (!ok) {
        printf("resample-hosttest: fail: %s: %s\n", row, what);
        failures++;
    }
}

/** sin(x), by its series after reducing `x` to within pi of 0. */
static double tsin(double x)
{
    double pi = PI, term, sum;
    long turns = (long)(x / (2.0 * pi));
    int k;

    x -= (double)turns * 2.0 * pi;
    if (x > pi)
        x -= 2.0 * pi;
    if (x < -pi)
        x += 2.0 * pi;
    term = sum = x;
    for (k = 1; k < 30; k++) {
        term *= -x * x / ((2.0 * k) * (2.0 * k + 1.0));
        sum += term;
    }
    return sum;
}

/** The sine of `amp` and frequency `f` at frame `n` of rate `rate`; the
 * product n * f is reduced exactly first, so a long row loses no phase. */
static double sine(double amp, uint32_t f, uint32_t n, uint32_t rate)
{
    uint32_t m = (uint32_t)((uint64_t)n * f % rate);

    return amp * tsin(2.0 * PI * (double)m / (double)rate);
}

/** Round `v` to the nearest integer, a half away from zero. */
static int32_t iround(double v)
{
    return (int32_t)(v < 0 ? v - 0.5 : v + 0.5);
}

/** Fill `n` input frames with a sine of `fl` Hz on the left and one of `fr`
 * on the right, of amplitude `amp`, at `rate`. */
static void fill_sines(uint32_t n, uint32_t rate, uint32_t fl, uint32_t fr, double amp)
{
    uint32_t i;

    for (i = 0; i < n; i++) {
        in[i][0] = iround(sine(amp, fl, i, rate));
        in[i][1] = iround(sine(amp, fr, i, rate));
    }
}

/** The output frames left out at each end of `n`: those whose window
 * reaches past the input, and more, so that a row measures the sine alone. */
static uint32_t settle(uint32_t n)
{
    return n / 4u < SETTLE ? n / 4u : SETTLE;
}

/** Convert `nin` input frames from `rate_in` to `rate_out`, putting each
 * frame as the converter owes one and silence past the input, into `out`;
 * gives the output frames made, those whose time lies within the input. */
static uint32_t convert(uint32_t nin, uint32_t rate_in, uint32_t rate_out)
{
    uint32_t p = 0, k, n = (uint32_t)((uint64_t)nin * rate_out / rate_in);
    int32_t zero[2] = {0, 0};

    resample_init(&conv, rate_in, rate_out);
    for (k = 0; k < n; k++) {
        while (conv.owed > 0) {
            resample_put(&conv, p < nin ? in[p] : zero);
            p++;
        }
        resample_get(&conv, out[k]);
    }
    return n;
}

/** sin(2 pi f k / rate), or its cosine with `cosine`; k * f is reduced
 * exactly first. */
static double osc(uint32_t f, uint32_t k, uint32_t rate, int cosine)
{
    double m = (double)(uint32_t)((uint64_t)k * f % rate);

    return tsin(2.0 * PI * m / (double)rate + (cosine ? PI / 2.0 : 0.0));
}

/* A sine fitted to output frames: its gain and the power left beside it. */
struct fit {
    double gain; /* the amplitude relative to the one expected, squared */
    double rest; /* the power left once the sine is taken out, over its own */
};

/**
 * The sine of `f` Hz in channel `ch` of output frames `from` to `to` at
 * `rate`: its amplitude relative to `amp`, squared, and the power of what is
 * left once it is taken out, relative to the sine's. The sine and its cosine
 * are fitted by least squares, so the frames need not hold whole periods.
 */
static struct fit measure(uint32_t from, uint32_t to, int ch, uint32_t f, uint32_t rate, double amp)
{
    struct fit fit;
    double s, c, y, ss = 0, cc = 0, sc = 0, ys = 0, yc = 0, a, b, det, e, power = 0;
    uint32_t k;

    for (k = from; k < to; k++) {
        s = osc(f, k, rate, 0);
        c = osc(f, k, rate, 1);
        y = out[k][ch];
        ss += s * s;
        cc += c * c;
        sc += s * c;
        ys += y * s;
        yc += y * c;
    }
    det = ss * cc - sc * sc;
    a = (ys * cc - yc * sc) / det;
    b = (yc * ss - ys * sc) / det;
    for (k = from; k < to; k++) {
        e = out[k][ch] - a * osc(f, k, rate, 0) - b * osc(f, k, rate, 1);
        power += e * e;
    }
    fit.gain = (a * a + b * b) / (amp * amp);
    fit.rest = power / (double)(to - from) / ((a * a + b * b) / 2.0 + 1e-30);
    return fit;
}

/* The conversions the rows make: the mix's from each rate /dev/dsp takes
 * and back, and the extremes of the rates granted. */
static const uint32_t rates[][2] = {
    {96000, 48000}, {192000, 48000}, {44100, 48000}, {48000, 44100}, {8000, 48000},
    {11025, 48000}, {22050, 44100},  {48000, 96000}, {48000, 8000},  {32000, 48000},
};
#define NRATES (sizeof(rates) / sizeof(rates[0]))

/** The input frames of a row at `rate`: a quarter of a second, at most LEN. */
static uint32_t row_len(uint32_t rate)
{
    return rate / 4u < LEN ? rate / 4u : LEN;
}

/**
 * Each conversion keeps a sine at 1 kHz, at half and at 0.85 of the lower
 * rate's Nyquist frequency within 0.1 dB, and what it adds, its images
 * above the band and its aliases, is at least 80 dB below the sine.
 */
static void row_passband(void)
{
    static const double at[] = {0.0, 0.5, 0.85};
    uint32_t i, j, fin, fout, low, f, n;
    double gain, rest, worst = 0, lo = 1, hi = 1;
    struct fit fit;

    row = "passband";
    for (i = 0; i < NRATES; i++) {
        fin = rates[i][0];
        fout = rates[i][1];
        low = fin < fout ? fin : fout;
        for (j = 0; j < sizeof(at) / sizeof(at[0]); j++) {
            f = j == 0 ? 1000u : (uint32_t)(at[j] * low / 2.0);
            fill_sines(row_len(fin), fin, f, f, FULL / 2.0);
            n = convert(row_len(fin), fin, fout);
            fit = measure(settle(n), n - settle(n), 0, f, fout, FULL / 2.0);
            gain = fit.gain;
            rest = fit.rest;
            check(gain > 0.97724 && gain < 1.02329, "a sine in the band is kept within 0.1 dB");
            check(rest < 1e-8, "what the conversion adds is 80 dB below the sine");
            lo = gain < lo ? gain : lo;
            hi = gain > hi ? gain : hi;
            worst = rest > worst ? rest : worst;
        }
    }
    printf("resample-hosttest: passband: power from %.6f to %.6f, rest at most %.3g\n", lo, hi,
           worst);
}

/** The power of channel `ch` of output frames `from` to `to`, relative to a
 * full scale sine's. */
static double power_of(uint32_t from, uint32_t to, int ch)
{
    double p = 0;
    uint32_t k;

    for (k = from; k < to; k++)
        p += (double)out[k][ch] * out[k][ch];
    return p / (double)(to - from) / (FULL * FULL / 2.0);
}

/**
 * A conversion down attenuates a sine at the lower rate's Nyquist frequency
 * and above it by at least 80 dB: what comes out, the alias, has a power
 * under 10^-8 of the sine's. 96 kHz to 48 kHz is the tone the audio suite
 * plays at 30 kHz, which would fold to 18 kHz.
 */
static void row_stopband(void)
{
    static const uint32_t cases[][3] = {
        {96000, 48000, 24000},  {96000, 48000, 25000},  {96000, 48000, 30000},
        {96000, 48000, 47000},  {192000, 48000, 24000}, {192000, 48000, 60000},
        {192000, 48000, 95000}, {48000, 44100, 22050},  {48000, 44100, 23000},
        {48000, 8000, 4000},    {48000, 8000, 13000},   {44100, 22050, 11025},
    };
    uint32_t i, n;
    double p, worst = 0;

    row = "stopband";
    for (i = 0; i < sizeof(cases) / sizeof(cases[0]); i++) {
        fill_sines(row_len(cases[i][0]), cases[i][0], cases[i][2], cases[i][2], FULL);
        n = convert(row_len(cases[i][0]), cases[i][0], cases[i][1]);
        p = power_of(settle(n), n - settle(n), 0);
        check(p < 1e-8, "a sine above the lower Nyquist frequency is 80 dB down");
        worst = p > worst ? p : worst;
    }
    printf("resample-hosttest: stopband: power at most %.3g\n", worst);
}

/** The first output frame whose taps all lie at or after input frame 0:
 * its time, k * fin / fout - span, at least span. */
static uint32_t pure_from(uint32_t fin, uint32_t fout)
{
    return (uint32_t)((uint64_t)(2u * conv.span + 1u) * fout / fin) + 1u;
}

/** The output frame past the last whose taps all lie within `nin` input
 * frames: its time below nin - span, its window's last frame nin - 1. */
static uint32_t pure_to(uint32_t nin, uint32_t fin, uint32_t fout)
{
    return (uint32_t)((uint64_t)(nin - 1u) * fout / fin);
}

/** A constant comes out exactly once the window holds it, at every ratio
 * and at both ends of 24 bits. */
static void row_dc(void)
{
    static const int32_t level[] = {1234567, -8388608, 8388607, -1, 0};
    static const uint32_t extra[][2] = {
        {192000, 1000}, {1000, 192000}, {384000, 48000}, {44100, 44100}};
    uint32_t i, j, k, n, fin, fout;
    int ok;

    row = "dc";
    for (i = 0; i < NRATES + sizeof(extra) / sizeof(extra[0]); i++) {
        fin = i < NRATES ? rates[i][0] : extra[i - NRATES][0];
        fout = i < NRATES ? rates[i][1] : extra[i - NRATES][1];
        for (j = 0; j < sizeof(level) / sizeof(level[0]); j++) {
            for (k = 0; k < row_len(fin); k++)
                in[k][0] = in[k][1] = level[j];
            n = convert(row_len(fin), fin, fout);
            ok = 1;
            (void)n;
            for (k = pure_from(fin, fout); k < pure_to(row_len(fin), fin, fout); k++)
                ok &= out[k][0] == level[j] && out[k][1] == level[j];
            check(ok, "a constant is kept exactly");
        }
    }
}

/**
 * Output frame `k` of channel `ch` from `nin` input frames, as the
 * converter's filter gives it computed in double: each tap's distance from
 * the output time in exact arithmetic, its coefficient interpolated in the
 * table, and the sum held to 24 bits.
 */
static double reference(uint32_t k, int ch, uint32_t nin, uint32_t fin, uint32_t fout)
{
    int64_t t = (int64_t)k * fin - (int64_t)conv.span * fout, n;
    int64_t i = (t - (t < 0 ? (int64_t)fout - 1 : 0)) / (int64_t)fout;
    double phase = (double)(t - i * (int64_t)fout) / fout, low = fin < fout ? fin : fout;
    double d, pos, c, sum = 0, centre, x;
    long e;

    if (low * RESAMPLE_RATIO < fin)
        low = (double)((fin + RESAMPLE_RATIO - 1u) / RESAMPLE_RATIO);
    centre = i >= 0 && i < (int64_t)nin ? in[i][ch] : 0;
    for (n = i - (int64_t)conv.span + 1; n <= i + (int64_t)conv.span; n++) {
        x = n >= 0 && n < (int64_t)nin ? in[n][ch] : 0;
        d = (double)(n - i) - phase;
        pos = (d < 0 ? -d : d) * low / fin * RESAMPLE_STEPS;
        e = (long)pos;
        if (e >= RESAMPLE_ZEROS * RESAMPLE_STEPS)
            continue;
        c = resample_coef[e] + (resample_coef[e + 1] - resample_coef[e]) * (pos - e);
        sum += c * (x - centre);
    }
    sum = centre + sum / 1073741824.0 * (low < fin ? low / fin : 1.0);
    return sum < -8388608.0 ? -8388608.0 : sum > FULL ? FULL : sum;
}

/**
 * A step between the two ends of 24 bits, and a sine at full scale, give
 * the filter's output computed in double to within eight steps of 24 bits,
 * every frame: the sums do not overflow, the overshoot of the step is held
 * to 24 bits, and the rounding of the 16.16 step, which moves a far tap by
 * up to a few thousandths of a table entry, loses no more.
 */
static void row_reference(void)
{
    uint32_t i, k, n, len, fin, fout;
    double v, worst = 0;

    row = "reference";
    for (i = 0; i < NRATES; i++) {
        fin = rates[i][0];
        fout = rates[i][1];
        len = row_len(fin) / 4u;
        for (k = 0; k < len; k++) {
            in[k][0] = (k / 97u) % 2u ? 8388607 : -8388608;
            in[k][1] = iround(sine(FULL, fin / 3u, k, fin));
        }
        n = convert(len, fin, fout);
        for (k = 0; k < n; k++) {
            v = reference(k, 0, len, fin, fout) - out[k][0];
            worst = v < 0 ? (-v > worst ? -v : worst) : (v > worst ? v : worst);
            check(v > -8.0 && v < 8.0, "the step is the filter's in double");
            v = reference(k, 1, len, fin, fout) - out[k][1];
            worst = v < 0 ? (-v > worst ? -v : worst) : (v > worst ? v : worst);
            check(v > -8.0 && v < 8.0, "the sine is the filter's in double");
        }
    }
    printf("resample-hosttest: reference: within %.3f of the filter in double\n", worst);
}

/**
 * Converting in blocks of every size, each taking the frames
 * resample_need says, gives the frames of one long conversion: the history
 * carries across blocks, and each block takes exactly what it was said to.
 */
static void row_blocks(void)
{
    static int32_t whole[LEN][2];
    uint32_t i, k, n, p, b, want, size, len, before, seed = 1;
    int32_t zero[2] = {0, 0};
    int ok;

    row = "blocks";
    for (i = 0; i < NRATES; i++) {
        len = row_len(rates[i][0]);
        fill_sines(len, rates[i][0], 997u, 3001u, FULL / 3.0);
        n = convert(len, rates[i][0], rates[i][1]);
        n = n < LEN ? n : LEN;
        k_memcpy(whole, out, n * sizeof(out[0]));
        resample_init(&conv, rates[i][0], rates[i][1]);
        ok = 1;
        for (k = 0, p = 0; k < n; k += size) {
            seed = seed * 1103515245u + 12345u;
            size = 1u + (seed >> 16) % 300u;
            size = size < n - k ? size : n - k;
            want = resample_need(&conv, size);
            before = p;
            for (b = 0; b < size; b++) {
                for (; conv.owed > 0; p++)
                    resample_put(&conv, p < len ? in[p] : zero);
                resample_get(&conv, out[k + b]);
            }
            ok &= p - before == want;
            for (b = 0; b < size; b++)
                ok &= out[k + b][0] == whole[k + b][0] && out[k + b][1] == whole[k + b][1];
        }
        check(ok, "blocks give the frames of one conversion");
    }
}

/** At equal rates every frame goes through unchanged, one put for each
 * frame made, and no multiply is made. */
static void row_copy(void)
{
    uint32_t k, n;
    int ok = 1;

    row = "copy";
    for (k = 0; k < 4800u; k++) {
        in[k][0] = (int32_t)(k * 2654435761u) >> 8;
        in[k][1] = -in[k][0] - 1;
    }
    n = convert(4800u, 48000u, 48000u);
    for (k = 0; k < n; k++)
        ok &= out[k][0] == in[k][0] && out[k][1] == in[k][1];
    check(ok && n == 4800u, "equal rates copy each frame");
    check(conv.mults == 0, "a copy makes no multiply");
    resample_init(&conv, 48000u, 48000u);
    check(resample_need(&conv, 256u) == 256u, "a copy needs a frame for a frame");
}

/** The multiplies of a stereo frame at 96 kHz to 48 kHz, and at 8 kHz and
 * 44.1 kHz to 48 kHz, for the cost the audio Readme gives. */
static void row_cost(void)
{
    static const uint32_t cases[][2] = {{96000, 48000}, {44100, 48000}, {8000, 48000}};
    uint32_t i, n;

    row = "cost";
    for (i = 0; i < sizeof(cases) / sizeof(cases[0]); i++) {
        fill_sines(9600u, cases[i][0], 1000u, 1000u, FULL / 2.0);
        n = convert(9600u, cases[i][0], cases[i][1]);
        printf("resample-hosttest: cost: %u Hz to %u Hz: %u multiplies a stereo frame\n",
               cases[i][0], cases[i][1], (conv.mults + n / 2u) / n);
        check(conv.span <= RESAMPLE_TAPS / 2u, "the window fits");
    }
}

int main(void)
{
    row_passband();
    row_stopband();
    row_dc();
    row_reference();
    row_blocks();
    row_copy();
    row_cost();
    printf("resample-hosttest: result: %s\n", failures ? "fail" : "pass");
    return failures ? 1 : 0;
}
