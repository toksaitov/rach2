/**
 * kstd_hosttest.c: k_itoa, the kernel's integer formatter (`lib/kstd.c`).
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
 * depends on k_strreverse and k_strtoupper, compiled in. Exits with the count
 * of failed checks, at most 120.
 */

#include "kstd.h"

static int fails;

/** Count a failure when `cond` is false. */
#define CHECK(cond)    \
    do {               \
        if (!(cond)) { \
            fails++;   \
        }              \
    } while (0)

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

    return (fails > 120) ? 120 : fails;
}
