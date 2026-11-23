/*
 * Replay the file calls of the first WebGPU batch's Mesa shader cache, by
 * bare x86-64 system calls with no C library, and time each by the
 * time-stamp counter, so that Linux and Rach2 run the same calls.
 *
 * Build: gcc -O2 -ffreestanding -static -nostdlib -fno-stack-protector -fno-pie
 *        -no-pie -o replay replay.c
 * Use:   replay [FILE [ROUNDS]]
 *        replay FILE COUNT 1 KIND
 *
 * The program makes FILE's directory its root by chroot, as the Lenovo's
 * trials run on an ext2 root, and writes there the cache of 50 parts that
 * Mesa 25.0's mesa_cache_db_multipart keeps under MESA_SHADER_CACHE_DIR,
 * /work/case/mesa/mesa_shader_cache_db/partN/mesa_cache.{db,idx}, then
 * syncs. Each pass then makes the 10,549 calls of the Debian capture of
 * 23 November, in the order Mesa's code and glibc's stdio make them for
 * eight lookups that miss in every part: the first lookup's visit of a part
 * makes its directory (EEXIST), opens and loads both files under flock and
 * unlinks the two absent files of the old single-file cache; each later
 * visit reopens both files, reads both headers and the index's end, and
 * closes them. A pass's calls are 900 openat with O_RDWR|O_CREAT|O_CLOEXEC,
 * 900 close, 900 fcntl(F_GETFL), 1,800 flock, 1,350 fstat, 2,300 lseek,
 * 2,249 read, 50 mkdir and 100 unlink, the capture's counts; the files'
 * sizes are made up, as the capture's were not kept.
 *
 * The first pass follows the seeding; ROUNDS more, 9 unless given, give each
 * kind's median total and median call in nanoseconds, by the counter's rate
 * over the pass by CLOCK_MONOTONIC. Every call's result is checked, and a
 * pass prints how many differed from what the replay expects.
 *
 * Given KIND, as profile.sh gives barecall's, the program repeats COUNT
 * times, untimed, one part of a pass for a profile of its kernel time:
 * pass, the whole pass; openat, its 900 opens and the close of each pair;
 * unlink, its 100 absent unlinks; or mkdir, its 50 mkdirs of a directory
 * that exists.
 */

#define BARECALL_LIBRARY
#include "barecall.c"

#define SYS_fcntl 72
#define SYS_flock 73
#define SYS_chroot 161
#define SYS_sync 162
#define SYS_openat 257
#define AT_FDCWD (-100)
#define O_EXCL 0200
#define F_GETFL 3
#define LOCK_EX 2
#define LOCK_UN 8
#define SEEK_SET 0
#define SEEK_CUR 1
#define EEXIST 17
#define ENOENT 2

#define PARTS 50
#define LOOKUPS 8
#define HEADER 24 /* struct mesa_db_file_header */
#define INDEX_ENTRY 28 /* struct mesa_index_db_file_entry */
#define BUFSIZE 4096 /* glibc's stdio buffer, st_blksize on both */
#define CACHE "/work/case/mesa/mesa_shader_cache_db"

enum { K_OPEN, K_CLOSE, K_FCNTL, K_FLOCK, K_FSTAT, K_LSEEK, K_READ, K_MKDIR, K_UNLINK, NKINDS };

static const char *const kind_name[NKINDS] = {"openat", "close", "fcntl", "flock", "fstat",
                                              "lseek",  "read",  "mkdir", "unlink"};

/* the capture's calls of each kind in a pass */
static const unsigned long expect[NKINDS] = {900, 900, 900, 1800, 1350, 2300, 2249, 50, 100};

static unsigned long cycles[NKINDS][2300]; /* each call's counter cycles */
static unsigned long count[NKINDS];
static unsigned long wrong; /* calls whose result differed */
static char buf[BUFSIZE];
static long st[32];

/** Bytes of part P's index: a header and P's entries; part 0 has none. */
static unsigned long index_size(int p)
{
    return HEADER + (p ? (unsigned long)(8 + p * 37 % 53) * INDEX_ENTRY : 0);
}

/** Bytes of part P's cache file: a header and blobs. */
static unsigned long cache_size(int p)
{
    return HEADER + (unsigned long)(8 + p * 37 % 53) * (1200 + (unsigned long)(p * 53 % 97) * 41);
}

/** Make system call N with A, B, C and 0644 as call kind K, timed; its
 * result. The fourth argument is openat's mode. */
