#ifndef _KRANDOM_H_
#define _KRANDOM_H_

#include "kint.h"

/*
 * The kernel CSPRNG: a ChaCha20 keystream generator.
 *
 * krandom_seed() keys it from the best entropy the machine offers: the hardware
 * RNG (RDSEED, else RDRAND, each behind a CPUID feature bit) when present -- a
 * defensible seed -- otherwise boot-time jitter (the CPU timestamp counter, the
 * PIT tick, the CMOS RTC seconds), which only VARIES across boots and is NOT
 * defensible entropy. The fail-closed getrandom/urandom seeded by virtio-rng
 * arrives in Part II-B (before dropbear); on a host that lacks RDSEED/RDRAND,
 * treat keys produced here as seeded by jitter, not by real entropy.
 *
 * Real-OS analogue: the kernel CSPRNG (ChaCha) and its early-boot seeding.
 *
 * The CSPRNG core (reseed/u32/bytes, ChaCha) is pure and compiles both
 * freestanding and hosted, so it is unit-tested natively under the sanitizers
 * (the dual-target seam); only krandom_seed() touches hardware and is i386-only.
 */

/* Re-seed the generator from four explicit words (pure; the non-zero state is
 * enforced internally so the generator never locks at zero). */
void krandom_reseed(uint32_t a, uint32_t b, uint32_t c, uint32_t d);

/* Seed from hardware jitter (TSC + PIT tick + RTC seconds). Kernel-only; a
 * no-op-but-deterministic stub on a non-i386 host build. */
void krandom_seed(void);

/* One pseudo-random 32-bit word. */
uint32_t krandom_u32(void);

/* Fill n bytes of buf with pseudo-random data. */
void krandom_bytes(void *buf, uint32_t n);

#endif /* _KRANDOM_H_ */
