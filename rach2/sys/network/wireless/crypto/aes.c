/*
 * aes.c: AES-128 in constant time, and AES-CMAC, for the Wi-Fi ciphers.
 *
 * The cipher is BearSSL's aes_ct, at tag v0.6 of
 * https://bearssl.org/git/BearSSL: aes_ct_sbox, aes_ct_ortho and the key
 * schedule follow src/symcipher/aes_ct.c (br_aes_ct_bitslice_Sbox,
 * br_aes_ct_ortho, br_aes_ct_keysched, br_aes_ct_skey_expand), and the
 * rounds follow src/symcipher/aes_ct_enc.c (br_aes_ct_bitslice_encrypt).
 * The S-box is a Boolean circuit over bitsliced words, so no table is
 * indexed by a secret byte and the time taken does not depend on the key or
 * the data. A pass holds two blocks, which wcrypto_aes2 uses for CCM.
 * Rach2 takes only AES-128 encryption, and keeps the compressed schedule
 * in struct wlan_aes128.
 *
 * The copied code keeps its permission notice:
 *
 * Copyright (c) 2016 Thomas Pornin <pornin@bolet.org>
 *
 * Permission is hereby granted, free of charge, to any person obtaining
 * a copy of this software and associated documentation files (the
 * "Software"), to deal in the Software without restriction, including
 * without limitation the rights to use, copy, modify, merge, publish,
 * distribute, sublicense, and/or sell copies of the Software, and to
 * permit persons to whom the Software is furnished to do so, subject to
 * the following conditions:
 *
 * The above copyright notice and this permission notice shall be
 * included in all copies or substantial portions of the Software.
 *
 * THE SOFTWARE IS PROVIDED "AS IS", WITHOUT WARRANTY OF ANY KIND,
 * EXPRESS OR IMPLIED, INCLUDING BUT NOT LIMITED TO THE WARRANTIES OF
 * MERCHANTABILITY, FITNESS FOR A PARTICULAR PURPOSE AND
 * NONINFRINGEMENT. IN NO EVENT SHALL THE AUTHORS OR COPYRIGHT HOLDERS
 * BE LIABLE FOR ANY CLAIM, DAMAGES OR OTHER LIABILITY, WHETHER IN AN
 * ACTION OF CONTRACT, TORT OR OTHERWISE, ARISING FROM, OUT OF OR IN
 * CONNECTION WITH THE SOFTWARE OR THE USE OR OTHER DEALINGS IN THE
 * SOFTWARE.
 */
#include "wcrypto_int.h"

/** Read a little-endian 32-bit word from four bytes. */
static uint32_t aes_dec32le(const uint8_t *p)
{
    return (uint32_t)p[0] | (uint32_t)p[1] << 8 | (uint32_t)p[2] << 16 | (uint32_t)p[3] << 24;
}

/** Write a 32-bit word as four little-endian bytes. */
static void aes_enc32le(uint8_t *p, uint32_t x)
{
    p[0] = (uint8_t)x;
    p[1] = (uint8_t)(x >> 8);
    p[2] = (uint8_t)(x >> 16);
    p[3] = (uint8_t)(x >> 24);
}

