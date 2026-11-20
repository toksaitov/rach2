/**
 * atcallmusl: check the directory-descriptor forms of the namespace calls.
 *
 * GNU libc makes these calls where musl's wrappers make the plain ones:
 * mkdirat, unlinkat with and without AT_REMOVEDIR, renameat, renameat2 and
 * fchdir. Each call is made with a relative path through a descriptor on a
 * directory the probe makes, and its result is read back by an absolute
 * path, so a call that resolved the name from the wrong directory fails its
 * row. The errors are the ones Linux returns: a descriptor that is not open
 * or not a directory, a flag the call does not take, unlink of a directory
 * and rmdir of a file, and for renameat2 with RENAME_NOREPLACE a new name
 * that exists, which must leave both files as they were. fchdir enters a
 * directory of the root, of /dev and of /proc through descriptors and reads
 * the result back with getcwd. On the tmpfs of /dev/shm, renameat2 swaps
 * two names with RENAME_EXCHANGE and leaves a whiteout with
 * RENAME_WHITEOUT; open makes a file with no name with O_TMPFILE, which
 * linkat names through AT_EMPTY_PATH and through /proc/self/fd, and which
 * follows a link to a directory before a slash whatever O_NOFOLLOW says,
 * and takes O_DIRECT, which /proc and a character device refuse; and a
 * rename or an exchange by another user that moves a directory it cannot
 * write to another parent is EACCES.
 *
 * Prints `ATCALLMUSL OK` and exits 0 on success, and prints
 * `ATCALLMUSL FAIL (detail)` and exits 1 otherwise. Writes etude's report
 * records (../probe_report.h) for the case ETUDE_CASE names, an assertion for
 * each check `checks` lists; a failure leaves the checks after it not run.
 */

#define _GNU_SOURCE
#include <errno.h>
#include <fcntl.h>
#include <stdio.h>
#include <string.h>
#include <sys/stat.h>
#include <sys/syscall.h>
#include <sys/wait.h>
#include <unistd.h>

#include "../probe_report.h"

/* The renameat2 flags, which musl does not declare. */
#define RENAME_NOREPLACE 1
#define RENAME_EXCHANGE  2
#define RENAME_WHITEOUT  4

#define WORK "/atcall"

/* The checks, one assertion each, in the order the program makes them, and
 * the text it prints for each that fails. */
enum {
    WORKDIR,
    DESCRIPTORS,
    MKDIRAT,
    MKDIRAT_MODE,
    MKDIRAT_EEXIST,
    MKDIRAT_EBADF,
    MKDIRAT_ENOTDIR,
    MKDIRAT_ABSOLUTE,
    SUB_FILE,
    UNLINKAT_ENOTEMPTY,
    UNLINKAT_EISDIR,
    UNLINKAT_ENOTDIR,
    UNLINKAT_EINVAL,
    UNLINKAT_FILE,
    UNLINKAT_DIR,
    UNLINKAT_ABSOLUTE,
    RENAME_DIRS,
    RENAME_DESCRIPTORS,
    RENAME_FILES,
    RENAMEAT,
    NOREPLACE_EEXIST,
    NOREPLACE_KEPT,
    NOREPLACE_ENOENT,
    NOREPLACE_FREE,
    RENAMEAT2_REPLACE,
    RENAMEAT2_EINVAL,
    EXCHANGE_ENOENT,
    RENAMEAT_EBADF,
    FCHDIR_ROOT_DIR,
    FCHDIR_RELATIVE,
    FCHDIR_DEV,
    FCHDIR_PROC,
    FCHDIR_ENOTDIR,
    FCHDIR_EBADF,
    SHM_DIR,
    EXCHANGE_FILES,
    EXCHANGE_DIR,
    WHITEOUT,
    TMPFILE_OPEN,
    TMPFILE_NAMELESS,
    TMPFILE_EMPTY_PATH,
    TMPFILE_PROC_FD,
    TMPFILE_EXCL,
    TMPFILE_EINVAL,
    TMPFILE_EOPNOTSUPP,
    DIRECT_TMPFS,
    DIRECT_EINVAL,
    TMPFILE_SLASH,
    MOVE_DIR_WRITE,
    EXCHANGE_DIR_WRITE,
    CHECKS
};

