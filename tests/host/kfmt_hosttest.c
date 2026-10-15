/*
 * kfmt_hosttest.c: the formatter every kernel message goes through.
 *
 * host-deps: rach2/sys/lib/kfmt.c rach2/sys/lib/kstd.c rach2/sys/lib/kstring.c
 * host-deps: rach2/sys/lib/kctypes.c
 *
 * `k_vprintf` takes one argument for each conversion it finds in the format
 * string, so a conversion it does not know is more than a wrong letter: it is
 * written as its letter alone and takes no argument, and every field after it
 * in the same message prints the argument meant for the field before. Without
 * the `0` flag and the width, a format of `%x:%x at %02x:%02x.%d` prints
 * `8086:a0c at 02x:02x.0`: three of the five fields are wrong and the text of
 * two is the format's own.
 *
 * The rows below check each conversion, the `0` flag and the width, the cases
 * where padding does not apply, and the format that ends on its percent.
 * Exits with the count of failed checks, at most 255.
 */

#include <stdio.h>
#include <stdarg.h>
#include <string.h>

#include "kfmt.h"

/* What the formatter wrote, `used` bytes of it. */
static char out[512];
static size_t used;
static int fails;

/** Count and print a failure when `cond` is false; `what` says what held. */
#define CHECK(cond, what)                          \
    do {                                           \
        if (!(cond)) {                             \
            fails++;                               \
            fprintf(stderr, "  FAIL: %s\n", what); \
        }                                          \
    } while (0)

/** The formatter's output: append `c` to `out`, dropping what does not fit. */
static void sink(char c)
{
    if (used + 1 < sizeof(out)) {
        out[used++] = c;
    }
}

/** Format with the arguments given, each of the type its conversion reads,
 * and compare the whole result with `want`. `n`, the count of arguments, is
 * not read; it is the parameter `va_start` needs. */
static void row(const char *want, const char *format, int n, ...)
{
    va_list ap;

    (void)n;
    used = 0;
    memset(out, 0, sizeof(out));
    va_start(ap, n);
    k_vprintf(sink, format, ap);
    va_end(ap);
    out[used] = '\0';
    if (strcmp(out, want) != 0) {
        fails++;
        fprintf(stderr, "  FAIL: %s: got \"%s\", want \"%s\"\n", format, out, want);
    }
}

/**
 * Run the rows. Unsigned decimal is checked at an int's width and a long's, so
 * that a count past the largest int does not print as a negative number. A
 * conversion the formatter does not know is written as its letter alone, which
 * `tools/verify/fmtcheck.sh` keeps out of the kernel's messages. Returns the
 * count of failures, at most 255.
 */
int main(void)
{
    /* The conversions without a flag or a width. */
    row("42", "%d", 1, 42);
    row("-7", "%d", 1, -7);
    row("ff", "%x", 1, 255);
    row("FF", "%X", 1, 255);
    row("777", "%o", 1, 511);
    row("1010", "%b", 1, 10);
    row("hi", "%s", 1, (char *)"hi");
    row("(null)", "%s", 1, (char *)0);
    row("A", "%c", 1, 'A');
    row("100%", "100%%", 0);

    /* Unsigned decimal: a count past the largest int prints as itself. */
    row("4000000000", "%u", 1, 4000000000u);
    row("0", "%u", 1, 0u);
    row("4294967295", "%lu", 1, 4294967295ul);
    row("  9", "%3u", 1, 9u);

    /* The `0` flag and the width, which a PCI address needs. */
    row("0f", "%02x", 1, 15);
    row("00", "%02x", 1, 0);
    row("ff", "%02x", 1, 255);
    row("1ff", "%02x", 1, 511); /* wider than the width: not cut */
    row("  7", "%3d", 1, 7);
    row("007", "%03d", 1, 7);
    row("0000ff", "%06x", 1, 255);

    /* A whole message of a PCI device, with every field in place. */
    row("8086:a0c at 00:1f.3", "%x:%x at %02x:%02x.%d", 5, 0x8086, 0xa0c, 0, 0x1f, 3);

    /* A width does not disturb the argument after it. */
    row("00:1f x", "%02x:%02x %s", 3, 0, 0x1f, (char *)"x");

    /* The format that stops on its percent writes the sign and stops. */
    row("a%", "a%", 0);

    /* A conversion the formatter does not know prints its letter alone. */
    row("q", "%q", 0);

    if (fails == 0) {
        printf("kfmt_hosttest: 22 rows passed\n");
    }
    return fails > 255 ? 255 : fails;
}
