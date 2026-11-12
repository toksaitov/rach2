/*
 * process.c: indexed process records, user counts, active timers and
 * reparenting.
 */
#include "process.h"
#include "../lib/kstring.h"
#include "../fs/fd.h"
#include "../fs/pathbuf.h"
#include "../panic.h"
#include "../mm/kheap.h"
#include "../lib/kirq.h"
#include "../lib/spinlock.h"
#include "../lib/lockdep.h"
#include "sched.h"
#include "tgroup.h"
#include "seccomp.h"
#include "../rtc.h"
#include "../timer.h"
#include "ns.h"
#include "../tty/tty.h"

process_t *proc_list;
process_t *proc_idle;
static process_t *proc_tail; /* the last record of proc_list */

/* The pid hash doubles before its load exceeds one record per bucket. */
static process_t **pid_table;
static uint32_t pid_capacity, pid_records;
#define USER_BUCKETS 256
struct proc_user_count {
    uint32_t uid;
    uint32_t records; /* allocated records, including forks in build and zombies */
    struct proc_user_count *next;
};
static struct proc_user_count *user_table[USER_BUCKETS];
/* The timer list: the records with an active timed wait or a real alarm.
 * Its lock comes after the run queues' lock, which the wakes and the switch
 * hold when they change a record's membership, as Linux's timer base lock
 * comes after the run queue's; nothing is taken under it but the list of
 * the tick's signals, below. */
static process_t *timers;
static struct spinlock timer_list_lock = SPINLOCK_INIT_NAMED("timer_list");
/* The records with signals of the tick to post (`proc_tick_signal`), and
 * whether any has some; the list's lock is the innermost of the timers'. */
static process_t *tick_post_head;
static volatile int tick_posts_due;
static struct spinlock tick_post_lock = SPINLOCK_INIT_NAMED("tick_post");
static uint32_t work_count[4];

/** Read one process-index work counter. */
uint32_t proc_work_count(unsigned int index)
{
    return index < 4 ? work_count[index] : 0;
}

/** Remove an indexed record before changing its pid or freeing it. */
static void pid_remove(process_t *p)
{
    process_t **link = &pid_table[p->pid & (pid_capacity - 1)];

    while (*link != p) {
        work_count[0]++;
        link = &(*link)->pid_next;
    }
    work_count[0]++;
    *link = p->pid_next;
}

/** Insert a record in an allocated pid bucket. */
static void pid_insert(process_t *p)
{
    uint32_t slot = p->pid & (pid_capacity - 1);

    p->pid_next = pid_table[slot];
    pid_table[slot] = p;
}

/** Grow the pid index for one more record; -1 leaves the old index intact. */
static int pid_reserve(void)
{
    process_t **table, **old;
    process_t *p;
    uint32_t flags;
    uint32_t capacity = pid_capacity != 0 ? pid_capacity * 2 : 64;

    if (pid_records < pid_capacity) {
        return 0;
    }
    if (capacity <= pid_capacity || capacity > (uint32_t)(~(size_t)0 / sizeof(*table))) {
        return -1;
    }
    table = kzalloc(capacity * sizeof(*table));
    if (!table) {
        return -1;
    }
    flags = kirq_save();
    old = pid_table;
    pid_table = table;
    pid_capacity = capacity;
    for (p = proc_list; p; p = p->next) {
        work_count[0]++;
        pid_insert(p);
    }
    kirq_restore(flags);
    kfree(old);
    return 0;
}

/** Exchange the kernel and namespace thread ids of `a` and `b` from the
 * same PID namespace, with the big lock held and interrupts masked during
 * the exchange, as Linux 6.12.115's de_thread exchanges the whole PID
 * objects (fs/exec.c:1112). Neither allocation nor namespace counts change. */
void proc_exchange_pids(process_t *a, process_t *b)
{
    uint32_t pid = a->pid;
    uint32_t flags = kirq_save();
    uint32_t i;

    pid_remove(a);
    pid_remove(b);
    a->pid = b->pid;
    b->pid = pid;
    for (i = 0; i < NS_LEVELS; i++) {
        pid = a->nspid[i];
        a->nspid[i] = b->nspid[i];
        b->nspid[i] = pid;
    }
    pid_insert(a);
    pid_insert(b);
    kirq_restore(flags);
}

