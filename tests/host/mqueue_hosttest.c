/**
 * Check that a message-queue wait to a time of real time follows a setting
 * of the clock, as Linux's wq_sleep waits on an hrtimer of CLOCK_REALTIME,
 * and that a signal pending at its deadline ends it with EINTR; and that the
 * mqueue file system's setattr stores a queue's and the root's attributes
 * as Linux's simple_setattr does, and a message moves the queue's times.
 *
 * host-deps: rach2/sys/lib/kstring.c rach2/sys/lib/kctypes.c
 * host-deps: rach2/sys/tick.c tests/host/compat/kheap_host.c
 * host-cflags: -Wno-pointer-to-int-cast -Wno-int-to-pointer-cast
 * host-cflags: -DTICK_HZ=1000
 *
 * The source is included whole, to reach `mq_deadline` and `mq_sleep`. User
 * memory is a byte array, and a user address is an offset into it. The
 * model's wait returns at once when its condition holds, as the kernel's
 * does; otherwise it notes the tick, the time of the monotonic clock and
 * the step count it was armed with, may set the clock as another processor
 * would, and returns as the timer's wake at that tick would, or with
 * `fine_wakes` at that time, with nothing on the queue changed. A setting of
 * the clock wakes the sleep at the next tick, which the model takes as now;
 * a condition that does not see the setting sends the sleep on to the tick
 * it was armed for. Setting the clock moves its time and its step count
 * (`rtc_step_count`); otherwise real time goes on with the monotonic clock,
 * which is the tick count in TICK_NS steps and a place within the tick. A
 * wait
 * called a hundred times fails its row and ends with EINTR, so that a sleep
 * that never ends cannot hang the test.
 */
#include "ipc/mqueue.c"
#include "fs_caps_first.h"

extern int printf(const char *format, ...);

static process_t self;
struct percpu cpu_table[NR_CPUS] = {{.current = &self}};
struct ipcns init_ipcns;
volatile uint64_t kernel_ticks;
static uint8_t user[512];
static int failures, waits;
static int calls; /* the calls of the wait since the row began */
/* The tick, the time and the step count the last wait was armed with. */
static uint64_t armed;
static int64_t armed_ns;
static uint32_t armed_steps;
/* The real-time clock's time in seconds at tick 2000 and its step count,
 * the seconds a setting moves it by in the next wait, 0 for none, and how
 * far real time falls behind the tick, for the lag row. */
static int64_t rt_sec = 100;
static uint32_t rt_steps = 1u;
static int64_t step_in_wait;
static int64_t rt_lag_ns;
/* The monotonic clock's place within the tick, and whether the interrupt
 * between ticks ends a wait at its time (`wakeup_ns`), not at its tick. */
static uint32_t mono_phase_ns;
static int fine_wakes;
/* With `step_after_fire`, the next wait reaches its time, where the timer's
 * pass records the expiry (`wakeup_fired`), and the clock is then set by
 * that many seconds before the thread runs; with `fire_late` too, the pass
 * tests the time before the setting and holds its record (`late_fired`)
 * until the thread ends its timed wait, as a pass on another processor that
 * holds the run queues' lock between its test and its record does; with
 * `signal_in_wait`, a caught signal is posted at that time. Whether a
 * signal the process acts on is pending, and the big lock's depth and
 * takes. */
static int64_t step_after_fire, late_fired;
static int fire_late;
static int signal_in_wait;
static int sig_pending;
static int bkl_depth, bkl_takes;

/** Count and print a failure when `c` is false. */
#define CHECK(c, detail)                               \
    do {                                               \
        if (!(c)) {                                    \
            printf("mqueue-test: fail: %s\n", detail); \
            failures++;                                \
        }                                              \
    } while (0)

/** The tick count as the kernel reads it whole: the model's `kernel_ticks`. */
uint64_t tick_count(void)
{
    return kernel_ticks;
}

/** The real-time clock's step count (`rtc_step_count`). */
uint32_t rtc_step_count(void)
{
    return rt_steps;
}

/** The monotonic clock in nanoseconds: the tick count in TICK_NS steps and
 * the place within the tick, `rt_lag_ns` behind once the tick has passed
 * 2000, as a clock read between ticks can be behind the tick count. */
static int64_t model_mono_ns(void)
{
    int64_t ns = (int64_t)kernel_ticks * TICK_NS + mono_phase_ns;

    if (ns > 2000LL * TICK_NS) {
        ns -= rt_lag_ns;
    }
    return ns;
}

