/**
 * Check the deadlines `poll`, `ppoll`, `select` and `pselect6` wait to.
 *
 * host-cflags: -Wno-int-to-pointer-cast -Wno-pointer-to-int-cast
 * host-deps: rach2/sys/tick.c
 *
 * The system call exports and the poll core's other exports are made private
 * before the source is included, so at -O1, which the host suite and the
 * scripts that build this test use, the native compiler removes every path
 * but the conversions and the comparison this test calls, and nothing else of
 * the kernel is linked. The wait itself, which asks `deadline_reached` and
 * `deadline_passed` at each wake, is not compiled.
 *
 * A wait ends at the first tick at or after its end, measured from where in
 * the tick in progress it began, so a timeout of three ticks begun as tick
 * 1000 starts ends at 1003: `poll`'s milliseconds, `ppoll`'s and `pselect6`'s
 * seconds and nanoseconds alike. A `ppoll` of 2.1 ticks rounds up to three;
 * a `ppoll` and a `select` of 1 s and a little more end at the tick after
 * 1000 + TICK_HZ, so seconds count TICK_HZ ticks and a part of a tick rounds
 * up; and a `poll` of 1 ms, a `select` of 1 us and a `ppoll` of 1 ns end at
 * 1001. Begun four tenths into the tick, a `ppoll` of a tenth of a tick ends
 * at 1001 and one of seven tenths at 1002, and a `poll` of a tick at 1002. A
 * zero timeout ends at once, a tick the timer has reached and not yet raised
 * notwithstanding, and a negative or null one has no deadline. The
 * longest `ppoll` timeout of the i686, begun nine tenths into a tick just
 * short of 2^32 ticks, ends 2^31 s and a tick later, past them, and is not
 * reached at the tick it began. A deadline is reached at its tick and not the
 * tick before.
 *
 * A wait also lasts until time since boot has advanced by its timeout. After
 * a lost tick the clock keeps a reading ahead of the count, here eight tenths
 * of a tick past tick 1000. A `poll` of two ticks from there has its deadline
 * tick at 1002, where the clock is short of its end, so the deadline moves to
 * 1003, where it has passed; a `ppoll` of a tick and a half does the same.
 * Without a lost tick, a timeout has passed at its deadline tick, and a
 * `ppoll` of 2^40 seconds, which the tick cuts, ends at its tick alone.
 */
#include "isr.h"
struct isr_params;
static long sys_poll(struct isr_params *params);
static long sys_ppoll(struct isr_params *params);
static long sys__newselect(struct isr_params *params);
static long sys_select(struct isr_params *params);
static long sys_pselect6(struct isr_params *params);
struct fd_entry;
struct wait_queue;
static short poll_fd(struct fd_entry *e, short events, struct wait_queue **wq);
struct poll_deadline;
static int poll_wait(int (*scan)(void *ctx, struct wait_queue **wqs), void *ctx, uint32_t n,
                     struct poll_deadline *d, const char *wchan);
static int32_t poll_mask_set(uintptr_t umask, uint32_t size);
static int32_t poll_mask_done(int32_t ret);
#include "io/poll.c"

extern int printf(const char *format, ...);
extern int dprintf(int fd, const char *format, ...);

volatile uint64_t kernel_ticks;

/* The processors' records, which `current_proc` reads when a wait writes
 * back its time left; no thread runs in the model. */
struct percpu cpu_table[NR_CPUS];

/** Copy `n` bytes to user memory, the host's own in the model; 0. */
int copy_to_user(void __user *dst, const void *src, size_t n)
{
    unsigned char *d = (unsigned char *)dst;
    const unsigned char *s = src;

    while (n-- > 0) {
        *d++ = *s++;
    }
    return 0;
}

/** The tick count as the kernel reads it whole: the model's `kernel_ticks`. */
uint64_t tick_count(void)
{
    return kernel_ticks;
}
static uint32_t phase_now_ns;

/* A tick the timer has reached and not yet raised, which `rtc_tick_phase`
 * counts and `tick_count` does not: 0 or 1. */
static uint32_t pending_now;

/* When not 0, the reading in nanoseconds the clock keeps after a lost tick
 * put the tick count behind it, which it reads until the count passes. */
static int64_t mono_held_ns;

/** The tick count and the place within the tick, as the deadlines read them. */
void rtc_tick_phase(uint64_t *ticks, uint32_t *phase_ns)
{
    *ticks = kernel_ticks + pending_now;
    *phase_ns = phase_now_ns;
}

/** Time since boot, TICK_NS for each tick of the count from 0 and the place
 * within the tick, or the held reading while it is later. The count of the
 * 2^40 s row is past nanoseconds in 64 bits, so seconds are kept apart. */
