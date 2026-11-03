/*
 * mkresample: write rach2/sys/audio/resample_coef.h, the coefficient table of
 * the kernel's resampler, a Kaiser-windowed sinc sampled RESAMPLE_STEPS times
 * between each two zero crossings, in Q30.
 *
 * Usage: cc -ansi -O2 -o mkresample tools/audio/mkresample.c -lm
 *        ./mkresample > rach2/sys/audio/resample_coef.h
 *
 * The filter is designed as FreeBSD's sys/tools/sound/feeder_rate_mkfilter.awk
 * designs feeder_rate's: Kaiser's formulas give the window's beta from the
 * stopband attenuation and its length from the attenuation and the width of
 * the transition band. The band is that of the lower of the two rates: the
 * passband ends at PASS of its Nyquist frequency and the stopband begins at
 * STOP, so a conversion down folds nothing into the band, and the sinc's
 * cutoff lies halfway between. The output is committed with the kernel's
 * sources, since the kernel also builds on Rach2 from its own directory, and
 * a libm of another system may round a coefficient differently.
 */
#include <math.h>
#include <stdio.h>

#define PASS  0.90         /* the passband's edge, in the lower rate's Nyquist */
#define STOP  1.00         /* the stopband's edge */
#define ATTEN 90.0         /* the design's stopband attenuation, in dB */
#define STEPS 256          /* table entries between two zero crossings */
#define ONE   1073741824.0 /* 2^30, the coefficients' unit */

/** The modified Bessel function of the first kind and order zero, by its
 * power series, which converges for every argument the window gives. */
static double bessel_i0(double x)
{
    double sum = 1.0, term = 1.0, q = x * x / 4.0;
    int k;

    for (k = 1; k < 200 && term > sum * 1e-17; k++) {
        term *= q / ((double)k * (double)k);
        sum += term;
    }
    return sum;
}

int main(void)
{
    double pi = 4.0 * atan(1.0);
    double cutoff = (PASS + STOP) / 2.0;
    double width = (STOP - PASS) / 2.0; /* the transition, in the rate's units */
    double beta = 0.1102 * (ATTEN - 8.7);
    double taps = (ATTEN - 7.95) / (14.36 * width) + 1.0;
    int zeros = (int)ceil(taps / 2.0);
    int i, n = zeros * STEPS;
    double x, u, h;
    char entry[24];

    printf("/*\n");
    printf(" * resample_coef.h: the resampler's coefficients (resample.c), written by\n");
    printf(" * tools/audio/mkresample.c; do not edit. A Kaiser-windowed sinc of cutoff\n");
    printf(" * %.3f of the lower rate's Nyquist frequency, beta %.4f, for %.0f dB\n", cutoff, beta,
           ATTEN);
    printf(" * from %.2f to %.2f of it, %d zero crossings each side and %d entries\n", PASS, STOP,
           zeros, STEPS);
    printf(" * between two, in Q30, ending in 0 at the last crossing.\n");
    printf(" */\n");
    printf("#define RESAMPLE_ZEROS %d\n", zeros);
    printf("#define RESAMPLE_STEPS %d\n\n", STEPS);
    printf("static const int32_t resample_coef[RESAMPLE_ZEROS * RESAMPLE_STEPS + 1] = {\n");
    for (i = 0; i <= n; i++) {
        x = (double)i / STEPS;
        u = x / zeros;
        h = i == 0 ? cutoff : sin(pi * cutoff * x) / (pi * x);
        h = i == n ? 0.0 : h * bessel_i0(beta * sqrt(1.0 - u * u)) / bessel_i0(beta);
        /* clang-format's layout, eight entries a line in columns of 11,
           so that the commit hook leaves the output as written. */
        sprintf(entry, "%ld,", (long)floor(h * ONE + 0.5));
        if (i % 8 == 7 || i == n) {
            printf("%s%s\n", i % 8 == 0 ? "    " : "", entry);
        } else {
            printf("%s%-11s ", i % 8 == 0 ? "    " : "", entry);
        }
    }
    printf("};\n");
    return 0;
}
