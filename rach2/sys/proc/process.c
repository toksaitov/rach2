#include "process.h"
#include "../lib/kstring.h"
#include "../fs/fd.h"
#include "../panic.h"

/*
 * Offset of a PCB field, without pulling in <stddef.h> (whose size_t would
 * clash with kdef.h). Pointer and uint32_t are the same width on i386.
 */
#define PCB_OFF(field) ((uint32_t)&((process_t *)0)->field)

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

    /* switch.s hard-codes these PCB field offsets; assert they still match the
     * struct so a future field insertion fails loudly here at boot instead of
     * silently corrupting context switches. Keep in sync with the .set values
     * in proc/switch.s. */
    if (PCB_OFF(regs.esp) != 48 ||
        PCB_OFF(cr3) != 52 ||
        PCB_OFF(kernel_stack) != 56 ||
        PCB_OFF(tls_base) != 584) {
        k_panic("process_t layout drifted from switch.s offsets");
    }

    /* Clear all process slots */
    for (i = 0; i < MAX_PROCS; i++) {
        k_memset(&proc_table[i], 0, sizeof(process_t));
        proc_table[i].state = PROC_UNUSED;
    }

    next_pid = 0;
    current_proc = NULL;
}

process_t *alloc_process(void)
{
    uint32_t i;

    /* Find first unused slot */
    for (i = 0; i < MAX_PROCS; i++) {
        if (proc_table[i].state == PROC_UNUSED) {
            uint32_t candidate;
            k_memset(&proc_table[i], 0, sizeof(process_t));
            /* Assign a pid bounded to the positive int range -- the /proc path
             * layer carries pid as int -- skipping any still-live pid so live pids
             * stay unique. The wrap is unreachable in practice (~2e9 spawns), so
             * this is normally just next_pid++. */
            do {
                if (next_pid >= 2000000000u) {
                    next_pid = 1;
                }
                candidate = next_pid++;
            } while (get_process(candidate) != NULL);
            proc_table[i].pid = candidate;
            proc_table[i].state = PROC_READY;

            /* Initialize file descriptor table with stdin/stdout/stderr */
            fd_table_init(&proc_table[i].fds);
            fd_init_stdio(&proc_table[i].fds);

            /* Initialize current working directory to root */
            proc_table[i].cwd[0] = '/';
            proc_table[i].cwd[1] = '\0';

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

void proc_wake_sleepers(uint32_t now)
{
    uint32_t i;

    /* Move any timed sleeper (nanosleep) whose wakeup tick has arrived from
     * BLOCKED back to READY so the scheduler runs it. Called from the timer tick.
     * The (int32_t) difference is tick-wraparound-safe (~248 days at 100 Hz). */
    for (i = 0; i < MAX_PROCS; i++) {
        process_t *p = &proc_table[i];
        if (p->state == PROC_BLOCKED && p->wakeup_tick != 0 &&
            (int32_t)(now - p->wakeup_tick) >= 0) {
            p->wakeup_tick = 0;
            p->state = PROC_READY;
        }
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

void proc_set_comm(process_t *p, const char *path)
{
    const char *base;
    const char *q;
    int i;

    if (!p || !path) {
        return;
    }
    base = path;
    for (q = path; *q; q++) {
        if (*q == '/') {
            base = q + 1; /* keep only the final component */
        }
    }
    for (i = 0; i < (int)sizeof(p->comm) - 1 && base[i]; i++) {
        p->comm[i] = base[i];
    }
    p->comm[i] = '\0';
}

void setup_process_stack(process_t *proc, void (*entry_point)(void))
{
    uint32_t *sp;

    /*
     * Set up the kernel stack for first context switch.
     * switch_context expects the stack to have saved callee registers:
     *
     *   [entry_point]  <- return address (where to start)
     *   [ebp]          <- saved EBP (0 for new process)
     *   [ebx]          <- saved EBX (0)
     *   [esi]          <- saved ESI (0)
     *   [edi]          <- saved EDI (0) <- ESP points here
     */

    sp = (uint32_t *)proc->kernel_stack;

    /* Push return address (entry point) */
    --sp;
    *sp = (uint32_t)entry_point;

    /* Push saved callee registers (all zero for new process) */
    --sp;
    *sp = 0;  /* EBP */
    --sp;
    *sp = 0;  /* EBX */
    --sp;
    *sp = 0;  /* ESI */
    --sp;
    *sp = 0;  /* EDI */

    /* Save stack pointer in process regs */
    proc->regs.esp = (uint32_t)sp;
}
