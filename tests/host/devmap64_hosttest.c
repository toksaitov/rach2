/**
 * Host test of device mappings with x86-64's `cpu.h`: the rows of
 * devmap_hosttest.c, whose page directory is the test's own, over x86-64's
 * page table entries, 512 to a table, and 64-bit addresses.
 *
 * host-deps: rach2/sys/mm/vm.c rach2/sys/mm/frame.c rach2/sys/mm/notify.c
 * host-deps: rach2/sys/lib/kstring.c rach2/sys/lib/kctypes.c
 * host-deps: tests/host/compat/spinlock_host.c
 * host-cflags: -DRACH2_HOST_X86_64
 *
 * Returns 0 when every row passes, 1 otherwise.
 */

#include "devmap_hosttest.c"
