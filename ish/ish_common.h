#ifndef ISH_COMMON_H
#define ISH_COMMON_H

#ifdef ISH_USE_STDLIB
    #include <stdlib.h>
#else
    #include "ish_syscalls.h"
#endif

/**
 * Abort the process with `exit(-1)` when `value` is false.
 *
 * The shell's assertion: a failed precondition is fatal, never recovered.
 * `exit` resolves to the C library under `ISH_USE_STDLIB` and to the raw
 * `int 0x80` wrapper in `ish_syscalls.h` otherwise.
 */
#define ish_check(value) do { if (!(value)) exit(-1); } while(0)

#endif
