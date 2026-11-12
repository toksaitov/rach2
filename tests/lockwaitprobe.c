/**
 * lockwaitprobe: one file's wait for the disk, and ext2 under many writers.
 *
 * `lockwaitprobe lag`, under `rach2.blocklag=N` with the root disk's empty
 * ext2 volume on /dev/disk0p2: the volume is mounted on /lw, a file `slow`
 * is written a block at a time between the blocks of a file `fill`, so that
 * no two of its blocks are next to each other on the disk, and a small file
 * `near` beside them; the volume is unmounted and mounted again, which
 * leaves nothing of it in the caches. A child then reads `near`, rewrites a
 * block of it, and makes and removes a file `t` in the same directory, over
 * and over, counting each round in a page it shares with the parent, while
 * the parent reads `slow` whole in one read(2), every block of which the
 * lag device answers late. The read must have waited for at least 32 of the
 * lag device's answers (`lag-completions` in /proc/disk), and the child
 * must have finished at least one round for each of them: a slow request
 * on one file leaves the users of another file of the same mount running.
 * The kernel counts both; the clock is not read. /proc/fs/lockwait is
 * printed before the counts are judged, and must have its `mount` line;
 * each row first turns on kernel.lock_stat, without which the kernel times
 * no hold for it.
 *
 * `lockwaitprobe stress`, with the same volume and no lag: a child takes the
 * machine's free memory and the buffer cache's, but for a sixteenth of the
 * whole, so that the caches must give frames back while four workers make,
 * rename, unlink, truncate, read, write and fsync files, and make, rename and
 * remove directories, under the two directories /ls/a and /ls/b they share,
 * each choosing its calls from a seed of its own. A call may find a name
 * gone or taken by another worker; any other error fails. Then every file
 * left reads whole, the mount is still writable, and the volume unmounts,
 * after which `e2fsck -fn` judges it.
 *
 * Prints "LOCKWAITPROBE OK" and exits 0, or "LOCKWAITPROBE FAIL: " and the
 * reason and exits 1.
 */

#include "probe.h"

#define O_RDONLY 0
#define O_WRONLY 1
#define O_RDWR   2
#define O_CREAT  0100
#define O_EXCL   0200
#define O_TRUNC  01000

#define ENOENT    2
#define EEXIST    17
#define ENOTEMPTY 39

#define PROT_RW       3
#define MAP_SHARED    0x01
#define MAP_PRIVATE   0x02
#define MAP_ANONYMOUS 0x20

#define SIGKILL 9

/* The slow file: 64 blocks of 4 KiB, each between two of `fill`'s. */
#define SLOW_BLOCKS 64u
#define BLOCK       4096u
#define SLOW_BYTES  (SLOW_BLOCKS * BLOCK)
#define NEAR_BYTES  (4u * BLOCK)

/* The waits the slow read must cross: half its blocks, since the indirect
 * block and a run the cache reads ahead may answer for more than one. */
#define SLOW_WAITS_MIN 32

/* The stress: workers, calls each, names of files and of directories in
 * each shared directory, and the largest file. */
#define WORKERS    4
#define CALLS      400
#define FILE_NAMES 12
#define DIR_NAMES  4
#define FILE_MOST  (64u * 1024u)

static unsigned char buf[SLOW_BYTES];

/** End the process with `status`; it does not return. */
static void sys_exit(int status)
{
    syscall1(SYS_EXIT, status);
    __builtin_unreachable();
}

/* The line being made for standard output, written whole at its newline so
 * that the kernel's own lines on the console cannot fall inside it. */
static char line[512];
static unsigned line_len;

/** Add `s` to the line, writing the line out at each newline and when it
 * is full. */
static void out(const char *s)
{
    for (; *s; s++) {
        line[line_len++] = *s;
        if (*s == '\n' || line_len == sizeof(line)) {
            syscall3(SYS_WRITE, 1, (long)line, (long)line_len);
            line_len = 0;
        }
    }
}

/** Write `n` in decimal to standard output. */
static void out_num(long n)
{
    char digits[24];
    int i = sizeof(digits);
    unsigned long v = n < 0 ? (unsigned long)-n : (unsigned long)n;

    digits[--i] = 0;
    do {
        digits[--i] = (char)('0' + v % 10u);
        v /= 10u;
    } while (v);
    if (n < 0) {
        digits[--i] = '-';
    }
    out(&digits[i]);
}

