#include "ish_syscalls.h"

/* Per-arch indirection: pull in the implementation for the host CPU. */
#if defined __i386__ || defined __i686__
   #include "ish_syscalls.i386.c"
#else
   #error "Unsupported CPU architecture. This project requires i386."
#endif
