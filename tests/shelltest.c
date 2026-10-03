/**
 * Exercises the fork+exec+waitpid chain that a shell uses to run a program.
 *
 * The child exec's `hello`; the parent waits and checks its exit status.
 * Freestanding; direct system calls (probe.h).
 */

#include "probe.h"

#define STDOUT_FILENO 1

/* Syscall numbers (32-bit Linux / Rach2 compatible) */

static int sys_write(int fd, const void *buf, size_t count)
{
    return syscall3(SYS_WRITE, (long)fd, (long)buf, (long)count);
}

static void sys_exit(int status)
{
    syscall1(SYS_EXIT, (long)status);
    __builtin_unreachable();
}

static int sys_fork(void)
{
    return syscall0(SYS_FORK);
}

static int sys_waitpid(int pid, int *status, int options)
{
    return syscall3(SYS_WAITPID, (long)pid, (long)status, (long)options);
}

static int sys_execve(const char *path, char *const argv[], char *const envp[])
{
    return syscall3(SYS_EXECVE, (long)path, (long)argv, (long)envp);
}

static size_t strlen(const char *s)
{
    size_t len = 0;
    while (s[len]) {
        len++;
    }
    return len;
}

static void puts(const char *s)
{
    sys_write(STDOUT_FILENO, s, strlen(s));
    sys_write(STDOUT_FILENO, "\n", 1);
}

static void print(const char *s)
{
    sys_write(STDOUT_FILENO, s, strlen(s));
}

static void print_num(int n)
{
    char buf[12];
    int i = 0;
    int neg = 0;

    if (n < 0) {
        neg = 1;
        n = -n;
    }
    if (n == 0) {
        buf[i++] = '0';
    } else {
        while (n > 0) {
            buf[i++] = '0' + (n % 10);
            n /= 10;
        }
    }
    if (neg) {
        buf[i++] = '-';
    }

    while (i > 0) {
        sys_write(STDOUT_FILENO, &buf[--i], 1);
    }
}

/**
 * ELF entry point: runs the fork+exec+waitpid shell pattern.
 *
 * The parent forks; the child exec's `hello`; the parent waits and verifies the
 * child exited with status 0.
 *
 * The program it execs is staged at the image's root and is named by a path
 * rather than by a bare word, so a run whose working directory is not the
 * root fails on what the kernel did and not on where it was started.
 */
PROBE_START void _start(void)
{
    int pid;
    int status;
    int child_pid;
    int exec_result;

    const char *program = "/hello";
    char *const argv[] = {"/hello", (char *)0};
    char *const envp[] = {(char *)0};

    puts("Shell test (fork+exec+waitpid) starting...");

    print("Forking... ");
    pid = sys_fork();

    if (pid < 0) {
        puts("FAILED (fork returned -1)");
        sys_exit(1);
    }

    if (pid == 0) {
        print("OK (child)\n");
        print("Child: Calling execve(\"");
        print(program);
        print("\")... ");

        exec_result = sys_execve(program, argv, envp);

        print("FAILED (exec returned ");
        print_num(exec_result);
        puts(")");
        sys_exit(1);
    }

    print("OK (parent, child PID=");
    print_num(pid);
    puts(")");

    print("Parent: Waiting for child... ");
    child_pid = sys_waitpid(pid, &status, 0);

    if (child_pid < 0) {
        puts("FAILED (waitpid returned -1)");
        sys_exit(1);
    }
    puts("OK");

    print("Parent: Child PID=");
    print_num(child_pid);
    print(" exited with status=");
    print_num(status);
    puts("");

    if (status == 0) {
        puts("All exec syscalls passed!");
        sys_exit(0);
    } else {
        print("Note: hello exited with non-zero status ");
        print_num(status);
        puts(" (may indicate exec issue)");
        sys_exit(1);
    }
}
