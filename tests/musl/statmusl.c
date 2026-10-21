/**
 * statmusl: check that every stat path reports the times the file system has.
 *
 * The kernel fills a stat buffer in four places: `krffs_fill_stat64` for a
 * path, `fstat_fill` for a descriptor, `do_old_stat` for the narrower stat(2)
 * ABI that only i386 has, and `synth_stat` for /dev and /proc. A fault in one
 * leaves the others right, so each is reached here by a call of its own. On
 * i686 musl's stat, lstat, fstat and fstatat all make statx, which fills
 * through the same code; on x86-64 they make stat, lstat, fstat and
 * newfstatat.
 *
 * The times are set with `utimensat`, so the probe needs no other call to
 * stamp a file. Two kinds of node cannot be checked that way:
 * `utimensat` succeeds for the KRFFS root without changing its times, and
 * fails with EROFS for a synthetic node. stampmusl checks the root, where a
 * file made in it stamps it. A synthetic node reports the boot time, which
 * moves with every step of the clock, and the probe computes that as the real
 * time less the time since boot, instead of comparing two synthetic nodes,
 * which a fill that returned the same wrong constant for both would pass. On
 * a machine whose clock could not be read and has not been set, the real time
 * is the time since boot, so the computed boot time is zero, which is also
 * what a synthetic node reports then.
 *
 * Times outside 32 bits are checked too: a time before 1970, and times past
 * 2^31 and 2^32 seconds, in 2038 and 2106. On i686 musl sets the later two
 * with utimensat_time64, since utimensat's timespec has 32-bit seconds there.
 * i386's stat64 has room for only the low 32 bits of a time; statx, which
 * musl's stat makes there, reports them whole.
 *
 * Prints `STATMUSL OK` and exits 0 on success, and prints
 * `STATMUSL FAIL (detail)` and exits 1 otherwise. Writes etude's report
 * records (../probe_report.h) for the case ETUDE_CASE names, an assertion for
 * each check `checks` lists, with the branch `old-stat` present on i686 and
 * absent on x86-64; a failure leaves the checks after it not run.
 */

#define _GNU_SOURCE
#include <fcntl.h>
#include <stdio.h>
#include <sys/stat.h>
#include <sys/syscall.h>
#include <sys/time.h>
#include <time.h>
#include <unistd.h>

#include "../probe_report.h"

#if !defined __x86_64__
/*
 * The old stat(2) struct, as the kernel writes it (struct linux_stat in
 * rach2/sys/arch/i686/uabi.h). musl never makes that call, since its stat on
 * i386 goes to statx, so the probe makes it by number. x86-64 has no narrower
 * stat: its stat(2) is the whole struct, which musl's stat reaches, and the
 * rows of the old struct are built for i686 alone.
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
#endif

/* An instant with a nonzero nanosecond part, so a fill that reports whole
 * seconds and drops the rest is caught too. */
#define WHEN_SEC  1786712707L
#define WHEN_NSEC 123456789L

/* A time before 1970: 1 January 1969, a negative count of seconds. */
#define BEFORE_1970 (-31536000L)

/* Times past the signed and the unsigned 32-bit seconds: ten seconds past
 * 2^31, and past 2^32. */
#define PAST_2038 (((time_t)1 << 31) + 10)
#define PAST_2106 (((time_t)1 << 32) + 10)

static const char *const PATH = "/stattest.tmp";
static const char *const DIR = "/stattestdir";

/* The checks, one assertion each, in the order the program makes them, and
 * the text it prints for each that fails. The checks from `OLD_STAT` to
 * `OLD_STAT_DIR_TYPE` are those of the old stat(2), which i386 alone has. */
enum {
    CREATE_FILE,
    WRITE_FILE,
    UTIMENSAT,
    STAT_TIMES,
    STAT_CTIME,
    LSTAT_TIMES,
    FSTAT_TIMES,
    OPEN_ROOT,
    FSTATAT_TIMES,
    FSTATAT_CTIME,
    MKDIR,
    UTIMENSAT_DIR,
    DIR_TIMES,
    DIR_TYPE,
    OLD_STAT,
    OLD_STAT_TIMES,
    OLD_STAT_CTIME,
    OLD_LSTAT,
    OLD_LSTAT_TIMES,
    OLD_STAT_DIR_TYPE,
    CLOCK_GETTIME,
    STAT_DEV_NULL,
    DEV_NULL_BOOT,
    STAT_PROC,
    SYNTHETIC_AGREE,
    SYNTHETIC_STEADY,
    OPEN_DEV_NULL,
    FSTAT_DEVICE,
    REOPEN_FILE,
    TIME_BEFORE_1970,
    TIME_PAST_2038,
    TIME_PAST_2106,
    CHECKS
};

