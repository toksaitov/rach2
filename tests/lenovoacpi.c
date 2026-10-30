/* lenovoacpi: accept the Lenovo's ACPI devices and brightness actions on
 * the running system, with the installed acpid acting on firmware events. */

#include "probe.h"
#include "probe_report.h"
#include "../rach2/sys/klog.h"

#define BACKLIGHT     "/sys/class/backlight/intel_backlight/"
#define EVIOCGNAME(n) (0x80004506L | (long)(n) << 16)

struct input_event {
    unsigned long sec, usec;
    unsigned short type, code;
    int value;
};

struct k_sigaction {
    void (*handler)(int);
    unsigned long flags;
    void (*restorer)(void);
    unsigned mask[2];
};

static volatile int stopped;
static int saved = -1;
static int changed;
static char ring[KLOG_RING_SIZE + 1];

void signal_return(void);
PROBE_RT_RESTORER(signal_return);

/** Record a stop request; the main path restores the level before exit. */
static void stop(int signal)
{
    stopped = signal;
}

/** Return the length of the NUL-terminated string s. */
static int length(const char *s)
{
    int n = 0;

    while (s[n]) {
        n++;
    }
    return n;
}

/** Return whether a and b are equal NUL-terminated strings. */
static int same(const char *a, const char *b)
{
    while (*a && *a == *b) {
        a++;
        b++;
    }
    return *a == *b;
}

/** Return the first occurrence of text in s, or NULL. */
static const char *find(const char *s, const char *text)
{
    int i, n = length(text);

    for (; *s; s++) {
        for (i = 0; i < n && s[i] == text[i]; i++) {
        }
        if (i == n) {
            return s;
        }
    }
    return 0;
}

/** Read path into text, with room for its NUL; return bytes or an error. */
static long read_text(const char *path, char *text, int cap)
{
    long fd = syscall3(SYS_OPEN, (long)path, 0, 0), n;

    if (fd < 0) {
        return fd;
    }
    n = syscall3(SYS_READ, fd, (long)text, cap - 1);
    syscall1(SYS_CLOSE, fd);
    if (n >= 0) {
        text[n] = 0;
    }
    return n;
}

/** Write all of text to path; return whether the write and close succeeded. */
static int write_text(const char *path, const char *text)
{
    long fd = syscall3(SYS_OPEN, (long)path, 1, 0), n, closed;

    if (fd < 0) {
        return 0;
    }
    n = syscall3(SYS_WRITE, fd, (long)text, length(text));
    closed = syscall1(SYS_CLOSE, fd);
    return n == length(text) && closed == 0;
}

/** Read a decimal backlight attribute; return -1 for a malformed value. */
static int level(const char *path)
{
    char text[32];
    int i = 0, n = 0;

    if (read_text(path, text, sizeof(text)) <= 0) {
        return -1;
    }
    while (text[i] >= '0' && text[i] <= '9') {
        if (n > 65535) {
            return -1;
        }
        n = n * 10 + text[i++] - '0';
    }
    return i && (text[i] == 0 || (text[i] == '\n' && !text[i + 1])) ? n : -1;
}

/** Set the requested backlight level to n; return whether it was accepted. */
static int set_level(int n)
{
    char text[12];

    return write_text(BACKLIGHT "brightness", report_decimal(text, n));
}

/** Write a diagnostic to standard error. */
static void diagnostic(const char *text)
{
    syscall3(SYS_WRITE, 2, (long)"lenovoacpi: ", 12);
    syscall3(SYS_WRITE, 2, (long)text, length(text));
    syscall3(SYS_WRITE, 2, (long)"\n", 1);
}

/** Restore the original requested level, reporting a failed restoration. */
static int restore(void)
{
    if (changed && (!set_level(saved) || level(BACKLIGHT "brightness") != saved)) {
        diagnostic("cannot restore the original brightness");
        return 0;
    }
    changed = 0;
    return 1;
}

/** Restore the level, report why the current assertion failed, and exit 1. */
static void fail(const char *why)
{
    restore();
    report_stop(why);
    diagnostic(why);
    syscall1(SYS_EXIT, 1);
    __builtin_unreachable();
}

/** Install stop handlers before changing the level; fail if one is refused. */
static void signals(void)
{
    struct k_sigaction action;
    static const int numbers[] = {1, 2, 15};
    int i;

    action.handler = stop;
    action.flags = 0x04000000; /* Linux's SA_RESTORER. */
    action.restorer = signal_return;
    action.mask[0] = action.mask[1] = 0;
    for (i = 0; i < 3; i++) {
        if (syscall4(SYS_RT_SIGACTION, numbers[i], (long)&action, 0, 8) < 0) {
            fail("cannot install the brightness cleanup handlers");
        }
    }
}