static long timed(int k, long n, long a, long b, long c)
{
    unsigned long t0, t1;
    long r;

    t0 = tsc();
    r = sys4(n, a, b, c, 0644);
    t1 = tsc();
    cycles[k][count[k]++] = t1 - t0;
    return r;
}

/** Count a call whose result R is not WANT. */
static void check(long r, long want)
{
    if (r != want)
        wrong++;
}

/** The smaller of A and B. */
static long least(long a, long b)
{
    return a < b ? a : b;
}

/** Part P's directory, or its file NAME when NAME is given, in DST. */
static char *part_path(char *dst, int p, const char *name)
{
    char num[24], dir[64];

    join(dir, CACHE "/part", decimal(num, (unsigned long)p), "");
    return name ? join(dst, dir, "/", name) : join(dst, dir, "", "");
}

/** Open PATH as mesa_db_fopen does: openat and fdopen's F_GETFL. */
static long db_open(const char *path)
{
    long fd = timed(K_OPEN, SYS_openat, AT_FDCWD, (long)path, O_RDWR | O_CREAT | O_CLOEXEC);

    if (fd < 0)
        wrong++;
    check(timed(K_FCNTL, SYS_fcntl, fd, F_GETFL, 0) & 3, O_RDWR);
    return fd;
}

/**
 * Read FD's header, of a file of SIZE bytes, as mesa_db_read_header does
 * through stdio: rewind's fstat for the buffer, seek and fill, then
 * fflush's seek back over the unread bytes and fread's fill again.
 */
static void read_header(long fd, long size)
{
    long n;

    timed(K_FSTAT, SYS_fstat, fd, (long)st, 0);
    check(timed(K_LSEEK, SYS_lseek, fd, 0, SEEK_SET), 0);
    n = timed(K_READ, SYS_read, fd, (long)buf, BUFSIZE);
    check(n, least(size, BUFSIZE));
    check(timed(K_LSEEK, SYS_lseek, fd, HEADER - n, SEEK_CUR), HEADER);
    check(timed(K_READ, SYS_read, fd, (long)buf, BUFSIZE), least(size - HEADER, BUFSIZE));
}

/**
 * Lock part P's two files, read them as a load (FIRST) or a lookup that
 * misses does, and unlock and close them. A load opens the cache file
 * first and reads the index's entries from the header on; a lookup reopens
 * the index first and reads only the index's last block.
 */
static void visit(int p, int first)
{
    char db_path[128], idx_path[128];
    long db, idx, isize = (long)index_size(p), end = isize & -BUFSIZE;

    part_path(db_path, p, "mesa_cache.db");
    part_path(idx_path, p, "mesa_cache.idx");
    if (first) {
        db = db_open(db_path);
        idx = db_open(idx_path);
    } else {
        idx = db_open(idx_path);
        db = db_open(db_path);
    }
    check(timed(K_FLOCK, SYS_flock, db, LOCK_EX, 0), 0);
    check(timed(K_FLOCK, SYS_flock, idx, LOCK_EX, 0), 0);
    read_header(db, (long)cache_size(p));
    read_header(idx, isize);
    /* update_index: fseek SEEK_END's fstat and block seek */
    timed(K_FSTAT, SYS_fstat, idx, (long)st, 0);
    check(timed(K_LSEEK, SYS_lseek, idx, end, SEEK_SET), end);
    if (first) {
        check(timed(K_LSEEK, SYS_lseek, idx, HEADER, SEEK_SET), HEADER);
        if (isize > HEADER) /* fread of no entries makes no call */
            check(timed(K_READ, SYS_read, idx, (long)buf, BUFSIZE), least(isize - HEADER, BUFSIZE));
    } else {
        check(timed(K_READ, SYS_read, idx, (long)buf, BUFSIZE), isize - end);
    }
    check(timed(K_FLOCK, SYS_flock, idx, LOCK_UN, 0), 0);
    check(timed(K_FLOCK, SYS_flock, db, LOCK_UN, 0), 0);
    check(timed(K_CLOSE, SYS_close, idx, 0, 0), 0);
    check(timed(K_CLOSE, SYS_close, db, 0, 0), 0);
}

