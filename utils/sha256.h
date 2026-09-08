/**
 * SHA-256, FIPS 180-4, for the programs that must judge what they wrote.
 *
 * `reflash --verify` reads a range of a device back and prints its digest, and
 * the installer compares that with the manifest the stick carries. The digest
 * has to be computed by the machine doing the writing: a hash taken on the
 * host says what left, not what arrived.
 *
 * This is the same algorithm as `tools/verify/sha256.c`, which the build's own
 * scripts use, written again here because that one is a host program with a
 * `main` and this is a unit two guest programs link. Neither is derived from
 * the other, so a mistake in one is caught by the vectors both are held to.
 */
#ifndef SHA256_H
#define SHA256_H

#include <stddef.h>

struct sha256 {
    unsigned long h[8];
    unsigned long long bytes;
    unsigned char block[64];
    size_t fill;
};

/** Begin a digest. */
void sha256_init(struct sha256 *s);

/** Take `n` more bytes. */
void sha256_update(struct sha256 *s, const unsigned char *p, size_t n);

/** Finish, writing the 32 bytes of the digest. The state is spent. */
void sha256_final(struct sha256 *s, unsigned char out[32]);

/** Write the digest as 64 lowercase hex digits and a terminator. */
void sha256_hex(const unsigned char digest[32], char out[65]);

#endif /* SHA256_H */
