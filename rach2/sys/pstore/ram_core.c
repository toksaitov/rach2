/*
 * ram_core.c: a persistent RAM zone in Linux 6.12's layout
 * (fs/pstore/ram_core.c), and the Reed-Solomon code that guards it.
 */
#include "ram_core.h"

#include "../lib/kerrno.h"
#include "../lib/kstring.h"
#include "../mm/kheap.h"

/* The field of the code, GF(2^8) by the polynomial 0x11d, as Linux's
 * init_rs(8, 0x11d, 0, 1, nroots) builds it (lib/reed_solomon): `alpha_to`
 * gives a power of the primitive element, `index_of` its logarithm, and
 * index_of[0] is RS_NN, which stands for the logarithm of 0. */
#define RS_NN   255u
#define RS_POLY 0x11du

static uint8_t alpha_to[256];
static uint8_t index_of[256];
/* The generator for `gen_roots` parity symbols, in logarithms, lowest
 * degree first: the product of (x + alpha^i) for i from 0. */
static uint8_t genpoly[PRZ_ECC_MAX + 1];
static uint32_t gen_roots;

/** `x` modulo RS_NN, for a sum of two logarithms or more. */
static uint32_t modnn(uint32_t x)
{
    while (x >= RS_NN) {
        x -= RS_NN;
        x = (x >> 8) + (x & RS_NN);
    }
    return x;
}

/** Build the field's tables once, and the generator for `nroots` when the
 * last one built was for another number. */
static void rs_setup(uint32_t nroots)
{
    uint32_t i, j, sr;

    if (index_of[0] != RS_NN) {
        index_of[0] = (uint8_t)RS_NN;
        alpha_to[RS_NN] = 0;
        sr = 1;
        for (i = 0; i < RS_NN; i++) {
            index_of[sr] = (uint8_t)i;
            alpha_to[i] = (uint8_t)sr;
            sr <<= 1;
            if (sr & 0x100u) {
                sr ^= RS_POLY;
            }
            sr &= RS_NN;
        }
    }
    if (gen_roots == nroots) {
        return;
    }
    genpoly[0] = 1;
    for (i = 0; i < nroots; i++) {
        genpoly[i + 1] = 1;
        for (j = i; j > 0; j--) {
            genpoly[j] = genpoly[j] != 0
                             ? (uint8_t)(genpoly[j - 1] ^ alpha_to[modnn(index_of[genpoly[j]] + i)])
                             : genpoly[j - 1];
        }
        genpoly[0] = alpha_to[modnn(index_of[genpoly[0]] + i)];
    }
    for (i = 0; i <= nroots; i++) {
        genpoly[i] = index_of[genpoly[i]];
    }
    gen_roots = nroots;
}

void prz_rs_encode(const uint8_t *data, uint32_t len, uint8_t *par, uint32_t nroots)
{
    uint32_t i, j, fb;

    rs_setup(nroots);
    k_memset(par, 0, nroots);
    for (i = 0; i < len; i++) {
        fb = index_of[data[i] ^ par[0]];
        if (fb != RS_NN) {
            for (j = 1; j < nroots; j++) {
                par[j] ^= alpha_to[modnn(fb + genpoly[nroots - j])];
            }
        }
        for (j = 0; j + 1 < nroots; j++) {
            par[j] = par[j + 1];
        }
        par[nroots - 1] = fb != RS_NN ? alpha_to[modnn(fb + genpoly[0])] : 0;
    }
}

/** The symbol at `pos` of the received word: the data, then the parity. */
static uint8_t *symbol(uint8_t *data, uint32_t len, uint8_t *par, uint32_t pos)
{
    return pos < len ? &data[pos] : &par[pos - len];
}

/**
 * The decoder is the textbook one for a code whose roots are alpha^0 to
 * alpha^(nroots-1): the syndromes, the error locator by Berlekamp and
 * Massey, its roots by Chien's search over the word's positions, and each
 * error's value by Forney's formula. A position is a power of the
 * primitive element: the word's last symbol is degree 0, and the shortened
 * code's absent leading symbols are zeros that no root may name.
 */
/** Multiply the nonzero `a` and `b` in the field. */
static uint8_t gf_mul(uint8_t a, uint8_t b)
{
    return a && b ? alpha_to[modnn((uint32_t)index_of[a] + index_of[b])] : 0;
}

/** `p`, of degree `deg`, at alpha^`e`. */
static uint8_t poly_at(const uint8_t *p, uint32_t deg, uint32_t e)
{
    uint8_t sum = 0;
    uint32_t i;

    for (i = 0; i <= deg; i++) {
        if (p[i]) {
            sum ^= alpha_to[modnn(index_of[p[i]] + e * i % RS_NN)];
        }
    }
    return sum;
}

