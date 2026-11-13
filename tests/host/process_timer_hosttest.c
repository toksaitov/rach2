/**
 * Check the timer tick's part of `proc/process.c`: `proc_wake_sleepers`,
 * the signals of the tick it leaves for a holder of the big lock
 * (`proc_tick_signal`, `proc_tick_signals_post`, `proc_tick_signals_own`),
 * `proc_alarm_set`, `proc_alarm_taken`, `proc_alarm_due_tick`,
 * `proc_alarm_at`, and `proc_wake_due`, the interrupt between ticks' part.
 *
 * host-deps: rach2/sys/lib/kstring.c rach2/sys/lib/kctypes.c
 * host-deps: tests/host/compat/spinlock_host.c
 * host-cflags: -Wno-int-to-pointer-cast -Wno-pointer-to-int-cast
 *
 * The functions `process.c` exports that the tick does not use are made
 * private before the source is included, so at -O1, which the host suite and
 * the scripts that build this test use, the native compiler discards them and
 * compiles only the functions under test; the globals stay external.
 * `signal_post` is a model that counts each signal sent to each process and,
 * unlike the kernel's, does not wake a blocked one; the run queues are a
 * model whose lock counts its takes and whose wake makes a blocked process
 * ready.
 *
 * The list has a blocked process ahead of a running one, each with a timer.
 * At its tick a due timer is marked, not posted: the pass runs without the
 * big lock, and a later pass marks it no second time. The post, under the
 * lock, sends SIGALRM to its process, blocked or running, and stops the
 * timer, which keeps its interval and posts nothing more until a SIGALRM is
 * taken; the take runs it again at the first tick of its interval's grid
 * after then, counted from the tick it was due, an interval of one tick
 * included, and a second take moves nothing. A timer with no interval is
 * left disarmed; a timer not yet due is not marked. A marked or stopped
 * timer reports no due tick, so no time left, as Linux's timer is inactive
 * once it has sent SIGALRM until the signal is taken. A setting made
 * between the mark and the post posts the marked SIGALRM, once, as Linux
 * sends it at expiry. A thread's own return posts its SIGVTALRM and SIGPROF
 * and takes it off the list. A sleeper is made
 * runnable at its deadline tick and not before, under the run queues' lock,
 * a blocked process with no deadline is never made runnable by the tick,
 * and a running process is left as it is whatever its wakeup tick says. A
 * tick that wakes a sleeper still marks the sleeper's alarm and a later
 * process's. More sleepers due than one hold of the lock wakes are all
 * woken by one pass, which still marks a timer linked behind them. The
 * count is 64 bits wide: a timer armed again across 2^32 ticks is due at
 * 2^32, the tick before serves nothing due after it, and tick 2^32 + 5
 * marks that timer and wakes the sleeper, whose deadline it is.
 * `proc_alarm_at` gives a tick past 2^32 whole. A sleep to a time of real
 * time (`rt_steps`) waits while the clock's step count stays, and a setting
 * of the clock wakes it at the next tick, before its deadline, for it to
 * count its tick again; a sleep on time since boot stays asleep.
 *
 * A sleep with a time of the monotonic clock (`wakeup_ns`) has the interrupt
 * between ticks armed for it at every pass, however far the time, whatever
 * its deadline tick, which counts from a clock that runs ahead of the tick
 * count; that interrupt, or a tick whose clock has passed the time, makes it
 * ready before its deadline tick, under the run queues' lock, and an
 * interrupt that comes before the time arms it again. A sleeper the pass
 * collected that leaves its wait and starts another before the wake, as a
 * thread whose condition came true does without the run queues' lock,
 * keeps the new wait's deadline: the wake tests the record again.
 */
