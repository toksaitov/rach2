/**
 * The AHCI disks driven by their interrupts: a slow disk does not hold up
 * a fast one, and the fallback root's fault shares a port with the queue.
 *
 * Mode `slow`: the suite throttles the disk on port 0 to 256 KiB a second,
 * after a burst of one second's worth, and leaves the one on port 1 at
 * full speed. A child reads 1 MiB from the slow disk's partition, which
 * takes it past the burst and three seconds into the throttle; once it has
 * begun, a second child reads 256 KiB from the fast disk's. The fast reader
 * must finish within a fraction of the slow one's time, and no single read
 * of its may take
 * longer than the slow disk's transfer of one request, which is the delay
 * a polled driver would put in front of it. Then the controller's vector
 * in `/proc/interrupts` must have counted the completions. A polled driver
 * holds the processor for every slow transfer, and the fast reader waits
 * behind each of them.
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
 * Prints "AHCIPROBE OK" or "AHCIPROBE FAIL: reason". Freestanding; int 0x80.
 */

typedef unsigned int size_t;

#define SYS_EXIT          1
#define SYS_FORK          2
#define SYS_READ          3
#define SYS_WRITE         4
#define SYS_OPEN          5
#define SYS_CLOSE         6
#define SYS_WAITPID       7
#define SYS_EXECVE        11
#define SYS_PIPE          42
#define SYS_SYNC          36
#define SYS_MKDIR         39
#define SYS_MOUNT         21
#define SYS_UMOUNT2       52
#define SYS_CLOCK_GETTIME 265
#define SYS_NANOSLEEP     162

#define O_RDONLY        0
#define O_WRONLY        1
#define O_CREAT         0x40
#define O_TRUNC         0x200
#define CLOCK_MONOTONIC 1

#define SLOW_BYTES  (1024 * 1024)
#define FAST_BYTES  (256 * 1024)
#define CHUNK       (32 * 1024)
#define SLOW_MS     3000 /* 1 MiB less the burst, at 256 KiB a second */
#define FAST_MS     200  /* the fast reader's whole read, generous */
#define FAST_ONE_MS 60   /* one of its reads; a slow transfer is 128 ms */

static int sys1(int n, int a)
{
    int r;
    __asm__ __volatile__("int $0x80" : "=a"(r) : "0"(n), "b"(a) : "memory");
    return r;
}

static int sys2(int n, int a, int b)
{
    int r;
    __asm__ __volatile__("int $0x80" : "=a"(r) : "0"(n), "b"(a), "c"(b) : "memory");
    return r;
}

static int sys3(int n, int a, int b, int c)
{
    int r;
    __asm__ __volatile__("int $0x80" : "=a"(r) : "0"(n), "b"(a), "c"(b), "d"(c) : "memory");
    return r;
}

/** A system call with five arguments. */
static int sys5(int n, int a, int b, int c, int d, int e)
{
    int r;
    __asm__ __volatile__("int $0x80"
                         : "=a"(r)
                         : "0"(n), "b"(a), "c"(b), "d"(c), "S"(d), "D"(e)
                         : "memory");
    return r;
}

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
    sys3(SYS_WRITE, 1, (int)(size_t)s, (int)strlen(s));
    sys3(SYS_WRITE, 1, (int)(size_t)"\n", 1);
}

static void putnum(int v)
{
    char buf[12];
    int n = 0, i;

    if (v < 0) {
        sys3(SYS_WRITE, 1, (int)(size_t)"-", 1);
        v = -v;
    }
    do {
        buf[n++] = (char)('0' + v % 10);
        v /= 10;
    } while (v > 0);
    for (i = n - 1; i >= 0; i--) {
        sys3(SYS_WRITE, 1, (int)(size_t)&buf[i], 1);
    }
}

static void fail(const char *why)
{
    sys3(SYS_WRITE, 1, (int)(size_t)"AHCIPROBE FAIL: ", 16);
    puts(why);
    sys2(SYS_EXIT, 1, 0);
}

static int now_ms(void)
{
    int ts[2];

    ts[0] = ts[1] = 0;
    sys2(SYS_CLOCK_GETTIME, CLOCK_MONOTONIC, (int)(size_t)ts);
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
        fd = sys3(SYS_OPEN, (int)(size_t)node, O_RDONLY, 0);
        if (fd < 0) {
            int ts[2];

            ts[0] = 0;
            ts[1] = 250000000;
            sys2(SYS_NANOSLEEP, (int)(size_t)ts, 0);
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

        n = sys3(SYS_READ, fd, (int)(size_t)buf, CHUNK);
        if (n != CHUNK) {
            sys2(SYS_CLOSE, fd, 0);
            return -1;
        }
        dt = now_ms() - t0;
        if (dt > *longest) {
            *longest = dt;
        }
        done += n;
    }
    sys2(SYS_CLOSE, fd, 0);
    return 0;
}

