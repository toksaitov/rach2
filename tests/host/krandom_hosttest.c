/**
 * Dual-target host test for the kernel's ChaCha20 keystream CSPRNG.
 *
 * host-deps: rach2/sys/lib/krandom.c
 *
 * Compiles and runs `sys/lib/krandom.c` natively under ASan/UBSan. Returns the
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

    /* Monobit sanity (the fail-closed gate trusts this stream for keys): over
     * 4096 ChaCha keystream bits the set-bit count must be near half (2048). A
     * stuck or biased generator lands far outside; a uniform one is comfortably
     * within +/-205 (~5%), wide enough that a healthy stream never flakes. */
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