/** Apply the S-box to each of the 32 bytes held bitsliced in q[0..7]. */
static void aes_ct_sbox(uint32_t *q)
{
    /*
     * This S-box implementation is a straightforward translation of
     * the circuit described by Boyar and Peralta in "A new
     * combinational logic minimization technique with applications
     * to cryptology" (https://eprint.iacr.org/2009/191.pdf).
     *
     * Note that variables x* (input) and s* (output) are numbered
     * in "reverse" order (x0 is the high bit, x7 is the low bit).
     */

    uint32_t x0, x1, x2, x3, x4, x5, x6, x7;
    uint32_t y1, y2, y3, y4, y5, y6, y7, y8, y9;
    uint32_t y10, y11, y12, y13, y14, y15, y16, y17, y18, y19;
    uint32_t y20, y21;
    uint32_t z0, z1, z2, z3, z4, z5, z6, z7, z8, z9;
    uint32_t z10, z11, z12, z13, z14, z15, z16, z17;
    uint32_t t0, t1, t2, t3, t4, t5, t6, t7, t8, t9;
    uint32_t t10, t11, t12, t13, t14, t15, t16, t17, t18, t19;
    uint32_t t20, t21, t22, t23, t24, t25, t26, t27, t28, t29;
    uint32_t t30, t31, t32, t33, t34, t35, t36, t37, t38, t39;
    uint32_t t40, t41, t42, t43, t44, t45, t46, t47, t48, t49;
    uint32_t t50, t51, t52, t53, t54, t55, t56, t57, t58, t59;
    uint32_t t60, t61, t62, t63, t64, t65, t66, t67;
    uint32_t s0, s1, s2, s3, s4, s5, s6, s7;

    x0 = q[7];
    x1 = q[6];
    x2 = q[5];
    x3 = q[4];
    x4 = q[3];
    x5 = q[2];
    x6 = q[1];
    x7 = q[0];

    /*
     * Top linear transformation.
     */
    y14 = x3 ^ x5;
    y13 = x0 ^ x6;
    y9 = x0 ^ x3;
    y8 = x0 ^ x5;
    t0 = x1 ^ x2;
    y1 = t0 ^ x7;
    y4 = y1 ^ x3;
    y12 = y13 ^ y14;
    y2 = y1 ^ x0;
    y5 = y1 ^ x6;
    y3 = y5 ^ y8;
    t1 = x4 ^ y12;
    y15 = t1 ^ x5;
    y20 = t1 ^ x1;
    y6 = y15 ^ x7;
    y10 = y15 ^ t0;
    y11 = y20 ^ y9;
    y7 = x7 ^ y11;
    y17 = y10 ^ y11;
    y19 = y10 ^ y8;
    y16 = t0 ^ y11;
    y21 = y13 ^ y16;
    y18 = x0 ^ y16;

    /*
     * Non-linear section.
     */
    t2 = y12 & y15;
    t3 = y3 & y6;
    t4 = t3 ^ t2;
    t5 = y4 & x7;
    t6 = t5 ^ t2;
    t7 = y13 & y16;
    t8 = y5 & y1;
    t9 = t8 ^ t7;
    t10 = y2 & y7;
    t11 = t10 ^ t7;
    t12 = y9 & y11;
    t13 = y14 & y17;
    t14 = t13 ^ t12;
    t15 = y8 & y10;
    t16 = t15 ^ t12;
    t17 = t4 ^ t14;
    t18 = t6 ^ t16;
    t19 = t9 ^ t14;
    t20 = t11 ^ t16;
    t21 = t17 ^ y20;
    t22 = t18 ^ y19;
    t23 = t19 ^ y21;
    t24 = t20 ^ y18;

    t25 = t21 ^ t22;
    t26 = t21 & t23;
    t27 = t24 ^ t26;
    t28 = t25 & t27;
    t29 = t28 ^ t22;
    t30 = t23 ^ t24;
    t31 = t22 ^ t26;
    t32 = t31 & t30;
    t33 = t32 ^ t24;
    t34 = t23 ^ t33;
    t35 = t27 ^ t33;
    t36 = t24 & t35;
    t37 = t36 ^ t34;
    t38 = t27 ^ t36;
    t39 = t29 & t38;
    t40 = t25 ^ t39;

    t41 = t40 ^ t37;
    t42 = t29 ^ t33;
    t43 = t29 ^ t40;
    t44 = t33 ^ t37;
    t45 = t42 ^ t41;
    z0 = t44 & y15;
    z1 = t37 & y6;
    z2 = t33 & x7;
    z3 = t43 & y16;
    z4 = t40 & y1;
    z5 = t29 & y7;
    z6 = t42 & y11;
    z7 = t45 & y17;
    z8 = t41 & y10;
    z9 = t44 & y12;
    z10 = t37 & y3;
    z11 = t33 & y4;
    z12 = t43 & y13;
    z13 = t40 & y5;
    z14 = t29 & y2;
    z15 = t42 & y9;
    z16 = t45 & y14;
    z17 = t41 & y8;

    /*
     * Bottom linear transformation.
     */
    t46 = z15 ^ z16;
    t47 = z10 ^ z11;
    t48 = z5 ^ z13;
    t49 = z9 ^ z10;
    t50 = z2 ^ z12;
    t51 = z2 ^ z5;
    t52 = z7 ^ z8;
    t53 = z0 ^ z3;
    t54 = z6 ^ z7;
    t55 = z16 ^ z17;
    t56 = z12 ^ t48;
    t57 = t50 ^ t53;
    t58 = z4 ^ t46;
    t59 = z3 ^ t54;
    t60 = t46 ^ t57;
    t61 = z14 ^ t57;
    t62 = t52 ^ t58;
    t63 = t49 ^ t58;
    t64 = z4 ^ t59;
    t65 = t61 ^ t62;
    t66 = z1 ^ t63;
    s0 = t59 ^ t63;
    s6 = t56 ^ ~t62;
    s7 = t48 ^ ~t60;
    t67 = t64 ^ t65;
    s3 = t53 ^ t66;
    s4 = t51 ^ t66;
    s5 = t47 ^ t65;
    s1 = t64 ^ ~s3;
    s2 = t55 ^ ~t67;

    q[7] = s0;
    q[6] = s1;
    q[5] = s2;
    q[4] = s3;
    q[3] = s4;
    q[2] = s5;
    q[1] = s6;
    q[0] = s7;
}

