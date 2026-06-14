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
 * The state is initialized with the ChaCha constants and a non-zero default key,
 * so even a draw before krandom_seed() yields a non-zero (deterministic) stream
 * rather than an all-zero block.
 */
static uint32_t cc_state[16] = {
    0x61707865u, 0x3320646eu, 0x79622d32u, 0x6b206574u, /* "expand 32-byte k" */
    0x9e3779b9u, 0x243f6a88u, 0xb7e15162u, 0xdeadbeefu, /* default key */
    0x9e3779b9u, 0x243f6a88u, 0xb7e15162u, 0xdeadbeefu,
    0u, 0u, 0u, 0u /* counter | nonce */
};
static uint32_t cc_out[16];     /* current keystream block */
static uint32_t cc_byte_pos = 64; /* byte cursor into cc_out (64 == exhausted) */

#define ROTL32(x, n) (((x) << (n)) | ((x) >> (32 - (n))))

#define QR(a, b, c, d)             \
    do {                           \
        (a) += (b);                \
        (d) ^= (a);                \
        (d) = ROTL32((d), 16);     \
        (c) += (d);                \
        (b) ^= (c);                \
        (b) = ROTL32((b), 12);     \
        (a) += (b);                \
        (d) ^= (a);                \
        (d) = ROTL32((d), 8);      \
        (c) += (d);                \
        (b) ^= (c);                \
        (b) = ROTL32((b), 7);      \
    } while (0)

/* One ChaCha20 block: 20 rounds (10 column/diagonal double-rounds) over a copy
 * of `in`, then add `in` back, giving 16 words of keystream in `out`. */
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

/* Install an eight-word key, reset the counter/nonce, and force a fresh block on
 * the next draw. Internal; reseed/seed are the entry points. */
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

/* Next keystream byte, regenerating a block (and bumping the 64-bit counter)
 * when the current one is spent. Both u32 and bytes draw from this one cursor, so
 * the byte stream is contiguous (bytes(1) then bytes(3) == bytes(4)). */
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

void krandom_reseed(uint32_t a, uint32_t b, uint32_t c, uint32_t d)
{
    uint32_t key[8];

    /* Expand the four seed words to a full key deterministically (each high word
     * a distinct mix), so reseed stays a pure function of its inputs. ChaCha has
     * no all-zero lock state, so a zero seed still yields a non-zero stream. */
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

#if defined(__i386__)

/* Hardware entropy sources, kernel-side only (see kio.c / kernel.c). */
extern void outb(uint16_t port, uint8_t val);
extern uint8_t inb(uint16_t port);
extern volatile uint32_t kernel_ticks;

/* CPUID leaf `leaf`, sub-leaf 0. */
static void cpuid(uint32_t leaf, uint32_t *a, uint32_t *b, uint32_t *c, uint32_t *d)
{
    __asm__ __volatile__("cpuid"
                         : "=a"(*a), "=b"(*b), "=c"(*c), "=d"(*d)
                         : "a"(leaf), "c"(0));
}

/* RDSEED -> a true-hardware entropy word; RDRAND -> a hardware CSPRNG word. Both
 * set CF on success and may transiently fail, so each retries a bounded number of
 * times. Return 1 and store *out on success, 0 if the instruction never delivers.
 */
static int rdseed32(uint32_t *out)
{
    uint32_t v = 0;
    uint8_t ok = 0;
    int tries;

    for (tries = 0; tries < 32; tries++) {
        __asm__ __volatile__("rdseed %0; setc %1" : "=r"(v), "=qm"(ok)::"cc");
        if (ok) {
            *out = v;
            return 1;
        }
    }
    return 0;
}

static int rdrand32(uint32_t *out)
{
    uint32_t v = 0;
    uint8_t ok = 0;
    int tries;

    for (tries = 0; tries < 32; tries++) {
        __asm__ __volatile__("rdrand %0; setc %1" : "=r"(v), "=qm"(ok)::"cc");
        if (ok) {
            *out = v;
            return 1;
        }
    }
    return 0;
}

/* Fill key[8] from a hardware draw; returns 1 if all eight words came through. */
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

/* Gather an eight-word key from hardware: RDSEED (leaf 7 EBX[18]) preferred, then
 * RDRAND (leaf 1 ECX[30]). Each is gated by BOTH its feature bit and a CPUID
 * max-leaf check (probing an unsupported leaf returns junk that could falsely
 * advertise the bit, then #UD when the instruction runs). If RDSEED is present
 * but does not deliver, it falls through to RDRAND. Returns 1 on a full hardware
 * key, 0 otherwise. */
static int hw_key(uint32_t key[8])
{
    uint32_t a, b, c, d, max_leaf;

    cpuid(0, &max_leaf, &b, &c, &d);

    if (max_leaf >= 7) {
        cpuid(7, &a, &b, &c, &d);
        if ((b & (1u << 18)) && collect_key(rdseed32, key)) {
            return 1; /* RDSEED */
        }
    }
    if (max_leaf >= 1) {
        cpuid(1, &a, &b, &c, &d);
        if ((c & (1u << 30)) && collect_key(rdrand32, key)) {
            return 1; /* RDRAND */
        }
    }
    return 0;
}

void krandom_seed(void)
{
    uint32_t key[8];
    uint32_t lo = 0, hi = 0;
    uint8_t sec;

    /* Real hardware entropy when the CPU offers it: a defensible CSPRNG seed. */
    if (hw_key(key)) {
        chacha_setup(key);
        return;
    }

    /* Fallback: boot-time jitter (CPU timestamp counter + CMOS RTC seconds + the
     * PIT tick). This VARIES across boots but is NOT defensible entropy -- the
     * real fail-closed source (virtio-rng) lands in Part II-B. */
    __asm__ __volatile__("rdtsc" : "=a"(lo), "=d"(hi));
    outb(0x70, 0x00);
    sec = inb(0x71);

    krandom_reseed(lo ^ (uint32_t)sec, hi, kernel_ticks ^ ((uint32_t)sec << 16),
                   lo ^ hi ^ kernel_ticks ^ (uint32_t)sec);
}

#else

void krandom_seed(void)
{
    /* Host build (dual-target seam): no hardware entropy. Tests that need a
     * specific stream call krandom_reseed() explicitly. */
    krandom_reseed(0x12345678u, 0x9abcdef0u, 0x0f0f0f0fu, 0xa5a5a5a5u);
}

#endif
