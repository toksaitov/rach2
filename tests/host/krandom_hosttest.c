/**
 * krandom_hosttest.c: the kernel's random number generator, a ChaCha20
 * keystream (`lib/krandom.c`), and the choice of the source that seeds it.
 *
 * Includes `sys/lib/krandom.c` whole, to reach `chacha_block` and the state,
 * and runs it natively under ASan and UBSan. The test gives `krandom_seed`
 * its hardware: `arch_random` answers from a script of words and failures
 * for each source, and `arch_cycles`, `rtc_boot_time` and `kernel_ticks` are
 * values the rows set.
 *
 * Prints `krandom-test: fail: FILE:LINE: ROW` for each failed check and
 * exits with their count, capped at 120 so that it cannot wrap to 0.
 */

#include "krandom.c"

#include <stdio.h>
#include <string.h>

static int fails;

/** Count a failure when `cond` is false, and print its file, line and
 * row. */
#define CHECK(cond, row)                                                          \
    do {                                                                          \
        if (!(cond)) {                                                            \
            printf("krandom-test: fail: %s:%d: %s\n", __FILE__, __LINE__, (row)); \
            fails++;                                                              \
        }                                                                         \
    } while (0)

/* What one hardware source of the model gives: `words` in turn, each after
 * `fails_before` failed attempts, until `fail_from`, from which word on every
 * attempt fails. `calls` counts the attempts. */
struct source_model {
    const uint32_t *words;
    int fails_before;
    int fail_from;
    int calls;
    int failed;    /* failed attempts made for the current word */
    int delivered; /* words given */
};

static struct source_model seed_model; /* RDSEED */
static struct source_model rand_model; /* RDRAND */

static uint64_t cycles_now;
static int64_t boot_seconds;
volatile uint32_t kernel_ticks;

/** One attempt at a word of the source `m`, as `arch_random` makes it. */
static int model_draw(struct source_model *m, uint32_t *out)
{
    m->calls++;
    if (m->words == NULL || m->delivered >= m->fail_from) {
        return 0;
    }
    if (m->failed < m->fails_before) {
        m->failed++;
        return 0;
    }
    *out = m->words[m->delivered++];
    m->failed = 0;
    return 1;
}

int arch_random(uint32_t *out, int seed)
{
    return model_draw(seed ? &seed_model : &rand_model, out);
}

uint64_t arch_cycles(void)
{
    return cycles_now;
}

int64_t rtc_boot_time(void)
{
    return boot_seconds;
}

/** Give a source no hardware: every attempt fails. */
static void model_none(struct source_model *m)
{
    memset(m, 0, sizeof(*m));
}

/** Give a source eight words, each after `fails_before` failures, failing
 * every attempt from word `fail_from` on. */
static void model_words(struct source_model *m, const uint32_t *words, int fails_before,
                        int fail_from)
{
    memset(m, 0, sizeof(*m));
    m->words = words;
    m->fails_before = fails_before;
    m->fail_from = fail_from;
}

/** Whether the generator's key is the eight words `key`, with the counter
 * and the nonce zero and no block of the old key left to dispense. */
static int keyed_with(const uint32_t key[8])
{
    int i;

    for (i = 0; i < 8; i++) {
        if (cc_state[4 + i] != key[i]) {
            return 0;
        }
    }
    return cc_state[12] == 0 && cc_state[13] == 0 && cc_state[14] == 0 && cc_state[15] == 0 &&
           cc_byte_pos == 64;
}

/** RFC 8439 section 2.3.2: the block of its key, counter 1 and nonce. */
static void check_rfc8439_block(void)
{
    static const uint32_t in[16] = {0x61707865u, 0x3320646eu, 0x79622d32u, 0x6b206574u,
                                    0x03020100u, 0x07060504u, 0x0b0a0908u, 0x0f0e0d0cu,
                                    0x13121110u, 0x17161514u, 0x1b1a1918u, 0x1f1e1d1cu,
                                    0x00000001u, 0x09000000u, 0x4a000000u, 0x00000000u};
    static const uint32_t want[16] = {0xe4e7f110u, 0x15593bd1u, 0x1fdd0f50u, 0xc47120a3u,
                                      0xc7f4d1c7u, 0x0368c033u, 0x9aaa2204u, 0x4e6cd4c3u,
                                      0x466482d2u, 0x09aa9f07u, 0x05d7c214u, 0xa2028bd9u,
                                      0xd19c12b5u, 0xb94e16deu, 0xe883d0cbu, 0x4e3c50a2u};
    static const uint8_t serial[16] = {0x10, 0xf1, 0xe7, 0xe4, 0xd1, 0x3b, 0x59, 0x15,
                                       0x50, 0x0f, 0xdd, 0x1f, 0xa3, 0x20, 0x71, 0xc4};
    uint32_t out[16];
    uint8_t bytes[16];

    chacha_block(out, in);
    CHECK(memcmp(out, want, sizeof(want)) == 0, "chacha_block gives RFC 8439's block 2.3.2");

    memcpy(cc_state, in, sizeof(in));
    cc_byte_pos = 64;
    krandom_bytes(bytes, sizeof(bytes));
    CHECK(memcmp(bytes, serial, sizeof(serial)) == 0,
          "the bytes of RFC 8439's block 2.3.2 come out in its serialized order");
}

