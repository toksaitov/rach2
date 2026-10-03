/**
 * Check the floating-point state's images in x86-64's layouts: the rows of
 * fpuframe_hosttest.c, with FPU_X86_64 defined before `arch/x86/fpu.c` is
 * included, so the frame's state has no x87 part before its image.
 *
 * host-deps: rach2/sys/lib/kstring.c rach2/sys/lib/kctypes.c
 * host-cflags: -Wno-int-to-pointer-cast -Wno-pointer-to-int-cast
 */
#define FPU_X86_64 1
#include "fpuframe_hosttest.c"