/** Release one user's count, deleting its bucket entry at the last record. */
static void user_put(struct proc_user_count *u)
{
    struct proc_user_count **link;

    if (!u || --u->records != 0) {
        return;
    }
    link = &user_table[u->uid & (USER_BUCKETS - 1)];
    while (*link != u) {
        work_count[1]++;
        link = &(*link)->next;
    }
    work_count[1]++;
    *link = u->next;
    kfree(u);
}

/** Move a record to the new real user, allocating its count if needed. */
int proc_set_ruid(process_t *p, uint32_t uid)
{
    struct proc_user_count *u;
    uint32_t slot = uid & (USER_BUCKETS - 1);

    if (p->user_count && p->ruid == uid) {
        return 0;
    }
    for (u = user_table[slot]; u; u = u->next) {
        work_count[1]++;
        if (u->uid == uid) {
            break;
        }
    }
    if (!u) {
        u = kzalloc(sizeof(*u));
        if (!u) {
            return -1;
        }
        u->uid = uid;
        u->next = user_table[slot];
        user_table[slot] = u;
    }
    u->records++;
    user_put(p->user_count);
    p->user_count = u;
    p->ruid = uid;
    return 0;
}

/** Read the number of records charged to this record's real user. */
uint32_t proc_user_records(const process_t *p)
{
    work_count[1]++;
    return p->user_count ? p->user_count->records : 0;
}

/** Keep on the timer list only the records with active timed waits or real
 * alarms. The caller holds `timer_list_lock`. */
static void timer_update_locked(process_t *p)
{
    int active =
        p->state != PROC_UNUSED && (p->alarm_tick || (p->state == PROC_BLOCKED && p->wakeup_tick));

    if (active == p->timer_linked) {
        return;
    }
    if (active) {
        p->timer_next = timers;
        p->timer_prev = NULL;
        if (timers) {
            timers->timer_prev = p;
        }
        timers = p;
    } else {
        if (p->timer_prev) {
            p->timer_prev->timer_next = p->timer_next;
        } else {
            timers = p->timer_next;
        }
        if (p->timer_next) {
            p->timer_next->timer_prev = p->timer_prev;
        }
        p->timer_next = p->timer_prev = NULL;
    }
    p->timer_linked = active;
}

void proc_timer_update(process_t *p)
{
    uint32_t flags = spin_lock_irqsave(&timer_list_lock);

    timer_update_locked(p);
    spin_unlock_irqrestore(&timer_list_lock, flags);
}

/** Take `p` off the list of the tick's signals and return the signals it
 * had waiting; the caller holds `tick_post_lock`. */
static uint32_t tick_post_unlink(process_t *p)
{
    process_t **link = &tick_post_head;
    uint32_t sigs = p->tick_sigs;

    if (!p->tick_post_linked) {
        return 0;
    }
    while (*link && *link != p) {
        link = &(*link)->tick_post_next;
    }
    if (*link) {
        *link = p->tick_post_next;
    }
    p->tick_post_next = NULL;
    p->tick_post_linked = 0;
    p->tick_sigs = 0;
    if (!tick_post_head) {
        tick_posts_due = 0;
    }
    return sigs;
}

void proc_tick_signal(process_t *p, int signo)
{
    uint32_t flags = spin_lock_irqsave(&tick_post_lock);

    if (!p->tick_post_linked) {
        p->tick_post_next = tick_post_head;
        tick_post_head = p;
        p->tick_post_linked = 1;
    }
    p->tick_sigs |= 1u << signo;
    tick_posts_due = 1;
    spin_unlock_irqrestore(&tick_post_lock, flags);
}

int proc_tick_signals_due(void)
{
    return tick_posts_due;
}

/** Set `p`'s real-time timer as `proc_alarm_set` does, and return whether
 * the tick had marked a SIGALRM of the old setting that no holder of the big
 * lock has posted yet, which this takes off the list of the tick's signals.
 * The caller holds the big lock and no spinlock. */