static const struct {
    const char *id;
    const char *text;
} checks[CHECKS] = {
    {"workdir", "the work directory"},
    {"descriptors", "the descriptors the rows use"},
    {"mkdirat", "mkdirat of a relative name"},
    {"mkdirat-mode", "mkdirat does not keep the mode"},
    {"mkdirat-eexist", "mkdirat of a name that exists is not EEXIST"},
    {"mkdirat-ebadf", "mkdirat through a descriptor that is not open is not EBADF"},
    {"mkdirat-enotdir", "mkdirat through a file is not ENOTDIR"},
    {"mkdirat-absolute", "mkdirat of an absolute path reads the descriptor"},
    {"sub-file", "a file in the new directory"},
    {"unlinkat-enotempty", "unlinkat AT_REMOVEDIR of a directory that has a file is not ENOTEMPTY"},
    {"unlinkat-eisdir", "unlinkat of a directory without AT_REMOVEDIR is not EISDIR"},
    {"unlinkat-enotdir", "unlinkat AT_REMOVEDIR of a file is not ENOTDIR"},
    {"unlinkat-einval", "unlinkat with a flag it does not take is not EINVAL"},
    {"unlinkat-file", "unlinkat of a file"},
    {"unlinkat-dir", "unlinkat AT_REMOVEDIR of an empty directory"},
    {"unlinkat-absolute", "unlinkat AT_REMOVEDIR of an absolute path"},
    {"rename-dirs", "the rename directories"},
    {"rename-descriptors", "the rename directories' descriptors"},
    {"rename-files", "the files to rename"},
    {"renameat", "renameat between two descriptors"},
    {"noreplace-eexist", "renameat2 RENAME_NOREPLACE onto a name that exists is not EEXIST"},
    {"noreplace-kept", "a refused RENAME_NOREPLACE changed a file"},
    {"noreplace-enoent", "renameat2 RENAME_NOREPLACE of a missing name is not ENOENT"},
    {"noreplace-free", "renameat2 RENAME_NOREPLACE onto a free name"},
    {"renameat2-replace", "renameat2 without flags does not replace"},
    {"renameat2-einval",
     "renameat2 RENAME_EXCHANGE, RENAME_WHITEOUT or an unknown flag on the root is not EINVAL"},
    {"exchange-enoent", "renameat2 RENAME_EXCHANGE onto a missing name is not ENOENT"},
    {"renameat-ebadf", "renameat through a descriptor that is not open is not EBADF"},
    {"fchdir-root-dir", "fchdir to a directory of the root"},
    {"fchdir-relative", "a relative name after fchdir"},
    {"fchdir-dev", "fchdir to /dev"},
    {"fchdir-proc", "fchdir to /proc/self"},
    {"fchdir-enotdir", "fchdir to a file is not ENOTDIR"},
    {"fchdir-ebadf", "fchdir through a descriptor that is not open is not EBADF"},
    {"shm-dir", "a directory of /dev/shm"},
    {"exchange-files", "renameat2 RENAME_EXCHANGE of two files on tmpfs"},
    {"exchange-dir", "renameat2 RENAME_EXCHANGE of a file and a directory on tmpfs"},
    {"whiteout", "renameat2 RENAME_WHITEOUT on tmpfs leaves no whiteout"},
    {"tmpfile-open", "open O_TMPFILE of a directory of tmpfs"},
    {"tmpfile-nameless", "the O_TMPFILE file has a link or no deleted /proc/self/fd name"},
    {"tmpfile-empty-path", "linkat AT_EMPTY_PATH does not name the O_TMPFILE file"},
    {"tmpfile-proc-fd",
     "linkat AT_SYMLINK_FOLLOW of /proc/self/fd does not name the O_TMPFILE file"},
    {"tmpfile-excl", "linkat of an O_TMPFILE file made with O_EXCL is not ENOENT"},
    {"tmpfile-einval", "open O_TMPFILE with O_RDONLY is not EINVAL"},
    {"tmpfile-eopnotsupp", "open O_TMPFILE on the root is not EOPNOTSUPP"},
    {"direct-tmpfs", "open O_DIRECT of a tmpfs file, its F_GETFL, or an unaligned write"},
    {"direct-einval", "open O_DIRECT of a /proc file or a character device is not EINVAL"},
    {"tmpfile-slash", "open O_TMPFILE with O_NOFOLLOW of a link to a directory and a slash"},
    {"move-dir-write",
     "a rename by another user that moves a directory it cannot write is not EACCES"},
    {"exchange-dir-write", "renameat2 RENAME_EXCHANGE by another user that moves a directory it "
                           "cannot write is not EACCES, or one in its own parent is refused"},
};

