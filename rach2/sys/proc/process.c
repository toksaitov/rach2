#include "process.h"
#include "../lib/kstring.h"
#include "../fs/fd.h"
#include "../panic.h"
#include "../mm/kheap.h"
#include "sched.h"

/* The offsets switch.s reads from the process record, as `.set` values
 * there and as numbers here; a field added before one of them stops the
 * GNU build, and TinyCC's, which cannot fold an offset into an array size,
 * checks them at boot in `init_proc`. */
#define PROC_OFF(field)       ((uint32_t)&((process_t *)0)->field)
#define PROC_OFF_REGS_ESP     48u
#define PROC_OFF_CR3          52u
#define PROC_OFF_KERNEL_STACK 56u
#define PROC_OFF_TLS_BASE     148u
#define PROC_OFF_PAGE_DIR     2128u
#ifndef __TINYC__
typedef char process_offsets_fit
    [(__builtin_offsetof(struct process, regs) + __builtin_offsetof(cpu_regs_t, esp) ==
          PROC_OFF_REGS_ESP &&
      __builtin_offsetof(struct process, cr3) == PROC_OFF_CR3 &&
      __builtin_offsetof(struct process, kernel_stack) == PROC_OFF_KERNEL_STACK &&
      __builtin_offsetof(struct process, tls_base) == PROC_OFF_TLS_BASE &&
      __builtin_offsetof(struct process, page_dir) == PROC_OFF_PAGE_DIR)
         ? 1
         : -1];
#endif

process_t *proc_list;
process_t *proc_idle;
static process_t *proc_tail;

process_t *current_proc = NULL;

/**
 * Whether this processor is inside a user process's system call.
 *
 * Asked by the interrupt router, which reschedules after a message vector so
 * that a reader woken by it runs without waiting for the next tick, and must
 * not do so inside a system call: `sys_exit` and `schedule` itself both run
 * with interrupts on while their own bookkeeping is half done, and a switch
 * taken there strands the process they were in the middle of moving. The idle
 * loop and a kernel thread answer no, as they do for the timer.
 */
int proc_in_syscall(void)
{
    return current_proc != NULL && current_proc->in_syscall != 0;
}

/* The pid of init, owned by kernel.c; reparent_children adopts orphans onto
 * it. */
extern volatile uint32_t init_pid;

static uint32_t next_pid = 0;

/**
 * Reset the process table to its boot state and check the record's layout.
 *
 * `switch.s` hard-codes the byte offsets of the fields it reads during a
 * context switch; `process_offsets_fit` above checks them at build time
 * where the compiler can, and this checks them again here for the build
 * that cannot.
 */
void init_proc(void)
{
    if (PROC_OFF(regs.esp) != PROC_OFF_REGS_ESP || PROC_OFF(cr3) != PROC_OFF_CR3 ||
        PROC_OFF(kernel_stack) != PROC_OFF_KERNEL_STACK ||
        PROC_OFF(tls_base) != PROC_OFF_TLS_BASE || PROC_OFF(page_dir) != PROC_OFF_PAGE_DIR) {
        k_panic("process: the record's layout is not what switch.s reads");
    }
    proc_list = NULL;
    proc_tail = NULL;
    proc_idle = NULL;
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
 * A process record from the heap, on the list and ready to run.
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
    process_t *p = kmalloc(sizeof(*p));
    uint32_t candidate;

    if (!p) {
        return NULL;
    }
    k_memset(p, 0, sizeof(*p));
    do {
        if (next_pid >= 2000000000u) {
            next_pid = 1;
        }
        candidate = next_pid++;
    } while (get_process(candidate) != NULL);
    p->pid = candidate;
    p->state = PROC_READY;
    p->sid = candidate;
    p->pgid = candidate;
    p->umask = 022;
    p->rlim[RLIM_NPROC].cur = p->rlim[RLIM_NPROC].max = PROC_COUNT_POLICY;
    p->rlim[RLIM_NOFILE].cur = p->rlim[RLIM_NOFILE].max = FD_TABLE_LIMIT;
    p->rlim[RLIM_AS].cur = p->rlim[RLIM_AS].max = RLIM_INFINITY32;
    fpu_init_area(p->fpu);
    p->waiter.proc = p;

    if (fd_table_init(&p->fds) != 0) {
        kfree(p);
        return NULL;
    }
    fd_init_stdio(&p->fds);

    p->cwd[0] = '/';
    p->cwd[1] = '\0';

    p->prev = proc_tail;
    p->next = NULL;
    if (proc_tail) {
        proc_tail->next = p;
    } else {
        proc_list = p;
    }
    proc_tail = p;

    process_count_fork();
    return p;
}

void free_process(process_t *proc)
{
    if (!proc) {
        return;
    }
    proc->state = PROC_UNUSED;
    sched_forget(proc);
    if (proc->prev) {
        proc->prev->next = proc->next;
    } else {
        proc_list = proc->next;
    }
    if (proc->next) {
        proc->next->prev = proc->prev;
    } else {
        proc_tail = proc->prev;
    }
    if (proc == proc_idle) {
        proc_idle = NULL;
    }
    fd_table_free(&proc->fds);
    vm_free(&proc->vm);
    kfree(proc->exec_ws);
    kfree(proc);
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
    process_t *p;

    for (p = proc_list; p; p = p->next) {
        if (p->state == PROC_BLOCKED && p->wakeup_tick != 0 &&
            (int32_t)(now - p->wakeup_tick) >= 0) {
            p->wakeup_tick = 0;
            p->state = PROC_READY;
        }
    }
}

process_t *get_process(uint32_t pid)
{
    process_t *p;

    for (p = proc_list; p; p = p->next) {
        if (p->pid == pid) {
            return p;
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
    process_t *p;

    if (init_pid == 0 || dead_pid == init_pid) {
        return;
    }
    init = get_process(init_pid);
    if (init == NULL) {
        return;
    }
    for (p = proc_list; p; p = p->next) {
        if (p->parent_pid != dead_pid) {
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
    *sp = 0; /* EBP */
    --sp;
    *sp = 0; /* EBX */
    --sp;
    *sp = 0; /* ESI */
    --sp;
    *sp = 0; /* EDI */

    proc->regs.esp = (uint32_t)sp;
}
