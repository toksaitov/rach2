/**
 * Check the timer tick's part of `proc/process.c`: `proc_wake_sleepers` and
 * `proc_alarm_at`.
 *
 * host-deps: rach2/sys/lib/kstring.c rach2/sys/lib/kctypes.c
 * host-cflags: -Wno-int-to-pointer-cast -Wno-pointer-to-int-cast
 *
 * The functions `process.c` exports that the tick does not use are made
 * private before the source is included, so at -O1, which the host suite and
 * the scripts that build this test use, the native compiler discards them and
 * compiles only the two functions under test; the globals stay external.
 * `signal_post` is a model that counts each SIGALRM sent to each process and,
 * unlike the kernel's, does not wake a blocked one.
 *
 * The list has a blocked process ahead of a running one, each with a timer.
 * At its tick a due timer posts SIGALRM to its process, blocked or running,
 * and is armed again for its interval from that tick, an interval of one tick
 * included, or left disarmed when it has no interval; a timer not yet due
 * posts nothing. A sleeper is made runnable at its deadline tick and not
 * before, a blocked process with no deadline is never made runnable by the
 * tick, and a running process is left as it is whatever its wakeup tick says.
 * A tick that wakes a sleeper still posts the sleeper's alarm and a later
 * process's. Across the count's wrap, a timer armed again onto tick 0 is due
 * at 1, the tick before 0 serves nothing due after it, and tick 5 posts that
 * timer and wakes the sleeper, whose deadline is 5.
 * `proc_alarm_at` moves a tick that wraps to 0, the value of a disarmed timer,
 * to 1.
 */
#include "lib/kint.h"
struct process;
static int proc_in_syscall(void);
static void init_proc(void);
static void proc_reclaim_boot_pids(void);
static uint32_t process_forks_since_boot(void);
static uint32_t process_sleeps_cut_short(void);
static void process_count_sleep_cut_short(void);
static void process_count_fork(void);
static struct process *alloc_process(void);
static void free_process(struct process *proc);
static struct process *get_process(uint32_t pid);
static void reparent_children(uint32_t dead_pid);
static void proc_set_comm(struct process *p, const char *path);
static void setup_process_stack(struct process *proc, void (*entry_point)(void));
struct tgroup;
static struct tgroup *tgroup_new(struct process *leader);
static void tgroup_put(struct tgroup *tg);
static void proc_cpu_time(const struct process *p, uint32_t *user, uint32_t *sys);
#include "proc/process.c"
#include "ns_first.h"

volatile uint32_t kernel_ticks;

extern int printf(const char *format, ...);
extern int dprintf(int fd, const char *format, ...);

static int failures;
static process_t sleeper, runner;
static int sleeper_posts, runner_posts;

#define CHECK(c, detail)                                          \
    do {                                                          \
        if (!(c)) {                                               \
            dprintf(2, "process-timer-test: fail: %s\n", detail); \
            failures++;                                           \
        }                                                         \
    } while (0)

/** A wake makes the process ready; there is one processor and no queue. */
void sched_make_ready(process_t *p)
{
    p->state = PROC_READY;
}

/** The model of `signal_post`: count each SIGALRM, for each process. */
void signal_post(process_t *target, int signo)
{
    if (signo == SIGALRM && target == &sleeper) {
        sleeper_posts++;
    }
    if (signo == SIGALRM && target == &runner) {
        runner_posts++;
    }
}

int main(void)
{
    proc_list = &sleeper;
    sleeper.next = &runner;
    sleeper.state = PROC_BLOCKED;
    sleeper.wakeup_tick = 1200;
    sleeper.alarm_tick = 1004;
    sleeper.alarm_interval = 10;
    runner.state = PROC_RUNNING;
    runner.alarm_tick = 1004;
    runner.alarm_interval = 25;
    proc_wake_sleepers(1003);
    CHECK(sleeper_posts == 0 && runner_posts == 0 && runner.alarm_tick == 1004,
          "a timer not yet due posts nothing");
    proc_wake_sleepers(1004);
    CHECK(runner_posts == 1, "a due timer posts SIGALRM to a process that is running");
    CHECK(sleeper_posts == 1, "and to one that is asleep");
    CHECK(runner.alarm_tick == 1029 && sleeper.alarm_tick == 1014,
          "and each is armed again for its interval from that tick");

    runner.alarm_tick = 1100;
    runner.alarm_interval = 0;
    sleeper.alarm_tick = 0;
    proc_wake_sleepers(1100);
    CHECK(runner_posts == 2 && runner.alarm_tick == 0,
          "a single alarm posts once and leaves the timer disarmed");
    proc_wake_sleepers(1101);
    CHECK(runner_posts == 2, "and posts nothing after");

    proc_wake_sleepers(1199);
    CHECK(sleeper.state == PROC_BLOCKED && sleeper.wakeup_tick == 1200,
          "a sleeper is not woken before its deadline");
    proc_wake_sleepers(1200);
    CHECK(sleeper.state == PROC_READY && sleeper.wakeup_tick == 0,
          "and is made runnable at its deadline tick");

    sleeper.state = PROC_BLOCKED;
    sleeper.wakeup_tick = 0;
    proc_wake_sleepers(1300);
    CHECK(sleeper.state == PROC_BLOCKED, "a blocked process with no deadline is not made runnable");
    runner.wakeup_tick = 1300;
    proc_wake_sleepers(1300);
    CHECK(runner.state == PROC_RUNNING && runner.wakeup_tick == 1300,
          "a running process is left as it is whatever its wakeup tick says");

    sleeper.state = PROC_BLOCKED;
    sleeper.wakeup_tick = 1400;
    sleeper.alarm_tick = 1400;
    sleeper.alarm_interval = 1;
    runner.alarm_tick = 1400;
    runner.alarm_interval = 0;
    proc_wake_sleepers(1400);
    CHECK(sleeper.state == PROC_READY && sleeper_posts == 2 && runner_posts == 3,
          "a tick that wakes a sleeper still posts its alarm and a later process's");
    CHECK(sleeper.alarm_tick == 1401, "an interval of one tick is armed again");

    sleeper.state = PROC_BLOCKED;
    sleeper.wakeup_tick = 5;
    sleeper.alarm_tick = 0;
    runner.alarm_tick = 0xFFFFFFFEu;
    runner.alarm_interval = 2;
    proc_wake_sleepers(0xFFFFFFFEu);
    CHECK(runner_posts == 4 && runner.alarm_tick == 1,
          "a timer armed again onto tick 0 across the wrap is due at 1");
    proc_wake_sleepers(0xFFFFFFFFu);
    CHECK(runner_posts == 4 && sleeper.state == PROC_BLOCKED,
          "a timer and a sleeper due past the wrap wait for it");
    proc_wake_sleepers(5);
    CHECK(runner_posts == 5 && sleeper.state == PROC_READY, "and are served after it");

    CHECK(proc_alarm_at(1000, 25) == 1025, "an alarm 25 ticks after tick 1000 is due at 1025");
    CHECK(proc_alarm_at(0xFFFFFFFFu, 1) == 1, "an alarm that wraps to 0 is due at 1");
    if (failures) {
        return 1;
    }
    printf("process-timer-test: pass: the tick's timers and sleepers\n");
    return 0;
}
