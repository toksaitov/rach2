/**
 * Reads `/proc/interrupts` and requires what this boot's options say it must
 * hold.
 *
 * Four machines are checked by the one program, because what the file must
 * say is decided by the boot line and the device list, both of which the
 * guest can read for itself:
 *
 * * With a USB device and messages, which is the ordinary boot: the
 *   controller's vector has taken at least one interrupt per key typed
 *   before this ran, and the timer's pump has found none of the work they
 *   carry, so the fallback count is zero.
 * * With `rach2.nomsi`: the vector has no line at all, since a vector that
 *   has taken nothing is not listed, and the fallback count is zero because
 *   nothing was ever on messages.
 * * With `rach2.msidrop=N`: the controller was silenced after N messages, so
 *   the vector's count is exactly N and the timer's pump has been finding
 *   the work since, which is what the fallback count is for.
 * * With no USB device on the machine: nothing sends a message, so the
 *   vector has no line and the fallback count is zero. The timer's own
 *   vector is required to have counted in every one of them, which on a
 *   machine with the APIC enabled is what says the 8259's interrupts still
 *   arrive.
 *
 * Prints "INTRPROBE OK" with the two numbers and exits 0. Freestanding;
 * direct int 0x80.
 */

typedef unsigned int size_t;

#define STDOUT_FILENO 1

#define SYS_EXIT  1
#define SYS_READ  3
#define SYS_WRITE 4
#define SYS_OPEN  5
#define SYS_CLOSE 6

#define O_RDONLY 0

/* The vector the xHCI's messages arrive on, which the kernel fixes at 48. */
#define USB_VECTOR "48:"

static char text[2048];
static char cmdline[512];

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

static void putnum(int v)
{
    char digits[12];
    int n = 0;

    if (v < 0) {
        puts("-");
        v = -v;
    }
    if (v == 0) {
        puts("0");
        return;
    }
    while (v > 0 && n < 11) {
        digits[n++] = (char)('0' + (v % 10));
        v /= 10;
    }
    while (n > 0) {
        char one[2];

        one[0] = digits[--n];
        one[1] = '\0';
        puts(one);
    }
}

static void fail(const char *why)
{
    puts("INTRPROBE FAIL: ");
    puts(why);
    puts("\n");
    sys_exit(1);
}

/** Read a whole file into `buf`, or -1. */
static int slurp(const char *path, char *buf, int cap)
{
    int fd = syscall3(SYS_OPEN, (int)(size_t)path, O_RDONLY, 0);
    int done = 0;

    if (fd < 0) {
        return -1;
    }
    for (;;) {
        int n = syscall3(SYS_READ, fd, (int)(size_t)(buf + done), cap - 1 - done);

        if (n <= 0) {
            break;
        }
        done += n;
        if (done >= cap - 1) {
            break;
        }
    }
    syscall3(SYS_CLOSE, fd, 0, 0);
    buf[done] = '\0';
    return done;
}

/** Where `needle` starts in `hay`, or -1. */
static int find(const char *hay, const char *needle)
{
    int n = (int)strlen(needle);
    int i, j;

    for (i = 0; hay[i] != '\0'; i++) {
        for (j = 0; j < n && hay[i + j] == needle[j]; j++) {
            /* compare */
        }
        if (j == n) {
            return i;
        }
    }
    return -1;
}

/**
 * The count on the line that begins with `label`, or -1 when there is none.
 *
 * A line is `label`, a tab, the count, a tab, and the rest; the label is
 * matched at the start of a line so that a count of 48 elsewhere in the file
 * cannot be read as the line for vector 48.
 */
