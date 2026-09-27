#include "process.h"
#include "../lib/kstring.h"
#include "../fs/fd.h"
#include "../fs/pathbuf.h"
#include "../panic.h"
#include "../mm/kheap.h"
#include "../lib/kirq.h"
#include "sched.h"
#include "tgroup.h"

/* The offsets switch.s reads from the process record, as `.set` values
 * there and as numbers here; a field added before one of them stops the
 * GNU build. TinyCC cannot fold an offset into an array size, so a kernel it
 * builds finds the change at boot, in `init_proc`, which checks every
 * build. A host test compiles this file with 64-bit pointers, where the
 * offsets differ, and skips the compile-time check. */
#define PROC_OFF(field)       ((uint32_t)&((process_t *)0)->field)
#define PROC_OFF_REGS_ESP     48u
#define PROC_OFF_CR3          52u
#define PROC_OFF_KERNEL_STACK 56u
#define PROC_OFF_TLS_BASE     80u
#define PROC_OFF_PAGE_DIR     720u
#if !defined(__TINYC__) && !defined(RACH2_HOSTTEST)
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

uint32_t proc_dead_threads;

/**
 * Whether this processor is inside a user process's system call.
 *
 * Asked by the code whose rules depend on it: the ATA driver's wait and
 * reclaim's writeback sleep only inside a call, and the USB disk settles a
 * stick only outside one. The idle process and a kernel thread answer no.
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
 * `next_pid` starts at 0 and the boot self-tests spawn processes before init
 * exists, but init must be pid 1: `kill(1, ...)` is how the shipped `halt`,
 * `poweroff` and `reboot` ask for a shutdown. `alloc_process` skips any pid
 * that is still live, so a self-test record that outlived its test would
 * push init to the next free number, not collide with it.
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

/* The sleeps a signal ended before their deadline; see process.h. */
static uint32_t _sleeps_cut_short = 0;

uint32_t process_sleeps_cut_short(void)
{
    return _sleeps_cut_short;
}

