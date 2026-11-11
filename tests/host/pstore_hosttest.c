/**
 * The persistent RAM zones of ramoops (sys/pstore/ram_core.c): the
 * Reed-Solomon code's encoder and decoder, a zone's header and wrap, what a
 * second boot finds in it, and the parity's mending of the data and the
 * header, against a zone in host memory.
 *
 * host-deps: rach2/sys/lib/kstring.c rach2/sys/lib/kctypes.c
 * host-deps: tests/host/compat/kheap_host.c
 */
extern int printf(const char *format, ...);

#include "pstore/ram_core.c"

static int failures;

#define CHECK(test)                                                    \
    do {                                                               \
        if (!(test)) {                                                 \
            printf("pstore_hosttest: line %d: %s\n", __LINE__, #test); \
            failures++;                                                \
        }                                                              \
    } while (0)

static uint32_t seed = 12345u;

/** A pseudo-random number, the same on every run. */
static uint32_t next(void)
{
    seed = seed * 1103515245u + 12345u;
    return seed >> 8;
}

/** Whether the `n` bytes at `a` and `b` match. */
static int same(const void *a, const void *b, uint32_t n)
{
    const uint8_t *x = (const uint8_t *)a, *y = (const uint8_t *)b;
    uint32_t i;

    for (i = 0; i < n; i++) {
        if (x[i] != y[i]) {
            return 0;
        }
    }
    return 1;
}

/** Encode a random block of `len` bytes with `nroots` parity bytes, check
 * that it is a code word, then flip `errs` symbols, the last first, and
 * check that the decoder mends exactly those. */
static void check_word(uint32_t len, uint32_t nroots, uint32_t errs)
{
    uint8_t data[PRZ_ECC_BLOCK], par[PRZ_ECC_MAX], s[PRZ_ECC_MAX];
    uint8_t want[PRZ_ECC_BLOCK], wantp[PRZ_ECC_MAX];
    uint32_t i, e, pos;
    int n = 0;

    for (i = 0; i < len; i++) {
        data[i] = (uint8_t)next();
    }
    prz_rs_encode(data, len, par, nroots);
    CHECK(!rs_syndromes(data, len, par, nroots, s));
    k_memcpy(want, data, len);
    k_memcpy(wantp, par, nroots);
    CHECK(prz_rs_decode(data, len, par, nroots) == 0);
    for (e = 0; e < errs; e++) {
        pos = e == 0 ? len + nroots - 1 : next() % (len + nroots);
        *symbol(data, len, par, pos) ^= (uint8_t)(1u + next() % 255u);
    }
    for (i = 0; i < len + nroots; i++) {
        n += *symbol(data, len, par, i) != (i < len ? want[i] : wantp[i - len]);
    }
    CHECK(prz_rs_decode(data, len, par, nroots) == n);
    CHECK(same(data, want, len) && same(par, wantp, nroots));
}

/** Words of every length and parity size, with up to half the parity's
 * count of errors. */
static void check_code(void)
{
    static const uint32_t roots[] = {2, 4, 16, 32};
    uint32_t r, len, trial;

    for (r = 0; r < sizeof(roots) / sizeof(roots[0]); r++) {
        for (len = 1; len <= PRZ_ECC_BLOCK; len += 7) {
            for (trial = 0; trial < 20; trial++) {
                check_word(len, roots[r], trial % (roots[r] / 2 + 1));
            }
        }
    }
}

/** A zone with no parity: an empty start, a wrap, and the second boot's
 * copy, oldest byte first. */
static void check_wrap(void)
{
    static uint8_t mem[64];
    struct prz z;
    char want[52];
    uint32_t i;

    for (i = 0; i < sizeof(mem); i++) {
        mem[i] = 0xa5;
    }
    CHECK(prz_init(&z, mem, sizeof(mem), 0, 0, 0) == 0);
    CHECK(z.buffer_size == 52 && z.old_log == 0);
    CHECK(mem[0] == 0x44 && mem[1] == 0x42 && mem[2] == 0x47 && mem[3] == 0x43);
    CHECK(hdr_get(&z, HDR_START) == 0 && hdr_get(&z, HDR_SIZE) == 0);
    for (i = 0; i < 100; i++) {
        char c = (char)('a' + i % 26);

        prz_write(&z, &c, 1);
        want[(i + 52 - 100 % 52) % 52] = c;
    }
    CHECK(hdr_get(&z, HDR_SIZE) == 52 && hdr_get(&z, HDR_START) == 100 % 52);
    CHECK(prz_init(&z, mem, sizeof(mem), 0, 0, 0) == 0);
    CHECK(z.old_size == 52 && z.old_log && same(z.old_log, want, 52));
    prz_free_old(&z);

    /* A write longer than the zone keeps its last bytes. */
    prz_zap(&z);
    prz_write(&z, "0123456789012345678901234567890123456789012345678901234567890123", 64);
    CHECK(prz_init(&z, mem, sizeof(mem), 0, 0, 1) == 0);
    CHECK(z.old_size == 52 &&
          same(z.old_log, "2345678901234567890123456789012345678901234567890123", 52));
    /* zap_old empties the zone once copied. */
    CHECK(hdr_get(&z, HDR_SIZE) == 0);
    prz_free_old(&z);

    /* A header out of range, or another signature, empties the zone. */
    hdr_set(&z, HDR_SIZE, 53);
    CHECK(prz_init(&z, mem, sizeof(mem), 0, 0, 0) == 0 && z.old_log == 0);
    CHECK(hdr_get(&z, HDR_SIZE) == 0);
    prz_write(&z, "x", 1);
    CHECK(prz_init(&z, mem, sizeof(mem), 1, 0, 0) == 0 && z.old_log == 0);
    CHECK(hdr_get(&z, HDR_SIG) == (PRZ_SIG ^ 1u) && hdr_get(&z, HDR_SIZE) == 0);
    CHECK(prz_init(&z, mem, PRZ_HEADER, 0, 0, 0) == -EINVAL);
}

/** A zone of 4096 bytes with Linux's ecc=1, 16 parity bytes a block: the
 * layout, the record mended across a reset, and the notice. */
static void check_ecc(void)
{
    static uint8_t mem[4096];
    struct prz z;
    char text[3000], notice[80];
    uint32_t i;

    k_memset(mem, 0, sizeof(mem));
    CHECK(prz_init(&z, mem, sizeof(mem), 0, 16, 0) == 0);
    /* ram_core.c:212: DIV_ROUND_UP(4084 - 16, 144) blocks and one more
     * parity for the header. */
    CHECK(z.ecc_blocks == 29 && z.buffer_size == 4084 - 30 * 16);
    CHECK(prz_ecc_string(&z, notice, sizeof(notice)) == 25);
    CHECK(same(notice, "\nECC: No errors detected\n", 26));
    for (i = 0; i < sizeof(text); i++) {
        text[i] = (char)(' ' + next() % 95u);
    }
    prz_write(&z, text, 1000);
    prz_write(&z, text + 1000, 2000);
    prz_write(&z, text, 3000);

    /* Damage eight bytes of one block, one of another and a header byte. */
    for (i = 0; i < 8; i++) {
        mem[PRZ_HEADER + 300 + i * 9] ^= 0x5a;
    }
    mem[PRZ_HEADER + 2000] ^= 1;
    mem[5] ^= 0x80;
    CHECK(prz_init(&z, mem, sizeof(mem), 0, 16, 0) == 0);
    CHECK(z.old_size == z.buffer_size && z.bad_blocks == 0 && z.corrected_bytes == 10);
    CHECK(z.old_log && same(z.old_log + z.buffer_size - 3000, text, 3000));
    CHECK(prz_ecc_string(&z, notice, sizeof(notice)) == 49);
    CHECK(same(notice, "\nECC: 10 Corrected bytes, 0 unrecoverable blocks\n", 50));
    prz_free_old(&z);

    /* Nine errors in one block are past what the code mends. */
    for (i = 0; i < 9; i++) {
        mem[PRZ_HEADER + 1024 + i] ^= 0xff;
    }
    CHECK(prz_init(&z, mem, sizeof(mem), 0, 16, 0) == 0);
    CHECK(z.bad_blocks == 1 && z.corrected_bytes == 0);
    CHECK(prz_ecc_string(&z, notice, 20) == 48 && notice[19] == '\0');
    CHECK(same(notice, "\nECC: 0 Corrected b", 20));
    prz_free_old(&z);
}

int main(void)
{
    check_code();
    check_wrap();
    check_ecc();
    if (failures) {
        printf("pstore_hosttest: %d checks failed\n", failures);
    }
    return failures ? 1 : 0;
}
