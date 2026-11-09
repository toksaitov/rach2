/*
 * mempolicy.c: get_mempolicy(2) on a machine of one memory node. A stub until
 * 76-19 writes it and the system call tables name it, which answers -ENOSYS,
 * as the unnamed call does.
 */

#include "mempolicy.h"

#include "../lib/kerrno.h"

long sys_get_mempolicy(isr_params_t *params)
{
    (void)params;
    return -ENOSYS;
}