/** In the process that began the report, report every check before `check`
 * as held, `check` as failed with its text, those after it as not run, and
 * the report's end; with `check` CHECKS, every check held. */
static void report_at(int check)
{
    int i;

    if (report_ended || report_getpid() != report_owner) {
        return;
    }
    for (i = 0; i < CHECKS; i++) {
        if (i < check) {
            report_pass(checks[i].id);
        } else if (i == check) {
            report_fail(checks[i].id, checks[i].text);
        } else {
            report_notrun(checks[i].id, "an earlier check failed");
        }
    }
    report_end();
}

/** Report the failed check `check` and return 1, the probe's failure
 * status. */
static int fail(int check)
{
    printf("ATCALLMUSL FAIL (%s)\n", checks[check].text);
    report_at(check);

    return 1;
}

/** Whether a call returned -1 with `err`. */
static int refused(long rc, int err)
{
    return rc == -1 && errno == err;
}

/** Create `name` in `dirfd` with `text` in it. Returns 1, or 0 on failure. */
static int make_file(int dirfd, const char *name, const char *text)
{
    int fd = openat(dirfd, name, O_WRONLY | O_CREAT | O_TRUNC, 0644);
    size_t len = strlen(text);
    int ok;

    if (fd < 0) {
        return 0;
    }
    ok = write(fd, text, len) == (ssize_t)len;
    close(fd);
    return ok;
}

/** Whether the file at `path` has exactly `text` in it. */
static int has_text(const char *path, const char *text)
{
    char buf[32];
    int fd = open(path, O_RDONLY);
    ssize_t n;

    if (fd < 0) {
        return 0;
    }
    n = read(fd, buf, sizeof(buf));
    close(fd);
    return n == (ssize_t)strlen(text) && memcmp(buf, text, (size_t)n) == 0;
}

/** Whether `path` names a directory. */
static int is_dir(const char *path)
{
    struct stat st;

    return stat(path, &st) == 0 && S_ISDIR(st.st_mode);
}

/**
 * Check mkdirat and unlinkat through `dirfd`, a descriptor on the work
 * directory; `filefd` is a descriptor on a regular file. Returns 0, or 1 once
 * the failure is reported.
 */
static int mkdir_unlink_rows(int dirfd, int filefd)
{
    struct stat st;

    if (mkdirat(dirfd, "sub", 0700) != 0 || !is_dir(WORK "/sub")) {
        return fail(MKDIRAT);
    }
    if (stat(WORK "/sub", &st) != 0 || (st.st_mode & 0777) != 0700) {
        return fail(MKDIRAT_MODE);
    }
    if (!refused(mkdirat(dirfd, "sub", 0755), EEXIST)) {
        return fail(MKDIRAT_EEXIST);
    }
    if (!refused(mkdirat(999, "x", 0755), EBADF)) {
        return fail(MKDIRAT_EBADF);
    }
    if (!refused(mkdirat(filefd, "x", 0755), ENOTDIR)) {
        return fail(MKDIRAT_ENOTDIR);
    }
    if (mkdirat(999, WORK "/abs", 0755) != 0 || !is_dir(WORK "/abs")) {
        return fail(MKDIRAT_ABSOLUTE);
    }
    if (!make_file(dirfd, "sub/f", "f")) {
        return fail(SUB_FILE);
    }
    if (!refused(unlinkat(dirfd, "sub", AT_REMOVEDIR), ENOTEMPTY)) {
        return fail(UNLINKAT_ENOTEMPTY);
    }
    if (!refused(unlinkat(dirfd, "sub", 0), EISDIR)) {
        return fail(UNLINKAT_EISDIR);
    }
    if (!refused(unlinkat(dirfd, "sub/f", AT_REMOVEDIR), ENOTDIR)) {
        return fail(UNLINKAT_ENOTDIR);
    }
    if (!refused(unlinkat(dirfd, "sub/f", 0x1), EINVAL)) {
        return fail(UNLINKAT_EINVAL);
    }
    if (unlinkat(dirfd, "sub/f", 0) != 0 || access(WORK "/sub/f", F_OK) == 0) {
        return fail(UNLINKAT_FILE);
    }
    if (unlinkat(dirfd, "sub", AT_REMOVEDIR) != 0 || access(WORK "/sub", F_OK) == 0) {
        return fail(UNLINKAT_DIR);
    }
    if (unlinkat(AT_FDCWD, WORK "/abs", AT_REMOVEDIR) != 0) {
        return fail(UNLINKAT_ABSOLUTE);
    }
    return 0;
}

