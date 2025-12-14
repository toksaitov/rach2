/*
 * shelltest.c - Shell-like exec syscall test for Rach2 OS
 *
 * Tests the fork+exec+waitpid chain that shells use to run programs.
 * Forks a child that exec's "hello", parent waits and verifies exit status.
 *
 * Compile with:
 *   i686-elf-gcc -m32 -nostdlib -static -ffreestanding -Wl,-Ttext=0x08048000 -o shelltest shelltest.c
 */

typedef unsigned int size_t;

#define STDOUT_FILENO 1

/* Syscall numbers (32-bit Linux / Rach2 compatible) */
#define SYS_EXIT    1
#define SYS_FORK    2
#define SYS_WRITE   4
#define SYS_WAITPID 7
#define SYS_EXECVE  11

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
 * sys_execve - Replace process image with new program
 * Args: path to executable, argv array (NULL-terminated), envp array (NULL-terminated)
 * Returns: only on error (-1), otherwise doesn't return
 */
static int sys_execve(const char *path, char *const argv[], char *const envp[])
{
    int result;
    __asm__ __volatile__ (
        "int $0x80"
        : "=a"(result)
        : "0"(SYS_EXECVE), "b"(path), "c"(argv), "d"(envp)
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
 * Tests fork+exec+waitpid (shell pattern):
 * 1. Parent forks a child
 * 2. Child exec's "hello" program
 * 3. Parent waits for child and verifies exit status (hello exits with 0)
 */
void _start(void)
{
    int pid;
    int status;
    int child_pid;
    int exec_result;

    /* Program to execute - "hello" should print message and exit(0) */
    const char *program = "hello";
    char *const argv[] = { "hello", (char *)0 };
    char *const envp[] = { (char *)0 };

    puts("Shell test (fork+exec+waitpid) starting...");

    print("Forking... ");
    pid = sys_fork();

    if (pid < 0) {
        puts("FAILED (fork returned -1)");
        sys_exit(1);
    }

    if (pid == 0) {
        /* Child process - exec the program */
        print("OK (child)\n");
        print("Child: Calling execve(\"");
        print(program);
        print("\")... ");

        exec_result = sys_execve(program, argv, envp);

        /* If we get here, exec failed */
        print("FAILED (exec returned ");
        print_num(exec_result);
        puts(")");
        sys_exit(1);
        /* Never reached if exec succeeds */
    }

    /* Parent process */
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

    /* "hello" program should exit with status 0 */
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
