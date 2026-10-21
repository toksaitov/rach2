/* Read and write real AHCI disks, count their interrupts, and run root faults
 * beside raw reads. Deadline and completion order use the host models.
 *
 * Writes etude's report records (probe_report.h) for the case ETUDE_CASE
 * names, or without it guest.queue.read, guest.queue.fault or
 * guest.queue.write by the mode; each mode's function says what its
 * assertions check.
 */

#include "probe.h"
#include "probe_report.h"

#define O_RDONLY        0
#define O_WRONLY        1
#define O_CREAT         0x40
#define O_TRUNC         0x200
#define CLOCK_MONOTONIC 1

#define FIRST_BYTES  (1024 * 1024)
#define SECOND_BYTES (256 * 1024)
#define CHUNK        (32 * 1024)
#define WNOHANG      1
#define SIGKILL      9

/* The children a failure must end: the readers and blockprobe, 0 when none. */
static int children[2];

/** The length of the string `s`. */
static size_t strlen(const char *s)
{
    size_t n = 0;
    while (s[n]) {
        n++;
    }
    return n;
}

/** Write `s` and a newline to standard output. */
static void puts(const char *s)
{
    syscall3(SYS_WRITE, 1, (long)s, (int)strlen(s));
    syscall3(SYS_WRITE, 1, (long)"\n", 1);
}

/** Write `v` in decimal. */
static void putnum(int v)
{
    char buf[12];
    int n = 0, i;

    if (v < 0) {
        syscall3(SYS_WRITE, 1, (long)"-", 1);
        v = -v;
    }
    do {
        buf[n++] = (char)('0' + v % 10);
        v /= 10;
    } while (v > 0);
    for (i = n - 1; i >= 0; i--) {
        syscall3(SYS_WRITE, 1, (long)&buf[i], 1);
    }
}

/** Report the failure `why` of the planned assertion now checked, print
 * "AHCIPROBE FAIL: " and `why`, kill and reap each child in `children` that
 * is still running, and exit 1. */
static void fail(const char *why)
{
    int i, status;

    report_stop(why);
    syscall3(SYS_WRITE, 1, (long)"AHCIPROBE FAIL: ", 16);
    puts(why);
    for (i = 0; i < 2; i++) {
        if (children[i] > 0 && syscall3(SYS_WAITPID, children[i], (long)&status, WNOHANG) == 0) {
            syscall2(SYS_KILL, children[i], SIGKILL);
            syscall3(SYS_WAITPID, children[i], (long)&status, 0);
        }
    }
    syscall2(SYS_EXIT, 1, 0);
}

/** The monotonic clock in milliseconds. */
static int now_ms(void)
{
    long ts[2];

    ts[0] = ts[1] = 0;
    syscall2(SYS_CLOCK_GETTIME, CLOCK_MONOTONIC, (long)ts);
    return ts[0] * 1000 + ts[1] / 1000000;
}

/** Open the partition node that the suite prepared before boot. */
static int open_node(const char *node)
{
    return syscall3(SYS_OPEN, (long)node, O_RDONLY, 0);
}

/** Read `bytes` from `node` in chunks; 0 on success, -1 on a short read. */
static int read_node(const char *node, int bytes)
{
    static char buf[CHUNK];
    int fd = open_node(node);
    int done = 0;

    buf[0] = 0;
    if (fd < 0) {
        return -1;
    }
    while (done < bytes) {
        int n = syscall3(SYS_READ, fd, (long)buf, CHUNK);

        if (n != CHUNK) {
            syscall2(SYS_CLOSE, fd, 0);
            return -1;
        }
        done += n;
    }
    syscall2(SYS_CLOSE, fd, 0);
    return 0;
}

/** Read the file `path` into `buf`, at most `cap` - 1 bytes and a NUL: the
 * bytes read, or -1 when it does not open. */
static int slurp(const char *path, char *buf, int cap)
{
    int fd = syscall3(SYS_OPEN, (long)path, O_RDONLY, 0);
    int n, total = 0;

    if (fd < 0) {
        return -1;
    }
    while (total < cap - 1) {
        n = syscall3(SYS_READ, fd, (long)(buf + total), cap - 1 - total);
        if (n <= 0) {
            break;
        }
        total += n;
    }
    syscall2(SYS_CLOSE, fd, 0);
    buf[total] = 0;
    return total;
}

/** The count on the `/proc/interrupts` line whose names include `name`, or
 * -1 when no line does. */