static int count_for(const char *label, const char *file)
{
    int i = 0;
    int n = (int)strlen(label);

    while (file[i] != '\0') {
        int j;
        int start = i;

        for (j = 0; j < n && file[start + j] == label[j]; j++) {
            /* compare */
        }
        if (j == n) {
            int value = 0;
            int k = start + n;

            while (file[k] == ' ' || file[k] == '\t') {
                k++;
            }
            if (file[k] < '0' || file[k] > '9') {
                return -1;
            }
            while (file[k] >= '0' && file[k] <= '9') {
                value = value * 10 + (file[k] - '0');
                k++;
            }
            return value;
        }
        while (file[i] != '\0' && file[i] != '\n') {
            i++;
        }
        if (file[i] == '\n') {
            i++;
        }
    }
    return -1;
}

/**
 * How many USB devices this machine has configured.
 *
 * `/proc/usb` has one line per device and is empty on a machine with none,
 * which is the difference between a boot that must have taken messages and
 * one that had nothing to send them.
 */
static int usb_devices(void)
{
    static char list[1024];
    int n = slurp("/proc/usb", list, (int)sizeof(list));
    int lines = 0;
    int i;

    if (n <= 0) {
        return 0;
    }
    for (i = 0; i < n; i++) {
        if (list[i] == '\n') {
            lines++;
        }
    }
    return lines;
}

/** The number after `key` on the boot line, or -1 when it is not there. */
static int option_value(const char *key)
{
    int at = find(cmdline, key);
    int value = 0;

    if (at < 0) {
        return -1;
    }
    at += (int)strlen(key);
    if (cmdline[at] < '0' || cmdline[at] > '9') {
        return -1;
    }
    while (cmdline[at] >= '0' && cmdline[at] <= '9') {
        value = value * 10 + (cmdline[at] - '0');
        at++;
    }
    return value;
}

void _start(void)
{
    int vector, fallback, drop, devices;

    if (slurp("/proc/cmdline", cmdline, (int)sizeof(cmdline)) < 0) {
        fail("/proc/cmdline would not open");
    }
    if (slurp("/proc/interrupts", text, (int)sizeof(text)) < 0) {
        fail("/proc/interrupts would not open");
    }
    if (count_for("32:", text) <= 0) {
        fail("the timer's vector has taken no interrupts");
    }
    vector = count_for(USB_VECTOR, text);
    fallback = count_for("FBK:", text);
    if (fallback < 0) {
        fail("there is no fallback line");
    }
    drop = option_value("rach2.msidrop=");
    devices = usb_devices();

    /* What the file said, before anything is required of it: a run that fails
       is worth more when the numbers it failed on are in the log. */
    puts("INTRPROBE READ vector ");
    putnum(vector);
    puts(", fallback ");
    putnum(fallback);
    puts("\n");

    if (find(cmdline, "rach2.nomsi") >= 0) {
        if (vector >= 0) {
            fail("a machine with no messages has a line for the controller's vector");
        }
        if (fallback != 0) {
            fail("a machine with no messages counted a lost edge");
        }
    } else if (drop > 0) {
        if (vector != drop) {
            fail("the controller sent a different number of messages than it was allowed");
        }
        if (fallback <= 0) {
            fail("the timer's pump found nothing after the messages stopped");
        }
    } else if (devices == 0) {
        if (vector >= 0) {
            fail("a machine with no USB device took a message anyway");
        }
        if (fallback != 0) {
            fail("a machine with no USB device counted a lost edge");
        }
    } else {
        /*
         * Four is the least this boot can honestly have taken: the keys typed
         * before it ran are five, and each of them is at least one message.
         * One message and no more is what a kernel that never ends the
         * interrupt at the processor takes, and the machine goes on typing on
         * the timer's pump with nothing else to show for it.
         */
        if (vector < 4) {
            fail("the controller's vector took fewer interrupts than the keys typed");
        }
        if (fallback != 0) {
            fail("the timer's pump found work the messages should have brought");
        }
    }

    puts("INTRPROBE OK vector ");
    putnum(vector);
    puts(", fallback ");
    putnum(fallback);
    puts("\n");
    sys_exit(0);
}