/** Print "LOCKWAITPROBE FAIL: ", `why` and, when nonzero, `rc`, and exit 1. */
static void fail_rc(const char *why, long rc)
{
    out("LOCKWAITPROBE FAIL: ");
    out(why);
    if (rc != 0) {
        out(": ");
        out_num(rc);
    }
    out("\n");
    sys_exit(1);
}

/** Print "LOCKWAITPROBE FAIL: " and `why`, and exit 1. */
static void fail(const char *why)
{
    fail_rc(why, 0);
}

/** The number after `label` at the start of a line of the file `path`, or
 * -1 when it is not there. */
static long counter(const char *path, const char *label)
{
    static char text[4096];
    int fd = syscall3(SYS_OPEN, (long)path, O_RDONLY, 0);
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
    for (i = 0; i < n; i++) {
        if (i > 0 && text[i - 1] != '\n') {
            continue;
        }
        for (j = 0; label[j] && text[i + j] == label[j]; j++) {
        }
        if (!label[j]) {
            long v = 0;

            i += j;
            while (text[i] == ' ') {
                i++;
            }
            while (text[i] >= '0' && text[i] <= '9') {
                v = v * 10 + (text[i] - '0');
                i++;
            }
            return v;
        }
    }
    return -1;
}

/** The raw wait status of the child `pid`; fail when the wait fails. */
static int wait_status(int pid)
{
    int status = 0;

    if (syscall3(SYS_WAITPID, pid, (long)&status, 0) != pid) {
        fail("waitpid");
    }
    return status;
}

/** Fill `len` bytes of `buf` with the pattern of `seed` and block `block`. */
static void pattern(unsigned seed, unsigned block, unsigned len)
{
    unsigned i;

    for (i = 0; i < len; i++) {
        buf[i] = (unsigned char)(seed * 31u + block * 7u + i);
    }
}

/** Write `len` bytes of `buf` to `fd`, failing with `what` on a short write. */
static void write_all(int fd, const unsigned char *from, unsigned len, const char *what)
{
    long rc = syscall3(SYS_WRITE, fd, (long)from, (long)len);

    if (rc != (long)len) {
        fail_rc(what, rc);
    }
}

/** Turn on kernel.lock_stat, so that the sleeping locks time their holds
 * for /proc/fs/lockwait. */
static void lock_stat_on(void)
{
    int fd = syscall3(SYS_OPEN, (long)"/proc/sys/kernel/lock_stat", O_WRONLY, 0);

    if (fd < 0) {
        fail_rc("opening /proc/sys/kernel/lock_stat", fd);
    }
    write_all(fd, (const unsigned char *)"1\n", 2, "the write of kernel.lock_stat");
    syscall1(SYS_CLOSE, fd);
}

/** Mount the root disk's second partition, ext2, on `dir`. */
static void mount_volume(const char *dir)
{
    long rc = syscall5(SYS_MOUNT, (long)"/dev/disk0p2", (long)dir, (long)"ext2", 0, 0);

    if (rc != 0) {
        fail_rc("the mount of /dev/disk0p2", rc);
    }
}

/** Unmount `dir`. */
static void unmount_volume(const char *dir)
{
    long rc = syscall2(SYS_UMOUNT2, (long)dir, 0);

    if (rc != 0) {
        fail_rc("the unmount of the volume", rc);
    }
}

/** Print each line of /proc/fs/lockwait after "LOCKWAITPROBE lockwait: ",
 * and fail unless it opens and has the line of the mounts' lock. */
static void show_lockwait(void)
{
    static char text[2048];
    int fd = syscall3(SYS_OPEN, (long)"/proc/fs/lockwait", O_RDONLY, 0);
    int n, i, start = 0;

    if (fd < 0) {
        fail_rc("opening /proc/fs/lockwait", fd);
    }
    n = syscall3(SYS_READ, fd, (long)text, sizeof(text) - 1);
    syscall1(SYS_CLOSE, fd);
    if (n <= 0) {
        fail("reading /proc/fs/lockwait");
    }
    text[n] = 0;
    for (i = 0; i < n; i++) {
        if (text[i] == '\n') {
            text[i] = 0;
            out("LOCKWAITPROBE lockwait: ");
            out(&text[start]);
            out("\n");
            text[i] = '\n';
            start = i + 1;
        }
    }
    if (counter("/proc/fs/lockwait", "mount ") < 0) {
        fail("/proc/fs/lockwait has no line for the mounts' lock");
    }
}

