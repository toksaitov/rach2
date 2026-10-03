#include "krandom.h"

/*
 * ChaCha20 keystream CSPRNG. The 16-word input block is the standard layout:
 * four constant words, an eight-word key, a 64-bit block counter, and a 64-bit
 * nonce. krandom_reseed/krandom_seed install the key; each 64-byte block of
 * keystream is produced on demand and dispensed as a contiguous BYTE stream.
 *
 * NOT reentrant: the single CSPRNG instance is file-static. Call it only from
 * kernel-thread / syscall context (where the timer never preempts, in_syscall),
 * never from an interrupt handler; a nested call would corrupt a half-dispensed
 * block. krandom_seed() runs once at boot before any user.
 *
 * The state is initialized with the ChaCha constants and a non-zero default
 * key, so even a draw before krandom_seed() yields a non-zero
 * (deterministic) stream, not an all-zero block.
 */
static uint32_t cc_state[16] = {
    0x61707865u, 0x3320646eu, 0x79622d32u, 0x6b206574u,                /* "expand 32-byte k" */
    0x9e3779b9u, 0x243f6a88u, 0xb7e15162u, 0xdeadbeefu,                /* default key */
    0x9e3779b9u, 0x243f6a88u, 0xb7e15162u, 0xdeadbeefu, 0u, 0u, 0u, 0u /* counter | nonce */
};
static uint32_t cc_out[16];       /* current keystream block */
static uint32_t cc_byte_pos = 64; /* byte cursor into cc_out (64 == exhausted) */

/* The entropy source the last krandom_seed() used (KRANDOM_SRC_*); the
 * fail-closed policy reads it. Starts at JITTER so a draw before seeding is
 * never "trusted". */
static int cc_src = KRANDOM_SRC_JITTER;

#define ROTL32(x, n) (((x) << (n)) | ((x) >> (32 - (n))))

#define QR(a, b, c, d)         \
    do {                       \
        (a) += (b);            \
        (d) ^= (a);            \
        (d) = ROTL32((d), 16); \
        (c) += (d);            \
        (b) ^= (c);            \
        (b) = ROTL32((b), 12); \
        (a) += (b);            \
        (d) ^= (a);            \
        (d) = ROTL32((d), 8);  \
        (c) += (d);            \
        (b) ^= (c);            \
        (b) = ROTL32((b), 7);  \
    } while (0)

/**
 * Compute one ChaCha20 keystream block.
 *
 * Twenty rounds (ten column/diagonal double-rounds) over a copy of `in`, then
 * `in` is added back, giving 16 words of keystream in `out`.
 */
static void chacha_block(uint32_t out[16], const uint32_t in[16])
{
    uint32_t x[16];
    int i;

    for (i = 0; i < 16; i++) {
        x[i] = in[i];
    }
    for (i = 0; i < 10; i++) {
        QR(x[0], x[4], x[8], x[12]);
        QR(x[1], x[5], x[9], x[13]);
        QR(x[2], x[6], x[10], x[14]);
        QR(x[3], x[7], x[11], x[15]);
        QR(x[0], x[5], x[10], x[15]);
        QR(x[1], x[6], x[11], x[12]);
        QR(x[2], x[7], x[8], x[13]);
        QR(x[3], x[4], x[9], x[14]);
    }
    for (i = 0; i < 16; i++) {
        out[i] = x[i] + in[i];
    }
}

/**
 * Install an eight-word key, reset the counter and nonce, and force a fresh
 * block on the next draw.
 *
 * Internal; krandom_reseed and krandom_seed are the entry points.
 */
static void chacha_setup(const uint32_t key[8])
{
    int i;

    cc_state[0] = 0x61707865u; /* "expand 32-byte k" */
    cc_state[1] = 0x3320646eu;
    cc_state[2] = 0x79622d32u;
    cc_state[3] = 0x6b206574u;
    for (i = 0; i < 8; i++) {
        cc_state[4 + i] = key[i];
    }
    cc_state[12] = 0; /* block counter */
    cc_state[13] = 0;
    cc_state[14] = 0; /* nonce */
    cc_state[15] = 0;
    cc_byte_pos = 64; /* exhausted: regenerate on the next byte */
}

/**
 * Return the next keystream byte, regenerating a block and bumping the 64-bit
 * counter when the current one is spent.
 *
 * Both the u32 and byte draws pull from this one cursor, so the byte stream is
 * contiguous: bytes(1) then bytes(3) yields the same four bytes as bytes(4).
 */
static uint8_t cc_next_byte(void)
{
    if (cc_byte_pos >= 64) {
        chacha_block(cc_out, cc_state);
        if (++cc_state[12] == 0) { /* 64-bit counter, carry into the high word */
            ++cc_state[13];
        }
        cc_byte_pos = 0;
    }
    return ((const uint8_t *)cc_out)[cc_byte_pos++];
}

