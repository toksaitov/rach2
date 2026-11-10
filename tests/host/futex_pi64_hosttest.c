/* PI futex rows with the x86-64 kernel's process and syscall structures.
 * host-deps: rach2/sys/lib/kstring.c rach2/sys/lib/kctypes.c rach2/sys/tick.c
 * host-cflags: -DRACH2_HOST_X86_64
 * host-cflags: -Wno-int-to-pointer-cast -Wno-pointer-to-int-cast
 */
#define ebx rdi
#define ecx rsi
#define edx rdx
#define esi r10
#define edi r8
#define ebp r9
#define eax rax
#include "futex_pi_hosttest.c"
