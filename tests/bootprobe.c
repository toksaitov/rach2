/**
 * Exercises /proc/cmdline and the raw windows onto the boot disk:
 * /dev/bootpart, /dev/grubcore and /dev/disk0.
 *
 * Several boots share this one probe, told apart by the command line the
 * harness passes. With `rach2.bootprobe=fixture` the disk has a FAT-typed
 * partition 1 of known bytes (sector k has byte j = (k * 7 + j) & 0xff), and
 * the probe proves the device end to end: the 0600 mode from stat and fstat
 * both, seeks resolved against the partition size, a read of known bytes at a
 * seek, a written sector read back, exactly 0 at the end, -ENOSPC past it,
 * -EINVAL unaligned, and -EACCES for an unprivileged open. With
 * `rach2.bootprobe=ramroot` the machine mounted a RAM image and no disk file
 * system, and both /dev/bootpart and /dev/grubcore must exist anyway, the
 * gap bounded and refusing as the fixture's partition does. With
 * `rach2.bootprobe=wide` a sparse 260 GB drive is attached over AHCI with
 * markers at 5 GiB and 258 GiB: the far one needs 38 bits of offset, so an
 * implementation keeping fewer misses it however well it does at 5 GiB, and the
 * boot also walks SEEK_CUR across the 4 GiB line both ways, refuses a seek
 * before the start, and duplicates a descriptor to prove the wide offset is
 * shared through the open-file description. With `rach2.bootprobe=auth` and
 * `rach2.bootprobe=auth-recovery` the probe checks whether the kernel may write
 * its own disk (`auth_case`) and whether the bounded windows batch
 * (`window_batch_case`); with `rach2.bootprobe=shared`, behind a delayed
 * device, it races a transfer on an inherited description with another
 * (`raw_readv_case`, `raw_shared_case`). Without the flag the disk has only
 * the KRFFS partition and the device must be absent from stat and open
 * alike. /proc/cmdline is proved at each of its layers in every boot:
 * content, the /proc listing, stat/fstat identity, and the fd's readlink
 * origin.
 *
 * Prints "BOOTPROBE OK", "BOOTPROBE ABSENT OK", "BOOTPROBE RAMROOT OK",
 * "BOOTPROBE WIDE OK", "BOOTPROBE SHARED OK", "BOOTPROBE AUTH ORDINARY OK"
 * or "BOOTPROBE AUTH RECOVERY OK", and "BOOTPROBE FAILED" when a check
 * failed.
 *
 * Freestanding; direct system calls (probe.h).
 */

#include "probe.h"

#define O_RDONLY 0
#define O_WRONLY 1
#define O_RDWR   2

#define SEEK_SET 0
#define SEEK_CUR 1
#define SEEK_END 2

#define ENOENT    2
#define EPERM     1
#define EACCES    13
#define EINVAL    22
#define EOVERFLOW 75
#define ENOSPC    28
#define EBUSY     16

#define PART_SECTORS 1024
#define SECTOR       512

/** Whether `buf` has the 512-byte marker this fixture writes at `gib` GiB. */
static int pattern_ok(const char *buf, int gib)
{
    int j;

    for (j = 0; j < 512; j++) {
        if ((unsigned char)buf[j] != (unsigned char)((j * 7 + 3 + gib) & 0xff)) {
            return 0;
        }
    }
    return 1;
}

/** exit(2) with `status`; does not return. */
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

/** Write `s` and a newline to standard output. */
static void puts(const char *s)
{
    syscall3(SYS_WRITE, 1, (long)s, (int)strlen(s));
    syscall3(SYS_WRITE, 1, (long)"\n", 1);
}

static int fails = 0;

/** Count a failure and print `msg` when `cond` is 0. */
static void check(int cond, const char *msg)
{
    if (!cond) {
        fails++;
        syscall3(SYS_WRITE, 1, (long)"  FAIL: ", 8);
        puts(msg);
    }
}

/** Whether `hay` (NUL-terminated) contains `needle`. */
static int contains(const char *hay, const char *needle)
{
    size_t i, j;

    for (i = 0; hay[i]; i++) {
        for (j = 0; needle[j] && hay[i + j] == needle[j]; j++) {
        }
        if (!needle[j]) {
            return 1;
        }
    }
    return needle[0] == 0;
}

static char cmdline[300];
static char statbuf[KSTAT_SIZE];
static char fstatbuf[KSTAT_SIZE];
static char sector[SECTOR];
static char big[200 * SECTOR];
/* More than one batch of 2048 sectors: a read or write of this many is two
   driver calls, and the bytes at the boundary between them are checked. */
#define PAST_SECTORS 2100
static char past[PAST_SECTORS * SECTOR];

/**
 * Read the decimal field after `key` at the start of a line of /proc/disk
 * into `*out`: 1, or 0 when the file cannot be read or has no such line.
 *
 * The file is small and read whole each time, because the fields this looks
 * for change as the machine runs and a cached copy would answer with the
 * machine's past.
 */
static int proc_disk_field(const char *key, unsigned long long *out)
{
    static char text[2048];
    int fd, n, i, j;
    unsigned long long v = 0;

    fd = syscall3(SYS_OPEN, (long)"/proc/disk", O_RDONLY, 0);
    if (fd < 0) {
        return 0;
    }
    n = syscall3(SYS_READ, fd, (long)text, (int)sizeof(text) - 1);
    syscall1(SYS_CLOSE, fd);
    if (n <= 0) {
        return 0;
    }
    text[n] = '\0';
    for (i = 0; i < n; i++) {
        if (i != 0 && text[i - 1] != '\n') {
            continue;
        }
        for (j = 0; key[j] && text[i + j] == key[j]; j++) {
        }
        if (key[j]) {
            continue;
        }
        i += j;
        if (text[i] < '0' || text[i] > '9') {
            return 0;
        }
        while (text[i] >= '0' && text[i] <= '9') {
            v = v * 10 + (unsigned long long)(text[i] - '0');
            i++;
        }
        *out = v;
        return 1;
    }
    return 0;
}

