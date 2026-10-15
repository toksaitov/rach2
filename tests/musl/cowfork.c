/**
 * cowfork: check fork, wait and the Linux wait-status encoding through musl.
 *
 * The program forks; the child prints its pid and returns 7, and the parent
 * reaps it with `wait`, which musl implements on the wait4 system call, and
 * decodes the status with `WIFEXITED` and `WEXITSTATUS`. The child runs in its
 * own copy-on-write copy of the address space with a pid of its own.
 *
 * It prints `COWFORK OK` when `wait` returns the child's pid with exit status
 * 7, and a `COWFORK FAIL` line naming the step that failed otherwise. Linked
 * statically against the ported musl; see tests/musl/build.sh.
 */

#include <stdio.h>
#include <sys/wait.h>
#include <unistd.h>

int main(void)
{
    pid_t pid = fork();

    if (pid < 0) {
        printf("COWFORK FAIL: fork\n");
        return 1;
    }

    if (pid == 0) {
        printf("child: pid %d, exiting 7\n", (int)getpid());
        return 7;
    }

    {
        int status = 0;
        pid_t reaped = wait(&status);

        if (reaped == pid && WIFEXITED(status) && WEXITSTATUS(status) == 7) {
            printf("COWFORK OK (parent %d reaped child %d, exit %d)\n", (int)getpid(), (int)reaped,
                   WEXITSTATUS(status));
            return 0;
        }
        printf("COWFORK FAIL: wait reaped=%d status=0x%x\n", (int)reaped, status);
        return 1;
    }
}