/**
 * Reseed the CSPRNG from four seed words.
 *
 * The four words expand to a full eight-word key deterministically (each
 * high word a distinct mix), so a reseed is a pure function of its inputs.
 * ChaCha has no all-zero lock state, so even a zero seed yields a non-zero
 * stream.
 */
void krandom_reseed(uint32_t a, uint32_t b, uint32_t c, uint32_t d)
{
    uint32_t key[8];

    key[0] = a;
    key[1] = b;
    key[2] = c;
    key[3] = d;
    key[4] = a ^ 0x9e3779b9u;
    key[5] = b ^ 0x243f6a88u;
    key[6] = c ^ 0xb7e15162u;
    key[7] = d ^ 0xdeadbeefu;
    chacha_setup(key);
}

uint32_t krandom_u32(void)
{
    uint32_t w = 0;
    int i;

    for (i = 0; i < 4; i++) {
        w |= (uint32_t)cc_next_byte() << (i * 8);
    }
    return w;
}

void krandom_bytes(void *buf, uint32_t n)
{
    uint8_t *p = (uint8_t *)buf;
    uint32_t i;

    for (i = 0; i < n; i++) {
        p[i] = cc_next_byte();
    }
}

int krandom_source(void)
{
    return cc_src;
}

int krandom_is_trusted(void)
{
    return cc_src == KRANDOM_SRC_RDSEED || cc_src == KRANDOM_SRC_RDRAND;
}

#if !defined(RACH2_HOSTTEST)

#include "../arch/arch.h"
#include "../rtc.h"
#include "../timer.h"

/**
 * Draw one word from the processor's conditioned seed source (RDSEED on an
 * x86), through `arch_random`.
 *
 * The source may transiently fail, so it is asked a bounded number of times.
 * Return 1 and store *out on success, 0 if it never delivers, or the
 * processor has none. (`rdrand32`, below, draws from the processor's
 * generator, RDRAND on an x86, on the same terms.)
 */
static int rdseed32(uint32_t *out)
{
    int tries;

    for (tries = 0; tries < 32; tries++) {
        if (arch_random(out, 1)) {
            return 1;
        }
    }
    return 0;
}

static int rdrand32(uint32_t *out)
{
    int tries;

    for (tries = 0; tries < 32; tries++) {
        if (arch_random(out, 0)) {
            return 1;
        }
    }
    return 0;
}

/** Fill key[8] from `draw`; return 1 only if all eight words came through. */
static int collect_key(int (*draw)(uint32_t *), uint32_t key[8])
{
    int i;

    for (i = 0; i < 8; i++) {
        if (!draw(&key[i])) {
            return 0;
        }
    }
    return 1;
}

/**
 * Gather an eight-word key from hardware entropy.
 *
 * The seed source is preferred, then the generator; `arch_random` reports
 * neither on a processor without it. If the seed source is present but does
 * not deliver, the code falls through to the generator.
 *
 * @return The KRANDOM_SRC_* of the source that filled the key, or
 *         KRANDOM_SRC_JITTER if neither delivered.
 */
static int hw_key(uint32_t key[8])
{
    if (collect_key(rdseed32, key)) {
        return KRANDOM_SRC_RDSEED;
    }
    if (collect_key(rdrand32, key)) {
        return KRANDOM_SRC_RDRAND;
    }
    return KRANDOM_SRC_JITTER;
}

/**
 * Seed the CSPRNG once at boot, before any user.
 *
 * Real hardware entropy is used when the CPU offers it: a defensible seed that
 * marks the generator trusted. Otherwise the seed comes from boot-time jitter
 * (the processor's cycle counter, the clock's reading at boot, and the
 * tick). Jitter varies
 * across boots but is not defensible entropy, so the source stays untrusted:
 * getrandom without GRND_INSECURE and /dev/[u]random fail closed, and only
 * best-effort uses draw from it, such as AT_RANDOM, from which the C library
 * takes its stack canary.
 */
void krandom_seed(void)
{
    uint32_t key[8];
    uint64_t cycles;
    uint32_t lo, hi, sec;
    int src;

    src = hw_key(key);
    if (src != KRANDOM_SRC_JITTER) {
        chacha_setup(key);
        cc_src = src;
        return;
    }

    cycles = arch_cycles();
    lo = (uint32_t)cycles;
    hi = (uint32_t)(cycles >> 32);
    sec = (uint32_t)rtc_boot_time();

    krandom_reseed(lo ^ sec, hi, kernel_ticks ^ (sec << 16), lo ^ hi ^ kernel_ticks ^ sec);
    cc_src = KRANDOM_SRC_JITTER;
}

#else

/**
 * Seed the CSPRNG for the host build, which has no hardware entropy.
 *
 * Tests that need a specific stream call krandom_reseed() explicitly.
 */
void krandom_seed(void)
{
    krandom_reseed(0x12345678u, 0x9abcdef0u, 0x0f0f0f0fu, 0xa5a5a5a5u);
}

#endif