/**
 * Check whether this kernel may write any sector of its own disk.
 *
 * Both boots run this against the same image and differ only in the kernel:
 * `recovery` says which one is running. An ordinary build must refuse and leave
 * the sector as it found it, so the refusal is shown to have written nothing,
 * not merely returned an error. A recovery build must write past 2 GiB,
 * read it back, and take a large write in few driver calls, which is what makes
 * a whole-disk reflash finishable: batching is invisible in the result, so the
 * count of calls is read out of /proc/disk either side of it. What the driver
 * did with each call decides whether a reflash finishes, so the disk commands
 * and the cache flushes are counted as they leave for the device, and a
 * transfer longer than one batch must take two calls with the bytes on both
 * sides of the boundary between them intact.
 *
 * One writer at a time is allowed, across every window onto this disk. Two
 * reflashes over ranges that overlap each write their whole length and each
 * report success, and what is on the disk is neither image; and the windows
 * are not three disks, since the boot partition and the loader's gap are both
 * inside the whole one. So the second writer is refused whichever name it
 * asks by. A reader is not a writer and is not counted.
 */
static void auth_case(int recovery)
{
    long long at = 3LL * 1024 * 1024 * 1024;
    long long pos = 0;
    int fd, rc, n, j;

    fd = syscall3(SYS_OPEN, (long)"/dev/disk0", O_RDWR, 0);
    check(fd >= 0, "root opens /dev/disk0 read-write");

    /* One writer at a time, across every window onto this disk. */
    {
        int second = syscall3(SYS_OPEN, (long)"/dev/disk0", O_RDWR, 0);

        check(second == -EBUSY, "a second writer of the whole disk is refused");
        second = syscall3(SYS_OPEN, (long)"/dev/disk0", O_WRONLY, 0);
        check(second == -EBUSY, "however it asks for the write");
        second = syscall3(SYS_OPEN, (long)"/dev/bootpart", O_RDWR, 0);
        check(second == -EBUSY || second == -ENOENT,
              "a writer of the boot partition is refused while the whole disk is held");
        second = syscall3(SYS_OPEN, (long)"/dev/grubcore", O_RDWR, 0);
        check(second == -EBUSY || second == -ENOENT, "and so is a writer of the loader's gap");
        second = syscall3(SYS_OPEN, (long)"/dev/disk0", O_RDONLY, 0);
        check(second >= 0, "but a reader alongside it is not");
        if (second >= 0) {
            syscall1(SYS_CLOSE, second);
        }
    }

    rc = probe_llseek(fd, at, SEEK_SET, &pos);
    check(rc == 0 && pos == at, "seek to 3 GiB");

    for (j = 0; j < SECTOR; j++) {
        sector[j] = (char)((j * 11 + 5) & 0xff);
    }
    n = syscall3(SYS_WRITE, fd, (long)sector, SECTOR);

    if (!recovery) {
        check(n == -EPERM, "an everyday kernel refuses the write");
        rc = probe_llseek(fd, at, SEEK_SET, &pos);
        check(rc == 0, "seek back to it");
        n = syscall3(SYS_READ, fd, (long)sector, SECTOR);
        check(n == SECTOR, "but still reads it");
        {
            int ok = 1;

            for (j = 0; j < SECTOR; j++) {
                if (sector[j] != 0) {
                    ok = 0;
                    break;
                }
            }
            check(ok, "and the sector is still zero, so the refusal wrote nothing");
        }
        syscall1(SYS_CLOSE, fd);
        return;
    }

    check(n == SECTOR, "a recovery kernel writes a sector past 2 GiB");
    rc = probe_llseek(fd, at, SEEK_SET, &pos);
    check(rc == 0, "seek back to it");
    for (j = 0; j < SECTOR; j++) {
        sector[j] = 0;
    }
    n = syscall3(SYS_READ, fd, (long)sector, SECTOR);
    check(n == SECTOR, "and reads it back");
    {
        int ok = 1;

        for (j = 0; j < SECTOR; j++) {
            if ((unsigned char)sector[j] != (unsigned char)((j * 11 + 5) & 0xff)) {
                ok = 0;
                break;
            }
        }
        check(ok, "byte for byte");
    }

    {
        unsigned long long rb = 0, ra = 0;

        rc = probe_llseek(fd, at, SEEK_SET, &pos);
        check(rc == 0, "seek back for a batched read");
        check(proc_disk_field("raw-driver-calls ", &rb), "the call count is readable");
        n = syscall3(SYS_READ, fd, (long)big, 200 * SECTOR);
        check(n == 200 * SECTOR, "a 200-sector read of the whole-disk window is taken whole");
        check(proc_disk_field("raw-driver-calls ", &ra) && ra - rb == 1,
              "and took one driver call, the batch holding the whole read");
    }

    rc = probe_llseek(fd, at, SEEK_SET, &pos);
    check(rc == 0, "seek back once more");
    {
        unsigned long long before = 0, after = 0, batch = 0;
        int got_before, got_after;

        unsigned long long cmd_before = 0, cmd_after = 0;
        unsigned long long flush_before = 0, flush_after = 0;

        got_before = proc_disk_field("raw-driver-calls ", &before);
        (void)proc_disk_field("disk-commands ", &cmd_before);
        (void)proc_disk_field("disk-flushes ", &flush_before);
        n = syscall3(SYS_WRITE, fd, (long)big, 200 * SECTOR);
        check(n == 200 * SECTOR, "a 200-sector write is taken whole");
        got_after = proc_disk_field("raw-driver-calls ", &after);
        check(got_before && got_after, "/proc/disk reports a driver-call count");
        check(after > before, "the write is visible in it");
        check(after - before == 1,
              "and took one driver call, not two hundred: the batch holds 200 sectors");
        check(proc_disk_field("raw-batch-sectors ", &batch) && batch == 2048,
              "/proc/disk reports the batch size the count should be read against");

        /* The driver's commands and cache flushes for the batch. */
        check(proc_disk_field("disk-commands ", &cmd_after), "/proc/disk reports disk commands");
        check(proc_disk_field("disk-flushes ", &flush_after), "and cache flushes");
        check(cmd_after - cmd_before == 2,
              "and two disk commands, the write and its flush, not four hundred");
        check(flush_after - flush_before == 1, "and exactly one cache flush, not two hundred");
    }

    /* A transfer longer than one batch: two calls, and the boundary between. */
    rc = probe_llseek(fd, at, SEEK_SET, &pos);
    check(rc == 0, "seek back for a transfer past one batch");
    {
        unsigned long long before = 0, after = 0, cmd_before = 0, cmd_after = 0;
        unsigned long long flush_before = 0, flush_after = 0;
        long i;
        int ok = 1;

        for (i = 0; i < PAST_SECTORS * SECTOR; i++) {
            past[i] = (char)((i / SECTOR) * 13 + (i % SECTOR) * 7 + 1);
        }
        (void)proc_disk_field("raw-driver-calls ", &before);
        (void)proc_disk_field("disk-commands ", &cmd_before);
        (void)proc_disk_field("disk-flushes ", &flush_before);
        n = syscall3(SYS_WRITE, fd, (long)past, PAST_SECTORS * SECTOR);
        check(n == PAST_SECTORS * SECTOR, "a 2100-sector write is taken whole");
        check(proc_disk_field("raw-driver-calls ", &after) && after - before == 2,
              "and took two driver calls, a batch of 2048 and one of 52");
        check(proc_disk_field("disk-commands ", &cmd_after) && cmd_after - cmd_before == 4,
              "and four disk commands, two writes and their two flushes");
        check(proc_disk_field("disk-flushes ", &flush_after) && flush_after - flush_before == 2,
              "and two cache flushes, one per batch");
        rc = probe_llseek(fd, at, SEEK_SET, &pos);
        check(rc == 0, "seek back to read it");
        for (i = 0; i < PAST_SECTORS * SECTOR; i++) {
            past[i] = 0;
        }
        (void)proc_disk_field("raw-driver-calls ", &before);
        n = syscall3(SYS_READ, fd, (long)past, PAST_SECTORS * SECTOR);
        check(n == PAST_SECTORS * SECTOR, "a 2100-sector read is taken whole");
        check(proc_disk_field("raw-driver-calls ", &after) && after - before == 2,
              "in two driver calls");
        for (i = 0; i < PAST_SECTORS * SECTOR; i++) {
            if (past[i] != (char)((i / SECTOR) * 13 + (i % SECTOR) * 7 + 1)) {
                ok = 0;
                break;
            }
        }
        check(ok, "and the bytes are the ones written, across the boundary between the batches");
        check(ok || i / SECTOR < 2047 || i / SECTOR > 2049,
              "the first wrong byte, if any, is not at the boundary");
    }
    syscall1(SYS_CLOSE, fd);
}

