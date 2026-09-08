/*
 * sha256.c: the digest itself. The contract is in sha256.h.
 *
 * The state keeps its eight words in `unsigned long`, which is at least 32
 * bits everywhere this is built, and masks every result back to 32 bits, so
 * the arithmetic is the same on a host where that type is wider.
 */

#include "sha256.h"

#include <string.h>

#define M32 0xFFFFFFFFUL

static const unsigned long K[64] = {
    0x428a2f98UL, 0x71374491UL, 0xb5c0fbcfUL, 0xe9b5dba5UL, 0x3956c25bUL, 0x59f111f1UL,
    0x923f82a4UL, 0xab1c5ed5UL, 0xd807aa98UL, 0x12835b01UL, 0x243185beUL, 0x550c7dc3UL,
    0x72be5d74UL, 0x80deb1feUL, 0x9bdc06a7UL, 0xc19bf174UL, 0xe49b69c1UL, 0xefbe4786UL,
    0x0fc19dc6UL, 0x240ca1ccUL, 0x2de92c6fUL, 0x4a7484aaUL, 0x5cb0a9dcUL, 0x76f988daUL,
    0x983e5152UL, 0xa831c66dUL, 0xb00327c8UL, 0xbf597fc7UL, 0xc6e00bf3UL, 0xd5a79147UL,
    0x06ca6351UL, 0x14292967UL, 0x27b70a85UL, 0x2e1b2138UL, 0x4d2c6dfcUL, 0x53380d13UL,
    0x650a7354UL, 0x766a0abbUL, 0x81c2c92eUL, 0x92722c85UL, 0xa2bfe8a1UL, 0xa81a664bUL,
    0xc24b8b70UL, 0xc76c51a3UL, 0xd192e819UL, 0xd6990624UL, 0xf40e3585UL, 0x106aa070UL,
    0x19a4c116UL, 0x1e376c08UL, 0x2748774cUL, 0x34b0bcb5UL, 0x391c0cb3UL, 0x4ed8aa4aUL,
    0x5b9cca4fUL, 0x682e6ff3UL, 0x748f82eeUL, 0x78a5636fUL, 0x84c87814UL, 0x8cc70208UL,
    0x90befffaUL, 0xa4506cebUL, 0xbef9a3f7UL, 0xc67178f2UL};

static unsigned long ror(unsigned long x, int n)
{
    return ((x >> n) | (x << (32 - n))) & M32;
}

static void compress(struct sha256 *s, const unsigned char *p)
{
    unsigned long w[64], a, b, c, d, e, f, g, h;
    int i;

    for (i = 0; i < 16; i++) {
        w[i] = ((unsigned long)p[4 * i] << 24) | ((unsigned long)p[4 * i + 1] << 16) |
               ((unsigned long)p[4 * i + 2] << 8) | (unsigned long)p[4 * i + 3];
    }
    for (i = 16; i < 64; i++) {
        unsigned long s0 = ror(w[i - 15], 7) ^ ror(w[i - 15], 18) ^ (w[i - 15] >> 3);
        unsigned long s1 = ror(w[i - 2], 17) ^ ror(w[i - 2], 19) ^ (w[i - 2] >> 10);

        w[i] = (w[i - 16] + s0 + w[i - 7] + s1) & M32;
    }
    a = s->h[0];
    b = s->h[1];
    c = s->h[2];
    d = s->h[3];
    e = s->h[4];
    f = s->h[5];
    g = s->h[6];
    h = s->h[7];
    for (i = 0; i < 64; i++) {
        unsigned long s1 = ror(e, 6) ^ ror(e, 11) ^ ror(e, 25);
        unsigned long ch = (e & f) ^ (~e & g);
        unsigned long t1 = (h + s1 + (ch & M32) + K[i] + w[i]) & M32;
        unsigned long s0 = ror(a, 2) ^ ror(a, 13) ^ ror(a, 22);
        unsigned long mj = (a & b) ^ (a & c) ^ (b & c);
        unsigned long t2 = (s0 + mj) & M32;

        h = g;
        g = f;
        f = e;
        e = (d + t1) & M32;
        d = c;
        c = b;
        b = a;
        a = (t1 + t2) & M32;
    }
    s->h[0] = (s->h[0] + a) & M32;
    s->h[1] = (s->h[1] + b) & M32;
    s->h[2] = (s->h[2] + c) & M32;
    s->h[3] = (s->h[3] + d) & M32;
    s->h[4] = (s->h[4] + e) & M32;
    s->h[5] = (s->h[5] + f) & M32;
    s->h[6] = (s->h[6] + g) & M32;
    s->h[7] = (s->h[7] + h) & M32;
}

void sha256_init(struct sha256 *s)
{
    static const unsigned long h0[8] = {0x6a09e667UL, 0xbb67ae85UL, 0x3c6ef372UL, 0xa54ff53aUL,
                                        0x510e527fUL, 0x9b05688cUL, 0x1f83d9abUL, 0x5be0cd19UL};

    memcpy(s->h, h0, sizeof(h0));
    s->bytes = 0;
    s->fill = 0;
}

void sha256_update(struct sha256 *s, const unsigned char *p, size_t n)
{
    s->bytes += n;
    while (n > 0) {
        size_t take = 64 - s->fill;

        if (take > n) {
            take = n;
        }
        memcpy(s->block + s->fill, p, take);
        s->fill += take;
        p += take;
        n -= take;
        if (s->fill == 64) {
            compress(s, s->block);
            s->fill = 0;
        }
    }
}

void sha256_final(struct sha256 *s, unsigned char out[32])
{
    unsigned long long bits = s->bytes * 8;
    unsigned char pad = 0x80;
    unsigned char zero = 0;
    unsigned char len[8];
    int i;

    /* The padding is not part of the message, so the length this keeps is put
       back after each byte of it. */
    sha256_update(s, &pad, 1);
    s->bytes -= 1;
    while (s->fill != 56) {
        sha256_update(s, &zero, 1);
        s->bytes -= 1;
    }
    for (i = 0; i < 8; i++) {
        len[i] = (unsigned char)(bits >> (56 - 8 * i));
    }
    sha256_update(s, len, 8);
    for (i = 0; i < 8; i++) {
        out[4 * i] = (unsigned char)(s->h[i] >> 24);
        out[4 * i + 1] = (unsigned char)(s->h[i] >> 16);
        out[4 * i + 2] = (unsigned char)(s->h[i] >> 8);
        out[4 * i + 3] = (unsigned char)s->h[i];
    }
}

void sha256_hex(const unsigned char digest[32], char out[65])
{
    static const char hex[] = "0123456789abcdef";
    int i;

    for (i = 0; i < 32; i++) {
        out[2 * i] = hex[(digest[i] >> 4) & 0xF];
        out[2 * i + 1] = hex[digest[i] & 0xF];
    }
    out[64] = '\0';
}
