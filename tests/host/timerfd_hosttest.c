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
 * A read of an expired one-shot timer, blocking or not, returns a count of
 * one, and the next finds nothing.
 *
 * A timer expires at the first tick at or after its time, counted from the
 * clock's place within its tick, and a timer armed while a tick is pending
 * counts from that tick. A timer's time is armed on the timer's interrupt
 * between ticks when it is set, however far, at its end on the monotonic clock
 * or its end less the real-time clock's offset, which expires the timer at that
 * time, and every tick arms it again. A POSIX timer of a nanosecond is armed
 * between ticks only while its signal was just set pending or taken, never for
 * an ignored signal of an interval under a tick or SIGEV_NONE; one of a tick or
 * more stays armed between ticks while its signal is ignored. A timer whose
 * signal is pending has the intervals its clock passed since the ticks last
 * counted it added to the signal's overrun when the signal is taken, and its
 * time moved past the clock, and so does a query of its time; a timer whose
 * expirations waited for another timer's signal of the same number has its
 * signal posted when that signal is taken, a one-shot timer's too, and is
 * armed between ticks again. timer_getoverrun reports the overrun of the
 * signal last delivered. Only a delivery under the setting in force counts
 * the intervals since the signal was queued; notifications that wait are
 * posted oldest first; a discarded signal takes its timers' waiting
 * notifications; and exec deletes every timer and discards the timers'
 * pending signals. The tick, which runs without the big lock, leaves a
 * POSIX timer's expirations for a holder of it to signal, joined into one
 * signal; timer_settime and timer_delete post one still waiting, and exec
 * drops it. An expired one-shot timer of
 * SIGEV_NONE has no time left. A tick that comes before
 * the clock reaches the time does not expire it. A periodic timer counts its
 * expirations from the time and interval it was given, to the nanosecond: one
 * armed for a time long past counts every interval since, and an interval of a
 * tick and a half keeps its phase. A timer of 200 days, past 2^32 ticks,
 * reports 200 days, does not expire a tick early and expires at its tick; an
 * interval of 200 days is reported and waited whole; and a time past the most
 * seconds a setting keeps is taken as that most.
 *
 * The model's clock is the tick count as the clocks count it, its place
 * within the tick and `clock_lag_ns` less, on every clock, with
 * `real_step_ns` more on CLOCK_REALTIME, which a row moves before it calls
 * `timerfd_clock_was_set` as a setting of the clock does; at a tick the place
 * is 0.
 *
 * A step of the real-time clock forward past an absolute timer's time
 * expires it at the next tick, and a step back keeps it waiting until the
 * clock reaches the time again, while a relative timer of CLOCK_REALTIME
 * does not move. A timerfd set with TFD_TIMER_CANCEL_ON_SET becomes readable
 * at a step, and its read fails with ECANCELED and finds nothing after; a
 * slew of adjtimex's ADJ_OFFSET counts its tick again and cancels nothing. A
 * periodic one whose expiration nothing touched stays disarmed after that
 * read and reports its interval; one that timerfd_gettime touched runs on.
 * A canceled timerfd set to disarmed is not readable: a blocking read waits,
 * and a nonblocking read fails with ECANCELED; a setting that arms a
 * canceled timerfd arms it and fails with ECANCELED. A tick that comes
 * between the arming's reading of the tick and of the clock leaves the timer
 * to expire at the first tick at which the clock reaches its time.
 */
#include "io/timerfd.c"
#include "compat/waketime_none.h"

/* The interrupt between ticks runs under the big lock and posts the
 * signals of the POSIX timers it expired (`timer_hr_pass`), as `tick` below
 * posts those of the tick, as the tick's locked part does. */
#define timerfd_due() (timerfd_due(), ptimer_post_due())

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
/* How far into its tick the clock is, which the phase rows set; half a
 * tick otherwise. */
static uint32_t model_phase_ns = NS_PER_TICK / 2;
/* How far the model's clock moves on after each reading `clock_now` makes,
 * 0 for not at all. */
static uint32_t read_step_ns;
/* How far the model's clock is behind its ticks, which the lag row sets. */
static int64_t clock_lag_ns;

/* What a setting of the real-time clock added to it, which the step rows
 * set. */
static int64_t real_step_ns;
/* With 1, a tick is raised after the next reading of the tick count or of a
 * clock, before the other, which the reading row sets. */
static int tick_after_reading;

/** Raise a pending tick once when `tick_after_reading` asks for one. */
static void reading_done(void)
{
    if (tick_after_reading) {
        tick_after_reading = 0;
        tick_pending++;
    }
}

/** The tick count as the clocks count it and the model's place within the
 * tick. */
void rtc_tick_phase(uint64_t *ticks, uint32_t *phase_ns)
{
    *ticks = rtc_ticks_now();
    *phase_ns = model_phase_ns;
    reading_done();
}
static struct ktimer timer;
static fd_entry_t entry;
static struct open_file description;
static process_t reader;
struct percpu cpu_table[NR_CPUS] = {{.current = &reader}};
static uint64_t user[2];
static int failures, masked, wakes, waits;
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

/* The process of the POSIX timer rows, its leader, whether it ignores the
 * timer's signal, and the signals posted to it. */
static struct tgroup group;
static process_t leader;
static int ignores, posts;

/* The stop signals, which SIGCONT discards and which discard SIGCONT. */
#define STOPS (KSIG_BIT(SIGSTOP) | KSIG_BIT(SIGTSTP) | KSIG_BIT(SIGTTIN) | KSIG_BIT(SIGTTOU))

/** Discard the signals `bits` of the rows' process and its timers'
 * notifications of them, as `sig_discard` does. */
static void discard(uint64_t bits)
{
    group.pending &= ~bits;
    ptimer_discard(&group, bits);
}

/** A signal to the POSIX timer rows' process: counted, its job control
 * done, as `sig_job_control` discards the stops for SIGCONT and SIGCONT
 * for a stop, and set pending with its siginfo unless the process ignores
 * it, as posting drops an ignored signal; a failure for any other target,
 * since a timerfd posts none. */
