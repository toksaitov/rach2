/**
 * pollmusl: check poll and select on a pipe through musl's wrappers.
 *
 * The freestanding pollprobe makes the system calls itself; this program goes
 * through musl, so it checks the calls musl makes. On i686 `poll` is SYS_poll
 * (168), and `select` with a timeout that fits in 32 bits is SYS__newselect
 * (142); musl tries SYS_pselect6_time64 (413) first only for a larger
 * timeout. On x86-64 `poll` is SYS_poll (7) and `select` is SYS_select (23).
 *
 * `poll` of an empty pipe with a short timeout returns 0. After a write,
 * `poll` reports the read end POLLIN, and `select` with `FD_SET` and
 * `FD_ISSET` finds that descriptor readable. Last, a child that writes after
 * 60 ms wakes the parent blocked in `poll`, which returns 1 well before its
 * timeout of 3 s.
 *
 * Staged into the test image as /musl/pollmusl. Prints `POLLMUSL OK`, or
 * `POLLMUSL FAIL` with the number of failed checks after a line for each.
 */
#include <unistd.h>
#include <stdio.h>
#include <string.h>
#include <poll.h>
#include <sys/select.h>
#include <sys/wait.h>
#include <time.h>

static int fails;

/** Count and print a failed check when `cond` is zero. */
static void check(int cond, const char *msg)
{
    if (!cond) {
        fails++;
        printf("  FAIL: %s\n", msg);
    }
}

/** Sleep for `ms` milliseconds with `nanosleep`. */
static void msleep(int ms)
{
    struct timespec ts;
    ts.tv_sec = ms / 1000;
    ts.tv_nsec = (long)(ms % 1000) * 1000000L;
    nanosleep(&ts, 0);
}

int main(void)
{
    int p[2];
    struct pollfd pfd;
    fd_set rset;
    struct timeval tv;
    char buf[8];
    pid_t pid;
    int n, status;

    if (pipe(p) != 0) {
        printf("POLLMUSL FAIL: pipe\n");
        return 1;
    }

    pfd.fd = p[0];
    pfd.events = POLLIN;
    pfd.revents = 0;
    check(poll(&pfd, 1, 30) == 0, "poll empty pipe -> 0 (timeout)");

    write(p[1], "x", 1);
    pfd.revents = 0;
    n = poll(&pfd, 1, 1000);
    check(n == 1 && (pfd.revents & POLLIN), "poll reports POLLIN after write");

    FD_ZERO(&rset);
    FD_SET(p[0], &rset);
    tv.tv_sec = 1;
    tv.tv_usec = 0;
    n = select(p[0] + 1, &rset, 0, 0, &tv);
    check(n == 1 && FD_ISSET(p[0], &rset), "select finds the readable fd");
    read(p[0], buf, sizeof(buf));

    pid = fork();
    if (pid < 0) {
        printf("POLLMUSL FAIL: fork\n");
        return 1;
    }
    if (pid == 0) {
        close(p[0]);
        msleep(60);
        write(p[1], "z", 1);
        _exit(0);
    } else {
        pfd.fd = p[0];
        pfd.events = POLLIN;
        pfd.revents = 0;
        n = poll(&pfd, 1, 3000);
        check(n == 1 && (pfd.revents & POLLIN), "poll blocks then wakes on child write");
        read(p[0], buf, sizeof(buf));
        waitpid(pid, &status, 0);
    }

    if (fails == 0) {
        printf("POLLMUSL OK\n");
    } else {
        printf("POLLMUSL FAIL: %d check(s)\n", fails);
    }
    return fails ? 1 : 0;
}