/**
 * Check renameat and renameat2 between descriptors on two directories made in
 * `dirfd`, which are removed again. Returns 0, or 1 once the failure is
 * reported.
 */
static int rename_rows(int dirfd)
{
    int afd, bfd;
    int rc = 0;

    if (mkdirat(dirfd, "a", 0755) != 0 || mkdirat(dirfd, "b", 0755) != 0) {
        return fail(RENAME_DIRS);
    }
    afd = openat(dirfd, "a", O_RDONLY | O_DIRECTORY);
    bfd = openat(dirfd, "b", O_RDONLY | O_DIRECTORY);
    if (afd < 0 || bfd < 0) {
        rc = fail(RENAME_DESCRIPTORS);
    } else if (!make_file(afd, "f", "f") || !make_file(afd, "x", "x") ||
               !make_file(bfd, "y", "y")) {
        rc = fail(RENAME_FILES);
    } else if (renameat(afd, "f", bfd, "g") != 0 || !has_text(WORK "/b/g", "f") ||
               access(WORK "/a/f", F_OK) == 0) {
        rc = fail(RENAMEAT);
    } else if (!refused(syscall(SYS_renameat2, afd, "x", bfd, "y", RENAME_NOREPLACE), EEXIST)) {
        rc = fail(NOREPLACE_EEXIST);
    } else if (!has_text(WORK "/a/x", "x") || !has_text(WORK "/b/y", "y")) {
        rc = fail(NOREPLACE_KEPT);
    } else if (!refused(syscall(SYS_renameat2, afd, "gone", bfd, "y", RENAME_NOREPLACE), ENOENT)) {
        rc = fail(NOREPLACE_ENOENT);
    } else if (syscall(SYS_renameat2, afd, "x", bfd, "z", RENAME_NOREPLACE) != 0 ||
               !has_text(WORK "/b/z", "x") || access(WORK "/a/x", F_OK) == 0) {
        rc = fail(NOREPLACE_FREE);
    } else if (syscall(SYS_renameat2, bfd, "z", bfd, "y", 0) != 0 || !has_text(WORK "/b/y", "x")) {
        rc = fail(RENAMEAT2_REPLACE);
    } else if (!refused(syscall(SYS_renameat2, bfd, "y", bfd, "g", RENAME_EXCHANGE), EINVAL) ||
               !refused(syscall(SYS_renameat2, bfd, "y", bfd, "w", RENAME_WHITEOUT), EINVAL) ||
               !refused(syscall(SYS_renameat2, bfd, "y", bfd, "w", 8), EINVAL)) {
        rc = fail(RENAMEAT2_EINVAL);
    } else if (!refused(syscall(SYS_renameat2, bfd, "y", bfd, "w", RENAME_EXCHANGE), ENOENT)) {
        rc = fail(EXCHANGE_ENOENT);
    } else if (!refused(renameat(999, "y", bfd, "w"), EBADF)) {
        rc = fail(RENAMEAT_EBADF);
    }
    close(afd);
    close(bfd);
    unlink(WORK "/b/g");
    unlink(WORK "/b/y");
    rmdir(WORK "/a");
    rmdir(WORK "/b");
    return rc;
}

/**
 * Whether fchdir through a descriptor opened on `path` makes the working
 * directory `expect`, as getcwd reports it.
 */
static int enters(const char *path, const char *expect)
{
    char cwd[64];
    int fd = open(path, O_RDONLY | O_DIRECTORY);
    int ok;

    if (fd < 0) {
        return 0;
    }
    ok = fchdir(fd) == 0 && getcwd(cwd, sizeof(cwd)) && strcmp(cwd, expect) == 0;
    close(fd);
    return ok;
}

/**
 * Check fchdir through descriptors on three kinds of directory, and its
 * errors for `filefd`, a regular file, and for a descriptor that is not open.
 * Leaves the working directory at the root. Returns 0, or 1 once the failure
 * is reported.
 */
