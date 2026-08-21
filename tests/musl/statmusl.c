/**
 * Every stat path reports the times the file system stores.
 *
 * This kernel fills a stat buffer in four places: `krffs_fill_stat64` for a
 * path, `fstat_fill` for a descriptor, `do_old_stat` for the narrower stat(2)
 * ABI, and `synth_stat` for `/dev` and `/proc`. All four zeroed the three
 * times, and a fix to one of them would leave the others reporting 1970, so
 * each is asked here separately, not trusted to share code.
 *
 * The times are SET with `utimensat`, which is the one call that already wrote
 * them before this milestone, so this probe stands on its own: it proves the
 * reporting without needing anything to stamp. Two paths cannot be proved that
 * way and are not asked to be: `utimensat` reports success for the root without
 * modifying it, and refuses a synthetic path with EROFS. The root is proved
 * where creating a file in it stamps it (see the stamping probe); a synthetic
 * node is proved here against the boot time instead, which is what it reports.
 *
 * Prints "STATMUSL OK" on success and "STATMUSL FAIL (detail)" otherwise.
 */

#define _GNU_SOURCE
#include <fcntl.h>
#include <stdio.h>
#include <sys/stat.h>
#include <sys/syscall.h>
#include <sys/time.h>
#include <time.h>
#include <unistd.h>

/*
 * The old stat(2) struct, as the kernel writes it (struct linux_stat in
 * rach2/sys/syscall.c). It is declared here because musl never issues that call
 * on i386: stat() goes to statx, which this kernel does not have, and falls
 * back to fstatat64. So the only way to ask the old fill anything is to ask for
 * it by number, and a fill nothing exercises is a fill nobody notices is wrong.
 */
struct old_stat {
    unsigned long st_dev;
    unsigned long st_ino;
    unsigned short st_mode;
    unsigned short st_nlink;
    unsigned short st_uid;
    unsigned short st_gid;
    unsigned long st_rdev;
    unsigned long st_size;
    unsigned long st_blksize;
    unsigned long st_blocks;
    unsigned long st_atime_seconds;
    unsigned long st_atime_nanoseconds;
    unsigned long st_mtime_seconds;
    unsigned long st_mtime_nanoseconds;
    unsigned long st_ctime_seconds;
    unsigned long st_ctime_nanoseconds;
    unsigned long unused4;
    unsigned long unused5;
};

/* An instant with a nonzero nanosecond part, so a fill that reports whole
 * seconds and drops the rest is caught too. */
#define WHEN_SEC  1786712707L
#define WHEN_NSEC 123456789L

static const char *const PATH = "/stattest.tmp";
static const char *const DIR = "/stattestdir";

/** Report one failed check and return 1, the probe's failure status. */
static int fail(const char *what)
{
    printf("STATMUSL FAIL (%s)\n", what);

    return 1;
}

/** Whether a stat's three times are the instant set above. */
static int times_match(const struct stat *st)
{
    return (long)st->st_mtim.tv_sec == WHEN_SEC && (long)st->st_mtim.tv_nsec == WHEN_NSEC &&
           (long)st->st_atim.tv_sec == WHEN_SEC && (long)st->st_atim.tv_nsec == WHEN_NSEC;
}

/**
 * Whether a stat's ctime is the moment `utimensat` ran, which is the one of the
 * three that call sets to NOW, not to what it was handed. Bounded rather
 * than compared: it is a real instant, so the test is that it lies between two
 * readings of the clock taken around the call.
 */
static int ctime_between(const struct stat *st, long before, long after)
{
    long ctime = (long)st->st_ctim.tv_sec;

    return ctime >= before && ctime <= after;
}

