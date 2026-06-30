/**
 * Dual-target seam test for the kernel's pure string and memory routines.
 *
 * host-deps: rach2/sys/lib/kstring.c rach2/sys/lib/kctypes.c
 *
 * The routines in `sys/lib/kstring.c` are compiled and run NATIVELY on the
 * host under AddressSanitizer + UndefinedBehaviorSanitizer, and vector-tested.
 * The same .c file is linked into the freestanding kernel (one source, two
 * targets), so a bounds bug surfaces here, in milliseconds, instead of as a
 * mystery fault in QEMU. The test prints nothing; it returns the number of
 * failed checks as its exit status (0 == all passed), which is what
 * tools/hosttest.sh asserts.
 */

#include "kstring.h"
#include "kctypes.h"

static int fails;

#define CHECK(cond)    \
    do {               \
        if (!(cond)) { \
            fails++;   \
        }              \
    } while (0)

int main(void)
{
    char buf[32];
    char dst[8];

    /* k_strlen */
    CHECK(k_strlen("") == 0);
    CHECK(k_strlen("hello") == 5);

    /* k_memset then read back */
    k_memset(buf, 'A', 4);
    buf[4] = '\0';
    CHECK(buf[0] == 'A' && buf[1] == 'A' && buf[2] == 'A' && buf[3] == 'A');
    CHECK(k_strlen(buf) == 4);

    /* k_memcpy copies exactly n bytes */
    k_memcpy(buf, "world!", 7);
    CHECK(k_strlen(buf) == 6);
    CHECK(buf[5] == '!');

    /* k_strncpy never writes past n; pads short sources within n */
    k_memset(dst, 'Z', sizeof(dst));
    k_strncpy(dst, "ab", 8);
    CHECK(dst[0] == 'a' && dst[1] == 'b' && dst[2] == '\0');

    /* k_strreverse: the empty string must be a no-op (the length-1 upper index
     * would otherwise wrap to SIZE_MAX and walk off the buffer), then single,
     * even, and odd lengths reverse correctly. */
    {
        char r[16];
        k_strncpy(r, "", 16);
        k_strreverse(r);
        CHECK(r[0] == '\0');
        k_strncpy(r, "x", 16);
        k_strreverse(r);
        CHECK(r[0] == 'x' && r[1] == '\0');
        k_strncpy(r, "ab", 16);
        k_strreverse(r);
        CHECK(r[0] == 'b' && r[1] == 'a' && r[2] == '\0');
        k_strncpy(r, "abcde", 16);
        k_strreverse(r);
        CHECK(r[0] == 'e' && r[1] == 'd' && r[2] == 'c' && r[3] == 'b' && r[4] == 'a');
    }

    /* case fold: k_toupper/k_tolower change only their own case; k_strtoupper
     * folds a whole string and leaves digits and punctuation alone. */
    CHECK(k_toupper('a') == 'A' && k_toupper('z') == 'Z');
    CHECK(k_toupper('A') == 'A' && k_toupper('5') == '5' && k_toupper('!') == '!');
    CHECK(k_tolower('A') == 'a' && k_tolower('Z') == 'z');
    CHECK(k_tolower('a') == 'a' && k_tolower('5') == '5');
    {
        char u[16];
        k_strncpy(u, "aBc1!", 16);
        k_strtoupper(u);
        CHECK(u[0] == 'A' && u[1] == 'B' && u[2] == 'C' && u[3] == '1' && u[4] == '!' &&
              u[5] == '\0');
    }

    /* Bounded so the count never wraps to 0 (a false pass) as an exit
     * status. */
    return (fails > 120) ? 120 : fails;
}
