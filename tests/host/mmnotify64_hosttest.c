/**
 * Host test of the address-space change notice and the shrinkers with
 * x86-64's `cpu.h`: the rows of mmnotify_hosttest.c over x86-64's page
 * table entries and 64-bit addresses.
 *
 * host-deps: rach2/sys/mm/vm.c rach2/sys/mm/frame.c rach2/sys/mm/notify.c
 * host-deps: rach2/sys/mm/pressure.c
 * host-deps: rach2/sys/lib/kstring.c rach2/sys/lib/kctypes.c
 * host-deps: tests/host/compat/spinlock_host.c
 * host-deps: tests/host/compat/mmap_lock_host.c tests/host/compat/atomic_host.c
 * host-cflags: -DRACH2_HOST_X86_64
 *
 * Returns 0 when every row passes, 1 otherwise.
 */

#include "mmnotify_hosttest.c"