/** `rtc_monotonic` as nanoseconds since boot: the model's clock. */
int64_t rtc_monotonic_ns(void)
{
    return model_mono_ns();
}

/** The real time: `rt_sec` at tick 2000, where each row starts, and on
 * with the monotonic clock from there. */
void get_current_time(int64_t *seconds, int64_t *nanoseconds)
{
    int64_t ns = model_mono_ns() - 2000LL * TICK_NS;

    *seconds = rt_sec + ns / 1000000000;
    *nanoseconds = ns % 1000000000;
    if (*nanoseconds < 0) {
        *nanoseconds += 1000000000;
        (*seconds)--;
    }
}

/** The monotonic time at which the model's real time reaches `sec` and
 * `nsec`: the monotonic clock now, on by the time from real time now. */
void rtc_mono_of_real(int64_t sec, int32_t nsec, int64_t *mono_sec, int32_t *mono_nsec)
{
    int64_t now, now_ns, ns;

    get_current_time(&now, &now_ns);
    ns = model_mono_ns() + (sec - now) * 1000000000 + (nsec - now_ns);
    *mono_sec = ns / 1000000000;
    *mono_nsec = (int32_t)(ns % 1000000000);
    if (*mono_nsec < 0) {
        *mono_nsec += 1000000000;
        (*mono_sec)--;
    }
}

/** Move the clocks on until the monotonic clock reads `ns`. */
static void model_reach(int64_t ns)
{
    ns += rt_lag_ns;
    kernel_ticks = (uint64_t)(ns / TICK_NS);
    mono_phase_ns = (uint32_t)(ns % TICK_NS);
}

/** A setting of the real-time clock `sec` seconds on from where it is, as
 * `rtc_set_realtime` makes on another processor. */
static void model_step(int64_t sec)
{
    rt_sec += sec;
    rt_steps++;
}

/** Whether [addr, addr + len) lies in the model's user memory. */
static int in_user(uintptr_t addr, size_t len)
{
    return addr != 0 && addr <= sizeof(user) && len <= sizeof(user) - addr;
}

/** Copy `n` bytes from user address `src`; 0, or -EFAULT outside user
 * memory. */
int copy_from_user(void *dst, const void __user *src, size_t n)
{
    uintptr_t at = (uintptr_t)src;

    if (!in_user(at, n)) {
        return -EFAULT;
    }
    k_memcpy(dst, user + at, n);
    return 0;
}

/** Copy `n` bytes to user address `dst`; 0, or -EFAULT outside user
 * memory. */
int copy_to_user(void __user *dst, const void *src, size_t n)
{
    uintptr_t at = (uintptr_t)dst;

    if (!in_user(at, n)) {
        return -EFAULT;
    }
    k_memcpy(user + at, src, n);
    return 0;
}

/** Model the process's pathname page with a heap allocation. */
char *path_buf_get(void)
{
    return kmalloc(VFS_PATH_MAX);
}

/** Return the page after lookup. */
void path_buf_put(char *name)
{
    kfree(name);
}

/** Copy a terminated name from the model's user memory. */
int strncpy_from_user(char *dst, const char __user *src, uint32_t n)
{
    uintptr_t at = (uintptr_t)src;
    uint32_t i;

    for (i = 0; i < n; i++) {
        if (!in_user(at + i, 1))
            return -EFAULT;
        dst[i] = user[at + i];
        if (!dst[i])
            return (int)i;
    }
    return -ENAMETOOLONG;
}

/** The model's calls are native, not i386 calls on x86-64. */
int in_compat_syscall(void)
{
    return 0;
}

/** Set the thread's timed wait as the kernel does, under no lock, since the
 * model has one processor and no timer pass runs beside the thread, and
 * return the expiry record. A record a pass still holds (`late_fired`) is
 * stored first, as the pass that holds the run queues' lock stores it
 * before the change can take that lock. */
int64_t proc_wait_set(uint64_t tick, int64_t ns, uint32_t steps, int fresh)
{
    if (late_fired != 0) {
        current_proc->wakeup_fired = late_fired;
        late_fired = 0;
    }
    current_proc->wakeup_tick = tick;
    current_proc->wakeup_ns = ns;
    current_proc->rt_steps = steps;
    if (fresh) {
        current_proc->wakeup_fired = 0;
    }
    return current_proc->wakeup_fired;
}

