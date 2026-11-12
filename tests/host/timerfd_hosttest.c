/**
 * Check a timerfd's read against the timer tick, and its settings.
 *
 * host-deps: tests/host/compat/spinlock_host.c
 * host-cflags: -Wno-pointer-to-int-cast -Wno-int-to-pointer-cast
 * host-deps: rach2/sys/tick.c
 *
 * The timerfd code is the kernel's, included whole to reach its static read
 * and arm. The test owns the tick count and calls `timerfd_tick` as the
 * timer interrupt does; the interrupt mask is a counter.
 *
 * A read takes the count before its copy to the caller: an expiration that
 * comes while the copy sleeps is kept for the next read, a second read in
 * that time finds nothing, and a copy that fails loses what it took, as in
 * Linux.
 *
 * A timer armed while a tick is pending counts from that tick. A timer of
 * 200 days, past 2^32 ticks, reports 200 days, does not expire a tick early
 * and expires at its tick; an interval of 200 days is reported and waited
 * whole; and a time past the most seconds a setting keeps is taken as that
 * most.
 *
 * A POSIX timer's expiration posts no signal from the tick, which runs
 * without the big lock: it is queued once, the expirations meanwhile going
 * into its count, and a holder's `ptimer_post_due` posts one signal with
 * the rest as its overrun. A timer_settime or a delete ends the setting as
 * Linux does: a first expiration not yet signalled is posted, or adds 1 to
 * the timer's pending signal, and the overrun the setting's later intervals
 * gave that signal is taken back, while what earlier settings gave it
 * stays.
 *
 * An expiration begins the wake record of each reader on the timer's queue
 * under the queue's lock and the run queues' lock taken after it.
 */
#include "io/timerfd.c"

extern int printf(const char *format, ...);

volatile uint64_t kernel_ticks;

/** The tick count as the kernel reads it whole: the model's `kernel_ticks`. */
uint64_t tick_count(void)
{
    return kernel_ticks;
}
/* A tick the timer has raised and the boot processor has not yet counted,
 * which the pending row sets. */
static uint32_t tick_pending;

/** The tick count as the clocks count it: `kernel_ticks` and a pending
 * tick. */
uint64_t rtc_ticks_now(void)
{
    return kernel_ticks + tick_pending;
}
static struct ktimer timer;
static fd_entry_t entry;
static struct open_file description;
static process_t reader;
struct percpu cpu_table[NR_CPUS] = {{.current = &reader}};
static uint64_t user[2];
static int failures, masked, wakes;
static int tick_in_copy, read_in_copy;
static int32_t inner_result;
static uint64_t inner_value;

#define CHECK(condition, detail)                              \
    do {                                                      \
        if (!(condition)) {                                   \
            printf("timerfd-hosttest: failed: %s\n", detail); \
            failures++;                                       \
        }                                                     \
    } while (0)

/** NULL: the POSIX timers' part, which no row reaches, finds no process. */
process_t *get_process(uint32_t pid)
{
    (void)pid;
    return 0;
}

/* The POSIX timer rows' process and its signals posted: while `posts_ok`
 * is set a post is counted and kept, and otherwise it is a failure; while
 * `posts_queue` is set the signal stays pending in the process, as a
 * blocked one does. */
static struct tgroup group;
static int posts_ok, posts_queue, posts, post_signo;
static struct k_siginfo post_info;

/** Count a POSIX timer's signal to `target` while the rows allow one; a
 * failure otherwise, since a timerfd posts none. */
void signal_post_info(process_t *target, int signo, const struct k_siginfo *info)
{
    CHECK(posts_ok, "a timerfd posted a signal");
    CHECK(target == group.leader, "a timer's signal goes to its process's leader");
    CHECK(masked > 0, "a timer's signal is posted under timer_lock");
    posts++;
    post_signo = signo;
    post_info = *info;
    if (posts_queue) {
        group.pending |= KSIG_BIT(signo);
        group.info[signo] = *info;
    }
}

