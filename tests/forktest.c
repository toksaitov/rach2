/**
 * Exercises the process-lifecycle syscalls: fork a child that exits with a
 * known status, then confirm the parent reads back that status through waitpid.
 *
 * Freestanding; direct int 0x80.
 */

typedef unsigned int size_t;

#define STDOUT_FILENO 1

/* Syscall numbers (32-bit Linux / Rach2 compatible) */
#define SYS_EXIT    1
#define SYS_FORK    2
#define SYS_WRITE   4
#define SYS_WAITPID 7

/* Exit status for child to return */
#define CHILD_EXIT_STATUS 42

/* Linux wait-status decoding (the kernel encodes a normal exit as code<<8). */
#define WIFEXITED(s)   (((s) & 0x7F) == 0)
#define WEXITSTATUS(s) (((s) >> 8) & 0xFF)

static int sys_write(int fd, const void *buf, size_t count)
{
    int result;
    __asm__ __volatile__ (
        "int $0x80"
        : "=a"(result)
        : "0"(SYS_WRITE), "b"(fd), "c"(buf), "d"(count)
        : "memory"
    );
    return result;
}

static void sys_exit(int status)
{
    __asm__ __volatile__ (
        "int $0x80"
        :
        : "a"(SYS_EXIT), "b"(status)
    );
    __builtin_unreachable();
}

static int sys_fork(void)
{
    int result;
    __asm__ __volatile__ (
        "int $0x80"
        : "=a"(result)
        : "0"(SYS_FORK)
    );
    return result;
}

static int sys_waitpid(int pid, int *status, int options)
{
    int result;
    __asm__ __volatile__ (
        "int $0x80"
        : "=a"(result)
        : "0"(SYS_WAITPID), "b"(pid), "c"(status), "d"(options)
        : "memory"
    );
    return result;
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
 * ELF entry point and the test driver.
 *
 * The parent forks a child that exits with `CHILD_EXIT_STATUS`, then waits and
 * checks that `WIFEXITED` and `WEXITSTATUS` report that status back.
 */
void _start(void)
{
    int pid;
    int status;
    int child_pid;

    puts("Fork test starting...");

    print("Calling fork()... ");
    pid = sys_fork();

    if (pid < 0) {
        puts("FAILED");
        sys_exit(1);
    }

    if (pid == 0) {
        puts("OK (child)");
        print("Child: PID is 0 (as expected), exiting with status ");
        print_num(CHILD_EXIT_STATUS);
        puts("");
        sys_exit(CHILD_EXIT_STATUS);
    }

    print("OK (parent, child PID=");
    print_num(pid);
    puts(")");

    print("Parent: Calling waitpid()... ");
    child_pid = sys_waitpid(pid, &status, 0);

    if (child_pid < 0) {
        puts("FAILED");
        sys_exit(1);
    }
    puts("OK");

    print("Parent: waitpid returned PID=");
    print_num(child_pid);
    print(", exit status=");
    print_num(WEXITSTATUS(status));
    puts("");

    if (WIFEXITED(status) && WEXITSTATUS(status) == CHILD_EXIT_STATUS) {
        puts("All fork/waitpid syscalls passed!");
        sys_exit(0);
    } else {
        print("FAILED: Expected exit status ");
        print_num(CHILD_EXIT_STATUS);
        print(", got ");
        print_num(WEXITSTATUS(status));
        puts("");
        sys_exit(1);
    }
}