void process_count_sleep_cut_short(void)
{
    ++_sleeps_cut_short;
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
 * Return the process, or NULL if its record, descriptor table or standard
 * descriptions cannot be allocated.
 */
process_t *alloc_process(void)
{
    process_t *p = kmalloc(sizeof(*p));
    uint32_t candidate;

    if (!p) {
        return NULL;
    }
    k_memset(p, 0, sizeof(*p));
    p->tg = tgroup_new(p);
    if (!p->tg) {
        kfree(p);
        return NULL;
    }
    p->sigacts = p->tg->action;
    do {
        if (next_pid >= 2000000000u) {
            next_pid = 1;
        }
        candidate = next_pid++;
    } while (get_process(candidate) != NULL);
    p->pid = candidate;
    p->tgid = candidate;
    p->state = PROC_READY;
    p->sid = candidate;
    p->pgid = candidate;
    p->rlim[RLIM_NPROC].cur = p->rlim[RLIM_NPROC].max = PROC_COUNT_POLICY;
    p->rlim[RLIM_NOFILE].cur = p->rlim[RLIM_NOFILE].max = FD_TABLE_LIMIT;
    p->rlim[RLIM_AS].cur = p->rlim[RLIM_AS].max = RLIM_INFINITY32;
    p->rlim[RLIM_STACK].cur = RLIM_STACK_DEFAULT;
    p->rlim[RLIM_STACK].max = RLIM_INFINITY32;
    p->rlim[RLIM_CORE].cur = 0;
    p->rlim[RLIM_CORE].max = RLIM_INFINITY32;
    fpu_init_area(p->fpu);
    p->waiter.proc = p;

    p->fs = kzalloc(sizeof(*p->fs));
    if (!p->fs) {
        tgroup_put(p->tg);
        kfree(p);
        return NULL;
    }
    p->fs->refs = 1;
    p->fs->cwd[0] = '/';
    p->fs->umask = 022;
    p->fds = fd_table_new();
    if (!p->fds) {
        kfree(p->fs);
        tgroup_put(p->tg);
        kfree(p);
        return NULL;
    }
    if (fd_init_stdio(p->fds) != 0) {
        fd_table_delete(p->fds);
        kfree(p->fs);
        tgroup_put(p->tg);
        kfree(p);
        return NULL;
    }

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
    if (proc->fds && --proc->fds->refs == 0) {
        fd_table_delete(proc->fds);
    }
    if (proc->fs && --proc->fs->refs == 0) {
        kfree(proc->fs->cwd_long);
        kfree(proc->fs);
    }
    tgroup_put(proc->tg);
    kfree(proc->exec_ws);
    path_bufs_free(proc);
    kfree(proc);
}

/**
 * Wake timed sleepers whose deadline has passed, and post the alarms due.
 *
 * Moves any timed sleeper (nanosleep, poll with a deadline, or a wait-queue
 * wait with one, such as the kernel's ping) whose wakeup tick has arrived
 * from BLOCKED back to READY so the scheduler runs it, and posts SIGALRM to
 * each process whose real-time timer is due, arming the timer again for its
 * interval or leaving it disarmed. `signal_post` makes a blocked process
 * runnable. A nanosleep it wakes then ends with -EINTR if it runs again
 * before its deadline tick and the process acts on SIGALRM, and returns 0 if
 * it first runs at that tick or later; a wait that goes on arms its deadline
 * again. Called from the timer tick. The `(int32_t)` difference is
 * tick-wraparound-safe (~248 days at 100 Hz).
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
        if (p->alarm_tick != 0 && (int32_t)(now - p->alarm_tick) >= 0) {
            p->alarm_tick = p->alarm_interval != 0 ? proc_alarm_at(now, p->alarm_interval) : 0;
            signal_post(p, SIGALRM);
        }
    }
}

/**
 * The tick `ticks` after `now` at which an alarm is due: never 0, which
 * `alarm_tick` keeps for a disarmed timer.
 */
uint32_t proc_alarm_at(uint32_t now, uint32_t ticks)
{
    uint32_t at = now + ticks;

    return at != 0 ? at : 1u;
}

struct tgroup *tgroup_new(struct process *leader)
{
    struct tgroup *tg = kzalloc(sizeof(*tg));

    if (tg) {
        tg->refs = 1;
        tg->live = 1;
        tg->leader = leader;
        wait_queue_init(&tg->chld);
        wait_queue_init(&tg->gone);
    }
    return tg;
}

void tgroup_put(struct tgroup *tg)
{
    if (tg && --tg->refs == 0) {
        kfree(tg);
    }
}

void proc_cpu_time(const process_t *p, uint32_t *user, uint32_t *sys)
{
    const struct tgroup *tg = p->tg;
    const process_t *t;
    uint32_t flags = kirq_save();

    *user = tg->utime;
    *sys = tg->stime;
    for (t = proc_list; t; t = t->next) {
        if (t->tg == tg) {
            *user += t->utime;
            *sys += t->stime;
        }
    }
    kirq_restore(flags);
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
 * Reparent every child of `dead_pid` onto init, and post each living child
 * the parent-death signal it asked prctl for.
 *
 * Called from a process's death path before its record is reclaimed: a
 * child left with a dead parent's pid would never be matched in `do_wait`,
 * and its frames, page directory and record would leak. A child whose
 * threads have all ended has no future exit event to wake init, so init is
 * signalled now to reap it and a wait init is in is woken. No child is
 * adopted when there is no init yet (`init_pid == 0`,
 * the debug-shell-only boot) or when init itself is the one dying, since no
 * other init is left to adopt them; the parent-death signals are posted
 * either way, after the adoption, so a handler that asks for its parent
 * finds init, as on Linux.
 */
void reparent_children(uint32_t dead_pid)
{
    process_t *init = NULL;
    process_t *p;

    if (init_pid != 0 && dead_pid != init_pid) {
        init = get_process(init_pid);
    }
    for (p = proc_list; p; p = p->next) {
        if (p->parent_pid != dead_pid) {
            continue;
        }
        if (init != NULL) {
            p->parent_pid = init_pid;
            if (p->state == PROC_ZOMBIE && p->pid == p->tgid && p->tg->live == 0) {
                struct k_siginfo info;

                proc_child_info(p, &info);
                signal_post_info(init, SIGCHLD, &info);
                wq_wake_all(&init->tg->chld);
            }
        }
        if (p->pdeath_signal != 0 && p->state != PROC_ZOMBIE) {
            signal_post(p, (int)p->pdeath_signal);
        }
    }
}

/**
 * Set a process's `comm` name from a path's final component.
 *
 * Stores the basename of `path`, truncated to fit `comm`, with null bytes
 * to the end of `comm`, since prctl's PR_GET_NAME and a core file's
 * prpsinfo copy all of it: a shorter name must leave nothing of a longer one
 * behind it.
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
    k_memset(p->comm, 0, sizeof(p->comm));
    for (i = 0; i < (int)sizeof(p->comm) - 1 && base[i]; i++) {
        p->comm[i] = base[i];
    }
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

void proc_child_info(const process_t *child, struct k_siginfo *info)
{
    uint32_t status = (uint32_t)child->exit_code;

    k_memset(info, 0, sizeof(*info));
    info->pid = child->pid;
    info->uid = child->ruid;
    if ((status & 0x7Fu) == 0) {
        info->code = CLD_EXITED;
        info->status = (int32_t)((status >> 8) & 0xFFu);
    } else {
        info->code = (status & 0x80u) ? CLD_DUMPED : CLD_KILLED;
        info->status = (int32_t)(status & 0x7Fu);
    }
}