/**
 * The wait: at once when `cond` holds, else a sleep that the tick armed for
 * ends, or, when a row asks for it with `step_in_wait`, a setting of the
 * clock whose wake `cond` must see, or with `step_after_fire`, one after
 * the timer's wake at its time. With `signal_in_wait`, a caught signal is
 * posted as the sleep ends, after the condition was last tested.
 */
int wq_wait_event(wait_queue_t *wq, wq_cond_fn cond, void *arg)
{
    (void)wq;
    if (++calls == 100) {
        CHECK(0, "a wait ends within a hundred calls");
        return -EINTR;
    }
    if (cond(arg)) {
        return 0;
    }
    waits++;
    armed = current_proc->wakeup_tick;
    armed_ns = current_proc->wakeup_ns;
    armed_steps = current_proc->rt_steps;
    if (step_after_fire != 0 && armed_ns != 0) {
        model_reach(armed_ns);
        if (fire_late) {
            late_fired = armed_ns;
        } else {
            current_proc->wakeup_fired = armed_ns;
        }
        model_step(step_after_fire);
        step_after_fire = 0;
    } else if (step_in_wait != 0) {
        model_step(step_in_wait);
        step_in_wait = 0;
        if (!cond(arg)) {
            kernel_ticks = armed;
        }
    } else if (fine_wakes && armed_ns != 0) {
        model_reach(armed_ns);
    } else {
        kernel_ticks = armed;
    }
    if (signal_in_wait) {
        signal_in_wait = 0;
        sig_pending = 1;
        self.sig_woken = 1;
    }
    return 0;
}

/** Whether a signal the process acts on is pending: the rows' flag. */
int signal_pending(void)
{
    return sig_pending;
}

/** Whether this processor holds the big lock. */
int bkl_held(void)
{
    return bkl_depth > 0;
}

/** Take the big lock: count the depth and the take. */
void bkl_enter(void)
{
    bkl_depth++;
    bkl_takes++;
}

/** Give the big lock back. */
void bkl_exit(void)
{
    bkl_depth--;
}

/** No row wakes a queue. */
void wq_wake_all(wait_queue_t *wq)
{
    (void)wq;
}

/** A queue's waiters need nothing in the model. */
void wait_queue_init(wait_queue_t *wq)
{
    (void)wq;
}

static fd_entry_t slots[4];
static struct open_file descriptions[4];
static struct vfs_mount private_mount;

/** Reserve one slot and its description, as fd_alloc does. */
int fd_alloc(fd_table_t *table)
{
    int i;

    (void)table;
    for (i = 0; i < 4; i++) {
        if (!slots[i].ofd) {
            slots[i].ofd = &descriptions[i];
            return i;
        }
    }
    return -EMFILE;
}

/** Find a reserved or installed slot. */
fd_entry_t *fd_slot(const fd_table_t *table, int fd)
{
    (void)table;
    return fd >= 0 && fd < 4 ? &slots[fd] : NULL;
}

/** Find an installed slot. */
fd_entry_t *fd_get(fd_table_t *table, int fd)
{
    fd_entry_t *e = fd_slot(table, fd);

    return e && e->type ? e : NULL;
}

/** The model has one writer and publishes each slot immediately. */
void fd_install(fd_table_t *table, int fd)
{
    (void)table;
    (void)fd;
}

/** Release the open handle, inode and description of one descriptor. */
int fd_free(fd_table_t *table, int fd)
{
    fd_entry_t *e = fd_slot(table, fd);
    struct open_file *ofd = e->ofd;

    if (e->ops == &mq_ops)
        mq_flush(e);
    if (ofd->vmnt) {
        mqfs_release(ofd->vmnt, ofd->fhandle);
        mqfs_put(ofd->vmnt, ofd->vnode);
        vfs_mount_put(ofd->vmnt);
    }
    k_memset(ofd, 0, sizeof(*ofd));
    k_memset(e, 0, sizeof(*e));
    return 0;
}

/** A queue in this model has no conflicting text mapping. */
int fd_ofd_access(struct open_file *ofd, const struct vfs_ref *ref, int access)
{
    (void)ofd;
    (void)ref;
    (void)access;
    return 0;
}

/** Construct the one namespace's private mount. */
struct vfs_mount *vfs_kernel_mount(const struct vfs_type *type, void *root, uint32_t flags)
{
    private_mount.root = root;
    private_mount.type = type;
    private_mount.flags = flags;
    private_mount.refs = 1;
    return &private_mount;
}

/** Take the mount hold of a visible mount. */
void vfs_mount_get(struct vfs_mount *m)
{
    m->refs++;
}