/** A failure: a timerfd posts no signal. */
void signal_post_thread(process_t *t, int signo, const struct k_siginfo *info)
{
    (void)t;
    (void)signo;
    (void)info;
    CHECK(0, "a timerfd posted a signal");
}

/** Count a nesting of the model's interrupt mask; the count before. */
uint32_t kirq_save(void)
{
    return (uint32_t)masked++;
}

/** Put the count of nestings back to `flags`. */
void kirq_restore(uint32_t flags)
{
    masked = (int)flags;
}

/**
 * The copy to the caller. With `tick_in_copy` the next tick comes first, and
 * with `read_in_copy` a second read, as either can while the copy sleeps. A
 * destination of 0 is a buffer that cannot be written.
 */
int copy_to_user(void __user *dst, const void *src, size_t len)
{
    if (tick_in_copy) {
        tick_in_copy = 0;
        timerfd_tick(++kernel_ticks);
    }
    if (read_in_copy) {
        read_in_copy = 0;
        inner_result = timerfd_read(&entry, (char *)&inner_value, sizeof(inner_value));
    }
    if (!dst) {
        return -EFAULT;
    }
    k_memcpy(dst, src, len);
    return 0;
}

/** Copy `len` bytes from the host address `src`; 0. */
int copy_from_user(void *dst, const void __user *src, size_t len)
{
    k_memcpy(dst, src, len);
    return 0;
}

/** A read that would wait is interrupted; the rows use nonblocking reads. */
int wq_wait_event(wait_queue_t *wq, wq_cond_fn cond, void *arg)
{
    (void)wq;
    (void)cond;
    (void)arg;
    return -EINTR;
}

/* The reader the timer's queue holds in the wake record's row, or NULL;
 * whether the queue's lock and the run queues' lock are held; and the wake
 * records begun, with the reader, deadline and locks of the last. */
static process_t *waiter;
static int queue_held, sched_held;
static int records, record_queue, record_sched;
static process_t *record_proc;
static uint64_t record_due;

/** Call `fn` for the queue's one reader, `waiter`, under the queue's lock,
 * as the kernel's walk does; nothing when it has none. */
void wq_each_waiter(wait_queue_t *wq, void (*fn)(struct process *p, uint64_t arg), uint64_t arg)
{
    (void)wq;
    if (waiter) {
        queue_held = 1;
        fn(waiter, arg);
        queue_held = 0;
    }
}

/** Take the run queues' lock, which comes after a queue's: the interrupt
 * state before. */
uint32_t sched_lock_take(void)
{
    CHECK(!sched_held, "the run queues' lock is free when taken");
    sched_held = 1;
    return kirq_save();
}

/** Give back the run queues' lock and put interrupts back as `flags`. */
void sched_lock_give(uint32_t flags)
{
    CHECK(sched_held, "the run queues' lock given back is held");
    sched_held = 0;
    kirq_restore(flags);
}

/** Count a wake record begun for `p` at `due`, and the locks held. */
void waketime_expire(struct process *p, uint64_t due)
{
    records++;
    record_proc = p;
    record_due = due;
    record_queue = queue_held;
    record_sched = sched_held;
}

/** Count a wakeup of the timer's readers. */
void wq_wake_all(wait_queue_t *wq)
{
    (void)wq;
    wakes++;
}

/** Empty the queue `wq`. */
void wait_queue_init(wait_queue_t *wq)
{
    wq->head = NULL;
}

/** 1000 s and 0 ns on every clock. */
int32_t clock_now(int32_t clk, int64_t *sec, int32_t *nsec)
{
    (void)clk;
    *sec = 1000;
    *nsec = 0;
    return 0;
}

/** No timer here counts processor time, so none holds a process. */
int32_t clock_cpu_target(int32_t clk, struct tgroup **tg, uint32_t *tid, int *user_only)
{
    (void)clk;
    (void)tg;
    (void)tid;
    (void)user_only;
    return -EINVAL;
}

/** Nothing: no timer holds a process. */
void tgroup_put(struct tgroup *tg)
{
    (void)tg;
}

