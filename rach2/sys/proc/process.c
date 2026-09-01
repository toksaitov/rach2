#include "process.h"
#include "../lib/kstring.h"
#include "../fs/fd.h"
#include "../panic.h"

/*
 * Offset of a PCB field, without pulling in <stddef.h> (whose size_t would
 * clash with kdef.h). Pointer and uint32_t are the same width on i386.
 */
#define PCB_OFF(field) ((uint32_t)&((process_t *)0)->field)

process_t proc_table[MAX_PROCS];

process_t *current_proc = NULL;

/**
 * Whether another process may be scheduled on the way out of an interrupt.
 *
 * Asked by the interrupt router, which reschedules after a message vector so
 * that a reader woken by it runs without waiting for the next tick.
 *
 * A user process interrupted in user mode may be preempted, and so may the
 * idle loop and a kernel thread. A process running kernel code inside its own
 * system call may not: its state is half changed and the call must finish. A
 * process blocked inside one is a different thing again. It is not running,
 * it is waiting, and the processor is in its context only because nothing
 * else was runnable; switching away from it is what makes the wake worth
 * anything, and it is what the timer's tick would do a few milliseconds
 * later in any case.
 */
int proc_preemptible(void)
{
    return current_proc == NULL || current_proc->in_syscall == 0 ||
           current_proc->state != PROC_RUNNING;
}

/* The pid of init, owned by kernel.c; reparent_children adopts orphans onto
 * it. */
extern volatile uint32_t init_pid;

static uint32_t next_pid = 0;

/**
 * Reset the process table to its boot state and verify the PCB layout.
 *
 * `switch.s` hard-codes the byte offsets of the registers it saves and
 * restores during a context switch. Before clearing the table this asserts
 * those offsets still match `process_t`, so a future field insertion fails
 * loudly here at boot instead of silently corrupting context switches.
 */
void init_proc(void)
{
    uint32_t i;

    /* Keep in sync with the .set values in proc/switch.s. */
    if (PCB_OFF(regs.esp) != 48 ||
        PCB_OFF(cr3) != 52 ||
        PCB_OFF(kernel_stack) != 56 ||
        PCB_OFF(tls_base) != 584) {
        k_panic("process_t layout drifted from switch.s offsets");
    }

    for (i = 0; i < MAX_PROCS; i++) {
        k_memset(&proc_table[i], 0, sizeof(process_t));
        proc_table[i].state = PROC_UNUSED;
    }

    next_pid = 0;
    current_proc = NULL;
}

/**
 * Restart pid numbering at 1, for the boot that is about to start init.
 *
 * Every Unix says init is pid 1, and until this call ran, this one did not:
 * `next_pid` starts at 0 and the boot self-tests spawn processes before init
 * exists, so init took whatever number the counter had reached and the kernel
 * carried `init_pid` to remember it. That was not only untidy. `kill(1, ...)`
 * is how the shipped `halt`, `poweroff` and `reboot` ask for a shutdown, and
 * pid 1 was a dead self-test process, so the request went nowhere and the
 * programs failed.
 *
 * Safe by construction, not by timing: `alloc_process` already skips any
 * pid that is still live, so a self-test slot that outlived its test would push
 * init to the next free number, not collide with it.
 */
void proc_reclaim_boot_pids(void)
{
    next_pid = 1;
}

/* The running total /proc/stat reports; see process.h. */
static uint32_t _forks_since_boot = 0;

uint32_t process_forks_since_boot(void)
{
    return _forks_since_boot;
}

void process_count_fork(void)
{
    ++_forks_since_boot;
}

/**
 * Claim the first unused process slot and return it ready to run.
 *
 * The new pid is bounded to the positive int range, since the /proc path
 * layer has pid as an int, and any still-live pid is skipped so live
 * pids stay unique. The wrap is unreachable in practice (~2e9 spawns), so
 * the assignment is normally just `next_pid++`. The process starts as the
 * leader of its own session and process group; fork() overrides `sid` and
 * `pgid` to inherit the parent's.
 *
 * @return the slot, or NULL when the table is full.
 */