/** Take the mount hold of a descriptor. */
void vfs_desc_open(struct vfs_mount *m)
{
    vfs_mount_get(m);
}

/** Drop one mount hold. */
uint32_t vfs_mount_put(struct vfs_mount *m)
{
    return --m->refs;
}

/** Release the namespace root after the last hold. */
void vfs_reap_detached(void)
{
    mqfs_unmount(&private_mount);
}

/** Open the actual queue handle. */
int vfs_open_node(struct vfs_mount *m, void *node, int flags, void **handle)
{
    return mqfs_open(m, node, flags, handle);
}

/** The private mount carries the namespace's superblock flags. */
int vfs_sb_rdonly(const struct vfs_mount *m)
{
    return (m->flags & VFS_MS_RDONLY) != 0;
}

/** Read the model's mount flags. */
int vfs_mount_rdonly(const struct vfs_mount *m)
{
    return (m->flags & VFS_MS_RDONLY) != 0;
}

/** Namespace operations run on one thread. */
void vfs_ns_lock(struct vfs_mount *m)
{
    (void)m;
}

/** End a namespace operation. */
void vfs_ns_unlock(struct vfs_mount *m)
{
    (void)m;
}

/** No row has a descriptor whose flags mq_getsetattr sets. */
void fd_ofd_status_set(struct open_file *ofd, uint32_t clear, uint32_t set)
{
    (void)ofd;
    (void)clear;
    (void)set;
}

/** No row notifies a process. */
process_t *get_process(uint32_t pid)
{
    (void)pid;
    return NULL;
}

/** No row posts a signal. */
void signal_post_info(process_t *target, int signo, const struct k_siginfo *info)
{
    (void)target;
    (void)signo;
    (void)info;
}

/** The model's one IPC namespace. */
struct ipcns *proc_ipcns(const process_t *p)
{
    (void)p;
    return &init_ipcns;
}

/** No user namespace is consulted by the rows. */
struct userns *proc_userns(const process_t *p)
{
    (void)p;
    return NULL;
}

/** PIDs are the kernel's. */
uint32_t pid_seen_by(const process_t *viewer, uint32_t kid)
{
    (void)viewer;
    return kid;
}

/** No file system is mounted. */
int vfs_mount_slots(void)
{
    return 0;
}

/** No file system is mounted. */
struct vfs_mount *vfs_mount_at(int idx)
{
    (void)idx;
    return NULL;
}

/**
 * Count `w`'s deadline, `at` s of real time, from tick 2000, as mq_timedsend
 * and mq_timedreceive do, from the 64-bit timespec at user address 16.
 */
static void deadline(struct mq_wait *w, int64_t at)
{
    int64_t ts[2];

    ts[0] = at;
    ts[1] = 0;
    k_memcpy(user + 16, ts, sizeof(ts));
    kernel_ticks = 2000;
    mono_phase_ns = 0;
    waits = 0;
    calls = 0;
    armed = 0;
    armed_ns = 0;
    armed_steps = 0;
    k_memset(w, 0, sizeof(*w));
    CHECK(mq_deadline((const void __user *)16, 1, w) == 0 && w->timed, "the deadline is read");
}

/**
 * From real time 100 s at tick 2000, a wait until 101 s. A step back of a
 * second during the wait counts the tick again and sleeps on to the tick two
 * seconds on; a step forward past the time ends it with ETIMEDOUT without a
 * second sleep. The wait keeps the step count it counted its tick at, for the
 * tick's wake. A step between the reading of the deadline and the sleep is
 * counted before the sleep: back, the wait sleeps to the time as set; forward
 * past it, it ends without a sleep. Each is Linux's hrtimer of CLOCK_REALTIME,
 * which clock_was_set moves (ipc/mqueue.c:728-729,
 * kernel/time/hrtimer.c:951-987). A wait whose tick comes while the clock read
 * then is still short of the time counts again and sleeps on, since Linux's
 * hrtimer never expires before its clock reaches its time; where the interrupt
 * between ticks comes, it ends the wait in one sleep, when the monotonic clock
 * reaches the time real time reaches it at, a second after the call; and a
 * step back of the clock after that wake, before the thread runs, leaves it
 * ended, as Linux's hrtimer_wakeup clears the sleeper's task and
 * schedule_hrtimeout_range_clock returns 0 to wq_sleep.
 */
