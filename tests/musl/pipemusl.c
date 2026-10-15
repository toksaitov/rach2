/**
 * pipemusl: check a pipe between a parent and its forked child through musl.
 *
 * The parent writes the eleven bytes "hello, pipe" into the pipe and closes
 * the write end; the child reads once and exits 0 only if the bytes match.
 * The child's exit status decides the token: `PIPEMUSL OK`, or a
 * `PIPEMUSL FAIL` line naming the step that failed. Staged into the test
 * image as /musl/pipemusl.
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
