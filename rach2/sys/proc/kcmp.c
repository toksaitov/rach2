/*
 * kcmp.c: kcmp(2), the comparison of two processes' kernel objects. A stub
 * until 76-4a writes it and the system call tables name it, which answers
 * -ENOSYS, as the unnamed call does.
 */

#include "kcmp.h"

#include "../lib/kerrno.h"

long sys_kcmp(isr_params_t *params)
{
    (void)params;
    return -ENOSYS;
}
