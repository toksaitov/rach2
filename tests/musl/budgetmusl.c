/**
 * budgetmusl: open the descriptors one remote-login session needs, then count
 * how many more the process can open, through musl.
 *
 * A session is modelled as a connected socket pair (the listening socket plus
 * the accepted socket), a pty master, and the pty slave handed to a forked
 * child as its controlling-terminal stdio. When `socketpair` fails, the
 * program opens two unconnected TCP sockets in its place and checks neither.
 * The child becomes a session leader (`setsid`), claims the slave as its
 * controlling terminal (`TIOCSCTTY`), and dups it onto stdin, stdout, and
 * stderr, as a login server sets up a shell before exec, and exits. The
 * parent, still holding the sockets and the master, then opens `/dev/null` up
 * to eight more times; at least four spare descriptors leave room for the
 * pipelines a session's shell forks.
 *
 * Prints the spare-fd count, then "BUDGETMUSL OK" on success or a
 * "BUDGETMUSL FAIL (...)" line naming the failed step. No suite runs it; it is
 * a diagnostic run by hand. Linked against musl.
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
    int sp[2]; /* listen + accepted socket, modelled as a connected pair */
    int master = -1, slave = -1;
    pid_t pid;
    int status;
    int extra_fds[8];
    int extra = 0;
    int i;

    if (socketpair(AF_UNIX, SOCK_STREAM, 0, sp) != 0) {
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
           "%d spare fds open\n",
           extra);

    if (master >= 0 && extra >= 4) {
        printf("BUDGETMUSL OK\n");
        return 0;
    }
    printf("BUDGETMUSL FAIL (no headroom)\n");
    return 1;
}
