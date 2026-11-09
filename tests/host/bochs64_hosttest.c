/**
 * bochs64_hosttest.c: the rows of bochs_hosttest.c with x86-64's `cpu.h`,
 * whose structures are 64 bits wide.
 *
 * host-deps: rach2/sys/lib/kstring.c rach2/sys/lib/kctypes.c
 * host-cflags: -DRACH2_HOST_X86_64
 *
 * Returns 0 when every row passes, 1 otherwise.
 */

#include "bochs_hosttest.c"
