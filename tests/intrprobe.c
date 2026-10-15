/**
 * Reads `/proc/interrupts` and requires what this boot's options say it must
 * hold.
 *
 * Four machines are checked by the one program, because what the file must
 * say is decided by the boot line and the device list, both of which the
 * guest can read for itself:
 *
 * * With a USB device and messages, which is the ordinary boot: the
 *   controller's vector has taken at least four interrupts for the five keys
 *   typed before this ran, and the timer's pump has found none of the work
 *   they carry, so the fallback count is zero.
 * * With `rach2.nomsi`: the message vector has no line at all, since a
 *   vector that has taken nothing is not listed; the controller's PCI line,
 *   listed as `IO-APIC`, or `PIC` on a machine without one, with the
 *   controller's name, has taken at least four for the five keys; and
 *   the fallback count is zero because the line brought the work.
 * * With `rach2.msidrop=N`: the controller was silenced after N messages, so
 *   the vector's count is exactly N and the timer's pump has been finding
 *   the work since, which is what the fallback count is for.
 * * With no USB device on the machine: nothing sends a message, so the
 *   vector has no line and the fallback count is zero. The timer's own
 *   vector is required to have counted in every one of them.
 *
 * Prints "INTRPROBE OK" with the counts it read and exits 0. Freestanding;
 * direct system calls (probe.h).
 */

#include "probe.h"

#define STDOUT_FILENO 1

#define O_RDONLY 0

/* Larger than the file on the machines the suites boot. The probe fails on a
   file that fills it: that would be a file cut in the middle of a line, and a
   count cut in the middle of its digits reads as a smaller number that a
   check might accept. */
static char text[4096];
static char cmdline[512];

/** exit(2) with `status`; does not return. */
static void sys_exit(int status)
{
    syscall1(SYS_EXIT, (long)status);
    __builtin_unreachable();
}

/** The length of the string `s`. */
static size_t strlen(const char *s)
{
    size_t n = 0;

    while (s[n]) {
        n++;
    }
    return n;
}

/** Write `s` to standard output. */
static void puts(const char *s)
{
    syscall3(SYS_WRITE, STDOUT_FILENO, (long)s, (int)strlen(s));
}

/** Write `v` in decimal. */
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

/** Print "INTRPROBE FAIL: " and `why`, and exit 1. */
static void fail(const char *why)
{
    puts("INTRPROBE FAIL: ");
    puts(why);
    puts("\n");
    sys_exit(1);
}

/** Read the file `path` into `buf`, at most `cap` - 1 bytes and a NUL: the
 * bytes read, or -1 when it does not open. */
