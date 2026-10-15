/*
 * The definition every ish source shares: `ish_check`, the shell's assertion.
 */

#ifndef ISH_COMMON_H
#define ISH_COMMON_H

#ifdef ISH_USE_STDLIB
#include <stdlib.h>
#else
#include "ish_syscalls.h"
#endif

/**
 * Exit the process with status -1, which a parent reads as 255, when `value`
 * is false.
 *
 * A failed precondition is fatal and never recovered. `exit` is the C
 * library's under `ISH_USE_STDLIB` and the raw system call wrapper of
 * `ish_syscalls.h` otherwise.
 */
#define ish_check(value) \
    do {                 \
        if (!(value))    \
            exit(-1);    \
    } while (0)

#endif
