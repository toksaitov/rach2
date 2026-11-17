/**
 * vkms64_hosttest.c: the rows of vkms_hosttest.c with x86-64's `cpu.h`,
 * whose structures are 64 bits wide.
 *
 * host-deps: rach2/sys/lib/kstring.c rach2/sys/lib/kctypes.c
 * host-deps: rach2/sys/lib/crc32.c
 * host-deps: tests/host/compat/resv_host.c
 * host-deps: tests/host/compat/mutex_none.c
 * host-deps: tests/host/compat/spinlock_host.c
 * host-cflags: -DRACH2_HOST_X86_64
 *
 * Returns 0 when every row passes, 1 otherwise.
 */

#include "vkms_hosttest.c"