static const struct {
    const char *id;
    const char *text;
} checks[CHECKS] = {
    {"create-file", "could not create the file"},
    {"write-file", "could not write the file"},
    {"utimensat", "utimensat failed"},
    {"stat-times", "stat did not report the times that were set"},
    {"stat-ctime", "stat did not report the attribute-change time"},
    {"lstat-times", "lstat did not report the times that were set"},
    {"fstat-times", "fstat did not report the times that were set"},
    {"open-root", "could not open the root directory"},
    {"fstatat-times", "fstatat did not report the times that were set"},
    {"fstatat-ctime", "fstatat did not report the attribute-change time"},
    {"mkdir", "could not create the directory"},
    {"utimensat-dir", "utimensat on a directory failed"},
    {"dir-times", "a directory did not report the times that were set"},
    {"dir-type", "a directory did not report itself as one"},
    {"old-stat", "the old stat(2) failed"},
    {"old-stat-times", "the old stat(2) did not report the times that were set"},
    {"old-stat-ctime", "the old stat(2) did not report the attribute-change time"},
    {"old-lstat", "the old lstat(2) failed"},
    {"old-lstat-times", "the old lstat(2) did not report a directory's times"},
    {"old-stat-dir-type", "the old stat(2) reported a directory as a regular file"},
    {"clock-gettime", "clock_gettime failed"},
    {"stat-dev-null", "could not stat /dev/null"},
    {"dev-null-boot", "/dev/null does not report the boot time"},
    {"stat-proc", "could not stat a /proc file"},
    {"synthetic-agree", "two synthetic nodes disagree about the boot time"},
    {"synthetic-steady", "a synthetic node's time moved between two stats"},
    {"open-dev-null", "could not open /dev/null"},
    {"fstat-device", "fstat of a device did not report the boot time"},
    {"reopen-file", "could not open the file again"},
    {"time-before-1970", "a time before 1970"},
    {"time-past-2038", "a time past 2^31 seconds"},
    {"time-past-2106", "a time past 2^32 seconds"},
};

/** In the process that began the report, report the branch of the group
 * `old-stat`, every check of it before `check` as held, `check` as failed
 * with `why`, those after it as not run, and the report's end; with `check`
 * CHECKS, every check held. */
static void report_at(int check, const char *why)
{
    int old = 1;
    int i;

#if defined __x86_64__
    old = 0;
#endif
    if (report_ended || report_getpid() != report_owner) {
        return;
    }
    report_branch("old-stat", old ? "present" : "absent");
    for (i = 0; i < CHECKS; i++) {
        if (!old && i >= OLD_STAT && i <= OLD_STAT_DIR_TYPE) {
            continue;
        }
        if (i < check) {
            report_pass(checks[i].id);
        } else if (i == check) {
            report_fail(checks[i].id, why);
        } else {
            report_notrun(checks[i].id, "an earlier check failed");
        }
    }
    report_end();
}

/** Report that the check `check` failed as `what` and return 1, the probe's
 * failure status. */
static int fail_as(int check, const char *what)
{
    printf("STATMUSL FAIL (%s)\n", what);
    report_at(check, what);

    return 1;
}

/** Report that the check `check` failed and return 1. */
static int fail(int check)
{
    return fail_as(check, checks[check].text);
}

/** Whether a stat's access and modification times are `WHEN_SEC` and
 * `WHEN_NSEC`. */
static int times_match(const struct stat *st)
{
    return (long)st->st_mtim.tv_sec == WHEN_SEC && (long)st->st_mtim.tv_nsec == WHEN_NSEC &&
           (long)st->st_atim.tv_sec == WHEN_SEC && (long)st->st_atim.tv_nsec == WHEN_NSEC;
}

/**
 * Whether a stat's ctime is the moment `utimensat` ran: that call sets ctime
 * to the current time, whatever times it was given. The time is checked to
 * lie between `before` and `after`, two readings of the clock taken around
 * the call.
 */
static int ctime_between(const struct stat *st, long before, long after)
{
    long ctime = (long)st->st_ctim.tv_sec;

    return ctime >= before && ctime <= after;
}

