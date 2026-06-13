/*
 * krandom_hosttest.c
 *
 * host-deps: rach2/sys/lib/krandom.c
 *
 * Dual-target seam test for the kernel's pure xorshift128 PRNG
 * (sys/lib/krandom.c), compiled and run natively under ASan/UBSan. Returns the
 * failed-check count (0 == all passed), capped so it cannot wrap to a false 0.
 */

#include "krandom.h"

static int fails;

#define CHECK(cond)    \
    do {               \
        if (!(cond)) { \
            fails++;   \
        }              \
    } while (0)

int main(void)
{
    uint32_t a[8];
    uint32_t b[8];
    uint8_t buf[37];
    int i;
    int differ;
    int nonzero;

    /* Determinism: the same seed yields the same stream. */
    krandom_reseed(1, 2, 3, 4);
    for (i = 0; i < 8; i++) {
        a[i] = krandom_u32();
    }
    krandom_reseed(1, 2, 3, 4);
    for (i = 0; i < 8; i++) {
        b[i] = krandom_u32();
    }
    for (i = 0; i < 8; i++) {
        CHECK(a[i] == b[i]);
    }

    /* A different seed yields a different stream (overwhelmingly likely). */
    krandom_reseed(5, 6, 7, 8);
    differ = 0;
    for (i = 0; i < 8; i++) {
        if (krandom_u32() != a[i]) {
            differ = 1;
        }
    }
    CHECK(differ);

    /* The all-zero seed must NOT lock the generator at zero. */
    krandom_reseed(0, 0, 0, 0);
    nonzero = 0;
    for (i = 0; i < 8; i++) {
        if (krandom_u32() != 0) {
            nonzero = 1;
        }
    }
    CHECK(nonzero);

    /* krandom_bytes fills exactly n bytes (ASan guards the bounds) and the
     * result is not all-zero. */
    krandom_reseed(0xC0FFEEu, 0x0BADF00Du, 0x00001234u, 0x00005678u);
    for (i = 0; i < (int)sizeof(buf); i++) {
        buf[i] = 0;
    }
    krandom_bytes(buf, sizeof(buf));
    nonzero = 0;
    for (i = 0; i < (int)sizeof(buf); i++) {
        if (buf[i] != 0) {
            nonzero = 1;
        }
    }
    CHECK(nonzero);

    return (fails > 120) ? 120 : fails;
}