/* The page the lag row's parent and child share. */
struct shared {
    volatile long rounds; /* the child's rounds finished */
    volatile long ready;  /* nonzero once the child's first round is done */
    volatile long stop;   /* set by the parent when its read has returned */
    volatile long error;  /* the child's failed call's result, or 0 */
    volatile long step;   /* which call of the round failed */
};

/**
 * One round of the lag row's child on `near`, open for reading and writing:
 * a block read, a block rewritten in place, and the file `t` made, written
 * and removed beside it. 0, or the failed call's result with `*step` set.
 */
static long near_round(int fd, long *step)
{
    static unsigned char block[BLOCK];
    long rc;
    int t;

    *step = 1;
    if ((rc = syscall3(SYS_LSEEK, fd, 0, 0)) != 0 ||
        (rc = syscall3(SYS_READ, fd, (long)block, BLOCK)) != (long)BLOCK) {
        return rc < 0 ? rc : -1000;
    }
    *step = 2;
    if ((rc = syscall3(SYS_LSEEK, fd, BLOCK, 0)) != (long)BLOCK ||
        (rc = syscall3(SYS_WRITE, fd, (long)block, BLOCK)) != (long)BLOCK) {
        return rc < 0 ? rc : -1000;
    }
    *step = 3;
    t = syscall3(SYS_OPEN, (long)"/lw/t", O_WRONLY | O_CREAT | O_EXCL, 0644);
    if (t < 0) {
        return t;
    }
    rc = syscall3(SYS_WRITE, t, (long)block, 64);
    syscall1(SYS_CLOSE, t);
    if (rc != 64) {
        return rc < 0 ? rc : -1000;
    }
    *step = 4;
    rc = syscall1(SYS_UNLINK, (long)"/lw/t");
    return rc;
}

/** The lag row's child: rounds on `near` until the parent says stop. */
static void near_child(struct shared *sh)
{
    int fd = syscall3(SYS_OPEN, (long)"/lw/near", O_RDWR, 0);
    long rc, step = 0;

    if (fd < 0) {
        sh->error = fd;
        sh->ready = 1;
        sys_exit(1);
    }
    /* The first round reads what the others need, the directory's block and
     * the bitmaps among it, before the parent starts. */
    while (!sh->stop) {
        rc = near_round(fd, &step);
        if (rc != 0) {
            sh->error = rc;
            sh->step = step;
            sh->ready = 1;
            sys_exit(1);
        }
        sh->rounds++;
        sh->ready = 1;
    }
    syscall1(SYS_CLOSE, fd);
    sys_exit(0);
}

/** Make the lag row's files on the volume at /lw: `slow` and `fill` a block
 * at a time, one after the other, so that the allocator puts `fill`'s
 * blocks between `slow`'s, then `near`, all synced. */
static void make_lag_files(void)
{
    int slow, fill, near;
    unsigned k;

    slow = syscall3(SYS_OPEN, (long)"/lw/slow", O_WRONLY | O_CREAT | O_TRUNC, 0644);
    fill = syscall3(SYS_OPEN, (long)"/lw/fill", O_WRONLY | O_CREAT | O_TRUNC, 0644);
    near = syscall3(SYS_OPEN, (long)"/lw/near", O_WRONLY | O_CREAT | O_TRUNC, 0644);
    if (slow < 0 || fill < 0 || near < 0) {
        fail("making the files");
    }
    for (k = 0; k < SLOW_BLOCKS; k++) {
        pattern(1, k, BLOCK);
        write_all(slow, buf, BLOCK, "a write of slow");
        pattern(2, k, BLOCK);
        write_all(fill, buf, BLOCK, "a write of fill");
    }
    pattern(3, 0, NEAR_BYTES);
    write_all(near, buf, NEAR_BYTES, "the write of near");
    syscall1(SYS_CLOSE, slow);
    syscall1(SYS_CLOSE, fill);
    syscall1(SYS_CLOSE, near);
    if (syscall1(SYS_SYNC, 0) != 0) {
        fail("sync");
    }
}

