/**
 * Check the deadlines `poll`, `ppoll`, `select` and `pselect6` wait to.
 *
 * host-cflags: -Wno-int-to-pointer-cast -Wno-pointer-to-int-cast
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
 * 1000 starts ends at 1003: `poll`'s milliseconds, `select`'s microseconds,
 * which round up to milliseconds first, and `ppoll`'s and `pselect6`'s
 * seconds and nanoseconds alike. A `ppoll` and a `select` of 1 s and 11 ms
 * end at 1102, so seconds count a hundred ticks and a part of a tick rounds
 * up, and a `poll` of 1 ms and a `ppoll` of 1 ns end at 1001; begun 4 ms into
 * the tick, a `poll` of 1 ms ends at 1001 and one of 7 ms at 1002. A zero
 * timeout ends at once, and a negative or null one has no deadline. The
 * longest `ppoll` timeout, begun 9 ms into a tick so that its rounding would
 * carry it a tick past the limit, ends 0x7FFFFFFF ticks ahead when the count
 * is about to wrap, and is not reached at the tick it began. A deadline is
 * reached at its tick and not the tick before.
 *
 * A wait also lasts until time since boot has advanced by its timeout. After
 * a lost tick the clock keeps a reading ahead of the count, here 8 ms past
 * tick 1000. A `poll` of 15 ms from there has its deadline tick at 1002,
 * where the clock reads 10.020 s, short of 10.023 s, so the deadline moves to
 * 1003, where it has passed; a `ppoll` of 15 ms does the same. Without a lost
 * tick, a timeout has passed at its deadline tick, and a `ppoll` of 21474836
 * seconds, the clamp, ends at its tick alone.
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
                     struct poll_deadline *d);
static int32_t poll_mask_set(uintptr_t umask, uint32_t size);
static int32_t poll_mask_done(int32_t ret);
#include "io/poll.c"

extern int printf(const char *format, ...);
extern int dprintf(int fd, const char *format, ...);

volatile uint32_t kernel_ticks;
static uint32_t phase_now_ns;

/* When not 0, the reading in nanoseconds the clock keeps after a lost tick
 * put the tick count behind it, which it reads until the count passes. */
static int64_t mono_held_ns;

/** The tick count and the place within the tick, as the deadlines read them. */
void rtc_tick_phase(uint32_t *ticks, uint32_t *phase_ns)
{
    *ticks = kernel_ticks;
    *phase_ns = phase_now_ns;
}

/** Time since boot, a hundredth of a second for each tick of the count from
 * 0 and the place within the tick, or the held reading while it is later. */
void rtc_monotonic(int64_t *seconds, int64_t *nanoseconds)
{
    int64_t ns = (int64_t)kernel_ticks * 10000000 + phase_now_ns;

    if (mono_held_ns > ns) {
        ns = mono_held_ns;
    }
    *seconds = ns / 1000000000;
    *nanoseconds = ns % 1000000000;
}