#include "lib/kint.h"
#include "compat/waketime_none.h"
struct process;
static int proc_in_syscall(void);
static void init_proc(void);
static void proc_reclaim_boot_pids(void);
static uint32_t process_forks_since_boot(void);
static uint32_t process_sleeps_cut_short(void);
static void process_count_sleep_cut_short(void);
static void process_count_fork(void);
static struct process *alloc_process(void);
static struct process *alloc_process_fork(const struct process *parent, int thread, int share_fs);
static void free_process(struct process *proc);
static struct process *get_process(uint32_t pid);
static void reparent_children(uint32_t dead_pid);
static void proc_set_comm(struct process *p, const char *path);
static void setup_process_stack(struct process *proc, void (*entry_point)(void));
struct tgroup;
static struct tgroup *tgroup_new(struct process *leader);
static void tgroup_put(struct tgroup *tg);
static void proc_cpu_time(const struct process *p, uint64_t *user, uint64_t *sys);
static void proc_exchange_pids(struct process *a, struct process *b);
static int proc_set_ruid(struct process *p, uint32_t uid);
static uint32_t proc_user_records(const struct process *p);
static uint32_t proc_work_count(unsigned int index);
static void proc_alarm_take(struct process *to, const struct process *from);
#include "proc/process.c"
#include "ns_first.h"

volatile uint64_t kernel_ticks;

extern int printf(const char *format, ...);
extern int dprintf(int fd, const char *format, ...);

/** The single-threaded timer model has no hardware interrupts. */
uint32_t kirq_save(void)
{
    return 0;
}

/** Restore the model's unchanged interrupt state. */
void kirq_restore(uint32_t flags)
{
    (void)flags;
}

static int failures;
static process_t sleeper, runner, follower;
static process_t crowd[20];
static int sleeper_posts, runner_posts, follower_posts, vtalrm_posts, prof_posts;
static int sched_takes;

#define CHECK(c, detail)                                          \
    do {                                                          \
        if (!(c)) {                                               \
            dprintf(2, "process-timer-test: fail: %s\n", detail); \
            failures++;                                           \
        }                                                         \
    } while (0)

/** The tick count as the post reads it. */
uint64_t tick_count(void)
{
    return kernel_ticks;
}

/** `n` divided by `d`, with the remainder in `*rem` when it is not NULL:
 * the host's division, which the kernel's `tick_div64` gives. */
uint64_t tick_div64(uint64_t n, uint64_t d, uint64_t *rem)
{
    if (rem) {
        *rem = n % d;
    }
    return n / d;
}

/** The model's run queues' lock: counted, one processor. */
uint32_t sched_lock_take(void)
{
    sched_takes++;
    return 0;
}

/** Let the model's run queues' lock go. */
void sched_lock_give(uint32_t flags)
{
    (void)flags;
}

/** The tick's wake, which tests the record again as the kernel's does under
 * the run queues' lock, makes a due sleeper ready and takes it off the timer
 * list, as `sched_ready_locked` does, and marks it woken; there is one
 * processor and no queue, so no interrupt is asked for. */
int sched_wake_due_locked(process_t *p, uint64_t now)
{
    if (p->state != PROC_BLOCKED || p->wakeup_tick == 0 || now < p->wakeup_tick) {
        return -1;
    }
    p->wakeup_tick = 0;
    p->woken = 1;
    p->state = PROC_READY;
    proc_timer_update(p);
    return -1;
}

/** The wake of a timed sleeper at its time or at a setting of the clock,
 * as the kernel's under the run queues' lock: ready, off the timer list and
 * marked woken, its deadline cleared. */
int sched_wake_time_locked(process_t *p)
{
    if (p->state != PROC_BLOCKED || p->wakeup_tick == 0) {
        return -1;
    }
    p->wakeup_tick = 0;
    p->woken = 1;
    p->state = PROC_READY;
    proc_timer_update(p);
    return -1;
}

/* The model's monotonic clock, in nanoseconds since boot, and the time the
 * interrupt between ticks was last armed for, with how often it was. */
static int64_t clock_ns;
static int64_t wake_ns;
static int wake_arms;

/* A record the next arming moves to a new wait, at this tick and time,
 * as its thread would that left its wait while the pass walked the list. */
static process_t *restart;
static uint64_t restart_tick;
static int64_t restart_ns;