static void realtime_steps(void)
{
    static struct mqueue q;
    struct mq_wait w;

    w.q = &q;
    rt_sec = 100;
    deadline(&w, 101);
    w.q = &q;
    step_in_wait = -1;
    CHECK(mq_sleep(&q, &w) == -ETIMEDOUT && waits == 2 && armed == 2000u + 2u * TICK_HZ &&
              armed_steps == rt_steps,
          "a step back during a wait on real time sleeps on to the time");

    rt_sec = 100;
    deadline(&w, 101);
    w.q = &q;
    step_in_wait = 2;
    CHECK(mq_sleep(&q, &w) == -ETIMEDOUT && waits == 1 && armed == 2000u + TICK_HZ &&
              armed_steps == rt_steps - 1u,
          "a step forward past the time ends a wait on real time");

    rt_sec = 100;
    deadline(&w, 101);
    w.q = &q;
    model_step(-1);
    CHECK(mq_sleep(&q, &w) == -ETIMEDOUT && waits == 1 && armed == 2000u + 2u * TICK_HZ,
          "a step back before the sleep is counted before it");

    rt_sec = 100;
    deadline(&w, 101);
    w.q = &q;
    model_step(2);
    CHECK(mq_sleep(&q, &w) == -ETIMEDOUT && waits == 0,
          "a step forward past the time before the sleep ends it without one");
    CHECK(self.wakeup_tick == 0 && self.wakeup_ns == 0 && self.rt_steps == 0,
          "the wait leaves no deadline behind");

    rt_sec = 100;
    rt_lag_ns = 3 * TICK_NS / 2;
    deadline(&w, 101);
    w.q = &q;
    CHECK(mq_sleep(&q, &w) == -ETIMEDOUT && waits == 2 && armed == 2002u + TICK_HZ,
          "a wait whose tick comes before the clock reaches its time sleeps on to it");

    rt_sec = 100;
    fine_wakes = 1;
    deadline(&w, 101);
    w.q = &q;
    CHECK(mq_sleep(&q, &w) == -ETIMEDOUT && waits == 1 &&
              armed_ns == 2000LL * TICK_NS + 1000000000 &&
              model_mono_ns() == 2000LL * TICK_NS + 1000000000,
          "the interrupt between ticks ends it once the clock reaches its time");
    fine_wakes = 0;
    rt_lag_ns = 0;
    mono_phase_ns = 0;

    rt_sec = 100;
    deadline(&w, 101);
    w.q = &q;
    step_after_fire = -1;
    CHECK(mq_sleep(&q, &w) == -ETIMEDOUT && waits == 1 &&
              model_mono_ns() == 2000LL * TICK_NS + 1000000000,
          "a step back after the timer ended the wait at its time leaves it ended");
    rt_sec = 100;
    deadline(&w, 101);
    w.q = &q;
    step_after_fire = -1;
    fire_late = 1;
    CHECK(mq_sleep(&q, &w) == -ETIMEDOUT && waits == 1 && late_fired == 0 &&
              model_mono_ns() == 2000LL * TICK_NS + 1000000000,
          "a step back between the pass's test of the time and its record leaves the "
          "wait ended");
    fire_late = 0;
    step_after_fire = 0;
    mono_phase_ns = 0;
}

/**
 * A caught signal pending when the deadline has come ends the wait with
 * EINTR, not ETIMEDOUT, as Linux's wq_sleep tests signal_pending before the
 * timeout (ipc/mqueue.c): one posted as the timer ends the sleep at its
 * time, and one pending when a wait whose time has passed begins. The
 * signal is judged under the big lock, which the wait takes and gives
 * back. A signal not pending leaves the timeout ETIMEDOUT.
 */
static void signal_rows(void)
{
    static struct mqueue q;
    struct mq_wait w;

    rt_sec = 100;
    fine_wakes = 1;
    deadline(&w, 101);
    w.q = &q;
    signal_in_wait = 1;
    bkl_takes = 0;
    CHECK(mq_sleep(&q, &w) == -EINTR && waits == 1 && bkl_takes == 1 && bkl_depth == 0,
          "a signal posted as the timer ends the wait is EINTR, judged under the big lock");
    fine_wakes = 0;
    mono_phase_ns = 0;

    rt_sec = 100;
    deadline(&w, 99);
    w.q = &q;
    sig_pending = 1;
    self.sig_woken = 1;
    CHECK(mq_sleep(&q, &w) == -EINTR && waits == 0,
          "a signal pending when a wait past its time begins is EINTR");
    sig_pending = 0;
    self.sig_woken = 0;
    rt_sec = 100;
    deadline(&w, 99);
    w.q = &q;
    CHECK(mq_sleep(&q, &w) == -ETIMEDOUT && waits == 0,
          "with no signal pending the wait past its time is ETIMEDOUT");
}

