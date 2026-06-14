/*
 * pollmusl.c - Phase 14 integration: real musl poll() and select() over a pipe.
 *
 * Unlike the freestanding pollprobe (which issues int 0x80 directly), this drives
 * the kernel through musl's own wrappers, so it pins the actual ABI musl emits on
 * i386: poll() -> SYS_poll (168), and select() -> SYS_pselect6_time64 (413, which
 * the kernel answers -ENOSYS) falling back to SYS__newselect (142). It proves the
 * whole family works end to end:
 *   - poll() of an empty pipe with a short timeout returns 0;
 *   - after a write, poll() reports the read end POLLIN;
 *   - select() (FD_SET/FD_ISSET, with a timeout) finds that same fd readable;
 *   - a child that writes after a delay wakes a parent blocked in poll().
 * Installed as "mpoll" by krffs/build_test_ramdisk.sh. Prints "POLLMUSL OK".
 */
#include <unistd.h>
#include <stdio.h>
#include <string.h>
#include <poll.h>
#include <sys/select.h>
#include <sys/wait.h>
#include <time.h>

static int fails;

static void check(int cond, const char *msg)
{
    if (!cond) {
        fails++;
        printf("  FAIL: %s\n", msg);
    }
}

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

    /* poll() of an empty pipe times out to 0 (no descriptor ready). */
    pfd.fd = p[0];
    pfd.events = POLLIN;
    pfd.revents = 0;
    check(poll(&pfd, 1, 30) == 0, "poll empty pipe -> 0 (timeout)");

    /* After a write, poll() reports the read end readable. */
    write(p[1], "x", 1);
    pfd.revents = 0;
    n = poll(&pfd, 1, 1000);
    check(n == 1 && (pfd.revents & POLLIN), "poll reports POLLIN after write");

    /* select() must find the same readable fd (exercises the fd_set translation
     * and musl's pselect6_time64 -> _newselect fallback). Data is still buffered. */
    FD_ZERO(&rset);
    FD_SET(p[0], &rset);
    tv.tv_sec = 1;
    tv.tv_usec = 0;
    n = select(p[0] + 1, &rset, 0, 0, &tv);
    check(n == 1 && FD_ISSET(p[0], &rset), "select finds the readable fd");
    read(p[0], buf, sizeof(buf)); /* drain the byte */

    /* A child that writes after a delay wakes a parent blocked in poll() -- the
     * return must come from the write, well under the 3s timeout. */
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
