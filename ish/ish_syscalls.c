#include "ish_syscalls.h"

/* Select the system call implementation for the compiler's target CPU. */
#if defined __i386__ || defined __i686__
#include "ish_syscalls.i386.c"
#else
#error "Unsupported CPU architecture. This project requires i386."
#endif