static int fchdir_rows(int filefd)
{
    char self[32];

    snprintf(self, sizeof(self), "/proc/%d", (int)getpid());
    if (!enters(WORK, WORK)) {
        return fail(FCHDIR_ROOT_DIR);
    }
    if (!make_file(AT_FDCWD, "rel", "rel") || !has_text(WORK "/rel", "rel")) {
        return fail(FCHDIR_RELATIVE);
    }
    unlink(WORK "/rel");
    if (!enters("/dev", "/dev")) {
        return fail(FCHDIR_DEV);
    }
    if (!enters("/proc/self", self)) {
        return fail(FCHDIR_PROC);
    }
    if (!refused(fchdir(filefd), ENOTDIR)) {
        return fail(FCHDIR_ENOTDIR);
    }
    if (!refused(fchdir(999), EBADF)) {
        return fail(FCHDIR_EBADF);
    }
    chdir("/");
    return 0;
}

#define SHM "/dev/shm/atcall"

/** Whether the /proc/self/fd link of `fd` reads as a file of SHM with no
 * name: "SHM/#INO (deleted)". */
static int nameless(int fd)
{
    char link[32], target[96];
    ssize_t n;

    snprintf(link, sizeof(link), "/proc/self/fd/%d", fd);
    n = readlink(link, target, sizeof(target) - 1);
    if (n < 0) {
        return 0;
    }
    target[n] = '\0';
    return strncmp(target, SHM "/#", sizeof(SHM "/#") - 1) == 0 && n > 10 &&
           strcmp(target + n - 10, " (deleted)") == 0;
}

/** Make an O_TMPFILE file in SHM with `extra` among its flags and `text`
 * in it. The descriptor, or -1. */
static int tmpfile_with(int extra, const char *text)
{
    int fd = open(SHM, O_TMPFILE | O_RDWR | extra, 0600);
    size_t len = strlen(text);

    if (fd >= 0 && write(fd, text, len) != (ssize_t)len) {
        close(fd);
        return -1;
    }
    return fd;
}

/** Check renameat2's RENAME_EXCHANGE of two files and of a file and a
 * directory, and RENAME_WHITEOUT, on the files p, q and the directory d of
 * SHM. Returns 0, or 1 once the failure is reported. */
static int shm_rename_rows(void)
{
    struct stat st;

    if (syscall(SYS_renameat2, AT_FDCWD, SHM "/p", AT_FDCWD, SHM "/q", RENAME_EXCHANGE) != 0 ||
        !has_text(SHM "/p", "q") || !has_text(SHM "/q", "p")) {
        return fail(EXCHANGE_FILES);
    }
    if (syscall(SYS_renameat2, AT_FDCWD, SHM "/p", AT_FDCWD, SHM "/d", RENAME_EXCHANGE) != 0 ||
        !is_dir(SHM "/p") || !has_text(SHM "/d", "q")) {
        return fail(EXCHANGE_DIR);
    }
    if (syscall(SYS_renameat2, AT_FDCWD, SHM "/q", AT_FDCWD, SHM "/r", RENAME_WHITEOUT) != 0 ||
        !has_text(SHM "/r", "p") || stat(SHM "/q", &st) != 0 || !S_ISCHR(st.st_mode) ||
        st.st_rdev != 0 || (st.st_mode & 07777) != 0) {
        return fail(WHITEOUT);
    }
    return 0;
}

/** Check that an O_TMPFILE file of SHM has no name and no link, and that
 * linkat names one through AT_EMPTY_PATH as SHM/named and another through
 * its /proc/self/fd link as SHM/named2. Returns 0, or 1 once the failure is
 * reported. */
static int shm_tmpfile_rows(void)
{
    char link[32];
    struct stat st;
    int fd, fd2 = -1;
    int rc = 0;

    fd = tmpfile_with(0, "t");
    if (fd < 0) {
        return fail(TMPFILE_OPEN);
    }
    if (fstat(fd, &st) != 0 || st.st_nlink != 0 || !S_ISREG(st.st_mode) ||
        (st.st_mode & 0777) != 0600 || !nameless(fd)) {
        rc = fail(TMPFILE_NAMELESS);
    } else if (linkat(fd, "", AT_FDCWD, SHM "/named", AT_EMPTY_PATH) != 0 ||
               !has_text(SHM "/named", "t") || fstat(fd, &st) != 0 || st.st_nlink != 1) {
        rc = fail(TMPFILE_EMPTY_PATH);
    } else if ((fd2 = tmpfile_with(0, "u")) < 0 ||
               snprintf(link, sizeof(link), "/proc/self/fd/%d", fd2) < 0 ||
               linkat(AT_FDCWD, link, AT_FDCWD, SHM "/named2", AT_SYMLINK_FOLLOW) != 0 ||
               !has_text(SHM "/named2", "u")) {
        rc = fail(TMPFILE_PROC_FD);
    }
    close(fd);
    if (fd2 >= 0) {
        close(fd2);
    }
    return rc;
}

