/**
 * Host test of the SHA-256 that `reflash --verify` prints, utils/sha256.c.
 *
 * host-deps: utils/sha256.c
 *
 * `reflash --verify` reads a range of a disk back and prints its SHA-256,
 * and an installer compares it with the digest of the image it wrote. A
 * wrong digest would fail every install, or pass a wrong one if the same
 * fault were on both sides. The host's own `shasum` is on the other side,
 * so the vectors below are the published ones, not digests this tree
 * computes.
 *
 * The vectors are FIPS 180-4's two and the standard million-a test, plus the
 * empty message and the block-boundary lengths where the padding rule decides
 * whether one block or two are added: 55 bytes fit their length field in the
 * same block, 56 do not, and 64 is a whole block with nothing left.
 *
 * The million-a test is fed a thousand bytes at a time, and 200 bytes fed
 * one at a time must give the digest of the same bytes fed at once: a
 * digest taken over a disk arrives in whatever pieces the reads gave.
 */

#include <stdio.h>
#include <string.h>

#include "sha256.h"

static int fails;

#define CHECK(cond, what)                          \
    do {                                           \
        if (!(cond)) {                             \
            fails++;                               \
            fprintf(stderr, "  FAIL: %s\n", what); \
        }                                          \
    } while (0)

/** The digest of `n` bytes at `p`, as hex. */
static void digest(const void *p, size_t n, char out[65])
{
    struct sha256 s;
    unsigned char d[32];

    sha256_init(&s);
    sha256_update(&s, (const unsigned char *)p, n);
    sha256_final(&s, d);
    sha256_hex(d, out);
}

/** Count a failure, with `what` and the digest got, when the digest of
 * the string `message` is not the hex string `want`. */
static void vector(const char *message, const char *want, const char *what)
{
    char got[65];

    digest(message, strlen(message), got);
    if (strcmp(got, want) != 0) {
        fails++;
        fprintf(stderr, "  FAIL: %s: %s\n", what, got);
    }
}

/** Run every vector; the exit status is the count of failed checks, at most
 * 255. */
int main(void)
{
    static unsigned char big[1000];
    char got[65];
    struct sha256 s;
    unsigned char d[32];
    int i;

    printf("sha256_hosttest: the digest reflash verifies with\n");

    vector("", "e3b0c44298fc1c149afbf4c8996fb92427ae41e4649b934ca495991b7852b855",
           "the empty message");
    vector("abc", "ba7816bf8f01cfea414140de5dae2223b00361a396177a9cb410ff61f20015ad",
           "the specification's first vector");
    vector("abcdbcdecdefdefgefghfghighijhijkijkljklmklmnlmnomnopnopq",
           "248d6a61d20638b8e5c026930c3e6039a33ce45964ff2167f6ecedd419db06c1",
           "the specification's second vector");

    memset(big, 'a', sizeof(big));
    digest(big, 55, got);
    CHECK(strcmp(got, "9f4390f8d30c2dd92ec9f095b65e2b9ae9b0a925a5258e241c9f1e910f734318") == 0,
          "fifty-five bytes, the last length that fits its own block");
    digest(big, 56, got);
    CHECK(strcmp(got, "b35439a4ac6f0948b6d6f9e3c6af0f5f590ce20f1bde7090ef7970686ec6738a") == 0,
          "fifty-six bytes, the first that needs another");
    digest(big, 64, got);
    CHECK(strcmp(got, "ffe054fe7ae0cb6dc65c3af9b61d5209f439851db43d0ba5997337df154668eb") == 0,
          "a whole block");

    sha256_init(&s);
    for (i = 0; i < 1000; i++) {
        sha256_update(&s, big, sizeof(big));
    }
    sha256_final(&s, d);
    sha256_hex(d, got);
    CHECK(strcmp(got, "cdc76e5c9914fb9281a1c7e284d73e67f1809a48a497200e046d39ccc7112cd0") == 0,
          "a million a's in a thousand pieces");

    {
        char once[65];

        sha256_init(&s);
        for (i = 0; i < 200; i++) {
            sha256_update(&s, big, 1);
        }
        sha256_final(&s, d);
        sha256_hex(d, got);
        digest(big, 200, once);
        CHECK(strcmp(got, once) == 0, "a byte at a time is the same digest as all at once");
    }

    if (fails == 0) {
        printf("sha256_hosttest: OK\n");
        return 0;
    }
    printf("sha256_hosttest: %d failure(s)\n", fails);
    return fails > 255 ? 255 : fails;
}
