/**
 * clockmusl: check that the real-time clock reports the date and advances.
 *
 * `timeprobe` checks that `nanosleep` suspends a thread; this program checks
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
 * The runner supplies a host UDP reference port as the only positional
 * argument, at QEMU's host, 10.0.2.2; a native run gives the reference as
 * `peer=ADDRESS:PORT` instead (tests/native/Readme.md).
 * Guest reads enclose a host timestamp with at most 100 ms uncertainty;
 * their whole error interval must be inside 30 seconds. Serial latency
 * cannot change this comparison. An unavailable or uncertain reference is
 * a failed measurement, distinct from a date mismatch. A failure prints
 * "CLOCKMUSL FAIL (detail)" and exits 1; success exits 0.
 *
 * Given `machine=native`, it leaves out the bound on `CLOCK_MONOTONIC`, since
 * the machine's kernel may have run for longer, and, given no `peer=` as
 * well, the comparison with a reference, since the machine's `ntpd` keeps its
 * date.
 */

#include <stdio.h>
#include <time.h>
#include <unistd.h>
#include <stdlib.h>
#include <string.h>
#include "clockref.h"
#include "../probe_args.h"

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

/**
 * Read a host sample from the reference: the one `peer=ADDRESS:PORT` names,
 * or QEMU's host at the port of the only positional argument. Returns 1 with
 * `*s` filled, or 0 when the reference is absent or unavailable.
 */
static int read_reference(struct clockref_sample *s)
{
    const char *peer = probe_arg("peer", NULL);
    const char *colon;
    char host[64];
    long port;

    if (peer == NULL) {
        port = probe_words() == 1 ? strtol(probe_word(1), NULL, 10) : 0;
        return port >= 1 && port <= 65535 && clockref_read((int)port, s);
    }
    colon = strrchr(peer, ':');
    if (colon == NULL || (size_t)(colon - peer) >= sizeof(host)) {
        return 0;
    }
    memcpy(host, peer, (size_t)(colon - peer));
    host[colon - peer] = '\0';
    port = strtol(colon + 1, NULL, 10);
    return port >= 1 && port <= 65535 && clockref_read_at(host, (int)port, s);
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

    probe_args(argc, argv);
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
    if (!probe_native() && (long)mono.tv_sec > BOOT_CEILING) {
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

    if (probe_native() && probe_arg("peer", NULL) == NULL) {
        printf("CLOCKMUSL OK\n");
        return 0;
    }
    if (!read_reference(&reference)) {
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