/** NULL: no row makes a descriptor. */
void *kzalloc(size_t n)
{
    (void)n;
    return NULL;
}

/* The POSIX timer of the rows, which `ptimer_free` gives back here. */
static struct ktimer *posix_timer;
static int posix_freed;

/** Count the POSIX timer's free; no row frees anything else. */
void kfree(void *ptr)
{
    if (ptr && ptr == posix_timer)
        posix_freed++;
}

/** -1: no row makes a descriptor. */
int fd_alloc(fd_table_t *table)
{
    (void)table;
    return -1;
}

/** NULL: no row finds a descriptor by number. */
fd_entry_t *fd_get(fd_table_t *table, int fd)
{
    (void)table;
    (void)fd;
    return NULL;
}

/** Copy `n` bytes from `src` to `dst`; `dst`. */
void *k_memcpy(void *dst, const void *src, size_t n)
{
    uint8_t *d = dst;
    const uint8_t *s = src;
    size_t i;

    for (i = 0; i < n; i++) {
        d[i] = s[i];
    }
    return dst;
}

/** Fill `n` bytes of `dst` with `c`; `dst`. */
void *k_memset(void *dst, int c, size_t n)
{
    uint8_t *d = dst;
    size_t i;

    for (i = 0; i < n; i++) {
        d[i] = (uint8_t)c;
    }
    return dst;
}

/** A nonblocking descriptor over a timer armed to expire every tick from
 * the next, and the ticks run to its first expiration. */
static void setup(void)
{
    struct setting every = {0, NS_PER_TICK, 0, NS_PER_TICK};

    k_memset(&timer, 0, sizeof(timer));
    k_memset(&entry, 0, sizeof(entry));
    k_memset(&description, 0, sizeof(description));
    armed = NULL;
    kernel_ticks = 1000;
    description.status_flags = OFD_NONBLOCK;
    entry.ofd = &description;
    entry.priv = &timer;
    entry.flags = FD_FLAG_READ;
    user[0] = 0;
    inner_value = 0;
    inner_result = 0;
    tick_in_copy = read_in_copy = 0;
    CHECK(arm(&timer, 0, &every) == 0, "the timer arms");
    kernel_ticks += 2;
    timerfd_tick(kernel_ticks);
    CHECK(timer.count == 1, "the timer expires at its tick");
}

/** Disarm the timer and set the tick count to 1000. */
static void reset(void)
{
    k_memset(&timer, 0, sizeof(timer));
    armed = NULL;
    kernel_ticks = 1000;
}

/** A timer of a tick armed at tick 1000 while tick 1001 is pending, which
 * the clocks already count: it expires a tick after 1001, at 1003, and not
 * at 1002, which comes less than a tick after the clock's reading at the
 * arm. */
static void pending_rows(void)
{
    struct setting tick = {0, 0, 0, NS_PER_TICK};
    struct setting got;

    reset();
    tick_pending = 1;
    CHECK(arm(&timer, 0, &tick) == 0, "a timer of a tick arms while a tick is pending");
    current_setting(&timer, &got);
    CHECK(got.vsec == 0 && got.vnsec == 2 * NS_PER_TICK,
          "it reports the tick it waits and the tick in progress");
    tick_pending = 0;
    kernel_ticks = 1001;
    timerfd_tick(1001);
    kernel_ticks = 1002;
    timerfd_tick(1002);
    CHECK(timer.count == 0, "it does not expire at 1002, less than a tick after the arm");
    kernel_ticks = 1003;
    timerfd_tick(1003);
    CHECK(timer.count == 1, "it expires at 1003");
}

/** A timer of 200 days, past 2^32 ticks, and one past the most seconds a
 * setting keeps. */
