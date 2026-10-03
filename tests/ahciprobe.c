/**
 * The AHCI disks driven by their interrupts: a slow disk does not hold up
 * a fast one, and the fallback root's fault shares a port with the queue.
 *
 * Mode `slow`: the suite throttles the disk on port 0 to 256 KiB a second,
 * after a burst of one second's worth, and leaves the one on port 1 at
 * full speed. A child reads 1 MiB from the slow disk's partition, which
 * takes it past the burst and three seconds into the throttle; once it has
 * begun, a second child reads 256 KiB from the fast disk's. The fast reader
 * must be done while the slow one is still reading: the order is the
 * proof, since a polled driver holds the processor for every slow
 * transfer and the fast reader would then end after it. Then the
 * controller's vector in `/proc/interrupts` must have counted the
 * completions. The times are printed and not judged; a bound on them was
 * one machine's.
 *
 * Mode `fault`: the KRFFS root is the disk on port 0, mounted through the
 * page-cache window, whose faults read the disk polled with interrupts
 * off. A child runs `blockprobe`, which writes and reads the root through
 * those faults, while this program reads the disk's second partition raw
 * through the queue without pause. Both must succeed: every fault found
 * the port with a queued command active, drained it, and took its turn.
 *
 * Mode `late`: a request answered after its deadline is the answer; mode
 * `edge`: a request under one period and one over it, the second overdue
 * once; mode `quarantine`: a request past every period, given up on, whose
 * late completion must free its slot; mode `pool`: a pool of two slots
 * held by two slow readers, and a third reader that must wait for one and
 * succeed. Each says what it needs of the machine at its definition.
 *
 * Prints "AHCIPROBE OK" or "AHCIPROBE FAIL: reason". Freestanding; direct
 * system calls (probe.h).
 */

#include "probe.h"

#define O_RDONLY        0
#define O_WRONLY        1
#define O_CREAT         0x40
#define O_TRUNC         0x200
#define CLOCK_MONOTONIC 1

#define SLOW_BYTES (1024 * 1024)
#define FAST_BYTES (256 * 1024)
#define CHUNK      (32 * 1024)
#define WNOHANG    1

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
    syscall3(SYS_WRITE, 1, (long)s, (int)strlen(s));
    syscall3(SYS_WRITE, 1, (long)"\n", 1);
}

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

static void fail(const char *why)
{
    syscall3(SYS_WRITE, 1, (long)"AHCIPROBE FAIL: ", 16);
    puts(why);
    syscall2(SYS_EXIT, 1, 0);
}

/* A counter of /proc/disk by its label, defined with the file readers below. */
static int disk_counter(const char *label);

static int now_ms(void)
{
    int ts[2];

    ts[0] = ts[1] = 0;
    syscall2(SYS_CLOCK_GETTIME, CLOCK_MONOTONIC, (long)ts);
    return ts[0] * 1000 + ts[1] / 1000000;
}

/* Read `bytes` from `node` in chunks; the longest chunk's time in
   milliseconds is left in `*longest`. Returns 0, or -1 on a short read. */
/* Open `node`, waiting for it: a throttled disk is found seconds into the
   boot, after the shell has run the probe. */
static int open_node(const char *node)
{
    int fd = -1, tries;

    for (tries = 0; tries < 40 && fd < 0; tries++) {
        fd = syscall3(SYS_OPEN, (long)node, O_RDONLY, 0);
        if (fd < 0) {
            int ts[2];

            ts[0] = 0;
            ts[1] = 250000000;
            syscall2(SYS_NANOSLEEP, (long)ts, 0);
        }
    }
    return fd;
}

static int read_node(const char *node, int bytes, int *longest)
{
    static char buf[CHUNK];
    int fd = open_node(node);
    int done = 0;

    buf[0] = 0;
    *longest = 0;
    if (fd < 0) {
        return -1;
    }
    while (done < bytes) {
        int t0 = now_ms(), n, dt;

        n = syscall3(SYS_READ, fd, (long)buf, CHUNK);
        if (n != CHUNK) {
            syscall2(SYS_CLOSE, fd, 0);
            return -1;
        }
        dt = now_ms() - t0;
        if (dt > *longest) {
            *longest = dt;
        }
        done += n;
    }
    syscall2(SYS_CLOSE, fd, 0);
    return 0;
}

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

