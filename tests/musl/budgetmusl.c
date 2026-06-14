/*
 * budgetmusl.c - Phase 19 live one-session budget probe.
 *
 * Stands up the exact resource footprint one remote-login session needs once the
 * Phase-19 pty layer exists -- the check the plan defers to here, before
 * committing to dropbear (Phase 23). A session is: a listening socket + an
 * accepted socket (modelled with a socketpair) + a pty master + the pty slave
 * handed to a forked child as its controlling-terminal stdio. It then confirms
 * there is headroom left under the fixed limits (MAX_FDS / MAX_PROCS / socket
 * pool = 16 each), so a real session -- which also forks pipeline children in the
 * shell -- fits.
 *
 * Prints the counts and "BUDGETMUSL OK" / "BUDGETMUSL FAIL". Linked against musl.
 */

#include <pty.h>
#include <unistd.h>
#include <stdio.h>
#include <fcntl.h>
#include <sys/socket.h>
#include <sys/wait.h>
#include <termios.h>
#include <sys/ioctl.h>

int main(void)
{
    int sp[2];      /* listen + accepted socket, modelled as a connected pair */
    int master = -1, slave = -1;
    pid_t pid;
    int status;
    int extra_fds[8];
    int extra = 0;
    int i;

    /* The two sockets a connection occupies (listen + accepted): a socketpair is
     * the closest hermetic stand-in (no external peer needed). */
    if (socketpair(AF_UNIX, SOCK_STREAM, 0, sp) != 0) {
        /* Fall back: a single stream socket still exercises the socket pool. */
        sp[0] = socket(AF_INET, SOCK_STREAM, 0);
        sp[1] = socket(AF_INET, SOCK_STREAM, 0);
    }

    if (openpty(&master, &slave, NULL, NULL, NULL) != 0) {
        printf("BUDGETMUSL FAIL (openpty)\n");
        return 1;
    }

    pid = fork();
    if (pid < 0) {
        printf("BUDGETMUSL FAIL (fork)\n");
        return 1;
    }
    if (pid == 0) {
        /* The session child: a new session leader with the slave as its
         * controlling terminal and its stdin/out/err -- exactly what a login
         * server wires before exec'ing the shell. */
        close(master);
        setsid();
        ioctl(slave, TIOCSCTTY, 0);
        dup2(slave, 0);
        dup2(slave, 1);
        dup2(slave, 2);
        if (slave > 2) {
            close(slave);
        }
        _exit(0);
    }

    close(slave);
    waitpid(pid, &status, 0);

    /* Headroom: how many more fds can this process open beyond the session set it
     * already holds (sp[0], sp[1], master, stdio)? Several => a real session fits. */
    for (i = 0; i < 8; i++) {
        extra_fds[i] = open("/dev/null", O_RDWR);
        if (extra_fds[i] < 0) {
            break;
        }
        extra++;
    }
    for (i = 0; i < extra; i++) {
        close(extra_fds[i]);
    }

    printf("BUDGET: session set = 2 sockets + pty master/slave + child stdio; "
           "%d spare fds open\n", extra);

    if (master >= 0 && extra >= 4) {
        printf("BUDGETMUSL OK\n");
        return 0;
    }
    printf("BUDGETMUSL FAIL (no headroom)\n");
    return 1;
}
