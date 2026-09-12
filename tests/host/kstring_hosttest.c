/**
 * Dual-target host test for the kernel's pure string and memory routines.
 *
 * host-deps: rach2/sys/lib/kstring.c rach2/sys/lib/kctypes.c
 *
 * The routines in `sys/lib/kstring.c` are compiled and run NATIVELY on the
 * host under AddressSanitizer + UndefinedBehaviorSanitizer, and vector-tested.
 * The same .c file is linked into the freestanding kernel (one source, two
 * targets), so a bounds bug surfaces here, in milliseconds, instead of as a
 * mystery fault in QEMU. The test prints nothing; it returns the number of
 * failed checks as its exit status (0 == all passed), which is what
 * tests/system/suites/host.sh asserts.
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

    /* k_memmove over overlapping ranges, both directions. Forward overlap
     * (destination above the source, inside it) is the case a plain forward
     * copy corrupts: it overwrites source bytes before reading them, turning
     * "abcdef" into "ababab" instead of "ababcd". Backward overlap and the
     * disjoint case must behave exactly like k_memcpy. */
    {
        char m[16];

        k_memcpy(m, "abcdefgh", 9);
        k_memmove(m + 2, m, 4);
        CHECK(m[0] == 'a' && m[1] == 'b' && m[2] == 'a' && m[3] == 'b' && m[4] == 'c' &&
              m[5] == 'd' && m[6] == 'g' && m[7] == 'h');

        k_memcpy(m, "abcdefgh", 9);
        k_memmove(m, m + 2, 4);
        CHECK(m[0] == 'c' && m[1] == 'd' && m[2] == 'e' && m[3] == 'f' && m[4] == 'e' &&
              m[5] == 'f' && m[6] == 'g' && m[7] == 'h');

        k_memcpy(m, "abcdefgh", 9);
        k_memmove(m + 4, m, 4);
        CHECK(m[4] == 'a' && m[5] == 'b' && m[6] == 'c' && m[7] == 'd');

        k_memmove(m, m, 8);
        CHECK(m[0] == 'a' && m[7] == 'd');
    }

    /* k_memmove between two separate objects, which is the shape tcc's
     * compiled struct assignments produce. The overlap test must not choose
     * the backward copy or trip a sanitizer here, whatever the two objects'
     * relative addresses are. */
    {
        char a[8];
        char b[8];

        k_memcpy(a, "12345678", 8);
        k_memmove(b, a, 8);
        CHECK(b[0] == '1' && b[7] == '8');
        k_memmove(a, b, 8);
        CHECK(a[0] == '1' && a[7] == '8');
    }

    /* Every length to 67 at every alignment of both buffers, for the copy,
     * the fill and the forward move: the bytes asked for arrive, and the byte
     * before and the byte after each buffer are untouched. On the kernel's
     * target the words go through the string instructions and the tail
     * through the byte loop; here the byte loop is the whole function, so
     * this is the tail's test and the other target's proof is the pipe
     * probe's sweep in QEMU. */
    {
        static unsigned char pat[80], got[80];
        int len, sa, da, i, bad = 0;

        for (i = 0; i < 80; i++) {
            pat[i] = (unsigned char)(0x21 + (i * 7) % 90);
        }
        for (len = 0; len <= 67 && bad == 0; len++) {
            for (sa = 0; sa < 4 && bad == 0; sa++) {
                for (da = 1; da < 5 && bad == 0; da++) {
                    for (i = 0; i < 80; i++) {
                        got[i] = 0x5a;
                    }
                    k_memcpy(got + da, pat + sa, (size_t)len);
                    for (i = 0; i < len; i++) {
                        if (got[da + i] != pat[sa + i]) {
                            bad = 1;
                        }
                    }
                    if (got[da - 1] != 0x5a || got[da + len] != 0x5a) {
                        bad = 2;
                    }
                    for (i = 0; i < 80; i++) {
                        got[i] = 0x5a;
                    }
                    k_memset(got + da, (char)0xa5, (size_t)len);
                    for (i = 0; i < len; i++) {
                        if (got[da + i] != 0xa5) {
                            bad = 3;
                        }
                    }
                    if (got[da - 1] != 0x5a || got[da + len] != 0x5a) {
                        bad = 4;
                    }
                    for (i = 0; i < 80; i++) {
                        got[i] = pat[i];
                    }
                    k_memmove(got + da, got + da + sa + 1, (size_t)len);
                    for (i = 0; i < len; i++) {
                        if (got[da + i] != pat[da + sa + 1 + i]) {
                            bad = 5;
                        }
                    }
                    if (got[da - 1] != pat[da - 1]) {
                        bad = 6;
                    }
                }
            }
        }
        CHECK(bad == 0);
    }

    /* Bounded so the count never wraps to 0 (a false pass) as an exit
     * status. */
    return (fails > 120) ? 120 : fails;
}