/* The count on the `/proc/interrupts` line whose names include `name`. */
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

static void slow_row(void)
{
    static char text[4096];
    int pipefd[2];
    int slow, fast, status, t0, t_fast, longest, slow_running, reaped, overlaps;
    char byte;

    /* A vector that has counted nothing is not listed, so the controller's
       line is asked for after the reads and not before. */
    pipefd[0] = pipefd[1] = -1;
    if (syscall1(SYS_PIPE, (long)pipefd) != 0) {
        fail("pipe");
    }
    /* The clock starts before the slow reader, since a driver that holds
       the processor through its transfers would hold this process too, and
       a clock started after its word would start after its whole read. */
    overlaps = disk_counter("disk-overlaps ");
    t0 = now_ms();
    slow = syscall1(SYS_FORK, 0);
    if (slow < 0) {
        fail("fork");
    }
    if (slow == 0) {
        int l;

        syscall3(SYS_WRITE, pipefd[1], (long)"s", 1);
        syscall2(SYS_EXIT, read_node("/dev/disk0p1", SLOW_BYTES, &l) == 0 ? 0 : 3, 0);
    }
    if (syscall3(SYS_READ, pipefd[0], (long)&byte, 1) != 1) {
        fail("the slow reader did not start");
    }
    fast = syscall1(SYS_FORK, 0);
    if (fast < 0) {
        fail("fork");
    }
    if (fast == 0) {
        int l, rc = read_node("/dev/disk1p1", FAST_BYTES, &l);

        /* The longest single read, back to the parent through the exit
           status, below the value that means a failed read. */
        syscall2(SYS_EXIT, rc != 0 ? 200 : (l > 199 ? 199 : l), 0);
    }
    status = -1;
    if (syscall3(SYS_WAITPID, fast, (long)&status, 0) != fast) {
        fail("waiting for the fast reader");
    }
    t_fast = now_ms() - t0;
    longest = (status >> 8) & 0xFF;
    if (longest == 200) {
        fail("the fast disk's read");
    }
    /* The order: the slow reader has not ended when the fast one has. The
       one wait that reaps it is the one whose status is read. */
    status = -1;
    reaped = syscall3(SYS_WAITPID, slow, (long)&status, WNOHANG);
    slow_running = reaped == 0;
    puts("AHCIPROBE READ fast reader ms:");
    putnum(t_fast);
    puts(" longest read ms:");
    putnum(longest);
    puts(slow_running ? " slow reader still reading" : " slow reader already done");
    puts(" overlaps:");
    putnum(disk_counter("disk-overlaps ") - overlaps);
    puts("");
    if (!slow_running) {
        if (reaped != slow || ((status >> 8) & 0xFF) != 0) {
            fail("the slow disk's read");
        }
        fail("the fast reader ended after the slow disk's read, so it waited behind it");
    }
    /* And the requests overlapped: the fast disk's completed while the
       slow disk's were in flight, which the block layer counts. An order
       alone would pass a driver that serialized the two and let the fast
       reader finish first by its fewer reads. */
    if (disk_counter("disk-overlaps ") - overlaps < 1) {
        fail("no request of the fast disk completed while the slow disk's was in flight");
    }
    status = -1;
    if (syscall3(SYS_WAITPID, slow, (long)&status, 0) != slow || ((status >> 8) & 0xFF) != 0) {
        fail("the slow disk's read");
    }
    if (slurp("/proc/interrupts", text, (int)sizeof(text)) <= 0) {
        fail("/proc/interrupts");
    }
    if (count_for_named("ahci", text) < 16) {
        fail("the controller's vector did not count the completions");
    }
    puts("AHCIPROBE OK");
    syscall2(SYS_EXIT, 0, 0);
}

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

void probe_main(unsigned int *sp);

/* `ahciprobe late`: the throttled disk's ext2 partition mounted for
   writing, under a deadline period shorter than one of its requests, which
   the row sets with `rach2.blockdeadline=1` against a disk of 512 KiB a
   second. ext2 keeps a write's blocks dirty and writes them back in runs
   at the end of the call, a run being at most a request of 1 MiB; the
   file is longer than one, so its first run is a request of 1 MiB, which
   takes two seconds: it passes its deadline and is answered after it. The
   answer must be taken: the write succeeds, the file reads back whole, and
   the mount is still writable. FAT32 would not do: it writes each block
   through as it is written, and a 4 KiB request is under the deadline.
   The file is written in one call, so it is one batch of ext2's and its
   blocks go back in runs; a call per block would end a batch per block
   and write each block, its bitmap and its inode in requests of 4 KiB,
   all under the deadline. */
