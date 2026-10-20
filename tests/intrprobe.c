/**
 * Reads `/proc/interrupts` and requires what this boot's options say it must
 * hold.
 *
 * Four machines are checked by the one program, because what the file must
 * say is decided by the boot line and the device list, both of which the
 * guest can read for itself. On a machine with a USB keyboard the suite has
 * typed five keys before this runs, and on one with a mouse and no keyboard
 * it has moved the mouse with three button states; the probe first waits
 * until `/proc/usb` counts the reports that input makes, five from the
 * keyboards or three from the mice, so the interrupt counts are read after
 * the work they are judged against has arrived.
 *
 * * With a USB device and messages, which is the ordinary boot: the
 *   controller's vector has taken at least two interrupts for those reports,
 *   more than the one a kernel that never ends the interrupt takes; and the
 *   timer's pump has found none of the work they carry, so the fallback
 *   count is zero. A controller may take several reports on one interrupt,
 *   so the reports are counted where they arrive and the interrupts are not
 *   required to match them.
 * * With `rach2.nomsi`: the message vector has no line at all, since a
 *   vector that has taken nothing is not listed; the controller's PCI line,
 *   listed as `IO-APIC`, or `PIC` on a machine without one, with the
 *   controller's name, has taken at least two for the reports; and the
 *   fallback count is zero because the line brought the work.
 * * With `rach2.msidrop=N`: the controller was silenced after N messages, so
 *   the vector's count is exactly N and the timer's pump has been finding
 *   the work since, which is what the fallback count is for.
 * * With no USB device on the machine: nothing sends a message, so the
 *   vector has no line and the fallback count is zero. The timer's own
 *   vector is required to have counted in every one of them.
 *
 * Given `machine=native` (probe_args.h), on a machine that is already up
 * and on which nothing can type, the probe requires only what holds on any
 * boot: the timer's count, the whole file within the buffer, and a fallback
 * line whose count is zero. It neither waits for reports nor judges the
 * controller's counts or the boot line's profiles.
 *
 * Prints "INTRPROBE OK" with the counts it read and exits 0. Freestanding;
 * direct system calls (probe.h).
 */

#include "probe.h"
#include "probe_args.h"

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

/** Whether the `n` bytes at `line` end with `tail`. */
static int ends_with(const char *line, int n, const char *tail)
{
    int m = (int)strlen(tail);
    int i;

    if (m > n) {
        return 0;
    }
    for (i = 0; i < m; i++) {
        if (line[n - m + i] != tail[i]) {
            return 0;
        }
    }
    return 1;
}

/** The decimal number at the start of `s`, 0 when it has no digit. */
static int number_at(const char *s)
{
    int value = 0;

    while (*s >= '0' && *s <= '9') {
        value = value * 10 + (*s - '0');
        s++;
    }
    return value;
}

/**
 * How many of the USB devices this machine has configured have `kind` in
 * their line, which is all of them when `kind` is empty, with the sum of
 * their delivered reports in `*reports`.
 *
 * `/proc/usb` has one line per device, which begins with the port it is on,
 * and is empty on a machine with none; that difference is what tells a boot
 * that must have taken messages from one that had nothing to send them. The
 * file also has a line per stick that is a disk, a line counting the names
 * and, while the stack is enumerating, a `busy` line, so the count is of the
 * lines that begin with `port` and not of the lines. A keyboard's or mouse's
 * line ends with its class and protocol and then `reports N`, so a `kind` of
 * `" class 3 protocol 1 reports "` finds the boot-protocol keyboards and the
 * count after it.
 */
static int usb_devices(const char *kind, int *reports)
{
    static char list[1024];
    int n = slurp("/proc/usb", list, (int)sizeof(list));
    int counted = ends_with(kind, (int)strlen(kind), " reports ");
    int devices = 0;
    int at = 0;

    *reports = 0;
    if (n >= (int)sizeof(list) - 1) {
        fail("/proc/usb filled this program's buffer, so it was cut");
    }
    while (at < n) {
        int end = at;
        int k = -1;

        while (end < n && list[end] != '\n') {
            end++;
        }
        list[end] = '\0';
        if (find(list + at, "port ") == 0) {
            k = find(list + at, kind);
        }
        if (k >= 0) {
            devices++;
            if (counted) {
                *reports += number_at(list + at + k + (int)strlen(kind));
            }
        }
        at = end + 1;
    }
    return devices;
}

