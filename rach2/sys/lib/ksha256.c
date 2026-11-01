/* SHA-256 (FIPS 180-4), as ksha256.h describes it. */
#include "ksha256.h"

static const uint32_t k256[64] = {
    0x428a2f98u, 0x71374491u, 0xb5c0fbcfu, 0xe9b5dba5u, 0x3956c25bu, 0x59f111f1u, 0x923f82a4u,
    0xab1c5ed5u, 0xd807aa98u, 0x12835b01u, 0x243185beu, 0x550c7dc3u, 0x72be5d74u, 0x80deb1feu,
    0x9bdc06a7u, 0xc19bf174u, 0xe49b69c1u, 0xefbe4786u, 0x0fc19dc6u, 0x240ca1ccu, 0x2de92c6fu,
    0x4a7484aau, 0x5cb0a9dcu, 0x76f988dau, 0x983e5152u, 0xa831c66du, 0xb00327c8u, 0xbf597fc7u,
    0xc6e00bf3u, 0xd5a79147u, 0x06ca6351u, 0x14292967u, 0x27b70a85u, 0x2e1b2138u, 0x4d2c6dfcu,
    0x53380d13u, 0x650a7354u, 0x766a0abbu, 0x81c2c92eu, 0x92722c85u, 0xa2bfe8a1u, 0xa81a664bu,
    0xc24b8b70u, 0xc76c51a3u, 0xd192e819u, 0xd6990624u, 0xf40e3585u, 0x106aa070u, 0x19a4c116u,
    0x1e376c08u, 0x2748774cu, 0x34b0bcb5u, 0x391c0cb3u, 0x4ed8aa4au, 0x5b9cca4fu, 0x682e6ff3u,
    0x748f82eeu, 0x78a5636fu, 0x84c87814u, 0x8cc70208u, 0x90befffau, 0xa4506cebu, 0xbef9a3f7u,
    0xc67178f2u};

#define ROR(x, n) (((x) >> (n)) | ((x) << (32 - (n))))

/** Compress the 64 bytes in `c->block` into `c->h`. */
static void ksha256_block(struct ksha256 *c)
{
    uint32_t w[64], a, b, d, e, f, g, h, cc, t1, t2;
    int i;

    for (i = 0; i < 16; i++) {
        w[i] = ((uint32_t)c->block[4 * i] << 24) | ((uint32_t)c->block[4 * i + 1] << 16) |
               ((uint32_t)c->block[4 * i + 2] << 8) | c->block[4 * i + 3];
    }
    for (i = 16; i < 64; i++) {
        uint32_t s0 = ROR(w[i - 15], 7) ^ ROR(w[i - 15], 18) ^ (w[i - 15] >> 3);
        uint32_t s1 = ROR(w[i - 2], 17) ^ ROR(w[i - 2], 19) ^ (w[i - 2] >> 10);

        w[i] = w[i - 16] + s0 + w[i - 7] + s1;
    }
    a = c->h[0];
    b = c->h[1];
    cc = c->h[2];
    d = c->h[3];
    e = c->h[4];
    f = c->h[5];
    g = c->h[6];
    h = c->h[7];
    for (i = 0; i < 64; i++) {
        t1 = h + (ROR(e, 6) ^ ROR(e, 11) ^ ROR(e, 25)) + ((e & f) ^ (~e & g)) + k256[i] + w[i];
        t2 = (ROR(a, 2) ^ ROR(a, 13) ^ ROR(a, 22)) + ((a & b) ^ (a & cc) ^ (b & cc));
        h = g;
        g = f;
        f = e;
        e = d + t1;
        d = cc;
        cc = b;
        b = a;
        a = t1 + t2;
    }
    c->h[0] += a;
    c->h[1] += b;
    c->h[2] += cc;
    c->h[3] += d;
    c->h[4] += e;
    c->h[5] += f;
    c->h[6] += g;
    c->h[7] += h;
}

/** The initial state words of FIPS 180-4, 5.3.3. */
void ksha256_init(struct ksha256 *c)
{
    c->h[0] = 0x6a09e667u;
    c->h[1] = 0xbb67ae85u;
    c->h[2] = 0x3c6ef372u;
    c->h[3] = 0xa54ff53au;
    c->h[4] = 0x510e527fu;
    c->h[5] = 0x9b05688cu;
    c->h[6] = 0x1f83d9abu;
    c->h[7] = 0x5be0cd19u;
    c->fill = 0;
    c->len_lo = 0;
    c->len_hi = 0;
}

void ksha256_update(struct ksha256 *c, const void *data, uint32_t n)
{
    const uint8_t *p = (const uint8_t *)data;

    if (c->len_lo + n < c->len_lo) {
        c->len_hi++;
    }
    c->len_lo += n;
    while (n-- > 0) {
        c->block[c->fill++] = *p++;
        if (c->fill == 64) {
            ksha256_block(c);
            c->fill = 0;
        }
    }
}

/** The padding of FIPS 180-4, 5.1.1: a one bit, zeros, and the length in
 * bits as 64 bits, big-endian. */
void ksha256_final(struct ksha256 *c, uint8_t *out)
{
    uint32_t hi = (c->len_hi << 3) | (c->len_lo >> 29), lo = c->len_lo << 3;
    int i;

    c->block[c->fill++] = 0x80;
    if (c->fill > 56) {
        while (c->fill < 64) {
            c->block[c->fill++] = 0;
        }
        ksha256_block(c);
        c->fill = 0;
    }
    while (c->fill < 56) {
        c->block[c->fill++] = 0;
    }
    for (i = 0; i < 4; i++) {
        c->block[56 + i] = (uint8_t)(hi >> (24 - 8 * i));
        c->block[60 + i] = (uint8_t)(lo >> (24 - 8 * i));
    }
    ksha256_block(c);
    for (i = 0; i < 32; i++) {
        out[i] = (uint8_t)(c->h[i / 4] >> (24 - 8 * (i % 4)));
    }
}
