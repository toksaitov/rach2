/**
 * Check the Doom port's SHA-1 against the test vectors of FIPS 180.
 *
 * host-deps: ports/doom/sha1_fips.c
 * host-cflags: -I ports/doom/src
 */

#include <stdio.h>
#include <string.h>

#include "sha1.h"

static int fails;

#define CHECK(what, cond)                                            \
    do {                                                             \
        if (!(cond)) {                                               \
            fprintf(stderr, "doom_sha1_hosttest: fail: %s\n", what); \
            fails++;                                                 \
        }                                                            \
    } while (0)

/**
 * Compare digest with hex, 40 hex digits, and report a mismatch under name.
 * Returns 1 when they match.
 */
static int same(const char *name, sha1_digest_t digest, const char *hex)
{
    char got[41];
    int i;

    for (i = 0; i < 20; i++) {
        snprintf(got + 2 * i, 3, "%02x", digest[i]);
    }
    if (strcmp(got, hex) != 0) {
        fprintf(stderr, "doom_sha1_hosttest: %s: got %s, want %s\n", name, got, hex);
        return 0;
    }
    return 1;
}

/**
 * Digest the bytes of msg, repeated times times, fed in pieces of step
 * bytes, and compare the result with hex.
 */
static int digest_of(const char *msg, long times, size_t step, const char *hex)
{
    sha1_context_t ctx;
    sha1_digest_t digest;
    size_t len = strlen(msg), off, n;
    long i;

    SHA1_Init(&ctx);
    for (i = 0; i < times; i++) {
        for (off = 0; off < len; off += n) {
            n = len - off < step ? len - off : step;
            SHA1_Update(&ctx, (byte *)msg + off, n);
        }
    }
    SHA1_Final(digest, &ctx);
    return same(msg, digest, hex);
}

int main(void)
{
    static const char abc[] = "abc";
    static const char m448[] = "abcdbcdecdefdefgefghfghighijhijkijkljklmklmnlmnomnopnopq";
    static const char m896[] = "abcdefghbcdefghicdefghijdefghijkefghijklfghijklmghijklmn"
                               "hijklmnoijklmnopjklmnopqklmnopqrlmnopqrsmnopqrstnopqrstu";
    static const size_t steps[] = {1, 3, 55, 56, 63, 64, 65, 1000};
    sha1_context_t ctx, ref;
    sha1_digest_t a, b;
    unsigned char block[200];
    size_t s;
    int i;

    for (s = 0; s < sizeof(steps) / sizeof(steps[0]); s++) {
        CHECK("empty", digest_of("", 1, steps[s], "da39a3ee5e6b4b0d3255bfef95601890afd80709"));
        CHECK("abc", digest_of(abc, 1, steps[s], "a9993e364706816aba3e25717850c26c9cd0d89d"));
        CHECK("448 bits", digest_of(m448, 1, steps[s], "84983e441c3bd26ebaae4aa1f95129e5e54670f1"));
        CHECK("896 bits", digest_of(m896, 1, steps[s], "a49b2446a02c645bf419f995b67091253a04a259"));
    }
    CHECK("a million a",
          digest_of("aaaaaaaaaa", 100000, 7, "34aa973cd4c4daa4f61eeb2bdbad27316534016f"));

    /* Every length from 0 to 200 bytes, crossing the padding's boundaries
     * at 55, 56 and 64, digests the same whole and a byte at a time. */
    for (i = 0; i < (int)sizeof(block); i++) {
        block[i] = (unsigned char)(i * 7 + 1);
    }
    for (i = 0; i <= (int)sizeof(block); i++) {
        int j;

        SHA1_Init(&ref);
        SHA1_Update(&ref, block, (size_t)i);
        SHA1_Final(a, &ref);
        SHA1_Init(&ctx);
        for (j = 0; j < i; j++) {
            SHA1_Update(&ctx, block + j, 1);
        }
        SHA1_Final(b, &ctx);
        CHECK("lengths 0 to 200", memcmp(a, b, sizeof(a)) == 0);
    }

    /* The engine's helpers: a word big-endian, a string with its NUL. */
    SHA1_Init(&ctx);
    SHA1_UpdateInt32(&ctx, 0x61626300u);
    SHA1_Final(a, &ctx);
    SHA1_Init(&ctx);
    SHA1_UpdateString(&ctx, (char *)abc);
    SHA1_Final(b, &ctx);
    CHECK("UpdateInt32 and UpdateString", memcmp(a, b, sizeof(a)) == 0);
    CHECK("abc NUL", same("abc NUL", a, "686483805ac47ca14e03514f7481a7973b401762"));

    if (fails == 0) {
        printf("doom_sha1_hosttest: ok\n");
    }
    return fails > 255 ? 255 : fails;
}
