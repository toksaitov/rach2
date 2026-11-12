/**
 * Host test of the work of a return to user mode with x86-64's `cpu.h`:
 * the rows of leave_hosttest.c over x86-64's frame and thread records,
 * whose loop the native and the i386 entries' returns run.
 *
 * host-deps: rach2/sys/lib/kstring.c rach2/sys/lib/kctypes.c
 * host-cflags: -DRACH2_HOST_X86_64
 *
 * Returns 0 when every row passes, 1 otherwise.
 */

#include "leave_hosttest.c"
