/*
 * process.c: the process list, the making and freeing of records, and the
 * parts of a process's end and of the timer tick that walk the list.
 */
#include "process.h"
#include "../lib/kstring.h"
#include "../fs/fd.h"
#include "../fs/pathbuf.h"
#include "../panic.h"
#include "../mm/kheap.h"
#include "../lib/kirq.h"
#include "sched.h"
#include "tgroup.h"
#include "seccomp.h"
#include "ns.h"

extern volatile uint32_t kernel_ticks;

process_t *proc_list;
process_t *proc_idle;
static process_t *proc_tail; /* the last record of proc_list */

struct percpu cpu_table[NR_CPUS];

uint32_t proc_dead_threads;

int proc_in_syscall(void)
{
    return current_proc != NULL && current_proc->in_syscall != 0;
}

/* The pid of init, owned by kernel.c; reparent_children adopts orphans onto
 * it. */
extern volatile uint32_t init_pid;

/* The next pid `alloc_process` tries, and the last it gave out. */
static uint32_t next_pid = 0;
static uint32_t last_pid;

uint32_t proc_last_pid(void)
{
    return last_pid;
}

/**
 * The layout is checked at boot in every build, for the kernel TinyCC
 * builds, since that compiler cannot check it at build time.
 */
void init_proc(void)
{
    if (arch_context_check() != 0) {
        k_panic("process: the record's layout is not what the context switch reads");
    }
    proc_list = NULL;
    proc_tail = NULL;
    proc_idle = NULL;
    next_pid = 0;
    current_proc = NULL;
}

/**
 * `next_pid` starts at 0 and the boot self-tests spawn processes before init
 * exists, but init must be pid 1: `kill(1, ...)` is how BusyBox's `halt`,
 * `poweroff` and `reboot` ask for a shutdown. `alloc_process` skips any pid
 * that is still live, so a self-test record that outlived its test would
 * push init to the next free number and not collide with it.
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
 * The pid is kept in the positive int range, since the /proc path layer has
 * a pid as an int, and a pid still in use is skipped, so that the pids in
 * use stay unique; the wrap comes only after two thousand million records,
 * and the pid is otherwise `next_pid`. fork gives the child the parent's
 * session and process group in place of its own.
 */
