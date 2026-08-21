/**
 * The clock this machine reads at boot, seen from a program.
 *
 * Distinct from `timemusl`, which is about the SLEEP: that one proves
 * `nanosleep` really suspends a thread and that `getrandom` returns different
 * bytes each time. This one is about the DATE.
 *
 * Three claims, and they fail in different ways:
 *
 * The date is REAL. `time(NULL)` must be at least the lower bound below, which
 * is a moment already past when this probe was written. A machine reporting
 * time since boot answers a few dozen seconds and fails here, which is what it
 * did before there was a clock to read.
 *
 * The clock RUNS. Two reads across a sleep must differ, and by about the time
 * that was slept: a clock anchored once and never advanced would pass the first
 * claim and fail this one.
 *
 * The two clocks are DIFFERENT clocks. `CLOCK_MONOTONIC` reports time since
 * boot and `CLOCK_REALTIME` reports the date, so a machine that answered the
 * same number for both has one clock wearing two names. That is exactly what
 * this system did before, so it is asserted, not assumed.
 *
 * The bound is a literal committed with the probe, not a build-time
 * constant: a date compiled in at build time makes the binary differ on every
 * build, and a probe whose expected value moves with the thing it tests proves
 * nothing.
 *
 * Prints "CLOCKMUSL OK" on success and "CLOCKMUSL FAIL (detail)" otherwise.
 */

#include <stdio.h>
#include <time.h>
#include <unistd.h>

/* 2026-08-14 00:00:00 UTC, the day this probe was written. */
#define NOT_BEFORE 1786665600L

/* A boot that reaches a probe takes seconds, not weeks. */
#define BOOT_CEILING 1000000L

/** Report one failed check and return 1, the probe's failure status. */
static int fail(const char *what)
{
    printf("CLOCKMUSL FAIL (%s)\n", what);

    return 1;
}

int main(void)
{
    struct timespec real;
    struct timespec mono;
    time_t first;
    time_t second;
    long slept;

    first = time(NULL);
    if (first == (time_t)-1) {
        return fail("time() failed");
    }
    if ((long)first < NOT_BEFORE) {
        char detail[80];

        snprintf(detail, sizeof(detail),
                 "the clock reports %ld, which is before this probe was written", (long)first);

        return fail(detail);
    }

    if (clock_gettime(CLOCK_REALTIME, &real) != 0) {
        return fail("CLOCK_REALTIME failed");
    }
    if (clock_gettime(CLOCK_MONOTONIC, &mono) != 0) {
        return fail("CLOCK_MONOTONIC failed");
    }
    if ((long)real.tv_sec < NOT_BEFORE) {
        return fail("CLOCK_REALTIME is not the date");
    }
    if ((long)mono.tv_sec > BOOT_CEILING) {
        return fail("CLOCK_MONOTONIC is not time since boot");
    }

    sleep(2);
    second = time(NULL);
    slept = (long)second - (long)first;
    if (slept < 1 || slept > 30) {
        char detail[80];

        snprintf(detail, sizeof(detail), "a two-second sleep moved the clock by %ld seconds",
                 slept);

        return fail(detail);
    }

    /* Printed for the host side of the check: the harness compares this with
     * the clock QEMU handed the machine. A bound committed in this file cannot
     * catch a date that is wrong by hours, and a machine reading a local-time
     * CMOS as UTC is wrong by exactly that. */
    printf("CLOCKMUSL EPOCH %ld\n", (long)second);
    printf("CLOCKMUSL OK\n");

    return 0;
}