/** The uptime's seconds and ticks, which count from tick 0, and the count. */
void tick_now(uint32_t *seconds, uint32_t *second_ticks, uint32_t *ticks)
{
    *seconds = kernel_ticks / 100u;
    *second_ticks = kernel_ticks % 100u;
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
static uint32_t ms_tick(int ms, int *set)
{
    struct poll_deadline d;

    poll_ms_deadline(ms, &d);
    *set = d.set;
    return d.set ? d.tick : 0;
}

/** The deadline tick of a `ppoll` of `ts`, or 0, with `*set` whether it has
 * one. */
static uint32_t ts_tick(const struct poll_timespec *ts, int *set)
{
    struct poll_deadline d;

    ts_deadline(ts, &d);
    *set = d.set;
    return d.set ? d.tick : 0;
}

/** The rows of a clock that a lost tick put ahead of the count, and of one
 * that is not. */
static void held_clock(void)
{
    struct poll_deadline d;
    struct poll_timespec ts;

    kernel_ticks = 1000;
    phase_now_ns = 0;
    mono_held_ns = 10008000000LL;
    poll_ms_deadline(15, &d);
    CHECK(d.tick == 1002,
          "a poll of 15 ms from a clock 8 ms ahead of tick 1000 has its deadline tick at 1002");
    kernel_ticks = 1002;
    CHECK(!deadline_passed(&d) && d.tick == 1003,
          "where the clock is short of its end, and the deadline moves to 1003");
    kernel_ticks = 1003;
    CHECK(deadline_passed(&d), "where it has passed");
    kernel_ticks = 1000;
    ts.tv_sec = 0;
    ts.tv_nsec = 15000000;
    ts_deadline(&ts, &d);
    kernel_ticks = 1002;
    CHECK(d.tick == 1002 && !deadline_passed(&d) && d.tick == 1003,
          "a ppoll of 15 ms from there moves to 1003 as well");
    mono_held_ns = 0;
    kernel_ticks = 1000;
    poll_ms_deadline(30, &d);
    kernel_ticks = 1003;
    CHECK(d.tick == 1003 && deadline_passed(&d),
          "without a lost tick a poll of 30 ms from tick 1000 has passed at 1003");
    kernel_ticks = 1000;
    ts.tv_sec = 21474836;
    ts.tv_nsec = 999999999;
    ts_deadline(&ts, &d);
    kernel_ticks = d.tick;
    CHECK(deadline_passed(&d), "a ppoll of 21474836 s, the clamp, ends at its tick alone");
}

/** Run every check; 0 when each passes, 1 otherwise. */
int main(void)
{
    struct poll_timespec ts;
    struct poll_timeval tv;
    int has = -1;

    kernel_ticks = 1000;
    CHECK(ms_tick(30, &has) == 1003 && has == 1, "a poll of 30 ms from tick 1000 ends at 1003");
    CHECK(ms_tick(1, &has) == 1001, "a poll of 1 ms from tick 1000 ends at 1001");
    CHECK(ms_tick(21, &has) == 1003, "a poll of 21 ms rounds up to three ticks");
    phase_now_ns = 4000000;
    CHECK(ms_tick(1, &has) == 1001, "a poll of 1 ms begun 4 ms into a tick ends at the next");
    CHECK(ms_tick(7, &has) == 1002, "and one of 7 ms at the tick after");
    phase_now_ns = 0;
    CHECK(ms_tick(0, &has) == 1000 && has == 1, "a zero poll timeout ends at once");
    CHECK(ms_tick(-1, &has) == 0 && has == 0, "a negative poll timeout has no deadline");
    ts.tv_sec = 0;
    ts.tv_nsec = 30000000;
    CHECK(ts_tick(&ts, &has) == 1003 && has == 1, "a ppoll of 30 ms from tick 1000 ends at 1003");
    ts.tv_nsec = 1;
    CHECK(ts_tick(&ts, &has) == 1001, "a ppoll of 1 ns from tick 1000 ends at 1001");
    ts.tv_sec = 1;
    ts.tv_nsec = 11000000;
    CHECK(ts_tick(&ts, &has) == 1102, "a ppoll of 1 s and 11 ms from tick 1000 ends at 1102");
    ts.tv_sec = 0;
    ts.tv_nsec = 0;
    CHECK(ts_tick(&ts, &has) == 1000 && has == 1, "a zero ppoll timeout ends at once");
    tv.tv_sec = 0;
    tv.tv_usec = 1;
    CHECK(ms_tick(tv_to_ms(&tv), &has) == 1001 && has == 1,
          "a select of 1 us from tick 1000 ends at 1001");
    tv.tv_sec = 1;
    tv.tv_usec = 11000;
    CHECK(ms_tick(tv_to_ms(&tv), &has) == 1102,
          "a select of 1 s and 11 ms from tick 1000 ends at 1102");
    tv.tv_sec = 0;
    tv.tv_usec = 0;
    CHECK(ms_tick(tv_to_ms(&tv), &has) == 1000 && has == 1, "a zero select timeout ends at once");
    CHECK(!deadline_reached(1001), "a deadline one tick ahead is not reached");
    CHECK(deadline_reached(1000) && deadline_reached(999),
          "a deadline at the tick or behind it is");
    has = -1;
    CHECK(ts_tick(NULL, &has) == 0 && has == 0, "a null ppoll timeout has no deadline");
    ts.tv_sec = 0x7FFFFFFF;
    ts.tv_nsec = 999999999;
    kernel_ticks = 0xFFFFFFF0u;
    phase_now_ns = 9000000;
    CHECK(ts_tick(&ts, &has) - kernel_ticks == 0x7FFFFFFFu,
          "the longest ppoll timeout ends 0x7FFFFFFF ticks ahead across the wrap");
    CHECK(!deadline_reached(ts_tick(&ts, &has)),
          "and its deadline is not reached at the tick it began");
    held_clock();
    if (failures) {
        return 1;
    }
    printf("poll-deadline-test: pass: poll, ppoll and select deadlines\n");
    return 0;
}