/** The model's monotonic clock. */
int64_t rtc_monotonic_ns(void)
{
    return clock_ns;
}

/** Note the time the interrupt between ticks is armed for. */
void timer_wake_at(int64_t ns)
{
    wake_ns = ns;
    wake_arms++;
    if (restart) {
        restart->wakeup_tick = restart_tick;
        restart->wakeup_ns = restart_ns;
        restart = NULL;
    }
}

/* The real-time clock's step count, which the step row moves. */
static uint32_t step_count = 1;

/** The model's step count of the real-time clock (`rtc_step_count`). */
uint32_t rtc_step_count(void)
{
    return step_count;
}

/** No processor to interrupt in the model. */
void sched_kick_cpus(uint64_t cpus)
{
    (void)cpus;
}

/** The model of `signal_post`: count each signal, for each process. */
void signal_post(process_t *target, int signo)
{
    if (signo == SIGALRM && target == &sleeper) {
        sleeper_posts++;
    }
    if (signo == SIGALRM && target == &follower) {
        follower_posts++;
    }
    if (signo == SIGALRM && target == &runner) {
        runner_posts++;
    }
    if (signo == SIGVTALRM) {
        vtalrm_posts++;
    }
    if (signo == SIGPROF) {
        prof_posts++;
    }
}

/** A tick at `now`: the pass without the big lock, and the post under it. */
static void tick_at(uint64_t now)
{
    kernel_ticks = now;
    proc_wake_sleepers(now);
    proc_tick_signals_post();
}

/** A due real-time timer is marked by the pass and posted by a holder,
 * which stops it. */
static void alarm_rows(void)
{
    proc_list = &sleeper;
    sleeper.next = &runner;
    sleeper.state = PROC_BLOCKED;
    sleeper.wakeup_tick = 1200;
    runner.state = PROC_RUNNING;
    proc_alarm_set(&sleeper, 1004, 10);
    proc_alarm_set(&runner, 1004, 25);
    tick_at(1003);
    CHECK(sleeper_posts == 0 && runner_posts == 0 && runner.alarm_tick == 1004 &&
              !proc_tick_signals_due(),
          "a timer not yet due is not marked");
    CHECK(proc_alarm_due_tick(&runner) == 1004, "an armed timer reports its due tick");
    kernel_ticks = 1004;
    proc_wake_sleepers(1004);
    CHECK(runner_posts == 0 && sleeper_posts == 0 && runner.alarm_due && sleeper.alarm_due &&
              (runner.tick_sigs & (1u << SIGALRM)) && proc_tick_signals_due(),
          "a due timer is marked for a holder of the big lock, not posted");
    CHECK(runner.alarm_tick == 1004 && sleeper.alarm_tick == 1004,
          "and is not armed again before its post");
    CHECK(proc_alarm_due_tick(&runner) == 0,
          "a marked timer reports no time left, as Linux's inactive timer does");
    CHECK(sched_takes > 0, "the pass takes the run queues' lock");
    kernel_ticks = 1005;
    proc_wake_sleepers(1005);
    proc_tick_signals_post();
    CHECK(runner_posts == 1, "the post sends SIGALRM to a process that is running");
    CHECK(sleeper_posts == 1, "and to one that is asleep, once though two passes saw it due");
    CHECK(runner.alarm_tick == 0 && runner.alarm_interval == 25 && runner.alarm_last == 1004 &&
              sleeper.alarm_tick == 0 && sleeper.alarm_interval == 10 && !runner.alarm_due &&
              !runner.timer_linked && !proc_tick_signals_due(),
          "and then stops each, keeping its interval and the tick it was due");
    CHECK(proc_alarm_due_tick(&runner) == 0,
          "a stopped timer reports no time left, as Linux's does until its SIGALRM is taken");
}

/** A stopped timer runs again on its grid when its SIGALRM is taken, a
 * single alarm stays disarmed, and a setting between the mark and the post
 * posts the mark; after `alarm_rows`. */
