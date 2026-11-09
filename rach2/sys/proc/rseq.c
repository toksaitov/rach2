/*
 * rseq.c: rseq(2), restartable sequences. A stub until 76-19 writes it and the
 * system call tables name it, which answers -ENOSYS, as the unnamed call does.
 */

#include "rseq.h"

#include "../lib/kerrno.h"

long sys_rseq(isr_params_t *params)
{
    (void)params;
    return -ENOSYS;
}