static int alarm_set(process_t *p, uint64_t tick, uint64_t interval)
{
    uint32_t flags = spin_lock_irqsave(&timer_list_lock);
    int marked = p->alarm_due;

    p->alarm_tick = tick;
    p->alarm_interval = tick != 0 ? interval : 0;
    if (marked) {
        p->alarm_due = 0;
        spin_lock(&tick_post_lock);
        p->tick_sigs &= ~(1u << SIGALRM);
        if (!p->tick_sigs) {
            (void)tick_post_unlink(p);
        }
        spin_unlock(&tick_post_lock);
    }
    timer_update_locked(p);
    spin_unlock_irqrestore(&timer_list_lock, flags);
    return marked;
}

void proc_alarm_set(process_t *p, uint64_t tick, uint64_t interval)
{
    if (alarm_set(p, tick, interval)) {
        signal_post(p, SIGALRM);
    }
}

uint64_t proc_alarm_due_tick(const process_t *p)
{
    uint32_t flags = spin_lock_irqsave(&timer_list_lock);
    uint64_t tick = p->alarm_due ? 0 : p->alarm_tick;

    spin_unlock_irqrestore(&timer_list_lock, flags);
    return tick;
}

/** The tick a periodic real-time timer due at `due` is due at next: the
 * first `due` plus a whole number of `interval`s after the tick `now`, so
 * that a SIGALRM taken late does not move the timer off its grid, as
 * Linux's `hrtimer_forward` steps from the old expiry
 * (kernel/time/hrtimer.c:1046-1078, from kernel/signal.c:654-662). A
 * SIGALRM taken long after `due` makes the loop run once for each interval
 * passed. */
static uint64_t alarm_next(uint64_t due, uint64_t interval, uint64_t now)
{
    uint64_t next = due + interval;

    while (next <= now) {
        next += interval;
    }
    return next != 0 ? next : 1u;
}

/** Stop `p`'s real-time timer once the SIGALRM the tick marked is posted,
 * as Linux's `it_real_fn` sends SIGALRM and leaves the timer stopped
 * (kernel/time/itimer.c:156-166): a periodic one keeps its interval and
 * the tick it was due at, from which `proc_alarm_taken` runs it again.
 * The caller holds the big lock and no spinlock. */
static void alarm_stop(process_t *p)
{
    uint32_t flags = spin_lock_irqsave(&timer_list_lock);

    p->alarm_due = 0;
    p->alarm_last = p->alarm_tick;
    p->alarm_tick = 0;
    timer_update_locked(p);
    spin_unlock_irqrestore(&timer_list_lock, flags);
}

void proc_alarm_take(process_t *to, const process_t *from)
{
    uint32_t flags = spin_lock_irqsave(&timer_list_lock);

    to->alarm_tick = from->alarm_tick;
    to->alarm_interval = from->alarm_interval;
    to->alarm_last = from->alarm_last;
    timer_update_locked(to);
    spin_unlock_irqrestore(&timer_list_lock, flags);
}

void proc_alarm_taken(process_t *p)
{
    uint32_t flags = spin_lock_irqsave(&timer_list_lock);

    if (p->alarm_tick == 0 && p->alarm_interval != 0) {
        p->alarm_tick = alarm_next(p->alarm_last, p->alarm_interval, tick_count());
        timer_update_locked(p);
    }
    spin_unlock_irqrestore(&timer_list_lock, flags);
}

/**
 * Post the signals `sigs` of the tick to `p`, SIGVTALRM and SIGPROF as
 * they come and SIGALRM followed by the stop of its timer (`alarm_stop`),
 * as the tick would have posted them had it held the big lock. A periodic
 * timer runs again only once a SIGALRM is taken (`proc_alarm_taken`), so
 * while its SIGALRM is pending no tick marks another, and getitimer
 * reports no time left with the interval, as Linux does. The caller holds
 * the big lock, under which a record is freed, and no spinlock.
 */
static void tick_signals_send(process_t *p, uint32_t sigs)
{
    if (sigs & (1u << SIGVTALRM)) {
        signal_post(p, SIGVTALRM);
    }
    if (sigs & (1u << SIGPROF)) {
        signal_post(p, SIGPROF);
    }
    if (sigs & (1u << SIGALRM)) {
        signal_post(p, SIGALRM);
        alarm_stop(p);
    }
}