static void alarm_take_rows(void)
{
    tick_at(1040);
    CHECK(runner_posts == 1 && sleeper_posts == 1,
          "a stopped timer posts nothing more while its SIGALRM is not taken");
    proc_alarm_taken(&runner);
    proc_alarm_taken(&sleeper);
    CHECK(runner.alarm_tick == 1054 && sleeper.alarm_tick == 1044 && runner.timer_linked,
          "a SIGALRM taken runs each again at the first tick of its grid after then");
    proc_alarm_taken(&runner);
    CHECK(runner.alarm_tick == 1054, "and a second taken while it runs moves nothing");

    proc_alarm_set(&runner, 1100, 0);
    proc_alarm_set(&sleeper, 0, 0);
    tick_at(1100);
    CHECK(runner_posts == 2 && runner.alarm_tick == 0 && !runner.timer_linked,
          "a single alarm posts once and leaves the timer disarmed");
    tick_at(1101);
    CHECK(runner_posts == 2, "and posts nothing after");

    proc_alarm_set(&runner, 1110, 3);
    kernel_ticks = 1110;
    proc_wake_sleepers(1110);
    kernel_ticks = 1117;
    proc_tick_signals_post();
    proc_alarm_taken(&runner);
    CHECK(runner_posts == 3 && runner.alarm_tick == 1119,
          "a SIGALRM taken seven ticks late runs the timer at the next tick of its grid");

    proc_alarm_set(&runner, 1130, 5);
    kernel_ticks = 1130;
    proc_wake_sleepers(1130);
    proc_alarm_set(&runner, 1150, 0);
    CHECK(runner_posts == 4 && runner.alarm_tick == 1150 && !runner.tick_sigs &&
              !runner.tick_post_linked,
          "a setting made between the mark and the post posts the marked SIGALRM");
    proc_tick_signals_post();
    CHECK(runner_posts == 4, "and the post sends no second");
    proc_alarm_set(&runner, 0, 0);
}

/** A thread's own return posts its signals of the CPU timers. */
static void own_rows(void)
{
    cpu_table[0].current = &runner;
    proc_tick_signal(&runner, SIGVTALRM);
    proc_tick_signal(&runner, SIGPROF);
    proc_tick_signal(&runner, SIGVTALRM);
    CHECK(runner.tick_post_linked && proc_tick_signals_due(),
          "a CPU timer's signal waits on the list");
    proc_tick_signals_own();
    CHECK(vtalrm_posts == 1 && prof_posts == 1 && !runner.tick_post_linked && !runner.tick_sigs &&
              !proc_tick_signals_due(),
          "the thread's own return posts each signal once and leaves the list");
    cpu_table[0].current = NULL;
}

/** Sleepers wake at their deadline, and only blocked ones. */
static void sleeper_rows(void)
{
    tick_at(1199);
    CHECK(sleeper.state == PROC_BLOCKED && sleeper.wakeup_tick == 1200,
          "a sleeper is not woken before its deadline");
    tick_at(1200);
    CHECK(sleeper.state == PROC_READY && sleeper.wakeup_tick == 0 && sleeper.woken,
          "and is made runnable and marked woken at its deadline tick");

    sleeper.state = PROC_BLOCKED;
    sleeper.woken = 0;
    sleeper.wakeup_tick = 0;
    proc_timer_update(&sleeper);
    tick_at(1300);
    CHECK(sleeper.state == PROC_BLOCKED, "a blocked process with no deadline is not made runnable");
    runner.wakeup_tick = 1300;
    proc_timer_update(&runner);
    tick_at(1300);
    CHECK(runner.state == PROC_RUNNING && runner.wakeup_tick == 1300,
          "a running process is left as it is whatever its wakeup tick says");
    runner.wakeup_tick = 0;

    sleeper.state = PROC_BLOCKED;
    sleeper.wakeup_tick = 1400;
    proc_alarm_set(&sleeper, 1400, 1);
    proc_alarm_set(&runner, 1400, 0);
    tick_at(1400);
    CHECK(sleeper.state == PROC_READY && sleeper_posts == 2 && runner_posts == 5,
          "a tick that wakes a sleeper still marks its alarm and a later process's");
    proc_alarm_taken(&sleeper);
    CHECK(sleeper.alarm_tick == 1401, "an interval of one tick runs again at the next tick");
}

