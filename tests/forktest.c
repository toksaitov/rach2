/*
 * forktest.c - Process lifecycle syscall test for Rach2 OS
 *
 * Tests sys_fork, sys_waitpid, and sys_exit syscalls by forking
 * a child process that exits with a known status, then verifying
 * the parent receives the correct exit status via waitpid.
 *
 * Compile with:
 *   i686-elf-gcc -m32 -nostdlib -static -ffreestanding -Wl,-Ttext=0x08048000 -o forktest forktest.c
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

/*
 * sys_write - Write to a file descriptor
 */
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

/*
 * sys_exit - Terminate the process
 */
static void sys_exit(int status)
{
    __asm__ __volatile__ (
        "int $0x80"
        :
        : "a"(SYS_EXIT), "b"(status)
    );
    __builtin_unreachable();
}

/*
 * sys_fork - Create a child process
 * Returns: 0 to child, child's PID to parent, -1 on error
 */
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

/*
 * sys_waitpid - Wait for child process to exit
 * Args: pid (-1 for any child), status pointer, options (0)
 * Returns: PID of exited child, or -1 on error
 */
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

/*
 * strlen - Calculate string length
 */
static size_t strlen(const char *s)
{
    size_t len = 0;
    while (s[len]) {
        len++;
    }
    return len;
}

/*
 * puts - Print string followed by newline
 */
static void puts(const char *s)
{
    sys_write(STDOUT_FILENO, s, strlen(s));
    sys_write(STDOUT_FILENO, "\n", 1);
}

/*
 * print - Print string without newline
 */
static void print(const char *s)
{
    sys_write(STDOUT_FILENO, s, strlen(s));
}

/*
 * print_num - Print a decimal number
 */
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

    /* Reverse and print */
    while (i > 0) {
        sys_write(STDOUT_FILENO, &buf[--i], 1);
    }
}

/*
 * _start - ELF entry point
 *
 * Tests fork/waitpid:
 * 1. Parent forks a child
 * 2. Child prints message and exits with status 42
 * 3. Parent waits for child and verifies exit status
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
        /* Child process */
        puts("OK (child)");
        print("Child: PID is 0 (as expected), exiting with status ");
        print_num(CHILD_EXIT_STATUS);
        puts("");
        sys_exit(CHILD_EXIT_STATUS);
        /* Never reached */
    }

    /* Parent process */
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
    print(", status=");
    print_num(status);
    puts("");

    if (status == CHILD_EXIT_STATUS) {
        puts("All fork/waitpid syscalls passed!");
        sys_exit(0);
    } else {
        print("FAILED: Expected status ");
        print_num(CHILD_EXIT_STATUS);
        print(", got ");
        print_num(status);
        puts("");
        sys_exit(1);
    }
}