int main(void)
{
    struct timespec times[2];
    struct stat st;
    struct timespec now;
    struct timespec uptime;
    long before;
    long after;
    long boot;
    long expected_boot;
    int fd;
    int dirfd;

    times[0].tv_sec = WHEN_SEC;
    times[0].tv_nsec = WHEN_NSEC;
    times[1].tv_sec = WHEN_SEC;
    times[1].tv_nsec = WHEN_NSEC;

    fd = open(PATH, O_CREAT | O_RDWR | O_TRUNC, 0644);
    if (fd < 0) {
        return fail("could not create the file");
    }
    if (write(fd, "x", 1) != 1) {
        close(fd);
        return fail("could not write the file");
    }
    before = (long)time(NULL);
    if (utimensat(AT_FDCWD, PATH, times, 0) != 0) {
        close(fd);
        return fail("utimensat failed");
    }
    after = (long)time(NULL);

    /* stat64, through a path. */
    if (stat(PATH, &st) != 0 || !times_match(&st)) {
        close(fd);
        return fail("stat did not report the times that were set");
    }
    if (!ctime_between(&st, before, after)) {
        close(fd);
        return fail("stat did not report the attribute-change time");
    }
    /* lstat64: the same node, the other entry point. */
    if (lstat(PATH, &st) != 0 || !times_match(&st)) {
        close(fd);
        return fail("lstat did not report the times that were set");
    }
    /* fstat64, through the descriptor: a different fill entirely. */
    if (fstat(fd, &st) != 0 || !times_match(&st)) {
        close(fd);
        return fail("fstat did not report the times that were set");
    }
    close(fd);

    /* fstatat64 (300), which needs a real directory descriptor and a RELATIVE
     * name to reach: musl turns fstatat(AT_FDCWD, "/abs", ...) into a plain
     * stat, so the obvious spelling of this assertion would have tested the
     * call above a second time. */
    dirfd = open("/", O_RDONLY | O_DIRECTORY);
    if (dirfd < 0) {
        return fail("could not open the root directory");
    }
    if (fstatat(dirfd, PATH + 1, &st, 0) != 0 || !times_match(&st)) {
        close(dirfd);
        return fail("fstatat did not report the times that were set");
    }
    if (!ctime_between(&st, before, after)) {
        close(dirfd);
        return fail("fstatat did not report the attribute-change time");
    }
    close(dirfd);

    /* A directory, which reports its own times and its own type. The old ABI
     * reported every KRFFS node as a regular file until this milestone. */
    if (mkdir(DIR, 0755) != 0) {
        return fail("could not create the directory");
    }
    if (utimensat(AT_FDCWD, DIR, times, 0) != 0) {
        return fail("utimensat on a directory failed");
    }
    if (stat(DIR, &st) != 0 || !times_match(&st)) {
        return fail("a directory did not report the times that were set");
    }
    if (!S_ISDIR(st.st_mode)) {
        return fail("a directory did not report itself as one");
    }

    /* The old ABI, by number, on the file and then on the directory. */
    {
        struct old_stat os;
        long rc;

        rc = syscall(SYS_stat, PATH, &os);
        if (rc != 0) {
            return fail("the old stat(2) failed");
        }
        if ((long)os.st_mtime_seconds != WHEN_SEC || (long)os.st_mtime_nanoseconds != WHEN_NSEC ||
            (long)os.st_atime_seconds != WHEN_SEC || (long)os.st_atime_nanoseconds != WHEN_NSEC) {
            return fail("the old stat(2) did not report the times that were set");
        }
        if ((long)os.st_ctime_seconds < (unsigned long)before ||
            (long)os.st_ctime_seconds > (unsigned long)after) {
            return fail("the old stat(2) did not report the attribute-change time");
        }
        rc = syscall(SYS_lstat, DIR, &os);
        if (rc != 0) {
            return fail("the old lstat(2) failed");
        }
        if ((long)os.st_mtime_seconds != WHEN_SEC) {
            return fail("the old lstat(2) did not report a directory's times");
        }
        if ((os.st_mode & 0170000) != 0040000) {
            return fail("the old stat(2) reported a directory as a regular file");
        }
    }

    /* A synthetic node reports the BOOT time, and the probe works out what that
     * should be instead of taking the answer from the thing it is testing: the
     * real time now, minus the time since boot, is when this machine started. A
     * fill returning a stable wrong constant passes a check that only asks
     * whether two synthetic nodes agree, so the value is computed here.
     *
     * On a machine whose clock could not be read this comes out at zero, and
     * that is the documented fallback, not a failure: the two clocks
     * then report the same thing and the difference between them is nothing. */
    if (clock_gettime(CLOCK_REALTIME, &now) != 0 || clock_gettime(CLOCK_MONOTONIC, &uptime) != 0) {
        return fail("clock_gettime failed");
    }
    expected_boot = (long)now.tv_sec - (long)uptime.tv_sec;
    if (stat("/dev/null", &st) != 0) {
        return fail("could not stat /dev/null");
    }
    boot = (long)st.st_mtim.tv_sec;
    if (boot < expected_boot - 2 || boot > expected_boot + 2) {
        char detail[96];

        snprintf(detail, sizeof(detail), "/dev/null reports %ld, and this machine booted at %ld",
                 boot, expected_boot);

        return fail(detail);
    }
    if (stat("/proc/self/stat", &st) != 0) {
        return fail("could not stat a /proc file");
    }
    if ((long)st.st_mtim.tv_sec != boot) {
        return fail("two synthetic nodes disagree about the boot time");
    }
    sleep(1);
    if (stat("/dev/null", &st) != 0 || (long)st.st_mtim.tv_sec != boot) {
        return fail("a synthetic node's time moved between two stats");
    }

    /* And through a DESCRIPTOR on a synthetic node, which is a fifth place the
     * kernel fills a stat: fstat_fill's own branches, not krffs_fill_stat64. */
    fd = open("/dev/null", O_RDONLY);
    if (fd < 0) {
        return fail("could not open /dev/null");
    }
    if (fstat(fd, &st) != 0 || (long)st.st_mtim.tv_sec != boot) {
        close(fd);
        return fail("fstat of a device did not report the boot time");
    }
    close(fd);

    unlink(PATH);
    rmdir(DIR);

    printf("STATMUSL OK\n");

    return 0;
}
