/**
 * Reads `/proc/usb` and requires the devices the machine was given.
 *
 * The stack's own records say what the kernel found; this is the guest's view
 * of the same thing, which is what a program on the machine can act on. It
 * requires the file to open, a line per device, a keyboard and a mouse among
 * them, and each line to name a port, a speed and a class.
 *
 * A machine with no controller has this file and it is empty. That case is not
 * this probe's: it is checked by the boot that passes `rach2.nousb`, because a
 * missing file and an empty one are different answers and both have to be the
 * right one.
 *
 * Prints "USBPROBE OK" and exits 0. Freestanding; direct int 0x80.
 */

typedef unsigned int size_t;

#define STDOUT_FILENO 1

#define SYS_EXIT  1
#define SYS_READ  3
#define SYS_WRITE 4
#define SYS_OPEN  5
#define SYS_CLOSE 6

#define O_RDONLY 0

static char text[512];

static int syscall3(int n, int a, int b, int c)
{
    int r;
    __asm__ __volatile__("int $0x80" : "=a"(r) : "0"(n), "b"(a), "c"(b), "d"(c) : "memory");
    return r;
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
    syscall3(SYS_WRITE, STDOUT_FILENO, (int)(size_t)s, (int)strlen(s));
}

static void fail(const char *why)
{
    puts("USBPROBE FAIL: ");
    puts(why);
    puts("\n");
    sys_exit(1);
}

/** Whether `needle` appears in the first `len` bytes of `text`. */
static int contains(const char *needle, int len)
{
    int n = (int)strlen(needle);
    int i;
    int j;

    for (i = 0; i + n <= len; i++) {
        for (j = 0; j < n; j++) {
            if (text[i + j] != needle[j]) {
                break;
            }
        }
        if (j == n) {
            return 1;
        }
    }
    return 0;
}

void _start(void)
{
    int fd;
    int len;
    int lines = 0;
    int i;

    fd = syscall3(SYS_OPEN, (int)(size_t)"/proc/usb", O_RDONLY, 0);
    if (fd < 0) {
        fail("/proc/usb did not open");
    }
    len = syscall3(SYS_READ, fd, (int)(size_t)text, (int)sizeof(text) - 1);
    syscall3(SYS_CLOSE, fd, 0, 0);
    if (len <= 0) {
        fail("/proc/usb read nothing");
    }
    text[len] = '\0';

    for (i = 0; i < len; i++) {
        if (text[i] == '\n') {
            lines++;
        }
    }
    if (lines < 2) {
        fail("fewer than two devices are listed");
    }
    if (!contains("port ", len)) {
        fail("no line names a port");
    }
    if (!contains("class 3 protocol 1", len)) {
        fail("no keyboard is listed");
    }
    if (!contains("class 3 protocol 2", len)) {
        fail("no mouse is listed");
    }
    if (!contains("low", len) && !contains("full", len) && !contains("high", len)) {
        fail("no line names a speed");
    }

    puts("USBPROBE OK\n");
    sys_exit(0);
}
