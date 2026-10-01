/**
 * Check the deadlines `poll`, `ppoll`, `select` and `pselect6` wait to.
 *
 * host-cflags: -Wno-int-to-pointer-cast -Wno-pointer-to-int-cast
 *
 * The system call exports and the poll core's other exports are made private
 * before the source is included, so at -O1, which the host suite and the
 * scripts that build this test use, the native compiler removes every path
 * but the conversions and the comparison this test calls, and nothing else of
 * the kernel is linked. The wait itself, which asks `deadline_reached` at each
 * wake, is not compiled.
 *
 * A wait ends at the first tick at or after its end, measured from where in
 * the tick in progress it began, so a timeout of three ticks begun as tick
 * 1000 starts ends at 1003: `poll`'s milliseconds, `select`'s microseconds,
 * which round up to milliseconds first, and `ppoll`'s and `pselect6`'s
 * seconds and nanoseconds alike. A `ppoll` and a `select` of 1 s and 11 ms
 * end at 1102, so seconds count a hundred ticks and a part of a tick rounds
 * up, and a `poll` of 1 ms and a `ppoll` of 1 ns end at 1001; begun 4 ms into
 * the tick, a `poll` of 1 ms ends at 1001 and one of 7 ms at 1002. A zero
 * timeout ends at once, a negative or
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
struct fd_entry;
struct wait_queue;
static short poll_fd(struct fd_entry *e, short events, struct wait_queue **wq);
static int poll_wait(int (*scan)(void *ctx, struct wait_queue **wqs), void *ctx, uint32_t n,
                     int has_deadline, uint32_t deadline);
static int32_t poll_mask_set(uint32_t umask, uint32_t size);
static int32_t poll_mask_done(int32_t ret);
#include "io/poll.c"

extern int printf(const char *format, ...);
extern int dprintf(int fd, const char *format, ...);

volatile uint32_t kernel_ticks;
static uint32_t phase_now_ns;

/** The tick count and the place within the tick, as the deadlines read them. */
void rtc_tick_phase(uint32_t *ticks, uint32_t *phase_ns)
{
    *ticks = kernel_ticks;
    *phase_ns = phase_now_ns;
}
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
    CHECK(poll_ms_deadline(30, &has) == 1003 && has == 1,
          "a poll of 30 ms from tick 1000 ends at 1003");
    CHECK(poll_ms_deadline(1, &has) == 1001, "a poll of 1 ms from tick 1000 ends at 1001");
    CHECK(poll_ms_deadline(21, &has) == 1003, "a poll of 21 ms rounds up to three ticks");
    phase_now_ns = 4000000;
    CHECK(poll_ms_deadline(1, &has) == 1001, "a poll of 1 ms begun 4 ms into a tick ends at the next");
    CHECK(poll_ms_deadline(7, &has) == 1002, "and one of 7 ms at the tick after");
    phase_now_ns = 0;
    CHECK(poll_ms_deadline(0, &has) == 1000 && has == 1, "a zero poll timeout ends at once");
    CHECK(poll_ms_deadline(-1, &has) == 0 && has == 0, "a negative poll timeout has no deadline");
    ts.tv_sec = 0;
    ts.tv_nsec = 30000000;
    CHECK(ts_deadline(&ts, &has) == 1003 && has == 1,
          "a ppoll of 30 ms from tick 1000 ends at 1003");
    ts.tv_nsec = 1;
    CHECK(ts_deadline(&ts, &has) == 1001, "a ppoll of 1 ns from tick 1000 ends at 1001");
    ts.tv_sec = 1;
    ts.tv_nsec = 11000000;
    CHECK(ts_deadline(&ts, &has) == 1102, "a ppoll of 1 s and 11 ms from tick 1000 ends at 1102");
    ts.tv_sec = 0;
    ts.tv_nsec = 0;
    CHECK(ts_deadline(&ts, &has) == 1000 && has == 1, "a zero ppoll timeout ends at once");
    tv.tv_sec = 0;
    tv.tv_usec = 1;
    CHECK(poll_ms_deadline(tv_to_ms(&tv), &has) == 1001 && has == 1,
          "a select of 1 us from tick 1000 ends at 1001");
    tv.tv_sec = 1;
    tv.tv_usec = 11000;
    CHECK(poll_ms_deadline(tv_to_ms(&tv), &has) == 1102,
          "a select of 1 s and 11 ms from tick 1000 ends at 1102");
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