/**
 * Check that the bounded windows batch too, in both directions.
 *
 * The whole-disk window is the one a reflash uses, but all three share the same
 * loops, and a batching regression in the two bounded ones would be just as
 * slow and just as invisible in the result. This drives /dev/grubcore, which
 * every boot with a readable disk has, over more sectors than one batch has.
 */
static void window_batch_case(void)
{
    unsigned long long before = 0, after = 0;
    int fd, rc, n;

    fd = syscall3(SYS_OPEN, (long)"/dev/grubcore", O_RDONLY, 0);
    check(fd >= 0, "root opens /dev/grubcore");
    rc = syscall3(SYS_LSEEK, fd, 0, SEEK_SET);
    check(rc == 0, "seek to the start of the gap");

    check(proc_disk_field("raw-driver-calls ", &before), "the driver-call count is readable");
    n = syscall3(SYS_READ, fd, (long)big, 200 * SECTOR);
    check(n == 200 * SECTOR, "a 200-sector read of the gap is taken whole");
    check(proc_disk_field("raw-driver-calls ", &after), "and the count is readable after");
    check(after - before == 1, "and took one driver call, not two hundred");
    syscall1(SYS_CLOSE, fd);

    fd = syscall3(SYS_OPEN, (long)"/dev/grubcore", O_RDWR, 0);
    check(fd >= 0, "root opens /dev/grubcore read-write");
    rc = syscall3(SYS_LSEEK, fd, 0, SEEK_SET);
    check(rc == 0, "seek to the start of the gap again");
    check(proc_disk_field("raw-driver-calls ", &before), "the call count is readable again");
    n = syscall3(SYS_WRITE, fd, (long)big, 200 * SECTOR);
    check(n == 200 * SECTOR, "a 200-sector write of the gap is taken whole");
    check(proc_disk_field("raw-driver-calls ", &after) && after - before == 1,
          "and took one driver call, so the bounded write loop batches too");
    syscall1(SYS_CLOSE, fd);
}

