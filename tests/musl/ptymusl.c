/**
 * ptymusl: check a pseudo-terminal round trip through musl, in the form a
 * login server uses to give a remote shell a terminal.
 *
 * `openpty()` opens `/dev/ptmx`, unlocks it (`TIOCSPTLCK`), resolves the slave
 * name via `ptsname()` (`TIOCGPTN`), and opens `/dev/pts/N`. The test then
 * forks. The child checks that `ttyname(slave)` gives a name under `/dev/pts/`
 * (musl's `ttyname` calls `isatty`, reads the link `/proc/self/fd/N`, and
 * compares the target's `stat` with the descriptor's `fstat`), reads the cooked
 * line off the slave, and replies "pong" on it; a child whose `ttyname` check
 * fails exits without replying. The parent writes "ping" to the master, which
 * the line discipline echoes back and cooks into the slave, and passes when it
 * reads both the echo and the reply from the master. On x86-64 a TCFLSH of the
 * master whose selector passes 32 bits is EINVAL, as Linux's is.
 *
 * Prints "PTYMUSL OK" on success and a "PTYMUSL FAIL (...)" line naming the
 * failed step otherwise. No suite runs it; it is run by hand at the kernel
 * shell. Linked against the ported musl.
 */

#include <errno.h>
#include <pty.h>
#include <unistd.h>
#include <stdio.h>
#include <string.h>
#include <sys/ioctl.h>
#include <sys/syscall.h>
#include <sys/wait.h>

int main(void)
{
    int master = -1, slave = -1;
    pid_t pid;
    int status;
    char mbuf[128];
    int total = 0;

    if (openpty(&master, &slave, NULL, NULL, NULL) != 0) {
        printf("PTYMUSL FAIL (openpty)\n");
        return 1;
    }
#if defined __x86_64__
    if (syscall(SYS_ioctl, master, TCFLSH, 0x100000000UL) != -1 || errno != EINVAL) {
        printf("PTYMUSL FAIL (TCFLSH of a selector past 32 bits is not EINVAL)\n");
        return 1;
    }
#endif

    pid = fork();
    if (pid < 0) {
        printf("PTYMUSL FAIL (fork)\n");
        return 1;
    }
    if (pid == 0) {
        char sbuf[64];
        char *tn;
        int n;
        close(master);
        tn = ttyname(slave);
        if (!tn || strncmp(tn, "/dev/pts/", 9) != 0) {
            _exit(7);
        }
        n = (int)read(slave, sbuf, sizeof(sbuf));
        if (n >= 4 && memchr(sbuf, 'p', (size_t)n)) {
            write(slave, "pong\n", 5);
        }
        close(slave);
        _exit(0);
    }

    close(slave);
    write(master, "ping\n", 5);
    waitpid(pid, &status, 0);

    while (total < (int)sizeof(mbuf) - 1) {
        int r = (int)read(master, mbuf + total, sizeof(mbuf) - 1 - (size_t)total);
        if (r <= 0) {
            break;
        }
        total += r;
        mbuf[total] = '\0';
        if (strstr(mbuf, "ping") && strstr(mbuf, "pong")) {
            break;
        }
    }
    mbuf[total > 0 ? total : 0] = '\0';
    close(master);

    if (strstr(mbuf, "ping") && strstr(mbuf, "pong")) {
        printf("PTYMUSL OK\n");
        return 0;
    }
    printf("PTYMUSL FAIL (round-trip)\n");
    return 1;
}
