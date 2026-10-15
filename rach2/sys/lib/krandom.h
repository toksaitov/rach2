#ifndef _KRANDOM_H_
#define _KRANDOM_H_

#include "kint.h"

/*
 * The kernel CSPRNG: a ChaCha20 keystream generator.
 *
 * krandom_seed() keys it from the best entropy the machine offers: the
 * hardware generator (RDSEED, else RDRAND, each behind a CPUID feature bit)
 * when present, a defensible "trusted" seed; otherwise boot-time jitter (the
 * processor's cycle counter, the timer tick and the clock's reading at
 * boot), which only varies across boots and is not defensible entropy.
 *
 * Callers choose by krandom_is_trusted(). The paths that make keys fail
 * closed: getrandom(2) and /dev/[u]random return -EIO when the source is
 * untrusted, so a key is never built from jitter; GRND_INSECURE is the
 * explicit best-effort choice. Dropbear's key generation therefore needs a
 * hardware seed (under QEMU, a CPU model with RDSEED or RDRAND, such as
 * -cpu max). The best-effort uses, AT_RANDOM and the stack canary, draw
 * from the stream whatever its source: jitter is acceptable for address
 * randomization and the canary, not for keys.
 *
 * There is one generator, file-static, and it is not reentrant: draw from
 * it in system call context, where the timer does not switch processes, or
 * at boot, and never from an interrupt handler, whose draw could fall in
 * the middle of a block another draw is dispensing.
 *
 * The core (reseed, the draws and ChaCha) has no hardware access and
 * compiles both freestanding and hosted, so it is unit-tested natively
 * under the sanitizers; only krandom_seed() touches hardware, and the
 * kernel alone compiles it.
 */

/** Reseed the generator from four words, so that the stream that follows
 * is a function of them alone; the host tests use it for a known stream. */
void krandom_reseed(uint32_t a, uint32_t b, uint32_t c, uint32_t d);

/** Seed once at boot, before any user, from hardware entropy, else from
 * boot-time jitter, and record which for krandom_source(). */
void krandom_seed(void);

/** One pseudo-random 32-bit word. */
uint32_t krandom_u32(void);

/** Fill `n` bytes of `buf` with pseudo-random data. The bytes continue the
 * stream krandom_u32() draws from. */
void krandom_bytes(void *buf, uint32_t n);

/*
 * The entropy source krandom_seed() used. RDSEED and RDRAND are hardware
 * entropy ("trusted"); JITTER is boot-time timing variance, which is not
 * defensible for keys, and is the source before krandom_seed() runs.
 */
#define KRANDOM_SRC_JITTER 0
#define KRANDOM_SRC_RDSEED 1
#define KRANDOM_SRC_RDRAND 2

int krandom_source(void);     /* KRANDOM_SRC_* used by the last krandom_seed() */
int krandom_is_trusted(void); /* 1 iff the source is real hardware entropy */

#endif /* _KRANDOM_H_ */