static char written[SECTOR];

/**
 * Wait until the child is blocked in its raw transfer, after its pipe signal:
 * in the state D, a sleep no signal ends, as Linux reports a process waiting
 * on its disk. The delayed device provides the wait; no elapsed-time
 * assertion is needed. 1 once the state reads D; 0 when the child is a
 * zombie or its stat does not open, or after 100000 reads without a D.
 */
static int raw_child_sleeping(int pid)
{
    char path[40], digits[12], status[256];
    int n = 0, at = 6, fd, got, i, tries;
    unsigned int value = (unsigned int)pid;
    const char *prefix = "/proc/";
    const char *suffix = "/stat";

    for (i = 0; i < 6; i++) {
        path[i] = prefix[i];
    }
    do {
        digits[n++] = (char)('0' + value % 10);
        value /= 10;
    } while (value);
    while (n) {
        path[at++] = digits[--n];
    }
    for (i = 0; i <= 5; i++) {
        path[at++] = suffix[i];
    }
    for (tries = 0; tries < 100000; tries++) {
        fd = syscall3(SYS_OPEN, (long)path, O_RDONLY, 0);
        if (fd < 0) {
            return 0;
        }
        got = syscall3(SYS_READ, fd, (long)status, sizeof(status));
        syscall1(SYS_CLOSE, fd);
        for (i = got - 4; i >= 0; i--) {
            if (status[i] == ')' && status[i + 1] == ' ') {
                if (status[i + 2] == 'D') {
                    return 1;
                }
                if (status[i + 2] == 'Z') {
                    return 0;
                }
                break;
            }
        }
    }
    return 0;
}

/** Whether `bytes` is sector `which` of the boot fixture's pattern, byte i
 * being (which * 7 + i) & 255. */
static int raw_pattern(const char *bytes, int which)
{
    int i;

    for (i = 0; i < SECTOR; i++) {
        if ((unsigned char)bytes[i] != (unsigned char)((which * 7 + i) & 255)) {
            return 0;
        }
    }
    return 1;
}

/**
 * Race a read with the second vector of an inherited raw readv. A child's
 * readv of two sectors on /dev/disk0p1 is seen blocked after its second
 * driver call; a read through the parent's copy of the description then
 * takes the third sector, so the readv reserved its second, and the shared
 * offset ends three sectors in.
 */
static void raw_readv_case(void)
{
    struct {
        char *base;
        size_t length;
    } vec[2];
    char bytes[2 * SECTOR], signal;
    unsigned long long before = 0, after = 0;
    int fd, pid, pipefd[2], rc, status, tries;

    fd = syscall3(SYS_OPEN, (long)"/dev/disk0p1", O_RDONLY, 0);
    check(fd >= 0, "open raw readv description");
    if (fd < 0) {
        return;
    }
    rc = syscall1(SYS_PIPE, (long)pipefd);
    check(rc == 0, "create raw readv rendezvous");
    if (rc != 0) {
        syscall1(SYS_CLOSE, fd);
        return;
    }
    check(proc_disk_field("raw-driver-calls ", &before), "read raw readv starting count");
    pid = syscall1(SYS_FORK, 0);
    check(pid >= 0, "fork raw readv");
    if (pid == 0) {
        syscall1(SYS_CLOSE, pipefd[0]);
        vec[0].base = bytes;
        vec[0].length = SECTOR;
        vec[1].base = bytes + SECTOR;
        vec[1].length = SECTOR;
        if (syscall3(SYS_WRITE, pipefd[1], (long)"x", 1) != 1) {
            sys_exit(1);
        }
        rc = syscall3(SYS_READV, fd, (long)vec, 2);
        sys_exit(rc != 2 * SECTOR || !raw_pattern(bytes, 0) || !raw_pattern(bytes + SECTOR, 1));
    }
    syscall1(SYS_CLOSE, pipefd[1]);
    if (pid > 0) {
        check(syscall3(SYS_READ, pipefd[0], (long)&signal, 1) == 1, "child starts raw readv");
        for (tries = 0; tries < 100000; tries++) {
            if (!proc_disk_field("raw-driver-calls ", &after) || after >= before + 2) {
                break;
            }
        }
        check(after == before + 2 && raw_child_sleeping(pid), "second raw vector blocks");
        rc = syscall3(SYS_READ, fd, (long)bytes, SECTOR);
        check(rc == SECTOR && raw_pattern(bytes, 2), "raw readv reserves its second sector");
        status = -1;
        check(syscall3(SYS_WAITPID, pid, (long)&status, 0) == pid && status == 0,
              "raw readv child exits successfully");
        check(syscall3(SYS_LSEEK, fd, 0, SEEK_CUR) == 3 * SECTOR,
              "raw readv retains all three offset updates");
    }
    syscall1(SYS_CLOSE, pipefd[0]);
    syscall1(SYS_CLOSE, fd);
}

/** Require both concurrent raw writes at their distinct sector offsets. */
static void raw_write_readback(int fd)
{
    char bytes[SECTOR];
    int i, rc;

    check(syscall3(SYS_LSEEK, fd, 0, SEEK_SET) == 0, "rewind raw writes");
    for (i = 0; i < 2; i++) {
        int j, ok = 1;

        rc = syscall3(SYS_READ, fd, (long)bytes, SECTOR);
        for (j = 0; j < SECTOR; j++) {
            if (bytes[j] != (char)('a' + i)) {
                ok = 0;
            }
        }
        check(rc == SECTOR && ok, "raw writers use separate sectors");
    }
}