/** Check O_TMPFILE's refusals: ENOENT from linkat for a file made with
 * O_EXCL, EINVAL without write access, and EOPNOTSUPP on the KRFFS root.
 * Returns 0, or 1 once the failure is reported. */
static int shm_tmpfile_refusals(void)
{
    int fd = tmpfile_with(O_EXCL, "v");
    int rc = 0;

    if (fd < 0 || !refused(linkat(fd, "", AT_FDCWD, SHM "/named3", AT_EMPTY_PATH), ENOENT)) {
        rc = fail(TMPFILE_EXCL);
    } else if (!refused(open(SHM, O_TMPFILE | O_RDONLY, 0600), EINVAL)) {
        rc = fail(TMPFILE_EINVAL);
    } else if (!refused(open(WORK, O_TMPFILE | O_RDWR, 0600), EOPNOTSUPP)) {
        rc = fail(TMPFILE_EOPNOTSUPP);
    }
    if (fd >= 0) {
        close(fd);
    }
    return rc;
}

/** Check that SHM/named takes O_DIRECT, which F_GETFL reports, and an
 * unaligned write, as tmpfs does on Linux, and that /proc and /dev/null
 * refuse it with EINVAL. Returns 0, or 1 once the failure is reported. */
static int shm_direct_rows(void)
{
    int fd = open(SHM "/named", O_RDWR | O_DIRECT);
    int rc = 0;

    if (fd < 0 || !(fcntl(fd, F_GETFL) & O_DIRECT) || pwrite(fd, "xyz", 3, 1) != 3 ||
        !has_text(SHM "/named", "txyz")) {
        rc = fail(DIRECT_TMPFS);
    } else if (!refused(open("/proc/self/status", O_RDONLY | O_DIRECT), EINVAL) ||
               !refused(open("/dev/null", O_RDONLY | O_DIRECT), EINVAL)) {
        rc = fail(DIRECT_EINVAL);
    }
    if (fd >= 0) {
        close(fd);
    }
    return rc;
}

/** Check that O_TMPFILE follows SHM/link, a link to SHM, when a slash
 * comes after it, whatever O_NOFOLLOW says, as Linux's walk follows a link
 * before a trailing slash. Returns 0, or 1 once the failure is reported. */
static int shm_tmpfile_slash(void)
{
    int fd = -1;

    if (symlink(SHM, SHM "/link") == 0) {
        fd = open(SHM "/link/", O_TMPFILE | O_RDWR | O_NOFOLLOW, 0600);
    }
    unlink(SHM "/link");
    if (fd < 0) {
        return fail(TMPFILE_SLASH);
    }
    close(fd);
    return 0;
}

/* The user the rows of a moved directory's write permission run as. */
#define NOBODY 65534

/** As NOBODY, try the renames of `shm_dir_write_rows`: 0 when each is
 * answered as on Linux, 1 for the rename, 2 for an exchange, 3 when the
 * user cannot be taken. */
static int dir_write_child(void)
{
    if (setgid(NOBODY) != 0 || setuid(NOBODY) != 0) {
        return 3;
    }
    if (!refused(rename(SHM "/a/ro", SHM "/b/ro"), EACCES)) {
        return 1;
    }
    if (!refused(
            syscall(SYS_renameat2, AT_FDCWD, SHM "/a/ro", AT_FDCWD, SHM "/b/g", RENAME_EXCHANGE),
            EACCES) ||
        !refused(
            syscall(SYS_renameat2, AT_FDCWD, SHM "/b/g", AT_FDCWD, SHM "/a/ro", RENAME_EXCHANGE),
            EACCES) ||
        syscall(SYS_renameat2, AT_FDCWD, SHM "/a/ro", AT_FDCWD, SHM "/a/f", RENAME_EXCHANGE) != 0) {
        return 2;
    }
    return 0;
}

