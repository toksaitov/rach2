#ifndef _KRANDOM_H_
#define _KRANDOM_H_

#include "kint.h"

/*
 * Best-effort, NON-cryptographic entropy.
 *
 * The generator is a small, fast xorshift128 PRNG. krandom_seed() mixes real
 * boot-time jitter into it (the CPU timestamp counter, the PIT tick, and the
 * CMOS RTC seconds), which gives values that VARY across boots -- adequate for
 * AT_RANDOM as a source of variability, but NOT unpredictable to an attacker and
 * NOT a hardening primitive. It is not cryptographically secure: the
 * fail-closed getrandom/urandom seeded by virtio-rng arrives in Part II-B
 * (before dropbear), and that is what stack canaries and keys must use.
 *
 * Real-OS analogue: a weak early-boot entropy source vs. the kernel CSPRNG.
 *
 * The PRNG core (reseed/u32/bytes) is pure and compiles both freestanding and
 * hosted, so it is unit-tested natively under the sanitizers (the dual-target
 * seam); only krandom_seed() touches hardware and is i386-only.
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