/**
 * The stream of the four words 1, 2, 3 and 4: the key they expand to and
 * its first three blocks, counted from zero, as OpenSSL's ChaCha20 gives
 * them. Words 14 to 17 cross the end of the first block.
 */
static void check_reseed_stream(void)
{
    static const int at[12] = {0, 1, 2, 3, 14, 15, 16, 17, 44, 45, 46, 47};
    static const uint32_t want[12] = {0x8973d963u, 0x37d1fd19u, 0x9d1cc12fu, 0x76f130ccu,
                                      0x87b3f9edu, 0x07c74aceu, 0xe42a127eu, 0x0ce97bb7u,
                                      0xe0ea900au, 0x137582b4u, 0x83e8f963u, 0xdb53a150u};
    static const uint32_t key[8] = {1u,          2u,          3u,          4u,
                                    0x9e3779b8u, 0x243f6a8au, 0xb7e15161u, 0xdeadbeebu};
    uint32_t words[48];
    uint8_t bytes[8];
    uint32_t w0, w1;
    int i, same;

    krandom_reseed(1, 2, 3, 4);
    CHECK(keyed_with(key), "krandom_reseed expands four words to its eight-word key");
    for (i = 0; i < 48; i++) {
        words[i] = krandom_u32();
    }
    same = 1;
    for (i = 0; i < 12; i++) {
        if (words[at[i]] != want[i]) {
            same = 0;
        }
    }
    CHECK(same, "the stream of the seed 1, 2, 3, 4 is the ChaCha20 keystream of its key");

    /* One cursor: one byte and three bytes are the first word, least
     * significant first, and the rest of the block is dropped by a reseed. */
    krandom_reseed(1, 2, 3, 4);
    krandom_bytes(bytes, 1);
    krandom_bytes(bytes + 1, 3);
    w0 = (uint32_t)bytes[0] | (uint32_t)bytes[1] << 8 | (uint32_t)bytes[2] << 16 |
         (uint32_t)bytes[3] << 24;
    CHECK(w0 == want[0], "krandom_bytes and krandom_u32 draw from one cursor, low byte first");
    krandom_reseed(1, 2, 3, 4);
    w1 = krandom_u32();
    CHECK(w1 == want[0], "a reseed drops the rest of the block in use");
}

/** The 64-bit block counter carries from its low word into its high one. */
static void check_counter_carry(void)
{
    static const uint32_t last_low[4] = {0xb14444f7u, 0xa83d71f4u, 0xe9972ee6u, 0x325f38afu};
    static const uint32_t first_high[4] = {0x470d5672u, 0x1c65fae7u, 0xbc041858u, 0xd2c3da69u};
    uint32_t block[16];
    int i;

    krandom_reseed(1, 2, 3, 4);
    cc_state[12] = 0xffffffffu;
    for (i = 0; i < 16; i++) {
        block[i] = krandom_u32();
    }
    CHECK(memcmp(block, last_low, sizeof(last_low)) == 0,
          "the block of counter 0xffffffff is the keystream's");
    CHECK(cc_state[12] == 0 && cc_state[13] == 1, "the counter carries into its high word");
    for (i = 0; i < 16; i++) {
        block[i] = krandom_u32();
    }
    CHECK(memcmp(block, first_high, sizeof(first_high)) == 0,
          "the block after the carry is the keystream's of counter 2^32");
}

/** The properties a stream of any key has, as the earlier test had them. */
static void check_stream_properties(void)
{
    uint32_t a[8];
    uint8_t buf[37];
    uint8_t big[512];
    int i, differ, nonzero, ones;
    unsigned int x;

    krandom_reseed(1, 2, 3, 4);
    for (i = 0; i < 8; i++) {
        a[i] = krandom_u32();
    }
    krandom_reseed(5, 6, 7, 8);
    differ = 0;
    for (i = 0; i < 8; i++) {
        if (krandom_u32() != a[i]) {
            differ = 1;
        }
    }
    CHECK(differ, "another seed gives another stream");

    krandom_reseed(0, 0, 0, 0);
    nonzero = 0;
    for (i = 0; i < 8; i++) {
        if (krandom_u32() != 0) {
            nonzero = 1;
        }
    }
    CHECK(nonzero, "the all-zero seed does not give zeros");

    /* ASan bounds the fill to the 37 bytes. */
    krandom_reseed(0xC0FFEEu, 0x0BADF00Du, 0x00001234u, 0x00005678u);
    memset(buf, 0, sizeof(buf));
    krandom_bytes(buf, sizeof(buf));
    nonzero = 0;
    for (i = 0; i < (int)sizeof(buf); i++) {
        if (buf[i] != 0) {
            nonzero = 1;
        }
    }
    CHECK(nonzero, "krandom_bytes fills 37 bytes, not with zeros alone");

    /* Of 4096 bits, a uniform stream sets 2048 with a deviation of 32. */
    krandom_reseed(0x243f6a88u, 0x85a308d3u, 0x13198a2eu, 0x03707344u);
    krandom_bytes(big, sizeof(big));
    ones = 0;
    for (i = 0; i < (int)sizeof(big); i++) {
        for (x = big[i]; x; x &= x - 1) {
            ones++;
        }
    }
    CHECK(ones > 2048 - 205 && ones < 2048 + 205, "4096 bits have 2048 set, within 205");
}