/** `lockwaitprobe lag`: see the file's comment. */
static void run_lag(void)
{
    struct shared *sh;
    long got, lag0, lag1, rounds0, rounds1;
    unsigned k, i;
    int fd, pid, status;

    if (syscall2(SYS_MKDIR, (long)"/lw", 0755) != 0) {
        fail("mkdir /lw");
    }
    mount_volume("/lw");
    make_lag_files();
    unmount_volume("/lw");
    mount_volume("/lw");
    sh = (struct shared *)probe_mmap(0, 4096, PROT_RW, MAP_SHARED | MAP_ANONYMOUS, -1, 0);
    if ((long)sh < 0 && (long)sh > -4096) {
        fail("the shared page");
    }
    pid = syscall1(SYS_FORK, 0);
    if (pid < 0) {
        fail("fork");
    }
    if (pid == 0) {
        near_child(sh);
    }
    while (!sh->ready) {
        syscall0(SYS_SCHED_YIELD);
    }
    if (sh->error != 0) {
        wait_status(pid);
        out("LOCKWAITPROBE near: failed step ");
        out_num(sh->step);
        out("\n");
        fail_rc("the first round on near", sh->error);
    }
    fd = syscall3(SYS_OPEN, (long)"/lw/slow", O_RDONLY, 0);
    if (fd < 0) {
        sh->stop = 1;
        wait_status(pid);
        fail_rc("opening slow", fd);
    }
    rounds0 = sh->rounds;
    lag0 = counter("/proc/disk", "lag-completions ");
    got = syscall3(SYS_READ, fd, (long)buf, SLOW_BYTES);
    lag1 = counter("/proc/disk", "lag-completions ");
    rounds1 = sh->rounds;
    sh->stop = 1;
    status = wait_status(pid);
    syscall1(SYS_CLOSE, fd);
    out("LOCKWAITPROBE lag: waits ");
    out_num(lag1 - lag0);
    out(", rounds on near ");
    out_num(rounds1 - rounds0);
    out("\n");
    show_lockwait();
    if (got != (long)SLOW_BYTES) {
        fail_rc("the read of slow", got);
    }
    for (k = 0; k < SLOW_BLOCKS; k++) {
        for (i = 0; i < BLOCK; i++) {
            if (buf[k * BLOCK + i] != (unsigned char)(31u + k * 7u + i)) {
                fail("the bytes of slow");
            }
        }
    }
    if (sh->error != 0) {
        out("LOCKWAITPROBE near: failed step ");
        out_num(sh->step);
        out("\n");
        fail_rc("a round on near", sh->error);
    }
    if (status != 0) {
        fail("the child on near did not exit 0");
    }
    if (lag0 < 0 || lag1 - lag0 < SLOW_WAITS_MIN) {
        fail("the read of slow did not wait for the lag device on its blocks");
    }
    if (rounds1 - rounds0 < lag1 - lag0) {
        fail("the calls on near waited for the read of slow");
    }
    syscall1(SYS_UNLINK, (long)"/lw/slow");
    syscall1(SYS_UNLINK, (long)"/lw/fill");
    syscall1(SYS_UNLINK, (long)"/lw/near");
    unmount_volume("/lw");
    syscall1(SYS_RMDIR, (long)"/lw");
    out("LOCKWAITPROBE OK\n");
}

/** The next number of the generator whose state is `*x`. */
static unsigned next(unsigned *x)
{
    *x = *x * 1103515245u + 12345u;
    return (*x >> 16) & 0x7fffu;
}

/** Write into `path` "/ls/", the shared directory `d` (a or b), "/", and a
 * file's name fN or a directory's sN for `n`. */
static void name_of(char *path, unsigned d, unsigned n, int dir)
{
    path[0] = '/';
    path[1] = 'l';
    path[2] = 's';
    path[3] = '/';
    path[4] = (char)('a' + d);
    path[5] = '/';
    path[6] = dir ? 's' : 'f';
    path[7] = (char)('a' + n);
    path[8] = 0;
}

/** Whether `rc` is an answer a call in the shared directories may give
 * because another worker got there first. */
static int raced(long rc)
{
    return rc == -ENOENT || rc == -EEXIST || rc == -ENOTEMPTY;
}

/** Fail worker `w`'s call `what`, which answered `rc`. */
static void worker_fail(int w, const char *what, long rc)
{
    out("LOCKWAITPROBE worker ");
    out_num(w);
    out(": ");
    fail_rc(what, rc);
}

