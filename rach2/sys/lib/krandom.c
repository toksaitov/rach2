#include "krandom.h"

/*
 * xorshift128 state. The defaults are arbitrary non-zero constants so the
 * generator is usable before krandom_seed() runs.
 */
static uint32_t s0 = 0x9e3779b9;
static uint32_t s1 = 0x243f6a88;
static uint32_t s2 = 0xb7e15162;
static uint32_t s3 = 0xdeadbeef;

void krandom_reseed(uint32_t a, uint32_t b, uint32_t c, uint32_t d)
{
    /* The all-zero state locks xorshift at zero forever; forbid it. */
    s0 = a ? a : 0x9e3779b9;
    s1 = b ? b : 0x243f6a88;
    s2 = c ? c : 0xb7e15162;
    s3 = d ? d : 0xdeadbeef;
}

uint32_t krandom_u32(void)
{
    uint32_t t = s3;
    uint32_t s = s0;

    s3 = s2;
    s2 = s1;
    s1 = s;

    t ^= t << 11;
    t ^= t >> 8;
    s0 = t ^ s ^ (s >> 19);

    return s0;
}

void krandom_bytes(void *buf, uint32_t n)
{
    uint8_t *p = (uint8_t *)buf;
    uint32_t i = 0;

    while (i < n) {
        uint32_t w = krandom_u32();
        uint32_t k;

        for (k = 0; k < 4 && i < n; k++) {
            p[i] = (uint8_t)(w & 0xFF);
            w >>= 8;
            i++;
        }
    }
}

#if defined(__i386__)

/* Hardware entropy sources, kernel-side only (see kio.c / kernel.c). */
extern void outb(uint16_t port, uint8_t val);
extern uint8_t inb(uint16_t port);
extern volatile uint32_t kernel_ticks;

void krandom_seed(void)
{
    uint32_t lo = 0;
    uint32_t hi = 0;
    uint8_t sec;

    /* CPU timestamp counter: high-resolution, varies every boot. */
    __asm__ __volatile__("rdtsc" : "=a"(lo), "=d"(hi));

    /* CMOS RTC seconds (register 0 via ports 0x70/0x71): wall-clock jitter that
     * differs across boots even if the TSC starting point did not. */
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
