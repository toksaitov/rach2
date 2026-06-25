/*
 * hello.c - Minimal ELF test program for Rach2 OS
 *
 * This is a freestanding program (no libc) that uses inline assembly
 * to make syscalls directly. It prints "Hello from ELF!" and exits.
 *
 * Compile with (on macOS with cross-compiler):
 *   i686-elf-gcc -m32 -nostdlib -static -ffreestanding -Wl,-Ttext=0x08048000 -o hello hello.c
 */

typedef unsigned int size_t;

#define STDOUT_FILENO 1

/* Syscall numbers (32-bit Linux / Rach2 compatible) */
#define SYS_EXIT  1
#define SYS_WRITE 4

/*
 * sys_write - Write to a file descriptor
 * Uses int 0x80 with: EAX=4, EBX=fd, ECX=buf, EDX=count
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
 * Uses int 0x80 with: EAX=1, EBX=status
 */
static void sys_exit(int status)
{
    __asm__ __volatile__ (
        "int $0x80"
        :
        : "a"(SYS_EXIT), "b"(status)
    );
    /* Should never return, but prevent compiler warning */
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
 * _start - ELF entry point (no libc, no main)
 *
 * This is the actual entry point that the ELF loader jumps to.
 * The stack layout on entry (if argc/argv are passed) would be:
 *   [ESP+0]  argc
 *   [ESP+4]  argv[0]
 *   [ESP+8]  argv[1]
 *   ...
 *
 * Arguments are ignored; prints a fixed message.
 */
void _start(void)
{
    puts("Hello from ELF!");
    sys_exit(0);
}
