/**
 * timemusl: check `nanosleep`, `CLOCK_MONOTONIC` and `getrandom` through musl.
 *
 * It reads `CLOCK_MONOTONIC`, sleeps 120 ms with `nanosleep`, and requires the
 * clock to have advanced by at least 100 ms, so the sleep suspended the thread
 * rather than returning early. It then draws two 16-byte buffers from
 * `getrandom`, which reads the kernel's random generator, and requires them to
 * differ. musl 1.2.5 has no `arc4random`, so `getrandom` is the interface a
 * program uses.
 *
 * It prints "TIMEMUSL OK" with the milliseconds slept and a few random bytes
 * when every step works, or "TIMEMUSL FAIL: reason" at the first failure, and
 * exits 0 or 1 to match. It is linked statically against the ported musl; see
 * `tests/musl/build.sh`.
 */

#define _GNU_SOURCE /* expose getrandom */

#include <stdio.h>
#include <stdlib.h>
#include <time.h>
#include <string.h>
#include <sys/random.h>

int main(void)
{
    struct timespec t0, t1, req;
    long ms;
    unsigned char rb[16], rb2[16];

    if (clock_gettime(CLOCK_MONOTONIC, &t0) != 0) {
        printf("TIMEMUSL FAIL: clock_gettime\n");
        return 1;
    }

    req.tv_sec = 0;
    req.tv_nsec = 120000000; /* 120 ms */
    if (nanosleep(&req, NULL) != 0) {
        printf("TIMEMUSL FAIL: nanosleep\n");
        return 1;
    }

    if (clock_gettime(CLOCK_MONOTONIC, &t1) != 0) {
        printf("TIMEMUSL FAIL: clock_gettime (2)\n");
        return 1;
    }

    ms = (long)(t1.tv_sec - t0.tv_sec) * 1000 + (t1.tv_nsec - t0.tv_nsec) / 1000000;
    if (ms < 100) {
        printf("TIMEMUSL FAIL: slept only %ld ms\n", ms);
        return 1;
    }

    /* Two draws through libc must differ. */
    if (getrandom(rb, sizeof rb, 0) != (ssize_t)sizeof rb ||
        getrandom(rb2, sizeof rb2, 0) != (ssize_t)sizeof rb2) {
        printf("TIMEMUSL FAIL: getrandom\n");
        return 1;
    }
    if (memcmp(rb, rb2, sizeof rb) == 0) {
        printf("TIMEMUSL FAIL: getrandom repeated\n");
        return 1;
    }

    printf("TIMEMUSL OK (slept %ld ms, rand %02x%02x %02x%02x)\n", ms, rb[0], rb[15], rb2[0],
           rb2[15]);
    return 0;
}