/**
 * The root's attributes are its namespace's, made at the first use for the
 * first namespace: 01777 and the caller's ids. A ctime-only setattr moves
 * the change time alone, on the root and on a queue; a full one stores the
 * mode, the ids and the times; a size or inode flags are refused with
 * nothing changed; and a message sent moves all three of the queue's times.
 */
static void attr_rows(void)
{
    static struct mqueue q;
    static struct mq_msg msg;
    struct vfs_mount *m;
    struct vfs_stat st, in;
    int64_t t0, t0_ns, t1, t1_ns;

    k_memset(&in, 0, sizeof(in));
    rt_sec = 500;
    CHECK(mq_root_made(&init_ipcns) == 0, "create the IPC namespace root");
    m = init_ipcns.mq;
    get_current_time(&t0, &t0_ns);
    CHECK(mqfs_getattr(m, m->root, &st) == 0 && st.mode == (MQFS_S_IFDIR | 01777u) &&
              st.uid == proc_fsuid(&self) && st.ctime_sec == t0 && st.mtime_sec == t0,
          "the first namespace's root is made 01777, the caller's, at its first use");
    rt_sec = 600;
    get_current_time(&t1, &t1_ns);
    CHECK(mqfs_setattr(m, m->root, VFS_SET_CTIME, &in) == 0, "a ctime-only setattr of the root");
    mqfs_getattr(m, m->root, &st);
    CHECK(st.ctime_sec == t1 && st.mtime_sec == t0 && st.mode == (MQFS_S_IFDIR | 01777u),
          "a ctime-only setattr moves the root's change time alone");
    q.mode = 0600u;
    mq_touch(&q);
    rt_sec = 700;
    get_current_time(&t1, &t1_ns);
    in.mode = 0100640u;
    in.uid = 7;
    in.gid = 8;
    in.atime_sec = 21;
    in.mtime_sec = 22;
    CHECK(mqfs_setattr(m, &q,
                       VFS_SET_MODE | VFS_SET_UID | VFS_SET_GID | VFS_SET_ATIME | VFS_SET_MTIME,
                       &in) == 0,
          "a full setattr of a queue");
    mqfs_getattr(m, &q, &st);
    CHECK(st.mode == (MQFS_S_IFREG | 0640u) && st.uid == 7 && st.gid == 8 && st.atime_sec == 21 &&
              st.mtime_sec == 22 && st.ctime_sec == t1,
          "a queue's mode, ids and times stored, its change time now");
    CHECK(mqfs_setattr(m, &q, VFS_SET_SIZE, &in) == -EINVAL &&
              mqfs_setattr(m, &q, VFS_SET_FLAGS, &in) == -EOPNOTSUPP,
          "a size and inode flags are refused");
    mqfs_getattr(m, &q, &st);
    CHECK(st.ctime_sec == t1 && st.mtime_sec == 22, "a refused setattr changes nothing");
    rt_sec = 800;
    get_current_time(&t1, &t1_ns);
    mq_enqueue(&q, &msg);
    mqfs_getattr(m, &q, &st);
    CHECK(st.atime_sec == t1 && st.mtime_sec == t1 && st.ctime_sec == t1,
          "a message sent moves the queue's three times");
    q.head = NULL;
    q.curmsgs = 0;
}

/** Descriptor exhaustion precedes validation of a single queue component. */
static void bad_names(void)
{
    struct isr_params args = {0};
    int i;

    args.ebx = 1;
    for (i = 0; i < 4; i++)
        fd_alloc(NULL);
    user[1] = '/';
    CHECK(sys_mq_open(&args) == -EMFILE, "reserve the descriptor before validating a component");
    for (i = 0; i < 4; i++)
        fd_free(NULL, i);
    CHECK(sys_mq_open(&args) == -EACCES, "a slash is not a single queue name");
    k_memset(user + 1, 'q', MQ_NAME_MAX + 1);
    user[MQ_NAME_MAX + 2] = 0;
    CHECK(sys_mq_open(&args) == -ENAMETOOLONG, "a queue component past NAME_MAX");
    k_memset(user, 0, sizeof(user));
}

