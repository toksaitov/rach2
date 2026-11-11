/*
 * pstoreprobe.c: the crash record ramoops keeps across a reset, read under
 * /sys/fs/pstore, and the kernel's feed of the TCO watchdog it started.
 *
 * `pstoreprobe fed SECONDS` sleeps SECONDS with /dev/watchdog unopened and
 * says PSTOREPROBE FED: a TCO that the kernel did not feed would reset the
 * guest first. `pstoreprobe read` checks the panic's record, Linux's
 * dmesg-ramoops-0, and the console's, console-ramoops-0, removes the first
 * and checks that it is gone, and says PSTOREPROBE OK. Neither prints the
 * old log, whose records would read as this boot's.
 */
#include "probe.h"
#include "probe_args.h"

#define DMESG   "/sys/fs/pstore/dmesg-ramoops-0"
#define CONSOLE "/sys/fs/pstore/console-ramoops-0"

/* A record of ramoops.record_size=0x10000 and its ECC notice fit. */
static char text[0x11000];

/** The length of `s`. */
static unsigned length(const char *s)
{
    unsigned n = 0;

    while (s[n]) {
        n++;
    }
    return n;
}

/** Write `s` to the console. */
static void say(const char *s)
{
    syscall3(SYS_WRITE, 1, (long)s, length(s));
}

/** Report `why` and end with status 1. */
static void fail(const char *why)
{
    say("PSTOREPROBE FAIL: ");
    say(why);
    say("\n");
    syscall1(SYS_EXIT, 1);
    for (;;) {
    }
}

/** Whether the `n` bytes at `at` are `s`. */
static int match(const char *at, unsigned n, const char *s)
{
    unsigned i, len = length(s);

    if (len > n) {
        return 0;
    }
    for (i = 0; i < len; i++) {
        if (at[i] != s[i]) {
            return 0;
        }
    }
    return 1;
}

/** Whether the `n` bytes at `buf` hold `s`. */
static int holds(const char *buf, unsigned n, const char *s)
{
    unsigned i;

    for (i = 0; i < n; i++) {
        if (match(buf + i, n - i, s)) {
            return 1;
        }
    }
    return 0;
}

/** Whether `s` is `t`. */
static int same(const char *s, const char *t)
{
    return length(s) == length(t) && match(s, length(s), t);
}

/** Read the file `path` whole into `text`; its length. */
static unsigned slurp(const char *path)
{
    long fd = syscall3(SYS_OPEN, (long)path, 0, 0), n;
    unsigned got = 0;

    if (fd < 0) {
        fail(path);
    }
    while ((n = syscall3(SYS_READ, fd, (long)(text + got), (long)(sizeof(text) - 1 - got))) > 0) {
        got += (unsigned)n;
    }
    syscall1(SYS_CLOSE, fd);
    if (n < 0) {
        fail("a read of a record failed");
    }
    text[got] = '\0';
    return got;
}

/** Sleep `seconds` with the watchdog left to the kernel. */
static void fed(const char *seconds)
{
    struct {
        long sec, nsec;
    } ts = {0, 0};
    unsigned char st[KSTAT_SIZE];
    const char *p;

    for (p = seconds; *p >= '0' && *p <= '9'; p++) {
        ts.sec = ts.sec * 10 + (*p - '0');
    }
    if (syscall2(SYS_STAT, (long)"/sys/fs/pstore", (long)st) != 0) {
        fail("no /sys/fs/pstore");
    }
    /* interval: the kernel alone feeds the TCO for three heartbeats */
    if (syscall2(SYS_NANOSLEEP, (long)&ts, 0) != 0) {
        fail("the sleep was cut short");
    }
    say("PSTOREPROBE FED\n");
}

/** Print the lines of the `n` bytes of `text` that hold `panic: `, each
 * after `| `; the rest of the old log is not printed, since its records,
 * the debug shell's among them, would read as this boot's. */
static void show(unsigned n)
{
    unsigned at = 0, end;

    while (at < n) {
        for (end = at; end < n && text[end] != '\n'; end++) {
        }
        if (holds(text + at, end - at, "panic: ")) {
            say("| ");
            syscall3(SYS_WRITE, 1, (long)(text + at), end - at);
            say("\n");
        }
        at = end + 1;
    }
}

/** Check the two records, print the panic's, and remove it. */
static void read_records(void)
{
    unsigned n = slurp(DMESG);

    say("pstoreprobe: " DMESG ":\n");
    show(n);
    /* ram.c:178: the file is the record after ramoops's header line. */
    if (!match(text, n, "Panic#1 Part1\n") || holds(text, n, "====")) {
        fail("dmesg-ramoops-0 does not begin as Linux's dump does");
    }
    if (!holds(text, n, "panic: stopped: ")) {
        fail("dmesg-ramoops-0 lacks the panic's record");
    }
    if (n < 25 || !match(text + n - 25, 25, "\nECC: No errors detected\n")) {
        fail("dmesg-ramoops-0 lacks the ECC notice");
    }
    n = slurp(CONSOLE);
    if (!holds(text, n, "panic: stopped: ") || !holds(text, n, "ramoops: ready")) {
        fail("console-ramoops-0 lacks the last boot's log");
    }
    if (syscall1(SYS_UNLINK, (long)DMESG) != 0) {
        fail("the unlink of dmesg-ramoops-0 failed");
    }
    if (syscall3(SYS_OPEN, (long)DMESG, 0, 0) != -2) {
        fail("dmesg-ramoops-0 outlived its unlink");
    }
    say("PSTOREPROBE OK\n");
}

void probe_main(unsigned long *sp);

PROBE_SP_ENTRY;

/** Run the mode the first word names. */
void probe_main(unsigned long *sp)
{
    probe_args((int)sp[0], (char **)(sp + 1));
    if (probe_words() >= 2 && same(probe_word(1), "fed")) {
        fed(probe_word(2));
    } else if (probe_words() >= 1 && same(probe_word(1), "read")) {
        read_records();
    } else {
        fail("usage: pstoreprobe fed SECONDS | read");
    }
    syscall1(SYS_EXIT, 0);
}