void proc_tick_signals_post(void)
{
    while (tick_posts_due) {
        uint32_t flags = spin_lock_irqsave(&tick_post_lock);
        process_t *p = tick_post_head;
        uint32_t sigs = p ? tick_post_unlink(p) : 0;

        spin_unlock_irqrestore(&tick_post_lock, flags);
        if (!p) {
            return;
        }
        tick_signals_send(p, sigs);
    }
}

void proc_tick_signals_own(void)
{
    process_t *p = current_proc;
    uint32_t flags, sigs;

    if (!p || !p->tick_sigs) {
        return;
    }
    flags = spin_lock_irqsave(&tick_post_lock);
    sigs = tick_post_unlink(p);
    spin_unlock_irqrestore(&tick_post_lock, flags);
    tick_signals_send(p, sigs);
}

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

/** The next pid for a new record: `next_pid`, kept in the positive int range,
 * since the /proc path layer has a pid as an int, and skipping a pid still in
 * use, so that the pids in use stay unique; the wrap comes only after two
 * thousand million records. The caller holds the big kernel lock. */
static uint32_t pid_next(void)
{
    uint32_t candidate;

    do {
        if (next_pid >= 2000000000u) {
            next_pid = 1;
        }
        candidate = next_pid++;
    } while (get_process(candidate) != NULL);
    last_pid = candidate;
    return candidate;
}

/**
 * `next_pid` starts at 0 and the boot self-tests spawn processes before init
 * exists, but init must be pid 1: `kill(1, ...)` is how BusyBox's `halt`,
 * `poweroff` and `reboot` ask for a shutdown. A kernel thread made during
 * the boot that holds pid 1, such as the DRM commit worker a display
 * driver starts at its bind, takes the next free pid here, as nothing finds
 * a kernel thread by its pid. `alloc_process` skips any pid that is still
 * live, so a self-test's record that outlived its test would push init to
 * the next free number and not collide with it.
 */
void proc_reclaim_boot_pids(void)
{
    process_t *p = get_process(1);
    uint32_t flags;

    if (p && p->tgid == 1 && !p->page_dir && !p->space_root) {
        flags = kirq_save();
        pid_remove(p);
        next_pid = 2;
        p->pid = pid_next();
        p->tgid = p->pid;
        p->sid = p->pid;
        p->pgid = p->pid;
        pid_insert(p);
        kirq_restore(flags);
    }
    next_pid = 1;
}

/** Give a new record the resource limits every process starts with. */
static void process_limits(process_t *p)
{
    p->rlim[RLIM_NPROC].cur = p->rlim[RLIM_NPROC].max = PROC_COUNT_POLICY;
    p->rlim[RLIM_NOFILE].cur = FD_TABLE_LIMIT;
    p->rlim[RLIM_NOFILE].max = FD_NOFILE_HARD;
    p->rlim[RLIM_AS].cur = p->rlim[RLIM_AS].max = RLIM_INFINITY;
    p->rlim[RLIM_STACK].cur = RLIM_STACK_DEFAULT;
    p->rlim[RLIM_STACK].max = RLIM_INFINITY;
    p->rlim[RLIM_CORE].cur = 0;
    p->rlim[RLIM_CORE].max = RLIM_INFINITY;
    p->rlim[RLIM_FSIZE].cur = p->rlim[RLIM_FSIZE].max = RLIM_INFINITY;
    p->rlim[RLIM_MEMLOCK].cur = p->rlim[RLIM_MEMLOCK].max = RLIM_MEMLOCK_DEFAULT;
    p->rlim[RLIM_NICE].cur = p->rlim[RLIM_NICE].max = 0;
    p->rlim[RLIM_RTPRIO].cur = p->rlim[RLIM_RTPRIO].max = 0;
}

/** New directories, at / with umask 022 and one reference, or NULL. */
static struct fs_struct *fs_new(void)
{
    struct fs_struct *fs = kzalloc(sizeof(*fs));

    if (fs) {
        fs->refs = 1;
        fs->cwd[0] = '/';
        fs->umask = 022;
    }
    return fs;
}