/**
 * Wait until the keyboards, or on a machine with none the mice, have
 * delivered the reports the suite's input makes. The emulator's keyboard
 * reports each key that goes down or up on a report of its own, and the
 * shell has read the five keys typed, so the keyboards have delivered five
 * at least. Its mouse merges movements the guest has not yet read into one
 * report but starts a new one when the buttons change, and the mouse is
 * moved with three button states, so the mice deliver three at least.
 *
 * The reports arrive through the controller's interrupts or the timer's
 * pump, while this yields between readings; none of the readings is
 * counted, and the suite's wait for this program's word bounds a machine on
 * which they never arrive.
 */
static void await_reports(int keyboards, int mice)
{
    const char *kind = " class 3 protocol 2 reports ";
    int want = 3;
    int reports;

    if (keyboards == 0 && mice == 0) {
        return;
    }
    if (keyboards > 0) {
        kind = " class 3 protocol 1 reports ";
        want = 5;
    }
    usb_devices(kind, &reports);
    while (reports < want) {
        syscall0(SYS_SCHED_YIELD);
        usb_devices(kind, &reports);
    }
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
 * Judge the counts of the controller's vector, the fallback line and the
 * controller's PCI line against the profile the boot line names: no
 * messages, a number of messages dropped, no USB device, or the ordinary
 * boot with `devices` USB devices. With `native` set, the fallback count
 * alone.
 */
static void judge(int native, int vector, int fallback, int line, int devices)
{
    int drop = option_value("rach2.msidrop=");

    if (native) {
        if (fallback != 0) {
            fail("the timer's pump found work the messages or the line should have brought");
        }
    } else if (find(cmdline, "rach2.nomsi") >= 0) {
        if (vector >= 0) {
            fail("a machine with no messages has a line for the controller's vector");
        }
        if (devices > 0 && line < 2) {
            fail("the controller's PCI line took one interrupt or none for the reports");
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
        if (vector < 2) {
            fail("the controller's vector took one interrupt or none for the reports");
        }
        if (fallback != 0) {
            fail("the timer's pump found work the messages should have brought");
        }
    }
}

void probe_main(unsigned long *sp);

PROBE_SP_ENTRY;

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
 * have taken at least two interrupts for the reports the suite's input made;
 * one message and no more is what a kernel that never ends the interrupt at
 * the processor takes, while the machine goes on typing on the timer's pump.
 * A device has one report transfer outstanding, which the stack queues again
 * only after it has taken the report before, and the event that completes it
 * raises an interrupt of its own; so three reports or more cannot all arrive
 * on one message, however the controller batches the events it has. With
 * `machine=native` it makes only the checks the opening comment names for a
 * native run.
 */
void probe_main(unsigned long *sp)
{
    int vector, audio, fallback, devices, n, line, native;
    int key_reports, mouse_reports, all_reports;

    probe_args((int)sp[0], (char **)(sp + 1));
    native = probe_native();
    if (slurp("/proc/cmdline", cmdline, (int)sizeof(cmdline)) < 0) {
        fail("/proc/cmdline would not open");
    }
    if (!native) {
        await_reports(usb_devices(" class 3 protocol 1 reports ", &key_reports),
                      usb_devices(" class 3 protocol 2 reports ", &mouse_reports));
    }
    devices = usb_devices("", &all_reports);
    usb_devices(" class 3 protocol 1 reports ", &key_reports);
    usb_devices(" class 3 protocol 2 reports ", &mouse_reports);
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

    puts("INTRPROBE READ vector ");
    putnum(vector);
    puts(", audio ");
    putnum(audio);
    puts(", fallback ");
    putnum(fallback);
    puts(", line ");
    putnum(line);
    puts(", keyboard reports ");
    putnum(key_reports);
    puts(", mouse reports ");
    putnum(mouse_reports);
    puts("\n");

    judge(native, vector, fallback, line, devices);

    puts("INTRPROBE OK vector ");
    putnum(vector);
    puts(", audio ");
    putnum(audio);
    puts(", fallback ");
    putnum(fallback);
    puts("\n");
    sys_exit(0);
}
