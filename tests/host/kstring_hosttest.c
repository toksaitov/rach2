/*
 * kstring_hosttest.c
 *
 * host-deps: rach2/sys/lib/kstring.c rach2/sys/lib/kctypes.c
 *
 * Dual-target seam test. The kernel's pure string/memory routines in
 * sys/lib/kstring.c are compiled and run NATIVELY on the host under
 * AddressSanitizer + UndefinedBehaviorSanitizer, and vector-tested. The same
 * .c file is linked into the freestanding kernel (one source, two targets),
 * so a bounds bug surfaces here, in milliseconds, instead of as a mystery fault
 * in QEMU. The test prints nothing; it returns the number of failed checks as
 * its exit status (0 == all passed), which is what tools/hosttest.sh asserts.
 */

#include "kstring.h"

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

    /* Bounded so the count never wraps to 0 (a false pass) as an exit status. */
    return (fails > 120) ? 120 : fails;
}
