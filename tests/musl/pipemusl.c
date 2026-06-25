/**
 * Drives a real musl pipe and fork round-trip through the kernel.
 *
 * Exercises musl's `pipe`, `fork`, `read`, `write`, `close`, and `waitpid`
 * wrappers end to end, not just the freestanding probe. The parent writes a
 * line into the pipe and closes the write end; the forked child reads it back
 * and exits 0 only if the eleven bytes match "hello, pipe". The child's exit
 * status decides the result token.
 *
 * Installed as "mpipe" by krffs/build_test_ramdisk.sh.
 */
#include <unistd.h>
#include <stdio.h>
#include <string.h>
#include <sys/wait.h>

int main(void)
{
    int p[2];
    pid_t pid;

    if (pipe(p) != 0) {
        printf("PIPEMUSL FAIL: pipe\n");
        return 1;
    }

    pid = fork();
    if (pid < 0) {
        printf("PIPEMUSL FAIL: fork\n");
        return 1;
    }

    if (pid == 0) {
        char buf[64];
        ssize_t n;
        close(p[1]);
        n = read(p[0], buf, sizeof(buf));
        _exit((n == 11 && memcmp(buf, "hello, pipe", 11) == 0) ? 0 : 1);
    } else {
        int status;
        close(p[0]);
        write(p[1], "hello, pipe", 11);
        close(p[1]); /* EOF for the reader */
        waitpid(pid, &status, 0);
        if (WIFEXITED(status) && WEXITSTATUS(status) == 0) {
            printf("PIPEMUSL OK\n");
        } else {
            printf("PIPEMUSL FAIL: child did not receive the data\n");
        }
    }
    return 0;
}
