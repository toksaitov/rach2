/**
 * proclinkmusl: check that a path goes on past /proc/PID/fd/N, cwd and root,
 * and past /dev/fd/N, from the file or directory the link refers to, wherever
 * the link is in the path, as Linux's walk goes on from it.
 *
 * In a tmpfs it mounts, the probe opens a directory and, through
 * /proc/self/fd/N and /dev/fd/N, looks a file up, creates one, makes a
 * directory, renames a file into it and unlinks it; "." and ".." after the
 * link are the directory and its parent. The directory is then renamed: the
 * names through the descriptor still reach it while its old path names
 * nothing, chdir through the descriptor enters it, and a file opened through
 * it has a /proc link that names the file by its new path. A directory
 * removed while open is still a directory with no names and no links, takes
 * no new name, and `..` climbs from it as the working directory. A child's
 * working directory and root are reached through /proc/PID/cwd and root, and
 * ".." after cwd climbs from the directory. A child of uid 1000 holds a
 * directory open: root and another process of uid 1000 reach it through
 * /proc/PID/fd/N, and a process of uid 1001 is refused with EACCES. A
 * descriptor on a file or a pipe used as a directory is ENOTDIR, a closed one
 * ENOENT, and a descriptor on /dev reaches its devices. The links of a
 * directory and a file whose paths are longer than 64 bytes name them
 * whole, also for a directory opened with O_PATH through another's link.
 *
 * Prints "PROCLINKMUSL OK" on success and "PROCLINKMUSL FAIL (detail)"
 * otherwise. Uids 1000 and 1001 are the NEW of each when a native run gives
 * `ids=OLD:NEW,...` (tests/native/Readme.md).
 */

#define _GNU_SOURCE
#include <errno.h>
#include <fcntl.h>
#include <stdio.h>
#include <string.h>
#include <sys/mount.h>
#include <sys/stat.h>
#include <sys/wait.h>
#include <unistd.h>

#include "../probe_args.h"

#define TOP "/tmp/proclinkmusl"

/** Report one failed check and return 1, the probe's failure status. */
static int fail(const char *what)
{
    printf("PROCLINKMUSL FAIL (%s: errno %d)\n", what, errno);
    return 1;
}

/** Report a child that failed at `step`, its exit status (-1 for one that
 * did not exit), and return 1. */
static int fail_step(const char *what, int step)
{
    printf("PROCLINKMUSL FAIL (%s: step %d)\n", what, step);
    return 1;
}

/** Whether a call that returned `rc` failed with `want`. */
static int refused(int rc, int want)
{
    return rc == -1 && errno == want;
}

/** Whether the file at `path` has exactly the bytes of `want`, at most 32. */
static int holds(const char *path, const char *want)
{
    char buf[32];
    int fd = open(path, O_RDONLY);
    ssize_t n;

    if (fd < 0) {
        return 0;
    }
    n = read(fd, buf, sizeof(buf));
    close(fd);
    return n == (ssize_t)strlen(want) && memcmp(buf, want, (size_t)n) == 0;
}

/** Write `text` to the file at `path`, created or truncated; 1, or 0. */
static int put(const char *path, const char *text)
{
    int fd = open(path, O_WRONLY | O_CREAT | O_TRUNC, 0644);
    int ok = fd >= 0 && write(fd, text, strlen(text)) == (ssize_t)strlen(text);

    if (fd >= 0) {
        close(fd);
    }
    return ok;
}

/** Whether `a` and `b` name one file. */
static int same_file(const char *a, const char *b)
{
    struct stat sa, sb;

    return stat(a, &sa) == 0 && stat(b, &sb) == 0 && sa.st_dev == sb.st_dev &&
           sa.st_ino == sb.st_ino;
}

/** Whether `path` names a directory. */
static int is_dir(const char *path)
{
    struct stat st;

    return stat(path, &st) == 0 && S_ISDIR(st.st_mode);
}

/** Whether the working directory is `want`. */
static int cwd_is(const char *want)
{
    char buf[64];

    return getcwd(buf, sizeof(buf)) != NULL && strcmp(buf, want) == 0;
}

/** The exit status of the child `pid`, or -1 for one that did not exit. */
static int child_status(pid_t pid)
{
    int status;

    if (waitpid(pid, &status, 0) != pid || !WIFEXITED(status)) {
        return -1;
    }
    return WEXITSTATUS(status);
}

/** The path `head`/`n`/`rest` in `buf` of 96 bytes, which it returns:
 * "/proc/self/fd", 3 and "f" make "/proc/self/fd/3/f". */
static const char *at(char *buf, const char *head, int n, const char *rest)
{
    snprintf(buf, 96, "%s/%d/%s", head, n, rest);
    return buf;
}

