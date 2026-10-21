/**
 * bootlogprobe: the kernel's records of this boot, read from its ring, have
 * the texts the probe is told they have, and none of those it is told they
 * lack.
 *
 * The console of a boot shows what the kernel printed, and a test that
 * watches it can find a record there; it cannot run on the machine. This
 * probe reads the same records from inside, with syslog(2) action 3, which
 * gives the whole ring in one call, so that a case can also require a
 * record's absence, such as `audio: started` on a boot whose line has
 * `rach2.noaudio` (tests/cases/guest.subset.json). An absence means something
 * only while the ring still holds the boot's first record, so the probe first
 * requires, as the assertion `whole`, that the ring has never dropped one: the
 * ring drops whole records only to make room for a new one, after which it
 * stays within KLOG_RECORD_MAX of its size (rach2/sys/klog.c), so a ring that
 * holds less than that has dropped nothing. Without it each check of the
 * probe is reported as not run.
 *
 * Usage: bootlogprobe ID TEXT [ID TEXT]...
 *
 * Each TEXT is `+` or `-` and the text the ring must have or must lack, and
 * ID is the assertion it reports. Writes etude's report records
 * (probe_report.h) for the case ETUDE_CASE names, guest.subset.bootlogprobe
 * without it, and beside them "BOOTLOGPROBE OK" or a "BOOTLOGPROBE FAIL (id)"
 * line for each assertion that did not hold; exits 0 when every assertion
 * held, 1 when one did not, and 2 for arguments it cannot read. Freestanding;
 * direct system calls (probe.h).
 */

#include "probe.h"
#include "probe_report.h"

#include "../rach2/sys/klog.h"

/* syslog(2)'s actions, Linux's numbers: the whole ring, and its size. */
#define SYSLOG_ACTION_READ_ALL    3
#define SYSLOG_ACTION_SIZE_BUFFER 10

/* The ring as one read gives it, and the bytes the read gave. */
static char ring[KLOG_RING_SIZE];
static long ring_len;

/** exit(2); does not return. */
static void sys_exit(int status)
{
    syscall3(SYS_EXIT, status, 0, 0);
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

/** Write the string `s` to standard output. */
static void puts(const char *s)
{
    syscall3(SYS_WRITE, 1, (long)s, (int)strlen(s));
}

/** Whether the ring as read has the text `s`. */
static int ring_has(const char *s)
{
    long n = (long)strlen(s), i, k;

    for (i = 0; n > 0 && i + n <= ring_len; i++) {
        for (k = 0; k < n && ring[i + k] == s[k]; k++) {
        }
        if (k == n) {
            return 1;
        }
    }
    return 0;
}

/**
 * Read the ring, and report `whole`: the read succeeded, the ring is the
 * size this probe was built for, and it holds something and less than
 * KLOG_RECORD_MAX short of that size, so it has dropped no record of the
 * boot. Returns whether it held.
 */
static int read_ring(void)
{
    char a[12];
    long size = syscall3(SYS_SYSLOG, SYSLOG_ACTION_SIZE_BUFFER, 0, 0);
    const char *why = 0;

    ring_len = syscall3(SYS_SYSLOG, SYSLOG_ACTION_READ_ALL, (long)ring, (long)sizeof(ring));
    if (size != KLOG_RING_SIZE || ring_len < 0) {
        why = "syslog(2) did not give a ring of KLOG_RING_SIZE bytes";
    } else if (ring_len == 0) {
        why = "the ring is empty";
    } else if (ring_len > KLOG_RING_SIZE - KLOG_RECORD_MAX) {
        why = "the ring is full, so it may have dropped the boot's first records";
    }
    puts("BOOTLOGPROBE ring bytes: ");
    puts(report_decimal(a, ring_len < 0 ? 0 : (int)ring_len));
    puts("\n");
    if (why != 0) {
        report_fail("whole", why);
        puts("BOOTLOGPROBE FAIL (whole)\n");
        return 0;
    }
    report_pass("whole");
    return 1;
}

void probe_main(unsigned long *sp);

PROBE_SP_ENTRY;

/** The probe, entered with `sp` at argc: the ring read, then each check its
 * arguments give, each reported. */
void probe_main(unsigned long *sp)
{
    int argc = (int)sp[0];
    char **argv = (char **)(sp + 1);
    char **envp = argv + argc + 1;
    int i, ok, held, whole;

    if (argc < 3 || argc % 2 == 0) {
        puts("bootlogprobe: usage: bootlogprobe ID TEXT [ID TEXT]...\n");
        sys_exit(2);
    }
    for (i = 2; i < argc; i += 2) {
        if ((argv[i][0] != '+' && argv[i][0] != '-') || argv[i][1] == '\0') {
            puts("bootlogprobe: a TEXT begins with + or - and has a text after it\n");
            sys_exit(2);
        }
    }
    report_begin(report_case(envp, "guest.subset.bootlogprobe"));
    ok = whole = read_ring();
    for (i = 1; i < argc; i += 2) {
        if (!whole) {
            report_notrun(argv[i], "the ring may have dropped the boot's first records");
            continue;
        }
        held = ring_has(argv[i + 1] + 1) == (argv[i + 1][0] == '+');
        if (held) {
            report_pass(argv[i]);
            continue;
        }
        report_fail(argv[i],
                    argv[i + 1][0] == '+' ? "the ring lacks the text" : "the ring has the text");
        puts("BOOTLOGPROBE FAIL (");
        puts(argv[i]);
        puts(")\n");
        ok = 0;
    }
    report_end();
    if (!ok) {
        sys_exit(1);
    }
    puts("BOOTLOGPROBE OK\n");
    sys_exit(0);
}