#if !defined __x86_64__
/**
 * Check the old stat(2) ABI, by number, on the file and then on the
 * directory: the times that were set, and an attribute-change time between
 * `before` and `after`. Returns 0, or 1 after the failure is printed.
 */
static int old_stat_times(long before, long after)
{
    struct old_stat os;
    long rc;

    rc = syscall(SYS_stat, PATH, &os);
    if (rc != 0) {
        return fail(OLD_STAT);
    }
    if ((long)os.st_mtime_seconds != WHEN_SEC || (long)os.st_mtime_nanoseconds != WHEN_NSEC ||
        (long)os.st_atime_seconds != WHEN_SEC || (long)os.st_atime_nanoseconds != WHEN_NSEC) {
        return fail(OLD_STAT_TIMES);
    }
    if ((long)os.st_ctime_seconds < (unsigned long)before ||
        (long)os.st_ctime_seconds > (unsigned long)after) {
        return fail(OLD_STAT_CTIME);
    }
    rc = syscall(SYS_lstat, DIR, &os);
    if (rc != 0) {
        return fail(OLD_LSTAT);
    }
    if ((long)os.st_mtime_seconds != WHEN_SEC) {
        return fail(OLD_LSTAT_TIMES);
    }
    if ((os.st_mode & 0170000) != 0040000) {
        return fail(OLD_STAT_DIR_TYPE);
    }

    return 0;
}
#endif

/**
 * Set `PATH`'s times to `when` and `WHEN_NSEC`, through the path, or through
 * `fd` with futimens when `byfd`, and check that stat and fstat report them
 * whole, as the check `check`. Returns 0, or 1 after the failure is printed.
 */
static int wide_time(int fd, time_t when, int byfd, int check)
{
    struct timespec times[2];
    struct stat st, fst;
    char detail[96];

    times[0].tv_sec = when;
    times[0].tv_nsec = WHEN_NSEC;
    times[1] = times[0];
    if ((byfd ? futimens(fd, times) : utimensat(AT_FDCWD, PATH, times, 0)) != 0) {
        snprintf(detail, sizeof(detail), "%s to %lld seconds failed",
                 byfd ? "futimens" : "utimensat", (long long)when);

        return fail_as(check, detail);
    }
    if (stat(PATH, &st) != 0 || st.st_mtim.tv_sec != when || st.st_atim.tv_sec != when ||
        st.st_mtim.tv_nsec != WHEN_NSEC) {
        snprintf(detail, sizeof(detail), "stat reports %lld for %lld seconds",
                 (long long)st.st_mtim.tv_sec, (long long)when);

        return fail_as(check, detail);
    }
    if (fstat(fd, &fst) != 0 || fst.st_mtim.tv_sec != when || fst.st_atim.tv_sec != when) {
        snprintf(detail, sizeof(detail), "fstat reports %lld for %lld seconds",
                 (long long)fst.st_mtim.tv_sec, (long long)when);

        return fail_as(check, detail);
    }

    return 0;
}

/**
 * Check that stat, and fstat through `fd`, report the times of `PATH` whole
 * when they lie outside 32 bits: a time before 1970, set through the path,
 * one past 2^31 seconds, set through the path, and one past 2^32 seconds, set
 * through `fd`. Returns 0, or 1 after the failure is printed.
 */
static int wide_times(int fd)
{
    return wide_time(fd, BEFORE_1970, 0, TIME_BEFORE_1970) ||
           wide_time(fd, PAST_2038, 0, TIME_PAST_2038) ||
           wide_time(fd, PAST_2106, 1, TIME_PAST_2106);
}

/**
 * Check that the synthetic nodes /dev/null and /proc/self/stat report the boot
 * time, the real time less the time since boot, through stat and through
 * fstat_fill's own branches, and that it does not move. Returns 0, or 1 after
 * the failure is printed.
 */
