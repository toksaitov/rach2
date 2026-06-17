/*
 * cowfork.c - Phase 5 integration milestone: fork/wait under musl.
 *
 * An ordinary hosted C program: it fork()s, both sides printf distinct state,
 * and the parent reaps the child with wait(), which musl implements on top of
 * the new wait4(114) syscall and decodes with WIFEXITED/WEXITSTATUS. If it prints
 * "COWFORK OK" the whole Phase 5 path worked end to end: COW fork, the Linux
 * wait-status encoding, and musl's process-control wrappers.
 *
 * Linked statically against the ported musl; see tests/musl/build.sh.
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
        /* Child: its own copy of the address space (COW); a distinct pid. */
        printf("child: pid %d, exiting 7\n", (int)getpid());
        return 7;
    }

    {
        int status = 0;
        pid_t reaped = wait(&status); /* musl wait() -> wait4(-1, &status, 0, 0) */

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
