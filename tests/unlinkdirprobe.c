/**
 * unlinkdirprobe: removing a directory's files while listing it, as `rm -rf`
 * does.
 *
 * `rm -rf` opens a directory, reads a batch of names, unlinks them, and reads
 * again from where it left off. A directory cursor that counted children
 * instead of naming a position that survives deletion would renumber the
 * survivors at every unlink, and the next read would skip one entry for each
 * file already removed; the skipped files would stay, and `rmdir` would fail
 * with "Directory not empty".
 *
 * The probe makes a directory of more entries than one small read holds,
 * removes them as `rm` does, and requires every name to have been listed and
 * the directory to be removable afterwards. Each record's `d_off` increases
 * strictly, and its inode, length and type are checked. Two descriptors on
 * one directory each have a cursor of their own, and each reports the end,
 * the first a second time as well.
 *
 * The directory is `/unlinkdir`, or the path the argument `dir=PATH` gives
 * (probe_args.h), which a native run gives so that the probe works in its
 * workspace or on a volume mounted there.
 *
 * Prints "UNLINKDIRPROBE OK" and exits 0 on success; a failure prints
 * "UNLINKDIRPROBE FAIL: " with the reason and exits 1.
 *
 * Writes etude's report records (probe_report.h) for the case ETUDE_CASE
 * names, an assertion for each check: `create`, `getdents`, `d-off`,
 * `inode`, `reclen`, `dot-type`, `file-type`, `unlink`, `all-listed`,
 * `separate-cursors`, `end-first`, `end-twice`, `end-second`, `clear` and
 * `rmdir`. The branch of the group `dots` is `listed` when the file system
 * lists `.` and `..`, as ext2 does, and `absent` when it does not, as KRFFS
 * does; `dot-type` is an assertion of `listed` alone. A failure fails its
 * assertion and leaves those not yet reported not run. Freestanding; direct
 * system calls (probe.h).
 */

#include "probe.h"
#include "probe_args.h"
#include "probe_report.h"

typedef unsigned int uint32_t;
typedef unsigned short uint16_t;
typedef unsigned char uint8_t;
typedef unsigned long long uint64_t;
typedef long long int64_t;

#define STDOUT_FILENO 1

#define O_RDONLY    0
#define O_DIRECTORY 0200000

#define DT_REG 8
#define DT_DIR 4

/* Enough files that one small read cannot hold them all, so the listing
 * takes several calls and the cursor has to survive the unlinks between
 * them. */
#define FILE_COUNT 40

/* Small, so that the names take several reads: a cursor that loses its place
 * is not seen when every name fits in one. */
#define DIRENT_BUF 256

struct linux_dirent64 {
    uint64_t d_ino;
    int64_t d_off;
    uint16_t d_reclen;
    uint8_t d_type;
    char d_name[];
};

/** write(2). */
static int sys_write(int fd, const void *buf, size_t count)
{
    return syscall3(SYS_WRITE, (long)fd, (long)buf, (long)count);
}

/** exit(2); does not return. */
static void sys_exit(int status)
{
    syscall1(SYS_EXIT, (long)status);
    __builtin_unreachable();
}

/** open(2) with no mode. */
static int sys_open(const char *path, int flags)
{
    return syscall2(SYS_OPEN, (long)path, (long)flags);
}

/** close(2). */
static int sys_close(int fd)
{
    return syscall1(SYS_CLOSE, (long)fd);
}

/** creat(2). */
static int sys_creat(const char *path, int mode)
{
    return syscall2(SYS_CREAT, (long)path, (long)mode);
}

/** unlink(2). */
static int sys_unlink(const char *path)
{
    return syscall1(SYS_UNLINK, (long)path);
}

/** mkdir(2). */
static int sys_mkdir(const char *path, int mode)
{
    return syscall2(SYS_MKDIR, (long)path, (long)mode);
}

/** rmdir(2). */
static int sys_rmdir(const char *path)
{
    return syscall1(SYS_RMDIR, (long)path);
}

/** getdents64(2). */
static int sys_getdents64(int fd, void *dirp, unsigned int count)
{
    return syscall3(SYS_GETDENTS64, (long)fd, (long)dirp, (long)count);
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
    sys_write(STDOUT_FILENO, s, strlen(s));
    sys_write(STDOUT_FILENO, "\n", 1);
}

/* The assertions, in the order of `assertion_id`. */
enum {
    A_CREATE,
    A_GETDENTS,
    A_D_OFF,
    A_INODE,
    A_RECLEN,
    A_DOT_TYPE,
    A_FILE_TYPE,
    A_UNLINK,
    A_ALL_LISTED,
    A_SEPARATE,
    A_END_FIRST,
    A_END_TWICE,
    A_END_SECOND,
    A_CLEAR,
    A_RMDIR,
    A_COUNT
};

