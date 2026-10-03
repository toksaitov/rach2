#include "ish_syscalls.h"

/* Select the system call wrappers for the compiler's target CPU. A target
 * with no set of its own stops the build here. */
#if defined __i386__ || defined __i686__
#include "ish_syscalls.i386.c"
#elif defined __x86_64__
#include "ish_syscalls.x86_64.c"
#else
#error "Unsupported CPU architecture. This project requires i386 or x86-64."
#endif