static int synthetic_times(void)
{
    struct timespec now;
    struct timespec uptime;
    struct stat st;
    long boot;
    long expected_boot;
    int fd;

    if (clock_gettime(CLOCK_REALTIME, &now) != 0 || clock_gettime(CLOCK_MONOTONIC, &uptime) != 0) {
        return fail(CLOCK_GETTIME);
    }
    expected_boot = (long)now.tv_sec - (long)uptime.tv_sec;
    if (stat("/dev/null", &st) != 0) {
        return fail(STAT_DEV_NULL);
    }
    boot = (long)st.st_mtim.tv_sec;
    if (boot < expected_boot - 2 || boot > expected_boot + 2) {
        char detail[96];

        snprintf(detail, sizeof(detail), "/dev/null reports %ld, and this machine booted at %ld",
                 boot, expected_boot);

        return fail_as(DEV_NULL_BOOT, detail);
    }
    if (stat("/proc/self/stat", &st) != 0) {
        return fail(STAT_PROC);
    }
    if ((long)st.st_mtim.tv_sec != boot) {
        return fail(SYNTHETIC_AGREE);
    }
    sleep(1);
    if (stat("/dev/null", &st) != 0 || (long)st.st_mtim.tv_sec != boot) {
        return fail(SYNTHETIC_STEADY);
    }

    fd = open("/dev/null", O_RDONLY);
    if (fd < 0) {
        return fail(OPEN_DEV_NULL);
    }
    if (fstat(fd, &st) != 0 || (long)st.st_mtim.tv_sec != boot) {
        close(fd);
        return fail(FSTAT_DEVICE);
    }
    close(fd);

    return 0;
}

int main(void)
{
    struct timespec times[2];
    struct stat st;
    long before;
    long after;
    int fd;
    int dirfd;

    report_begin(report_case(environ, "guest.default.statmusl"));
    times[0].tv_sec = WHEN_SEC;
    times[0].tv_nsec = WHEN_NSEC;
    times[1].tv_sec = WHEN_SEC;
    times[1].tv_nsec = WHEN_NSEC;

    fd = open(PATH, O_CREAT | O_RDWR | O_TRUNC, 0644);
    if (fd < 0) {
        return fail(CREATE_FILE);
    }
    if (write(fd, "x", 1) != 1) {
        close(fd);
        return fail(WRITE_FILE);
    }
    before = (long)time(NULL);
    if (utimensat(AT_FDCWD, PATH, times, 0) != 0) {
        close(fd);
        return fail(UTIMENSAT);
    }
    after = (long)time(NULL);

    /* stat, through a path. */
    if (stat(PATH, &st) != 0 || !times_match(&st)) {
        close(fd);
        return fail(STAT_TIMES);
    }
    if (!ctime_between(&st, before, after)) {
        close(fd);
        return fail(STAT_CTIME);
    }
    /* lstat: the same node, the other entry point. */
    if (lstat(PATH, &st) != 0 || !times_match(&st)) {
        close(fd);
        return fail(LSTAT_TIMES);
    }
    /* fstat, through the descriptor: a different fill. */
    if (fstat(fd, &st) != 0 || !times_match(&st)) {
        close(fd);
        return fail(FSTAT_TIMES);
    }
    close(fd);

    /* A relative name: on x86-64 musl makes fstatat(AT_FDCWD, ...) a stat. */
    dirfd = open("/", O_RDONLY | O_DIRECTORY);
    if (dirfd < 0) {
        return fail(OPEN_ROOT);
    }
    if (fstatat(dirfd, PATH + 1, &st, 0) != 0 || !times_match(&st)) {
        close(dirfd);
        return fail(FSTATAT_TIMES);
    }
    if (!ctime_between(&st, before, after)) {
        close(dirfd);
        return fail(FSTATAT_CTIME);
    }
    close(dirfd);

    /* A directory, which reports its own times and its own type. */
    if (mkdir(DIR, 0755) != 0) {
        return fail(MKDIR);
    }
    if (utimensat(AT_FDCWD, DIR, times, 0) != 0) {
        return fail(UTIMENSAT_DIR);
    }
    if (stat(DIR, &st) != 0 || !times_match(&st)) {
        return fail(DIR_TIMES);
    }
    if (!S_ISDIR(st.st_mode)) {
        return fail(DIR_TYPE);
    }

#if !defined __x86_64__
    if (old_stat_times(before, after) != 0) {
        return 1;
    }
#endif

    if (synthetic_times() != 0) {
        return 1;
    }

    /* Times outside 32 bits, last, since they replace the file's. */
    fd = open(PATH, O_RDONLY);
    if (fd < 0) {
        return fail(REOPEN_FILE);
    }
    if (wide_times(fd) != 0) {
        close(fd);
        return 1;
    }
    close(fd);

    unlink(PATH);
    rmdir(DIR);

    report_at(CHECKS, NULL);
    printf("STATMUSL OK\n");

    return 0;
}