static const char *const assertion_id[A_COUNT] = {
    "create",    "getdents",  "d-off",      "inode",      "reclen",
    "dot-type",  "file-type", "unlink",     "all-listed", "separate-cursors",
    "end-first", "end-twice", "end-second", "clear",      "rmdir"};

/* Whether each assertion has been reported; whether the listing has named
 * `.` or `..`, and whether the branch of `dots` has been reported. */
static char reported[A_COUNT];
static int dots_seen, dots_reported;

/** Report the branch of the group `dots`, by whether a dot entry was
 * listed, once. */
static void report_dots(void)
{
    if (!dots_reported) {
        report_branch("dots", dots_seen ? "listed" : "absent");
        dots_reported = 1;
    }
}

/** Report that the assertion `a` held. */
static void held(int a)
{
    report_pass(assertion_id[a]);
    reported[a] = 1;
}

/** Print "UNLINKDIRPROBE FAIL: `why`", report the assertion `a` failed for
 * `why` and those not yet reported not run, end the report, and exit 1. */
static void fail(int a, const char *why)
{
    int i;

    report_dots();
    report_fail(assertion_id[a], why);
    reported[a] = 1;
    for (i = 0; i < A_COUNT; i++) {
        if (!reported[i] && (i != A_DOT_TYPE || dots_seen)) {
            report_notrun(assertion_id[i], "an earlier check failed");
        }
    }
    report_end();
    sys_write(STDOUT_FILENO, "UNLINKDIRPROBE FAIL: ", 21);
    puts(why);
    sys_exit(1);
}

/* The room for the directory's path and a file name under it. */
#define PATH_MAX_LEN 160

/** Build "DIR/fNN" (or "DIR" itself when `n` is negative) in `out`, of
 * PATH_MAX_LEN bytes, where DIR is the argument `dir`, `/unlinkdir` when
 * there is none. A longer DIR fails the probe. */
static void path_for(char *out, int n)
{
    const char *base = probe_arg("dir", "/unlinkdir");
    int i = 0;

    while (base[i]) {
        if (i >= PATH_MAX_LEN - 5) {
            fail(A_CREATE, "the directory's path is too long");
        }
        out[i] = base[i];
        i++;
    }
    if (n < 0) {
        out[i] = '\0';
        return;
    }
    out[i++] = '/';
    out[i++] = 'f';
    out[i++] = (char)('0' + (n / 10));
    out[i++] = (char)('0' + (n % 10));
    out[i] = '\0';
}

/** Unlink the file `name` of a listing of the directory `dir`; a failure
 * fails the probe. */
static void unlink_listed(const char *dir, const char *name)
{
    char victim[PATH_MAX_LEN + 32];
    int k = 0, j = 0;

    while (dir[k]) {
        victim[k] = dir[k];
        k++;
    }
    victim[k++] = '/';
    while (name[j]) {
        victim[k + j] = name[j];
        j++;
    }
    victim[k + j] = '\0';
    if (sys_unlink(victim) != 0) {
        fail(A_UNLINK, "could not unlink a name the listing returned");
    }
}

/** List the directory `dir` in small reads and unlink each file a read
 * names before the next read, checking each record; the count of files
 * removed. */
static int list_and_unlink(const char *dir)
{
    char buf[DIRENT_BUF];
    int fd, rounds, removed = 0;
    int64_t last_off = 0;

    /* Read a batch, unlink what it named, read again from the cursor. */
    fd = sys_open(dir, O_RDONLY | O_DIRECTORY);
    if (fd < 0) {
        fail(A_GETDENTS, "could not open the directory");
    }
    for (rounds = 0; rounds < FILE_COUNT + 8; rounds++) {
        int n = sys_getdents64(fd, buf, sizeof(buf));
        int off = 0;

        if (n < 0) {
            fail(A_GETDENTS, "getdents64 failed part way through");
        }
        if (n == 0) {
            break;
        }
        while (off < n) {
            struct linux_dirent64 *d = (struct linux_dirent64 *)(buf + off);

            /* d_off, where the next read resumes, increases strictly. */
            if (d->d_off <= last_off) {
                fail(A_D_OFF, "the cursor did not advance, so a read could repeat or skip entries");
            }
            last_off = d->d_off;
            if (d->d_ino == 0) {
                fail(A_INODE, "an entry came back with no inode");
            }
            if (d->d_reclen == 0) {
                fail(A_RECLEN, "an entry came back with no length, which would not advance");
            }
            /* `.` and `..`, where they are listed, are directories. */
            if (d->d_name[0] == '.' &&
                (d->d_name[1] == '\0' || (d->d_name[1] == '.' && d->d_name[2] == '\0'))) {
                dots_seen = 1;
                if (d->d_type != DT_DIR) {
                    fail(A_DOT_TYPE, "a dot entry came back as something other than a directory");
                }
                off += d->d_reclen;
                continue;
            }
            if (d->d_type != DT_REG) {
                fail(A_FILE_TYPE, "an entry came back with the wrong type");
            }
            unlink_listed(dir, d->d_name);
            removed++;
            off += d->d_reclen;
        }
    }
    sys_close(fd);
    held(A_GETDENTS);
    held(A_D_OFF);
    held(A_INODE);
    held(A_RECLEN);
    report_dots();
    if (dots_seen) {
        held(A_DOT_TYPE);
    }
    held(A_FILE_TYPE);
    held(A_UNLINK);
    return removed;
}