/** Timers and sleepers across 2^32 ticks, and `proc_alarm_at`. */
static void wide_rows(void)
{
    sleeper.state = PROC_BLOCKED;
    sleeper.wakeup_tick = 0x100000005ull;
    proc_alarm_set(&sleeper, 0, 0);
    proc_alarm_set(&runner, 0xFFFFFFFEu, 2);
    tick_at(0xFFFFFFFEu);
    proc_alarm_taken(&runner);
    CHECK(runner_posts == 6 && runner.alarm_tick == 0x100000000ull,
          "a timer armed again across 2^32 ticks is due at 2^32");
    tick_at(0xFFFFFFFFu);
    CHECK(runner_posts == 6 && sleeper.state == PROC_BLOCKED,
          "a timer and a sleeper due past 2^32 ticks wait for them");
    tick_at(0x100000005ull);
    CHECK(runner_posts == 7 && sleeper.state == PROC_READY, "and are served after");

    CHECK(proc_alarm_at(1000, 25) == 1025, "an alarm 25 ticks after tick 1000 is due at 1025");
    CHECK(proc_alarm_at(0xFFFFFFFFu, 1) == 0x100000000ull, "an alarm past 2^32 ticks is due whole");
}

/** More sleepers due than one hold of the run queues' lock wakes. */
static void crowd_rows(void)
{
    int i, woken;

    proc_alarm_set(&runner, 0, 0);
    /* linked first, so the twenty put at the list's head come before it */
    follower.state = PROC_RUNNING;
    proc_alarm_set(&follower, 1600, 0);
    for (i = 0; i < 20; i++) {
        crowd[i].state = PROC_BLOCKED;
        crowd[i].wakeup_tick = 1600;
        proc_timer_update(&crowd[i]);
    }
    tick_at(1600);
    for (i = 0, woken = 0; i < 20; i++) {
        woken += crowd[i].state == PROC_READY && !crowd[i].timer_linked;
    }
    CHECK(woken == 20, "more sleepers due than one hold wakes are all woken by one pass");
    CHECK(follower_posts == 1 && !follower.timer_linked,
          "and the pass still reaches the timer after them");
}

/** A sleep to a time of real time waits while the clock's step count stays,
 * and a setting of the clock wakes it at the next tick, before its deadline;
 * a sleep on time since boot stays asleep. */
static void step_rows(void)
{
    proc_alarm_set(&follower, 0, 0);
    proc_alarm_set(&sleeper, 0, 0);
    proc_alarm_set(&runner, 0, 0);
    sleeper.state = PROC_BLOCKED;
    sleeper.wakeup_tick = 9000;
    sleeper.rt_steps = 1;
    runner.state = PROC_BLOCKED;
    runner.wakeup_tick = 9000;
    proc_timer_update(&sleeper);
    proc_timer_update(&runner);
    tick_at(2000);
    CHECK(sleeper.state == PROC_BLOCKED && sleeper.wakeup_tick == 9000,
          "a sleep to a time of real time waits while the clock is not set");
    step_count = 2;
    tick_at(2001);
    CHECK(sleeper.state == PROC_READY && sleeper.wakeup_tick == 0 && !sleeper.timer_linked,
          "a setting of the clock wakes it at the next tick, before its deadline");
    CHECK(runner.state == PROC_BLOCKED && runner.wakeup_tick == 9000 && runner.timer_linked,
          "and leaves a sleep on time since boot asleep");
    sleeper.rt_steps = 0;
}

/** A sleep with a time between ticks: armed at every pass, however far,
 * woken by the interrupt between ticks once the clock reaches it, or by a
 * tick whose clock has passed it, before its deadline tick. */