/** The `nroots` syndromes of the `n`-symbol word into `s`, by Horner's rule
 * at each root; whether any is nonzero. */
static int rs_syndromes(uint8_t *data, uint32_t len, uint8_t *par, uint32_t nroots, uint8_t *s)
{
    uint32_t i, j, any = 0;

    for (i = 0; i < nroots; i++) {
        s[i] = 0;
        for (j = 0; j < len + nroots; j++) {
            s[i] = (uint8_t)(*symbol(data, len, par, j) ^
                             (s[i] ? alpha_to[modnn(index_of[s[i]] + i)] : 0));
        }
        any |= s[i];
    }
    return any != 0;
}

/** The error locator of the syndromes `s` into `lambda`, of PRZ_ECC_MAX + 1
 * terms, by Berlekamp and Massey; its degree. */
static uint32_t rs_locator(const uint8_t *s, uint32_t nroots, uint8_t *lambda)
{
    uint8_t b[PRZ_ECC_MAX + 1], t[PRZ_ECC_MAX + 1], d, bd = 1, x;
    uint32_t i, k, l = 0, m = 1;

    k_memset(lambda, 0, PRZ_ECC_MAX + 1);
    k_memset(b, 0, sizeof(b));
    lambda[0] = b[0] = 1;
    for (k = 0; k < nroots; k++, m++) {
        d = s[k];
        for (i = 1; i <= l; i++) {
            d ^= gf_mul(lambda[i], s[k - i]);
        }
        if (d == 0) {
            continue;
        }
        /* coef = d / bd, as a logarithm */
        x = (uint8_t)modnn(index_of[d] + RS_NN - index_of[bd]);
        k_memcpy(t, lambda, sizeof(t));
        for (i = 0; i + m <= nroots; i++) {
            if (b[i]) {
                lambda[i + m] ^= alpha_to[modnn(index_of[b[i]] + x)];
            }
        }
        if (2 * l <= k) {
            l = k + 1 - l;
            k_memcpy(b, t, sizeof(b));
            bd = d;
            m = 0;
        }
    }
    return l;
}

/** The degree, as a power of alpha, of the word's position `pos` of `n`,
 * negated: the exponent of its inverse. */
static uint32_t inv_degree(uint32_t n, uint32_t pos)
{
    return modnn(RS_NN - (n - 1 - pos) % RS_NN);
}

int prz_rs_decode(uint8_t *data, uint32_t len, uint8_t *par, uint32_t nroots)
{
    uint8_t s[PRZ_ECC_MAX], lambda[PRZ_ECC_MAX + 1], omega[PRZ_ECC_MAX + 1];
    uint8_t val[PRZ_ECC_MAX], den;
    uint32_t loc[PRZ_ECC_MAX];
    uint32_t n = len + nroots, i, j, k, l, found = 0, inv;

    rs_setup(nroots);
    if (!rs_syndromes(data, len, par, nroots, s)) {
        return 0;
    }
    l = rs_locator(s, nroots, lambda);
    if (l == 0 || l > nroots / 2) {
        return -EBADMSG;
    }
    /* Chien: position j holds an error when lambda has the root
     * alpha^-(n-1-j). */
    for (j = 0; j < n && found < l; j++) {
        if (poly_at(lambda, l, inv_degree(n, j)) == 0) {
            loc[found++] = j;
        }
    }
    if (found != l) {
        return -EBADMSG;
    }
    /* omega = s * lambda mod x^nroots */
    for (i = 0; i < nroots; i++) {
        omega[i] = 0;
        for (j = 0; j <= i && j <= l; j++) {
            omega[i] ^= gf_mul(lambda[j], s[i - j]);
        }
    }
    /* Forney, for the first root alpha^0: e = X omega(X^-1) / lambda'(X^-1),
     * lambda' having lambda's odd terms one degree down. */
    for (k = 0; k < found; k++) {
        inv = inv_degree(n, loc[k]);
        den = 0;
        for (i = 1; i <= l; i += 2) {
            den ^= gf_mul(lambda[i], alpha_to[modnn(inv * (i - 1) % RS_NN)]);
        }
        if (den == 0) {
            return -EBADMSG;
        }
        val[k] = gf_mul(gf_mul(poly_at(omega, nroots - 1, inv), alpha_to[modnn(RS_NN - inv)]),
                        alpha_to[modnn(RS_NN - index_of[den])]);
    }
    for (k = 0; k < found; k++) {
        *symbol(data, len, par, loc[k]) ^= val[k];
    }
    return (int)found;
}

/** The 32-bit field of the header at byte `off`, little-endian, read a
 * byte at a time, since the region's address need not be aligned. */