/** Make the calls of one pass, as the comment at the top says. */
static void pass(void)
{
    char path[128];
    int k, l, p;

    for (k = 0; k < NKINDS; k++)
        count[k] = 0;
    wrong = 0;
    for (l = 0; l < LOOKUPS; l++) {
        for (p = 0; p < PARTS; p++) {
            if (l == 0) {
                check(timed(K_MKDIR, SYS_mkdir, (long)part_path(path, p, 0), 0755, 0), -EEXIST);
                visit(p, 1);
                check(timed(K_UNLINK, SYS_unlink, (long)CACHE "/mesa_cache.db", 0, 0), -ENOENT);
                check(timed(K_UNLINK, SYS_unlink, (long)CACHE "/mesa_cache.idx", 0, 0), -ENOENT);
            }
            visit(p, 0);
        }
    }
}

/** Whether the strings A and B are equal. */
static int same(const char *a, const char *b)
{
    while (*a && *a == *b) {
        a++;
        b++;
    }
    return *a == *b;
}

/** Repeat N times the part of a pass that KIND names, as the comment at the
 * top says; 0, or -1 for a KIND it does not know. */
static int repeat(const char *kind, unsigned long n)
{
    char db[128], idx[128];
    unsigned long i;
    int k, l, p;

    if (!same(kind, "pass") && !same(kind, "openat") && !same(kind, "unlink") &&
        !same(kind, "mkdir"))
        return -1;
    wrong = 0;
    for (i = 0; i < n; i++) {
        if (same(kind, "pass")) {
            pass();
            continue;
        }
        for (k = 0; k < NKINDS; k++)
            count[k] = 0;
        for (l = 0; l <= LOOKUPS; l++) {
            for (p = 0; p < PARTS; p++) {
                if (same(kind, "openat")) {
                    long a = timed(K_OPEN, SYS_openat, AT_FDCWD, (long)part_path(idx, p, "mesa_cache.idx"),
                                   O_RDWR | O_CREAT | O_CLOEXEC);
                    long b = timed(K_OPEN, SYS_openat, AT_FDCWD, (long)part_path(db, p, "mesa_cache.db"),
                                   O_RDWR | O_CREAT | O_CLOEXEC);

                    check(timed(K_CLOSE, SYS_close, a, 0, 0), 0);
                    check(timed(K_CLOSE, SYS_close, b, 0, 0), 0);
                } else if (l == 0 && same(kind, "unlink")) {
                    check(timed(K_UNLINK, SYS_unlink, (long)CACHE "/mesa_cache.db", 0, 0), -ENOENT);
                    check(timed(K_UNLINK, SYS_unlink, (long)CACHE "/mesa_cache.idx", 0, 0), -ENOENT);
                } else if (l == 0) {
                    check(timed(K_MKDIR, SYS_mkdir, (long)part_path(db, p, 0), 0755, 0), -EEXIST);
                }
            }
        }
    }
    return 0;
}

/** Write a file of SIZE zero bytes at PATH unless it exists, as a run
 * before this one in the same boot left it; 0 or the error. */
static long seed_file(const char *path, unsigned long size)
{
    long fd = sys4(SYS_openat, AT_FDCWD, (long)path, O_WRONLY | O_CREAT | O_EXCL, 0644), rc = 0;
    unsigned long n;

    if (fd == -EEXIST)
        return 0;
    if (fd < 0)
        return fd;
    for (n = 0; n < BUFSIZE; n++)
        buf[n] = 0;
    for (n = 0; n < size && rc >= 0; n += BUFSIZE)
        rc = sys3(SYS_write, fd, (long)buf, (long)least((long)(size - n), BUFSIZE));
    sys3(SYS_close, fd, 0, 0);
    return rc < 0 ? rc : 0;
}

/** Make the cache's directories and files under the new root; 0 or -1. */
static int seed(void)
{
    static const char *const dirs[] = {"/work", "/work/case", "/work/case/mesa", CACHE};
    char path[128];
    long rc;
    int i, p;

    for (i = 0; i < 4; i++) {
        rc = sys3(SYS_mkdir, (long)dirs[i], 0755, 0);
        if (rc < 0 && rc != -EEXIST) {
            put_error("mkdir", rc);
            return -1;
        }
    }
    for (p = 0; p < PARTS; p++) {
        rc = sys3(SYS_mkdir, (long)part_path(path, p, 0), 0755, 0);
        if (rc == 0 || rc == -EEXIST)
            rc = seed_file(part_path(path, p, "mesa_cache.db"), cache_size(p));
        if (rc == 0)
            rc = seed_file(part_path(path, p, "mesa_cache.idx"), index_size(p));
        if (rc < 0) {
            put_error("seed", rc);
            return -1;
        }
    }
    sys3(SYS_sync, 0, 0, 0);
    return 0;
}