static void long_rows(void)
{
    struct setting days = {0, 0, 17280000, 0};
    struct setting every = {17280000, 0, 1, 0};
    struct setting forever = {0, 0, 0x7FFFFFFFFFFFFFFFll, 0};
    struct setting got;

    reset();
    CHECK(arm(&timer, 0, &days) == 0, "a timer of 200 days arms");
    current_setting(&timer, &got);
    CHECK(got.vsec == 17280000 && got.vnsec == NS_PER_TICK,
          "a timer of 200 days reports 200 days and the tick in progress");
    timerfd_tick(1000u + 17280000ull * TICK_HZ);
    CHECK(timer.count == 0 && timer.armed, "a timer of 200 days does not expire early");
    timerfd_tick(1001u + 17280000ull * TICK_HZ);
    CHECK(timer.count == 1 && !timer.armed, "a timer of 200 days expires at its tick");

    reset();
    CHECK(arm(&timer, 0, &every) == 0, "a timer every 200 days arms");
    kernel_ticks += TICK_HZ + 1u;
    timerfd_tick(kernel_ticks);
    CHECK(timer.count == 1, "its first expiration comes after a second");
    current_setting(&timer, &got);
    CHECK(got.vsec == 17280000 && got.vnsec == 0 && got.isec == 17280000 && got.insec == 0,
          "its next expiration is 200 days away, and its interval 200 days");

    reset();
    CHECK(arm(&timer, 0, &forever) == 0, "a timer past the most seconds a setting keeps arms");
    current_setting(&timer, &got);
    CHECK(got.vsec == (int64_t)TIME_MAX, "it is taken as the most seconds a setting keeps");
}

/* The POSIX timer of the rows and its process's leader. */
static struct ktimer pt;
static process_t leader;

/** A POSIX timer, id 7, SIGALRM with the value 0x55, of one tick's interval
 * and due at the next tick, armed and the one timer of a process that has a
 * thread; no post yet, and none allowed. */
static void posix_setup(void)
{
    reset();
    k_memset(&pt, 0, sizeof(pt));
    k_memset(&group, 0, sizeof(group));
    fire_head = NULL;
    fire_tail = &fire_head;
    fire_due = 0;
    group.leader = &leader;
    group.live = 1;
    leader.tg = &group;
    pt.tg = &group;
    pt.id = 7;
    pt.notify = SIGEV_SIGNAL;
    pt.signo = SIGALRM;
    pt.value = 0x55;
    pt.isub = 1;
    pt.due = kernel_ticks + 1;
    pt.armed = 1;
    pt.next = armed;
    armed = &pt;
    group.timers = &pt;
    posix_timer = &pt;
    posts = posix_freed = 0;
    posts_ok = posts_queue = 0;
}

/**
 * A POSIX timer of one tick's interval that expires at two ticks posts no
 * signal from either: it is queued once and counts both. `ptimer_post_due`
 * posts one SIGALRM with the timer's id, value and an overrun of one, and
 * empties the queue. A third expiration queued and the timer then deleted
 * takes it off the queue and posts its signal with an overrun of 0 before
 * the free, as Linux delivers a signal queued before `sigqueue_free`; after
 * the process's last thread has ended, the delete posts nothing.
 */
static void posix_rows(void)
{
    posix_setup();
    timerfd_tick(++kernel_ticks);
    timerfd_tick(++kernel_ticks);
    CHECK(ptimer_posts_due() && pt.fire_queued && pt.count == 2,
          "a POSIX timer's expirations are queued once and counted, posting nothing");
    posts_ok = 1;
    ptimer_post_due();
    CHECK(posts == 1 && post_signo == SIGALRM && post_info.code == SI_TIMER && post_info.pid == 7 &&
              post_info.value == 0x55 && post_info.uid == 1,
          "a holder posts one signal with the timer's id and value and an overrun of one");
    CHECK(!ptimer_posts_due() && !pt.fire_queued && pt.count == 0 && fire_head == NULL,
          "and the queue is empty");
    timerfd_tick(++kernel_ticks);
    timerfd_tick(++kernel_ticks);
    CHECK(ptimer_posts_due() && pt.fire_queued && pt.count == 2, "the next expirations are queued");
    ptimer_delete_all(&group);
    CHECK(posts == 2 && post_info.pid == 7 && post_info.uid == 0,
          "deleting the timer posts its queued expiration with an overrun of 0");
    CHECK(posix_freed == 1 && group.timers == NULL && !armed, "and frees it and disarms it");
    CHECK(!ptimer_posts_due() && fire_head == NULL && fire_tail == &fire_head,
          "and takes it off the queue");
    ptimer_post_due();
    CHECK(posts == 2, "so a holder posts nothing more for it");

    posix_setup();
    timerfd_tick(++kernel_ticks);
    group.live = 0;
    ptimer_delete_all(&group);
    CHECK(posts == 0 && posix_freed == 1 && fire_head == NULL,
          "a delete after the last thread has ended posts nothing and frees the timer");
    posix_timer = NULL;
}