#define LATE_BYTES (1280 * 1024)

static void late_row(void)
{
    static unsigned char whole[LATE_BYTES];
    static unsigned char block[4096];
    int fd, i, j, start;

    if (syscall2(SYS_MKDIR, (long)"/late", 0755) != 0) {
        fail("mkdir /late");
    }
    if (syscall5(SYS_MOUNT, (long)"/dev/disk0p1", (long)"/late", (long)"ext2", 0, 0) != 0) {
        fail("the mount of the throttled disk");
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
        fail("the sync after the late answers");
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
        fail("the mount is not writable after the late answers");
    }
    syscall1(SYS_CLOSE, fd);
    if (syscall1(SYS_SYNC, 0) != 0) {
        fail("the second sync");
    }
    if (syscall2(SYS_UMOUNT2, (long)"/late", 0) != 0) {
        fail("umount2 /late");
    }
    puts("AHCIPROBE OK");
    syscall2(SYS_EXIT, 0, 0);
}

/* One raw read of `bytes` from `node`, at most a window's request; the
   syscall's answer, so a request the deadline gave up on shows as a short
   or failed read. `*ms` is the call's time, and -1 when the node did not
   open, so that no row reads a time from a request never made. */
static char read_once_buf[32 * 1024];

static int read_once(const char *node, int bytes, int *ms)
{
    int fd = open_node(node);
    int n, t0;

    *ms = -1;
    if (fd < 0) {
        return -1;
    }
    t0 = now_ms();
    n = syscall3(SYS_READ, fd, (long)read_once_buf, bytes);
    *ms = now_ms() - t0;
    syscall2(SYS_CLOSE, fd, 0);
    return n;
}

static void sleep_ms(int ms)
{
    int ts[2];

    ts[0] = ms / 1000;
    ts[1] = (ms % 1000) * 1000000;
    syscall2(SYS_NANOSLEEP, (long)ts, 0);
}

/* `ahciprobe edge`: the deadline's boundary, one request of 4 KiB on the
   boot disk behind the lag device, which answers every request an exact
   number of ticks after it was made. The suite boots it twice under a
   period of a hundred ticks, `rach2.lagdeadline=100`: with
   `rach2.blocklag=75` the request is answered inside its period and must
   not be overdue, and with `rach2.blocklag=125` it is overdue once and
   taken; the suite reads
   the kernel's record of which. The request must answer its bytes either
   way. An emulator's throttle would not do here: it pays for a burst
   unevenly across the requests after it. */
static void edge_row(void)
{
    int ms, n;

    n = read_once("/dev/disk0p1", 4 * 1024, &ms);
    if (n != 4 * 1024) {
        fail("the read at the boundary");
    }
    puts("AHCIPROBE EDGE ms:");
    putnum(ms);
    puts("AHCIPROBE OK");
    syscall2(SYS_EXIT, 0, 0);
}

/* `ahciprobe quarantine`: one slot in the pool, `rach2.blockpool=1`, and
   the lag device answering 500 ticks late under four periods of a hundred:
   the request is given up on with EIO at the fourth deadline and its slot
   quarantined. The completion arrives a second after and must free the
   slot: a request made once it has is the proof, since with one slot in
   the pool a slot not freed leaves nothing to take. The lag answers that
   request 500 ticks late as well, so it too is given up on; what tells a
   slot taken from a slot refused is the time: a request refused for want
   of a slot answers at once, and one taken waits its four periods. */
/* A counter of /proc/disk by its label, or -1 when the file cannot be read. */
static int disk_counter(const char *label)
{
    static char text[2048];
    int fd = syscall3(SYS_OPEN, (long)"/proc/disk", O_RDONLY, 0);
    int n, i, j;

    if (fd < 0) {
        return -1;
    }
    n = syscall3(SYS_READ, fd, (long)text, sizeof(text) - 1);
    syscall1(SYS_CLOSE, fd);
    if (n <= 0) {
        return -1;
    }
    text[n] = 0;
    for (i = 0; text[i]; i++) {
        for (j = 0; label[j] && text[i + j] == label[j]; j++) {
        }
        if (!label[j]) {
            int v = 0;

            i += j;
            while (text[i] >= '0' && text[i] <= '9') {
                v = v * 10 + (text[i] - '0');
                i++;
            }
            return v;
        }
    }
    return -1;
}