static void between_rows(void)
{
    int64_t t = 3001 * (int64_t)TICK_NS + (int64_t)TICK_NS / 4;

    sleeper.state = PROC_BLOCKED;
    sleeper.wakeup_tick = 3003;
    sleeper.wakeup_ns = t;
    runner.state = PROC_RUNNING;
    runner.wakeup_tick = 0;
    proc_timer_update(&sleeper);
    proc_timer_update(&runner);
    wake_arms = 0;
    clock_ns = 2998 * (int64_t)TICK_NS + (int64_t)TICK_NS / 2;
    tick_at(2998);
    CHECK(sleeper.state == PROC_BLOCKED && wake_arms == 1 && wake_ns == t,
          "a time more than two ticks off is armed between ticks");
    clock_ns = 2999 * (int64_t)TICK_NS + (int64_t)TICK_NS / 2;
    tick_at(2999);
    CHECK(sleeper.state == PROC_BLOCKED && wake_arms == 2 && wake_ns == t,
          "and armed again by the next tick, its deadline tick four off");
    clock_ns = t - 1000;
    proc_wake_due();
    CHECK(sleeper.state == PROC_BLOCKED && wake_arms == 3 && wake_ns == t,
          "an interrupt before the time arms it again");
    clock_ns = t;
    proc_wake_due();
    CHECK(sleeper.state == PROC_READY && sleeper.wakeup_tick == 0 && !sleeper.timer_linked &&
              sleeper.woken,
          "the interrupt at the time makes it ready and woken, before its deadline tick");

    sleeper.state = PROC_BLOCKED;
    sleeper.wakeup_tick = 3003;
    proc_timer_update(&sleeper);
    clock_ns = t + 1000;
    tick_at(3001);
    CHECK(sleeper.state == PROC_READY && sleeper.wakeup_tick == 0,
          "a tick whose clock has passed the time makes it ready too");
    sleeper.wakeup_ns = 0;
}

/** A sleeper the pass collected that leaves its wait and starts another
 * before the wake keeps the new wait's deadline: the list's head is
 * collected, and the arming for the record behind it moves the head to a
 * later wait before the wake. */
static void restart_rows(void)
{
    int64_t t = 4001 * (int64_t)TICK_NS + (int64_t)TICK_NS / 4;
    int64_t later = 4099 * (int64_t)TICK_NS + (int64_t)TICK_NS / 4;

    /* linked first, so the sleeper put at the list's head comes before it */
    runner.state = PROC_BLOCKED;
    runner.wakeup_tick = 5000;
    runner.wakeup_ns = 4999 * (int64_t)TICK_NS;
    proc_timer_update(&runner);
    sleeper.state = PROC_BLOCKED;
    sleeper.woken = 0;
    sleeper.wakeup_tick = 4003;
    sleeper.wakeup_ns = t;
    proc_timer_update(&sleeper);
    restart = &sleeper;
    restart_tick = 4101;
    restart_ns = later;
    clock_ns = t + 1000;
    proc_wake_due();
    CHECK(restart == NULL, "the pass arms the record behind the one it collected");
    CHECK(sleeper.state == PROC_BLOCKED && sleeper.wakeup_tick == 4101 &&
              sleeper.wakeup_ns == later && sleeper.timer_linked && !sleeper.woken,
          "a sleeper collected that starts a new wait before the wake keeps its deadline");
    clock_ns = later;
    proc_wake_due();
    CHECK(sleeper.state == PROC_READY && sleeper.wakeup_tick == 0 && sleeper.woken,
          "and the new wait ends at its own time");
    sleeper.wakeup_ns = 0;
    runner.state = PROC_RUNNING;
    runner.wakeup_tick = 0;
    runner.wakeup_ns = 0;
    proc_timer_update(&runner);
}