/**
 * Make one pass and store, in picoseconds, each kind's total in TOTAL and
 * median call in MEDIAN; print them as round R, or as the first pass when
 * R is 0.
 */
static void timed_pass(unsigned long r, unsigned long *total, unsigned long *median)
{
    unsigned long t0, t1, c0, c1, sum, all = 0, i;
    unsigned long rate; /* picoseconds a thousand cycles */
    int k;

    t0 = now_ns();
    c0 = tsc();
    pass();
    c1 = tsc();
    t1 = now_ns();
    rate = (t1 - t0) * 1000000UL / (c1 - c0);
    put(r ? "replay: round " : "replay: first");
    if (r)
        put_num(r);
    for (k = 0; k < NKINDS; k++) {
        if (count[k] != expect[k])
            wrong++;
        for (sum = 0, i = 0; i < count[k]; i++)
            sum += cycles[k][i];
        sort(cycles[k], count[k]);
        total[k] = sum * rate / 1000;
        median[k] = cycles[k][count[k] / 2] * rate / 1000;
        all += total[k];
        put(" ");
        put_milli(total[k] / 1000);
    }
    put(" sum ");
    put_milli(all / 1000);
    put(" wrong ");
    put_num(wrong);
    put("\n");
    total[NKINDS] = all;
}

/** The program, from the stack the kernel left at SP. */
void start_c(long *sp)
{
    long argc = sp[0];
    char **argv = (char **)(sp + 1);
    int pid1 = sys3(SYS_getpid, 0, 0, 0) == 1;
    const char *file = argc > 1 ? argv[1] : "/mnt/f";
    unsigned long rounds = parse(argc > 2 ? argv[2] : 0, 9);
    unsigned long total[NKINDS + 1][MAX_ROUNDS], median[NKINDS][MAX_ROUNDS];
    unsigned long first[NKINDS + 1], first_median[NKINDS], r;
    char root[128];
    unsigned long n;
    long rc;
    int k;

    if (pid1 && mount_volume() != 0)
        finish(pid1);
    if (rounds > MAX_ROUNDS && argc <= 4)
        rounds = MAX_ROUNDS;
    for (n = 0; file[n] && n < sizeof(root) - 1; n++)
        root[n] = file[n];
    while (n > 1 && root[n - 1] != '/')
        n--;
    root[n] = 0;
    rc = sys3(SYS_chroot, (long)root, 0, 0);
    if (rc == 0)
        rc = sys3(SYS_chdir, (long)"/", 0, 0);
    if (rc != 0) {
        put_error("chroot", rc);
        finish(pid1);
    }
    if (seed() != 0)
        finish(pid1);
    if (argc > 4) {
        if (repeat(argv[4], rounds) != 0) {
            put_error("kind", -22);
        } else {
            put("replay: only ");
            put(argv[4]);
            put(" done, wrong ");
            put_num(wrong);
            put("\n");
        }
        finish(pid1);
    }
    put("replay: root ");
    put(root);
    put(" kinds openat close fcntl flock fstat lseek read mkdir unlink, us\n");
    timed_pass(0, first, first_median);
    for (r = 0; r < rounds; r++) {
        unsigned long t[NKINDS + 1], m[NKINDS];

        timed_pass(r + 1, t, m);
        for (k = 0; k <= NKINDS; k++)
            total[k][r] = t[k];
        for (k = 0; k < NKINDS; k++)
            median[k][r] = m[k];
    }
    for (k = 0; k <= NKINDS; k++) {
        sort(total[k], rounds);
        if (k < NKINDS)
            sort(median[k], rounds);
        put("replay: ");
        put(k < NKINDS ? kind_name[k] : "sum");
        put(": total ");
        put_milli(total[k][rounds / 2] / 1000);
        put(" us, min ");
        put_milli(total[k][0] / 1000);
        put(" us, max ");
        put_milli(total[k][rounds - 1] / 1000);
        put(" us, first ");
        put_milli(first[k] / 1000);
        if (k < NKINDS) {
            put(" us; call median ");
            put_milli(median[k][rounds / 2]);
            put(" ns, first ");
            put_milli(first_median[k]);
            put(" ns\n");
        } else {
            put(" us\n");
        }
    }
    put("replay: done\n");
    finish(pid1);
}

__asm__(".globl _start\n"
        "_start:\n"
        "    mov %rsp, %rdi\n"
        "    and $-16, %rsp\n"
        "    call start_c\n"
        "    hlt\n");