/**
 * Race an inherited description's transfer with a read, write or relative seek.
 * Kinds 0 and 1 cover sector and byte readers; 2 writes; 3 and 4 seek through
 * the two syscall ABIs, which on x86-64 are both lseek. Both calls must
 * advance the shared position once.
 */
static void raw_shared_case(int kind)
{
    const char *path = kind == 1 ? "/dev/disk0p1" : "/dev/bootpart";
    char bytes[SECTOR], signal;
    int pipefd[2], fd, pid, status, rc, i;
    long long position = -1;

    fd = syscall3(SYS_OPEN, (long)path, kind == 2 ? O_RDWR : O_RDONLY, 0);
    check(fd >= 0, "open shared raw description");
    if (fd < 0) {
        return;
    }
    rc = syscall1(SYS_PIPE, (long)pipefd);
    check(rc == 0, "create raw transfer rendezvous");
    if (rc != 0) {
        syscall1(SYS_CLOSE, fd);
        return;
    }
    pid = syscall1(SYS_FORK, 0);
    check(pid >= 0, "fork raw transfer");
    if (pid == 0) {
        syscall1(SYS_CLOSE, pipefd[0]);
        for (i = 0; i < SECTOR; i++) {
            bytes[i] = 'a';
        }
        if (syscall3(SYS_WRITE, pipefd[1], (long)"x", 1) != 1) {
            sys_exit(1);
        }
        rc = syscall3(kind == 2 ? SYS_WRITE : SYS_READ, fd, (long)bytes, SECTOR);
        if (rc != SECTOR || (kind < 2 && !raw_pattern(bytes, 0))) {
            sys_exit(1);
        }
        sys_exit(0);
    }
    syscall1(SYS_CLOSE, pipefd[1]);
    if (pid > 0) {
        check(syscall3(SYS_READ, pipefd[0], (long)&signal, 1) == 1, "child starts raw transfer");
        check(raw_child_sleeping(pid), "child blocks in raw transfer");
        if (kind < 2) {
            rc = syscall3(SYS_READ, fd, (long)bytes, SECTOR);
            check(rc == SECTOR && raw_pattern(bytes, 1),
                  "concurrent raw reader consumes second sector");
        } else if (kind == 2) {
            for (i = 0; i < SECTOR; i++) {
                bytes[i] = 'b';
            }
            check(syscall3(SYS_WRITE, fd, (long)bytes, SECTOR) == SECTOR,
                  "concurrent raw writer completes");
        } else if (kind == 3) {
            check(syscall3(SYS_LSEEK, fd, SECTOR, SEEK_CUR) == 2 * SECTOR,
                  "raw lseek waits for transfer offset");
        } else {
            rc = probe_llseek(fd, SECTOR, SEEK_CUR, &position);
            check(rc == 0 && position == 2 * SECTOR, "raw llseek waits for transfer offset");
        }
        status = -1;
        rc = syscall3(SYS_WAITPID, pid, (long)&status, 0);
        check(rc == pid && status == 0, "raw child exits successfully");
        check(syscall3(SYS_LSEEK, fd, 0, SEEK_CUR) == 2 * SECTOR,
              "raw shared offset retains both operations");
        if (kind == 2) {
            raw_write_readback(fd);
        }
    }
    syscall1(SYS_CLOSE, pipefd[0]);
    syscall1(SYS_CLOSE, fd);
}

/** Require cmdline in the proc directory listing. */
static void proc_listing_case(void)
{
    int fd, n;

    fd = syscall3(SYS_OPEN, (long)"/proc", O_RDONLY, 0);
    check(fd >= 0, "open /proc as a directory");
    if (fd >= 0) {
        char dbuf[512];
        int saw = 0, off = 0;

        n = syscall3(SYS_GETDENTS64, fd, (long)dbuf, (int)sizeof(dbuf));
        while (off + 19 < n) {
            unsigned short reclen = *(unsigned short *)(dbuf + off + 16);
            const char *nm = dbuf + off + 19;

            if (contains(nm, "cmdline")) {
                saw = 1;
            }
            if (reclen == 0) {
                break;
            }
            off += reclen;
        }
        check(saw, "getdents /proc lists cmdline");
        syscall1(SYS_CLOSE, fd);
    }
}

/**
 * Check /proc/cmdline, then run the boot's mode, each ending with its token:
 * `shared`, `ramroot`, `auth`, `wide`, the absent device when no mode is
 * named, and the fixture.
 *
 * A machine that mounted a RAM image and no disk file system still has a
 * disk, and both of its level-one devices: the boot partition it may have to
 * repair, and the gap that holds the boot loader. A device's mode stops a
 * caller other than root at open and nothing else, since the credential
 * check passes uid 0 whatever the mode says; so the write handler checks
 * again, and a descriptor opened while privileged and carried across a drop
 * of privilege must stop writing.
 */
