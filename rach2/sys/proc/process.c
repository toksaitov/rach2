#include "process.h"
#include "../lib/kstring.h"
#include "../serial.h"

/*
 * Process table - all process slots
 */
process_t proc_table[MAX_PROCS];

/*
 * Currently running process
 */
process_t *current_proc = NULL;

/*
 * Next PID to assign
 */
static uint32_t next_pid = 0;

void init_proc(void)
{
    uint32_t i;

    /* Clear all process slots */
    for (i = 0; i < MAX_PROCS; i++) {
        k_memset(&proc_table[i], 0, sizeof(process_t));
        proc_table[i].state = PROC_UNUSED;
    }

    next_pid = 0;
    current_proc = NULL;

    serial_printf("Process subsystem initialized\n");
}

process_t *alloc_process(void)
{
    uint32_t i;

    /* Find first unused slot */
    for (i = 0; i < MAX_PROCS; i++) {
        if (proc_table[i].state == PROC_UNUSED) {
            k_memset(&proc_table[i], 0, sizeof(process_t));
            proc_table[i].pid = next_pid++;
            proc_table[i].state = PROC_READY;
            return &proc_table[i];
        }
    }

    /* No free slots */
    return NULL;
}

void free_process(process_t *proc)
{
    if (proc) {
        proc->state = PROC_UNUSED;
    }
}

process_t *get_process(uint32_t pid)
{
    uint32_t i;

    for (i = 0; i < MAX_PROCS; i++) {
        if (proc_table[i].state != PROC_UNUSED && proc_table[i].pid == pid) {
            return &proc_table[i];
        }
    }

    return NULL;
}
