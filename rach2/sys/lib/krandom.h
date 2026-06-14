#ifndef _KRANDOM_H_
#define _KRANDOM_H_

#include "kint.h"

/*
 * The kernel CSPRNG: a ChaCha20 keystream generator.
 *
 * krandom_seed() keys it from the best entropy the machine offers: the hardware
 * RNG (RDSEED, else RDRAND, each behind a CPUID feature bit) when present -- a
 * defensible "trusted" seed -- otherwise boot-time jitter (the CPU timestamp
 * counter, the PIT tick, the CMOS RTC seconds), which only VARIES across boots and
 * is NOT defensible entropy.
 *
 * Phase 20 tiers the consumers on krandom_is_trusted():
 *   - CRYPTO (fail-closed): getrandom(2) and /dev/[u]random refuse (-EIO) when the
 *     source is untrusted, so a key is never built from jitter; GRND_INSECURE is
 *     the explicit best-effort opt-out. A real hardware seed (run QEMU with a CPU
 *     that has RDSEED/RDRAND, e.g. -cpu max) is required for SSH keygen (dropbear).
 *   - BEST-EFFORT: AT_RANDOM and the stack canary (Phase 1) draw straight from the
 *     stream regardless -- jitter is acceptable for ASLR/canary, not for keys.
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

/*
 * The entropy SOURCE krandom_seed() actually used. RDSEED/RDRAND are real
 * hardware entropy ("trusted"); JITTER is boot-time timing variance, which is NOT
 * defensible for keys. Phase 20 keys the fail-closed policy on this: getrandom and
 * /dev/[u]random refuse when the source is untrusted, so no weak randomness is
 * ever handed out for a key (AT_RANDOM / the stack canary stay best-effort).
 */
#define KRANDOM_SRC_JITTER 0
#define KRANDOM_SRC_RDSEED 1
#define KRANDOM_SRC_RDRAND 2

int krandom_source(void);   /* KRANDOM_SRC_* used by the last krandom_seed() */
int krandom_is_trusted(void); /* 1 iff the source is real hardware entropy */

#endif /* _KRANDOM_H_ */
