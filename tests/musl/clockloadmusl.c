/**
 * clockloadmusl: print the clocks once a second while the machine is busy.
 *
 * `clockloadmusl SECONDS` runs for SECONDS of CLOCK_MONOTONIC and keeps the
 * processor busy meanwhile, in the kernel and in user mode: each pass sends
 * a page through a pipe and reads it back, then sums a page of numbers.
 * Once a second by CLOCK_MONOTONIC it prints
 *
 *     CLOCKLOAD AT MONO REAL
 *
 * the two clocks in nanoseconds, and at the end "CLOCKLOAD OK". The clock
 * suite (tests/system/suites/clock.sh) stops the machine from the host while
 * this runs and compares the time these lines give with the host's time
 * between their arrivals: a tick the timer delays or coalesces must still be
 * counted. A failure prints "CLOCKLOAD FAIL (detail)" and exits 1.
 */

#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <time.h>
#include <unistd.h>

#define PAGE 4096

/** Report a failed call and return 1, the program's failure status. */
static int fail(const char *what)
{
    printf("CLOCKLOAD FAIL (%s)\n", what);
    return 1;
}

/** Nanoseconds of clock `id`, or -1 when it cannot be read. */
static long long clock_ns(clockid_t id)
{
    struct timespec t;

    if (clock_gettime(id, &t) != 0) {
        return -1;
    }
    return (long long)t.tv_sec * 1000000000LL + t.tv_nsec;
}

/** One pass of the load: a page through the pipe `fd` and back, and a sum
 * over it. Returns the sum, or -1 when the pipe fails. */
static long one_pass(const int fd[2], unsigned char *page)
{
    long sum = 0;
    int i;

    if (write(fd[1], page, PAGE) != PAGE || read(fd[0], page, PAGE) != PAGE) {
        return -1;
    }
    for (i = 0; i < PAGE; i++) {
        sum += page[i] * (i + 1);
        page[i] = (unsigned char)(sum >> 3);
    }
    return sum & 0x7FFFFFFF;
}

int main(int argc, char **argv)
{
    static unsigned char page[PAGE];
    long long start, next, now, end;
    int fd[2];
    long seconds;

    seconds = argc == 2 ? strtol(argv[1], NULL, 10) : 0;
    if (seconds <= 0 || seconds > 3600) {
        return fail("usage: clockloadmusl SECONDS, 1 to 3600");
    }
    if (pipe(fd) != 0) {
        return fail("pipe");
    }
    memset(page, 1, sizeof(page));
    setvbuf(stdout, NULL, _IOLBF, 0);
    start = clock_ns(CLOCK_MONOTONIC);
    if (start < 0 || clock_ns(CLOCK_REALTIME) < 0) {
        return fail("clock_gettime");
    }
    end = start + seconds * 1000000000LL;
    next = start;
    for (;;) {
        now = clock_ns(CLOCK_MONOTONIC);
        if (now >= next) {
            printf("CLOCKLOAD AT %lld %lld\n", now, clock_ns(CLOCK_REALTIME));
            if (now >= end) {
                break;
            }
            next += 1000000000LL;
        }
        if (one_pass(fd, page) < 0) {
            return fail("the pipe's round trip");
        }
    }
    printf("CLOCKLOAD OK\n");
    return 0;
}
