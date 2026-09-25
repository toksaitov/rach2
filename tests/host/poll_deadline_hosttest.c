/**
 * Check that a finite poll or select timeout is never shorter than asked.
 *
 * host-cflags: -Wno-int-to-pointer-cast -Wno-pointer-to-int-cast
 *
 * The system call exports are made private before the source is included, so
 * at -O1, which the host suite and the scripts that build this test use, the
 * native compiler removes every path but the deadline conversions this test
 * calls, and nothing else of the kernel is linked.
 *
 * The tick under way has partly passed when the wait starts, so a timeout of
 * three ticks from tick 1000 ends at 1004: `poll`'s milliseconds, which round
 * up to ticks, and `ppoll`'s and `pselect6`'s seconds and nanoseconds alike.
 * A zero timeout ends at once, a negative or null one has no deadline, and
 * the longest `ppoll` timeout still ends ahead of the tick when the count is
 * about to wrap.
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
    int has = -1;

    kernel_ticks = 1000;
    CHECK(ms_deadline(30, &has) == 1004 && has == 1, "a poll of 30 ms from tick 1000 ends at 1004");
    CHECK(ms_deadline(21, &has) == 1004, "a poll of 21 ms rounds up to three ticks");
    CHECK(ms_deadline(0, &has) == 1000 && has == 1, "a zero poll timeout ends at once");
    CHECK(ms_deadline(-1, &has) == 0 && has == 0, "a negative poll timeout has no deadline");
    ts.tv_sec = 0;
    ts.tv_nsec = 30000000;
    CHECK(ts_deadline(&ts, &has) == 1004 && has == 1,
          "a ppoll of 30 ms from tick 1000 ends at 1004");
    ts.tv_nsec = 0;
    CHECK(ts_deadline(&ts, &has) == 1000 && has == 1, "a zero ppoll timeout ends at once");
    has = -1;
    CHECK(ts_deadline(NULL, &has) == 0 && has == 0, "a null ppoll timeout has no deadline");
    ts.tv_sec = 0x7FFFFFFF;
    ts.tv_nsec = 999999999;
    kernel_ticks = 0xFFFFFFF0u;
    CHECK((int32_t)(ts_deadline(&ts, &has) - kernel_ticks) > 0,
          "the longest ppoll timeout ends ahead of the tick across the wrap");
    if (failures) {
        return 1;
    }
    printf("poll-deadline-test: pass: poll and ppoll deadlines\n");
    return 0;
}