/**
 * Check lookup, create, mkdir, rename and unlink through the descriptor `d`
 * on TOP/d, by /proc/self/fd/N and by /dev/fd/N, and "." and ".." after the
 * link. Returns 0, or 1 after printing the failure.
 */
static int through(int d)
{
    char a[96], b[96];

    if (!holds(at(a, "/proc/self/fd", d, "f"), "one") || !holds(at(a, "/dev/fd", d, "f"), "one")) {
        return fail("a lookup through the link");
    }
    if (!same_file(at(a, "/proc/self/fd", d, "."), TOP "/d") ||
        !same_file(at(a, "/proc/self/fd", d, ".."), TOP)) {
        return fail("dots after the link");
    }
    if (!put(at(a, "/proc/self/fd", d, "new"), "two") || !holds(TOP "/d/new", "two")) {
        return fail("a create through the link");
    }
    if (mkdir(at(a, "/proc/self/fd", d, "sub"), 0755) != 0 || !is_dir(TOP "/d/sub")) {
        return fail("a mkdir through the link");
    }
    if (rename(at(a, "/proc/self/fd", d, "new"), at(b, "/dev/fd", d, "sub/moved")) != 0 ||
        !holds(TOP "/d/sub/moved", "two") || !refused(access(TOP "/d/new", F_OK), ENOENT)) {
        return fail("a rename through the link");
    }
    if (unlink(at(a, "/proc/self/fd", d, "sub/moved")) != 0 ||
        !refused(access(TOP "/d/sub/moved", F_OK), ENOENT)) {
        return fail("an unlink through the link");
    }
    return 0;
}

/**
 * Rename TOP/d, which `d` is open on, to TOP/d2, and check that the names
 * through `d` reach the directory while its old path names nothing; that
 * chdir through `d` enters it under its new path; and that a file opened
 * through `d` has a /proc link that names it by its new path. Returns 0, or
 * 1 after printing the failure.
 */
static int renamed(int d)
{
    char a[96], target[64];
    int f;
    ssize_t n;

    if (rename(TOP "/d", TOP "/d2") != 0) {
        return fail("the rename of the directory");
    }
    if (!holds(at(a, "/proc/self/fd", d, "f"), "one") ||
        !refused(access(TOP "/d/f", F_OK), ENOENT)) {
        return fail("a lookup through the link to a renamed directory");
    }
    if (mkdir(at(a, "/proc/self/fd", d, "sub2"), 0755) != 0 || !is_dir(TOP "/d2/sub2") ||
        !same_file(at(a, "/proc/self/fd", d, ".."), TOP)) {
        return fail("a mkdir through the link to a renamed directory");
    }
    if (chdir(at(a, "/proc/self/fd", d, "sub2")) != 0 || !cwd_is(TOP "/d2/sub2") ||
        chdir("/") != 0) {
        return fail("a chdir through the link");
    }
    f = open(at(a, "/proc/self/fd", d, "f"), O_RDONLY);
    if (f < 0) {
        return fail("an open through the link");
    }
    snprintf(a, sizeof(a), "/proc/self/fd/%d", f);
    n = readlink(a, target, sizeof(target));
    close(f);
    if (n != (ssize_t)strlen(TOP "/d2/f") || memcmp(target, TOP "/d2/f", (size_t)n) != 0) {
        return fail("the link of a file opened through a link");
    }
    return 0;
}

/** Whether the /proc link of the descriptor `fd` reads `want`. */
static int link_is(int fd, const char *want)
{
    char a[96], target[256];
    ssize_t n;

    snprintf(a, sizeof(a), "/proc/self/fd/%d", fd);
    n = readlink(a, target, sizeof(target));
    return n == (ssize_t)strlen(want) && memcmp(target, want, (size_t)n) == 0;
}

/**
 * Check that the /proc links of a directory and a file whose paths are
 * longer than 64 bytes name them whole, as Linux's do: the directory opened
 * with O_PATH, a directory made through its link and opened with O_PATH
 * through it, as a program that pins a path by its directories does, and a
 * file made there through that one's link. Returns 0, or 1 after printing
 * the failure.
 */
static int long_paths(void)
{
    char dir[160], sub[176], file[192], a[96];
    int d, s, f, bad;

    snprintf(dir, sizeof(dir), "%s/%s", TOP,
             "0123456789abcdef0123456789abcdef0123456789abcdef0123456789abcdef");
    snprintf(sub, sizeof(sub), "%s/sub", dir);
    snprintf(file, sizeof(file), "%s/file", sub);
    if (mkdir(dir, 0755) != 0 || (d = open(dir, O_PATH | O_DIRECTORY)) < 0) {
        return fail("the directory of a long path");
    }
    if (mkdir(at(a, "/proc/self/fd", d, "sub"), 0755) != 0 ||
        (s = open(a, O_PATH | O_DIRECTORY | O_NOFOLLOW)) < 0) {
        close(d);
        return fail("a directory of a long path made through the link");
    }
    f = open(at(a, "/proc/self/fd", s, "file"), O_WRONLY | O_CREAT | O_EXCL, 0644);
    bad = !link_is(d, dir) || !link_is(s, sub) || f < 0 || !link_is(f, file);
    if (f >= 0) {
        close(f);
    }
    close(s);
    close(d);
    if (bad) {
        return fail("the link of a long path");
    }
    if (unlink(file) != 0 || rmdir(sub) != 0 || rmdir(dir) != 0) {
        return fail("the removal of the long path");
    }
    return 0;
}

