#include "ish_syscalls.h"

/* Select the system call wrappers for the compiler's target CPU. Only the
 * i386 set exists, so any other target stops the build here. */
#if defined __i386__ || defined __i686__
#include "ish_syscalls.i386.c"
#else
#error "Unsupported CPU architecture. This project requires i386."
#endif
