/**
 * Host test of term's chime, term/bell.c: the samples term computes against
 * the formula, worked out here another way, and a bell that finds no
 * /dev/dsp it can play on, which leaves the speaker to sound it.
 *
 * host-cflags: -I build/host/libtsm-4.8.0/src/tsm -I rach2/include
 *
 * bell.c is included whole, with TERM_DSP_PATH set to a path under the
 * test's own directory. term turns a phasor by a frame's angle and decay;
 * the reference here takes each sample's phase exactly from the integer
 * product of pitch and frame, and its sine and exponential from series in
 * long double, so that the two share no arithmetic but the constants.
 */
/* macOS declares mkdtemp(3) in <unistd.h> only with its own extensions. */
#define _DARWIN_C_SOURCE
#include <stdio.h>
#include <string.h>

static char dsp_path[4096];
#define TERM_DSP_PATH dsp_path
#include "../../term/bell.c"

static int fails;

#define CHECK(cond, what)                          \
    do {                                           \
        if (!(cond)) {                             \
            fails++;                               \
            fprintf(stderr, "  FAIL: %s\n", what); \
        }                                          \
    } while (0)

#define REF_PI 3.14159265358979323846264338327950288L

/** sin(x) for |x| at most pi, by its series. */
static long double ref_sin(long double x)
{
    long double sum = x, term = x;
    int n;

    for (n = 1; n < 40; n++) {
        term *= -x * x / ((2 * n) * (2 * n + 1));
        sum += term;
    }
    return sum;
}

/** exp(-x) for x from 0 to 10: the series of x / 64, raised to the 64th. */
static long double ref_exp_neg(long double x)
{
    long double sum = 1.0L, term = 1.0L, y = x / 64.0L;
    int n;

    for (n = 1; n < 30; n++) {
        term *= y / n;
        sum += term;
    }
    for (n = 0; n < 6; n++)
        sum *= sum;
    return 1.0L / sum;
}

/** The chime's sample `n` by the formula, in steps of 16 bits, unrounded. */
static long double ref_sample(unsigned n)
{
    static const unsigned hz[3] = {880, 1760, 2640};
    static const long double weight[3] = {1.0L, 0.35L, 0.12L};
    static const long double decay[3] = {6.0L, 9.0L, 14.0L};
    long double t = (long double)n / TERM_CHIME_RATE;
    long double v = 0.0L, x;
    unsigned i;

    for (i = 0; i < 3; i++) {
        /* The phase in turns is hz * n / rate; its whole turns go first. */
        x = 2.0L * REF_PI * (long double)((unsigned long)hz[i] * n % TERM_CHIME_RATE) /
            TERM_CHIME_RATE;
        if (x > REF_PI)
            x -= 2.0L * REF_PI;
        v += weight[i] * ref_exp_neg(decay[i] * t) * ref_sin(x);
    }
    v *= 0.35L / 1.47L;
    if (t < 0.004L)
        v *= t / 0.004L;
    return v * 32767.0L;
}

/** Every sample within one step of 16 bits of the formula. */
static void test_samples(void)
{
    int16_t *pcm = malloc(TERM_CHIME_FRAMES * sizeof(int16_t));
    long double d, worst = 0.0L;
    unsigned n, at = 0, peak = 0;
    char what[128];

    if (!pcm) {
        CHECK(0, "memory for the chime");
        return;
    }
    term_bell_chime(pcm);
    for (n = 0; n < TERM_CHIME_FRAMES; n++) {
        d = (long double)pcm[n] - ref_sample(n);
        if (d < 0)
            d = -d;
        if (d > worst) {
            worst = d;
            at = n;
        }
        if ((unsigned)(pcm[n] < 0 ? -pcm[n] : pcm[n]) > peak)
            peak = (unsigned)(pcm[n] < 0 ? -pcm[n] : pcm[n]);
    }
    snprintf(what, sizeof what, "samples within one step: worst %.3Lf at frame %u", worst, at);
    CHECK(worst <= 1.0L, what);
    CHECK(pcm[0] == 0, "the attack starts from silence");
    /* The partials' weights sum to 1.47, so the peak is below 0.35 of full
       scale and the partials, out of phase, keep it above a quarter. */
    snprintf(what, sizeof what, "the peak, %u, is near 0.35 of full scale", peak);
    CHECK(peak < 11469 && peak > 8000, what);
    fprintf(stderr, "term_bell: worst %.3Lf at frame %u, peak %u\n", worst, at, peak);
    free(pcm);
}

/** A bell with no /dev/dsp, or one that takes no OSS request, plays
 * nothing, holds nothing open and asks for the speaker. */
static void test_no_dsp(const char *dir)
{
    struct term_bell b;
    int fd;

    term_bell_init(&b);
    CHECK(b.pcm != NULL, "the chime computed at start");
    snprintf(dsp_path, sizeof dsp_path, "%s/missing", dir);
    CHECK(term_bell_ring(&b) == -ENOENT, "a missing /dev/dsp gives ENOENT");
    CHECK(b.fd < 0 && term_bell_timeout(&b) < 0 && term_bell_events(&b) == 0,
          "nothing waits after a missing /dev/dsp");

    snprintf(dsp_path, sizeof dsp_path, "%s/plain", dir);
    fd = open(dsp_path, O_WRONLY | O_CREAT | O_TRUNC, 0600);
    CHECK(fd >= 0, "a plain file for /dev/dsp");
    if (fd >= 0)
        close(fd);
    CHECK(term_bell_ring(&b) < 0, "a file that takes no OSS request is refused");
    CHECK(b.fd < 0 && term_bell_timeout(&b) < 0, "nothing stays open after a refusal");
    unlink(dsp_path);
    term_bell_close(&b);
    CHECK(b.pcm == NULL, "the chime freed");
}

int main(void)
{
    char dir[4096];

    snprintf(dir, sizeof dir, "%s/term_bell_dir.XXXXXX",
             getenv("TMPDIR") ? getenv("TMPDIR") : "/tmp");
    if (!mkdtemp(dir)) {
        perror("term_bell: mkdtemp");
        return 1;
    }
    test_samples();
    test_no_dsp(dir);
    rmdir(dir);
    if (fails) {
        fprintf(stderr, "term_bell: %d failures\n", fails);
        return 1;
    }
    printf("term_bell: ok\n");
    return 0;
}
