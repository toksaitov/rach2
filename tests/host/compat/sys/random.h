/**
 * The host stand-in for musl's `<sys/random.h>`.
 *
 * See `../crypt.h` for why the compat headers exist. A host may have no
 * `getrandom(2)` at all (Darwin has `getentropy` instead), and no host call
 * could stand in for the one property that matters: this kernel refuses the
 * draw outright when the CPU exposes neither RDSEED nor RDRAND. So the seam
 * test defines the symbol as a refusal, and the fail-closed policy is proved in
 * the guest by `tests/musl/saltnoent.c` on a CPU that has neither.
 */
#ifndef _HOSTCOMPAT_SYS_RANDOM_H_
#define _HOSTCOMPAT_SYS_RANDOM_H_

#include <stddef.h>
#include <sys/types.h>

ssize_t getrandom(void *buffer, size_t length, unsigned int flags);

#endif /* _HOSTCOMPAT_SYS_RANDOM_H_ */
