/* Syncobj rows with the x86-64 kernel's structures.
 * host-deps: rach2/sys/tick.c
 * host-deps: rach2/sys/lib/kstring.c rach2/sys/lib/kctypes.c
 * host-deps: tests/host/compat/spinlock_host.c tests/host/compat/atomic_host.c
 * host-deps: tests/host/compat/fd_flat.c
 * host-cflags: -DRACH2_HOST_X86_64
 */
#include "syncobj_hosttest.c"
