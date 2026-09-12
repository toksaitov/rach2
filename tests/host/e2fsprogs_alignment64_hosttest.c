/**
 * Test the port's buffer alignment through pread64 and pwrite64.
 *
 * The shared fixture includes the actual patched unix_io.c with a small
 * host configuration and stubs. It exercises raw I/O, including its bounce
 * path and error handlers, without copying the guarded expressions. It
 * does not test the cache, channel opening, guest libc, or kernel I/O.
 *
 * host-deps:
 * host-cflags: -I tests/host/compat/e2fsprogs
 * host-cflags: -I vendor/e2fsprogs/e2fsprogs-1.47.4/lib
 */

#define ALIGNMENT_TEST_64 1
#include "compat/e2fsprogs/alignment.h"
