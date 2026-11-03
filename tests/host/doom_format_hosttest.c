/**
 * Compare the Doom port's printf formatter with the host's snprintf over
 * the conversions, flags, widths and precisions Doom's sources use, and
 * floats of every magnitude.
 *
 * host-deps: ports/doom/format.c
 * host-cflags: -iquote ports/doom
 */

#include <float.h>
#include <limits.h>
#include <math.h>
#include <stdarg.h>
#include <stdint.h>
#include <stdio.h>
#include <string.h>

#include "format.h"

static int fails;
static int rows;

/** Format with the port's formatter, as snprintf takes its arguments. */
static int port_snprintf(char *buf, size_t size, const char *fmt, ...)
{
    va_list ap;
    int n;

    va_start(ap, fmt);
    n = format_vsnprintf(buf, size, fmt, ap);
    va_end(ap);
    return n;
}

/**
 * Report a row whose result `got`, of return `gn`, differs from `want`,
 * of return `wn`. `what` names the format and its argument.
 */
static void same(const char *what, const char *got, int gn, const char *want, int wn)
{
    rows++;
    if (gn != wn || strcmp(got, want) != 0) {
        fprintf(stderr, "doom_format_hosttest: %s: got \"%s\" (%d), want \"%s\" (%d)\n", what, got,
                gn, want, wn);
        fails++;
    }
}

/* The rows of one argument type: the port's result against the host's. */
#define ROW(fmt, arg)                                          \
    do {                                                       \
        char got_[1200], want_[1200];                          \
        int gn_ = port_snprintf(got_, sizeof(got_), fmt, arg); \
        int wn_ = snprintf(want_, sizeof(want_), fmt, arg);    \
        same(fmt, got_, gn_, want_, wn_);                      \
    } while (0)

/* A row whose expected result is glibc's, which another host may not give. */
#define FIXED(fmt, arg, want)                                  \
    do {                                                       \
        char got_[256];                                        \
        int gn_ = port_snprintf(got_, sizeof(got_), fmt, arg); \
        same(fmt, got_, gn_, want, (int)strlen(want));         \
    } while (0)

static const char *const int_formats[] = {
    "%d",    "%i",     "%u",    "%x",    "%X",     "%o",     "%02i",  "%7i",    "%3i",  "%.2d",
    "%2.2d", "%.3d",   "%5d",   "%-5d|", "%05d",   "%+d",    "% d",   "%-+6d|", "%.0d", "%5.0d|",
    "%#x",   "%#X",    "%#o",   "%#.3o", "%08.3d", "%-08d|", "%#08x", "%hd",    "%hhd", "%hu",
    "%hhx",  "%10.5i", "% 05d", "%+.4i", "%.10u",  "%#5o",   "%c",
};

static const char *const long_formats[] = {
    "%li", "%ld", "%lu", "%lx", "%-12ld|", "%012li", "%+.3ld", "% li", "%lo",
};

static const char *const llong_formats[] = {
    "%lld", "%llu", "%llx", "%llX", "%llo", "%20lld", "%-22lli|", "%.25lld", "%+lld", "%#llx",
};

static const char *const str_formats[] = {
    "%s", "%.8s", "%79s", "%-10s|", "%10s", "%.0s|", "%.3s", "%-4.2s|", "%12.5s|",
};

static const char *const float_formats[] = {
    "%f",       "%e",        "%g",    "%F",      "%E",       "%G",      "%.0f",  "%.1f",
    "%.2f",     "%.3f",      "%.10f", "%.20f",   "%.0e",     "%.1e",    "%.3e",  "%.12e",
    "%.17e",    "%.0g",      "%.1g",  "%.2g",    "%.3g",     "%.10g",   "%.17g", "%#g",
    "%#.0f",    "%#.0e",     "%#.3g", "%10.3f|", "%-10.3f|", "%010.3f", "%+f",   "% f",
    "%+012.4e", "%-+14.5g|", "%015g", "% .3e",   "%#.10g",   "%.40f",   "%.60e", "%20.10g|",
};

