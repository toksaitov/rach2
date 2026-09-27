/**
 * Check the deadlines `poll`, `ppoll`, `select` and `pselect6` wait to.
 *
 * host-cflags: -Wno-int-to-pointer-cast -Wno-pointer-to-int-cast
 *
 * The system call exports are made private before the source is included, so
 * at -O1, which the host suite and the scripts that build this test use, the
 * native compiler removes every path but the conversions and the comparison
 * this test calls, and nothing else of the kernel is linked. The wait itself,
 * which asks `deadline_reached` at each wake, is not compiled.
 *
 * The tick in progress has partly passed when the wait starts, so a timeout
 * of three ticks from tick 1000 ends at 1004: `poll`'s milliseconds, which
 * round up to ticks, `select`'s microseconds, which round up to milliseconds
 * first, and `ppoll`'s and `pselect6`'s seconds and nanoseconds alike. A
 * `ppoll` and a `select` of 1 s and 11 ms end at 1103, so seconds count a
 * hundred ticks and a part of a tick rounds up, and a `poll` of 1 ms and a
 * `ppoll` of 1 ns end at 1002. A zero timeout ends at once, a negative or
 * null one has no deadline, the longest `ppoll` timeout ends 0x7FFFFFFF
 * ticks ahead when the count is about to wrap and is not reached at the tick
 * it began, and a deadline is reached at its tick and not the tick before.
 */
#include "isr.h"
struct isr_params;
static int32_t sys_poll(struct isr_params *params);
static int32_t sys_ppoll(struct isr_params *params);
static int32_t sys__newselect(struct isr_params *params);
static int32_t sys_select(struct isr_params *params);
static int32_t sys_pselect6(struct isr_params *params);
#include "io/poll.c"

extern int printf(const char *format, ...);
extern int dprintf(int fd, const char *format, ...);

volatile uint32_t kernel_ticks;
static int failures;

#define CHECK(c, detail)                                          \
    do {                                                          \
        if (!(c)) {                                               \
            dprintf(2, "poll-deadline-test: fail: %s\n", detail); \
            failures++;                                           \
        }                                                         \
    } while (0)

int main(void)
{
    struct poll_timespec ts;
    struct poll_timeval tv;
    int has = -1;

    kernel_ticks = 1000;
    CHECK(poll_ms_deadline(30, &has) == 1004 && has == 1,
          "a poll of 30 ms from tick 1000 ends at 1004");
    CHECK(poll_ms_deadline(1, &has) == 1002, "a poll of 1 ms from tick 1000 ends at 1002");
    CHECK(poll_ms_deadline(21, &has) == 1004, "a poll of 21 ms rounds up to three ticks");
    CHECK(poll_ms_deadline(0, &has) == 1000 && has == 1, "a zero poll timeout ends at once");
    CHECK(poll_ms_deadline(-1, &has) == 0 && has == 0, "a negative poll timeout has no deadline");
    ts.tv_sec = 0;
    ts.tv_nsec = 30000000;
    CHECK(ts_deadline(&ts, &has) == 1004 && has == 1,
          "a ppoll of 30 ms from tick 1000 ends at 1004");
    ts.tv_nsec = 1;
    CHECK(ts_deadline(&ts, &has) == 1002, "a ppoll of 1 ns from tick 1000 ends at 1002");
    ts.tv_sec = 1;
    ts.tv_nsec = 11000000;
    CHECK(ts_deadline(&ts, &has) == 1103, "a ppoll of 1 s and 11 ms from tick 1000 ends at 1103");
    ts.tv_sec = 0;
    ts.tv_nsec = 0;
    CHECK(ts_deadline(&ts, &has) == 1000 && has == 1, "a zero ppoll timeout ends at once");
    tv.tv_sec = 0;
    tv.tv_usec = 1;
    CHECK(poll_ms_deadline(tv_to_ms(&tv), &has) == 1002 && has == 1,
          "a select of 1 us from tick 1000 ends at 1002");
    tv.tv_sec = 1;
    tv.tv_usec = 11000;
    CHECK(poll_ms_deadline(tv_to_ms(&tv), &has) == 1103,
          "a select of 1 s and 11 ms from tick 1000 ends at 1103");
    tv.tv_sec = 0;
    tv.tv_usec = 0;
    CHECK(poll_ms_deadline(tv_to_ms(&tv), &has) == 1000 && has == 1,
          "a zero select timeout ends at once");
    CHECK(!deadline_reached(1001), "a deadline one tick ahead is not reached");
    CHECK(deadline_reached(1000) && deadline_reached(999),
          "a deadline at the tick or behind it is");
    has = -1;
    CHECK(ts_deadline(NULL, &has) == 0 && has == 0, "a null ppoll timeout has no deadline");
    ts.tv_sec = 0x7FFFFFFF;
    ts.tv_nsec = 999999999;
    kernel_ticks = 0xFFFFFFF0u;
    CHECK(ts_deadline(&ts, &has) - kernel_ticks == 0x7FFFFFFFu,
          "the longest ppoll timeout ends 0x7FFFFFFF ticks ahead across the wrap");
    CHECK(!deadline_reached(ts_deadline(&ts, &has)),
          "and its deadline is not reached at the tick it began");
    if (failures) {
        return 1;
    }
    printf("poll-deadline-test: pass: poll, ppoll and select deadlines\n");
    return 0;
}