static uint32_t hdr_get(const struct prz *z, uint32_t off)
{
    const volatile uint8_t *p = z->mem + off;

    return (uint32_t)p[0] | (uint32_t)p[1] << 8 | (uint32_t)p[2] << 16 | (uint32_t)p[3] << 24;
}

/** Set the 32-bit field of the header at byte `off`, as `hdr_get` reads it. */
static void hdr_set(struct prz *z, uint32_t off, uint32_t value)
{
    volatile uint8_t *p = z->mem + off;

    p[0] = (uint8_t)value;
    p[1] = (uint8_t)(value >> 8);
    p[2] = (uint8_t)(value >> 16);
    p[3] = (uint8_t)(value >> 24);
}

#define HDR_SIG   0u
#define HDR_START 4u
#define HDR_SIZE  8u

/** Where block `i`'s parity is, and the header's after all of them. */
static volatile uint8_t *par_of(const struct prz *z, uint32_t i)
{
    return z->mem + PRZ_HEADER + z->buffer_size + i * z->ecc_size;
}

/** Copy `n` bytes out of the zone at `src`. */
static void copy_in(uint8_t *dst, const volatile uint8_t *src, uint32_t n)
{
    uint32_t i;

    for (i = 0; i < n; i++) {
        dst[i] = src[i];
    }
}

/** Copy `n` bytes into the zone at `dst`. */
static void copy_out(volatile uint8_t *dst, const uint8_t *src, uint32_t n)
{
    uint32_t i;

    for (i = 0; i < n; i++) {
        dst[i] = src[i];
    }
}

/** Write the parity of the header, as persistent_ram_update_header_ecc. */
static void header_ecc(struct prz *z)
{
    uint8_t h[PRZ_HEADER], par[PRZ_ECC_MAX];

    if (!z->ecc_size) {
        return;
    }
    copy_in(h, z->mem, PRZ_HEADER);
    prz_rs_encode(h, PRZ_HEADER, par, z->ecc_size);
    copy_out(par_of(z, z->ecc_blocks), par, z->ecc_size);
}

/** Write the parity of the blocks that the `count` bytes of data from
 * `start` touch, as persistent_ram_update_ecc (ram_core.c:122); the last
 * block may be short. */
static void data_ecc(struct prz *z, uint32_t start, uint32_t count)
{
    uint8_t block[PRZ_ECC_BLOCK], par[PRZ_ECC_MAX];
    uint32_t at, len;

    if (!z->ecc_size || count == 0) {
        return;
    }
    for (at = start & ~(PRZ_ECC_BLOCK - 1u); at < start + count; at += PRZ_ECC_BLOCK) {
        len = z->buffer_size - at < PRZ_ECC_BLOCK ? z->buffer_size - at : PRZ_ECC_BLOCK;
        copy_in(block, z->mem + PRZ_HEADER + at, len);
        prz_rs_encode(block, len, par, z->ecc_size);
        copy_out(par_of(z, at / PRZ_ECC_BLOCK), par, z->ecc_size);
    }
}

/** Mend the `len` bytes of the zone at `at` by the parity at `par`,
 * counting what was mended or could not be; the mended bytes are written
 * back, as decode_rs8 mends the mapping in place. */
static void mend(struct prz *z, volatile uint8_t *at, uint32_t len, const volatile uint8_t *par)
{
    uint8_t block[PRZ_ECC_BLOCK], p[PRZ_ECC_MAX];
    int n;

    copy_in(block, at, len);
    copy_in(p, par, z->ecc_size);
    n = prz_rs_decode(block, len, p, z->ecc_size);
    if (n > 0) {
        z->corrected_bytes += n;
        copy_out(at, block, len);
    } else if (n < 0) {
        z->bad_blocks++;
    }
}

void prz_zap(struct prz *z)
{
    hdr_set(z, HDR_START, 0);
    hdr_set(z, HDR_SIZE, 0);
    header_ecc(z);
}

void prz_free_old(struct prz *z)
{
    kfree(z->old_log);
    z->old_log = 0;
    z->old_size = 0;
}

/** Copy the data the header describes to `old_log`, oldest byte first,
 * mending each block it holds first, as persistent_ram_save_old with
 * persistent_ram_ecc_old. 0, or -ENOMEM. */
static int save_old(struct prz *z)
{
    uint32_t size = hdr_get(z, HDR_SIZE), start = hdr_get(z, HDR_START), at, len;

    if (size == 0) {
        return 0;
    }
    if (z->ecc_size) {
        for (at = 0; at < size; at += PRZ_ECC_BLOCK) {
            len = z->buffer_size - at < PRZ_ECC_BLOCK ? z->buffer_size - at : PRZ_ECC_BLOCK;
            mend(z, z->mem + PRZ_HEADER + at, len, par_of(z, at / PRZ_ECC_BLOCK));
        }
    }
    z->old_log = kmalloc(size);
    if (!z->old_log) {
        return -ENOMEM;
    }
    z->old_size = size;
    copy_in((uint8_t *)z->old_log, z->mem + PRZ_HEADER + start, size - start);
    copy_in((uint8_t *)z->old_log + size - start, z->mem + PRZ_HEADER, start);
    return 0;
}