/** Allocate a standalone record or an unfinished child of parent.
 * thread and share_fs reuse the parent's group and directories, and are
 * ignored without a parent. The child has no files yet; a standalone record
 * has stdio. NULL on allocation failure, with no record published. The
 * caller holds the big kernel lock. The pid is `pid_next`'s. fork gives the
 * child the parent's session and process group in place of its own.
 */
static process_t *process_alloc(const process_t *parent, int thread, int share_fs)
{
    process_t *p;
    struct tgroup *group = NULL;
    struct fs_struct *dirs = NULL;
    uint32_t tgid = 0, ruid = 0;
    int fd;

    bkl_assert_held();
    p = kmalloc(sizeof(*p));
    if (!p) {
        return NULL;
    }
    if (parent) {
        group = thread ? parent->tg : NULL;
        tgid = parent->tgid;
        dirs = share_fs ? parent->fs : NULL;
        ruid = parent->ruid;
    }
    k_memset(p, 0, sizeof(*p));
    p->start_ticks = tick_count();
    p->fpu = fpu_area_alloc();
    if (!p->fpu) {
        kfree(p);
        return NULL;
    }
    p->tg = group ? group : tgroup_new(p);
    if (!p->tg) {
        fpu_area_free(p->fpu);
        kfree(p);
        return NULL;
    }
    if (group) {
        group->refs++;
    }
    p->sigacts = p->tg->action;
    p->pid = pid_next();
    p->tgid = group ? tgid : p->pid;
    p->state = parent ? PROC_UNUSED : PROC_READY;
    p->sid = p->pid;
    p->pgid = p->pid;
    process_limits(p);
    p->waiter.proc = p;
    p->cpu = (int)arch_cpu_id();
    p->affinity = ~(uint64_t)0;

    p->fs = dirs ? dirs : fs_new();
    if (!p->fs) {
        tgroup_put(p->tg);
        fpu_area_free(p->fpu);
        kfree(p);
        return NULL;
    }
    if (dirs) {
        dirs->refs++;
    }
    if (!parent) {
        fd_table_t *fds = fd_table_new();

        if (fds) {
            fd_table_join(fds, p);
        }
        if (!p->fds || fd_init_stdio(p->fds, tty_console_vhangups()) != 0) {
            goto fail_parts;
        }
    }
    if (pid_reserve() != 0 || proc_set_ruid(p, ruid) != 0) {
        goto fail_parts;
    }
    p->prev = proc_tail;
    p->next = NULL;
    if (proc_tail) {
        proc_tail->next = p;
    } else {
        proc_list = p;
    }
    proc_tail = p;

    pid_insert(p);
    pid_records++;
    process_count_fork();
    return p;

fail_parts:
    if (p->fds) {
        for (fd = FD_STDIN; fd <= FD_STDERR; fd++) {
            fd_free(p->fds, fd);
        }
        fd_table_delete(p->fds);
    }
    if (--p->fs->refs == 0) {
        kfree(p->fs);
    }
    tgroup_put(p->tg);
    fpu_area_free(p->fpu);
    kfree(p);
    return NULL;
}

/** Allocate a standalone record with its own group, directories and stdio. */
process_t *alloc_process(void)
{
    return process_alloc(NULL, 0, 0);
}

/** Allocate an unfinished child, sharing the parts its flags select. */
process_t *alloc_process_fork(const process_t *parent, int thread, int share_fs)
{
    return process_alloc(parent, thread, share_fs);
}