/**
 * Check that a directory removed while a descriptor is open on it is still a
 * directory through the descriptor, with no names in it and no links, and
 * that no name is made in it: a create through /proc/self/fd/N or by openat
 * on the descriptor and a mkdir through the link are each ENOENT, as on
 * Linux. A working directory removed under the probe takes no create either,
 * and `..` from it climbs to TOP. Returns 0, or 1 after printing the
 * failure.
 */
static int removed(void)
{
    struct stat st;
    char a[96];
    int e;

    if (mkdir(TOP "/e", 0755) != 0 || (e = open(TOP "/e", O_RDONLY | O_DIRECTORY)) < 0 ||
        rmdir(TOP "/e") != 0) {
        return fail("the removed directory");
    }
    if (!is_dir(at(a, "/proc/self/fd", e, ".")) ||
        !refused(open(at(a, "/proc/self/fd", e, "f"), O_RDONLY), ENOENT)) {
        close(e);
        return fail("a lookup through the link to a removed directory");
    }
    if (fstat(e, &st) != 0 || st.st_nlink != 0) {
        close(e);
        return fail("the links of a removed directory");
    }
    if (!refused(open(at(a, "/proc/self/fd", e, "f"), O_WRONLY | O_CREAT, 0644), ENOENT) ||
        !refused(openat(e, "f", O_WRONLY | O_CREAT, 0644), ENOENT) ||
        !refused(mkdir(at(a, "/proc/self/fd", e, "s"), 0755), ENOENT)) {
        close(e);
        return fail("a name made in a removed directory");
    }
    close(e);
    if (mkdir(TOP "/w", 0755) != 0 || chdir(TOP "/w") != 0 || rmdir(TOP "/w") != 0 ||
        !refused(open("f", O_WRONLY | O_CREAT, 0644), ENOENT) || chdir("..") != 0 || !cwd_is(TOP) ||
        chdir("/") != 0) {
        chdir("/");
        return fail("a removed working directory");
    }
    return 0;
}

/**
 * Check descriptors no path goes on past: on a file and on a pipe, ENOTDIR
 * for a lookup and a mkdir; a closed one, ENOENT. A descriptor on /dev, a
 * directory with no node, reaches /dev/null. Returns 0, or 1 after printing
 * the failure.
 */
static int not_dirs(void)
{
    char a[96];
    int p[2];
    int f = open(TOP "/plain", O_RDONLY);
    int dev = open("/dev", O_RDONLY | O_DIRECTORY);
    int null;

    if (f < 0 || dev < 0 || pipe(p) != 0) {
        return fail("the descriptors");
    }
    if (!refused(open(at(a, "/proc/self/fd", f, "x"), O_RDONLY), ENOTDIR) ||
        !refused(mkdir(at(a, "/proc/self/fd", f, "x"), 0755), ENOTDIR) ||
        !refused(open(at(a, "/dev/fd", f, "x"), O_RDONLY), ENOTDIR)) {
        return fail("a file used as a directory");
    }
    if (!refused(open(at(a, "/proc/self/fd", p[0], "x"), O_RDONLY), ENOTDIR)) {
        return fail("a pipe used as a directory");
    }
    if (!refused(open(at(a, "/proc/self/fd", 999, "x"), O_RDONLY), ENOENT)) {
        return fail("a closed descriptor");
    }
    null = open(at(a, "/proc/self/fd", dev, "null"), O_WRONLY);
    if (null < 0 || write(null, "x", 1) != 1) {
        return fail("a device through a link to /dev");
    }
    close(null);
    close(dev);
    close(f);
    close(p[0]);
    close(p[1]);
    return 0;
}

/**
 * Check /proc/PID/cwd and root of a child whose working directory is
 * TOP/d2/sub2 and whose root is TOP/d2: a file through each, and ".." after
 * cwd, which climbs from the directory. Two pipes hold the child in place
 * while the parent looks. Returns 0, or 1 after printing the failure.
 */
