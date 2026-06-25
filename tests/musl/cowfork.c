/**
 * Exercises fork/wait through real musl: COW fork, the Linux wait-status
 * encoding, and musl's process-control wrappers.
 *
 * An ordinary hosted C program. It fork()s, both sides printf distinct state,
 * and the parent reaps the child with wait(), which musl implements on top of
 * the wait4(114) syscall and decodes with WIFEXITED/WEXITSTATUS. The child runs
 * in its own copy of the address space (COW) with a distinct pid and exits 7;
 * the parent must reap exactly that child and read back exit status 7.
 *
 * "COWFORK OK" means the whole path worked end to end; any "COWFORK FAIL" line
 * names the step that broke. Linked statically against the ported musl; see
 * tests/musl/build.sh.
 */

#include <stdio.h>
#include <sys/wait.h>
#include <unistd.h>

int main(void)
{
    pid_t pid = fork();

    if (pid < 0)
    {
        printf("COWFORK FAIL: fork\n");
        return 1;
    }

    if (pid == 0)
    {
        /* Child. */
        printf("child: pid %d, exiting 7\n", (int)getpid());
        return 7;
    }

    {
        int status = 0;
        /* musl wait() -> wait4(-1, &status, 0, 0). */
        pid_t reaped = wait(&status);

        if (reaped == pid && WIFEXITED(status) && WEXITSTATUS(status) == 7)
        {
            printf("COWFORK OK (parent %d reaped child %d, exit %d)\n",
                   (int)getpid(), (int)reaped, WEXITSTATUS(status));
            return 0;
        }
        printf("COWFORK FAIL: wait reaped=%d status=0x%x\n", (int)reaped, status);
        return 1;
    }
}