/** Rebuild the directory `dir` with four files, and require two descriptors
 * on it to have a cursor each and to report the end, the first twice; then
 * clear it. */
static void two_cursors(const char *dir)
{
    char path[PATH_MAX_LEN];
    char buf[DIRENT_BUF];
    int i, fd, a, b, na, nb;

    for (i = 0; i < 4; i++) {
        path_for(path, i);
        fd = sys_creat(path, 0644);
        if (fd < 0) {
            fail(A_SEPARATE, "could not rebuild the directory for the two-descriptor case");
        }
        sys_close(fd);
    }
    a = sys_open(dir, O_RDONLY | O_DIRECTORY);
    b = sys_open(dir, O_RDONLY | O_DIRECTORY);
    if (a < 0 || b < 0) {
        fail(A_SEPARATE, "could not open the directory twice");
    }
    na = sys_getdents64(a, buf, sizeof(buf));
    if (na <= 0) {
        fail(A_SEPARATE, "the first descriptor listed nothing");
    }
    nb = sys_getdents64(b, buf, sizeof(buf));
    if (nb != na) {
        fail(A_SEPARATE, "the second descriptor saw a different listing, so they share a cursor");
    }
    held(A_SEPARATE);
    /* Each reports the end, and again. */
    if (sys_getdents64(a, buf, sizeof(buf)) != 0) {
        fail(A_END_FIRST, "the first descriptor did not report the end");
    }
    held(A_END_FIRST);
    if (sys_getdents64(a, buf, sizeof(buf)) != 0) {
        fail(A_END_TWICE, "the end was not reported a second time, so a caller could loop");
    }
    held(A_END_TWICE);
    if (sys_getdents64(b, buf, sizeof(buf)) != 0) {
        fail(A_END_SECOND, "the second descriptor did not report the end");
    }
    held(A_END_SECOND);
    sys_close(a);
    sys_close(b);
    for (i = 0; i < 4; i++) {
        path_for(path, i);
        if (sys_unlink(path) != 0) {
            fail(A_CLEAR, "could not clear the rebuilt directory");
        }
    }
    held(A_CLEAR);
}

void probe_main(unsigned long *sp);

PROBE_SP_ENTRY;

/** The probe's entry: the listing with unlinks, the two descriptors, and
 * the rmdir at the end. */
void probe_main(unsigned long *sp)
{
    char path[PATH_MAX_LEN];
    char dir[PATH_MAX_LEN];
    int i, fd;

    probe_args((int)sp[0], (char **)(sp + 1));
    report_begin(report_case((char **)(sp + 1) + sp[0] + 1, "guest.default.unlinkdirprobe"));
    path_for(dir, -1);

    /* Start from nothing, in case an earlier run left the directory behind. */
    for (i = 0; i < FILE_COUNT; i++) {
        path_for(path, i);
        sys_unlink(path);
    }
    sys_rmdir(dir);

    if (sys_mkdir(dir, 0755) != 0) {
        fail(A_CREATE, "could not create the directory");
    }
    for (i = 0; i < FILE_COUNT; i++) {
        path_for(path, i);
        fd = sys_creat(path, 0644);
        if (fd < 0) {
            fail(A_CREATE, "could not create a file in it");
        }
        sys_close(fd);
    }
    held(A_CREATE);

    if (list_and_unlink(dir) != FILE_COUNT) {
        fail(A_ALL_LISTED,
             "the listing did not name every file, so unlinking during it lost entries");
    }
    held(A_ALL_LISTED);

    /* Two descriptors on one directory, rebuilt with four files. */
    two_cursors(dir);

    /* The directory is empty, and rmdir removes it. */
    if (sys_rmdir(dir) != 0) {
        fail(A_RMDIR, "the directory would not be removed, so files were left behind");
    }
    held(A_RMDIR);
    report_end();

    puts("UNLINKDIRPROBE OK");
    sys_exit(0);
}