/** One call of worker `w` on the shared directories, chosen from `*x`. */
static void worker_call(int w, unsigned *x)
{
    static unsigned char block[FILE_MOST];
    char a[16], b[16];
    unsigned op = next(x) % 10u, len = next(x) % FILE_MOST, at = next(x) % FILE_MOST;
    long rc = 0;
    int fd;

    name_of(a, next(x) % 2u, next(x) % FILE_NAMES, 0);
    name_of(b, next(x) % 2u, next(x) % FILE_NAMES, 0);
    switch (op) {
    case 0: /* make or empty a file and write it */
        fd = syscall3(SYS_OPEN, (long)a, O_WRONLY | O_CREAT | O_TRUNC, 0644);
        if (fd < 0) {
            rc = fd;
            break;
        }
        rc = syscall3(SYS_WRITE, fd, (long)block, len);
        rc = rc == (long)len ? 0 : rc < 0 ? rc : -1000;
        syscall1(SYS_CLOSE, fd);
        break;
    case 1: /* rename a file over another, in its directory or the other */
        rc = syscall2(SYS_RENAME, (long)a, (long)b);
        break;
    case 2:
        rc = syscall1(SYS_UNLINK, (long)a);
        break;
    case 3:
        rc = syscall2(SYS_TRUNCATE, (long)a, len);
        break;
    case 4:
        fd = syscall3(SYS_OPEN, (long)a, O_RDONLY, 0);
        if (fd < 0) {
            rc = fd;
            break;
        }
        rc = syscall3(SYS_READ, fd, (long)block, FILE_MOST);
        rc = rc < 0 ? rc : 0;
        syscall1(SYS_CLOSE, fd);
        break;
    case 5: /* write into an existing file, past its end or inside it */
    case 6: /* and fsync it */
        fd = syscall3(SYS_OPEN, (long)a, O_WRONLY, 0);
        if (fd < 0) {
            rc = fd;
            break;
        }
        if (len > FILE_MOST - at) {
            len = FILE_MOST - at;
        }
        rc = syscall3(SYS_LSEEK, fd, at, 0);
        if (rc == (long)at) {
            rc = syscall3(SYS_WRITE, fd, (long)block, len);
            rc = rc == (long)len ? 0 : rc < 0 ? rc : -1000;
        }
        if (rc == 0 && op == 6) {
            rc = syscall1(SYS_FSYNC, fd);
        }
        syscall1(SYS_CLOSE, fd);
        break;
    case 7:
        name_of(a, next(x) % 2u, next(x) % DIR_NAMES, 1);
        rc = syscall2(SYS_MKDIR, (long)a, 0755);
        break;
    case 8:
        name_of(a, next(x) % 2u, next(x) % DIR_NAMES, 1);
        rc = syscall1(SYS_RMDIR, (long)a);
        break;
    default: /* rename a directory, over an empty one or to a free name */
        name_of(a, next(x) % 2u, next(x) % DIR_NAMES, 1);
        name_of(b, next(x) % 2u, next(x) % DIR_NAMES, 1);
        rc = syscall2(SYS_RENAME, (long)a, (long)b);
        break;
    }
    if (rc != 0 && !raced(rc)) {
        static const char *const what[] = {"create", "rename", "unlink", "truncate", "read",
                                           "write",  "fsync",  "mkdir",  "rmdir",    "rename dir"};

        worker_fail(w, what[op], rc);
    }
}

/** The memory child: take the free memory and the buffer cache's, but for
 * a sixteenth of the whole, a page at a time, say so on `ready` and wait
 * to be killed. */
static void hog(int ready)
{
    long total = counter("/proc/meminfo", "MemTotal:");
    long avail = counter("/proc/meminfo", "MemFree:");
    long buffers = counter("/proc/meminfo", "Buffers:");
    unsigned long take, off;
    unsigned char *mem;

    if (total <= 0 || avail < 0 || buffers < 0) {
        sys_exit(2);
    }
    take = avail + buffers > total / 16 ? (unsigned long)(avail + buffers - total / 16) * 1024u : 0;
    if (take != 0) {
        mem =
            (unsigned char *)probe_mmap(0, (long)take, PROT_RW, MAP_PRIVATE | MAP_ANONYMOUS, -1, 0);
        if ((long)mem < 0 && (long)mem > -4096) {
            sys_exit(3);
        }
        for (off = 0; off < take; off += 4096) {
            mem[off] = 1;
        }
    }
    syscall3(SYS_WRITE, ready, (long)"m", 1);
    for (;;) {
        syscall0(SYS_PAUSE);
    }
}

/** Read every file left under /ls/a and /ls/b whole, failing on an error. */
static void read_back(void)
{
    char a[16];
    unsigned d, n;
    long rc;
    int fd;

    for (d = 0; d < 2; d++) {
        for (n = 0; n < FILE_NAMES; n++) {
            name_of(a, d, n, 0);
            fd = syscall3(SYS_OPEN, (long)a, O_RDONLY, 0);
            if (fd == -ENOENT) {
                continue;
            }
            if (fd < 0) {
                fail_rc("opening a file left", fd);
            }
            do {
                rc = syscall3(SYS_READ, fd, (long)buf, SLOW_BYTES);
            } while (rc > 0);
            syscall1(SYS_CLOSE, fd);
            if (rc < 0) {
                fail_rc("reading a file left", rc);
            }
        }
    }
}