static int cwd_root(void)
{
    char a[96];
    int ready[2], go[2];
    char c = 0;
    pid_t pid;
    int ok;

    if (pipe(ready) != 0 || pipe(go) != 0) {
        return fail("the pipes");
    }
    pid = fork();
    if (pid == 0) {
        close(ready[0]);
        close(go[1]);
        if (chdir(TOP "/d2/sub2") != 0 || chroot(TOP "/d2") != 0) {
            _exit(2);
        }
        if (write(ready[1], &c, 1) != 1 || read(go[0], &c, 1) != 1) {
            _exit(3);
        }
        _exit(0);
    }
    close(ready[1]);
    close(go[0]);
    if (read(ready[0], &c, 1) != 1) {
        return fail("the child's start");
    }
    ok = holds(at(a, "/proc", pid, "cwd/../f"), "one") &&
         holds(at(a, "/proc", pid, "root/f"), "one") &&
         same_file(at(a, "/proc", pid, "root/sub2"), TOP "/d2/sub2");
    if (write(go[1], &c, 1) != 1 || child_status(pid) != 0 || !ok) {
        return fail("another process's cwd and root");
    }
    close(ready[0]);
    close(go[1]);
    if (chdir(TOP "/d2/sub2") != 0 || !holds("/proc/self/cwd/../f", "one") ||
        !holds("/proc/self/root" TOP "/d2/f", "one") || chdir("/") != 0) {
        return fail("the caller's cwd and root");
    }
    return 0;
}

/**
 * Fork a child that takes the uid and gid `id` and opens the file at `path`:
 * 0 when that succeeds with `want` 0 or fails with errno `want`, its step
 * otherwise.
 */
static int as_user(int id, const char *path, int want)
{
    pid_t pid = fork();
    int fd;

    if (pid == 0) {
        if (setgid((gid_t)id) != 0 || setuid((uid_t)id) != 0) {
            _exit(2);
        }
        fd = open(path, O_RDONLY);
        if (want == 0) {
            _exit(fd >= 0 ? 0 : 3);
        }
        _exit(refused(fd, want) ? 0 : 4);
    }
    return child_status(pid);
}

/**
 * Check another process's /proc/PID/fd/N: a child of uid 1000 holds TOP/d2
 * open, and root and a process of uid 1000 reach TOP/d2/f through its link,
 * where a process of uid 1001 is refused with EACCES. Returns 0, or 1 after
 * printing the failure.
 */
static int other_process(void)
{
    char a[96];
    int ready[2], go[2];
    char c = 0;
    pid_t pid;
    int d = -1, st;

    if (pipe(ready) != 0 || pipe(go) != 0) {
        return fail("the pipes");
    }
    pid = fork();
    if (pid == 0) {
        close(ready[0]);
        close(go[1]);
        if (setgid((gid_t)probe_id(1000)) != 0 || setuid((uid_t)probe_id(1000)) != 0 ||
            (d = open(TOP "/d2", O_RDONLY)) < 0) {
            _exit(2);
        }
        if (write(ready[1], &d, sizeof(d)) != sizeof(d) || read(go[0], &c, 1) != 1) {
            _exit(3);
        }
        _exit(0);
    }
    close(ready[1]);
    close(go[0]);
    if (read(ready[0], &d, sizeof(d)) != sizeof(d)) {
        return fail("the child's start");
    }
    snprintf(a, sizeof(a), "/proc/%d/fd/%d/f", (int)pid, d);
    if (!holds(a, "one")) {
        st = 10;
    } else {
        st = as_user((int)probe_id(1000), a, 0);
        if (st == 0) {
            st = as_user((int)probe_id(1001), a, EACCES);
            st = st == 0 ? 0 : 20 + st;
        }
    }
    if (write(go[1], &c, 1) != 1 || child_status(pid) != 0) {
        return fail("the holder of the descriptor");
    }
    close(ready[0]);
    close(go[1]);
    return st == 0 ? 0 : fail_step("another process's descriptor", st);
}

int main(int argc, char **argv)
{
    int d;
    int bad;

    probe_args(argc, argv);

    mkdir(TOP, 0755);
    if (mount("none", TOP, "tmpfs", 0, NULL) != 0 || chmod(TOP, 0755) != 0 ||
        mkdir(TOP "/d", 0755) != 0 || !put(TOP "/d/f", "one") || !put(TOP "/plain", "x")) {
        return fail("the tmpfs");
    }
    d = open(TOP "/d", O_RDONLY | O_DIRECTORY);
    if (d < 0) {
        return fail("the directory");
    }
    bad = through(d) || renamed(d) || removed() || not_dirs() || cwd_root() || other_process() ||
          long_paths();
    close(d);
    if (umount2(TOP, 0) != 0 || rmdir(TOP) != 0) {
        return fail("the tmpfs's removal");
    }
    if (bad) {
        return 1;
    }
    printf("PROCLINKMUSL OK\n");
    return 0;
}
