/**
 * kstd_hosttest.c: k_itoa, the kernel's integer formatter, and k_strtoul,
 * its reader of a number written to /sys (`lib/kstd.c`).
 *
 * host-deps: rach2/sys/lib/kstd.c rach2/sys/lib/kstring.c
 * host-deps: rach2/sys/lib/kctypes.c
 *
 * k_itoa converts every number the kernel prints, so it is run here natively
 * under AddressSanitizer and UndefinedBehaviorSanitizer across the bases and
 * the signed edges. INT_MIN is the edge that matters: negating it in signed
 * `int` overflows, which is undefined, and UBSan reports it here. The
 * rejections of a buffer too small and of a radix out of range, the count
 * returned, and the plus sign `force_sign` writes are covered too. It
 * depends on k_strreverse and k_strtoupper, compiled in. k_strtoul is
 * checked against what Linux's kstrtoul takes with base 0: the bases, the
 * plus sign, one newline, leading zeros past 31 bytes, the end of the text
 * at a NUL, junk, a number past 64 bits before junk and one past 32 bits
 * with and without it, which i686's unsigned long cannot hold, with `*out`
 * left alone on an error. Exits with the count of failed checks, at most 120.
 */

#include "kerrno.h"
#include "kstd.h"

static int fails;

/** Count a failure when `cond` is false. */
#define CHECK(cond)    \
    do {               \
        if (!(cond)) { \
            fails++;   \
        }              \
    } while (0)

/** Whether k_strtoul reads the text `s` as `want`, an error code or 0,
 * with the value `value` on success and `*out` untouched on an error. */
static int reads(const char *s, int want, unsigned long value)
{
    unsigned long out = 12345;
    int rc = k_strtoul(s, k_strlen(s), &out);

    return rc == want && out == (want == 0 ? value : 12345ul);
}

/** Whether the strings `a` and `b` are equal. */
static int streq(const char *a, const char *b)
{
    while (*a && *a == *b) {
        a++;
        b++;
    }
    return *a == *b;
}

/** Run the checks; the count of failures, at most 120. */
int main(void)
{
    char b[64];

    /* Signed decimal, including both edges of int. */
    k_itoa(0, b, sizeof(b), 'd', false);
    CHECK(streq(b, "0"));
    k_itoa(12345, b, sizeof(b), 'd', false);
    CHECK(streq(b, "12345"));
    k_itoa(-42, b, sizeof(b), 'd', false);
    CHECK(streq(b, "-42"));
    k_itoa(2147483647, b, sizeof(b), 'd', false);
    CHECK(streq(b, "2147483647"));
    /* INT_MIN, whose magnitude only unsigned arithmetic holds. */
    k_itoa(-2147483647 - 1, b, sizeof(b), 'd', false);
    CHECK(streq(b, "-2147483648"));

    /* Non-decimal bases format the raw bit pattern as unsigned. */
    k_itoa(255, b, sizeof(b), 'x', false);
    CHECK(streq(b, "ff"));
    k_itoa(255, b, sizeof(b), 'X', false);
    CHECK(streq(b, "FF"));
    k_itoa(8, b, sizeof(b), 'o', false);
    CHECK(streq(b, "10"));
    k_itoa(5, b, sizeof(b), 'b', false);
    CHECK(streq(b, "101"));
    k_itoa(-1, b, sizeof(b), 'x', false);
    CHECK(streq(b, "ffffffff"));
    /* A plain numeric radix in [2, 36] works too; 35 in base 36 is 'z'. */
    k_itoa(35, b, sizeof(b), 36, false);
    CHECK(streq(b, "z"));

    /* The returned count includes the sign and the terminating NUL. */
    CHECK(k_itoa(-42, b, sizeof(b), 'd', false) == 4);
    CHECK(k_itoa(7, b, sizeof(b), 'd', false) == 2);

    /* A buffer smaller than the needed count returns 0 and writes nothing. */
    CHECK(k_itoa(12345, b, 3, 'd', false) == 0);

    /* An out-of-range radix returns 0. */
    CHECK(k_itoa(1, b, sizeof(b), 99, false) == 0);
    CHECK(k_itoa(1, b, sizeof(b), 1, false) == 0);

    /* force_sign puts a plus before a positive decimal, and not before 0. */
    CHECK(k_itoa(7, b, sizeof(b), 'd', true) == 3);
    CHECK(streq(b, "+7"));
    CHECK(k_itoa(0, b, sizeof(b), 'd', true) == 2);
    CHECK(streq(b, "0"));
    CHECK(k_itoa(-7, b, sizeof(b), 'd', true) == 3);
    CHECK(streq(b, "-7"));

    /* k_strtoul, as Linux's kstrtoul with base 0. */
    CHECK(reads("42", 0, 42));
    CHECK(reads("42\n", 0, 42));
    CHECK(reads("+7", 0, 7));
    CHECK(reads("010", 0, 8));
    CHECK(reads("0", 0, 0));
    CHECK(reads("0x1F", 0, 31));
    CHECK(reads("+0X1f\n", 0, 31));
    CHECK(reads("0xffffffff", 0, 0xfffffffful));
    CHECK(reads("000000000000000000000000000000000005", 0, 5));
    CHECK(reads("", -EINVAL, 0));
    CHECK(reads("+", -EINVAL, 0));
    CHECK(reads("\n", -EINVAL, 0));
    CHECK(reads("-1", -EINVAL, 0));
    CHECK(reads("0x", -EINVAL, 0));
    CHECK(reads("0xg", -EINVAL, 0));
    CHECK(reads("08", -EINVAL, 0));
    CHECK(reads("1a", -EINVAL, 0));
    CHECK(reads("1 ", -EINVAL, 0));
    CHECK(reads("1\n\n", -EINVAL, 0));
    CHECK(reads("0x10000000000000000", -ERANGE, 0));
    /* A 64-bit overflow is ERANGE before the junk after it, and junk after
     * a number past 32 bits EINVAL, at either width. */
    CHECK(reads("18446744073709551616x", -ERANGE, 0));
    CHECK(reads("4294967296x", -EINVAL, 0));
    /* The unsigned long's bound comes last: ERANGE where it has 32 bits,
     * as on i686. */
    CHECK(sizeof(unsigned long) == 4 ? reads("4294967296", -ERANGE, 0)
                                     : reads("4294967296", 0, (unsigned long)4294967296ull));
    {
        unsigned long out = 0;

        /* The text ends at a NUL before `len`, as Linux's string does. */
        CHECK(k_strtoul("9\0junk", 6, &out) == 0 && out == 9);
        /* And at `len` before a NUL. */
        CHECK(k_strtoul("123", 2, &out) == 0 && out == 12);
    }

    return (fails > 120) ? 120 : fails;
}
