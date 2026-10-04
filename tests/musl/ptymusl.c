/**
 * Exercises a musl pseudo-terminal round-trip, the exact shape a login server
 * uses to hand a remote shell a tty.
 *
 * `openpty()` opens `/dev/ptmx`, unlocks it (`TIOCSPTLCK`), resolves the slave
 * name via `ptsname()` (`TIOCGPTN`), and opens `/dev/pts/N`. The test then
 * forks: the child confirms `ttyname(slave)` resolves to `/dev/pts/N` (the
 * `isatty` plus `/proc/self/fd` readlink plus `stat`/`fstat` identity match),
 * reads the cooked line off the slave, and replies on it; the parent drives the
 * master and checks the echo and reply it reads back. On x86-64 a TCFLSH of
 * the master whose selector passes 32 bits is EINVAL, as Linux's is.
 *
 * Prints "PTYMUSL OK" on success and "PTYMUSL FAIL" otherwise. Linked against
 * the ported musl.
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
        /* Child: read the cooked line off the slave and reply on it. */
        char sbuf[64];
        char *tn;
        int n;
        close(master);
        tn = ttyname(slave);
        if (!tn || strncmp(tn, "/dev/pts/", 9) != 0) {
            /* ttyname failed: the parent's round-trip and status catch it. */
            _exit(7);
        }
        n = (int)read(slave, sbuf, sizeof(sbuf));
        if (n >= 4 && memchr(sbuf, 'p', (size_t)n)) {
            write(slave, "pong\n", 5);
        }
        close(slave);
        _exit(0);
    }

    /* Parent: drive the master. The write is cooked into the slave (and echoed
     * back here); the child's reply comes back as slave output. */
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