/**
 * krandom_seed's choice of source: RDSEED's eight words when it gives them,
 * a word allowed 32 attempts; else eight new words of RDRAND's on the same
 * terms; else the boot's timing, untrusted. A key from hardware is installed
 * as it came.
 */
static void check_seed_sources(void)
{
    static const uint32_t seeds[8] = {0x11111111u, 0x22222222u, 0x33333333u, 0x44444444u,
                                      0x55555555u, 0x66666666u, 0x77777777u, 0x88888888u};
    static const uint32_t rands[8] = {0xa1a1a1a1u, 0xb2b2b2b2u, 0xc3c3c3c3u, 0xd4d4d4d4u,
                                      0xe5e5e5e5u, 0xf6f6f6f6u, 0x07070707u, 0x18181818u};
    uint32_t want[8], got[8], other[8];
    uint32_t lo, hi, sec, ticks;
    int i;

    CHECK(krandom_source() == KRANDOM_SRC_JITTER && !krandom_is_trusted(),
          "before the boot seeds it the source is the untrusted timing");

    model_words(&seed_model, seeds, 0, 8);
    model_words(&rand_model, rands, 0, 8);
    krandom_seed();
    CHECK(krandom_source() == KRANDOM_SRC_RDSEED && krandom_is_trusted(),
          "RDSEED that gives eight words is the trusted source");
    CHECK(keyed_with(seeds), "RDSEED's eight words are the key as they came");
    CHECK(seed_model.calls == 8 && rand_model.calls == 0, "RDRAND is not asked when RDSEED gives");

    krandom_reseed(9, 9, 9, 9);
    CHECK(krandom_source() == KRANDOM_SRC_RDSEED, "a reseed leaves the recorded source");

    model_words(&seed_model, seeds, 31, 8);
    model_words(&rand_model, rands, 0, 8);
    krandom_seed();
    CHECK(krandom_source() == KRANDOM_SRC_RDSEED && keyed_with(seeds),
          "a word of RDSEED's on its 32nd attempt is taken");
    CHECK(seed_model.calls == 8 * 32 && rand_model.calls == 0,
          "RDSEED is given 32 attempts for each word");

    model_words(&seed_model, seeds, 0, 4);
    model_words(&rand_model, rands, 0, 8);
    krandom_seed();
    CHECK(krandom_source() == KRANDOM_SRC_RDRAND && krandom_is_trusted(),
          "RDRAND is the trusted source when RDSEED stops part way");
    CHECK(keyed_with(rands), "RDRAND's eight new words are the key, none of RDSEED's");
    CHECK(seed_model.calls == 4 + 32 && rand_model.calls == 8,
          "RDSEED is given up after a word's 32 attempts fail");

    model_none(&seed_model);
    model_words(&rand_model, rands, 31, 8);
    krandom_seed();
    CHECK(krandom_source() == KRANDOM_SRC_RDRAND && keyed_with(rands),
          "a word of RDRAND's on its 32nd attempt is taken");
    CHECK(rand_model.calls == 8 * 32, "RDRAND is given 32 attempts for each word");

    model_none(&seed_model);
    model_words(&rand_model, rands, 32, 8);
    cycles_now = 0x0123456789abcdefull;
    boot_seconds = 1792454400;
    kernel_ticks = 4242;
    krandom_seed();
    CHECK(krandom_source() == KRANDOM_SRC_JITTER && !krandom_is_trusted(),
          "without hardware words the source is the untrusted timing");
    CHECK(seed_model.calls == 32 && rand_model.calls == 32,
          "each source is given 32 attempts at its first word");
    for (i = 0; i < 8; i++) {
        got[i] = krandom_u32();
    }
    lo = (uint32_t)cycles_now;
    hi = (uint32_t)(cycles_now >> 32);
    sec = (uint32_t)boot_seconds;
    ticks = kernel_ticks;
    krandom_reseed(lo ^ sec, hi, ticks ^ (sec << 16), lo ^ hi ^ ticks ^ sec);
    for (i = 0; i < 8; i++) {
        want[i] = krandom_u32();
    }
    CHECK(memcmp(got, want, sizeof(want)) == 0,
          "the timing seed is the cycle count, the boot's time and the tick");

    cycles_now += 1;
    krandom_seed();
    for (i = 0; i < 8; i++) {
        other[i] = krandom_u32();
    }
    CHECK(memcmp(got, other, sizeof(other)) != 0, "another cycle count gives another stream");
}

/** Run the rows; the count of failures, at most 120. */
int main(void)
{
    check_seed_sources();
    check_rfc8439_block();
    check_reseed_stream();
    check_counter_carry();
    check_stream_properties();
    return (fails > 120) ? 120 : fails;
}