/**
 * Check, as NOBODY with write permission on both parents, SHM/a and SHM/b,
 * that a rename moving the directory SHM/a/ro, of mode 0555, to the other
 * parent, and RENAME_EXCHANGE moving it there from either name, are EACCES,
 * since each rewrites the directory's `..`, as Linux's `vfs_rename` asks
 * write permission on a directory that changes parent; and that its
 * exchange with SHM/a/f, in its own parent, moves nothing and is allowed.
 * What the rows make is removed. Returns 0, or 1 once the failure is
 * reported.
 */
static int shm_dir_write_rows(void)
{
    int status = 0, code = 3;
    pid_t pid;

    if (mkdir(SHM "/a", 0777) == 0 && chmod(SHM "/a", 0777) == 0 && mkdir(SHM "/b", 0777) == 0 &&
        chmod(SHM "/b", 0777) == 0 && mkdir(SHM "/a/ro", 0555) == 0 &&
        make_file(AT_FDCWD, SHM "/a/f", "f") && make_file(AT_FDCWD, SHM "/b/g", "g")) {
        pid = fork();
        if (pid == 0) {
            _exit(dir_write_child());
        }
        if (pid > 0 && waitpid(pid, &status, 0) == pid && WIFEXITED(status)) {
            code = WEXITSTATUS(status);
        }
    }
    unlink(SHM "/a/f");
    rmdir(SHM "/a/f");
    unlink(SHM "/a/ro");
    rmdir(SHM "/a/ro");
    rmdir(SHM "/b/ro");
    unlink(SHM "/b/g");
    rmdir(SHM "/b/g");
    rmdir(SHM "/a");
    rmdir(SHM "/b");
    if (code != 0) {
        return fail(code == 2 ? EXCHANGE_DIR_WRITE : MOVE_DIR_WRITE);
    }
    return 0;
}

/**
 * Check renameat2's RENAME_EXCHANGE and RENAME_WHITEOUT, O_TMPFILE with the
 * two ways linkat names its file and through a link with a slash, O_DIRECT,
 * and the write permission a moved directory needs, in a directory of the
 * tmpfs of /dev/shm, which is removed again. Returns 0, or 1 once the
 * failure is reported.
 */
static int tmpfs_rows(void)
{
    int rc;

    rmdir(SHM);
    if (mkdir(SHM, 0755) != 0 || !make_file(AT_FDCWD, SHM "/p", "p") ||
        !make_file(AT_FDCWD, SHM "/q", "q") || mkdir(SHM "/d", 0755) != 0) {
        return fail(SHM_DIR);
    }
    rc = shm_rename_rows();
    if (rc == 0) {
        rc = shm_tmpfile_rows();
    }
    if (rc == 0) {
        rc = shm_tmpfile_refusals();
    }
    if (rc == 0) {
        rc = shm_direct_rows();
    }
    if (rc == 0) {
        rc = shm_tmpfile_slash();
    }
    if (rc == 0) {
        rc = shm_dir_write_rows();
    }
    unlink(SHM "/named");
    unlink(SHM "/named2");
    unlink(SHM "/q");
    unlink(SHM "/r");
    unlink(SHM "/d");
    rmdir(SHM "/p");
    rmdir(SHM);
    return rc;
}

int main(void)
{
    int dirfd, filefd;
    int rc;

    report_begin(report_case(environ, "guest.default.atcallmusl"));
    rmdir(WORK);
    if (mkdir(WORK, 0755) != 0) {
        return fail(WORKDIR);
    }
    dirfd = open(WORK, O_RDONLY | O_DIRECTORY);
    filefd = open("/hello.txt", O_RDONLY);
    if (dirfd < 0 || filefd < 0) {
        return fail(DESCRIPTORS);
    }
    rc = mkdir_unlink_rows(dirfd, filefd);
    if (rc == 0) {
        rc = rename_rows(dirfd);
    }
    if (rc == 0) {
        rc = fchdir_rows(filefd);
    }
    if (rc == 0) {
        rc = tmpfs_rows();
    }
    chdir("/");
    close(dirfd);
    close(filefd);
    rmdir(WORK);
    if (rc == 0) {
        report_at(CHECKS);
        printf("ATCALLMUSL OK\n");
    }
    return rc;
}