process_t *alloc_process(void)
{
    uint32_t i;

    for (i = 0; i < MAX_PROCS; i++) {
        if (proc_table[i].state == PROC_UNUSED) {
            uint32_t candidate;
            k_memset(&proc_table[i], 0, sizeof(process_t));
            do {
                if (next_pid >= 2000000000u) {
                    next_pid = 1;
                }
                candidate = next_pid++;
            } while (get_process(candidate) != NULL);
            proc_table[i].pid = candidate;
            proc_table[i].state = PROC_READY;
            proc_table[i].sid = candidate;
            proc_table[i].pgid = candidate;
            proc_table[i].umask = 022;

            fd_table_init(&proc_table[i].fds);
            fd_init_stdio(&proc_table[i].fds);

            proc_table[i].cwd[0] = '/';
            proc_table[i].cwd[1] = '\0';

            process_count_fork();
            return &proc_table[i];
        }
    }

    return NULL;
}

void free_process(process_t *proc)
{
    if (proc) {
        proc->state = PROC_UNUSED;
    }
}

/**
 * Wake timed sleepers whose deadline has passed.
 *
 * Moves any nanosleep sleeper whose wakeup tick has arrived from BLOCKED back
 * to READY so the scheduler runs it. Called from the timer tick. The
 * `(int32_t)` difference is tick-wraparound-safe (~248 days at 100 Hz).
 */
void proc_wake_sleepers(uint32_t now)
{
    uint32_t i;

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

/**
 * Reparent every child of `dead_pid` onto init.
 *
 * Called from a process's death path before its slot is reclaimed, so a
 * dying process never strands a child on a parent pid about to be freed
 * (that orphan would otherwise leak its frames, page directory, and zombie
 * slot, since no live process would ever match it in `do_wait`). A child
 * that is already a zombie has no future exit event to wake init, so init is
 * signalled now to reap it; `signal_post` flips a blocked init to ready
 * itself. A no-op when there is no init yet (`init_pid == 0`, the
 * debug-shell-only boot) or when init itself is the one dying (it has no
 * surviving init to adopt to, and the kernel panics on init's death anyway).
 */
void reparent_children(uint32_t dead_pid)
{
    process_t *init;
    uint32_t i;

    if (init_pid == 0 || dead_pid == init_pid) {
        return;
    }
    init = get_process(init_pid);
    if (init == NULL) {
        return;
    }
    for (i = 0; i < MAX_PROCS; i++) {
        process_t *p = &proc_table[i];

        if (p->state == PROC_UNUSED || p->parent_pid != dead_pid) {
            continue;
        }
        p->parent_pid = init_pid;
        if (p->state == PROC_ZOMBIE) {
            signal_post(init, SIGCHLD);
        }
    }
}

/**
 * Set a process's `comm` name from a path's final component.
 *
 * Stores the basename of `path`, truncated to fit `comm` and NUL-terminated.
 */
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

/**
 * Prime a new process's kernel stack for its first context switch.
 *
 * `switch_context` restores callee-saved registers and rets, so it expects
 * the stack to look as if it had pushed them. The frame this builds, from
 * high to low address:
 *
 *     [entry_point]  return address (where execution begins)
 *     [ebp]          saved EBP (0 for a new process)
 *     [ebx]          saved EBX (0)
 *     [esi]          saved ESI (0)
 *     [edi]          saved EDI (0)   <- regs.esp points here
 */
void setup_process_stack(process_t *proc, void (*entry_point)(void))
{
    uint32_t *sp;

    sp = (uint32_t *)proc->kernel_stack;

    --sp;
    *sp = (uint32_t)entry_point;

    --sp;
    *sp = 0;  /* EBP */
    --sp;
    *sp = 0;  /* EBX */
    --sp;
    *sp = 0;  /* ESI */
    --sp;
    *sp = 0;  /* EDI */

    proc->regs.esp = (uint32_t)sp;
}