static void quarantine_row(void)
{
    int ms, n, quarantined;

    n = read_once("/dev/disk0p1", 4 * 1024, &ms);
    if (ms < 0) {
        fail("the partition did not open");
    }
    if (n == 4 * 1024) {
        fail("a request past every period was answered as if in time");
    }
    puts("AHCIPROBE GAVEUP ms:");
    putnum(ms);
    sleep_ms(6000); /* the late completion lands in the meantime */
    /* The request after the late completion is taken, which is what shows
       the slot was freed: with one slot in the pool a slot not freed leaves
       nothing to take and the request is refused at once. Taken, it waits
       its periods under the same lag and is quarantined in its turn, which
       the kernel counts; a refusal counts nothing. */
    quarantined = disk_counter("disk-quarantines ");
    n = read_once("/dev/disk0p1", 4 * 1024, &ms);
    if (ms < 0) {
        fail("the partition did not open again");
    }
    if (n != 4 * 1024 && disk_counter("disk-quarantines ") <= quarantined) {
        fail("the request after the late completion was refused at once: the slot was not freed");
    }
    puts("AHCIPROBE AFTER ms:");
    putnum(ms);
    puts("AHCIPROBE OK");
    syscall2(SYS_EXIT, 0, 0);
}

/* `ahciprobe pool`: two slots in the pool, `rach2.blockpool=2`, two disks
   throttled to 16 KiB a second and one at full speed. A reader on each
   slow disk holds a slot for a 64 KiB read; a reader of the fast disk,
   started once both are in flight, finds the pool full, waits for a slot,
   and must succeed. The kernel's record of the wait is the suite's. */
static void pool_row(void)
{
    int pipefd[2], readers[2], i, ms, n, status;
    char byte;

    pipefd[0] = pipefd[1] = -1;
    if (syscall1(SYS_PIPE, (long)pipefd) != 0) {
        fail("pipe");
    }
    for (i = 0; i < 2; i++) {
        readers[i] = syscall1(SYS_FORK, 0);
        if (readers[i] < 0) {
            fail("fork");
        }
        if (readers[i] == 0) {
            int l;

            syscall3(SYS_WRITE, pipefd[1], (long)"s", 1);
            syscall2(
                SYS_EXIT,
                read_node(i == 0 ? "/dev/disk0p1" : "/dev/disk1p1", 64 * 1024, &l) == 0 ? 0 : 3, 0);
        }
    }
    syscall2(SYS_CLOSE, pipefd[1], 0); /* so a reader that dies first ends the wait */
    for (i = 0; i < 2; i++) {
        if (syscall3(SYS_READ, pipefd[0], (long)&byte, 1) != 1) {
            fail("a slow reader did not start");
        }
    }
    sleep_ms(300); /* both slow reads are in flight, each holding a slot */
    n = read_once("/dev/disk2p1", 4 * 1024, &ms);
    if (n != 4 * 1024) {
        fail("the reader of the fast disk was refused while the pool was full");
    }
    puts("AHCIPROBE WAITED ms:");
    putnum(ms);
    for (i = 0; i < 2; i++) {
        status = 0;
        if (syscall3(SYS_WAITPID, readers[i], (long)&status, 0) != readers[i] || status != 0) {
            fail("a slow reader failed or was killed");
        }
    }
    puts("AHCIPROBE OK");
    syscall2(SYS_EXIT, 0, 0);
}

__asm__(".global _start\n"
        "_start:\n"
        "   mov %esp, %eax\n"
        "   push %eax\n"
        "   call probe_main\n");

void probe_main(unsigned int *sp)
{
    int argc = (int)sp[0];
    char **argv = (char **)(sp + 1);

    if (argc >= 2 && argv[1][0] == 'f') {
        fault_row();
    }
    if (argc >= 2 && argv[1][0] == 'l') {
        late_row();
    }
    if (argc >= 2 && argv[1][0] == 'e') {
        edge_row();
    }
    if (argc >= 2 && argv[1][0] == 'q') {
        quarantine_row();
    }
    if (argc >= 2 && argv[1][0] == 'p') {
        pool_row();
    }
    slow_row();
}