/** A remote pass expires a due sleep but rearms no other CPU's future event. */
static void event_owner_rows(void)
{
    sleeper.state = PROC_BLOCKED;
    sleeper.wakeup_tick = 100;
    sleeper.wakeup_ns = 10000000;
    sleeper.wakeup_cpu = 1;
    proc_timer_update(&sleeper);
    wake_arms = 0;
    clock_ns = 9000000;
    proc_wake_due();
    CHECK(wake_arms == 0 && sleeper.state == PROC_BLOCKED,
          "a remote pass does not copy a future event onto this processor");
    sleeper.wakeup_cpu = 0;
    proc_wake_due();
    CHECK(wake_arms == 1 && wake_ns == sleeper.wakeup_ns,
          "the owning processor rearms its pending event");
    sleeper.wakeup_cpu = 1;
    clock_ns = sleeper.wakeup_ns;
    proc_wake_due();
    CHECK(sleeper.state == PROC_READY && !sleeper.timer_linked,
          "a remote processor can still expire the sleep once its time comes");
}

/** Fine alarms expire and retain their grid without advancing a coarse tick. */
static void fine_alarm_rows(void)
{
    uint64_t interval;

    runner.state = PROC_RUNNING;
    sleeper.state = PROC_RUNNING;
    clock_ns = 10000000;
    proc_alarm_set_ns(&runner, 500000, 500000);
    CHECK(runner.alarm_fine && wake_ns == 10500000, "a fine alarm arms its own event");
    clock_ns = 10200000;
    CHECK(proc_alarm_read_ns(&runner, &interval) == 300000 && interval == 500000,
          "a fine alarm reports sub-tick time and interval");
    clock_ns = 10499999;
    proc_wake_due();
    CHECK(!runner.alarm_due && runner_posts == 0, "a fine alarm never expires early");
    clock_ns = 10500000;
    CHECK(proc_alarm_read_ns(&runner, &interval) == 1000,
          "a callback still pending reports one microsecond as Linux does");
    proc_wake_due();
    CHECK(proc_alarm_read_ns(&runner, &interval) == 0,
          "an expiry already collected reports zero time left");
    CHECK(runner.alarm_due && kernel_ticks == 0, "its expiry needs no coarse tick");
    proc_tick_signals_post();
    CHECK(runner_posts == 1 && runner.alarm_tick == 0 && runner.alarm_interval == 500000,
          "posting stops a fine periodic alarm until its signal is taken");
    clock_ns = 12200000;
    proc_alarm_taken(&runner);
    CHECK(runner.alarm_tick == 12500000 && wake_ns == 12500000,
          "late signal consumption preserves the half-millisecond grid");
    proc_alarm_take(&sleeper, &runner);
    CHECK(sleeper.alarm_fine && sleeper.alarm_tick == runner.alarm_tick &&
              sleeper.alarm_interval == runner.alarm_interval,
          "exec transfers a fine timer with its units");
    proc_alarm_set(&runner, 0, 0);
    proc_alarm_set(&sleeper, 0, 0);
    runner_posts = 0;
    clock_ns = 0;
}

/** Run every check; 0 when each passes, 1 otherwise. */
int main(int argc, char **argv)
{
    if (argc == 2 && __builtin_strcmp(argv[1], "--fine-alarm") == 0) {
        fine_alarm_rows();
        printf("process-timer-test: fine alarm: %s\n", failures ? "fail" : "pass");
        return failures != 0;
    }
    if (argc == 2 && __builtin_strcmp(argv[1], "--event-owner") == 0) {
        event_owner_rows();
        printf("process-timer-test: event owner: %s\n", failures ? "fail" : "pass");
        return failures != 0;
    }
    fine_alarm_rows();
    alarm_rows();
    alarm_take_rows();
    own_rows();
    sleeper_rows();
    wide_rows();
    crowd_rows();
    step_rows();
    between_rows();
    restart_rows();
    event_owner_rows();
    if (failures) {
        return 1;
    }
    printf("process-timer-test: pass: the tick's timers and sleepers\n");
    return 0;
}

/** Whole-tick arming is checked by the timer's model; these rows check the
 * timer list's expiry and the wakes it selects. */
void timer_wake_tick(uint64_t tick)
{
    (void)tick;
}