void rtc_monotonic(int64_t *seconds, int64_t *nanoseconds)
{
    int64_t sec = (int64_t)(kernel_ticks / TICK_HZ);
    int64_t ns = (int64_t)(kernel_ticks % TICK_HZ * TICK_NS) + phase_now_ns;
    int64_t held_sec = mono_held_ns / 1000000000, held_ns = mono_held_ns % 1000000000;

    if (held_sec > sec || (held_sec == sec && held_ns > ns)) {
        sec = held_sec;
        ns = held_ns;
    }
    *seconds = sec;
    *nanoseconds = ns;
}

/** The uptime's seconds and ticks, which count from tick 0, and the count. */
void tick_now(uint32_t *seconds, uint32_t *second_ticks, uint64_t *ticks)
{
    *seconds = (uint32_t)(kernel_ticks / TICK_HZ);
    *second_ticks = (uint32_t)(kernel_ticks % TICK_HZ);
    *ticks = kernel_ticks;
}
static int failures;

#define CHECK(c, detail)                                          \
    do {                                                          \
        if (!(c)) {                                               \
            dprintf(2, "poll-deadline-test: fail: %s\n", detail); \
            failures++;                                           \
        }                                                         \
    } while (0)

/** The deadline tick of a `poll` of `ms` milliseconds, or 0, with `*set`
 * whether it has one. */
static uint64_t ms_tick(int ms, int *set)
{
    struct poll_deadline d;

    poll_ms_deadline(ms, &d);
    *set = d.set;
    return d.set ? d.tick : 0;
}

/** The deadline tick of a `ppoll` of `ts`, or 0, with `*set` whether it has
 * one. */
static uint64_t ts_tick(const struct poll_timespec *ts, int *set)
{
    struct poll_deadline d;

    ts_deadline(ts, &d);
    *set = d.set;
    return d.set ? d.tick : 0;
}

/** The deadline tick of a `select` of `tv`, or 0, with `*set` whether it has
 * one. */
static uint64_t tv_tick(const struct poll_timeval *tv, int *set)
{
    struct poll_deadline d;

    tv_deadline(tv, &d);
    *set = d.set;
    return d.set ? d.tick : 0;
}

/* The milliseconds and the nanoseconds of `n` ticks. */
#define TICKS_MS(n) ((int)((n) * (1000u / TICK_HZ)))
#define TICKS_NS(n) ((long)((n) * TICK_NS))

/** The rows of a clock that a lost tick put ahead of the count, and of one
 * that is not. */
static void held_clock(void)
{
    struct poll_deadline d;
    struct poll_timespec ts;

    kernel_ticks = 1000;
    phase_now_ns = 0;
    mono_held_ns = 1000LL * TICK_NS + TICKS_NS(8) / 10;
    poll_ms_deadline(TICKS_MS(2), &d);
    CHECK(d.tick == 1002, "a poll of two ticks from a clock 0.8 tick ahead of tick 1000 has its "
                          "deadline tick at 1002");
    kernel_ticks = 1002;
    CHECK(!deadline_passed(&d) && d.tick == 1003,
          "where the clock is short of its end, and the deadline moves to 1003");
    kernel_ticks = 1003;
    CHECK(deadline_passed(&d), "where it has passed");
    kernel_ticks = 1000;
    ts.tv_sec = 0;
    ts.tv_nsec = TICKS_NS(3) / 2;
    ts_deadline(&ts, &d);
    kernel_ticks = 1002;
    CHECK(d.tick == 1002 && !deadline_passed(&d) && d.tick == 1003,
          "a ppoll of a tick and a half from there moves to 1003 as well");
    mono_held_ns = 0;
    kernel_ticks = 1000;
    poll_ms_deadline(TICKS_MS(3), &d);
    kernel_ticks = 1003;
    CHECK(d.tick == 1003 && deadline_passed(&d),
          "without a lost tick a poll of three ticks from tick 1000 has passed at 1003");
    kernel_ticks = 1000;
    ts.tv_sec = (long)((int64_t)1 << 40);
    ts.tv_nsec = 999999999;
    ts_deadline(&ts, &d);
    kernel_ticks = d.tick;
    CHECK(deadline_passed(&d), "a ppoll of 2^40 s, which the tick cuts, ends at its tick alone");
}

/** The rows of a zero timeout begun while a tick is pending, which must be
 * reached and passed at once, as a wait that does not sleep. */
