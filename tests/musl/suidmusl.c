/**
 * The setuid-exec contract through the ported musl: prove libc sees the ids
 * and the AT_SECURE flag the kernel set on a set-id exec.
 *
 * Run with no argument, the program forks a child, drops it to uid 1000, and
 * execs the setuid-root copy of itself (`/musl/suidmusl.suid`) with the
 * argument `child`. That instance, entered through the C runtime, checks that
 * getresuid reports real 1000 and effective and saved 0, that
 * getauxval(AT_SECURE) is 1, and that the saved uid lets it shed root to euid
 * 1000 and take it back. The parent relays the child's status.
 *
 * Prints "SUIDMUSL OK" on success, "SUIDMUSL FAIL ..." otherwise. Linked
 * against the ported musl.
 */

#define _GNU_SOURCE /* getresuid, setresuid, setresgid */
#include <stdio.h>
#include <string.h>
#include <unistd.h>
#include <sys/auxv.h>
#include <sys/wait.h>

static int run_child(void)
{
    uid_t r, e, s;

    if (getresuid(&r, &e, &s) != 0) {
        printf("SUIDMUSL FAIL (getresuid)\n");
        return 1;
    }
    if (r != 1000 || e != 0 || s != 0) {
        printf("SUIDMUSL FAIL (ids r=%ld e=%ld s=%ld)\n", (long)r, (long)e, (long)s);
        return 1;
    }
    if (getauxval(AT_SECURE) != 1) {
        printf("SUIDMUSL FAIL (AT_SECURE not 1)\n");
        return 1;
    }
    /* Shed the acquired root and take it back: the saved uid is 0. */
    if (seteuid(1000) != 0 || geteuid() != 1000 || seteuid(0) != 0 || geteuid() != 0) {
        printf("SUIDMUSL FAIL (saved-id dance)\n");
        return 1;
    }
    printf("SUIDMUSL OK\n");
    return 0;
}

int main(int argc, char **argv)
{
    pid_t pid;
    int status;

    if (argc >= 2 && strcmp(argv[1], "child") == 0) {
        return run_child();
    }

    pid = fork();
    if (pid < 0) {
        printf("SUIDMUSL FAIL (fork)\n");
        return 1;
    }
    if (pid == 0) {
        if (setresgid(1000, 1000, 1000) != 0 || setresuid(1000, 1000, 1000) != 0) {
            printf("SUIDMUSL FAIL (drop to 1000)\n");
            _exit(1);
        }
        execl("/musl/suidmusl.suid", "suidmusl", "child", (char *)0);
        printf("SUIDMUSL FAIL (exec setuid copy)\n");
        _exit(1);
    }

    if (waitpid(pid, &status, 0) != pid || !WIFEXITED(status) || WEXITSTATUS(status) != 0) {
        printf("SUIDMUSL FAIL (child status)\n");
        return 1;
    }
    return 0;
}
