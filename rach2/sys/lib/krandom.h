#ifndef _KRANDOM_H_
#define _KRANDOM_H_

#include "kint.h"

/*
 * The kernel CSPRNG: a ChaCha20 keystream generator.
 *
 * krandom_seed() keys it from the best entropy the machine offers: the hardware
 * RNG (RDSEED, else RDRAND, each behind a CPUID feature bit) when present, a
 * defensible "trusted" seed; otherwise boot-time jitter (the CPU timestamp
 * counter, the PIT tick, the CMOS RTC seconds), which only varies across boots
 * and is not defensible entropy.
 *
 * Consumers tier on krandom_is_trusted(). The crypto path fails closed:
 * getrandom(2) and /dev/[u]random refuse (-EIO) when the source is untrusted,
 * so a key is never built from jitter; GRND_INSECURE is the explicit
 * best-effort opt-out. A real hardware seed (QEMU with a CPU that has
 * RDSEED/RDRAND, e.g. -cpu max) is required for SSH keygen (dropbear). The
 * best-effort path, AT_RANDOM and the stack canary, draws straight from the
 * stream regardless: jitter is acceptable for ASLR and the canary, not for
 * keys.
 *
 * The CSPRNG core (reseed/u32/bytes, ChaCha) is pure and compiles both
 * freestanding and hosted, so it is unit-tested natively under the
 * sanitizers; only krandom_seed() touches hardware, and the kernel alone
 * compiles it.
 */

/** Reseed the generator from four explicit words. */
void krandom_reseed(uint32_t a, uint32_t b, uint32_t c, uint32_t d);

/** Seed once at boot from hardware entropy, else boot-time jitter. */
void krandom_seed(void);

/** One pseudo-random 32-bit word. */
uint32_t krandom_u32(void);

/** Fill n bytes of buf with pseudo-random data. */
void krandom_bytes(void *buf, uint32_t n);

/*
 * The entropy source krandom_seed() actually used. RDSEED/RDRAND are real
 * hardware entropy ("trusted"); JITTER is boot-time timing variance, which is
 * not defensible for keys. The fail-closed policy keys on this: getrandom and
 * /dev/[u]random refuse when the source is untrusted, so no weak randomness is
 * ever handed out for a key (AT_RANDOM and the stack canary stay best-effort).
 */
#define KRANDOM_SRC_JITTER 0
#define KRANDOM_SRC_RDSEED 1
#define KRANDOM_SRC_RDRAND 2

int krandom_source(void);     /* KRANDOM_SRC_* used by the last krandom_seed() */
int krandom_is_trusted(void); /* 1 iff the source is real hardware entropy */

#endif /* _KRANDOM_H_ */
