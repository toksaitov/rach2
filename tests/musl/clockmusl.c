/**
 * clockmusl: check that the real-time clock reports the date and advances.
 *
 * `timemusl` checks that `nanosleep` suspends a thread; this program checks
 * the date. Each of its three checks catches a different fault:
 *
 * The date is later than `NOT_BEFORE`. A machine that reports the time since
 * boot as the date answers a few dozen seconds and fails here.
 *
 * The clock advances. Two reads of `time` across a two-second sleep must
 * differ by at least one second; a clock set once at boot and never advanced
 * passes the first check and fails this one.
 *
 * The two clocks differ. `CLOCK_REALTIME` must be later than `NOT_BEFORE`
 * and `CLOCK_MONOTONIC`, the time since boot, under `BOOT_CEILING`, so a
 * machine that answers the same number for both fails one of them.
 *
 * The bound is a literal in this file rather than the date of the build,
 * which would make the binary differ on every build.
 *
 * The runner supplies a host UDP reference port as the only argument.
 * Guest reads enclose a host timestamp with at most 100 ms uncertainty;
 * their whole error interval must be inside 30 seconds. Serial latency
 * cannot change this comparison. An unavailable or uncertain reference is
 * a failed measurement, distinct from a date mismatch. A failure prints
 * "CLOCKMUSL FAIL (detail)" and exits 1; success exits 0.
 */

#include <stdio.h>
#include <time.h>
#include <unistd.h>
#include <stdlib.h>
#include "clockref.h"

/* 2026-08-14 00:00:00 UTC, in seconds; a correct clock reads later. */
#define NOT_BEFORE 1786665600L

/* The bound on CLOCK_MONOTONIC, in seconds: about 11 days. */
#define BOOT_CEILING 1000000L

/** Report one failed check and return 1, the probe's failure status. */
static int fail(const char *what)
{
    printf("CLOCKMUSL FAIL (%s)\n", what);

    return 1;
}

/** Check the date and its progress, then the independent host reference. */
int main(int argc, char **argv)
{
    struct timespec real;
    struct timespec mono;
    time_t first;
    time_t second;
    long slept;
    struct clockref_sample reference;
    int verdict;
    long port = argc == 2 ? strtol(argv[1], NULL, 10) : 0;

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
    if (slept < 1) {
        char detail[80];

        snprintf(detail, sizeof(detail), "a two-second sleep moved the clock by %ld seconds",
                 slept);

        return fail(detail);
    }

    if (port < 1 || port > 65535 || !clockref_read((int)port, &reference)) {
        return fail("reference unavailable");
    }
    verdict = clockref_judge(reference.real_before - reference.host_real,
                             reference.real_after - reference.host_real, 30 * CLOCKREF_NS);
    if (verdict != 1) {
        return fail(verdict == 0 ? "date differs from host" : "reference uncertainty");
    }
    printf("CLOCKMUSL REFERENCE OK\n");
    printf("CLOCKMUSL OK\n");

    return 0;
}