static const double float_values[] = {
    0.0,
    -0.0,
    1.0,
    -1.0,
    0.5,
    1.5,
    2.5,
    -2.5,
    0.125,
    0.375,
    9.5,
    10.5,
    0.05,
    0.15,
    0.25,
    0.35,
    0.45,
    99.995,
    999999.5,
    1e-5,
    1.5e-5,
    0.0001,
    0.00012345,
    123456.0,
    1234567.0,
    12345678.9,
    45.6935,
    3.14159265358979,
    2.718281828459045,
    1e15,
    1e16,
    1e17,
    123456789012345678.0,
    1e21,
    1e22,
    1e23,
    1e100,
    1e300,
    1.7976931348623157e308,
    2.2250738585072014e-308,
    4.9406564584124654e-324,
    1e-300,
    1e-10,
    0.1,
    0.2,
    0.3,
    0.7,
    0.9999995,
    9.9999995,
    0.999,
    999.999,
    5e-324,
    6.103515625e-05,
    1.0 / 3.0,
    2.0 / 3.0,
    45.69354838709677,
    100.0,
    1000000.0,
    8388608.5,
    4503599627370495.5,
    9007199254740993.0,
};

/** The next of a fixed sequence of 64-bit numbers (xorshift64*). */
static uint64_t next_random(void)
{
    static uint64_t x = 0x9e3779b97f4a7c15ULL;

    x ^= x >> 12;
    x ^= x << 25;
    x ^= x >> 27;
    return x * 0x2545f4914f6cdd1dULL;
}

/** Compare every float format over `v`. */
static void float_rows(double v)
{
    size_t f;

    for (f = 0; f < sizeof(float_formats) / sizeof(float_formats[0]); f++) {
        ROW(float_formats[f], v);
    }
}

/** The integer, string, pointer and character rows. */
static void integer_rows(void)
{
    static const int ints[] = {0,    1,      -1,    7,      42,      -42,       255,
                               1000, -32768, 65535, 123456, -987654, INT32_MAX, INT32_MIN};
    static const long longs[] = {0, 1, -1, 2147483647L, -2147483647L - 1, LONG_MAX, LONG_MIN};
    static const long long llongs[] = {
        0, 1, -1, 4294967296LL, -4294967296LL, LLONG_MAX, LLONG_MIN, 1234567890123LL};
    static const char *const strs[] = {
        "",
        "a",
        "STCFN",
        "lumpname12",
        "E1M1",
        "a string of more than eighty characters, which the width of %79s leaves as it is"};
    size_t f, v;
    char buf[1];

    for (f = 0; f < sizeof(int_formats) / sizeof(int_formats[0]); f++)
        for (v = 0; v < sizeof(ints) / sizeof(ints[0]); v++)
            ROW(int_formats[f], ints[v]);
    for (f = 0; f < sizeof(long_formats) / sizeof(long_formats[0]); f++)
        for (v = 0; v < sizeof(longs) / sizeof(longs[0]); v++)
            ROW(long_formats[f], longs[v]);
    for (f = 0; f < sizeof(llong_formats) / sizeof(llong_formats[0]); f++)
        for (v = 0; v < sizeof(llongs) / sizeof(llongs[0]); v++)
            ROW(llong_formats[f], llongs[v]);
    for (f = 0; f < sizeof(str_formats) / sizeof(str_formats[0]); f++)
        for (v = 0; v < sizeof(strs) / sizeof(strs[0]); v++)
            ROW(str_formats[f], strs[v]);

    ROW("%p", (void *)buf);
    ROW("%20p|", (void *)buf);
    ROW("%-20p|", (void *)buf);
    ROW("%zu", (size_t)123456789);
    ROW("%ti", (ptrdiff_t)-5);
    ROW("%jd", (intmax_t)-77);
    ROW("%5c|", 'x');
    ROW("%-5c|", 'x');
    ROW("100%% %d", 5);
}

