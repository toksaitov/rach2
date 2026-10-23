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
 * `FD_ISSET` finds that descriptor readable. Last, the parent waits in `poll`
 * on the empty pipe while a child reads the parent's state in
 * /proc/PID/stat until it says S, asleep, and only then writes. `poll` must
 * return 1 with the byte already in the pipe, as FIONREAD counts it, and the
 * child must exit 0, which it does only when it saw the parent asleep and its
 * write took. A `poll` that returned at once would find the pipe empty, since
 * the child writes only to a sleeping parent.
 *
 * Staged into the test image as /musl/pollmusl. Prints `POLLMUSL OK`, or
 * `POLLMUSL FAIL` with the number of failed checks after a line for each.
 */
#include <unistd.h>
#include <stdio.h>
#include <string.h>
#include <poll.h>
#include <fcntl.h>
#include <sys/ioctl.h>
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

/** The state letter of /proc/`pid`/stat, the field after the command's
 * closing parenthesis, or 0 when the file cannot be read. */
static char state_of(pid_t pid)
{
    char path[32], text[256], *end;
    int fd, n;

    snprintf(path, sizeof(path), "/proc/%d/stat", (int)pid);
    fd = open(path, O_RDONLY);
    if (fd < 0) {
        return 0;
    }
    n = (int)read(fd, text, sizeof(text) - 1);
    close(fd);
    if (n <= 0) {
        return 0;
    }
    text[n] = '\0';
    end = strrchr(text, ')');
    return end && end[1] == ' ' ? end[2] : 0;
}

/** The child's half: wait until `parent` sleeps, and then write a byte to
 * `fd`. Exit 0 when the parent was seen asleep and the write took, 1 when the
 * write failed and 2 when the parent ended without sleeping. */
static void wake_sleeper(pid_t parent, int fd)
{
    struct timespec tick = {0, 1000000};
    char state;

    for (;;) {
        state = state_of(parent);
        if (state == 'S') {
            _exit(write(fd, "z", 1) == 1 ? 0 : 1);
        }
        if (state == 0 || state == 'Z') {
            _exit(2);
        }
        /* interval: the parent's state is looked at again every millisecond */
        nanosleep(&tick, 0);
    }
}

int main(void)
{
    int p[2];
    struct pollfd pfd;
    fd_set rset;
    struct timeval tv;
    char buf[8];
    pid_t pid;
    int n, status, queued;

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
    n = poll(&pfd, 1, -1);
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
        wake_sleeper(getppid(), p[1]);
    }
    pfd.fd = p[0];
    pfd.events = POLLIN;
    pfd.revents = 0;
    n = poll(&pfd, 1, -1);
    queued = -1;
    ioctl(p[0], FIONREAD, &queued);
    check(n == 1 && (pfd.revents & POLLIN), "poll wakes on the child's write");
    check(queued == 1, "poll returned only once the child had written");
    read(p[0], buf, sizeof(buf));
    check(waitpid(pid, &status, 0) == pid && WIFEXITED(status) && WEXITSTATUS(status) == 0,
          "the child saw the parent asleep in poll and wrote");

    if (fails == 0) {
        printf("POLLMUSL OK\n");
    } else {
        printf("POLLMUSL FAIL: %d check(s)\n", fails);
    }
    return fails ? 1 : 0;
}