static int slurp(const char *path, char *buf, int cap)
{
    int fd = sys3(SYS_OPEN, (int)(size_t)path, O_RDONLY, 0);
    int n, total = 0;

    if (fd < 0) {
        return -1;
    }
    while (total < cap - 1) {
        n = sys3(SYS_READ, fd, (int)(size_t)(buf + total), cap - 1 - total);
        if (n <= 0) {
            break;
        }
        total += n;
    }
    sys2(SYS_CLOSE, fd, 0);
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
    int slow, fast, status, t0, t_fast, longest;
    char byte;

    /* A vector that has counted nothing is not listed, so the controller's
       line is asked for after the reads and not before. */
    pipefd[0] = pipefd[1] = -1;
    if (sys1(SYS_PIPE, (int)(size_t)pipefd) != 0) {
        fail("pipe");
    }
    /* The clock starts before the slow reader, since a driver that holds
       the processor through its transfers would hold this process too, and
       a clock started after its word would start after its whole read. */
    t0 = now_ms();
    slow = sys1(SYS_FORK, 0);
    if (slow < 0) {
        fail("fork");
    }
    if (slow == 0) {
        int l;

        sys3(SYS_WRITE, pipefd[1], (int)(size_t)"s", 1);
        sys2(SYS_EXIT, read_node("/dev/disk0p1", SLOW_BYTES, &l) == 0 ? 0 : 3, 0);
    }
    if (sys3(SYS_READ, pipefd[0], (int)(size_t)&byte, 1) != 1) {
        fail("the slow reader did not start");
    }
    fast = sys1(SYS_FORK, 0);
    if (fast < 0) {
        fail("fork");
    }
    if (fast == 0) {
        int l, rc = read_node("/dev/disk1p1", FAST_BYTES, &l);

        /* The longest single read, back to the parent through the exit
           status, below the value that means a failed read. */
        sys2(SYS_EXIT, rc != 0 ? 200 : (l > 199 ? 199 : l), 0);
    }
    status = -1;
    if (sys3(SYS_WAITPID, fast, (int)(size_t)&status, 0) != fast) {
        fail("waiting for the fast reader");
    }
    t_fast = now_ms() - t0;
    longest = (status >> 8) & 0xFF;
    if (longest == 200) {
        fail("the fast disk's read");
    }
    puts("AHCIPROBE READ fast reader ms:");
    putnum(t_fast);
    puts(" longest read ms:");
    putnum(longest);
    puts("");
    status = -1;
    if (sys3(SYS_WAITPID, slow, (int)(size_t)&status, 0) != slow || ((status >> 8) & 0xFF) != 0) {
        fail("the slow disk's read");
    }
    if (now_ms() - t0 < SLOW_MS / 2) {
        fail("the slow disk was not slow; the throttle is not in force");
    }
    if (t_fast > FAST_MS) {
        fail("the fast reader waited behind the slow disk");
    }
    if (longest > FAST_ONE_MS) {
        fail("one read of the fast disk waited a slow transfer's time");
    }
    if (slurp("/proc/interrupts", text, (int)sizeof(text)) <= 0) {
        fail("/proc/interrupts");
    }
    if (count_for_named("ahci", text) < 16) {
        fail("the controller's vector did not count the completions");
    }
    puts("AHCIPROBE OK");
    sys2(SYS_EXIT, 0, 0);
}

static void fault_row(void)
{
    static char buf[CHUNK];
    static const char *const argv[] = {"/blockprobe", 0};
    static const char *const envp[] = {0};
    int child, status = -1, fd, reads = 0;

    buf[0] = 0;
    child = sys1(SYS_FORK, 0);
    if (child < 0) {
        fail("fork");
    }
    if (child == 0) {
        sys3(SYS_EXECVE, (int)(size_t)"/blockprobe", (int)(size_t)argv, (int)(size_t)envp);
        sys2(SYS_EXIT, 4, 0);
    }
    fd = sys3(SYS_OPEN, (int)(size_t)"/dev/disk0p2", O_RDONLY, 0);
    if (fd < 0) {
        fail("/dev/disk0p2");
    }
    while (sys3(SYS_WAITPID, child, (int)(size_t)&status, 1) == 0) {
        int n = sys3(SYS_READ, fd, (int)(size_t)buf, CHUNK);

        if (n < 0) {
            fail("a raw read beside the faults");
        }
        if (n == 0) {
            sys2(SYS_CLOSE, fd, 0);
            fd = sys3(SYS_OPEN, (int)(size_t)"/dev/disk0p2", O_RDONLY, 0);
            if (fd < 0) {
                fail("/dev/disk0p2 again");
            }
        }
        reads++;
    }
    sys2(SYS_CLOSE, fd, 0);
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
    sys2(SYS_EXIT, 0, 0);
}

void probe_main(unsigned int *sp);

/* `ahciprobe late`: the throttled disk's ext2 partition mounted for
   writing, under a deadline period shorter than one of its requests, which
   the row sets with `rach2.blockdeadline=1` against a disk of 16 KiB a
   second. ext2 keeps a write's blocks dirty and writes them back in runs of
   32 KiB at the end of the call, and such a request takes two seconds:
   it passes its deadline and is answered after it. The answer must be
   taken: the write succeeds, the file reads back whole, and the mount is
   still writable. FAT32 would not do: it writes each block through as it
   is written, and a 4 KiB request is under the deadline. The file is
   written in one call, so it is one batch of ext2's and its blocks go back
   in runs; a call per block would end a batch per block and write each
   block, its bitmap and its inode in requests of 4 KiB, all under the
   deadline. */
