#include "syscall.h"
#include "serial.h"

/*
 * Syscall handler - reads syscall number from EAX and dispatches.
 * Currently just prints the syscall number for debugging.
 */
static void _syscall_handler(isr_params_t params)
{
    uint32_t syscall_num = params.eax;

    serial_printf("Syscall: %d\n", syscall_num);
}

void init_syscall(void)
{
    register_isr_handler(SYSCALL_VECTOR, _syscall_handler);
}