static int slurp(const char *path, char *buf, int cap)
{
    int fd = syscall3(SYS_OPEN, (long)path, O_RDONLY, 0);
    int done = 0;

    if (fd < 0) {
        return -1;
    }
    for (;;) {
        int n = syscall3(SYS_READ, fd, (long)(buf + done), cap - 1 - done);

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
 * `/proc/usb` has one line per device, which begins with the port it is on,
 * and is empty on a machine with none; that difference is what tells a boot
 * that must have taken messages from one that had nothing to send them. The
 * file also has a line per stick that is a disk and a line counting the
 * names, so the count is of the lines that begin with `port` and not of the
 * lines.
 */
static int usb_devices(void)
{
    static char list[1024];
    int n = slurp("/proc/usb", list, (int)sizeof(list));
    int devices = 0;
    int at = 0;

    if (n <= 0) {
        return 0;
    }
    while (at < n) {
        int end = at;

        while (end < n && list[end] != '\n') {
            end++;
        }
        if (end - at > 5 && list[at] == 'p' && list[at + 1] == 'o' && list[at + 2] == 'r' &&
            list[at + 3] == 't' && list[at + 4] == ' ') {
            devices++;
        }
        at = end + 1;
    }
    return devices;
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

/**
 * The count on the line the file lists as KIND with NAME, whatever its
 * vector, or -1 when no line has both. A driver's message vector is found by
 * the name its handler registered under, since a vector is given to a device
 * when it enables its messages and the number is whatever came next; a PCI
 * line's vector differs from machine to machine, as in `39:\t57\tPIC\txhci`.
 * The names on a line are one per device, `rtl8139, xhci` where the emulator
 * puts both on IRQ 11, and the name asked for must be one of them whole.
 */
static int count_for_named(const char *kind, const char *name, const char *file)
{
    int i = 0;

    while (file[i] != '\0') {
        int start = i;
        int k = start;

        while (file[k] >= '0' && file[k] <= '9') {
            k++;
        }
        if (k > start && file[k] == ':') {
            int j;
            const char *fields[3];
            int f = 0;

            k++;
            for (j = k; file[j] != '\0' && file[j] != '\n'; j++) {
                if (file[j] == '\t' && f < 3) {
                    fields[f++] = file + j + 1;
                }
            }
            if (f == 3 && find(fields[1], kind) == 0 && fields[1][strlen(kind)] == '\t') {
                const char *names = fields[2];
                int m = (int)strlen(name);
                int found = 0;

                while (*names != '\0' && *names != '\n') {
                    if (find(names, name) == 0 &&
                        (names[m] == ',' || names[m] == '\n' || names[m] == '\0')) {
                        found = 1;
                        break;
                    }
                    while (*names != '\0' && *names != '\n' && *names != ' ') {
                        names++;
                    }
                    while (*names == ' ') {
                        names++;
                    }
                }
                if (found) {
                    int value = 0;

                    while (fields[0][0] == ' ') {
                        fields[0]++;
                    }
                    while (fields[0][0] >= '0' && fields[0][0] <= '9') {
                        value = value * 10 + (fields[0][0] - '0');
                        fields[0]++;
                    }
                    return value;
                }
            }
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
 * Read the boot line and /proc/interrupts, print the counts, and require
 * those of the machine the boot line and /proc/usb describe. The timer's
 * vector must have counted on every machine. The counts are printed before
 * any is judged, so that a failing run logs them. The kernel acts on
 * `rach2.msidrop` only with a count above zero, so a count of zero, or one
 * that is not a number, is the ordinary boot.
 *
 * The audio controller's vector is printed and not judged: whether it has
 * taken anything depends on whether this boot played a sound, which is the
 * audio suite's business. On the ordinary boot the controller's vector must
 * have taken at least four interrupts for the five keys the suite typed
 * before the probe ran; one message and no more is what a kernel that never
 * ends the interrupt at the processor takes, while the machine goes on
 * typing on the timer's pump.
 */
PROBE_START void _start(void)
{
    int vector, audio, fallback, drop, devices, n, line;

    if (slurp("/proc/cmdline", cmdline, (int)sizeof(cmdline)) < 0) {
        fail("/proc/cmdline would not open");
    }
    n = slurp("/proc/interrupts", text, (int)sizeof(text));
    if (n < 0) {
        fail("/proc/interrupts would not open");
    }
    if (n >= (int)sizeof(text) - 1) {
        fail("/proc/interrupts filled this program's buffer, so it was cut");
    }
    if (count_for("32:", text) <= 0) {
        fail("the timer's vector has taken no interrupts");
    }
    vector = count_for_named("MSI", "xhci", text);
    audio = count_for_named("MSI", "hda", text);
    line = count_for_named("IO-APIC", "xhci", text);
    if (line < 0) {
        line = count_for_named("PIC", "xhci", text);
    }
    fallback = count_for("FBK:", text);
    if (fallback < 0) {
        fail("there is no fallback line");
    }
    drop = option_value("rach2.msidrop=");
    devices = usb_devices();

    puts("INTRPROBE READ vector ");
    putnum(vector);
    puts(", audio ");
    putnum(audio);
    puts(", fallback ");
    putnum(fallback);
    puts(", line ");
    putnum(line);
    puts("\n");

    if (find(cmdline, "rach2.nomsi") >= 0) {
        if (vector >= 0) {
            fail("a machine with no messages has a line for the controller's vector");
        }
        if (devices > 0 && line < 4) {
            fail("the controller's PCI line took fewer interrupts than the keys typed");
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
        if (vector < 4) {
            fail("the controller's vector took fewer interrupts than the keys typed");
        }
        if (fallback != 0) {
            fail("the timer's pump found work the messages should have brought");
        }
    }

    puts("INTRPROBE OK vector ");
    putnum(vector);
    puts(", audio ");
    putnum(audio);
    puts(", fallback ");
    putnum(fallback);
    puts("\n");
    sys_exit(0);
}
