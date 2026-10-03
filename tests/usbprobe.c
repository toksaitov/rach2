/**
 * Reads `/proc/usb` and requires the devices the machine was given.
 *
 * `usbprobe N` reads the file again, a bounded number of times, until it
 * lists N devices, and says so: a slowed scan of the hubs may still be
 * configuring them when the shell runs the probe.
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
 * Prints "USBPROBE OK" and exits 0. Freestanding; direct system calls
 * (probe.h).
 */

#include "probe.h"

#define STDOUT_FILENO 1

#define O_RDONLY 0

static char text[4096];

static void sys_exit(int status)
{
    syscall1(SYS_EXIT, (long)status);
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
    syscall3(SYS_WRITE, STDOUT_FILENO, (long)s, (int)strlen(s));
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

/**
 * How many times `needle` appears in the kernel's log.
 *
 * The log is root-only and this probe runs as the transient init the debug
 * shell starts, which is root. A machine where it cannot be opened answers
 * zero, because a check that cannot be made is not a check that failed.
 */
static int count_in_log(const char *needle)
{
    static char chunk[1024];
    int fd;
    int n;
    int found = 0;

    fd = syscall3(SYS_OPEN, (long)"/dev/bootlog", O_RDONLY, 0);
    if (fd < 0) {
        return 0;
    }
    /* Read a piece at a time, and take the overlap into account by counting
       only whole matches inside each piece: the log's lines are far shorter
       than a piece, so a match split across two reads would have to be a line
       longer than this buffer. */
    for (;;) {
        int i;
        int m = (int)strlen(needle);

        n = syscall3(SYS_READ, fd, (long)chunk, (int)sizeof(chunk));
        if (n <= 0) {
            break;
        }
        for (i = 0; i + m <= n; i++) {
            int j;

            for (j = 0; j < m; j++) {
                if (chunk[i + j] != needle[j]) {
                    break;
                }
            }
            if (j == m) {
                found++;
            }
        }
    }
    syscall3(SYS_CLOSE, fd, 0, 0);
    return found;
}

/** Whether the boot line switched USB off. */
/** Whether the boot line has `word` on it. */
static int cmdline_has(const char *word)
{
    static char line[256];
    int fd;
    int n;
    int i;

    fd = syscall3(SYS_OPEN, (long)"/proc/cmdline", O_RDONLY, 0);
    if (fd < 0) {
        return 0;
    }
    n = syscall3(SYS_READ, fd, (long)line, (int)sizeof(line) - 1);
    syscall3(SYS_CLOSE, fd, 0, 0);
    if (n <= 0) {
        return 0;
    }
    line[n] = '\0';
    for (i = 0; i < n; i++) {
        int k = 0;

        while (word[k] && i + k < n && line[i + k] == word[k]) {
            k++;
        }
        if (word[k] == '\0') {
            return 1;
        }
    }
    return 0;
}

static int switched_off(void)
{
    return cmdline_has("rach2.nousb");
}

/* Whether the line at `i` names a device behind a hub: its port is a path
   with a dot in it, `port 5.2`, rather than a root port alone. */
static int behind_hub(int i, int end)
{
    int k;

    for (k = i; k + 5 < end; k++) {
        if (text[k] == 'p' && text[k + 1] == 'o' && text[k + 2] == 'r' && text[k + 3] == 't' &&
            text[k + 4] == ' ') {
            int j = k + 5;

            while (j < end && text[j] != ' ') {
                if (text[j] == '.') {
                    return 1;
                }
                j++;
            }
            return 0;
        }
    }
    return 0;
}

/** Where the line beginning at `from` ends. */
static int line_end(int from, int len)
{
    int i = from;

    while (i < len && text[i] != '\n') {
        i++;
    }
    return i;
}

/** Whether `needle` appears between `from` and `to`. */
static int within(const char *needle, int from, int to)
{
    int n = (int)strlen(needle);
    int i;
    int j;

    for (i = from; i + n <= to; i++) {
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

/* Read /proc/usb whole into `text`, until its end: sixteen devices are more
   than one read of the file returns. Returns the length. */
static int read_usb(void)
{
    int fd = syscall3(SYS_OPEN, (long)"/proc/usb", O_RDONLY, 0);
    int len = 0;

    if (fd < 0) {
        fail("/proc/usb did not open");
    }
    for (;;) {
        int got = syscall3(SYS_READ, fd, (long)text + len, (int)sizeof(text) - 1 - len);

        if (got < 0) {
            fail("/proc/usb did not read");
        }
        if (got == 0) {
            break;
        }
        len += got;
        if (len >= (int)sizeof(text) - 1) {
            fail("/proc/usb is longer than the probe's buffer");
        }
    }
    syscall3(SYS_CLOSE, fd, 0, 0);
    text[len] = '\0';
    return len;
}

static int count_lines(int len)
{
    int i, lines = 0;

    for (i = 0; i < len; i++) {
        if (text[i] == '\n') {
            lines++;
        }
    }
    return lines;
}

void probe_main(unsigned int *sp);

__asm__(".global _start\n"
        "_start:\n"
        "   mov %esp, %eax\n"
        "   push %eax\n"
        "   call probe_main\n");

/**
 * The probe: read the USB device table and say what it lists. With an
 * argument N the machine was given N devices and a slowed scan may still
 * be configuring them, so the file is read again until it lists that many,
 * a bounded number of times, and the count is said.
 */
void probe_main(unsigned int *sp)
{
    int argc = (int)sp[0];
    char **argv = (char **)(sp + 1);
    int want = 0;
    int len;
    int lines = 0;
    int i;

    if (argc >= 2) {
        for (i = 0; argv[1][i] >= '0' && argv[1][i] <= '9'; i++) {
            want = want * 10 + (argv[1][i] - '0');
        }
    }
    len = read_usb();
    if (want > 0) {
        int tries;

        for (tries = 0; count_lines(len) < want && tries < 600; tries++) {
            static const int pause[2] = {0, 100000000};

            syscall3(SYS_NANOSLEEP, (long)pause, 0, 0);
            len = read_usb();
        }
        if (count_lines(len) < want) {
            fail("fewer devices than the machine was given, after the wait");
        }
        puts("USBPROBE settled: the devices the machine was given are listed\n");
    }

    /* A machine booted without USB has this file and it is empty, which is a
       different answer from the file not being there and the one a program
       that opens it can act on. Which of the two is expected is decided by the
       boot line, which is the only thing here that knows. */
    if (switched_off()) {
        if (len != 0) {
            fail("USB was switched off and devices are listed");
        }
        puts("USBPROBE OK\n");
        sys_exit(0);
    }
    if (len == 0) {
        fail("/proc/usb is empty and USB was not switched off");
    }

    lines = count_lines(len);
    if (lines < 2) {
        fail("fewer than two devices are listed");
    }
    /* Every line, not the file as a whole: a file whose first line held every
       word this looks for and whose second was empty would pass a search over
       the whole of it. */
    for (i = 0; i < len; i = line_end(i, len) + 1) {
        int end = line_end(i, len);

        if (end == i) {
            continue;
        }
        if (!within("port ", i, end)) {
            fail("a line names no port");
        }
        if (!within("low", i, end) && !within("full", i, end) && !within("high", i, end) &&
            !within("super", i, end)) {
            fail("a line names no speed");
        }
        if (!within(":", i, end)) {
            fail("a line names no device");
        }
        if (!within(" class ", i, end)) {
            fail("a line names no class");
        }
    }
    if (!contains("class 3 protocol 1", len)) {
        fail("no keyboard is listed");
    }
    if (!contains("class 3 protocol 2", len)) {
        fail("no mouse is listed");
    }
    /* On a boot that put them behind a hub, the boot line says so, and the
       keyboard's and the mouse's lines must give a path and not a root port:
       a device behind a hub that was listed on a root port would be a tree
       the kernel had lost. */
    if (cmdline_has("rach2.hubtree")) {
        for (i = 0; i < len; i = line_end(i, len) + 1) {
            int end = line_end(i, len);

            if (end == i) {
                continue;
            }
            if ((within("class 3 protocol 1", i, end) || within("class 3 protocol 2", i, end)) &&
                !behind_hub(i, end)) {
                fail("a device behind the hub is listed on a root port");
            }
        }
        if (!contains("class 9 ", len)) {
            fail("the hub itself is not listed");
        }
    }

    /*
     * And the log does not repeat itself. A device this kernel cannot drive is
     * refused once and then left alone; the notebook found one that was
     * enumerated again every tick until the slots ran out, which cost the
     * keyboard that was already working. One line is the contract.
     */
    {
        int refusals = count_in_log("no interface this kernel can drive");

        if (refusals > 1) {
            fail("a device that cannot be driven was refused more than once");
        }
    }

    puts("USBPROBE OK\n");
    sys_exit(0);
}