/** Transpose q[0..7] between bytes in words and bit planes; the transform
 * is its own inverse. */
static void aes_ct_ortho(uint32_t *q)
{
#define SWAPN(cl, ch, s, x, y)                                  \
    do {                                                        \
        uint32_t a, b;                                          \
        a = (x);                                                \
        b = (y);                                                \
        (x) = (a & (uint32_t)cl) | ((b & (uint32_t)cl) << (s)); \
        (y) = ((a & (uint32_t)ch) >> (s)) | (b & (uint32_t)ch); \
    } while (0)

#define SWAP2(x, y) SWAPN(0x55555555, 0xAAAAAAAA, 1, x, y)
#define SWAP4(x, y) SWAPN(0x33333333, 0xCCCCCCCC, 2, x, y)
#define SWAP8(x, y) SWAPN(0x0F0F0F0F, 0xF0F0F0F0, 4, x, y)

    SWAP2(q[0], q[1]);
    SWAP2(q[2], q[3]);
    SWAP2(q[4], q[5]);
    SWAP2(q[6], q[7]);

    SWAP4(q[0], q[2]);
    SWAP4(q[1], q[3]);
    SWAP4(q[4], q[6]);
    SWAP4(q[5], q[7]);

    SWAP8(q[0], q[4]);
    SWAP8(q[1], q[5]);
    SWAP8(q[2], q[6]);
    SWAP8(q[3], q[7]);
}

static const uint8_t aes_rcon[10] = {0x01, 0x02, 0x04, 0x08, 0x10, 0x20, 0x40, 0x80, 0x1B, 0x36};

/** Apply the S-box to each byte of one word, for the key schedule. */
static uint32_t aes_sub_word(uint32_t x)
{
    uint32_t q[8];
    int i;

    for (i = 0; i < 8; i++)
        q[i] = x;
    aes_ct_ortho(q);
    aes_ct_sbox(q);
    aes_ct_ortho(q);
    return q[0];
}

/**
 * Expand a 16-byte key into ctx->round_key: the 44 words of the AES-128
 * schedule, transposed and compressed to one word each as
 * br_aes_ct_keysched leaves them. aes_ct_expand restores the eight-word
 * form of each round key for a pass.
 */
void wlan_aes128_setkey(struct wlan_aes128 *ctx, const uint8_t key[16])
{
    uint32_t skey[88];
    uint32_t tmp;
    int i, j, k;

    tmp = 0;
    for (i = 0; i < 4; i++) {
        tmp = aes_dec32le(key + (i << 2));
        skey[(i << 1) + 0] = tmp;
        skey[(i << 1) + 1] = tmp;
    }
    for (i = 4, j = 0, k = 0; i < 44; i++) {
        if (j == 0) {
            tmp = (tmp << 24) | (tmp >> 8);
            tmp = aes_sub_word(tmp) ^ aes_rcon[k];
        }
        tmp ^= skey[(i - 4) << 1];
        skey[(i << 1) + 0] = tmp;
        skey[(i << 1) + 1] = tmp;
        if (++j == 4) {
            j = 0;
            k++;
        }
    }
    for (i = 0; i < 44; i += 4)
        aes_ct_ortho(skey + (i << 1));
    for (i = 0, j = 0; i < 44; i++, j += 2)
        ctx->round_key[i] = (skey[j + 0] & 0x55555555) | (skey[j + 1] & 0xAAAAAAAA);
    wcrypto_wipe(skey, sizeof(skey));
}

