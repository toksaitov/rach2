/* Read and write real AHCI disks, count their interrupts, and run root faults
 * beside raw reads. Deadline and completion order use the host models.
 */

#include "probe.h"

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

/** Print "AHCIPROBE FAIL: " and `why`, kill and reap each child in
 * `children` that is still running, and exit 1. */
static void fail(const char *why)
{
    int i, status;

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

/** Read both disks through the real driver and require counted interrupts. */
static void read_row(void)
{
    static char text[4096];
    int i, status;

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
    if (slurp("/proc/interrupts", text, (int)sizeof(text)) <= 0 ||
        count_for_named("ahci", text) < 16) {
        fail("the controller's vector did not count the completions");
    }
    puts("AHCIPROBE READ both disks completed");
    puts("AHCIPROBE OK");
    syscall2(SYS_EXIT, 0, 0);
}

/** The `fault` mode: blockprobe on the root in a child, and raw reads of
 * the disk's second partition beside it until the child exits; the child
 * must pass and at least 64 raw reads must have run. */
static void fault_row(void)
{
    static char buf[CHUNK];
    static const char *const argv[] = {"/blockprobe", 0};
    static const char *const envp[] = {0};
    int child, status = -1, fd, reads = 0;

    buf[0] = 0;
    child = syscall1(SYS_FORK, 0);
    if (child < 0) {
        fail("fork");
    }
    if (child == 0) {
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
    if (((status >> 8) & 0xFF) != 0) {
        fail("blockprobe on the root beside the queue");
    }
    puts("AHCIPROBE READ raw reads beside the faults:");
    putnum(reads);
    puts("");
    if (reads < 64) {
        fail("too few raw reads ran beside the faults to have met one");
    }
    puts("AHCIPROBE OK");
    syscall2(SYS_EXIT, 0, 0);
}

void probe_main(unsigned long *sp);

#define LATE_BYTES (1280 * 1024)

/** Write an ext2 batch, read every byte back, and write another file. */
static void write_row(void)
{
    static unsigned char whole[LATE_BYTES];
    static unsigned char block[4096];
    int fd, i, j, start;

    if (syscall2(SYS_MKDIR, (long)"/late", 0755) != 0) {
        fail("mkdir /late");
    }
    if (syscall5(SYS_MOUNT, (long)"/dev/disk0p1", (long)"/late", (long)"ext2", 0, 0) != 0) {
        fail("the ext2 mount");
    }
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
    puts("AHCIPROBE WRITE ms:");
    putnum(now_ms() - start);
    if (syscall1(SYS_SYNC, 0) != 0) {
        fail("the sync after the batch");
    }
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
    fd = syscall3(SYS_OPEN, (long)"/late/again.bin", O_WRONLY | O_CREAT, 0644);
    if (fd < 0) {
        fail("the mount is not writable after the batch");
    }
    syscall1(SYS_CLOSE, fd);
    if (syscall1(SYS_SYNC, 0) != 0) {
        fail("the second sync");
    }
    if (syscall2(SYS_UMOUNT2, (long)"/late", 0) != 0) {
        fail("umount2 /late");
    }
    if (syscall1(SYS_RMDIR, (long)"/late") != 0) {
        fail("remove mount directory");
    }
    puts("AHCIPROBE OK");
    syscall2(SYS_EXIT, 0, 0);
}

PROBE_SP_ENTRY;

/** Run `fault`, `write`, or the two-disk read when no mode is supplied. */
void probe_main(unsigned long *sp)
{
    int argc = (int)sp[0];
    char **argv = (char **)(sp + 1);

    if (argc >= 2 && argv[1][0] == 'f') {
        fault_row();
    }
    if (argc >= 2 && argv[1][0] == 'w') {
        write_row();
    }
    read_row();
}
