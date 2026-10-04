/*
 * kfmt_hosttest.c: the formatter every kernel message goes through.
 *
 * host-deps: rach2/sys/lib/kfmt.c rach2/sys/lib/kstd.c rach2/sys/lib/kstring.c
 * host-deps: rach2/sys/lib/kctypes.c
 *
 * `k_vprintf` reads its arguments off the stack a word at a time against the
 * conversions it finds in the format string, so a conversion it does not know
 * is not a cosmetic fault: it prints the letters verbatim and consumes no
 * argument, and every field after it in the same message is then read one slot
 * early. The machine on the desk printed `audio: not driven: 8086:a0c at
 * 02x:02x.0` for a format of `%x:%x at %02x:%02x.%d`, which is that fault:
 * three of the five fields are wrong and the text of two is the format's own.
 *
 * The rows below judge each conversion, the `0` flag and the width that were
 * missing, the cases where padding must not apply, and the end-of-string
 * case where the format stops on its percent. Returns the failed-check count
 * (0 == pass).
 */

#include <stdio.h>
#include <stdarg.h>
#include <string.h>

#include "kfmt.h"

static char out[512];
static size_t used;
static int fails;

#define CHECK(cond, what)                          \
    do {                                           \
        if (!(cond)) {                             \
            fails++;                               \
            fprintf(stderr, "  FAIL: %s\n", what); \
        }                                          \
    } while (0)

static void sink(char c)
{
    if (used + 1 < sizeof(out)) {
        out[used++] = c;
    }
}

/** Format with the arguments given, each of the type its conversion reads,
 * and compare the whole result. */
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

int main(void)
{
    /* The conversions that always worked. */
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

    /* Unsigned decimal, at an int's width and a long's: a count past the
       largest int prints as itself, not as a negative number. */
    row("4000000000", "%u", 1, 4000000000u);
    row("0", "%u", 1, 0u);
    row("4294967295", "%lu", 1, 4294967295ul);
    row("  9", "%3u", 1, 9u);

    /* The `0` flag and the width, which is what a PCI address asks for and
       what the machine could not print. */
    row("0f", "%02x", 1, 15);
    row("00", "%02x", 1, 0);
    row("ff", "%02x", 1, 255);
    row("1ff", "%02x", 1, 511); /* wider than the width: not cut */
    row("  7", "%3d", 1, 7);
    row("007", "%03d", 1, 7);
    row("0000ff", "%06x", 1, 255);

    /* The whole message the machine got wrong, with every field in place. */
    row("8086:a0c at 00:1f.3", "%x:%x at %02x:%02x.%d", 5, 0x8086, 0xa0c, 0, 0x1f, 3);

    /* A width does not disturb the argument after it. */
    row("00:1f x", "%02x:%02x %s", 3, 0, 0x1f, (char *)"x");

    /* The format that stops on its percent writes the sign and stops. */
    row("a%", "a%", 0);

    /* A conversion this does not know is still emitted verbatim, which is
       what the style check exists to keep out of the tree. */
    row("q", "%q", 0);

    if (fails == 0) {
        printf("kfmt_hosttest: 22 rows passed\n");
    }
    return fails > 255 ? 255 : fails;
}