/** Expand the compressed schedule to the 88 words a pass reads. */
static void aes_ct_expand(uint32_t skey[88], const struct wlan_aes128 *ctx)
{
    unsigned u, v;

    for (u = 0, v = 0; u < 44; u++, v += 2) {
        uint32_t x, y;

        x = y = ctx->round_key[u];
        x &= 0x55555555;
        skey[v + 0] = x | (x << 1);
        y &= 0xAAAAAAAA;
        skey[v + 1] = y | (y >> 1);
    }
}

/** XOR one eight-word round key into the state. */
static void aes_ct_add_round_key(uint32_t *q, const uint32_t *sk)
{
    int i;

    for (i = 0; i < 8; i++)
        q[i] ^= sk[i];
}

/** Rotate each row of the bitsliced state, as ShiftRows does. */
static void aes_ct_shift_rows(uint32_t *q)
{
    int i;

    for (i = 0; i < 8; i++) {
        uint32_t x;

        x = q[i];
        q[i] = (x & 0x000000FF) | ((x & 0x0000FC00) >> 2) | ((x & 0x00000300) << 6) |
               ((x & 0x00F00000) >> 4) | ((x & 0x000F0000) << 4) | ((x & 0xC0000000) >> 6) |
               ((x & 0x3F000000) << 2);
    }
}

/** Rotate a word by 16 bits. */
static uint32_t aes_rotr16(uint32_t x)
{
    return (x << 16) | (x >> 16);
}

/** Multiply each column of the bitsliced state, as MixColumns does. */
static void aes_ct_mix_columns(uint32_t *q)
{
    uint32_t q0, q1, q2, q3, q4, q5, q6, q7;
    uint32_t r0, r1, r2, r3, r4, r5, r6, r7;

    q0 = q[0];
    q1 = q[1];
    q2 = q[2];
    q3 = q[3];
    q4 = q[4];
    q5 = q[5];
    q6 = q[6];
    q7 = q[7];
    r0 = (q0 >> 8) | (q0 << 24);
    r1 = (q1 >> 8) | (q1 << 24);
    r2 = (q2 >> 8) | (q2 << 24);
    r3 = (q3 >> 8) | (q3 << 24);
    r4 = (q4 >> 8) | (q4 << 24);
    r5 = (q5 >> 8) | (q5 << 24);
    r6 = (q6 >> 8) | (q6 << 24);
    r7 = (q7 >> 8) | (q7 << 24);

    q[0] = q7 ^ r7 ^ r0 ^ aes_rotr16(q0 ^ r0);
    q[1] = q0 ^ r0 ^ q7 ^ r7 ^ r1 ^ aes_rotr16(q1 ^ r1);
    q[2] = q1 ^ r1 ^ r2 ^ aes_rotr16(q2 ^ r2);
    q[3] = q2 ^ r2 ^ q7 ^ r7 ^ r3 ^ aes_rotr16(q3 ^ r3);
    q[4] = q3 ^ r3 ^ q7 ^ r7 ^ r4 ^ aes_rotr16(q4 ^ r4);
    q[5] = q4 ^ r4 ^ r5 ^ aes_rotr16(q5 ^ r5);
    q[6] = q5 ^ r5 ^ r6 ^ aes_rotr16(q6 ^ r6);
    q[7] = q6 ^ r6 ^ r7 ^ aes_rotr16(q7 ^ r7);
}

/** Run the ten rounds over two blocks: a in the even words of q, b in the
 * odd ones, each word four bytes little endian. */
static void aes_ct_encrypt(const struct wlan_aes128 *ctx, uint32_t q[8])
{
    uint32_t skey[88];
    unsigned u;

    aes_ct_expand(skey, ctx);
    aes_ct_ortho(q);
    aes_ct_add_round_key(q, skey);
    for (u = 1; u < 10; u++) {
        aes_ct_sbox(q);
        aes_ct_shift_rows(q);
        aes_ct_mix_columns(q);
        aes_ct_add_round_key(q, skey + (u << 3));
    }
    aes_ct_sbox(q);
    aes_ct_shift_rows(q);
    aes_ct_add_round_key(q, skey + (10 << 3));
    aes_ct_ortho(q);
    wcrypto_wipe(skey, sizeof(skey));
}

