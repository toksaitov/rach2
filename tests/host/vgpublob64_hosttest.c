/* vgpublob64_hosttest.c: the blob, context-init and ring rows with x86-64
 * kernel structures.
 * host-deps: rach2/sys/lib/kstring.c rach2/sys/lib/kctypes.c
 * host-deps: rach2/sys/cmdline.c rach2/sys/lib/kstd.c
 * host-deps: tests/host/compat/atomic_host.c tests/host/compat/resv_host.c
 * host-deps: tests/host/compat/mutex_none.c
 * host-cflags: -DRACH2_HOST_X86_64
 * Returns 0 when every blob row passes. */
#include "vgpublob_hosttest.c"