int prz_init(struct prz *z, volatile void *mem, uint32_t size, uint32_t sig, uint32_t ecc_size,
             int zap_old)
{
    uint32_t total, held, start;
    int rc = 0, zap = zap_old;

    k_memset(z, 0, sizeof(*z));
    z->mem = (volatile uint8_t *)mem;
    z->size = size;
    z->sig = sig ^ PRZ_SIG;
    if (size <= PRZ_HEADER || ecc_size > PRZ_ECC_MAX) {
        return -EINVAL;
    }
    z->buffer_size = size - PRZ_HEADER;
    if (ecc_size) {
        /* ram_core.c:204: the blocks that fit with their parity, and one
         * more parity for the header. */
        if (z->buffer_size <= ecc_size) {
            return -EINVAL;
        }
        z->ecc_blocks =
            (z->buffer_size - ecc_size + PRZ_ECC_BLOCK + ecc_size - 1) / (PRZ_ECC_BLOCK + ecc_size);
        total = (z->ecc_blocks + 1) * ecc_size;
        if (total >= z->buffer_size) {
            return -EINVAL;
        }
        z->buffer_size -= total;
        z->ecc_size = ecc_size;
        mend(z, z->mem, PRZ_HEADER, par_of(z, z->ecc_blocks));
    }
    held = hdr_get(z, HDR_SIZE);
    start = hdr_get(z, HDR_START);
    if (hdr_get(z, HDR_SIG) != z->sig) {
        hdr_set(z, HDR_SIG, z->sig);
        zap = 1;
    } else if (held > z->buffer_size || start > held) {
        zap = 1;
    } else if (held != 0) {
        rc = save_old(z);
        zap |= rc != 0;
    }
    if (zap) {
        prz_zap(z);
    }
    return rc;
}

void prz_write(struct prz *z, const void *s, uint32_t n)
{
    const uint8_t *p = (const uint8_t *)s;
    uint32_t held, start, rem;

    if (n > z->buffer_size) {
        p += n - z->buffer_size;
        n = z->buffer_size;
    }
    if (n == 0) {
        return;
    }
    held = hdr_get(z, HDR_SIZE);
    if (held != z->buffer_size) {
        hdr_set(z, HDR_SIZE, held + n > z->buffer_size ? z->buffer_size : held + n);
    }
    start = hdr_get(z, HDR_START);
    hdr_set(z, HDR_START, start + n >= z->buffer_size ? start + n - z->buffer_size : start + n);
    rem = z->buffer_size - start;
    if (rem < n) {
        copy_out(z->mem + PRZ_HEADER + start, p, rem);
        data_ecc(z, start, rem);
        p += rem;
        n -= rem;
        start = 0;
    }
    copy_out(z->mem + PRZ_HEADER + start, p, n);
    data_ecc(z, start, n);
    header_ecc(z);
}

/** Append the decimal digits of `v` to `buf` at `*at`, within `cap`. */
static void put_num(char *buf, uint32_t cap, uint32_t *at, uint32_t v)
{
    char digits[10];
    int n = 0;

    do {
        digits[n++] = (char)('0' + v % 10u);
        v /= 10u;
    } while (v);
    while (n > 0) {
        n--;
        if (*at + 1 < cap) {
            buf[*at] = digits[n];
        }
        (*at)++;
    }
}

/** Append `s` to `buf` at `*at`, within `cap`. */
static void put_str(char *buf, uint32_t cap, uint32_t *at, const char *s)
{
    for (; *s; s++, (*at)++) {
        if (*at + 1 < cap) {
            buf[*at] = *s;
        }
    }
}

uint32_t prz_ecc_string(const struct prz *z, char *buf, uint32_t cap)
{
    uint32_t at = 0;

    if (!z->ecc_size) {
        return 0;
    }
    if (z->corrected_bytes || z->bad_blocks) {
        put_str(buf, cap, &at, "\nECC: ");
        put_num(buf, cap, &at, (uint32_t)z->corrected_bytes);
        put_str(buf, cap, &at, " Corrected bytes, ");
        put_num(buf, cap, &at, (uint32_t)z->bad_blocks);
        put_str(buf, cap, &at, " unrecoverable blocks\n");
    } else {
        put_str(buf, cap, &at, "\nECC: No errors detected\n");
    }
    if (cap > 0) {
        buf[at < cap ? at : cap - 1] = '\0';
    }
    return at;
}
