/*
 * process_vm.c: process_vm_readv(2) and process_vm_writev(2). A stub until
 * 76-19 writes it and the system call tables name it, which answers -ENOSYS,
 * as the unnamed call does.
 */

#include "process_vm.h"

#include "../lib/kerrno.h"

long sys_process_vm_readv(isr_params_t *params)
{
    (void)params;
    return -ENOSYS;
}

long sys_process_vm_writev(isr_params_t *params)
{
    (void)params;
    return -ENOSYS;
}