process_t *alloc_process(void)
{
    process_t *p = kmalloc(sizeof(*p));
    uint32_t candidate;

    if (!p) {
        return NULL;
    }
    k_memset(p, 0, sizeof(*p));
    p->start_ticks = kernel_ticks;
    p->fpu = fpu_area_alloc();
    if (!p->fpu) {
        kfree(p);
        return NULL;
    }
    p->tg = tgroup_new(p);
    if (!p->tg) {
        fpu_area_free(p->fpu);
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
    last_pid = candidate;
    p->pid = candidate;
    p->tgid = candidate;
    p->state = PROC_READY;
    p->sid = candidate;
    p->pgid = candidate;
    p->rlim[RLIM_NPROC].cur = p->rlim[RLIM_NPROC].max = PROC_COUNT_POLICY;
    p->rlim[RLIM_NOFILE].cur = p->rlim[RLIM_NOFILE].max = FD_TABLE_LIMIT;
    p->rlim[RLIM_AS].cur = p->rlim[RLIM_AS].max = RLIM_INFINITY;
    p->rlim[RLIM_STACK].cur = RLIM_STACK_DEFAULT;
    p->rlim[RLIM_STACK].max = RLIM_INFINITY;
    p->rlim[RLIM_CORE].cur = 0;
    p->rlim[RLIM_CORE].max = RLIM_INFINITY;
    p->rlim[RLIM_FSIZE].cur = p->rlim[RLIM_FSIZE].max = RLIM_INFINITY;
    p->waiter.proc = p;
    p->cpu = (int)arch_cpu_id();
    p->affinity = ~(uint64_t)0;

    p->fs = kzalloc(sizeof(*p->fs));
    if (!p->fs) {
        tgroup_put(p->tg);
        fpu_area_free(p->fpu);
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
        fpu_area_free(p->fpu);
        kfree(p);
        return NULL;
    }
    if (fd_init_stdio(p->fds) != 0) {
        fd_table_delete(p->fds);
        kfree(p->fs);
        tgroup_put(p->tg);
        fpu_area_free(p->fpu);
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
    seccomp_release(proc);
    ns_release(proc);
    kfree(proc->exec_ws);
    path_bufs_free(proc);
    fpu_area_free(proc->fpu);
    kfree(proc);
}

/**
 * The timed sleepers are nanosleep, poll with a deadline and a wait-queue
 * wait with one, such as the kernel's ping. `signal_post` makes a blocked
 * process ready too: a nanosleep it wakes then ends with -EINTR if it runs
 * again before its deadline tick and the process acts on SIGALRM, and
 * returns 0 if it first runs at that tick or later; a wait that goes on
 * arms its deadline again. The `(int32_t)` difference of two ticks is right
 * across the wrap of the count, for deadlines less than 2^31 ticks away,
 * about 248 days at 100 Hz.
 */
void proc_wake_sleepers(uint32_t now)
{
    process_t *p;

    for (p = proc_list; p; p = p->next) {
        if (p->state == PROC_BLOCKED && p->wakeup_tick != 0 &&
            (int32_t)(now - p->wakeup_tick) >= 0) {
            p->wakeup_tick = 0;
            sched_make_ready(p);
        }
        if (p->alarm_tick != 0 && (int32_t)(now - p->alarm_tick) >= 0) {
            p->alarm_tick = p->alarm_interval != 0 ? proc_alarm_at(now, p->alarm_interval) : 0;
            signal_post(p, SIGALRM);
        }
    }
}

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

isr_params_t *proc_user_frame(const process_t *t)
{
    return (isr_params_t *)(t->kernel_stack - sizeof(isr_params_t));
}

/**
 * A child left with a dead parent's pid would never be matched in `do_wait`,
 * and its frames, page directory and record would leak. A child whose
 * threads have all ended has no exit to come that would tell init, so init
 * is sent SIGCHLD now and its wait woken. When the system's init itself is
 * ending, no other init is left to adopt the children. The parent-death
 * signals are posted after the adoption, so that a handler that asks for
 * its parent finds init, as on Linux.
 */
void reparent_children(uint32_t dead_pid)
{
    process_t *system_init = NULL;
    process_t *p;

    if (init_pid != 0 && dead_pid != init_pid) {
        system_init = get_process(init_pid);
    }
    for (p = proc_list; p; p = p->next) {
        process_t *init = system_init;

        if (p->parent_pid != dead_pid) {
            continue;
        }
        if (ns_reaper(p) != 0 && ns_reaper(p) != dead_pid) {
            init = get_process(ns_reaper(p));
        }
        if (init != NULL) {
            p->parent_pid = init->pid;
            if (p->state == PROC_ZOMBIE && p->pid == p->tgid && p->tg->live == 0) {
                struct k_siginfo info;

                proc_child_info(p, init, &info);
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
 * The rest of `comm` is cleared since prctl's PR_GET_NAME and a core file's
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
            base = q + 1;
        }
    }
    k_memset(p->comm, 0, sizeof(p->comm));
    for (i = 0; i < (int)sizeof(p->comm) - 1 && base[i]; i++) {
        p->comm[i] = base[i];
    }
}

/**
 * The architecture lays the stack out as its `switch_context` finds it
 * (`arch_context_init`). The switch passes the big lock to the thread it
 * enters, and `schedule` gives the processor that thread's depth, so the
 * thread begins holding the lock once, as a forked child and an idle task
 * do; with a depth of 0 its first interrupt would wait for the lock its own
 * processor holds.
 */
void setup_process_stack(process_t *proc, void (*entry_point)(void))
{
    proc->bkl_depth = 1;
    arch_context_init(proc, entry_point);
}

void proc_child_info(const process_t *child, const process_t *receiver, struct k_siginfo *info)
{
    uint32_t status = (uint32_t)child->exit_code;

    k_memset(info, 0, sizeof(*info));
    info->pid = pid_seen_by(receiver, child->pid);
    info->uid = ns_uid_from_kernel(proc_userns(receiver), child->ruid);
    if ((status & 0x7Fu) == 0) {
        info->code = CLD_EXITED;
        info->status = (int32_t)((status >> 8) & 0xFFu);
    } else {
        info->code = (status & 0x80u) ? CLD_DUMPED : CLD_KILLED;
        info->status = (int32_t)(status & 0x7Fu);
    }
}
