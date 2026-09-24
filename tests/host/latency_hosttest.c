/**
 * Host test for the kernel's record of its longest delays.
 *
 * `sys/latency.c` is compiled natively with a clock the test sets, so the
 * record is driven through readings the machine's clock has produced: a
 * later reading that answers less than an earlier one, the backwards step
 * within a tick that entry 47 of milestone 50 found on the Lenovo, must be
 * dropped and not kept as a delay of nearly 2^32 microseconds. Prints
 * nothing on success; the exit status is the number of failed checks.
 */

#include "../../rach2/sys/latency.c"

static uint32_t clock_us;

uint32_t pit_now_us(void)
{
    return clock_us;
}

extern int printf(const char *fmt, ...);

static int fails;

#define CHECK(cond, what)                 \
    do {                                  \
        if (!(cond)) {                    \
            printf("  FAIL: %s\n", what); \
            fails++;                      \
        }                                 \
    } while (0)

int main(void)
{
    latency_span(LAT_DISK, 1000, 1450);
    CHECK(latency_max(LAT_DISK) == 450, "a delay is the difference of its two readings");
    latency_span(LAT_DISK, 2000, 2100);
    CHECK(latency_max(LAT_DISK) == 450, "a shorter delay leaves the longest in place");
    latency_span(LAT_DISK, 3000, 3900);
    CHECK(latency_max(LAT_DISK) == 900, "a longer one replaces it");

    /* The clock read later answers less: the step is dropped, not kept as
       the largest delay ever seen. */
    latency_span(LAT_DISK, 5000, 4237);
    CHECK(latency_max(LAT_DISK) == 900, "a reading before its start is dropped");
    latency_span(LAT_DISK, 5000, 5000);
    CHECK(latency_max(LAT_DISK) == 900, "and an equal pair is a delay of nothing");

    /* The timer's own gap through the same rule. */
    clock_us = 10000;
    latency_tick();
    CHECK(latency_max(LAT_TIMER) == 0, "the first tick has no gap to record");
    clock_us = 11200;
    latency_tick();
    CHECK(latency_max(LAT_TIMER) == 1200, "the second records the gap since the first");
    clock_us = 11000;
    latency_tick();
    CHECK(latency_max(LAT_TIMER) == 1200, "a tick whose clock stepped back records nothing");
    clock_us = 12500;
    latency_tick();
    CHECK(latency_max(LAT_TIMER) == 1500, "and the next measures from the stepped reading");

    CHECK(latency_max(LAT_COUNT) == 0 && latency_max(-1) == 0, "a kind out of range reads as zero");
    latency_note(LAT_COUNT, 77);
    latency_note(-1, 77);
    CHECK(latency_max(LAT_DISK) == 900 && latency_max(LAT_TIMER) == 1500,
          "and a note out of range changes nothing");
    return fails > 255 ? 255 : fails;
}