/** Queue operations use the inode held by an ordinary file description. */
static void queue_operations(int fd)
{
    struct isr_params args = {0};
    struct mq_attr attr;
    fd_entry_t *e = &slots[fd];
    struct mqueue *q = e->ofd->vnode;

    args.ebx = fd;
    args.ecx = 1;
    args.edx = 1;
    CHECK(sys_mq_timedsend(&args) == 0 && (e->ops->poll(e, POLLIN, NULL) & POLLIN),
          "sending a message makes the queue readable");
    args.ecx = 0;
    args.edx = 16;
    CHECK(sys_mq_getsetattr(&args) == 0, "read the queue attributes");
    k_memcpy(&attr, user + 16, sizeof(attr));
    CHECK(attr.curmsgs == 1, "the queue contains the message");
    {
        int32_t remaining;
        uint64_t allocated;
        q->atime_s = q->ctime_s = -1;
        CHECK(e->ops->read(e, (char __user *)16, 6) == 6 &&
                  !__builtin_memcmp(user + 16, "QSIZE:", 6) && q->atime_s != -1 &&
                  q->ctime_s == q->atime_s,
              "a status read advances the offset and times");
        CHECK(e->ops->ioctl && e->ops->ioctl(e, 0x541b, 16) == 0, "FIONREAD succeeds");
        k_memcpy(&remaining, user + 16, sizeof(remaining));
        CHECK(remaining == (int)q->size - 6, "FIONREAD uses inode size and file position");
        CHECK(e->ops->ioctl && e->ops->ioctl(e, 0x5460, 16) == 0, "FIOQSIZE succeeds");
        k_memcpy(&allocated, user + 16, sizeof(allocated));
        CHECK(!allocated, "the queue inode has no allocated blocks");
        CHECK(e->ops->ioctl && e->ops->ioctl(e, 0x5452, 16) == 0, "async is already clear");
        CHECK(e->ops->ioctl && e->ops->ioctl(e, 2, 16) == 0, "FIGETBSZ succeeds");
        k_memcpy(&remaining, user + 16, sizeof(remaining));
        CHECK(remaining == PAGE_SIZE, "the queue block size is one page");
        CHECK(e->ops->ioctl && e->ops->ioctl(e, 0x5452, 16) == -ENOTTY,
              "a queue cannot enable async I/O");
        CHECK(e->ops->ioctl && e->ops->ioctl(e, 0xc020660b, 0) == -EOPNOTSUPP,
              "a queue has no extent map");
        CHECK(e->ops->ioctl && e->ops->ioctl(e, 0x1234, 16) == -ENOTTY,
              "a private ioctl has no queue handler");
    }
    q->notify_tgid = self.tgid = 1;
    mq_flush(e);
    CHECK(!q->notify_tgid, "closing a descriptor removes its owner's notification");
}

/** Exclusive creation and invalid access precede write permission. */
static void readonly_queue(struct isr_params *args, struct vfs_ref *file)
{
    struct vfs_stat st = {0};

    st.mode = 0600;
    mqfs_setattr(file->mnt, file->node, VFS_SET_MODE, &st);
    self.euid = 20;
    file->mnt->flags |= VFS_MS_RDONLY;
    args->ecx = O_CREAT | O_EXCL | 3;
    CHECK(sys_mq_open(args) == -EEXIST, "exclusive creation precedes access checks");
    args->ecx = 3;
    CHECK(sys_mq_open(args) == -EINVAL, "an existing queue refuses an invalid access mode");
    args->ecx = 1;
    CHECK(sys_mq_open(args) == -EROFS, "a read-only superblock precedes queue write permission");
    args->ecx = 0;
    CHECK(sys_mq_open(args) == -EACCES, "a read-only open checks queue read permission");
    file->mnt->flags &= ~VFS_MS_RDONLY;
    args->ecx = O_CREAT | 2;
    st.mode = 0666;
    mqfs_setattr(file->mnt, file->node, VFS_SET_MODE, &st);
}