/** The float rows: the table's values and their negations, the infinities,
 * random bit patterns, and integers and short fractions. */
static void float_sweep(void)
{
    size_t v;
    int i;

    for (v = 0; v < sizeof(float_values) / sizeof(float_values[0]); v++) {
        float_rows(float_values[v]);
        float_rows(-float_values[v]);
    }
    float_rows(INFINITY);
    float_rows(-INFINITY);
    for (i = 0; i < 4000; i++) {
        union {
            uint64_t u;
            double d;
        } r;
        r.u = next_random();
        if (isnan(r.d))
            continue;
        float_rows(r.d);
    }
    for (i = 0; i < 2000; i++) {
        /* integers and short fractions, where ties to even are met */
        double d = (double)(int64_t)(next_random() % 2000001) / 1000.0 - 1000.0;
        float_rows(d);
        float_rows((double)(int64_t)(next_random() % 4097) / 64.0);
    }
}

/** The rows of whole calls: Doom's lines, stars, and C99's return when the
 * buffer is short. */
static void call_rows(void)
{
    char buf[64];
    int i, n;

    {
        /* g_game.c's timedemo line */
        char got[128], want[128];
        int gn = port_snprintf(got, sizeof(got), "timed %i gametics in %i realtics (%f fps)", 5026,
                               110, 35.0 * 5026 / 110);
        int wn = snprintf(want, sizeof(want), "timed %i gametics in %i realtics (%f fps)", 5026,
                          110, 35.0 * 5026 / 110);
        same("timedemo", got, gn, want, wn);
    }

    {
        /* stars: width and precision from the arguments */
        char got[64], want[64];
        int gn = port_snprintf(got, sizeof(got), "%*.*f|%-*d|%.*s|%*d", 10, 3, 3.14159, 4, 7, 3,
                               "abcdef", -5, 42);
        int wn = snprintf(want, sizeof(want), "%*.*f|%-*d|%.*s|%*d", 10, 3, 3.14159, 4, 7, 3,
                          "abcdef", -5, 42);
        same("stars", got, gn, want, wn);
    }

    /* C99's return: the whole length, with the stored part cut and terminated */
    memset(buf, 'z', sizeof(buf));
    n = port_snprintf(buf, 5, "%s-%d", "abcdef", 12);
    same("cut", buf, n, "abcd", 9);
    n = port_snprintf(buf, 1, "%d", 12345);
    same("size 1", buf, n, "", 5);
    n = port_snprintf(NULL, 0, "%.3f", 2.5);
    same("size 0", "", n, "", 5);
    n = port_snprintf(buf, sizeof(buf), "ab%ncd", &i);
    same("%n", buf, i, "abcd", 2);
}

/** The rows whose text is glibc's where hosts differ. */
static void fixed_rows(void)
{
    FIXED("%p", (void *)0, "(nil)");
    FIXED("%7p|", (void *)0, "  (nil)|");
    FIXED("%s", (char *)0, "(null)");
    FIXED("%.3s|", (char *)0, "|");
    FIXED("%y%d", 3, "%y3");
    FIXED("%Lf", (long double)2.5, "2.500000");
    FIXED("%f", NAN, "nan");
    FIXED("%+f", NAN, "+nan");
    FIXED("% e", NAN, " nan");
    FIXED("%f", -NAN, "-nan");
    FIXED("%7.2G|", NAN, "    NAN|");
    FIXED("%-6e|", NAN, "nan   |");
    FIXED("%010g", NAN, "       nan");
}

int main(void)
{
    integer_rows();
    float_sweep();
    call_rows();
    fixed_rows();
    if (fails) {
        fprintf(stderr, "doom_format_hosttest: %d of %d rows failed\n", fails, rows);
        return 1;
    }
    printf("doom_format_hosttest: %d rows pass\n", rows);
    return 0;
}
