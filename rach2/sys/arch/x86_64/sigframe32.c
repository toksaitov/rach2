/*
 * sigframe32.c: the signal frames of i386 handlers, Linux's `sigframe` and
 * `rt_sigframe` with their 32-bit floating-point state.
 */

#include "ia32.h"

/** No i386 frame is built yet: every handler gets the 64-bit frame of
 * sigframe.c. */
int ia32_sigframe_setup(isr_params_t *tf, int signo, const struct k_sigaction *sa,
                        const struct k_siginfo *info, uint64_t oldmask)
{
    (void)tf;
    (void)signo;
    (void)sa;
    (void)info;
    (void)oldmask;
    return -1;
}
