/*
 * sha1_fips.c: SHA-1 as FIPS 180-4 defines it, behind the interface of
 * src/sha1.h, which the engine uses to digest the WAD directory.
 */

#include <string.h>

#include "sha1.h"

/* Rotate a 32-bit word left by n bits, 0 < n < 32. */
#define ROL(x, n) (((x) << (n)) | ((x) >> (32 - (n))))

/**
 * Start a digest: set the five chaining words to FIPS 180-4's initial hash
 * value H(0) and empty the block buffer.
 */
void SHA1_Init(sha1_context_t *context)
{
    context->h0 = 0x67452301;
    context->h1 = 0xefcdab89;
    context->h2 = 0x98badcfe;
    context->h3 = 0x10325476;
    context->h4 = 0xc3d2e1f0;
    context->nblocks = 0;
    context->count = 0;
}

/**
 * Fold one 64-byte block into the chaining words, as FIPS 180-4's section
 * 6.1.2 computes it; the block's words are big-endian.
 */
static void Transform(sha1_context_t *context, const byte *block)
{
    uint32_t w[80];
    uint32_t a, b, c, d, e, f, k, t;
    int i;

    for (i = 0; i < 16; i++) {
        w[i] = (uint32_t)block[4 * i] << 24 | (uint32_t)block[4 * i + 1] << 16
               | (uint32_t)block[4 * i + 2] << 8 | (uint32_t)block[4 * i + 3];
    }
    for (; i < 80; i++) {
        t = w[i - 3] ^ w[i - 8] ^ w[i - 14] ^ w[i - 16];
        w[i] = ROL(t, 1);
    }

    a = context->h0;
    b = context->h1;
    c = context->h2;
    d = context->h3;
    e = context->h4;
    for (i = 0; i < 80; i++) {
        if (i < 20) {
            f = (b & c) | (~b & d);
            k = 0x5a827999;
        } else if (i < 40) {
            f = b ^ c ^ d;
            k = 0x6ed9eba1;
        } else if (i < 60) {
            f = (b & c) | (b & d) | (c & d);
            k = 0x8f1bbcdc;
        } else {
            f = b ^ c ^ d;
            k = 0xca62c1d6;
        }
        t = ROL(a, 5) + f + e + k + w[i];
        e = d;
        d = c;
        c = ROL(b, 30);
        b = a;
        a = t;
    }
    context->h0 += a;
    context->h1 += b;
    context->h2 += c;
    context->h3 += d;
    context->h4 += e;
}

/**
 * Add len bytes of buf to the digest. A partial block waits in
 * context->buf, and context->nblocks counts the blocks already folded in.
 */
void SHA1_Update(sha1_context_t *context, byte *buf, size_t len)
{
    size_t n;

    while (len > 0) {
        if (context->count == 0 && len >= 64) {
            Transform(context, buf);
            context->nblocks++;
            buf += 64;
            len -= 64;
            continue;
        }
        n = 64 - (size_t)context->count;
        if (n > len) {
            n = len;
        }
        memcpy(context->buf + context->count, buf, n);
        context->count += (int)n;
        buf += n;
        len -= n;
        if (context->count == 64) {
            Transform(context, context->buf);
            context->nblocks++;
            context->count = 0;
        }
    }
}

/**
 * Finish a digest: pad the message with 0x80, zeros and its length in bits
 * as a big-endian 64-bit number, and write the five chaining words to
 * digest, big-endian. The context must be started again before reuse.
 */
void SHA1_Final(sha1_digest_t digest, sha1_context_t *context)
{
    uint32_t hi, lo, h[5];
    int i;

    /* The length in bits, from 32 bits of blocks and the bytes waiting. */
    hi = context->nblocks >> 23;
    lo = (context->nblocks << 9) + ((uint32_t)context->count << 3);

    context->buf[context->count++] = 0x80;
    if (context->count > 56) {
        memset(context->buf + context->count, 0, 64 - context->count);
        Transform(context, context->buf);
        context->count = 0;
    }
    memset(context->buf + context->count, 0, 56 - context->count);
    for (i = 0; i < 4; i++) {
        context->buf[56 + i] = (byte)(hi >> (24 - 8 * i));
        context->buf[60 + i] = (byte)(lo >> (24 - 8 * i));
    }
    Transform(context, context->buf);

    h[0] = context->h0;
    h[1] = context->h1;
    h[2] = context->h2;
    h[3] = context->h3;
    h[4] = context->h4;
    for (i = 0; i < 20; i++) {
        digest[i] = (byte)(h[i / 4] >> (24 - 8 * (i % 4)));
    }
}

/** Add val to the digest as four bytes, the most significant first. */
void SHA1_UpdateInt32(sha1_context_t *context, unsigned int val)
{
    byte buf[4];

    buf[0] = (byte)(val >> 24);
    buf[1] = (byte)(val >> 16);
    buf[2] = (byte)(val >> 8);
    buf[3] = (byte)val;
    SHA1_Update(context, buf, 4);
}

/** Add the string str to the digest with its terminating NUL. */
void SHA1_UpdateString(sha1_context_t *context, char *str)
{
    SHA1_Update(context, (byte *)str, strlen(str) + 1);
}
