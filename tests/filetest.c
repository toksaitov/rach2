/*
 * filetest.c - File syscall test program for Rach2 OS
 *
 * Tests sys_open, sys_read, sys_write, and sys_close syscalls
 * by opening a file, reading its contents, and printing to stdout.
 *
 * Compile with:
 *   i686-elf-gcc -m32 -nostdlib -static -ffreestanding -Wl,-Ttext=0x08048000 -o filetest filetest.c
 */

typedef unsigned int size_t;

#define STDIN_FILENO  0
#define STDOUT_FILENO 1
#define STDERR_FILENO 2

/* Syscall numbers (32-bit Linux / Rach2 compatible) */
#define SYS_EXIT  1
#define SYS_READ  3
#define SYS_WRITE 4
#define SYS_OPEN  5
#define SYS_CLOSE 6

/* Open flags */
#define O_RDONLY 0

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
 * sys_read - Read from a file descriptor
 */
static int sys_read(int fd, void *buf, size_t count)
{
    int result;
    __asm__ __volatile__ (
        "int $0x80"
        : "=a"(result)
        : "0"(SYS_READ), "b"(fd), "c"(buf), "d"(count)
        : "memory"
    );
    return result;
}

/*
 * sys_open - Open a file
 */
static int sys_open(const char *path, int flags)
{
    int result;
    __asm__ __volatile__ (
        "int $0x80"
        : "=a"(result)
        : "0"(SYS_OPEN), "b"(path), "c"(flags)
        : "memory"
    );
    return result;
}

/*
 * sys_close - Close a file descriptor
 */
static int sys_close(int fd)
{
    int result;
    __asm__ __volatile__ (
        "int $0x80"
        : "=a"(result)
        : "0"(SYS_CLOSE), "b"(fd)
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
 * _start - ELF entry point
 *
 * Opens "hello.txt", reads its contents, and prints them.
 * This tests the file syscalls: open, read, write, close.
 */
void _start(void)
{
    char buf[256];
    int fd;
    int n;

    print("Opening hello.txt... ");

    fd = sys_open("hello.txt", O_RDONLY);
    if (fd < 0) {
        puts("FAILED");
        sys_exit(1);
    }
    puts("OK");

    print("Reading file... ");
    n = sys_read(fd, buf, sizeof(buf) - 1);
    if (n < 0) {
        puts("FAILED");
        sys_close(fd);
        sys_exit(1);
    }
    buf[n] = '\0';
    puts("OK");

    print("Content: ");
    puts(buf);

    print("Closing file... ");
    if (sys_close(fd) < 0) {
        puts("FAILED");
        sys_exit(1);
    }
    puts("OK");

    puts("All file syscalls passed!");
    sys_exit(0);
}