#define LATE_BYTES (128 * 1024)

static void late_row(void)
{
    static unsigned char whole[LATE_BYTES];
    static unsigned char block[4096];
    int fd, i, j, start;

    if (sys2(SYS_MKDIR, (int)(size_t)"/late", 0755) != 0) {
        fail("mkdir /late");
    }
    if (sys5(SYS_MOUNT, (int)(size_t)"/dev/disk0p1", (int)(size_t)"/late", (int)(size_t)"ext2", 0,
             0) != 0) {
        fail("the mount of the throttled disk");
    }
    fd = sys3(SYS_OPEN, (int)(size_t)"/late/late.bin", O_WRONLY | O_CREAT | O_TRUNC, 0644);
    if (fd < 0) {
        fail("create late.bin");
    }
    for (i = 0; i < LATE_BYTES; i++) {
        whole[i] = (unsigned char)((i / (int)sizeof(block)) * 7 + i % (int)sizeof(block));
    }
    start = now_ms();
    if (sys3(SYS_WRITE, fd, (int)(size_t)whole, LATE_BYTES) != LATE_BYTES) {
        fail("the write of late.bin");
    }
    sys1(SYS_CLOSE, fd);
    puts("AHCIPROBE WRITE ms:");
    putnum(now_ms() - start);
    if (sys1(SYS_SYNC, 0) != 0) {
        fail("the sync after the late answers");
    }
    fd = sys3(SYS_OPEN, (int)(size_t)"/late/late.bin", O_RDONLY, 0);
    if (fd < 0) {
        fail("open late.bin to read it back");
    }
    for (i = 0; i < LATE_BYTES / (int)sizeof(block); i++) {
        if (sys3(SYS_READ, fd, (int)(size_t)block, (int)sizeof(block)) != (int)sizeof(block)) {
            fail("a read of late.bin");
        }
        for (j = 0; j < (int)sizeof(block); j++) {
            if (block[j] != (unsigned char)(i * 7 + j)) {
                fail("the bytes read back are not what was written");
            }
        }
    }
    sys1(SYS_CLOSE, fd);
    fd = sys3(SYS_OPEN, (int)(size_t)"/late/again.bin", O_WRONLY | O_CREAT, 0644);
    if (fd < 0) {
        fail("the mount is not writable after the late answers");
    }
    sys1(SYS_CLOSE, fd);
    if (sys1(SYS_SYNC, 0) != 0) {
        fail("the second sync");
    }
    if (sys2(SYS_UMOUNT2, (int)(size_t)"/late", 0) != 0) {
        fail("umount2 /late");
    }
    puts("AHCIPROBE OK");
    sys2(SYS_EXIT, 0, 0);
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
    n = sys3(SYS_READ, fd, (int)(size_t)read_once_buf, bytes);
    *ms = now_ms() - t0;
    sys2(SYS_CLOSE, fd, 0);
    return n;
}

static void sleep_ms(int ms)
{
    int ts[2];

    ts[0] = ms / 1000;
    ts[1] = (ms % 1000) * 1000000;
    sys2(SYS_NANOSLEEP, (int)(size_t)ts, 0);
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
    sys2(SYS_EXIT, 0, 0);
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
static void quarantine_row(void)
{
    int ms, n;

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
    n = read_once("/dev/disk0p1", 4 * 1024, &ms);
    if (ms < 0) {
        fail("the partition did not open again");
    }
    if (n != 4 * 1024 && ms < 3000) {
        fail("the request after the late completion was refused at once: the slot was not freed");
    }
    puts("AHCIPROBE AFTER ms:");
    putnum(ms);
    puts("AHCIPROBE OK");
    sys2(SYS_EXIT, 0, 0);
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
    if (sys1(SYS_PIPE, (int)(size_t)pipefd) != 0) {
        fail("pipe");
    }
    for (i = 0; i < 2; i++) {
        readers[i] = sys1(SYS_FORK, 0);
        if (readers[i] < 0) {
            fail("fork");
        }
        if (readers[i] == 0) {
            int l;

            sys3(SYS_WRITE, pipefd[1], (int)(size_t)"s", 1);
            sys2(SYS_EXIT,
                 read_node(i == 0 ? "/dev/disk0p1" : "/dev/disk1p1", 64 * 1024, &l) == 0 ? 0 : 3,
                 0);
        }
    }
    sys2(SYS_CLOSE, pipefd[1], 0); /* so a reader that dies first ends the wait */
    for (i = 0; i < 2; i++) {
        if (sys3(SYS_READ, pipefd[0], (int)(size_t)&byte, 1) != 1) {
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
        if (sys3(SYS_WAITPID, readers[i], (int)(size_t)&status, 0) != readers[i] || status != 0) {
            fail("a slow reader failed or was killed");
        }
    }
    puts("AHCIPROBE OK");
    sys2(SYS_EXIT, 0, 0);
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
