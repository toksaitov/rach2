#ifndef _PROCESS_H_
#define _PROCESS_H_

#include "../lib/kdef.h"
#include "../lib/kint.h"

/*
 * Maximum number of processes
 */
#define MAX_PROCS 16

/*
 * Process states
 */
typedef enum {
    PROC_UNUSED,    /* Slot is free */
    PROC_READY,     /* Ready to run */
    PROC_RUNNING,   /* Currently executing */
    PROC_BLOCKED,   /* Waiting for I/O or event */
    PROC_ZOMBIE     /* Exited, waiting for parent to collect */
} proc_state_t;

/*
 * Saved CPU registers for context switching
 * Layout matches what pushal/popal expect
 */
struct cpu_regs {
    uint32_t edi;
    uint32_t esi;
    uint32_t ebp;
    uint32_t esp_dummy;  /* Ignored by popal */
    uint32_t ebx;
    uint32_t edx;
    uint32_t ecx;
    uint32_t eax;
    uint32_t eip;
    uint32_t eflags;
    uint32_t esp;        /* User stack pointer */
};

typedef struct cpu_regs cpu_regs_t;

/*
 * Process Control Block (PCB)
 */
struct process {
    uint32_t pid;           /* Process ID */
    proc_state_t state;     /* Current state */
    cpu_regs_t regs;        /* Saved CPU registers */
    uint32_t cr3;           /* Page directory physical address */
    uint32_t kernel_stack;  /* Top of kernel stack for this process */
    uint32_t parent_pid;    /* Parent process ID */
    int32_t exit_code;      /* Exit status (for zombie) */
};

typedef struct process process_t;

/*
 * Process table
 */
extern process_t proc_table[MAX_PROCS];

/*
 * Currently running process
 */
extern process_t *current_proc;

/*
 * Initialize the process subsystem
 */
void init_proc(void);

/*
 * Allocate a new process slot
 * Returns pointer to process, or NULL if table full
 */
process_t *alloc_process(void);

/*
 * Free a process slot
 */
void free_process(process_t *proc);

/*
 * Get process by PID
 * Returns pointer to process, or NULL if not found
 */
process_t *get_process(uint32_t pid);

/*
 * Context switch between two processes
 * Saves current state to old, restores from new
 */
extern void switch_context(process_t *old, process_t *new_proc);

/*
 * Set up a process's kernel stack for first context switch
 * entry_point: function to start executing
 * Returns: nothing, modifies proc->regs.esp
 */
void setup_process_stack(process_t *proc, void (*entry_point)(void));

#endif