static int count_for_named(const char *name, const char *file)
{
    const char *p;
    int m = (int)strlen(name);

    for (p = file; *p; p++) {
        const char *line = p, *q;
        int count = 0;

        if (p != file && p[-1] != '\n') {
            continue;
        }
        while (*p >= '0' && *p <= '9') {
            p++;
        }
        if (p == line || *p != ':') {
            continue;
        }
        p++;
        while (*p == ' ' || *p == '\t') {
            p++;
        }
        while (*p >= '0' && *p <= '9') {
            count = count * 10 + (*p - '0');
            p++;
        }
        for (q = p; *q && *q != '\n'; q++) {
            int k;

            for (k = 0; k < m && q[k] == name[k]; k++) {
            }
            if (k == m && (q[m] == '\n' || q[m] == '\0' || q[m] == ',' || q[m] == ' ')) {
                return count;
            }
        }
    }
    return -1;
}

/** Read both disks through the real driver and require counted interrupts:
 * `both-read`, each disk read in chunks by a child of its own, and
 * `interrupts`, the controller's vector in /proc/interrupts counting at least
 * sixteen completions. */
static void read_row(void)
{
    static char text[4096];
    int i, status;

    report_plan("both-read");
    report_plan("interrupts");
    for (i = 0; i < 2; i++) {
        children[i] = syscall1(SYS_FORK, 0);
        if (children[i] < 0) {
            fail("fork");
        }
        if (children[i] == 0) {
            int rc = read_node(i == 0 ? "/dev/disk0p1" : "/dev/disk1p1",
                               i == 0 ? FIRST_BYTES : SECOND_BYTES);

            syscall2(SYS_EXIT, rc == 0 ? 0 : 3, 0);
        }
    }
    for (i = 0; i < 2; i++) {
        if (syscall3(SYS_WAITPID, children[i], (long)&status, 0) != children[i] || status != 0) {
            fail("a disk's read");
        }
        children[i] = 0;
    }
    report_held();
    if (slurp("/proc/interrupts", text, (int)sizeof(text)) <= 0 ||
        count_for_named("ahci", text) < 16) {
        fail("the controller's vector did not count the completions");
    }
    report_held();
    report_end();
    puts("AHCIPROBE READ both disks completed");
    puts("AHCIPROBE OK");
    syscall2(SYS_EXIT, 0, 0);
}

/**
 * The decimal value of `disk-drains` in /proc/disk: the queued commands a
 * polled command found active and finished first. -1 when the file or the
 * line is absent.
 */
static long disk_drains(void)
{
    static char text[20000];
    static const char key[] = "disk-drains ";
    int n = slurp("/proc/disk", text, (int)sizeof(text));
    int i, k;
    long v;

    for (i = 0; i < n; i++) {
        if (i > 0 && text[i - 1] != '\n') {
            continue;
        }
        for (k = 0; key[k] && text[i + k] == key[k]; k++) {
        }
        if (key[k] || text[i + k] < '0' || text[i + k] > '9') {
            continue;
        }
        for (v = 0, i += k; text[i] >= '0' && text[i] <= '9'; i++) {
            v = v * 10 + (text[i] - '0');
        }
        return v;
    }
    return -1;
}

/**
 * The `fault` mode: blockprobe on the root in a child, and raw reads of the
 * disk's second partition beside it until the child exits. The child must
 * pass, and `disk-drains` must have risen: a page fault's polled command
 * found a raw read active on the port and finished it first, the meeting of
 * the fault path and the queue that the row is for. How many raw reads ran
 * beside the child depends on the speeds of the two, and is printed, not
 * judged.
 *
 * Its assertions are `counter`, /proc/disk's `disk-drains`; `raw-reads`,
 * every raw read beside the child; `blockprobe`, the child's exit with
 * status 0; and `drained`, the count risen. blockprobe's output goes to
 * standard error, so that its report records are not taken for this one's.
 */
static void fault_row(void)
{
    static char buf[CHUNK];
    static const char *const argv[] = {"/blockprobe", 0};
    static const char *const envp[] = {0};
    int child, status = -1, fd, reads = 0;
    long before, after;

    report_plan("counter");
    report_plan("raw-reads");
    report_plan("blockprobe");
    report_plan("drained");
    buf[0] = 0;
    before = disk_drains();
    if (before < 0) {
        fail("/proc/disk has no disk-drains");
    }
    report_held();
    child = syscall1(SYS_FORK, 0);
    if (child < 0) {
        fail("fork");
    }
    if (child == 0) {
        syscall2(SYS_DUP2, 2, 1);
        syscall3(SYS_EXECVE, (long)"/blockprobe", (long)argv, (long)envp);
        syscall2(SYS_EXIT, 4, 0);
    }
    children[0] = child;
    fd = syscall3(SYS_OPEN, (long)"/dev/disk0p2", O_RDONLY, 0);
    if (fd < 0) {
        fail("/dev/disk0p2");
    }
    while (syscall3(SYS_WAITPID, child, (long)&status, 1) == 0) {
        int n = syscall3(SYS_READ, fd, (long)buf, CHUNK);

        if (n < 0) {
            fail("a raw read beside the faults");
        }
        if (n == 0) {
            syscall2(SYS_CLOSE, fd, 0);
            fd = syscall3(SYS_OPEN, (long)"/dev/disk0p2", O_RDONLY, 0);
            if (fd < 0) {
                fail("/dev/disk0p2 again");
            }
        }
        reads++;
    }
    syscall2(SYS_CLOSE, fd, 0);
    report_held();
    if (((status >> 8) & 0xFF) != 0) {
        fail("blockprobe on the root beside the queue");
    }
    report_held();
    after = disk_drains();
    puts("AHCIPROBE READ raw reads beside the faults:");
    putnum(reads);
    puts("");
    puts("AHCIPROBE READ queued reads the faults finished first:");
    putnum((int)(after - before));
    puts("");
    if (after <= before) {
        fail("no fault's command found a raw read active on the port");
    }
    report_held();
    report_end();
    puts("AHCIPROBE OK");
    syscall2(SYS_EXIT, 0, 0);
}