void free_process(process_t *proc)
{
    uint32_t flags;

    if (!proc) {
        return;
    }
    bkl_assert_held();
    proc->state = PROC_UNUSED;
    proc_timer_update(proc);
    flags = spin_lock_irqsave(&tick_post_lock);
    (void)tick_post_unlink(proc);
    spin_unlock_irqrestore(&tick_post_lock, flags);
    pid_remove(proc);
    if (proc->pid == proc->tgid) {
        wq_wake_all(&proc->tg->exited);
    }
    pid_records--;
    user_put(proc->user_count);
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
    if (proc->fds && fd_table_put(proc)) {
        fd_table_delete(proc->fds);
    }
    kfree(proc->fd_held);
    proc->fd_held = NULL;
    proc->fd_held_words = 0;
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

/* The sleepers one hold of the run queues' lock wakes, at most, before it
 * lets the lock go and walks the list again. */
#define WAKE_BATCH 16

/* The reasons a pass wakes a timed sleeper for (`timer_pass`): its tick, a
 * setting of the real-time clock (`rt_steps`), or its time of the monotonic
 * clock (`wakeup_ns`). */
#define PASS_TICK 1
#define PASS_TIME 2

/**
 * Whether the pass for the reasons `why`, at the tick `now`, the real-time
 * clock's step count `steps` and the monotonic clock's reading `clock`, is
 * to wake `p`. The caller holds the run queues' lock, under which the state
 * and the deadline are tested, so that the test holds until the wake.
 */
static int timer_due(const process_t *p, int why, uint64_t now, uint32_t steps, int64_t clock)
{
    if (p->state != PROC_BLOCKED || p->wakeup_tick == 0) {
        return 0;
    }
    if ((why & PASS_TICK) &&
        (now >= p->wakeup_tick || (p->rt_steps != 0 && p->rt_steps != steps))) {
        return 1;
    }
    return (why & PASS_TIME) && p->wakeup_ns != 0 && clock >= p->wakeup_ns;
}

/**
 * Wake `p`, which `timer_due` says is due: as `sched_wake_due_locked` at its
 * tick, recording the expiry (`waketime_expire`); at its time of the
 * monotonic clock, recording the lateness read at the wake, and not taken
 * from the pass's one reading, which it shares with the sleepers before
 * `p` (`waketime_expire_late`); and at a setting of the real-time clock,
 * with no record, the wait then counting its tick again from the clock as
 * set. Returns the processor to interrupt, or -1. The caller holds the run
 * queues' lock.
 */
static int timer_wake(process_t *p, uint64_t now)
{
    if (now >= p->wakeup_tick) {
        return sched_wake_due_locked(p, now);
    }
    if (p->wakeup_ns != 0) {
        int64_t late = rtc_monotonic_ns() - p->wakeup_ns;

        if (late >= 0) {
            waketime_expire_late(p, late);
        }
    }
    return sched_wake_time_locked(p);
}

/**
 * A pass over the timer list for the reasons `why`, at the tick `now`. It
 * runs without the big lock, in three steps under the run queues' lock, as
 * Linux's timer base expires its timers under its own lock and wakes each
 * sleeper through `try_to_wake_up` (kernel/time/timer.c:1772-1789,
 * 2389-2410): the records due are collected under the timer list's lock,
 * which is let go, and then each is woken (`timer_wake`), its state and
 * deadline tested under the run queues' lock, so a wait queue's waker and
 * the pass do not both make one ready, and marked woken, as a wait queue's
 * wake marks it, so that the current thread's slice gives way to it
 * (`sched_tick`). The run queues' lock is held across the three, so that no
 * record collected is freed before its wake: `free_process` takes that lock
 * (`sched_forget`) before it frees. A sleeper with a time of the monotonic
 * clock still to come has the interrupt between ticks armed for it again
 * (`timer_wake_at`), however far, since a processor keeps only the earliest
 * time asked of it. At the tick, a real-time timer that is due is marked
 * for a holder of the big lock (`proc_tick_signal`), since a signal is
 * posted under it, and stays due until then; `alarm_due` keeps the next
 * passes from marking it again. The interrupts the wakes ask for go out
 * after the lock. The clocks are read once a pass; the clock decides a
 * time, not the tick count, which runs behind it by up to half a tick.
 */
static void timer_pass(int why, uint64_t now)
{
    uint32_t steps = rtc_step_count();
    int64_t clock = rtc_monotonic_ns();
    process_t *due[WAKE_BATCH];
    process_t *p;
    int n, i;

    do {
        uint32_t flags = sched_lock_take();
        uint64_t kicks = 0;

        n = 0;
        spin_lock(&timer_list_lock);
        work_count[3]++;
        for (p = timers; p; p = p->timer_next) {
            work_count[2]++;
            if (timer_due(p, why, now, steps, clock)) {
                if (n == WAKE_BATCH) {
                    break;
                }
                due[n++] = p;
            } else if (p->state == PROC_BLOCKED && p->wakeup_tick != 0 && p->wakeup_ns != 0) {
                timer_wake_at(p->wakeup_ns);
            }
            if ((why & PASS_TICK) && p->alarm_tick != 0 && now >= p->alarm_tick && !p->alarm_due) {
                p->alarm_due = 1;
                proc_tick_signal(p, SIGALRM);
            }
        }
        spin_unlock(&timer_list_lock);
        for (i = 0; i < n; i++) {
            int kick = timer_wake(due[i], now);

            if (kick >= 0) {
                kicks |= (uint64_t)1 << kick;
            }
        }
        sched_lock_give(flags);
        sched_kick_cpus(kicks);
    } while (n == WAKE_BATCH);
}

/**
 * The timed sleepers are nanosleep, poll with a deadline and a wait-queue
 * wait with one, such as the kernel's ping. `signal_post` makes a blocked
 * process ready too: a nanosleep it wakes then ends with -EINTR if it runs
 * again before its deadline tick and the process acts on SIGALRM, and
 * returns 0 if it first runs at that tick or later; a wait that goes on
 * arms its deadline again. The tick count is 64 bits wide and does not wrap,
 * so a deadline is compared with it as it stands. A sleep to an absolute
 * time of real time is woken at the first tick after a setting of the clock
 * (`rt_steps`), and counts its tick again from the clock as set. A sleep
 * with a time of the monotonic clock (`wakeup_ns`) is woken once the clock
 * reaches it, and has the interrupt between ticks armed for it again. The
 * pass is `timer_pass`'s.
 */
void proc_wake_sleepers(uint64_t now)
{
    timer_pass(PASS_TICK | PASS_TIME, now);
}

/**
 * A sleeper woken here is made ready as the tick makes one, and finds its
 * time come when it runs (`sleep_until`, syscall.c). One whose time is
 * still ahead, since the interrupt came a few counts early or was armed for
 * another's time, has its time armed again. The pass is `timer_pass`'s, for
 * the times alone, and reads the clock itself.
 */
void proc_wake_due(void)
{
    timer_pass(PASS_TIME, tick_count());
}

uint64_t proc_alarm_at(uint64_t now, uint64_t ticks)
{
    uint64_t at = now + ticks;

    return at != 0 ? at : 1u;
}

struct tgroup *tgroup_new(struct process *leader)
{
    struct tgroup *tg = kzalloc(sizeof(*tg));

    if (tg) {
        tg->refs = 1;
        tg->live = 1;
        tg->leader = leader;
        spin_lock_init(&tg->cpu_lock, "tg->cpu_lock");
        wait_queue_init(&tg->chld);
        wait_queue_init(&tg->gone);
        wait_queue_init(&tg->sigfd);
        wait_queue_init(&tg->exited);
    }
    return tg;
}

void tgroup_put(struct tgroup *tg)
{
    if (tg && --tg->refs == 0) {
        kfree(tg);
    }
}

void proc_cpu_time(const process_t *p, uint64_t *user, uint64_t *sys)
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

void proc_faults(const process_t *p, unsigned long *minor, unsigned long *major)
{
    const struct tgroup *tg = p->tg;
    const process_t *t;

    *minor = tg->min_flt;
    *major = tg->maj_flt;
    for (t = proc_list; t; t = t->next) {
        if (t->tg == tg) {
            *minor += t->min_flt;
            *major += t->maj_flt;
        }
    }
}

void proc_switches(const process_t *p, unsigned long *voluntary, unsigned long *involuntary)
{
    const struct tgroup *tg = p->tg;
    const process_t *t;

    *voluntary = tg->nvcsw;
    *involuntary = tg->nivcsw;
    for (t = proc_list; t; t = t->next) {
        if (t->tg == tg) {
            *voluntary += t->nvcsw;
            *involuntary += t->nivcsw;
        }
    }
}

void proc_thread_fold(const process_t *t)
{
    struct tgroup *tg = t->tg;

    tg->utime += t->utime;
    tg->stime += t->stime;
    tg->min_flt += t->min_flt;
    tg->maj_flt += t->maj_flt;
    tg->nvcsw += t->nvcsw;
    tg->nivcsw += t->nivcsw;
}

process_t *get_process(uint32_t pid)
{
    process_t *p;

    bkl_assert_held();
    if (!pid_capacity) {
        return NULL;
    }
    for (p = pid_table[pid & (pid_capacity - 1)]; p; p = p->pid_next) {
        work_count[0]++;
        if (p->pid == pid) {
            return p;
        }
    }
    return NULL;
}

int proc_is_kthread(const process_t *p)
{
    return p->state != PROC_UNUSED && p->state != PROC_ZOMBIE && p->space_root == 0 &&
           !p->in_syscall;
}

isr_params_t *proc_user_frame(const process_t *t)
{
    return (isr_params_t *)(t->kernel_stack - sizeof(isr_params_t));
}

/** Whether `p` is an init: the system's, or the first process of a PID
 * namespace below the first, whose number there is 1. */
static int proc_is_init(const process_t *p)
{
    const struct pidns *ns = proc_pidns(p);

    return p->pid == init_pid || (ns != &init_pidns && p->nspid[ns->level] == 1);
}

/**
 * The nearest ancestor of the ending process `dead_pid`, going up by
 * parent, that is a child subreaper and has a thread that has not ended;
 * NULL for none. The walk stops at the first init it meets, the ending
 * process included, so that it does not leave the ending process's PID
 * namespace, as Linux's stops at that namespace's init.
 */
static process_t *proc_subreaper(uint32_t dead_pid)
{
    process_t *a = get_process(dead_pid);

    while (a != NULL && a->parent_pid != 0 && !proc_is_init(a)) {
        a = get_process(a->parent_pid);
        if (a != NULL && !proc_is_init(a) && a->tg->child_subreaper && a->tg->live != 0) {
            return a;
        }
    }
    return NULL;
}

/**
 * A child left with a dead parent's pid would never be matched in `do_wait`,
 * and its frames, page directory and record would leak. Each child goes to
 * the nearest child subreaper above the dead parent (`proc_subreaper`), and
 * with none to the init of the dead parent's PID namespace. A child whose
 * threads have all ended has no exit to come that would tell its new
 * parent, so that parent is sent SIGCHLD now and its wait woken. When the
 * system's init itself is ending, no other init is left to adopt the
 * children. The parent-death signals are posted after the adoption, so that
 * a handler that asks for its parent finds the new one, as on Linux.
 */
void reparent_children(uint32_t dead_pid)
{
    process_t *dead = get_process(dead_pid);
    process_t *namespace_init = NULL;
    process_t *subreaper = proc_subreaper(dead_pid);
    process_t *p;
    uint32_t reaper = dead != NULL ? ns_reaper(dead) : 0;

    if (init_pid != 0 && dead_pid != init_pid) {
        namespace_init = get_process(init_pid);
    }
    if (reaper != 0 && reaper != dead_pid) {
        namespace_init = get_process(reaper);
    }
    for (p = proc_list; p; p = p->next) {
        process_t *init = namespace_init;

        if (p->parent_pid != dead_pid) {
            continue;
        }
        if (subreaper != NULL) {
            init = subreaper;
        }
        if (init != NULL) {
            p->parent_pid = init->pid;
            if (p->pid == p->tgid) {
                p->tg->no_child_signal = 0;
            }
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

/** A kernel thread's first code after its first switch: the switch's tail,
 * which takes the big lock at the thread's depth, then its entry. */
static void kthread_start(void)
{
    schedule_tail();
    current_proc->kthread_entry();
}

/**
 * The architecture lays the stack out as its `switch_context` finds it
 * (`arch_context_init`), returning first to `kthread_start`, which runs the
 * switch's tail and then `entry_point`. The thread begins holding the big
 * lock once, as a forked child does: the tail takes it at that depth.
 *
 * The thread defers the tick's switch to its sleeps and service points
 * (`no_tick_switch`), as a Linux kernel thread does without
 * CONFIG_PREEMPTION: an interrupt in the middle of its work would otherwise
 * switch it with state that only the big lock guards half changed, and the
 * switch lets the lock go.
 */
void setup_process_stack(process_t *proc, void (*entry_point)(void))
{
    proc->no_tick_switch = 1;
    proc->bkl_depth = 1;
    proc->bkl_frames = 0;
    proc->kthread_entry = entry_point;
    arch_context_init(proc, kthread_start);
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
