/**
 * krandom_hosttest.c: the kernel's random number generator, a ChaCha20
 * keystream (`lib/krandom.c`).
 *
 * host-deps: rach2/sys/lib/krandom.c
 *
 * Compiles and runs `sys/lib/krandom.c` natively under ASan and UBSan. Exits
 * with the count of failed checks, capped at 120 so that it cannot wrap to
 * 0.
 */

#include "krandom.h"

static int fails;

/** Count a failure when `cond` is false. */
#define CHECK(cond)    \
    do {               \
        if (!(cond)) { \
            fails++;   \
        }              \
    } while (0)

/**
 * Run the checks; the count of failures, at most 120.
 *
 * krandom_bytes must fill the n bytes it is given and no more, which ASan
 * bounds. The stream is used for keys, so its bits are counted: of 4096, the
 * count set must be within 205 (about 5%) of 2048. A stuck or biased generator
 * lands far outside, and a uniform one is well within, so that a sound stream
 * does not fail by chance.
 */
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

    /* The all-zero seed does not lock the generator at zero. */
    krandom_reseed(0, 0, 0, 0);
    nonzero = 0;
    for (i = 0; i < 8; i++) {
        if (krandom_u32() != 0) {
            nonzero = 1;
        }
    }
    CHECK(nonzero);

    /* krandom_bytes fills n bytes, and not with zeros alone. */
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

    /* About half of 4096 bits of the stream are set. */
    {
        uint8_t big[512];
        int ones = 0;
        unsigned int x;
        krandom_reseed(0x243f6a88u, 0x85a308d3u, 0x13198a2eu, 0x03707344u);
        krandom_bytes(big, sizeof(big));
        for (i = 0; i < (int)sizeof(big); i++) {
            for (x = big[i]; x; x &= x - 1) {
                ones++;
            }
        }
        CHECK(ones > 2048 - 205 && ones < 2048 + 205);
    }

    return (fails > 120) ? 120 : fails;
}