static void pending_tick(void)
{
    struct poll_deadline d;
    struct poll_timespec ts;
    struct poll_timeval tv;

    kernel_ticks = 1000;
    phase_now_ns = 0;
    mono_held_ns = 0;
    pending_now = 1;
    poll_ms_deadline(0, &d);
    CHECK(d.tick == 1000 && deadline_reached(d.tick) && deadline_passed(&d),
          "a zero poll timeout begun with a tick pending ends at once");
    ts.tv_sec = 0;
    ts.tv_nsec = 0;
    ts_deadline(&ts, &d);
    CHECK(d.tick == 1000 && deadline_reached(d.tick) && deadline_passed(&d),
          "a zero ppoll timeout begun with a tick pending ends at once");
    tv.tv_sec = 0;
    tv.tv_usec = 0;
    tv_deadline(&tv, &d);
    CHECK(d.tick == 1000 && deadline_reached(d.tick) && deadline_passed(&d),
          "a zero select timeout begun with a tick pending ends at once");
    poll_ms_deadline(1, &d);
    CHECK(d.tick == 1002, "a poll of 1 ms begun with a tick pending ends at the tick after it");
    pending_now = 0;
}

/** Run every check; 0 when each passes, 1 otherwise. */
int main(void)
{
    struct poll_timespec ts;
    struct poll_timeval tv;
    int has = -1;

    kernel_ticks = 1000;
    CHECK(ms_tick(TICKS_MS(3), &has) == 1003 && has == 1,
          "a poll of three ticks from tick 1000 ends at 1003");
    CHECK(ms_tick(1, &has) == 1001, "a poll of 1 ms from tick 1000 ends at 1001");
    phase_now_ns = TICKS_NS(4) / 10;
    CHECK(ms_tick(TICKS_MS(1), &has) == 1002,
          "a poll of a tick begun four tenths into a tick ends two ticks later");
    ts.tv_sec = 0;
    ts.tv_nsec = TICKS_NS(1) / 10;
    CHECK(ts_tick(&ts, &has) == 1001,
          "a ppoll of a tenth of a tick begun four tenths into a tick ends at the next");
    ts.tv_nsec = TICKS_NS(7) / 10;
    CHECK(ts_tick(&ts, &has) == 1002, "and one of seven tenths at the tick after");
    phase_now_ns = 0;
    CHECK(ms_tick(0, &has) == 1000 && has == 1, "a zero poll timeout ends at once");
    CHECK(ms_tick(-1, &has) == 0 && has == 0, "a negative poll timeout has no deadline");
    ts.tv_sec = 0;
    ts.tv_nsec = TICKS_NS(3);
    CHECK(ts_tick(&ts, &has) == 1003 && has == 1,
          "a ppoll of three ticks from tick 1000 ends at 1003");
    ts.tv_nsec = TICKS_NS(21) / 10;
    CHECK(ts_tick(&ts, &has) == 1003, "a ppoll of 2.1 ticks rounds up to three");
    ts.tv_nsec = 1;
    CHECK(ts_tick(&ts, &has) == 1001, "a ppoll of 1 ns from tick 1000 ends at 1001");
    ts.tv_sec = 1;
    ts.tv_nsec = 1;
    CHECK(ts_tick(&ts, &has) == 1001u + TICK_HZ,
          "a ppoll of 1 s and 1 ns from tick 1000 ends a second and a tick later");
    ts.tv_sec = 0;
    ts.tv_nsec = 0;
    CHECK(ts_tick(&ts, &has) == 1000 && has == 1, "a zero ppoll timeout ends at once");
    tv.tv_sec = 0;
    tv.tv_usec = 1;
    CHECK(tv_tick(&tv, &has) == 1001 && has == 1, "a select of 1 us from tick 1000 ends at 1001");
    tv.tv_sec = 1;
    tv.tv_usec = 1;
    CHECK(tv_tick(&tv, &has) == 1001u + TICK_HZ,
          "a select of 1 s and 1 us from tick 1000 ends a second and a tick later");
    tv.tv_sec = 0;
    tv.tv_usec = 0;
    CHECK(tv_tick(&tv, &has) == 1000 && has == 1, "a zero select timeout ends at once");
    CHECK(!deadline_reached(1001), "a deadline one tick ahead is not reached");
    CHECK(deadline_reached(1000) && deadline_reached(999),
          "a deadline at the tick or behind it is");
    has = -1;
    CHECK(ts_tick(NULL, &has) == 0 && has == 0, "a null ppoll timeout has no deadline");
    ts.tv_sec = 0x7FFFFFFF;
    ts.tv_nsec = 999999999;
    kernel_ticks = 0xFFFFFFF0u;
    phase_now_ns = TICKS_NS(9) / 10;
    CHECK(ts_tick(&ts, &has) - kernel_ticks == 0x7FFFFFFFull * TICK_HZ + TICK_HZ + 1u,
          "the longest ppoll timeout of the i686 ends 2^31 s and a tick later, past 2^32 ticks");
    CHECK(!deadline_reached(ts_tick(&ts, &has)),
          "and its deadline is not reached at the tick it began");
    held_clock();
    pending_tick();
    if (failures) {
        return 1;
    }
    printf("poll-deadline-test: pass: poll, ppoll and select deadlines\n");
    return 0;
}