/**
 * A setting's end, at timer_settime or timer_delete, as Linux's: Linux sends
 * a timer's signal at expiry, adds 1 to it for each setting's first
 * expiration while it is queued (`send_sigqueue`), and counts a periodic
 * setting's later intervals only at delivery (`posixtimer_rearm`), which a
 * set or a delete stops.
 *
 * A set between an expiration and its post posts it with an overrun of 0.
 * With SIGALRM blocked, a one-shot timer that expires, is set again and
 * expires again leaves its signal an overrun of 1, which a further set
 * keeps.
 */
static void posix_set_rows(void)
{
    struct setting every = {0, NS_PER_TICK, 0, NS_PER_TICK};
    struct setting once = {0, 0, 0, NS_PER_TICK};
    struct setting off = {0, 0, 0, 0};
    struct k_siginfo *held = &group.info[SIGALRM];

    posix_setup();
    timerfd_tick(++kernel_ticks);
    timerfd_tick(++kernel_ticks);
    CHECK(pt.fire_queued && pt.count == 2, "two expirations wait for their post");
    posts_ok = 1;
    CHECK(arm(&pt, 0, &every) == 0, "timer_settime between the expiration and the post");
    CHECK(posts == 1 && post_info.pid == 7 && post_info.uid == 0,
          "posts the expiration with an overrun of 0");
    CHECK(!pt.fire_queued && pt.count == 0 && pt.overrun == 0 && !fire_due,
          "and keeps none for later");

    posix_setup();
    pt.isub = 0;
    posts_ok = posts_queue = 1;
    timerfd_tick(++kernel_ticks);
    ptimer_post_due();
    CHECK(posts == 1 && held->uid == 0 && !pt.armed,
          "a one-shot timer's blocked signal is posted with an overrun of 0");
    CHECK(arm(&pt, 0, &once) == 0, "the timer is set again while its signal is pending");
    kernel_ticks += 2;
    timerfd_tick(kernel_ticks);
    ptimer_post_due();
    CHECK(posts == 1 && held->uid == 1 && pt.overrun == 0,
          "its second expiration adds 1 to the pending signal");
    CHECK(arm(&pt, 0, &off) == 0 && held->uid == 1,
          "and a disarm keeps the overrun the earlier settings gave the signal");
    group.pending = 0;
    posts_ok = posts_queue = 0;
    posix_timer = NULL;
    armed = NULL;
}

/**
 * A periodic timer's later intervals and the end of its setting, with
 * SIGALRM blocked. A timer whose signal has an overrun of 0 and whose next
 * interval is queued is deleted with the signal's overrun still 0. Later
 * intervals counted into the pending signal are taken back at a set, the
 * new setting's first expiration adds 1 and its next interval 1 more, and a
 * delete takes that interval back.
 */
