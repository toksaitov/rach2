/**
 * virtio_gpu64_hosttest.c: the rows of virtio_gpu_hosttest.c with x86-64's
 * `cpu.h`, whose structures are 64 bits wide.
 *
 * host-deps: rach2/sys/lib/kstring.c rach2/sys/lib/kctypes.c
 * host-deps: tests/host/compat/atomic_host.c tests/host/compat/resv_host.c
 * host-deps: tests/host/compat/mutex_none.c
 * host-cflags: -DRACH2_HOST_X86_64
 *
 * Returns 0 when every row passes, 1 otherwise.
 */

#include "virtio_gpu_hosttest.c"