PROBE_START void _start(void)
{
    int fd, rc, n, fixture;
    unsigned int mode, ino_stat, ino_fstat;

    /* /proc/cmdline: content, then the descriptor's readlink origin. */
    fd = syscall3(SYS_OPEN, (long)"/proc/cmdline", O_RDONLY, 0);
    check(fd >= 0, "open /proc/cmdline");
    n = syscall3(SYS_READ, fd, (long)cmdline, (int)sizeof(cmdline) - 1);
    check(n > 0, "read /proc/cmdline");
    if (n > 0) {
        cmdline[n] = '\0';
    }
    /* The RAM-root, wide and auth boots ask for no disk root. */
    if (contains(cmdline, "rach2.bootprobe=ramroot") || contains(cmdline, "rach2.bootprobe=wide") ||
        contains(cmdline, "rach2.bootprobe=auth")) {
        check(!contains(cmdline, "rach2.fsdisk"), "a RAM-root boot asks for no disk root");
    } else {
        check(contains(cmdline, "rach2.fsdisk"), "cmdline has rach2.fsdisk");
    }
    check(n > 0 && cmdline[n - 1] == '\n', "cmdline ends with a newline");
    {
        char link[32] = "";
        char path[24] = "/proc/self/fd/";
        int d = fd, len = 0;
        char digits[8];
        int k = 0;

        do {
            digits[k++] = (char)('0' + d % 10);
            d /= 10;
        } while (d > 0);
        while (path[len]) {
            len++;
        }
        while (k > 0) {
            path[len++] = digits[--k];
        }
        path[len] = '\0';
        rc = syscall3(SYS_READLINK, (long)path, (long)link, (int)sizeof(link) - 1);
        check(rc > 0, "readlink the cmdline fd");
        if (rc > 0) {
            link[rc] = '\0';
        }
        check(rc > 0 && contains(link, "/proc/cmdline"), "the fd's origin is /proc/cmdline");
    }

    /* stat and fstat agree on the file's identity. */
    rc = syscall3(SYS_STAT, (long)"/proc/cmdline", (long)statbuf, 0);
    check(rc == 0, "stat64 /proc/cmdline");
    rc = syscall3(SYS_FSTAT, fd, (long)fstatbuf, 0);
    check(rc == 0, "fstat64 the cmdline fd");
    mode = *(unsigned int *)(statbuf + KSTAT_MODE);
    check(mode == 0100444, "stat mode is a read-only regular file");
    ino_stat = *(unsigned int *)(statbuf + KSTAT_INO);
    ino_fstat = *(unsigned int *)(fstatbuf + KSTAT_INO);
    check(ino_stat == 0x101, "stat inode is the cmdline inode");
    check(mode == *(unsigned int *)(fstatbuf + KSTAT_MODE) && ino_fstat == 0x101,
          "fstat agrees on mode and the cmdline inode");
    syscall1(SYS_CLOSE, fd);

    proc_listing_case();

    fixture = contains(cmdline, "rach2.bootprobe=fixture");

    if (contains(cmdline, "rach2.bootprobe=shared")) {
        int kind;

        check(contains(cmdline, "rach2.blocklag="), "raw overlap uses delayed device");
        raw_readv_case();
        for (kind = 0; kind < 5; kind++) {
            raw_shared_case(kind);
        }
        puts(fails ? "BOOTPROBE FAILED" : "BOOTPROBE SHARED OK");
        sys_exit(fails != 0);
    }

    /* A RAM root still has the disk's boot partition and loader gap. */
    if (contains(cmdline, "rach2.bootprobe=ramroot")) {
        rc = syscall3(SYS_STAT, (long)"/dev/bootpart", (long)statbuf, 0);
        check(rc == 0, "a RAM-root boot still has /dev/bootpart");
        check(*(unsigned int *)(statbuf + KSTAT_MODE) == 020600, "and it is a 0600 device");
        rc = syscall3(SYS_STAT, (long)"/dev/grubcore", (long)statbuf, 0);
        check(rc == 0, "a RAM-root boot has /dev/grubcore");
        check(*(unsigned int *)(statbuf + KSTAT_MODE) == 020600, "and it is a 0600 device");

        fd = syscall3(SYS_OPEN, (long)"/dev/grubcore", O_RDWR, 0);
        check(fd >= 0, "root opens /dev/grubcore read-write");
        rc = syscall3(SYS_LSEEK, fd, 0, SEEK_END);
        check(rc == 2047 * SECTOR, "grubcore spans LBA 1 through 2047 and no more");

        rc = syscall3(SYS_LSEEK, fd, 0, SEEK_SET);
        check(rc == 0, "seek to the gap's first sector");
        n = syscall3(SYS_READ, fd, (long)sector, SECTOR);
        check(n == SECTOR, "the gap's first sector reads whole");

        /* A write at or across the end writes nothing, not part. */
        rc = syscall3(SYS_LSEEK, fd, 2047 * SECTOR, SEEK_SET);
        check(rc == 2047 * SECTOR, "seek to the gap's end");
        n = syscall3(SYS_READ, fd, (long)sector, SECTOR);
        check(n == 0, "a read at the end is exactly 0");
        n = syscall3(SYS_WRITE, fd, (long)sector, SECTOR);
        check(n == -ENOSPC, "a write at the end is -ENOSPC");
        rc = syscall3(SYS_LSEEK, fd, 2046 * SECTOR, SEEK_SET);
        check(rc == 2046 * SECTOR, "seek to the last sector");
        n = syscall3(SYS_WRITE, fd, (long)sector, 2 * SECTOR);
        check(n == -ENOSPC, "a write crossing the end writes nothing");

        rc = syscall3(SYS_LSEEK, fd, 1, SEEK_SET);
        check(rc == 1, "an unaligned seek is allowed");
        n = syscall3(SYS_READ, fd, (long)sector, SECTOR);
        check(n == -EINVAL, "an unaligned read is -EINVAL");
        n = syscall3(SYS_WRITE, fd, (long)sector, SECTOR);
        check(n == -EINVAL, "an unaligned write is -EINVAL");
        rc = syscall3(SYS_LSEEK, fd, 0, SEEK_SET);
        check(rc == 0, "seek back to the start");
        n = syscall3(SYS_WRITE, fd, (long)sector, SECTOR - 1);
        check(n == -EINVAL, "a partial-sector count is -EINVAL");

        /* The write handler checks privilege again, past the open. */
        rc = syscall1(SYS_SETUID, 1000);
        check(rc == 0, "the probe drops to an unprivileged uid");
        rc = syscall3(SYS_LSEEK, fd, 0, SEEK_SET);
        check(rc == 0, "the descriptor still seeks after the drop");
        n = syscall3(SYS_READ, fd, (long)sector, SECTOR);
        check(n == SECTOR, "and still reads, which is not the privilege in question");
        n = syscall3(SYS_WRITE, fd, (long)sector, SECTOR);
        check(n == -EPERM, "but a write through it is -EPERM after the drop");
        syscall1(SYS_CLOSE, fd);

        rc = syscall3(SYS_OPEN, (long)"/dev/grubcore", O_RDONLY, 0);
        check(rc == -EACCES, "and an unprivileged open of grubcore is -EACCES");

        if (fails == 0) {
            puts("BOOTPROBE RAMROOT OK");
            sys_exit(0);
        }
        puts("BOOTPROBE FAILED");
        sys_exit(1);
    }

    /* Whether this kernel may write its own disk. */
    if (contains(cmdline, "rach2.bootprobe=auth")) {
        auth_case(contains(cmdline, "rach2.bootprobe=auth-recovery"));
        window_batch_case();
        if (fails == 0) {
            puts(contains(cmdline, "rach2.bootprobe=auth-recovery") ? "BOOTPROBE AUTH RECOVERY OK"
                                                                    : "BOOTPROBE AUTH ORDINARY OK");
            sys_exit(0);
        }
        puts("BOOTPROBE FAILED");
        sys_exit(1);
    }

    if (contains(cmdline, "rach2.bootprobe=wide")) {
        long long pos = 0;
        long long near_at = 5LL * 1024 * 1024 * 1024;
        long long far_at = 258LL * 1024 * 1024 * 1024;
        long long size = 260LL * 1024 * 1024 * 1024;
        int fd2;

        fd = syscall3(SYS_OPEN, (long)"/dev/disk0", O_RDONLY, 0);
        check(fd >= 0, "root opens /dev/disk0 on a 260 GB drive");

        rc = probe_llseek(fd, 0, SEEK_END, &pos);
        check(rc == 0, LLSEEK_NAME " to the end of a 260 GB window succeeds");
        check(pos == size, "and reports the whole drive, not a truncation");

        rc = probe_llseek(fd, near_at, SEEK_SET, &pos);
        check(rc == 0 && pos == near_at, LLSEEK_NAME " to 5 GiB lands where it was asked");
        n = syscall3(SYS_READ, fd, (long)sector, SECTOR);
        check(n == SECTOR, "a sector reads at 5 GiB");
        check(pattern_ok(sector, 5), "and has the bytes written there");

        rc = probe_llseek(fd, far_at, SEEK_SET, &pos);
        check(rc == 0 && pos == far_at, LLSEEK_NAME " to 258 GiB lands where it was asked");
        n = syscall3(SYS_READ, fd, (long)sector, SECTOR);
        check(n == SECTOR, "a sector reads at 258 GiB");
        check(pattern_ok(sector, 258), "and has the bytes written there");

        rc = probe_llseek(fd, 0, SEEK_SET, &pos);
        check(rc == 0 && pos == 0, "seek back to the start");
        {
            long long step = near_at;

            rc = probe_llseek(fd, step, SEEK_CUR, &pos);
            check(rc == 0 && pos == near_at, "SEEK_CUR forward crosses 4 GiB");
            step = far_at - near_at;
            rc = probe_llseek(fd, step, SEEK_CUR, &pos);
            check(rc == 0 && pos == far_at, "SEEK_CUR forward again reaches 258 GiB");
            n = syscall3(SYS_READ, fd, (long)sector, SECTOR);
            check(n == SECTOR && pattern_ok(sector, 258),
                  "and a read there still finds the far marker");
            step = -(far_at + SECTOR);
            rc = probe_llseek(fd, step, SEEK_CUR, &pos);
            check(rc == 0 && pos == 0, "SEEK_CUR backward returns to the start");
            step = -1;
            rc = probe_llseek(fd, step, SEEK_CUR, &pos);
            check(rc == -EINVAL, "a seek before the start is refused, not wrapped");
        }

        rc = probe_llseek(fd, far_at, SEEK_SET, &pos);
        check(rc == 0, "seek to the far marker before duplicating");
        fd2 = syscall1(SYS_DUP, fd);
        check(fd2 >= 0, "the descriptor duplicates");
        rc = probe_llseek(fd2, 0, SEEK_CUR, &pos);
        check(rc == 0 && pos == far_at, "the duplicate shares the wide offset");
        n = syscall3(SYS_READ, fd2, (long)sector, SECTOR);
        check(n == SECTOR && pattern_ok(sector, 258), "and reads the far marker through it");
        rc = probe_llseek(fd, 0, SEEK_CUR, &pos);
        check(rc == 0 && pos == far_at + SECTOR,
              "the original saw the duplicate's read advance it");
        syscall1(SYS_CLOSE, fd2);

#if defined __x86_64__
        check(syscall3(SYS_LSEEK, fd, 0, SEEK_END) == size,
              "lseek, of 64 bits on x86-64, reports the whole drive");
#else
        rc = syscall3(SYS_LSEEK, fd, 0, SEEK_END);
        check(rc == -EOVERFLOW, "the 32-bit lseek reports overflow, not an invalid seek");
#endif
        rc = syscall3(SYS_LSEEK, fd, 0, SEEK_SET);
        check(rc == 0, "and still works for an offset it can name");
        syscall1(SYS_CLOSE, fd);

        if (fails == 0) {
            puts("BOOTPROBE WIDE OK");
            sys_exit(0);
        }
        puts("BOOTPROBE FAILED");
        sys_exit(1);
    }

    if (!fixture) {
        /* A single-partition disk: the device must not exist anywhere. */
        rc = syscall3(SYS_STAT, (long)"/dev/bootpart", (long)statbuf, 0);
        check(rc == -ENOENT, "stat64 /dev/bootpart is -ENOENT");
        rc = syscall3(SYS_OPEN, (long)"/dev/bootpart", O_RDONLY, 0);
        check(rc == -ENOENT, "open /dev/bootpart is -ENOENT");
        if (fails == 0) {
            puts("BOOTPROBE ABSENT OK");
            sys_exit(0);
        }
        puts("BOOTPROBE FAILED");
        sys_exit(1);
    }

    /* The fixture: partition 1 is FAT-typed known bytes. */
    rc = syscall3(SYS_STAT, (long)"/dev/bootpart", (long)statbuf, 0);
    check(rc == 0, "stat64 /dev/bootpart");
    mode = *(unsigned int *)(statbuf + KSTAT_MODE);
    check(mode == 020600, "bootpart is a 0600 character device");

    fd = syscall3(SYS_OPEN, (long)"/dev/bootpart", O_RDWR, 0);
    check(fd >= 0, "root opens /dev/bootpart read-write");
    rc = syscall3(SYS_FSTAT, fd, (long)fstatbuf, 0);
    check(rc == 0 && *(unsigned int *)(fstatbuf + KSTAT_MODE) == 020600,
          "fstat mode matches the path stat");

    /* Known bytes at a seek: sector 5 of the pattern. */
    rc = syscall3(SYS_LSEEK, fd, 5 * SECTOR, SEEK_SET);
    check(rc == 5 * SECTOR, "seek to sector 5");
    n = syscall3(SYS_READ, fd, (long)sector, SECTOR);
    check(n == SECTOR, "read sector 5 whole");
    {
        int j, ok = 1;

        for (j = 0; j < SECTOR; j++) {
            if ((unsigned char)sector[j] != (unsigned char)((5 * 7 + j) & 0xff)) {
                ok = 0;
                break;
            }
        }
        check(ok, "sector 5 has the pattern");
    }

    /* The end: reads at and past it return 0, and a write is -ENOSPC. */
    rc = syscall3(SYS_LSEEK, fd, 0, SEEK_END);
    check(rc == PART_SECTORS * SECTOR, "SEEK_END is the partition size");
    n = syscall3(SYS_READ, fd, (long)sector, SECTOR);
    check(n == 0, "a read at the end returns exactly 0");
    rc = syscall3(SYS_LSEEK, fd, SECTOR, SEEK_END);
    check(rc == (PART_SECTORS + 1) * SECTOR, "a seek past the end is allowed");
    n = syscall3(SYS_READ, fd, (long)sector, SECTOR);
    check(n == 0, "a read past the end returns exactly 0");
    n = syscall3(SYS_WRITE, fd, (long)sector, SECTOR);
    check(n == -ENOSPC, "a write past the end is -ENOSPC");

    /* Alignment: partial sectors are refused in both directions. */
    rc = syscall3(SYS_LSEEK, fd, 0, SEEK_SET);
    check(rc == 0, "seek back to 0");
    n = syscall3(SYS_READ, fd, (long)sector, 100);
    check(n == -EINVAL, "an unaligned read count is -EINVAL");
    rc = syscall3(SYS_LSEEK, fd, 100, SEEK_SET);
    check(rc == 100, "a seek needs no alignment");
    n = syscall3(SYS_READ, fd, (long)sector, SECTOR);
    check(n == -EINVAL, "an unaligned offset is -EINVAL");

    /* A write comes back on a re-read. */
    {
        int j;

        for (j = 0; j < SECTOR; j++) {
            written[j] = (char)(0xA5 ^ (j & 0xff));
        }
    }
    rc = syscall3(SYS_LSEEK, fd, 7 * SECTOR, SEEK_SET);
    check(rc == 7 * SECTOR, "seek to sector 7");
    n = syscall3(SYS_WRITE, fd, (long)written, SECTOR);
    check(n == SECTOR, "write sector 7 whole");
    rc = syscall3(SYS_LSEEK, fd, 7 * SECTOR, SEEK_SET);
    check(rc == 7 * SECTOR, "seek back to sector 7");
    n = syscall3(SYS_READ, fd, (long)sector, SECTOR);
    check(n == SECTOR, "read sector 7 back");
    {
        int j, ok = 1;

        for (j = 0; j < SECTOR; j++) {
            if (sector[j] != written[j]) {
                ok = 0;
                break;
            }
        }
        check(ok, "the written sector reads back");
    }
    syscall1(SYS_CLOSE, fd);

    /* Unprivileged, the device refuses at open with -EACCES. */
    rc = syscall1(SYS_SETUID, 1000);
    check(rc == 0, "drop to uid 1000");
    rc = syscall3(SYS_OPEN, (long)"/dev/bootpart", O_RDONLY, 0);
    check(rc == -EACCES, "an unprivileged open is -EACCES");
    rc = syscall3(SYS_OPEN, (long)"/dev/bootpart", O_RDWR, 0);
    check(rc == -EACCES, "an unprivileged read-write open is -EACCES");

    if (fails == 0) {
        puts("BOOTPROBE OK");
        sys_exit(0);
    }
    puts("BOOTPROBE FAILED");
    sys_exit(1);
}
