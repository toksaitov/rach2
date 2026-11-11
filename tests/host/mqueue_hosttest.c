/**
 * Check that a message-queue wait to a time of real time follows a setting
 * of the clock, as Linux's wq_sleep waits on an hrtimer of CLOCK_REALTIME.
 *
 * host-deps: rach2/sys/lib/kstring.c rach2/sys/lib/kctypes.c
 * host-deps: rach2/sys/tick.c tests/host/compat/kheap_host.c
 * host-cflags: -Wno-pointer-to-int-cast -Wno-int-to-pointer-cast
 *
 * The source is included whole, to reach `mq_deadline` and `mq_sleep`. User
 * memory is a byte array, and a user address is an offset into it. The
 * model's wait returns at once when its condition holds, as the kernel's
 * does; otherwise it notes the tick and the step count it was armed with,
 * may set the clock as another processor would, and returns as the timer's
 * wake at that tick would, with nothing on the queue changed. A setting of
 * the clock wakes the sleep at the next tick, which the model takes as now;
 * a condition that does not see the setting sends the sleep on to the tick
 * it was armed for. Setting the clock moves its time and its step count
 * (`rtc_step_count`); otherwise real time goes on with the tick. A wait
 * called a hundred times fails its row and ends with EINTR, so that a sleep
 * that never ends cannot hang the test.
 */
#include "ipc/mqueue.c"

extern int printf(const char *format, ...);

static process_t self;
struct percpu cpu_table[NR_CPUS] = {{.current = &self}};
struct ipcns init_ipcns;
volatile uint64_t kernel_ticks;
static uint8_t user[256];
static int failures, waits;
static int calls; /* the calls of the wait since the row began */
/* The tick and the step count the last wait was armed with. */
static uint64_t armed;
static uint32_t armed_steps;
/* The real-time clock's time in seconds at tick 2000 and its step count,
 * the seconds a setting moves it by in the next wait, 0 for none, and how
 * far real time falls behind the tick, for the slew row. */
static int64_t rt_sec = 100;
static uint32_t rt_steps = 1u;
static int64_t step_in_wait;
static int64_t rt_lag_ns;

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

/** The real time: `rt_sec` at tick 2000, where each row starts, and on
 * with the tick from there, `rt_lag_ns` behind it once the tick has moved,
 * as a clock slewed slower than the tick falls behind during a wait. */
void get_current_time(int64_t *seconds, int64_t *nanoseconds)
{
    int64_t ns = ((int64_t)kernel_ticks - 2000) * TICK_NS;

    if (ns > 0) {
        ns -= rt_lag_ns;
    }

    *seconds = rt_sec + ns / 1000000000;
    *nanoseconds = ns % 1000000000;
    if (*nanoseconds < 0) {
        *nanoseconds += 1000000000;
        (*seconds)--;
    }
}

/** The ticks in which real time goes on by `sec` and `nsec`
 * (`rtc_real_span`): the tick's own, since no row slews the clock's rate
 * as the kernel counts it. */
uint64_t rtc_real_span(int64_t sec, uint32_t nsec)
{
    return tick_span(sec, nsec);
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

/** No row names a queue: -EFAULT. */
int strncpy_from_user(char *dst, const char __user *src, uint32_t n)
{
    (void)dst;
    (void)src;
    (void)n;
    return -EFAULT;
}

/** The model's calls are native, not i386 calls on x86-64. */
int in_compat_syscall(void)
{
    return 0;
}

/**
 * The wait: at once when `cond` holds, else a sleep that the tick armed for
 * ends, or, when a row asks for it with `step_in_wait`, a setting of the
 * clock whose wake `cond` must see.
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
    armed_steps = current_proc->rt_steps;
    if (step_in_wait != 0) {
        model_step(step_in_wait);
        step_in_wait = 0;
        if (!cond(arg)) {
            kernel_ticks = armed;
        }
    } else {
        kernel_ticks = armed;
    }
    return 0;
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

/** No row opens a descriptor. */
int fd_alloc(fd_table_t *table)
{
    (void)table;
    return -EMFILE;
}

/** No row has a descriptor. */
fd_entry_t *fd_get(fd_table_t *table, int fd)
{
    (void)table;
    (void)fd;
    return NULL;
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

/** User IDs are the kernel's. */
uint32_t ns_uid_from_kernel(const struct userns *ns, uint32_t kid)
{
    (void)ns;
    return kid;
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
    waits = 0;
    calls = 0;
    armed = 0;
    armed_steps = 0;
    k_memset(w, 0, sizeof(*w));
    CHECK(mq_deadline((const void __user *)16, 1, w) == 0 && w->timed, "the deadline is read");
}

/**
 * From real time 100 s at tick 2000, a wait until 101 s. A step back of a
 * second during the wait counts the tick again and sleeps on to tick 4000,
 * two seconds on; a step forward past the time ends it with ETIMEDOUT without a
 * second sleep. The wait keeps the step count it counted its tick at, for
 * the tick's wake. A step between the reading of the deadline and the
 * sleep is counted before the sleep: back, the wait sleeps to the time as
 * set; forward past it, it ends without a sleep. Each is Linux's hrtimer of
 * CLOCK_REALTIME, which clock_was_set moves (ipc/mqueue.c:728-729,
 * kernel/time/hrtimer.c:951-987). A wait whose tick comes while a clock
 * slewed slower than the tick is still short of the time counts again and
 * sleeps on, since Linux's hrtimer never expires before its clock reaches
 * its time.
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
    CHECK(self.wakeup_tick == 0 && self.rt_steps == 0, "the wait leaves no deadline behind");

    rt_sec = 100;
    rt_lag_ns = 1500000;
    deadline(&w, 101);
    w.q = &q;
    CHECK(mq_sleep(&q, &w) == -ETIMEDOUT && waits == 2 && armed == 3002u,
          "a wait whose tick comes before a slewed clock reaches its time sleeps on to it");
    rt_lag_ns = 0;
}

/** Run the rows; 0 when every check passed, 1 otherwise. */
int main(void)
{
    realtime_steps();
    if (failures) {
        return 1;
    }
    printf("mqueue-test: pass: the steps of real time\n");
    return 0;
}
