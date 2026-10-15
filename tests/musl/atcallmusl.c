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
 * the result back with getcwd.
 *
 * Prints `ATCALLMUSL OK` and exits 0 on success, and prints
 * `ATCALLMUSL FAIL (detail)` and exits 1 otherwise.
 */

#define _GNU_SOURCE
#include <errno.h>
#include <fcntl.h>
#include <stdio.h>
#include <string.h>
#include <sys/stat.h>
#include <sys/syscall.h>
#include <unistd.h>

/* The renameat2 flags, which musl does not declare. */
#define RENAME_NOREPLACE 1
#define RENAME_EXCHANGE  2
#define RENAME_WHITEOUT  4

#define WORK "/atcall"

/** Report one failed check and return 1, the probe's failure status. */
static int fail(const char *what)
{
    printf("ATCALLMUSL FAIL (%s)\n", what);

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
        return fail("mkdirat of a relative name");
    }
    if (stat(WORK "/sub", &st) != 0 || (st.st_mode & 0777) != 0700) {
        return fail("mkdirat does not keep the mode");
    }
    if (!refused(mkdirat(dirfd, "sub", 0755), EEXIST)) {
        return fail("mkdirat of a name that exists is not EEXIST");
    }
    if (!refused(mkdirat(999, "x", 0755), EBADF)) {
        return fail("mkdirat through a descriptor that is not open is not EBADF");
    }
    if (!refused(mkdirat(filefd, "x", 0755), ENOTDIR)) {
        return fail("mkdirat through a file is not ENOTDIR");
    }
    if (mkdirat(999, WORK "/abs", 0755) != 0 || !is_dir(WORK "/abs")) {
        return fail("mkdirat of an absolute path reads the descriptor");
    }
    if (!make_file(dirfd, "sub/f", "f")) {
        return fail("a file in the new directory");
    }
    if (!refused(unlinkat(dirfd, "sub", AT_REMOVEDIR), ENOTEMPTY)) {
        return fail("unlinkat AT_REMOVEDIR of a directory that has a file is not ENOTEMPTY");
    }
    if (!refused(unlinkat(dirfd, "sub", 0), EISDIR)) {
        return fail("unlinkat of a directory without AT_REMOVEDIR is not EISDIR");
    }
    if (!refused(unlinkat(dirfd, "sub/f", AT_REMOVEDIR), ENOTDIR)) {
        return fail("unlinkat AT_REMOVEDIR of a file is not ENOTDIR");
    }
    if (!refused(unlinkat(dirfd, "sub/f", 0x1), EINVAL)) {
        return fail("unlinkat with a flag it does not take is not EINVAL");
    }
    if (unlinkat(dirfd, "sub/f", 0) != 0 || access(WORK "/sub/f", F_OK) == 0) {
        return fail("unlinkat of a file");
    }
    if (unlinkat(dirfd, "sub", AT_REMOVEDIR) != 0 || access(WORK "/sub", F_OK) == 0) {
        return fail("unlinkat AT_REMOVEDIR of an empty directory");
    }
    if (unlinkat(AT_FDCWD, WORK "/abs", AT_REMOVEDIR) != 0) {
        return fail("unlinkat AT_REMOVEDIR of an absolute path");
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
        return fail("the rename directories");
    }
    afd = openat(dirfd, "a", O_RDONLY | O_DIRECTORY);
    bfd = openat(dirfd, "b", O_RDONLY | O_DIRECTORY);
    if (afd < 0 || bfd < 0) {
        rc = fail("the rename directories' descriptors");
    } else if (!make_file(afd, "f", "f") || !make_file(afd, "x", "x") ||
               !make_file(bfd, "y", "y")) {
        rc = fail("the files to rename");
    } else if (renameat(afd, "f", bfd, "g") != 0 || !has_text(WORK "/b/g", "f") ||
               access(WORK "/a/f", F_OK) == 0) {
        rc = fail("renameat between two descriptors");
    } else if (!refused(syscall(SYS_renameat2, afd, "x", bfd, "y", RENAME_NOREPLACE), EEXIST)) {
        rc = fail("renameat2 RENAME_NOREPLACE onto a name that exists is not EEXIST");
    } else if (!has_text(WORK "/a/x", "x") || !has_text(WORK "/b/y", "y")) {
        rc = fail("a refused RENAME_NOREPLACE changed a file");
    } else if (!refused(syscall(SYS_renameat2, afd, "gone", bfd, "y", RENAME_NOREPLACE), ENOENT)) {
        rc = fail("renameat2 RENAME_NOREPLACE of a missing name is not ENOENT");
    } else if (syscall(SYS_renameat2, afd, "x", bfd, "z", RENAME_NOREPLACE) != 0 ||
               !has_text(WORK "/b/z", "x") || access(WORK "/a/x", F_OK) == 0) {
        rc = fail("renameat2 RENAME_NOREPLACE onto a free name");
    } else if (syscall(SYS_renameat2, bfd, "z", bfd, "y", 0) != 0 || !has_text(WORK "/b/y", "x")) {
        rc = fail("renameat2 without flags does not replace");
    } else if (!refused(syscall(SYS_renameat2, bfd, "y", bfd, "w", RENAME_EXCHANGE), EINVAL) ||
               !refused(syscall(SYS_renameat2, bfd, "y", bfd, "w", RENAME_WHITEOUT), EINVAL) ||
               !refused(syscall(SYS_renameat2, bfd, "y", bfd, "w", 8), EINVAL)) {
        rc = fail("renameat2 with a flag other than RENAME_NOREPLACE is not EINVAL");
    } else if (!refused(renameat(999, "y", bfd, "w"), EBADF)) {
        rc = fail("renameat through a descriptor that is not open is not EBADF");
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
        return fail("fchdir to a directory of the root");
    }
    if (!make_file(AT_FDCWD, "rel", "rel") || !has_text(WORK "/rel", "rel")) {
        return fail("a relative name after fchdir");
    }
    unlink(WORK "/rel");
    if (!enters("/dev", "/dev")) {
        return fail("fchdir to /dev");
    }
    if (!enters("/proc/self", self)) {
        return fail("fchdir to /proc/self");
    }
    if (!refused(fchdir(filefd), ENOTDIR)) {
        return fail("fchdir to a file is not ENOTDIR");
    }
    if (!refused(fchdir(999), EBADF)) {
        return fail("fchdir through a descriptor that is not open is not EBADF");
    }
    chdir("/");
    return 0;
}

int main(void)
{
    int dirfd, filefd;
    int rc;

    rmdir(WORK);
    if (mkdir(WORK, 0755) != 0) {
        return fail("the work directory");
    }
    dirfd = open(WORK, O_RDONLY | O_DIRECTORY);
    filefd = open("/hello.txt", O_RDONLY);
    if (dirfd < 0 || filefd < 0) {
        return fail("the descriptors the rows use");
    }
    rc = mkdir_unlink_rows(dirfd, filefd);
    if (rc == 0) {
        rc = rename_rows(dirfd);
    }
    if (rc == 0) {
        rc = fchdir_rows(filefd);
    }
    chdir("/");
    close(dirfd);
    close(filefd);
    rmdir(WORK);
    if (rc == 0) {
        printf("ATCALLMUSL OK\n");
    }
    return rc;
}
