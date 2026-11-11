/**
 * drm_core64_hosttest.c: the rows of drm_core_hosttest.c with x86-64's
 * `cpu.h`, whose system call arguments are 64 bits wide.
 *
 * host-deps: rach2/sys/lib/kstring.c rach2/sys/lib/kctypes.c
 * host-deps: tests/host/compat/kheap_host.c tests/host/compat/spinlock_host.c
 * host-deps: tests/host/compat/atomic_host.c
 * host-cflags: -DRACH2_HOST_X86_64
 *
 * Returns 0 when every row passes, 1 otherwise.
 */

#include "drm_core_hosttest.c"