static void posix_periodic_rows(void)
{
    struct setting every = {0, NS_PER_TICK, 0, NS_PER_TICK};
    struct k_siginfo *held = &group.info[SIGALRM];

    posix_setup();
    posts_ok = posts_queue = 1;
    timerfd_tick(++kernel_ticks);
    ptimer_post_due();
    CHECK(posts == 1 && held->uid == 0, "a periodic timer's blocked signal has an overrun of 0");
    timerfd_tick(++kernel_ticks);
    CHECK(pt.fire_queued && pt.count == 1, "and its next interval is queued");
    ptimer_delete_all(&group);
    CHECK(posts == 1 && held->uid == 0 && posix_freed == 1 && fire_head == NULL,
          "a delete drops the queued interval and leaves the signal's overrun 0");
    group.pending = 0;

    posix_setup();
    posts_ok = posts_queue = 1;
    timerfd_tick(++kernel_ticks);
    ptimer_post_due();
    timerfd_tick(++kernel_ticks);
    timerfd_tick(++kernel_ticks);
    ptimer_post_due();
    CHECK(posts == 1 && held->uid == 2 && pt.overrun == 2,
          "a periodic timer's later intervals add to its pending signal");
    CHECK(arm(&pt, 0, &every) == 0 && held->uid == 0 && pt.overrun == 0,
          "timer_settime takes them back out");
    kernel_ticks += 2;
    timerfd_tick(kernel_ticks);
    ptimer_post_due();
    CHECK(posts == 1 && held->uid == 1 && pt.overrun == 0,
          "the new setting's first expiration adds 1");
    timerfd_tick(++kernel_ticks);
    ptimer_post_due();
    CHECK(posts == 1 && held->uid == 2 && pt.overrun == 1, "and its next interval 1 more");
    ptimer_delete_all(&group);
    CHECK(posts == 1 && held->uid == 1 && posix_freed == 1,
          "a delete takes that interval back and keeps the first expiration's 1");
    group.pending = 0;
    posts_ok = posts_queue = 0;
    posix_timer = NULL;
    armed = NULL;
}

/**
 * A timerfd of a tick, armed at tick 1000 with a reader blocked on its
 * queue, expires at 1002 and begins the reader's wake record for that
 * deadline under the queue's lock and the run queues' lock taken after it,
 * so that a wake of the reader from elsewhere, which moves the record's
 * stage under the run queues' lock, is not overwritten.
 */
static void wake_record_rows(void)
{
    struct setting tick = {0, 0, 0, NS_PER_TICK};

    reset();
    records = 0;
    CHECK(arm(&timer, 0, &tick) == 0, "a timer of a tick arms");
    waiter = &reader;
    kernel_ticks = 1002;
    timerfd_tick(kernel_ticks);
    waiter = NULL;
    CHECK(timer.count == 1 && records == 1 && record_proc == &reader && record_due == 1002,
          "its expiration begins its reader's wake record at its deadline");
    CHECK(record_queue && record_sched,
          "under the queue's lock and the run queues' lock taken after it");
    CHECK(!sched_held && !queue_held, "and gives both back");
}

/**
 * Run every row; 0 when each passes, 1 otherwise. An expiration during a
 * read's copy is kept for the next read, a second read during the copy finds
 * none, a copy that fails loses the count it took, and a read shorter than
 * eight bytes gives -EINVAL.
 */
int main(void)
{
    setup();
    tick_in_copy = 1;
    CHECK(timerfd_read(&entry, (char *)user, 8) == 8 && user[0] == 1,
          "the read returns the expiration before its copy");
    CHECK(timer.count == 1, "an expiration during the copy is kept for the next read");
    CHECK(timerfd_read(&entry, (char *)user, 8) == 8 && user[0] == 1,
          "the next read returns the expiration the copy let in");

    setup();
    read_in_copy = 1;
    CHECK(timerfd_read(&entry, (char *)user, 8) == 8 && user[0] == 1, "the first read");
    CHECK(inner_result == -EAGAIN, "a read during the first read's copy finds none");

    setup();
    CHECK(timerfd_read(&entry, NULL, 8) == -EFAULT, "a read into memory it cannot write");
    CHECK(timer.count == 0, "a failed copy loses the count it took");
    CHECK(timerfd_read(&entry, (char *)user, 7) == -EINVAL, "a read shorter than eight bytes");
    pending_rows();
    long_rows();
    posix_rows();
    posix_set_rows();
    posix_periodic_rows();
    wake_record_rows();
    CHECK(masked == 0, "every interrupt mask is taken back");

    printf("timerfd-hosttest: result: %s: %d failures\n", failures ? "fail" : "pass", failures);
    return failures ? 1 : 0;
}