/** Encrypt two blocks in place in one bitsliced pass. */
void wcrypto_aes2(const struct wlan_aes128 *ctx, uint8_t a[16], uint8_t b[16])
{
    uint32_t q[8];
    int i;

    for (i = 0; i < 4; i++) {
        q[i << 1] = aes_dec32le(a + (i << 2));
        q[(i << 1) + 1] = aes_dec32le(b + (i << 2));
    }
    aes_ct_encrypt(ctx, q);
    for (i = 0; i < 4; i++) {
        aes_enc32le(a + (i << 2), q[i << 1]);
        aes_enc32le(b + (i << 2), q[(i << 1) + 1]);
    }
    wcrypto_wipe(q, sizeof(q));
}

/** Encrypt one block; the second slot of the pass carries zeros. */
void wlan_aes128_encrypt(const struct wlan_aes128 *ctx, const uint8_t in[16], uint8_t out[16])
{
    uint32_t q[8];
    int i;

    for (i = 0; i < 4; i++) {
        q[i << 1] = aes_dec32le(in + (i << 2));
        q[(i << 1) + 1] = 0;
    }
    aes_ct_encrypt(ctx, q);
    for (i = 0; i < 4; i++)
        aes_enc32le(out + (i << 2), q[i << 1]);
    wcrypto_wipe(q, sizeof(q));
}

/** Double a block in GF(2^128) with the CMAC polynomial, for a subkey of
 * NIST SP 800-38B; the reduction is masked, not branched on. */
static void cmac_double(uint8_t b[16])
{
    uint8_t carry;
    int i;

    carry = (uint8_t)(0 - (b[0] >> 7));
    for (i = 0; i < 15; i++)
        b[i] = (uint8_t)(b[i] << 1 | b[i + 1] >> 7);
    b[15] = (uint8_t)(b[15] << 1 ^ (carry & 0x87));
}

/** Start a CMAC with an empty chain. */
void wcrypto_cmac_init(struct wcrypto_cmac *st, const struct wlan_aes128 *ctx)
{
    int i;

    st->ctx = ctx;
    for (i = 0; i < 16; i++)
        st->x[i] = 0;
    st->n = 0;
}

/** Chain each full block once a later byte shows it is not the last. */
void wcrypto_cmac_update(struct wcrypto_cmac *st, const uint8_t *data, unsigned len)
{
    unsigned i;

    while (len > 0) {
        if (st->n == 16) {
            for (i = 0; i < 16; i++)
                st->x[i] ^= st->buf[i];
            wlan_aes128_encrypt(st->ctx, st->x, st->x);
            st->n = 0;
        }
        while (len > 0 && st->n < 16) {
            st->buf[st->n++] = *data++;
            --len;
        }
    }
}

/** Pad or not, mask with subkey K1 or K2, and encrypt the last block. */
void wcrypto_cmac_final(struct wcrypto_cmac *st, uint8_t out[16])
{
    uint8_t k[16];
    unsigned i;

    for (i = 0; i < 16; i++)
        k[i] = 0;
    wlan_aes128_encrypt(st->ctx, k, k);
    cmac_double(k);
    if (st->n < 16) {
        cmac_double(k);
        st->buf[st->n] = 0x80;
        for (i = st->n + 1; i < 16; i++)
            st->buf[i] = 0;
    }
    for (i = 0; i < 16; i++)
        st->x[i] ^= st->buf[i] ^ k[i];
    wlan_aes128_encrypt(st->ctx, st->x, out);
    wcrypto_wipe(k, sizeof(k));
    wcrypto_wipe(st, sizeof(*st));
}

/** Compute the CMAC of one contiguous message. */
void wlan_aes128_cmac(const struct wlan_aes128 *ctx, const uint8_t *data, unsigned len,
                      uint8_t out[16])
{
    struct wcrypto_cmac st;

    wcrypto_cmac_init(&st, ctx);
    wcrypto_cmac_update(&st, data, len);
    wcrypto_cmac_final(&st, out);
}

/** Compare by accumulating differences, with no early exit. */
int wcrypto_equal(const uint8_t *a, const uint8_t *b, unsigned n)
{
    unsigned i;
    uint8_t d;

    d = 0;
    for (i = 0; i < n; i++)
        d |= (uint8_t)(a[i] ^ b[i]);
    return d == 0;
}

/** Zero through a volatile pointer, which the compiler may not drop. */
void wcrypto_wipe(void *p, unsigned n)
{
    volatile uint8_t *v;

    v = (volatile uint8_t *)p;
    while (n-- > 0)
        *v++ = 0;
}