/** Exercise name lookup and deletion through the actual queue syscalls. */
static void namespace_rows(void)
{
    static struct fs_struct fs;
    struct isr_params args = {0};
    struct vfs_mount *m = init_ipcns.mq;
    struct vfs_stat st = {0};
    struct vfs_ref file;
    int fd, other;

    self.fs = &fs;
    self.euid = 10;
    args.ebx = 1;
    args.ecx = O_CREAT | 2;
    args.edx = 0666;
    user[1] = 'q';
    fd = (int)sys_mq_open(&args);
    CHECK(fd >= 0, "make a queue");
    if (fd < 0)
        return;
    CHECK(mq_fd_ref(&slots[fd], &file) && file.mnt == m && file.node == slots[fd].ofd->vnode &&
              file.node == slots[fd].ofd->fhandle,
          "mq_open holds the inode and mount for descriptor operations");
    CHECK(slots[fd].ops == &mq_ops && !slots[fd].ops->mmap,
          "mq_open installs the operations that refuse mappings");
    queue_operations(fd);
    readonly_queue(&args, &file);
    mqfs_setattr(m, m->root, VFS_SET_MODE, &st);
    self.euid = 20;
    mqfs_getattr(m, m->root, &st);
    CHECK(st.mode == MQFS_S_IFDIR && st.uid == 0, "mode zero preserves the owner's directory");
    CHECK(sys_mq_open(&args) == -EACCES && sys_mq_unlink(&args) == -EACCES,
          "no search permission on an existing name");
    self.euid = 0;
    self.cap_lost[0] = ~(1u << CAP_DAC_READ_SEARCH);
    CHECK(sys_mq_unlink(&args) == -EACCES, "READ_SEARCH does not grant directory write access");
    self.euid = 20;
    self.cap_lost[0] = 0;
    user[1] = 'x';
    CHECK(sys_mq_open(&args) == -EACCES && sys_mq_unlink(&args) == -EACCES,
          "no search permission on a missing name");
    st.mode = 0111;
    mqfs_setattr(m, m->root, VFS_SET_MODE, &st);
    CHECK(sys_mq_open(&args) == -EACCES, "creation needs directory write permission");
    args.ecx = 0;
    CHECK(sys_mq_open(&args) == -ENOENT, "a searchable missing queue");
    user[1] = 'q';
    other = (int)sys_mq_open(&args);
    CHECK(other >= 0 && sys_mq_unlink(&args) == -EACCES,
          "existing open needs search and unlink also needs write");
    if (other >= 0)
        fd_free(NULL, other);
    st.mode = 01777;
    mqfs_setattr(m, m->root, VFS_SET_MODE, &st);
    CHECK(sys_mq_unlink(&args) == -EPERM, "sticky removal by a third user");
    st.uid = self.euid;
    mqfs_setattr(m, m->root, VFS_SET_UID, &st);
    CHECK(sys_mq_unlink(&args) == 0, "the sticky directory owner can unlink");
    CHECK(mqfs_getattr(m, file.node, &st) == 0 && st.nlink == 0 && st.size == 80,
          "unlinked descriptor retains its inode and its initial size");
    st.mode = 0640;
    mqfs_setattr(m, file.node, VFS_SET_MODE, &st);
    CHECK(mqfs_truncate(m, file.node, 7, 1) == 0 &&
              mqfs_truncate(m, file.node, 0x80000000ull, 1) == -EFBIG,
          "resize the held inode up to the superblock limit");
    mqfs_getattr(m, file.node, &st);
    CHECK(st.mode == (MQFS_S_IFREG | 0640) && st.size == 7, "held inode attributes survive unlink");
    fd_free(NULL, fd);
    st.mode = 0777;
    mqfs_setattr(m, m->root, VFS_SET_MODE, &st);
    args.ecx = O_CREAT;
    self.euid = 10;
    fd = (int)sys_mq_open(&args);
    self.euid = 30;
    CHECK(fd >= 0 && mqfs_unlink(m, m->root, "q") == 0, "a non-sticky directory allows unlink");
    if (fd >= 0)
        fd_free(NULL, fd);
    {
        void *node = NULL;
        struct mq_root *root = m->root;

        root->queues = MQ_QUEUES;
        CHECK(mqfs_create(m, root, "limit", 0600, &node) == -ENOSPC,
              "full namespace refuses another queue");
        root->queues = 0;
        CHECK(mqfs_create(m, root, "file", 0600, &node) == 0 && mqfs_unlink(m, root, "file") == 0,
              "creating through the mount holds its inode");
        if (node)
            mqfs_put(m, node);
    }
    CHECK(m->refs == 1 && !queues, "closing drops each inode and mount hold");
    mq_ns_gone(&init_ipcns);
}

/** Run the rows; 0 when every check passed, 1 otherwise. */
int main(void)
{
    realtime_steps();
    signal_rows();
    attr_rows();
    bad_names();
    namespace_rows();
    if (failures) {
        return 1;
    }
    printf("mqueue-test: pass: the steps of real time, a signal at the deadline and setattr\n");
    return 0;
}
