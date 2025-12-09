#include "syscall.h"
#include "serial.h"
#include "kio.h"
#include "proc/process.h"
#include "proc/sched.h"

/*
 * Syscall function type - takes pointer to params, returns int32_t
 */
typedef int32_t (*syscall_fn_t)(isr_params_t *);

/*
 * Syscall dispatch table
 */
static syscall_fn_t _syscall_table[SYSCALL_TABLE_SIZE];

/*
 * sys_exit - terminate current process
 * Args: EBX = exit status
 * Returns: does not return
 *
 * For now, this marks the process as zombie and halts.
 * Full implementation would clean up resources and reschedule.
 */
static int32_t sys_exit(isr_params_t *params)
{
    int status = (int)params->ebx;

    serial_printf("sys_exit: Process %d exited with status %d\n",
                  current_proc ? (int)current_proc->pid : -1, status);
    k_printf("\nProcess exited with status %d\n", status);

    /* Mark process as zombie */
    if (current_proc) {
        current_proc->state = PROC_ZOMBIE;
        current_proc->exit_code = status;
    }

    /* Halt the CPU - for now, sys_exit just stops everything */
    __asm__ __volatile__("cli; hlt");

    /* Never reached */
    return 0;
}

/*
 * sys_write - write to file descriptor
 * Args: EBX = fd, ECX = buf, EDX = count
 * Returns: number of bytes written, or -1 on error
 */
static int32_t sys_write(isr_params_t *params)
{
    int fd = (int)params->ebx;
    const char *buf = (const char *)params->ecx;
    uint32_t count = params->edx;
    uint32_t i;

    /* fd 1 (stdout) and fd 2 (stderr) write to VGA and serial */
    if (fd == 1 || fd == 2) {
        for (i = 0; i < count; i++) {
            k_putc(buf[i]);
            serial_putc(buf[i]);
        }
        return (int32_t)count;
    }

    /* Unknown fd */
    return -1;
}

/*
 * Syscall handler - dispatches to appropriate syscall function
 */
static void _syscall_handler(isr_params_t params)
{
    uint32_t syscall_num = params.eax;

    if (syscall_num < SYSCALL_TABLE_SIZE && _syscall_table[syscall_num]) {
        _syscall_table[syscall_num](&params);
    }
}

void init_syscall(void)
{
    uint32_t i;

    /* Clear dispatch table */
    for (i = 0; i < SYSCALL_TABLE_SIZE; i++) {
        _syscall_table[i] = NULL;
    }

    /* Register implemented syscalls */
    _syscall_table[SYS_EXIT] = sys_exit;
    _syscall_table[SYS_WRITE] = sys_write;

    /* Register the interrupt handler */
    register_isr_handler(SYSCALL_VECTOR, _syscall_handler);
}