void probe_main(unsigned long *sp);

#define LATE_BYTES (1280 * 1024)

/** Write an ext2 batch, read every byte back, and write another file:
 * `mounted`, the volume mounted on /late; `written`, the batch written in one
 * call; `synced`, the sync after it; `read-back`, every byte read back as
 * written; `writable`, a second file made and synced after the batch, which
 * a mount that went read-only refuses; and `unmounted`, the volume unmounted
 * and /late removed. */
static void write_row(void)
{
    static unsigned char whole[LATE_BYTES];
    static unsigned char block[4096];
    int fd, i, j, start;

    report_plan("mounted");
    report_plan("written");
    report_plan("synced");
    report_plan("read-back");
    report_plan("writable");
    report_plan("unmounted");
    if (syscall2(SYS_MKDIR, (long)"/late", 0755) != 0) {
        fail("mkdir /late");
    }
    if (syscall5(SYS_MOUNT, (long)"/dev/disk0p1", (long)"/late", (long)"ext2", 0, 0) != 0) {
        fail("the ext2 mount");
    }
    report_held();
    fd = syscall3(SYS_OPEN, (long)"/late/late.bin", O_WRONLY | O_CREAT | O_TRUNC, 0644);
    if (fd < 0) {
        fail("create late.bin");
    }
    for (i = 0; i < LATE_BYTES; i++) {
        whole[i] = (unsigned char)((i / (int)sizeof(block)) * 7 + i % (int)sizeof(block));
    }
    start = now_ms();
    if (syscall3(SYS_WRITE, fd, (long)whole, LATE_BYTES) != LATE_BYTES) {
        fail("the write of late.bin");
    }
    syscall1(SYS_CLOSE, fd);
    report_held();
    puts("AHCIPROBE WRITE ms:");
    putnum(now_ms() - start);
    puts("");
    if (syscall1(SYS_SYNC, 0) != 0) {
        fail("the sync after the batch");
    }
    report_held();
    fd = syscall3(SYS_OPEN, (long)"/late/late.bin", O_RDONLY, 0);
    if (fd < 0) {
        fail("open late.bin to read it back");
    }
    for (i = 0; i < LATE_BYTES / (int)sizeof(block); i++) {
        if (syscall3(SYS_READ, fd, (long)block, (int)sizeof(block)) != (int)sizeof(block)) {
            fail("a read of late.bin");
        }
        for (j = 0; j < (int)sizeof(block); j++) {
            if (block[j] != (unsigned char)(i * 7 + j)) {
                fail("the bytes read back are not what was written");
            }
        }
    }
    syscall1(SYS_CLOSE, fd);
    report_held();
    fd = syscall3(SYS_OPEN, (long)"/late/again.bin", O_WRONLY | O_CREAT, 0644);
    if (fd < 0) {
        fail("the mount is not writable after the batch");
    }
    syscall1(SYS_CLOSE, fd);
    if (syscall1(SYS_SYNC, 0) != 0) {
        fail("the second sync");
    }
    report_held();
    if (syscall2(SYS_UMOUNT2, (long)"/late", 0) != 0) {
        fail("umount2 /late");
    }
    if (syscall1(SYS_RMDIR, (long)"/late") != 0) {
        fail("remove mount directory");
    }
    report_held();
    report_end();
    puts("AHCIPROBE OK");
    syscall2(SYS_EXIT, 0, 0);
}

PROBE_SP_ENTRY;

/** Run `fault`, `write`, or the two-disk read when no mode is supplied. */
void probe_main(unsigned long *sp)
{
    int argc = (int)sp[0];
    char **argv = (char **)(sp + 1);
    char **envp = argv + argc + 1;

    if (argc >= 2 && argv[1][0] == 'f') {
        report_begin(report_case(envp, "guest.queue.fault"));
        fault_row();
    }
    if (argc >= 2 && argv[1][0] == 'w') {
        report_begin(report_case(envp, "guest.queue.write"));
        write_row();
    }
    report_begin(report_case(envp, "guest.queue.read"));
    read_row();
}