/** Check the retained boot records of ACPICA and the EC's enabled GPE. */
static void boot(void)
{
    const char *at;
    long n = syscall3(SYS_SYSLOG, 3, (long)ring, KLOG_RING_SIZE);

    if (n <= 0 || n > KLOG_RING_SIZE - KLOG_RECORD_MAX) {
        fail("the kernel log is empty, unreadable or may have lost boot records");
    }
    ring[n] = 0;
    at = find(ring, "acpi: acpica: ");
    while (at) {
        at += length("acpi: acpica: ");
        if (*at >= '1' && *at <= '9') {
            break;
        }
        at = find(at, "acpi: acpica: ");
    }
    if (!at) {
        fail("ACPICA has no initialization record");
    }
    while (*at >= '0' && *at <= '9') {
        at++;
    }
    if (find(at, " objects, 0 errors\n") != at || find(ring, "ACPI Error") ||
        find(ring, "ACPI BIOS Error") || find(ring, "ACPI Exception")) {
        fail("ACPICA did not initialize without errors");
    }
    report_held();
    if (!find(ring, "acpi: ec: \\_SB.PCI0.LPCB.EC0 at ports 0x62 and 0x66, gpe 0x7\n") ||
        !find(ring, "acpi: ec: gpe 0x7 enabled, status 0x") || find(ring, "acpi: ec: refused:") ||
        find(ring, "acpi: ec: timeout:")) {
        fail("the Lenovo's EC or its enabled GPE is absent");
    }
    report_held();
}

/** Open the device named name without grabbing it from acpid; fail if absent. */
static int device(const char *name)
{
    char path[32], text[64], number[12];
    int i, k, n;
    long fd, got;

    for (i = 0; i < 32; i++) {
        const char *prefix = "/dev/input/event";

        n = 0;
        while (*prefix) {
            path[n++] = *prefix++;
        }
        report_decimal(number, i);
        for (k = 0; number[k]; k++) {
            path[n++] = number[k];
        }
        path[n] = 0;
        fd = syscall3(SYS_OPEN, (long)path, 0, 0);
        if (fd < 0) {
            continue;
        }
        got = syscall3(SYS_IOCTL, fd, EVIOCGNAME(sizeof(text) - 1), (long)text);
        if (got > 0 && got < (long)sizeof(text)) {
            text[got] = 0;
            if (same(text, name)) {
                probe_line("lenovoacpi: device: ", name, 0);
                return (int)fd;
            }
        }
        syscall1(SYS_CLOSE, fd);
    }
    fail(name);
    return -1;
}

/** Yield between observations; the collection owns the progress bound. */
static void pause_observing(void)
{
    struct {
        long sec, nsec;
    } delay;

    if (stopped) {
        fail("interrupted");
    }
    delay.sec = 0;
    delay.nsec = 10000000;
    syscall2(SYS_NANOSLEEP, (long)&delay, 0);
}

/** Fire method and require key pressed and released, then acpid's level want. */
static void brightness(int fd, const char *method, int key, int before, int want)
{
    struct input_event event;
    char result[64];
    int i, now;
    long n;

    if (stopped || !write_text("/proc/acpi/call", method) ||
        read_text("/proc/acpi/call", result, sizeof(result)) <= 0 || !same(result, "0x0")) {
        fail("the firmware brightness method failed");
    }
    for (i = 0; i < 4; i++) {
        do {
            n = syscall3(SYS_READ, fd, (long)&event, sizeof(event));
            if (n == -11) {
                pause_observing();
            }
        } while (n == -11);
        if (n != sizeof(event) ||
            (i % 2 ? event.type != 0 || event.code != 0 || event.value != 0
                   : event.type != 1 || event.code != key || event.value != (i == 0))) {
            fail("the video bus did not give the expected press and release");
        }
    }
    report_held();
    while ((now = level(BACKLIGHT "brightness")) == before) {
        pause_observing();
    }
    if (stopped || now != want || level(BACKLIGHT "actual_brightness") != want) {
        fail("acpid did not set the expected backlight level");
    }
    report_held();
}

void probe_main(unsigned long *sp);
PROBE_SP_ENTRY;

/** Run the Lenovo's native acceptance with no private root or replacement daemon. */
void probe_main(unsigned long *sp)
{
    char **argv = (char **)(sp + 1);
    int fd, baseline, step;

    report_begin(report_case(argv + sp[0] + 1, "native.lenovoacpi"));
    report_plan("acpica");
    report_plan("ec");
    report_plan("devices");
    report_plan("backlight");
    report_plan("down-key");
    report_plan("down-action");
    report_plan("up-key");
    report_plan("up-action");
    report_plan("restore");
    signals();
    boot();
    fd = device("AT Translated Set 2 keyboard");
    syscall1(SYS_CLOSE, fd);
    fd = device("Power Button");
    syscall1(SYS_CLOSE, fd);
    fd = device("Video Bus");
    if (syscall3(SYS_FCNTL, fd, 4, 04000) < 0) {
        fail("cannot make the video reader nonblocking");
    }
    report_held();
    saved = level(BACKLIGHT "brightness");
    if (saved < 0 || saved > 937 || level(BACKLIGHT "max_brightness") != 937 ||
        level(BACKLIGHT "bl_power") != 0) {
        fail("the active Intel backlight does not have the Lenovo's range");
    }
    baseline = 937 / 2;
    step = 937 / 10;
    changed = 1;
    if (stopped || !set_level(baseline) || level(BACKLIGHT "brightness") != baseline ||
        level(BACKLIGHT "actual_brightness") != baseline) {
        fail("the panel must be unblanked and accept a middle brightness");
    }
    report_held();
    brightness(fd, "\\_SB.PCI0.LPCB.EC0._Q11", 224, baseline, baseline - step);
    brightness(fd, "\\_SB.PCI0.LPCB.EC0._Q12", 225, baseline - step, baseline);
    syscall1(SYS_CLOSE, fd);
    if (!restore()) {
        fail("the original brightness was not restored");
    }
    report_held();
    report_end();
    probe_line("LENOVOACPI OK", 0, 0);
    syscall1(SYS_EXIT, 0);
    __builtin_unreachable();
}
