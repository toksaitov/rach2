/**
 * e2fsprogs_alignment_hosttest.c: the buffer alignment of e2fsprogs'
 * unix_io.c, as vendor/e2fsprogs/patches/buffer-alignment.patch changes it,
 * through pread and pwrite.
 *
 * The rows are in compat/e2fsprogs/alignment.h, which compiles the patched
 * unix_io.c itself with a small host configuration and stubs, so that no
 * expression of it is copied into the test. They run its raw I/O, the
 * bounce buffer and the error handling included. The cache, the opening of
 * a channel, the guest's C library and the kernel's I/O are not tested.
 *
 * host-deps:
 * host-cflags: -I tests/host/compat/e2fsprogs
 * host-cflags: -I vendor/e2fsprogs/e2fsprogs-1.47.4/lib
 */

#include "compat/e2fsprogs/alignment.h"
