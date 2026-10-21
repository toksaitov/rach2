/**
 * persistprobe: fsync makes a write durable, across two boots of one disk.
 *
 * On a KRFFS mounted from a disk (rach2.fsdisk), the boot that runs
 * `persistprobe write` finds no /persist.txt, writes a marker to it, fsyncs
 * and syncs; the next boot of the same disk, ended in between as a power cut
 * would end it, runs `persistprobe read`, which reads the marker back. The
 * second boot shows that fsync pushed the write through the page cache to
 * the disk, since a write kept only in memory or dropped would leave the
 * file absent.
 *
 * Writes etude's report records (probe_report.h) for the case ETUDE_CASE
 * names, machine.persist.fsync without it. `write` reports `absent`, no
 * /persist.txt before the write; `written`, the file created and the marker
 * written whole; and `synced`, fsync and sync returning 0. `read` reports
 * `found`, the file opened; `whole`, the marker's length read; and `marker`,
 * the bytes equal to the marker. Beside them it prints "PERSISTPROBE OK
 * (wrote)" or "PERSISTPROBE OK (read back)" and exits 0, or prints
 * "PERSISTPROBE FAIL: " with the step and exits 1. Freestanding; direct
 * system calls (probe.h).
 */

#include "probe.h"
#include "probe_report.h"

#define O_RDONLY 0
#define O_WRONLY 1
#define O_CREAT  0x40
#define O_TRUNC  0x200

static const char MARKER[] = "RACH2 PERSIST MARKER v1\n";
#define MARKER_LEN (sizeof(MARKER) - 1)

static const char PATH[] = "/persist.txt";

/** exit(2); does not return. */
static void sys_exit(int status)
{
    syscall1(SYS_EXIT, status);
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

/** Write the string `s` and a newline to standard output. */
static void puts(const char *s)
{
    syscall3(SYS_WRITE, 1, (long)s, (int)strlen(s));
    syscall3(SYS_WRITE, 1, (long)"\n", 1);
}

/** Report the failure `why` of the planned assertion now checked, print
 * "PERSISTPROBE FAIL: " and `step`, and exit 1. */
static void fail(const char *why, const char *step)
{
    char line[64] = "PERSISTPROBE FAIL: ";
    size_t n = strlen(line), i;

    report_stop(why);
    for (i = 0; step[i] != '\0' && n < sizeof(line) - 1; i++) {
        line[n++] = step[i];
    }
    line[n] = '\0';
    puts(line);
    sys_exit(1);
}

/** Read the marker back from the file; it does not return. */
static void read_back(void)
{
    char buf[64];
    int fd, n;
    size_t i;

    report_plan("found");
    report_plan("whole");
    report_plan("marker");
    fd = syscall3(SYS_OPEN, (long)PATH, O_RDONLY, 0);
    if (fd < 0) {
        fail("/persist.txt did not open", "open for read");
    }
    report_held();
    n = syscall3(SYS_READ, fd, (long)buf, (int)sizeof(buf));
    syscall1(SYS_CLOSE, fd);
    if (n != (int)MARKER_LEN) {
        fail("the read did not give the marker's length", "short read");
    }
    report_held();
    for (i = 0; i < MARKER_LEN; ++i) {
        if (buf[i] != MARKER[i]) {
            fail("the bytes read are not the marker", "content mismatch");
        }
    }
    report_held();
    report_end();
    puts("PERSISTPROBE OK (read back)");
    sys_exit(0);
}

/** Write the marker to a new file and sync it; it does not return. */
static void write_new(void)
{
    int fd;

    report_plan("absent");
    report_plan("written");
    report_plan("synced");
    fd = syscall3(SYS_OPEN, (long)PATH, O_RDONLY, 0);
    if (fd >= 0) {
        syscall1(SYS_CLOSE, fd);
        fail("/persist.txt is there before the write", "the file exists");
    }
    report_held();
    fd = syscall3(SYS_OPEN, (long)PATH, O_WRONLY | O_CREAT | O_TRUNC, 0644);
    if (fd < 0) {
        fail("/persist.txt did not open for writing", "open for write");
    }
    if (syscall3(SYS_WRITE, fd, (long)MARKER, (int)MARKER_LEN) != (int)MARKER_LEN) {
        fail("the marker was not written whole", "write");
    }
    report_held();
    if (syscall1(SYS_FSYNC, fd) != 0) {
        fail("fsync failed", "fsync");
    }
    syscall1(SYS_CLOSE, fd);
    if (syscall1(SYS_SYNC, 0) != 0) {
        fail("sync failed", "sync");
    }
    report_held();
    report_end();
    puts("PERSISTPROBE OK (wrote)");
    sys_exit(0);
}

void probe_main(unsigned long *sp);

PROBE_SP_ENTRY;

/** The probe, entered with `sp` at argc: `read` reads the marker back, and
 * any other argument, or none, writes it. */
void probe_main(unsigned long *sp)
{
    int argc = (int)sp[0];
    char **argv = (char **)(sp + 1);
    char **envp = argv + argc + 1;
    const char *mode = argc >= 2 ? argv[1] : "";

    report_begin(report_case(envp, "machine.persist.fsync"));
    if (mode[0] == 'r' && mode[1] == 'e' && mode[2] == 'a' && mode[3] == 'd' && mode[4] == '\0') {
        read_back();
    }
    write_new();
}