void signal_post_info(process_t *target, int signo, const struct k_siginfo *info)
{
    if (target != &leader) {
        CHECK(0, "a timerfd posted a signal");
        return;
    }
    posts++;
    if (signo == SIGCONT) {
        discard(STOPS);
    } else if (KSIG_BIT(signo) & STOPS) {
        discard(KSIG_BIT(SIGCONT));
    }
    if (!ignores) {
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
static void tick(uint64_t n);

int copy_to_user(void __user *dst, const void *src, size_t len)
{
    if (tick_in_copy) {
        tick_in_copy = 0;
        tick(kernel_ticks + 1u);
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

/** Count a read's wait, which a signal interrupts at once. */
int wq_wait_event(wait_queue_t *wq, wq_cond_fn cond, void *arg)
{
    (void)wq;
    (void)cond;
    (void)arg;
    waits++;
    return -EINTR;
}

/** Count a wakeup of the timer's readers. */
void wq_wake_all(wait_queue_t *wq)
{
    (void)wq;
    wakes++;
}

/** Call `fn` for each reader on `wq`, as the kernel's walk does under the
 * queue's lock. */
void wq_each_waiter(wait_queue_t *wq, void (*fn)(struct process *p, uint64_t arg), uint64_t arg)
{
    struct wq_waiter *w;

    for (w = wq->head; w; w = w->next) {
        if (w->proc) {
            fn(w->proc, arg);
        }
    }
}

/** Take the run queues' lock: the interrupt state before. */
uint32_t sched_lock_take(void)
{
    return kirq_save();
}

/** Give back the run queues' lock and put interrupts back as `flags`. */
void sched_lock_give(uint32_t flags)
{
    kirq_restore(flags);
}

/** Empty the queue `wq`. */
void wait_queue_init(wait_queue_t *wq)
{
    wq->head = NULL;
}

/** The model's clock, on every clock: the tick count as the clocks count it
 * and the place within the tick, less `clock_lag_ns`, and with
 * `real_step_ns` on CLOCK_REALTIME. */
int32_t clock_now(int32_t clk, int64_t *sec, int32_t *nsec)
{
    int64_t ns = (int64_t)rtc_ticks_now() * NS_PER_TICK + model_phase_ns - clock_lag_ns;

    if (clk == CLOCK_REALTIME) {
        ns += real_step_ns;
    }
    *sec = ns / NS_PER_SEC;
    *nsec = (int32_t)(ns % NS_PER_SEC);
    model_phase_ns += read_step_ns;
    reading_done();
    return 0;
}

/* The time the timer's interrupt between ticks was last armed for
 * (`timer_wake_at`), and how many times it was armed, which the between
 * rows read. */
static int64_t wake_ns;
static int wake_arms;

/** Note the time the interrupt between ticks is armed for. */
void timer_wake_at(int64_t ns)
{
    wake_ns = ns;
    wake_arms++;
}

/** Nothing: the rows count each time asked of `timer_wake_at`, which the
 * kernel's passes collect into one programming, as timercal_hosttest's
 * hold rows check. */
void timer_wake_hold(void)
{}

/** Nothing, as `timer_wake_hold`. */
void timer_wake_release(void)
{}

/** Real time less the monotonic clock: the model's step of the real-time
 * clock. */
void rtc_offset(int64_t *seconds, int32_t *nanoseconds)
{
    *seconds = real_step_ns / NS_PER_SEC;
    *nanoseconds = (int32_t)(real_step_ns % NS_PER_SEC);
}

/** The model's monotonic clock, CLOCK_MONOTONIC's as `clock_now` reads it,
 * without counting as a reading. */
int64_t rtc_monotonic_ns(void)
{
    return (int64_t)rtc_ticks_now() * NS_PER_TICK + model_phase_ns - clock_lag_ns;
}

/** The timer interrupt of tick `n`: the count set to it and the clock at the
 * tick's start, as the handler finds it. */
static void tick(uint64_t n)
{
    uint32_t phase = model_phase_ns;

    kernel_ticks = n;
    model_phase_ns = 0;
    timerfd_tick(n);
    ptimer_post_due();
    model_phase_ns = phase;
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

/** Nothing: no row makes a descriptor. */
void kfree(void *ptr)
{
    (void)ptr;
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

/** A nonblocking descriptor over a timer armed as tick 1000 begins to
 * expire every tick from the next, and the tick of its first expiration,
 * with the clock left at the start of that tick, so that a read finds that
 * expiration alone. */
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
    model_phase_ns = 0;
    CHECK(arm(&timer, 0, &every) == 0, "the timer arms");
    tick(kernel_ticks + 1u);
    CHECK(timer.count == 1, "the timer expires at its tick");
}

/** Disarm the timer, take it off `cancelable`, set the tick count to 1000
 * and the clocks to agree. */
static void reset(void)
{
    k_memset(&timer, 0, sizeof(timer));
    timer.clock = CLOCK_MONOTONIC;
    armed = NULL;
    cancelable = NULL;
    fire_head = NULL;
    fire_tail = &fire_head;
    fire_due = 0;
    kernel_ticks = 1000;
    clock_lag_ns = 0;
    real_step_ns = 0;
}

/** A setting of the real-time clock `ns` nanoseconds on from where it is. */
static void step_clock(int64_t ns)
{
    real_step_ns += ns;
    timerfd_clock_was_set(1);
}

/** A step of `ns` nanoseconds that stands for adjtimex's ADJ_OFFSET, which
 * Linux slews and does not count as a setting of the clock. */
static void slew_clock(int64_t ns)
{
    real_step_ns += ns;
    timerfd_clock_was_set(0);
}

/** An absolute timer of real time for 2 s, armed at 1 s: a step forward
 * past it expires it at the next tick, and a step back of half a second
 * moves its tick to 2500 at the step, before any tick, so that the row needs
 * timerfd_clock_was_set's recount and not only expire's own check of the
 * clock, and keeps it waiting to 2.5 s. A relative one of a second runs on
 * CLOCK_MONOTONIC and its tick does not move at the step, as Linux's
 * clock_was_set retriggers only the real-time base. */
static void step_rows(void)
{
    struct setting at2 = {0, 0, 2, 0};
    struct setting in1 = {0, 0, 1, 0};
    uint64_t due;

    reset();
    timer.clock = CLOCK_REALTIME;
    model_phase_ns = 0;
    CHECK(arm(&timer, TFD_TIMER_ABSTIME, &at2) == 0, "an absolute timer of real time arms");
    step_clock(2 * (int64_t)NS_PER_SEC);
    tick(1001);
    CHECK(timer.count == 1 && !timer.armed, "a step forward past its time expires it at once");

    reset();
    timer.clock = CLOCK_REALTIME;
    model_phase_ns = 0;
    CHECK(arm(&timer, TFD_TIMER_ABSTIME, &at2) == 0, "an absolute timer of real time arms");
    step_clock(-(int64_t)NS_PER_SEC / 2);
    CHECK(timer.due == 2500u, "a step back moves its tick to 2.5 s at the step");
    tick(2u * TICK_HZ);
    CHECK(timer.count == 0 && timer.armed, "a step back keeps it waiting past its old tick");
    tick(5u * TICK_HZ / 2u);
    CHECK(timer.count == 1, "until the clock reaches its time again");

    reset();
    timer.clock = CLOCK_REALTIME;
    model_phase_ns = 0;
    CHECK(arm(&timer, 0, &in1) == 0, "a relative timer of real time arms");
    due = timer.due;
    step_clock(5 * (int64_t)NS_PER_SEC);
    CHECK(timer.due == due, "a step leaves its tick where it was");
    tick(1001);
    CHECK(timer.count == 0 && timer.armed, "a step does not move it");
    tick(2u * TICK_HZ);
    CHECK(timer.count == 1, "it expires a second after its arm by time since boot");
    model_phase_ns = NS_PER_TICK / 2;
}

/** Put `timer` behind the descriptor `entry`, nonblocking with `nonblock`. */
static void open_entry(int nonblock)
{
    k_memset(&entry, 0, sizeof(entry));
    k_memset(&description, 0, sizeof(description));
    description.status_flags = nonblock ? OFD_NONBLOCK : 0;
    entry.ofd = &description;
    entry.priv = &timer;
    entry.flags = FD_FLAG_READ;
}

/** Arm `timer` from `s` as a timerfd of real time with TFD_TIMER_ABSTIME and
 * TFD_TIMER_CANCEL_ON_SET, as tick 1000 begins, behind a descriptor that is
 * nonblocking with `nonblock`. */
static void cancel_arm(const struct setting *s, int nonblock)
{
    reset();
    timer.clock = CLOCK_REALTIME;
    model_phase_ns = 0;
    open_entry(nonblock);
    CHECK(arm(&timer, TFD_TIMER_ABSTIME | TFD_TIMER_CANCEL_ON_SET, s) == 0,
          "a timerfd canceled by a setting of the clock arms");
}

/** Whether a poll finds the timerfd readable. */
static int polls_readable(void)
{
    return timerfd_poll(&entry, POLLIN, NULL) == POLLIN;
}

/** A timerfd canceled by a setting of the clock, for 10 s: unreadable until
 * a step, readable after it, its read fails with ECANCELED and the next
 * finds nothing, and it keeps running, since it has not expired. */
static void cancel_read_rows(void)
{
    struct setting at10 = {0, 0, 10, 0};

    cancel_arm(&at10, 1);
    CHECK(!polls_readable(), "it is not readable before a step");
    step_clock((int64_t)NS_PER_SEC);
    CHECK(polls_readable(), "a step makes it readable");
    CHECK(timerfd_read(&entry, (char *)user, 8) == -ECANCELED, "its read fails with ECANCELED");
    CHECK(!polls_readable() && timerfd_read(&entry, (char *)user, 8) == -EAGAIN,
          "the next finds nothing");
    CHECK(timer.armed, "a timer that has not expired runs on");
}

/** A timerfd canceled by a setting of the clock, for 10 s, and an NTP slew of
 * half a second forward, as BusyBox ntpd's adjtimex with ADJ_OFFSET makes:
 * its tick moves to 9.5 s, since this clock jumps, but it is not canceled
 * and stays unreadable, as Linux's ADJ_OFFSET feeds the PLL and calls no
 * clock_was_set (kernel/time/ntp.c:754-755, kernel/time/timekeeping.c:
 * 2490-2532). */
static void slew_rows(void)
{
    struct setting at10 = {0, 0, 10, 0};

    cancel_arm(&at10, 1);
    slew_clock((int64_t)NS_PER_SEC / 2);
    CHECK(timer.due == 9500u, "a slew counts its tick again");
    CHECK(!polls_readable() && timerfd_read(&entry, (char *)user, 8) == -EAGAIN,
          "a slew does not cancel it");
    CHECK(timer.armed && !timer.canceled, "it runs on uncanceled");
    tick(9500);
    CHECK(timer.count == 1, "it expires at its time by the slewed clock");
}

/** A timerfd canceled by a setting of the clock, every tick from 1.001 s,
 * expired at tick 1001 and then stepped: with nothing between, its canceled
 * read leaves it disarmed, and it reports no time left and its interval, as
 * Linux's stopped timer does (fs/timerfd.c:286-289, 567-568); with
 * timerfd_gettime between, it runs on, since gettime started it again
 * (fs/timerfd.c:552-565). One at 2 s every second, expired at 2 s and then
 * stepped back to 1.5 s, stays stopped at 2 s until timerfd_gettime, which
 * forwards it against the clock as set: a time still ahead moves nothing
 * and takes back the expiration it counted, as hrtimer_forward_now's 0
 * less one does (kernel/time/hrtimer.c:1046-1079), so gettime reports half
 * a second and the timer expires again at 2 s, after the canceled read. */
static void cancel_periodic_rows(void)
{
    struct setting every = {0, NS_PER_TICK, 1, NS_PER_TICK};
    struct setting at2 = {1, 0, 2, 0};
    struct setting got;

    cancel_arm(&every, 1);
    tick(1001);
    CHECK(timer.count == 1, "the periodic timer expires");
    step_clock((int64_t)NS_PER_SEC);
    CHECK(timerfd_read(&entry, (char *)user, 8) == -ECANCELED, "its read fails with ECANCELED");
    CHECK(!timer.armed, "a timer whose expiration nothing touched stays disarmed");
    current_setting(&timer, &got);
    CHECK(got.vsec == 0 && got.vnsec == 0 && got.isec == 0 && got.insec == NS_PER_TICK,
          "it reports no time left and its interval");

    cancel_arm(&every, 1);
    tick(1001);
    current_setting(&timer, &got);
    step_clock((int64_t)NS_PER_SEC);
    CHECK(timerfd_read(&entry, (char *)user, 8) == -ECANCELED, "its read fails with ECANCELED");
    CHECK(timer.armed && timer.count == 0, "a timer timerfd_gettime touched runs on");
    tick(1002);
    CHECK(timer.count != 0, "and expires at the next tick");

    cancel_arm(&at2, 1);
    tick(2000);
    CHECK(timer.count == 1 && timer.expired, "a timer every second from 2 s expires at 2 s");
    step_clock(-(int64_t)NS_PER_SEC / 2);
    current_setting(&timer, &got);
    CHECK(got.vsec == 0 && got.vnsec == NS_PER_SEC / 2 && timer.count == 0,
          "after a step back to 1.5 s, timerfd_gettime reports the half second to 2 s");
    CHECK(timerfd_read(&entry, (char *)user, 8) == -ECANCELED, "its read fails with ECANCELED");
    CHECK(timer.armed && timer.due == 2500u, "and it runs on to 2 s, at tick 2500");
    tick(2500);
    CHECK(timer.count == 1, "where it expires again");
    model_phase_ns = NS_PER_TICK / 2;
}

/** A canceled timerfd set to disarmed is not readable, as Linux's setting
 * zeroes `ticks` and keeps the cancel mark (fs/timerfd.c:200-201, 215-228):
 * a blocking read waits, and the signal that ends the wait gives ECANCELED,
 * and a nonblocking read gives ECANCELED at once (fs/timerfd.c:276-290). A
 * setting that arms a canceled timerfd arms it and fails with ECANCELED,
 * and leaves it unreadable and no longer canceled. */
static void cancel_setting_rows(void)
{
    struct setting at10 = {0, 0, 10, 0};
    struct setting off = {0, 0, 0, 0};

    cancel_arm(&at10, 0);
    step_clock((int64_t)NS_PER_SEC);
    CHECK(arm(&timer, TFD_TIMER_ABSTIME | TFD_TIMER_CANCEL_ON_SET, &off) == 0 && !timer.armed,
          "a setting disarms the canceled timerfd");
    CHECK(!polls_readable(), "it is not readable");
    waits = 0;
    CHECK(timerfd_read(&entry, (char *)user, 8) == -ECANCELED && waits == 1,
          "a blocking read waits, and a signal that ends the wait gives ECANCELED");

    cancel_arm(&at10, 1);
    step_clock((int64_t)NS_PER_SEC);
    CHECK(arm(&timer, TFD_TIMER_ABSTIME | TFD_TIMER_CANCEL_ON_SET, &off) == 0,
          "a setting disarms the canceled timerfd");
    CHECK(timerfd_read(&entry, (char *)user, 8) == -ECANCELED,
          "a nonblocking read gives ECANCELED at once");

    cancel_arm(&at10, 1);
    step_clock((int64_t)NS_PER_SEC);
    CHECK(arm(&timer, TFD_TIMER_ABSTIME | TFD_TIMER_CANCEL_ON_SET, &at10) == -ECANCELED &&
              timer.armed,
          "a setting that arms a canceled timerfd arms it and fails with ECANCELED");
    CHECK(!polls_readable() && timerfd_read(&entry, (char *)user, 8) == -EAGAIN,
          "and leaves it unreadable and no longer canceled");
    model_phase_ns = NS_PER_TICK / 2;
}

/** A timer of a tick armed as tick 1000 begins, with a tick raised between
 * the arming's two readings: the clock then reads a tick on from the tick
 * count, and the timer expires at the first tick at which the clock reaches
 * its time, never before and not a tick after, as `read_now` reads the tick
 * first. */
static void reading_rows(void)
{
    struct setting one = {0, 0, 0, NS_PER_TICK};
    uint64_t n, first = 0;
    int64_t end;

    reset();
    model_phase_ns = 0;
    tick_after_reading = 1;
    CHECK(arm(&timer, 0, &one) == 0, "a timer of a tick arms across a tick");
    CHECK(tick_after_reading == 0, "the tick came between the readings");
    end = timer.end;
    tick_pending = 0;
    for (n = 1001; n <= 1004 && first == 0; n++) {
        tick(n);
        if (timer.count != 0) {
            first = n;
        }
    }
    CHECK(end == 1002 * (int64_t)NS_PER_TICK, "its time counts from the clock's later reading");
    CHECK(first == (uint64_t)((end + NS_PER_TICK - 1) / NS_PER_TICK),
          "it expires at the first tick at which the clock reaches its time");
    model_phase_ns = NS_PER_TICK / 2;
}

/** A timer of a tick armed half a tick after tick 1001, which is pending
 * and which the clocks already count: its time falls half a tick after
 * 1002, so it expires at 1003, and not at 1002, which comes less than a
 * tick after the clock's reading at the arm. */
static void pending_rows(void)
{
    struct setting one = {0, 0, 0, NS_PER_TICK};
    struct setting got;

    reset();
    tick_pending = 1;
    CHECK(arm(&timer, 0, &one) == 0, "a timer of a tick arms while a tick is pending");
    current_setting(&timer, &got);
    CHECK(got.vsec == 0 && got.vnsec == NS_PER_TICK, "it reports the tick it waits");
    tick_pending = 0;
    tick(1001);
    tick(1002);
    CHECK(timer.count == 0, "it does not expire at 1002, less than a tick after the arm");
    tick(1003);
    CHECK(timer.count == 1, "it expires at 1003");
}

/** A timer of a tick expires at the first tick at or after its time: armed
 * as tick 1000 begins, at 1001, and armed just before 1001, at 1002, a
 * tick after the arm in both, as Linux's hrtimer is never early. */
static void phase_rows(void)
{
    struct setting one = {0, 0, 0, NS_PER_TICK};

    reset();
    model_phase_ns = 0;
    CHECK(arm(&timer, 0, &one) == 0, "a timer of a tick arms as its tick begins");
    tick(1001);
    CHECK(timer.count == 1, "it expires at the next tick, a tick after the arm");

    reset();
    model_phase_ns = NS_PER_TICK - 1u;
    CHECK(arm(&timer, 0, &one) == 0, "a timer of a tick arms just before the next tick");
    tick(1001);
    CHECK(timer.count == 0, "it does not expire at the next tick, less than a tick after");
    tick(1002);
    CHECK(timer.count == 1, "it expires at the one after");
    model_phase_ns = NS_PER_TICK / 2;
}

/** A timer of a tick armed as tick 1000 begins, whose tick comes while the
 * clock is half a tick behind: it expires at the tick after, when the clock
 * has reached its time, as Linux's never expires before its clock does. */
static void lag_rows(void)
{
    struct setting one = {0, 0, 0, NS_PER_TICK};

    reset();
    model_phase_ns = 0;
    CHECK(arm(&timer, 0, &one) == 0, "a timer of a tick arms as its tick begins");
    clock_lag_ns = NS_PER_TICK / 2;
    tick(1001);
    CHECK(timer.count == 0 && timer.armed, "its tick does not expire it before the clock");
    tick(1002);
    CHECK(timer.count == 1 && !timer.armed, "the next tick, past its time, expires it");
    model_phase_ns = NS_PER_TICK / 2;
}

/**
 * A timer has the interrupt between ticks armed for it when it is set, at
 * the monotonic clock's time of its end, and that interrupt expires it once
 * the clock has reached its time (`timerfd_due`), with no tick, as Linux's
 * hrtimer does; one that comes before the time arms it again. A timer three
 * ticks ahead is armed when set, and again by every tick. An absolute timer
 * of real time is armed at its end less the real-time clock's offset, a time
 * of the monotonic clock, as Linux's hrtimer subtracts its base's offset.
 */
static void between_rows(void)
{
    struct setting quarter = {0, 0, 0, NS_PER_TICK / 4};
    struct setting three = {0, 0, 0, 3 * NS_PER_TICK};
    struct setting real = {0, 0, 7, 0};

    reset();
    model_phase_ns = NS_PER_TICK / 2;
    wake_arms = 0;
    CHECK(arm(&timer, 0, &quarter) == 0, "a timer of a quarter tick arms half a tick in");
    CHECK(wake_arms == 1 && wake_ns == 1000 * (int64_t)NS_PER_TICK + 3 * NS_PER_TICK / 4,
          "its time is armed between ticks");
    model_phase_ns = NS_PER_TICK / 2 + NS_PER_TICK / 8;
    timerfd_due();
    CHECK(timer.count == 0 && timer.armed && wake_arms == 2,
          "an interrupt before its time arms it again");
    model_phase_ns = 3 * NS_PER_TICK / 4;
    timerfd_due();
    CHECK(timer.count == 1 && !timer.armed, "the interrupt at its time expires it, with no tick");

    reset();
    model_phase_ns = 0;
    wake_arms = 0;
    CHECK(arm(&timer, 0, &three) == 0, "a timer of three ticks arms as tick 1000 begins");
    CHECK(wake_arms == 1 && wake_ns == 1003 * (int64_t)NS_PER_TICK,
          "a time three ticks ahead is armed between ticks when set");
    tick(1001);
    CHECK(wake_arms == 2 && wake_ns == 1003 * (int64_t)NS_PER_TICK, "tick 1001 arms it again");
    tick(1002);
    CHECK(wake_arms == 3 && wake_ns == 1003 * (int64_t)NS_PER_TICK, "and tick 1002");
    tick(1003);
    CHECK(timer.count == 1, "its own tick still expires it");

    reset();
    timer.clock = CLOCK_REALTIME;
    real_step_ns = 5 * (int64_t)NS_PER_SEC;
    kernel_ticks = 1000;
    model_phase_ns = NS_PER_TICK / 2;
    wake_arms = 0;
    CHECK(arm(&timer, TFD_TIMER_ABSTIME, &real) == 0,
          "an absolute timer of real time for 7 s arms at 6 s");
    CHECK(wake_arms == 1 && wake_ns == 2 * (int64_t)NS_PER_SEC,
          "it is armed at 7 s of real time less the offset of 5 s, 2 s of the monotonic clock");
    model_phase_ns = NS_PER_TICK / 2;
    timer.clock = CLOCK_MONOTONIC;
}

/** A timer of a quarter tick armed half a tick after tick 1001, which is
 * pending and which the clocks already count: its time between ticks is a
 * quarter tick after the clock's reading, three quarters past 1001, and the
 * interrupt at that time expires it while 1001 is still uncounted, since the
 * clock and not the tick count decides. */
static void between_pending_rows(void)
{
    struct setting quarter = {0, 0, 0, NS_PER_TICK / 4};

    reset();
    tick_pending = 1;
    model_phase_ns = NS_PER_TICK / 2;
    wake_arms = 0;
    CHECK(arm(&timer, 0, &quarter) == 0, "a timer of a quarter tick arms with a tick pending");
    CHECK(wake_arms == 1 && wake_ns == 1001 * (int64_t)NS_PER_TICK + 3 * NS_PER_TICK / 4,
          "its time between ticks counts the pending tick");
    model_phase_ns = NS_PER_TICK / 2 + NS_PER_TICK / 8;
    timerfd_due();
    CHECK(timer.count == 0 && timer.armed && wake_arms == 2,
          "an interrupt before its time, the tick still pending, arms it again");
    model_phase_ns = 3 * NS_PER_TICK / 4;
    timerfd_due();
    CHECK(timer.count == 1 && !timer.armed, "the interrupt at its time expires it, 1001 uncounted");
    tick_pending = 0;
    model_phase_ns = NS_PER_TICK / 2;
}

/**
 * A POSIX timer of an interval of a nanosecond is armed between ticks when
 * set and once more at each expiration that sets its signal pending, and not
 * while that signal is pending, since an expiration then only adds to its
 * overrun: interrupts between ticks stop, and the ticks count it, until the
 * signal is taken (`ptimer_rearm`), as Linux restarts such a timer at the
 * delivery of its signal, with the intervals since added to the signal's
 * overrun. One whose signal the process ignores is counted at ticks alone,
 * as Linux moves such a timer of an interval under a tick a tick on; and
 * SIGEV_NONE's is never armed between ticks, as Linux never starts its
 * hrtimer.
 */
static void ptimer_rows(void)
{
    struct setting fast = {0, 1, 0, 1};
    struct k_siginfo taken;

    reset();
    k_memset(&group, 0, sizeof(group));
    group.leader = &leader;
    group.timers = &timer;
    timer.tg = &group;
    timer.id = 1;
    timer.key = next_gen();
    timer.notify = SIGEV_SIGNAL;
    timer.signo = SIGALRM;
    ignores = posts = 0;
    model_phase_ns = NS_PER_TICK / 2;
    wake_arms = 0;
    CHECK(arm(&timer, 0, &fast) == 0 && wake_arms == 1,
          "a POSIX timer of a nanosecond every nanosecond is armed between ticks");
    model_phase_ns += 10;
    timerfd_due();
    CHECK(posts == 1 && (group.pending & KSIG_BIT(SIGALRM)) && wake_arms == 2,
          "its interrupt posts its signal and arms it once more");
    model_phase_ns += 10;
    timerfd_due();
    CHECK(posts == 1 && timer.set_count > 1 && group.info[SIGALRM].uid == 0 && wake_arms == 2 &&
              !timer.between,
          "the next counts one more of its setting and arms nothing");
    model_phase_ns += 10;
    timerfd_due();
    CHECK(wake_arms == 2, "and no interrupt between ticks expires it while the signal is pending");
    tick(1001);
    CHECK(timer.armed && wake_arms == 2, "the tick counts it and arms nothing");
    group.pending = 0;
    taken = group.info[SIGALRM];
    ptimer_rearm(&group, SIGALRM, &taken);
    CHECK(timer.between && wake_arms == 3, "the signal taken, it is armed between ticks again");
    CHECK(taken.uid > group.info[SIGALRM].uid && timer.overrun == taken.uid &&
              timer.end > rtc_monotonic_ns(),
          "with the intervals since the tick added to its overrun, and its time past the clock");

    group.pending = 0;
    ignores = 1;
    model_phase_ns += 10;
    timerfd_due();
    CHECK(posts == 2 && !timer.between && wake_arms == 3,
          "a signal the process ignores leaves it to the ticks");
    tick(1002);
    CHECK(posts == 3 && wake_arms == 3, "which count it, arming nothing");

    reset();
    timer.tg = &group;
    timer.id = 1;
    timer.notify = SIGEV_NONE;
    wake_arms = 0;
    CHECK(arm(&timer, 0, &fast) == 0 && wake_arms == 0 && !timer.between,
          "SIGEV_NONE's timer is not armed between ticks");
    timer.tg = NULL;
    group.timers = NULL;
    ignores = 0;
    model_phase_ns = NS_PER_TICK / 2;
}

/** A POSIX timer of `t`'s, id `id`, of SIGALRM, in the rows' process, with a
 * key of its own, as timer_create gives it. */
static void ptimer_model(struct ktimer *t, int id, int notify)
{
    t->clock = CLOCK_MONOTONIC;
    t->tg = &group;
    t->id = id;
    t->key = next_gen();
    t->notify = notify;
    t->signo = SIGALRM;
}

/** Start the process of the POSIX timer rows with `timer` alone, at tick
 * 1000 with the clock at its start. */
static void ptimer_start(int notify)
{
    reset();
    k_memset(&group, 0, sizeof(group));
    group.leader = &leader;
    group.timers = &timer;
    ptimer_model(&timer, 1, notify);
    ignores = posts = 0;
    model_phase_ns = 0;
    wake_arms = 0;
}

/**
 * A timer every 100 us from 0.1 ms whose signal stays pending: at 0.2 ms its
 * expiration counts one more of its setting, not in the pending signal, and
 * leaves it to the ticks, and the signal taken at 0.55 ms carries the
 * expirations at 0.2, 0.3, 0.4 and 0.5 ms, an overrun of 4, and arms the
 * timer for 0.6 ms, as Linux's posixtimer_rearm forwards it; timer_getoverrun
 * reports 4 until the next signal is taken, as Linux's it_overrun_last; then
 * a query at 0.95 ms of a timer whose signal is pending counts the
 * expirations since in its setting's count and reports 50 us, not a time
 * passed, as Linux's common_timer_get forwards it into it_overrun. A
 * periodic timer's first signal left pending across timer_settime, to which
 * the new setting's first expiration adds one, taken before that setting's
 * next expiration, has an overrun of 1, and timer_getoverrun reports 0, the
 * new setting's own, as Linux keeps si_overrun's carry apart from
 * it_overrun_last.
 */
static void deferred_rows(void)
{
    struct setting every = {0, 100000, 0, 100000};
    struct setting now;
    struct k_siginfo taken;

    ptimer_start(SIGEV_SIGNAL);
    CHECK(arm(&timer, 0, &every) == 0, "a timer every 100 us arms");
    model_phase_ns = 100000;
    timerfd_due();
    CHECK(posts == 1 && timer.between, "its first expiration posts its signal");
    model_phase_ns = 200000;
    timerfd_due();
    CHECK(group.info[SIGALRM].uid == 0 && timer.set_count == 2 && !timer.between,
          "the second counts in its setting, not the pending signal, and leaves it to the ticks");
    model_phase_ns = 550000;
    group.pending = 0;
    taken = group.info[SIGALRM];
    wake_arms = 0;
    ptimer_rearm(&group, SIGALRM, &taken);
    CHECK(taken.uid == 4 && timer.overrun == 4, "the signal taken at 0.55 ms has an overrun of 4");
    CHECK(timer.end == 1000 * (int64_t)NS_PER_TICK + 600000 && wake_arms == 1 &&
              wake_ns == timer.end && timer.between,
          "and the timer is armed between ticks for 0.6 ms");

    model_phase_ns = 600000;
    timerfd_due();
    model_phase_ns = 700000;
    timerfd_due();
    CHECK(posts == 2 && group.info[SIGALRM].uid == 0 && timer.set_count == 2 && !timer.between,
          "at 0.6 ms its signal is pending again, and at 0.7 ms its setting counts 2");
    CHECK(timer.overrun == 4,
          "while timer_getoverrun still reports 4, the overrun of the signal delivered");
    model_phase_ns = 950000;
    current_setting(&timer, &now);
    CHECK(now.vsec == 0 && now.vnsec == 50000 && now.insec == 100000,
          "a query at 0.95 ms reports 50 us to the next expiration");
    CHECK(group.info[SIGALRM].uid == 0 && timer.set_count == 4 && posts == 2,
          "and counts those at 0.8 and 0.9 ms in its setting's count");

    ptimer_start(SIGEV_SIGNAL);
    CHECK(arm(&timer, 0, &every) == 0, "a timer every 100 us arms again");
    model_phase_ns = 100000;
    timerfd_due();
    CHECK(posts == 1 && group.info[SIGALRM].uid == 0, "its first signal is pending at 0.1 ms");
    model_phase_ns = 150000;
    CHECK(arm(&timer, 0, &every) == 0, "timer_settime at 0.15 ms sets it for 0.25 ms");
    model_phase_ns = 250000;
    timerfd_due();
    CHECK(posts == 1 && group.info[SIGALRM].uid == 1 && group.info[SIGALRM].timer_gen == timer.gen,
          "the new setting's first expiration adds one to that signal");
    model_phase_ns = 300000;
    group.pending = 0;
    taken = group.info[SIGALRM];
    ptimer_rearm(&group, SIGALRM, &taken);
    CHECK(taken.uid == 1 && timer.overrun == 0,
          "taken at 0.3 ms, its overrun is 1 and timer_getoverrun reports 0");
    disarm(&timer);
    group.timers = NULL;
    model_phase_ns = NS_PER_TICK / 2;
}

/**
 * The tick, which runs without the big lock, posts no POSIX timer's
 * signal: an expiration at tick 1001 waits on the list of timers due, and
 * the one at 1002 joins it, until a holder of the big lock posts one signal
 * for both, whose delivery reports the second as its overrun. An expiration
 * still waiting is posted by timer_settime before its new setting starts,
 * and by timer_delete before the timer goes; exec drops it, since exec
 * discards the timers' signals.
 */
static void fire_later_rows(void)
{
    struct setting every = {0, NS_PER_TICK, 0, NS_PER_TICK};
    struct setting once = {0, 0, 0, NS_PER_TICK};
    struct k_siginfo taken;

    ptimer_start(SIGEV_SIGNAL);
    CHECK(arm(&timer, 0, &every) == 0, "a timer every tick arms");
    kernel_ticks = 1001;
    timerfd_tick(1001);
    CHECK(posts == 0 && timer.fire_queued && timer.fire_fresh == 1 && ptimer_posts_due(),
          "its expiration at tick 1001 waits for a holder of the big lock");
    kernel_ticks = 1002;
    timerfd_tick(1002);
    CHECK(posts == 0 && timer.fire_fresh == 2, "the one at tick 1002 joins it");
    ptimer_post_due();
    CHECK(posts == 1 && !timer.fire_queued && !ptimer_posts_due() && group.info[SIGALRM].uid == 0,
          "the holder posts one signal");
    group.pending = 0;
    taken = group.info[SIGALRM];
    ptimer_rearm(&group, SIGALRM, &taken);
    CHECK(taken.uid == 1 && timer.overrun == 1, "whose delivery counts the second as its overrun");

    kernel_ticks = 1003;
    timerfd_tick(1003);
    CHECK(posts == 1 && timer.fire_queued, "the expiration at tick 1003 waits");
    CHECK(arm(&timer, 0, &once) == 0 && posts == 2 && !timer.fire_queued,
          "timer_settime posts it before the new setting starts");
    group.pending = 0;
    kernel_ticks = 1004;
    timerfd_tick(1004);
    CHECK(posts == 2 && timer.fire_queued && !timer.armed, "the one-shot expiration waits");
    ptimer_free(&group.timers, 1);
    CHECK(posts == 3 && !ptimer_posts_due() && group.timers == NULL,
          "timer_delete posts it before the timer goes");

    ptimer_start(SIGEV_SIGNAL);
    CHECK(arm(&timer, 0, &once) == 0, "a one-shot timer arms again");
    kernel_ticks = 1001;
    timerfd_tick(1001);
    CHECK(timer.fire_queued, "its expiration waits");
    leader.tg = &group;
    ptimer_exec(&leader);
    leader.tg = NULL;
    CHECK(posts == 0 && !ptimer_posts_due() && group.timers == NULL,
          "exec drops it with the timer");
    model_phase_ns = NS_PER_TICK / 2;
}

/**
 * SIGEV_NONE's timers, which the ticks alone count: a one-shot timer of
 * 0.1 ms queried at 0.2 ms, before a tick, has no time left, as Linux
 * reports an expired one; a periodic one of 0.3 ms from 0.1 ms queried at
 * 0.5 ms reports 0.2 ms, its time moved past the clock. One of 0.1 ms
 * queried at 0.2999 ms, on a clock that moves 200 ns on at each reading,
 * reports the 100 ns from the reading it was moved on by to 0.3 ms, as
 * Linux's common_timer_get counts both from one `now`; a second reading,
 * at 0.3001 ms, would report no time left.
 */
static void none_rows(void)
{
    struct setting once = {0, 0, 0, 100000};
    struct setting every = {0, 300000, 0, 100000};
    struct setting tenth = {0, 100000, 0, 100000};
    struct setting now;

    ptimer_start(SIGEV_NONE);
    CHECK(arm(&timer, 0, &once) == 0 && wake_arms == 0, "a one-shot SIGEV_NONE timer arms");
    model_phase_ns = 200000;
    current_setting(&timer, &now);
    CHECK(now.vsec == 0 && now.vnsec == 0, "expired, it has no time left before the tick");

    ptimer_start(SIGEV_NONE);
    CHECK(arm(&timer, 0, &every) == 0, "a periodic SIGEV_NONE timer arms");
    model_phase_ns = 500000;
    current_setting(&timer, &now);
    CHECK(now.vsec == 0 && now.vnsec == 200000 && posts == 0,
          "queried at 0.5 ms it reports 0.2 ms, and posts nothing");

    ptimer_start(SIGEV_NONE);
    CHECK(arm(&timer, 0, &tenth) == 0, "a SIGEV_NONE timer every 0.1 ms arms");
    model_phase_ns = 299900;
    read_step_ns = 200;
    current_setting(&timer, &now);
    read_step_ns = 0;
    CHECK(now.vsec == 0 && now.vnsec == 100 && timer.end == 1000 * (int64_t)NS_PER_TICK + 300000,
          "queried at 0.2999 ms on a clock that moves on, it reports the 100 ns from its reading");
    group.timers = NULL;
    model_phase_ns = NS_PER_TICK / 2;
}

/**
 * A timer of two ticks whose signal the process ignores stays armed between
 * ticks, as Linux restarts such a timer at once when its interval is a tick
 * or more. Two timers of SIGALRM: the second's expiration while the first's
 * signal is pending waits, and leaves it to the ticks, until that signal is
 * taken, which posts the second's signal and arms it between ticks again, as
 * Linux queues each timer's signal apart from another sender's. A one-shot
 * second timer's signal is posted then too, though the timer is disarmed.
 */
static void shared_rows(void)
{
    static struct ktimer second;
    struct setting two = {0, 2 * NS_PER_TICK, 0, 2 * NS_PER_TICK};
    struct setting first_at = {0, NS_PER_TICK, 0, 100000};
    struct setting second_at = {0, NS_PER_TICK, 0, 200000};
    struct setting first_once = {0, 0, 0, 100000};
    struct setting second_once = {0, 0, 0, 200000};
    struct k_siginfo taken;

    ptimer_start(SIGEV_SIGNAL);
    ignores = 1;
    CHECK(arm(&timer, 0, &two) == 0 && wake_arms == 1, "a timer of two ticks arms");
    kernel_ticks = 1002;
    timerfd_due();
    CHECK(posts == 1 && timer.between && wake_arms == 2 && wake_ns == 1004 * (int64_t)NS_PER_TICK,
          "its signal ignored, it is armed between ticks for its next time");
    ignores = 0;

    ptimer_start(SIGEV_SIGNAL);
    k_memset(&second, 0, sizeof(second));
    ptimer_model(&second, 2, SIGEV_SIGNAL);
    timer.in_group = &second;
    CHECK(arm(&timer, 0, &first_at) == 0 && arm(&second, 0, &second_at) == 0,
          "two timers of SIGALRM arm, at 0.1 and 0.2 ms");
    model_phase_ns = 100000;
    timerfd_due();
    model_phase_ns = 200000;
    timerfd_due();
    CHECK(posts == 1 && group.info[SIGALRM].pid == 1 && second.count == 1 && !second.between,
          "the second's expiration waits for the first's pending signal");
    group.pending = 0;
    taken = group.info[SIGALRM];
    wake_arms = 0;
    ptimer_rearm(&group, SIGALRM, &taken);
    CHECK(posts == 2 && group.info[SIGALRM].pid == 2 && group.info[SIGALRM].uid == 0 &&
              second.count == 0 && second.between && wake_arms == 2 &&
              wake_ns == 1001 * (int64_t)NS_PER_TICK + 200000,
          "the first's signal taken, the second's is posted and it is armed between ticks for "
          "its next time");
    disarm(&second);

    ptimer_start(SIGEV_SIGNAL);
    k_memset(&second, 0, sizeof(second));
    ptimer_model(&second, 2, SIGEV_SIGNAL);
    timer.in_group = &second;
    CHECK(arm(&timer, 0, &first_once) == 0 && arm(&second, 0, &second_once) == 0,
          "two one-shot timers of SIGALRM arm, at 0.1 and 0.2 ms");
    model_phase_ns = 100000;
    timerfd_due();
    model_phase_ns = 200000;
    timerfd_due();
    CHECK(posts == 1 && group.info[SIGALRM].pid == 1 && second.count == 1 && !second.armed,
          "the one-shot second's expiration waits for the first's pending signal, disarmed");
    group.pending = 0;
    taken = group.info[SIGALRM];
    ptimer_rearm(&group, SIGALRM, &taken);
    CHECK(posts == 2 && group.info[SIGALRM].pid == 2 && group.info[SIGALRM].uid == 0 &&
              second.count == 0 && timer.overrun == 0 && second.overrun == 0,
          "the first's signal taken, the one-shot second's is posted, and neither has an overrun");
    group.timers = NULL;
    model_phase_ns = NS_PER_TICK / 2;
}

/**
 * A timer's later expirations add only to a pending signal that bears its
 * key, as Linux adds only to the timer's own sigqueue (kernel/signal.c:
 * 2029-2036). A deleted one-shot timer's signal left pending, which a new
 * timer of the same id finds, and a user's signal with SI_TIMER and that
 * id, each leave the new timer's expiration waiting; the deleted timer's
 * signal taken forwards no timer and posts the waiting one's. A periodic
 * timer's signal left pending across timer_settime forwards nothing when
 * taken, as Linux's posixtimer_rearm compares si_sys_private with the
 * it_requeue_pending the setting moved on (kernel/time/posix-timers.c:267,
 * 879); without the generation it would count three intervals.
 */
static void key_rows(void)
{
    static struct ktimer later;
    struct setting once = {0, 0, 0, 100000};
    struct setting every = {0, 100000, 0, 100000};
    struct setting from_two = {0, 100000, 0, 200000};
    struct k_siginfo taken;

    ptimer_start(SIGEV_SIGNAL);
    CHECK(arm(&timer, 0, &once) == 0, "a one-shot timer of id 1 arms for 0.1 ms");
    model_phase_ns = 100000;
    timerfd_due();
    CHECK(posts == 1 && group.info[SIGALRM].timer_key == timer.key, "and posts its signal");
    ptimer_free(&group.timers, 1);
    CHECK(group.timers == NULL && group.timers_gone == NULL,
          "deleted, it leaves no record, since none of its expirations waits");
    k_memset(&later, 0, sizeof(later));
    ptimer_model(&later, 1, SIGEV_SIGNAL);
    group.timers = &later;
    CHECK(arm(&later, 0, &from_two) == 0 && later.key != timer.key,
          "a new timer of id 1 arms, every 0.1 ms from 0.3 ms");
    model_phase_ns = 300000;
    timerfd_due();
    CHECK(posts == 1 && group.info[SIGALRM].uid == 0 && later.count == 1,
          "its expiration adds nothing to the deleted timer's pending signal, and waits");
    group.pending = 0;
    taken = group.info[SIGALRM];
    model_phase_ns = 350000;
    ptimer_rearm(&group, SIGALRM, &taken);
    CHECK(taken.uid == 0 && later.overrun == 0 && posts == 2 && later.count == 0 &&
              group.info[SIGALRM].timer_key == later.key &&
              group.info[SIGALRM].timer_gen == later.gen,
          "the deleted timer's signal taken forwards nothing and posts the new one's");

    group.pending = KSIG_BIT(SIGALRM);
    k_memset(&group.info[SIGALRM], 0, sizeof(group.info[SIGALRM]));
    group.info[SIGALRM].code = SI_TIMER;
    group.info[SIGALRM].pid = 1;
    model_phase_ns = 400000;
    timerfd_due();
    CHECK(posts == 2 && group.info[SIGALRM].uid == 0 && later.count == 1,
          "nor does an expiration add to a user's signal of SI_TIMER and id 1");
    disarm(&later);

    ptimer_start(SIGEV_SIGNAL);
    CHECK(arm(&timer, 0, &every) == 0, "a timer every 0.1 ms arms");
    model_phase_ns = 100000;
    timerfd_due();
    CHECK(posts == 1, "and posts its signal at 0.1 ms");
    model_phase_ns = 150000;
    CHECK(arm(&timer, 0, &every) == 0 && timer.end == 1000 * (int64_t)NS_PER_TICK + 250000,
          "timer_settime at 0.15 ms sets it again, for 0.25 ms");
    model_phase_ns = 450000;
    group.pending = 0;
    taken = group.info[SIGALRM];
    ptimer_rearm(&group, SIGALRM, &taken);
    CHECK(taken.uid == 0 && timer.overrun == 0 && timer.end == 1000 * (int64_t)NS_PER_TICK + 250000,
          "the old setting's signal taken at 0.45 ms forwards nothing and counts no overrun");
    disarm(&timer);
    group.timers = NULL;
    model_phase_ns = NS_PER_TICK / 2;
}

/**
 * Expirations that wait for another sender's signal outlive timer_settime
 * and timer_delete, as Linux leaves the timer's sigqueue queued
 * (kernel/time/posix-timers.c:879, kernel/signal.c:1974-1980). A second
 * timer's expiration waiting on the first's signal, the second then set
 * again, is posted when that signal is taken, with the old setting, so that
 * the second's signal taken forwards nothing under the new one. Deleted
 * instead, the second's record is kept until the first's signal is taken,
 * which posts the second's and frees the record.
 */
static void waiting_rows(void)
{
    static struct ktimer second;
    struct setting first_at = {0, 0, 0, 100000};
    struct setting second_every = {0, 100000, 0, 200000};
    struct setting second_once = {0, 0, 0, 200000};
    struct setting tenth = {0, 100000, 0, 100000};
    struct k_siginfo taken;
    uint32_t old_gen;

    ptimer_start(SIGEV_SIGNAL);
    k_memset(&second, 0, sizeof(second));
    ptimer_model(&second, 2, SIGEV_SIGNAL);
    timer.in_group = &second;
    CHECK(arm(&timer, 0, &first_at) == 0 && arm(&second, 0, &second_every) == 0,
          "a one-shot timer of SIGALRM arms at 0.1 ms, and a second every 0.1 ms from 0.2 ms");
    model_phase_ns = 100000;
    timerfd_due();
    model_phase_ns = 200000;
    timerfd_due();
    CHECK(posts == 1 && second.count == 1, "the second's expiration waits for the first's signal");
    old_gen = second.gen;
    model_phase_ns = 250000;
    CHECK(arm(&second, 0, &tenth) == 0 && second.count == 1 && second.gen != old_gen,
          "timer_settime at 0.25 ms keeps it waiting, under a new setting for 0.35 ms");
    group.pending = 0;
    taken = group.info[SIGALRM];
    ptimer_rearm(&group, SIGALRM, &taken);
    CHECK(posts == 2 && second.count == 0 && group.info[SIGALRM].timer_key == second.key &&
              group.info[SIGALRM].timer_gen == old_gen,
          "the first's signal taken, the second's is posted with the old setting");
    model_phase_ns = 650000;
    group.pending = 0;
    taken = group.info[SIGALRM];
    ptimer_rearm(&group, SIGALRM, &taken);
    CHECK(taken.uid == 0 && second.overrun == 0 &&
              second.end == 1000 * (int64_t)NS_PER_TICK + 350000,
          "and that signal taken at 0.65 ms forwards nothing under the new setting");
    disarm(&second);

    ptimer_start(SIGEV_SIGNAL);
    k_memset(&second, 0, sizeof(second));
    ptimer_model(&second, 2, SIGEV_SIGNAL);
    timer.in_group = &second;
    CHECK(arm(&timer, 0, &first_at) == 0 && arm(&second, 0, &second_once) == 0,
          "two one-shot timers of SIGALRM arm, at 0.1 and 0.2 ms");
    model_phase_ns = 100000;
    timerfd_due();
    model_phase_ns = 200000;
    timerfd_due();
    CHECK(posts == 1 && second.count == 1, "the second's expiration waits for the first's signal");
    ptimer_free(&timer.in_group, 1);
    CHECK(timer.in_group == NULL && group.timers_gone == &second,
          "timer_delete keeps the second's record while its expiration waits");
    group.pending = 0;
    taken = group.info[SIGALRM];
    ptimer_rearm(&group, SIGALRM, &taken);
    CHECK(posts == 2 && group.info[SIGALRM].timer_key == second.key &&
              group.info[SIGALRM].pid == 2 && group.timers_gone == NULL,
          "the first's signal taken, the deleted second's is posted and its record let go");
    group.timers = NULL;
    model_phase_ns = NS_PER_TICK / 2;
}

/** Set a user's signal `signo` pending for the rows' process, with SI_QUEUE,
 * as sigqueue sends it. */
static void user_signal(int signo)
{
    group.pending |= KSIG_BIT(signo);
    k_memset(&group.info[signo], 0, sizeof(group.info[signo]));
    group.info[signo].code = SI_QUEUE;
    group.info[signo].pid = 99;
}

/** Take the rows' process's pending signal `signo`, as `sig_dequeue` does;
 * its siginfo as taken. */
static struct k_siginfo take_signal(int signo)
{
    struct k_siginfo taken;

    group.pending &= ~KSIG_BIT(signo);
    taken = group.info[signo];
    ptimer_rearm(&group, signo, &taken);
    return taken;
}

/**
 * A periodic timer's expirations after its signal is queued are its
 * setting's, as Linux's it_overrun, and only a delivery under that setting
 * adds them to the signal's overrun. A timer of a tick whose expiration at
 * 1001 waits for a user's signal, and which expires again at 1002, is
 * disarmed, or deleted, at 1002.5: once the user's signal is taken, its own
 * is posted with an overrun of 0, as Linux queued it with 0, stopped the
 * timer, and posix_timer_set_common resets it_overrun or the deletion keeps
 * the timer from being forwarded (kernel/signal.c:589-593, 2044,
 * kernel/time/posix-timers.c:881-882); counting the expiration at 1002 in
 * the notification made it 1. So does a timer whose own signal is pending
 * from 1001.
 */
static void elapsed_rows(void)
{
    struct setting every = {0, NS_PER_TICK, 0, NS_PER_TICK};
    struct setting off = {0, 0, 0, 0};
    struct k_siginfo taken;
    int how;

    for (how = 0; how < 3; how++) {
        ptimer_start(SIGEV_SIGNAL);
        if (how != 2) {
            user_signal(SIGALRM);
        }
        CHECK(arm(&timer, 0, &every) == 0, "a timer every tick arms at tick 1000");
        tick(1001);
        tick(1002);
        CHECK(timer.set_count == 2 && (how == 2 ? posts == 1 : timer.count == 1),
              how == 2 ? "it posts its signal at 1001 and counts 1002 in its setting"
                       : "its notification waits at 1001 and its setting counts 1002");
        model_phase_ns = NS_PER_TICK / 2;
        if (how == 1) {
            ptimer_free(&group.timers, 1);
            CHECK(group.timers_gone == &timer, "deleted at 1002.5, it keeps its record");
        } else {
            CHECK(arm(&timer, 0, &off) == 0 && !timer.armed && timer.set_count == 0,
                  "disarmed at 1002.5, its setting's count goes");
        }
        if (how != 2) {
            taken = take_signal(SIGALRM);
            CHECK(taken.code == SI_QUEUE && posts == 1 &&
                      group.info[SIGALRM].timer_key == timer.key && group.info[SIGALRM].uid == 0 &&
                      group.timers_gone == NULL,
                  "the user's signal taken, the timer's is posted with an overrun of 0");
        }
        taken = take_signal(SIGALRM);
        CHECK(taken.uid == 0 && timer.overrun == 0,
              how == 0   ? "and taken with 0, after the disarming"
              : how == 1 ? "and taken with 0, after the deletion"
                         : "its pending signal taken after the disarming has an overrun of 0");
    }
    group.timers = NULL;
    model_phase_ns = NS_PER_TICK / 2;
}

/**
 * Notifications that wait for a signal are posted in the order they began
 * to wait, as Linux's collect_signal takes the first sigqueue of the number
 * queued (kernel/signal.c:566-582), so that later expirations cannot keep
 * overtaking one: timer 2's, which waited from 0.1 ms, before timer 1's,
 * from 0.2 ms, though timer 1 is first on the process's list; and a deleted
 * timer's that waited first before a live one's, which today's order of
 * live timers first posted ahead of it.
 */
static void order_rows(void)
{
    static struct ktimer second;
    struct setting at_one = {0, 0, 0, 100000};
    struct setting at_two = {0, 0, 0, 200000};
    int deleted;

    for (deleted = 0; deleted <= 1; deleted++) {
        ptimer_start(SIGEV_SIGNAL);
        k_memset(&second, 0, sizeof(second));
        ptimer_model(&second, 2, SIGEV_SIGNAL);
        timer.in_group = &second;
        user_signal(SIGALRM);
        CHECK(arm(&timer, 0, &at_two) == 0 && arm(&second, 0, &at_one) == 0,
              "timers 1 and 2 of SIGALRM arm, at 0.2 and 0.1 ms");
        model_phase_ns = 100000;
        timerfd_due();
        model_phase_ns = 200000;
        timerfd_due();
        CHECK(timer.count == 1 && second.count == 1 && posts == 0,
              "both notifications wait for a user's signal");
        if (deleted) {
            ptimer_free(&timer.in_group, 1);
            CHECK(group.timers_gone == &second, "timer 2, deleted, keeps its record");
        }
        (void)take_signal(SIGALRM);
        CHECK(posts == 1 && group.info[SIGALRM].pid == 2 && timer.count == 1,
              deleted ? "the user's signal taken, deleted timer 2's, which waited first, is posted"
                      : "the user's signal taken, timer 2's, which waited first, is posted");
        (void)take_signal(SIGALRM);
        CHECK(posts == 2 && group.info[SIGALRM].pid == 1 && timer.count == 0 &&
                  group.timers_gone == NULL,
              "and once that is taken, timer 1's");
    }
    group.timers = NULL;
    model_phase_ns = NS_PER_TICK / 2;
}

/**
 * A discarded signal takes its timers' waiting notifications, as Linux's
 * flush_sigqueue_mask takes every sigqueue of its number off the queues
 * (kernel/signal.c:782-798, 4249-4255). With a user's SIGALRM pending,
 * timer 1's and timer 2's notifications wait, and timer 2 is deleted;
 * SIG_IGN installed (`discard`, as `sig_discard` calls it) drops timer 1's,
 * frees timer 2's record, which then no longer counts against PTIMER_MAX,
 * and a later user's signal taken posts neither. A timer of SIGCONT, whose
 * post discards the stops (`sig_job_control`) while `timer_lock` is held,
 * drops a deleted SIGTSTP timer's notification without taking the lock
 * again, and the record, which counts for nothing, is freed at the next
 * dequeue.
 */
static void discard_rows(void)
{
    static struct ktimer second;
    struct setting at_one = {0, 0, 0, 100000};
    struct setting at_two = {0, 0, 0, 200000};

    ptimer_start(SIGEV_SIGNAL);
    k_memset(&second, 0, sizeof(second));
    ptimer_model(&second, 2, SIGEV_SIGNAL);
    timer.in_group = &second;
    user_signal(SIGALRM);
    CHECK(arm(&timer, 0, &at_one) == 0 && arm(&second, 0, &at_two) == 0,
          "timers 1 and 2 of SIGALRM arm, at 0.1 and 0.2 ms");
    model_phase_ns = 100000;
    timerfd_due();
    model_phase_ns = 200000;
    timerfd_due();
    ptimer_free(&timer.in_group, 1);
    CHECK(timer.count == 1 && group.timers_gone == &second && ptimer_count(&group) == 2,
          "both wait for a user's signal, timer 2 deleted with its record kept");
    discard(KSIG_BIT(SIGALRM));
    CHECK(timer.count == 0 && group.timers_gone == NULL && ptimer_count(&group) == 1,
          "SIG_IGN drops timer 1's notification and frees timer 2's record");
    user_signal(SIGALRM);
    (void)take_signal(SIGALRM);
    CHECK(posts == 0 && !(group.pending & KSIG_BIT(SIGALRM)),
          "a later user's signal taken posts neither notification");

    ptimer_start(SIGEV_SIGNAL);
    timer.signo = SIGCONT;
    k_memset(&second, 0, sizeof(second));
    ptimer_model(&second, 2, SIGEV_SIGNAL);
    second.signo = SIGTSTP;
    timer.in_group = &second;
    user_signal(SIGTSTP);
    CHECK(arm(&second, 0, &at_one) == 0 && arm(&timer, 0, &at_two) == 0,
          "a timer of SIGTSTP arms at 0.1 ms and one of SIGCONT at 0.2 ms");
    model_phase_ns = 100000;
    timerfd_due();
    ptimer_free(&timer.in_group, 1);
    CHECK(second.count == 1 && group.timers_gone == &second,
          "the SIGTSTP timer's notification waits for a user's SIGTSTP, the timer deleted");
    model_phase_ns = 200000;
    timerfd_due();
    CHECK(posts == 1 && (group.pending & KSIG_BIT(SIGCONT)) &&
              !(group.pending & KSIG_BIT(SIGTSTP)) && second.count == 0 &&
              group.timers_gone == &second && ptimer_count(&group) == 1,
          "the SIGCONT posted under the timers' lock drops it, its record counting for nothing");
    (void)take_signal(SIGCONT);
    CHECK(group.timers_gone == NULL && posts == 1, "the next dequeue frees the record");
    group.timers = NULL;
    model_phase_ns = NS_PER_TICK / 2;
}

/**
 * exec deletes every timer, deleted ones whose notifications wait among
 * them, and discards the timers' pending signals while it keeps another
 * sender's, as Linux's posixtimer_exec (fs/exec.c:1220-1229,
 * kernel/signal.c:483-516): timer 1's own SIGUSR1 pending, and deleted
 * timer 2's notification waiting for a user's SIGALRM. Before, exec kept
 * timer 2's record, whose signal the new image took after the user's.
 */
static void exec_rows(void)
{
    static struct ktimer second;
    struct setting at_one = {0, 0, 0, 100000};
    struct setting at_two = {0, 0, 0, 200000};

    ptimer_start(SIGEV_SIGNAL);
    timer.signo = SIGUSR1;
    k_memset(&second, 0, sizeof(second));
    ptimer_model(&second, 2, SIGEV_SIGNAL);
    timer.in_group = &second;
    user_signal(SIGALRM);
    CHECK(arm(&timer, 0, &at_one) == 0 && arm(&second, 0, &at_two) == 0,
          "a timer of SIGUSR1 arms at 0.1 ms and one of SIGALRM at 0.2 ms");
    model_phase_ns = 100000;
    timerfd_due();
    model_phase_ns = 200000;
    timerfd_due();
    ptimer_free(&timer.in_group, 1);
    CHECK(posts == 1 && (group.pending & KSIG_BIT(SIGUSR1)) && second.count == 1 &&
              group.timers_gone == &second,
          "the first's signal is pending and the deleted second's waits for a user's");
    leader.tg = &group;
    ptimer_exec(&leader);
    leader.tg = NULL;
    CHECK(group.timers == NULL && group.timers_gone == NULL && armed == NULL,
          "exec deletes the timer and the deleted one's record");
    CHECK(!(group.pending & KSIG_BIT(SIGUSR1)) && (group.pending & KSIG_BIT(SIGALRM)),
          "and discards the timer's pending signal, keeping the user's");
    (void)take_signal(SIGALRM);
    CHECK(posts == 1 && !(group.pending & KSIG_BIT(SIGALRM)),
          "the user's signal taken, no timer's follows");
    model_phase_ns = NS_PER_TICK / 2;
}

/**
 * A periodic timer counts from the time and interval it was given: armed at
 * 20 s for an absolute 10 s every second, its first tick counts one and
 * stops it, as Linux's timerfd_tmrproc does, and timerfd_gettime forwards it
 * to 21 s, counting the other ten as Linux's hrtimer_forward_now does; an
 * interval of a tick and a half from tick 1000, read at tick 1006, counts 4
 * expirations, at 1.5, 3, 4.5 and 6 ticks, where an interval rounded to two
 * ticks would count 3; and a periodic timer stopped at its expiry lets the
 * ticks after it pass uncounted until something reads it, and is due again
 * at the interval after the read.
 */
static void interval_rows(void)
{
    struct setting past = {1, 0, 10, 0};
    struct setting half = {0, 3 * NS_PER_TICK / 2, 0, 3 * NS_PER_TICK / 2};
    struct setting got;
    uint64_t n;

    reset();
    kernel_ticks = 20u * TICK_HZ;
    model_phase_ns = 0;
    CHECK(arm(&timer, TFD_TIMER_ABSTIME, &past) == 0, "a timer for 10 s every second arms at 20 s");
    tick(kernel_ticks);
    CHECK(timer.count == 1 && timer.expired, "its first tick counts one and stops it");
    current_setting(&timer, &got);
    CHECK(timer.count == 11, "timerfd_gettime counts every second from 10 s to 20 s");
    CHECK(got.vsec == 1 && got.vnsec == 0 && got.isec == 1 && got.insec == 0,
          "its next expiration is a second away, at 21 s");

    reset();
    model_phase_ns = 0;
    CHECK(arm(&timer, 0, &half) == 0, "a timer every tick and a half arms");
    current_setting(&timer, &got);
    CHECK(got.isec == 0 && got.insec == 3 * NS_PER_TICK / 2, "it reports its interval whole");
    for (n = 1001; n <= 1006; n++) {
        tick(n);
    }
    CHECK(timer.count == 1, "it stops at its first expiration");
    open_entry(1);
    k_memset(user, 0, 8);
    CHECK(timerfd_read(&entry, (char *)user, 8) == 8 && user[0] == 4,
          "a read at tick 1006 counts 4 expirations in 6 ticks");
    CHECK(timer.armed && timer.due == 1008u, "and starts it again for 7.5 ticks");
    model_phase_ns = NS_PER_TICK / 2;
}

/** A one-shot timer of a tick, expired at tick 1001: a nonblocking read and
 * a blocking one each get 8 bytes holding 1, without a wait, and the next
 * nonblocking read finds nothing, as Linux's read forwards only a timer with
 * both `expired` and `tintv` set and returns its `ticks` (fs/timerfd.c:
 * 292-313). */
static void oneshot_rows(void)
{
    struct setting one = {0, 0, 0, NS_PER_TICK};
    int nonblock;

    for (nonblock = 1; nonblock >= 0; nonblock--) {
        reset();
        model_phase_ns = 0;
        open_entry(nonblock);
        CHECK(arm(&timer, 0, &one) == 0, "a one-shot timer of a tick arms");
        tick(1001);
        CHECK(timer.count == 1 && timer.expired && !timer.armed, "it expires at its tick");
        user[0] = 0;
        waits = 0;
        CHECK(timerfd_read(&entry, (char *)user, 8) == 8 && user[0] == 1 && waits == 0,
              nonblock ? "a nonblocking read of it gets 1" : "a blocking read of it gets 1");
        CHECK(!timer.expired && timer.count == 0, "the read takes the expiration");
        open_entry(1);
        CHECK(timerfd_read(&entry, (char *)user, 8) == -EAGAIN, "the next read finds nothing");
    }
    model_phase_ns = NS_PER_TICK / 2;
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
    CHECK(got.vsec == 17280000 && got.vnsec == 0, "a timer of 200 days reports 200 days");
    tick(1000u + 17280000ull * TICK_HZ);
    CHECK(timer.count == 0 && timer.armed, "a timer of 200 days does not expire early");
    tick(1001u + 17280000ull * TICK_HZ);
    CHECK(timer.count == 1 && !timer.armed, "a timer of 200 days expires at its tick");

    reset();
    model_phase_ns = 0;
    CHECK(arm(&timer, 0, &every) == 0, "a timer every 200 days arms as its tick begins");
    tick(kernel_ticks + TICK_HZ);
    CHECK(timer.count == 1, "its first expiration comes after a second");
    current_setting(&timer, &got);
    CHECK(got.vsec == 17280000 && got.vnsec == 0 && got.isec == 17280000 && got.insec == 0,
          "its next expiration is 200 days away, and its interval 200 days");
    model_phase_ns = NS_PER_TICK / 2;

    reset();
    CHECK(arm(&timer, 0, &forever) == 0, "a timer past the most seconds a setting keeps arms");
    current_setting(&timer, &got);
    CHECK(got.vsec == (int64_t)TIME_MAX, "it is taken as the most seconds a setting keeps");
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
    model_phase_ns = NS_PER_TICK / 2;
    pending_rows();
    phase_rows();
    lag_rows();
    between_rows();
    between_pending_rows();
    deferred_rows();
    fire_later_rows();
    none_rows();
    shared_rows();
    key_rows();
    waiting_rows();
    elapsed_rows();
    order_rows();
    discard_rows();
    exec_rows();
    ptimer_rows();
    interval_rows();
    oneshot_rows();
    long_rows();
    step_rows();
    cancel_read_rows();
    slew_rows();
    cancel_periodic_rows();
    cancel_setting_rows();
    reading_rows();
    CHECK(masked == 0, "every interrupt mask is taken back");

    printf("timerfd-hosttest: result: %s: %d failures\n", failures ? "fail" : "pass", failures);
    return failures ? 1 : 0;
}
