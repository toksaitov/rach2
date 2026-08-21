/**
 * Probes that a user touch of the page-cache window is killed, not honored.
 *
 * `PCWIN_BASE` (0xD0000000) is the base of the kernel-only demand-paged disk
 * window. A user process has no business there; the segments are flat, so the
 * read reaches the fault handler, which must treat a user-mode in-window fault
 * as an illegal access and segfault the process. The window branch is for the
 * supervisor-mode faults the file system raises, not for ring 3: serving a
 * ring-3 fault would map a supervisor-only page it loops on, or, with no disk
 * backing, panic the whole kernel. The "still alive" line must NEVER appear.
 *
 * Run as a standalone boot (it kills itself); the harness then checks that the
 * kernel survived and the shell came back. Freestanding; direct int 0x80.
 */

typedef unsigned int size_t;

#define STDOUT_FILENO 1

#define SYS_EXIT  1
#define SYS_WRITE 4

#define PCWIN_BASE 0xD0000000u

static int sys_write(int fd, const void *buf, size_t count)
{
    int result;
    __asm__ __volatile__("int $0x80"
                         : "=a"(result)
                         : "0"(SYS_WRITE), "b"(fd), "c"(buf), "d"(count)
                         : "memory");
    return result;
}

static void sys_exit(int status)
{
    __asm__ __volatile__("int $0x80" : : "a"(SYS_EXIT), "b"(status));
    __builtin_unreachable();
}

static size_t strlen(const char *s)
{
    size_t n = 0;
    while (s[n]) {
        n++;
    }
    return n;
}

static void puts(const char *s)
{
    sys_write(STDOUT_FILENO, s, strlen(s));
    sys_write(STDOUT_FILENO, "\n", 1);
}

void _start(void)
{
    volatile unsigned *win = (volatile unsigned *)PCWIN_BASE;
    unsigned sink;

    puts("winprobe: about to read the page-cache window");

    sink = *win; /* illegal from ring 3: the kernel must kill us, not panic */
    (void)sink;

    /* If the window branch wrongly served a ring-3 fault, we would reach
     * here. */
    puts("winprobe: ERROR still alive after window read");
    sys_exit(0);
}