/** Whether /proc/mounts lists /ls read-only. */
static int ls_read_only(void)
{
    static char text[4096];
    static const char want[] = " /ls ext2 ro";
    int fd = syscall3(SYS_OPEN, (long)"/proc/mounts", O_RDONLY, 0);
    int n, i, j;

    if (fd < 0) {
        return 1;
    }
    n = syscall3(SYS_READ, fd, (long)text, sizeof(text) - 1);
    syscall1(SYS_CLOSE, fd);
    text[n > 0 ? n : 0] = 0;
    for (i = 0; i < n; i++) {
        for (j = 0; want[j] && text[i + j] == want[j]; j++) {
        }
        if (!want[j]) {
            return 1;
        }
    }
    return 0;
}

/** `lockwaitprobe stress`: see the file's comment. */
static void run_stress(void)
{
    int pids[WORKERS], ready[2], memory, w, k, status, failed = 0;
    char m;

    if (syscall2(SYS_MKDIR, (long)"/ls", 0755) != 0) {
        fail("mkdir /ls");
    }
    mount_volume("/ls");
    if (syscall2(SYS_MKDIR, (long)"/ls/a", 0755) != 0 ||
        syscall2(SYS_MKDIR, (long)"/ls/b", 0755) != 0) {
        fail("making the shared directories");
    }
    if (syscall1(SYS_PIPE, (long)ready) != 0) {
        fail("pipe");
    }
    out("LOCKWAITPROBE stress: buffers before, kB ");
    out_num(counter("/proc/meminfo", "Buffers:"));
    out("\n");
    memory = syscall1(SYS_FORK, 0);
    if (memory < 0) {
        fail("fork");
    }
    if (memory == 0) {
        syscall1(SYS_CLOSE, ready[0]);
        hog(ready[1]);
    }
    /* The child's exit before its word gives EOF only with this end closed. */
    syscall1(SYS_CLOSE, ready[1]);
    if (syscall3(SYS_READ, ready[0], (long)&m, 1) != 1) {
        fail_rc("the memory child", wait_status(memory));
    }
    syscall1(SYS_CLOSE, ready[0]);
    out("LOCKWAITPROBE stress: free under pressure, kB ");
    out_num(counter("/proc/meminfo", "MemFree:"));
    out("\n");
    for (w = 0; w < WORKERS; w++) {
        pids[w] = syscall1(SYS_FORK, 0);
        if (pids[w] < 0) {
            fail("fork");
        }
        if (pids[w] == 0) {
            unsigned x = 2654435761u * (unsigned)(w + 1);

            for (k = 0; k < CALLS; k++) {
                worker_call(w, &x);
            }
            sys_exit(0);
        }
    }
    for (w = 0; w < WORKERS; w++) {
        status = wait_status(pids[w]);
        failed |= status != 0;
    }
    out("LOCKWAITPROBE stress: buffers at the end, kB ");
    out_num(counter("/proc/meminfo", "Buffers:"));
    out("\n");
    syscall2(SYS_KILL, memory, SIGKILL);
    wait_status(memory);
    if (failed) {
        fail("a worker failed or was killed");
    }
    read_back();
    if (ls_read_only()) {
        fail("the volume went read-only");
    }
    if (syscall1(SYS_SYNC, 0) != 0) {
        fail("sync");
    }
    unmount_volume("/ls");
    syscall1(SYS_RMDIR, (long)"/ls");
    show_lockwait();
    out("LOCKWAITPROBE OK\n");
}

void probe_main(unsigned long *sp);

PROBE_SP_ENTRY;

/** Whether the strings `a` and `b` are equal. */
static int same(const char *a, const char *b)
{
    while (*a && *a == *b) {
        a++;
        b++;
    }
    return *a == *b;
}

/** The probe, entered with `sp` at argc: the mode its first argument names. */
void probe_main(unsigned long *sp)
{
    int argc = (int)sp[0];
    char **argv = (char **)(sp + 1);

    lock_stat_on();
    if (argc >= 2 && same(argv[1], "lag")) {
        run_lag();
    } else if (argc >= 2 && same(argv[1], "stress")) {
        run_stress();
    } else {
        fail("usage: lockwaitprobe lag|stress");
    }
    sys_exit(0);
}
