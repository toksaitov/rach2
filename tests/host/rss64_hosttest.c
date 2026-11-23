/**
 * Host test of the resident counts by kind with x86-64's `cpu.h`: the rows
 * of rss_hosttest.c over x86-64's page table entries, and the rows of huge
 * pages, which i686 has none of.
 *
 * host-deps: rach2/sys/mm/vm.c rach2/sys/mm/frame.c rach2/sys/mm/notify.c
 * host-deps: rach2/sys/mm/pin.c rach2/sys/mm/regions.c
 * host-deps: rach2/sys/lib/kstring.c rach2/sys/lib/kctypes.c
 * host-deps: tests/host/compat/spinlock_host.c
 * host-deps: tests/host/compat/mmap_lock_host.c tests/host/compat/atomic_host.c
 * host-cflags: -DRACH2_HOST_X86_64 -DRACH2_RSS_CHECK
 *
 * Returns 0 when every row passes, 1 otherwise.
 */

#define FRAMES 4096u
#include "rss_hosttest.c"
