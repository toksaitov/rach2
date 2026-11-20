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
 * it has a /proc link that names the file by its new path. An open file
 * moved to another directory has a link that names its new path, and once
 * unlinked that path and " (deleted)", as Linux's `d_path` gives. A directory
 * removed while open is still a directory with no names and no links, takes
 * no new name, and `..` climbs from it as the working directory. A child's
 * working directory and root are reached through /proc/PID/cwd and root, and
 * ".." after cwd climbs from the directory. A child of uid 1000 holds a
 * directory open, dumpable again after its setuid: root and another process
 * of uid 1000 reach it through /proc/PID/fd/N, and a process of uid 1001 is
 * refused with EACCES. Its /proc/PID/fd, mode 0500 and owned by uid and
 * gid 1000, is entered by chdir and by fchdir through an O_PATH descriptor
 * of uid 1000 and gid 1001, as Linux's proc_fd_permission judges the owner
 * whatever its group, and that process lstats a link in it; uid 1001 is
 * refused with EACCES all three ways. Once the holder is gone, fchdir
 * through such a descriptor still enters its fd
 * directory and gives ESRCH for its fdinfo directory. A
 * descriptor on a file or a pipe used as a directory is ENOTDIR, a closed one
 * ENOENT, and a descriptor on /dev reaches its devices. The links of a
 * directory and a file whose paths are longer than 64 bytes name them
 * whole, also for a directory opened with O_PATH through another's link.
 * /proc/thread-self names the calling thread, in the main thread and in a
 * second one, and /proc lists it and /proc/self as links, each with an inode
 * of its own. Both links of a proc mount whose PID namespace does not see
 * the probe are links to lstat, to fstat through O_PATH and O_NOFOLLOW, and
 * ENOENT to readlink and to stat.
 *
 * Prints "PROCLINKMUSL OK" on success and "PROCLINKMUSL FAIL (detail)"
 * otherwise. Uids 1000 and 1001 are the NEW of each when a native run gives
 * `ids=OLD:NEW,...` (tests/native/Readme.md).
 *
 * etude's report (probe_report.h), for the case ETUDE_CASE names, has an
 * assertion for each row in order, a setup step joined to the row after it:
 * `fd-directory`, `lookup`, `dots`, `create`, `mkdir`, `rename`, `unlink`,
 * `renamed-lookup`,
 * `renamed-mkdir`, `renamed-chdir`, `renamed-file-link`, `moved-file-link`,
 * `deleted-file-link`, `removed-lookup`,
 * `removed-links`, `removed-create`, `removed-cwd`, `file-notdir`,
 * `pipe-notdir`, `closed-enoent`, `dev-through`, `cwd-root-other`,
 * `cwd-root-self`, `fd-other-root`, `fd-other-same-uid`, `fd-other-eacces`,
 * `fd-dir-owner`, `fd-dir-eacces`, `fd-dir-ended`, `long-link`,
 * `long-removal`, `thread-self`, `self-inodes`, `self-other-ns` and
 * `cleanup`. The first
 * failure fails its assertion and leaves the rest not run.
 */

#define _GNU_SOURCE
#include <dirent.h>
#include <errno.h>
#include <fcntl.h>
#include <pthread.h>
#include <sched.h>
#include <signal.h>
#include <stdio.h>
#include <string.h>
#include <sys/mount.h>
#include <sys/prctl.h>
#include <sys/stat.h>
#include <sys/syscall.h>
#include <sys/wait.h>
#include <unistd.h>

#include "../probe_args.h"
#include "../probe_report.h"

#define TOP "/tmp/proclinkmusl"

/** Report one failed check and return 1, the probe's failure status. */
static int fail(const char *what)
{
    char why[96];

    snprintf(why, sizeof(why), "%s: errno %d", what, errno);
    printf("PROCLINKMUSL FAIL (%s)\n", why);
    report_stop(why);
    return 1;
}

/** Report a child that failed at `step`, its exit status (-1 for one that
 * did not exit), and return 1. */
static int fail_step(const char *what, int step)
{
    char why[96];

    snprintf(why, sizeof(why), "%s: step %d", what, step);
    printf("PROCLINKMUSL FAIL (%s)\n", why);
    report_stop(why);
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
    report_held();
    if (!same_file(at(a, "/proc/self/fd", d, "."), TOP "/d") ||
        !same_file(at(a, "/proc/self/fd", d, ".."), TOP)) {
        return fail("dots after the link");
    }
    report_held();
    if (!put(at(a, "/proc/self/fd", d, "new"), "two") || !holds(TOP "/d/new", "two")) {
        return fail("a create through the link");
    }
    report_held();
    if (mkdir(at(a, "/proc/self/fd", d, "sub"), 0755) != 0 || !is_dir(TOP "/d/sub")) {
        return fail("a mkdir through the link");
    }
    report_held();
    if (rename(at(a, "/proc/self/fd", d, "new"), at(b, "/dev/fd", d, "sub/moved")) != 0 ||
        !holds(TOP "/d/sub/moved", "two") || !refused(access(TOP "/d/new", F_OK), ENOENT)) {
        return fail("a rename through the link");
    }
    report_held();
    if (unlink(at(a, "/proc/self/fd", d, "sub/moved")) != 0 ||
        !refused(access(TOP "/d/sub/moved", F_OK), ENOENT)) {
        return fail("an unlink through the link");
    }
    report_held();
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
    report_held();
    if (mkdir(at(a, "/proc/self/fd", d, "sub2"), 0755) != 0 || !is_dir(TOP "/d2/sub2") ||
        !same_file(at(a, "/proc/self/fd", d, ".."), TOP)) {
        return fail("a mkdir through the link to a renamed directory");
    }
    report_held();
    if (chdir(at(a, "/proc/self/fd", d, "sub2")) != 0 || !cwd_is(TOP "/d2/sub2") ||
        chdir("/") != 0) {
        return fail("a chdir through the link");
    }
    report_held();
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
    report_held();
    if (!put(TOP "/d2/named", "n") || (f = open(TOP "/d2/named", O_RDONLY)) < 0) {
        return fail("the file to move");
    }
    if (!link_is(f, TOP "/d2/named") || rename(TOP "/d2/named", TOP "/moved") != 0 ||
        !link_is(f, TOP "/moved")) {
        close(f);
        return fail("the link of an open file moved to another directory");
    }
    report_held();
    if (unlink(TOP "/moved") != 0 || !link_is(f, TOP "/moved (deleted)")) {
        close(f);
        return fail("the link of an open file unlinked");
    }
    close(f);
    report_held();
    return 0;
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
    report_held();
    if (unlink(file) != 0 || rmdir(sub) != 0 || rmdir(dir) != 0) {
        return fail("the removal of the long path");
    }
    report_held();
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
    report_held();
    if (fstat(e, &st) != 0 || st.st_nlink != 0) {
        close(e);
        return fail("the links of a removed directory");
    }
    report_held();
    if (!refused(open(at(a, "/proc/self/fd", e, "f"), O_WRONLY | O_CREAT, 0644), ENOENT) ||
        !refused(openat(e, "f", O_WRONLY | O_CREAT, 0644), ENOENT) ||
        !refused(mkdir(at(a, "/proc/self/fd", e, "s"), 0755), ENOENT)) {
        close(e);
        return fail("a name made in a removed directory");
    }
    report_held();
    close(e);
    if (mkdir(TOP "/w", 0755) != 0 || chdir(TOP "/w") != 0 || rmdir(TOP "/w") != 0 ||
        !refused(open("f", O_WRONLY | O_CREAT, 0644), ENOENT) || chdir("..") != 0 || !cwd_is(TOP) ||
        chdir("/") != 0) {
        chdir("/");
        return fail("a removed working directory");
    }
    report_held();
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
    report_held();
    if (!refused(open(at(a, "/proc/self/fd", p[0], "x"), O_RDONLY), ENOTDIR)) {
        return fail("a pipe used as a directory");
    }
    report_held();
    if (!refused(open(at(a, "/proc/self/fd", 999, "x"), O_RDONLY), ENOENT)) {
        return fail("a closed descriptor");
    }
    report_held();
    null = open(at(a, "/proc/self/fd", dev, "null"), O_WRONLY);
    if (null < 0 || write(null, "x", 1) != 1) {
        return fail("a device through a link to /dev");
    }
    report_held();
    close(null);
    close(dev);
    close(f);
    close(p[0]);
    close(p[1]);
    return 0;
}

/**
 * Release and reap the held child `pid`: close `ready`, the read end of the
 * pipe it signals on, and `go`, the write end of the one it waits on, so that
 * a child still waiting reads end of file and exits. Returns the child's exit
 * status, or -1 when `pid` is a failed fork's or the child did not exit.
 */
static int let_go(pid_t pid, int ready, int go)
{
    close(ready);
    close(go);
    return pid > 0 ? child_status(pid) : -1;
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
    if (pid < 0 || read(ready[0], &c, 1) != 1) {
        let_go(pid, ready[0], go[1]);
        return fail("the child's start");
    }
    ok = holds(at(a, "/proc", pid, "cwd/../f"), "one") &&
         holds(at(a, "/proc", pid, "root/f"), "one") &&
         same_file(at(a, "/proc", pid, "root/sub2"), TOP "/d2/sub2");
    ok = write(go[1], &c, 1) == 1 && ok;
    if (let_go(pid, ready[0], go[1]) != 0 || !ok) {
        return fail("another process's cwd and root");
    }
    report_held();
    if (chdir(TOP "/d2/sub2") != 0 || !holds("/proc/self/cwd/../f", "one") ||
        !holds("/proc/self/root" TOP "/d2/f", "one") || chdir("/") != 0) {
        return fail("the caller's cwd and root");
    }
    report_held();
    return 0;
}

/**
 * Fork a child that takes the uid and gid `id` and opens the file at `path`:
 * 0 when that succeeds with `want` 0 or fails with errno `want`, 1 when the
 * child cannot be made, its step otherwise.
 */
static int as_user(int id, const char *path, int want)
{
    pid_t pid = fork();
    int fd;

    if (pid < 0) {
        return 1;
    }
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
 * Check another process's /proc/PID/fd/N: a child of uid 1000, dumpable
 * again after its setuid, which clears the flag as Linux's commit_creds
 * does, holds TOP/d2 open, and root and a process of uid 1000 reach TOP/d2/f
 * through its link,
 * where a process of uid 1001 is refused with EACCES. Returns 0, or 1 after
 * printing the failure; the step that failed says which of the three it is.
 */
static int other_process(void)
{
    char a[96];
    int ready[2], go[2];
    char c = 0;
    pid_t pid;
    int d = -1, st, sent;

    if (pipe(ready) != 0 || pipe(go) != 0) {
        return fail("the pipes");
    }
    pid = fork();
    if (pid == 0) {
        close(ready[0]);
        close(go[1]);
        if (setgid((gid_t)probe_id(1000)) != 0 || setuid((uid_t)probe_id(1000)) != 0 ||
            prctl(PR_SET_DUMPABLE, 1, 0, 0, 0) != 0 || (d = open(TOP "/d2", O_RDONLY)) < 0) {
            _exit(2);
        }
        if (write(ready[1], &d, sizeof(d)) != sizeof(d) || read(go[0], &c, 1) != 1) {
            _exit(3);
        }
        _exit(0);
    }
    close(ready[1]);
    close(go[0]);
    if (pid < 0 || read(ready[0], &d, sizeof(d)) != sizeof(d)) {
        let_go(pid, ready[0], go[1]);
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
    sent = write(go[1], &c, 1) == 1;
    if (let_go(pid, ready[0], go[1]) != 0 || !sent) {
        return fail("the holder of the descriptor");
    }
    if (st != 10) {
        report_held();
    }
    if (st == 0 || st > 10) {
        report_held();
    }
    if (st == 0) {
        report_held();
    }
    return st == 0 ? 0 : fail_step("another process's descriptor", st);
}

/**
 * In a child of uid `uid` and gid `gid`, enter `dir`, another process's
 * /proc/PID/fd, by chdir and by fchdir through an O_PATH descriptor, and
 * lstat its link `fd`: 0 when all three succeed with `want` 0 or fail with
 * errno `want`, 1 when the child cannot be made, its step otherwise. The
 * lstat takes only the directory's check, as Linux's link inode has no
 * permission of its own.
 */
static int fd_dir_enter(int uid, int gid, const char *dir, int fd, int want)
{
    char link[96];
    struct stat st;
    pid_t pid = fork();
    int o, rc;

    if (pid < 0) {
        return 1;
    }
    if (pid == 0) {
        if (setgid((gid_t)gid) != 0 || setuid((uid_t)uid) != 0) {
            _exit(2);
        }
        o = open(dir, O_PATH | O_DIRECTORY);
        if (o < 0) {
            _exit(3);
        }
        rc = chdir(dir);
        if (want == 0 ? rc != 0 : !refused(rc, want)) {
            _exit(4);
        }
        rc = fchdir(o);
        if (want == 0 ? rc != 0 : !refused(rc, want)) {
            _exit(want == 0 ? 5 : 6);
        }
        snprintf(link, sizeof(link), "%s/%d", dir, fd);
        rc = lstat(link, &st);
        if (want == 0) {
            _exit(rc == 0 && S_ISLNK(st.st_mode) ? 0 : 7);
        }
        _exit(refused(rc, want) ? 0 : 8);
    }
    return child_status(pid);
}

/**
 * In a child of uid 1000 and gid 1001, open O_PATH descriptors of `dir` and
 * `info`, the fd and fdinfo directories of the held child `owner`, then
 * release and reap `owner` (`let_go` on `ready` and `go`) and fchdir through
 * each: 0 when the fd directory is entered and the fdinfo one is ESRCH, 40
 * plus the child's step otherwise, or -1 when the child could not be made or
 * `owner` did not end well. `owner` is released and reaped, and `ready` and
 * `go` closed, on every path.
 */
static int fd_dir_ended(pid_t owner, int ready, int go, const char *dir, const char *info)
{
    int seen[2], gone[2];
    char c = 0;
    pid_t late;
    int st, fo, io, opened, ended;

    if (pipe(seen) != 0) {
        let_go(owner, ready, go);
        return -1;
    }
    if (pipe(gone) != 0) {
        close(seen[0]);
        close(seen[1]);
        let_go(owner, ready, go);
        return -1;
    }
    late = fork();
    if (late == 0) {
        close(ready);
        close(go);
        close(seen[0]);
        close(gone[1]);
        if (setgid((gid_t)probe_id(1001)) != 0 || setuid((uid_t)probe_id(1000)) != 0) {
            _exit(2);
        }
        fo = open(dir, O_PATH | O_DIRECTORY);
        io = open(info, O_PATH | O_DIRECTORY);
        if (fo < 0 || io < 0 || write(seen[1], &c, 1) != 1 || read(gone[0], &c, 1) != 1) {
            _exit(3);
        }
        if (fchdir(fo) != 0) {
            _exit(4);
        }
        _exit(refused(fchdir(io), ESRCH) ? 0 : 5);
    }
    close(seen[1]);
    close(gone[0]);
    opened = late > 0 && read(seen[0], &c, 1) == 1;
    ended = write(go, &c, 1) == 1;
    ended = let_go(owner, ready, go) == 0 && ended;
    if (opened && ended && write(gone[1], &c, 1) != 1) {
        ended = 0;
    }
    close(seen[0]);
    close(gone[1]);
    st = late > 0 ? child_status(late) : -1;
    if (late < 0 || !ended) {
        return -1;
    }
    return st == 0 ? 0 : 40 + st;
}

/**
 * Check the search of another process's /proc/PID/fd, mode 0500, whose
 * owner is a dumpable child of uid and gid 1000: a process of uid 1000 and
 * gid 1001 enters it by chdir and by fchdir, as Linux's proc_fd_permission
 * judges the owner whatever its group, and lstats the link of the child's
 * descriptor `go[0]`, and one of uid 1001 gets EACCES all three ways; and
 * once the child is reaped, the fd directory is entered and the
 * fdinfo one is ESRCH through descriptors opened before (`fd_dir_ended`).
 * Returns 0, or 1 after printing the failure; the step says which check
 * failed.
 */
static int fd_dir_search(void)
{
    char dir[64], info[64];
    int ready[2], go[2];
    char c = 0;
    pid_t pid;
    int st, sent;

    if (pipe(ready) != 0 || pipe(go) != 0) {
        return fail("the pipes of the fd directory's search");
    }
    pid = fork();
    if (pid == 0) {
        close(ready[0]);
        close(go[1]);
        if (setgid((gid_t)probe_id(1000)) != 0 || setuid((uid_t)probe_id(1000)) != 0 ||
            prctl(PR_SET_DUMPABLE, 1, 0, 0, 0) != 0 || write(ready[1], &c, 1) != 1 ||
            read(go[0], &c, 1) != 1) {
            _exit(2);
        }
        _exit(0);
    }
    close(ready[1]);
    close(go[0]);
    if (pid < 0 || read(ready[0], &c, 1) != 1) {
        let_go(pid, ready[0], go[1]);
        return fail("the fd directory's owner");
    }
    snprintf(dir, sizeof(dir), "/proc/%d/fd", (int)pid);
    snprintf(info, sizeof(info), "/proc/%d/fdinfo", (int)pid);
    st = fd_dir_enter((int)probe_id(1000), (int)probe_id(1001), dir, go[0], 0);
    if (st == 0) {
        report_held();
        st = fd_dir_enter((int)probe_id(1001), (int)probe_id(1001), dir, go[0], EACCES);
        st = st == 0 ? 0 : 20 + st;
    }
    if (st == 0) {
        report_held();
        st = fd_dir_ended(pid, ready[0], go[1], dir, info);
    } else {
        sent = write(go[1], &c, 1) == 1;
        if (let_go(pid, ready[0], go[1]) != 0 || !sent) {
            st = -1;
        }
    }
    if (st < 0) {
        return fail("the fd directory's owner's end");
    }
    if (st == 0) {
        report_held();
    }
    return st == 0 ? 0 : fail_step("another process's fd directory", st);
}

/**
 * Check /proc/thread-self from the calling thread: its text is
 * "PID/task/TID", unfollowed it is a link with an inode of its own,
 * followed it is /proc/self/task/TID, and its stat file is the thread's.
 * Returns 0, or the number of the first check that failed.
 */
static int thread_self_of_caller(void)
{
    char want[48], text[48], dir[48];
    struct stat lst, proc;
    long tid = syscall(SYS_gettid);
    ssize_t n;
    int fd;

    snprintf(want, sizeof(want), "%d/task/%ld", (int)getpid(), tid);
    n = readlink("/proc/thread-self", text, sizeof(text));
    if (n != (ssize_t)strlen(want) || memcmp(text, want, (size_t)n) != 0) {
        return 1;
    }
    if (lstat("/proc/thread-self", &lst) != 0 || !S_ISLNK(lst.st_mode) ||
        stat("/proc", &proc) != 0 || lst.st_ino == proc.st_ino) {
        return 2;
    }
    snprintf(dir, sizeof(dir), "/proc/self/task/%ld", tid);
    if (!is_dir("/proc/thread-self") || !same_file("/proc/thread-self", dir)) {
        return 3;
    }
    snprintf(want, sizeof(want), "%ld (", tid);
    fd = open("/proc/thread-self/stat", O_RDONLY);
    n = fd < 0 ? -1 : read(fd, text, sizeof(text));
    if (fd >= 0) {
        close(fd);
    }
    if (n < (ssize_t)strlen(want) || memcmp(text, want, strlen(want)) != 0) {
        return 4;
    }
    return 0;
}

/** The second thread of `thread_self`: its checks' result as a pointer. */
static void *thread_self_thread(void *arg)
{
    (void)arg;
    return (void *)(long)thread_self_of_caller();
}

/**
 * Check /proc/thread-self from the main thread, whose thread ID is the
 * process's, and from a second thread, whose is its own, as Linux's
 * fs/proc/thread_self.c gives the link. Returns 0, or 1 after printing the
 * failure.
 */
static int thread_self(void)
{
    pthread_t t;
    void *res = (void *)-1L;
    int st = thread_self_of_caller();

    if (st == 0 && (pthread_create(&t, NULL, thread_self_thread, NULL) != 0 ||
                    pthread_join(t, &res) != 0 || res != NULL)) {
        st = res == (void *)-1L ? 9 : 10 + (int)(long)res;
    }
    if (st != 0) {
        return fail_step("/proc/thread-self", st);
    }
    report_held();
    return 0;
}

/**
 * Check that /proc lists `self` and `thread-self` as links, each with the
 * inode lstat gives it, which is neither /proc's nor the other's, as Linux's
 * self_inum and thread_self_inum are each a number of its own. Returns 0, or
 * 1 after printing the failure.
 */
static int self_inodes(void)
{
    struct stat proc, self, thread;
    struct dirent *e;
    DIR *d;
    int seen = 0;

    if (stat("/proc", &proc) != 0 || lstat("/proc/self", &self) != 0 ||
        lstat("/proc/thread-self", &thread) != 0 || self.st_ino == proc.st_ino ||
        thread.st_ino == proc.st_ino || self.st_ino == thread.st_ino) {
        return fail("the inodes of /proc/self and /proc/thread-self");
    }
    d = opendir("/proc");
    if (!d) {
        return fail("the listing of /proc");
    }
    while ((e = readdir(d)) != NULL) {
        if (strcmp(e->d_name, "self") == 0 && e->d_type == DT_LNK && e->d_ino == self.st_ino) {
            seen |= 1;
        }
        if (strcmp(e->d_name, "thread-self") == 0 && e->d_type == DT_LNK &&
            e->d_ino == thread.st_ino) {
            seen |= 2;
        }
    }
    closedir(d);
    if (seen != 3) {
        return fail_step("the entries of /proc/self and /proc/thread-self", seen);
    }
    report_held();
    return 0;
}

/**
 * Check one self link `path` of a proc mount whose PID namespace does not
 * see the caller, which has lstat'ed /proc's link of the same name into
 * `own`: it lstats as a link with that inode, opens with O_PATH and
 * O_NOFOLLOW and fstats as the same link, and gives ENOENT to readlink, to
 * readlinkat of the descriptor and to stat, which follows it. Returns 0, or
 * the number of the first check that failed.
 */
static int unseen_link(const char *path, const struct stat *own)
{
    struct stat lst, fst;
    char text[32];
    int fd, ok;

    if (lstat(path, &lst) != 0 || !S_ISLNK(lst.st_mode) || lst.st_ino != own->st_ino) {
        return 1;
    }
    fd = open(path, O_PATH | O_NOFOLLOW);
    if (fd < 0) {
        return 2;
    }
    ok = fstat(fd, &fst) == 0 && S_ISLNK(fst.st_mode) && fst.st_ino == lst.st_ino &&
         fst.st_dev == lst.st_dev;
    if (ok && !refused((int)readlinkat(fd, "", text, sizeof(text)), ENOENT)) {
        ok = -1;
    }
    close(fd);
    if (ok != 1) {
        return ok == 0 ? 3 : 4;
    }
    if (!refused((int)readlink(path, text, sizeof(text)), ENOENT)) {
        return 5;
    }
    return refused(stat(path, &lst), ENOENT) ? 0 : 6;
}

/**
 * Check /proc/self and /proc/thread-self of a proc mount whose PID
 * namespace does not see the caller. A child unshares its PID namespace and
 * forks the namespace's first process, which mounts proc on TOP/p in the
 * mount namespace it shares with the probe and waits. To the probe each link
 * of that mount is there, as `unseen_link` checks, since Linux makes the
 * link's inode with the mount and asks for a caller the namespace sees only
 * for its text and its target (fs/proc/self.c and thread_self.c). Returns 0,
 * or 1 after printing the failure.
 */
static int self_other_ns(void)
{
    struct stat self, thread;
    int ready[2], go[2];
    char c = 0;
    pid_t pid, first;
    int st, sent;

    if (lstat("/proc/self", &self) != 0 || lstat("/proc/thread-self", &thread) != 0 ||
        mkdir(TOP "/p", 0755) != 0 || pipe(ready) != 0 || pipe(go) != 0) {
        return fail("the links, the mount point and the pipes");
    }
    pid = fork();
    if (pid == 0) {
        close(ready[0]);
        close(go[1]);
        if (unshare(CLONE_NEWPID) != 0) {
            _exit(2);
        }
        first = fork();
        if (first == 0) {
            if (mount("proc", TOP "/p", "proc", 0, NULL) != 0) {
                _exit(3);
            }
            if (write(ready[1], &c, 1) != 1) {
                _exit(4);
            }
            (void)read(go[0], &c, 1);
            _exit(umount2(TOP "/p", 0) != 0 ? 5 : 0);
        }
        close(ready[1]);
        close(go[0]);
        _exit(first < 0 ? 6 : child_status(first));
    }
    close(ready[1]);
    close(go[0]);
    if (pid < 0 || read(ready[0], &c, 1) != 1) {
        st = let_go(pid, ready[0], go[1]);
        rmdir(TOP "/p");
        return fail_step("the mount of another PID namespace", st);
    }
    st = unseen_link(TOP "/p/self", &self);
    if (st == 0) {
        st = unseen_link(TOP "/p/thread-self", &thread);
        st = st == 0 ? 0 : 10 + st;
    }
    sent = write(go[1], &c, 1) == 1;
    if (let_go(pid, ready[0], go[1]) != 0 || !sent || rmdir(TOP "/p") != 0) {
        return fail("the mount of another PID namespace's removal");
    }
    if (st != 0) {
        return fail_step("a self link the mount's PID namespace does not see", st);
    }
    report_held();
    return 0;
}

/** Begin the report and plan its assertions, in the order they are
 * checked. */
static void plan(void)
{
    static const char *const ids[] = {"fd-directory",
                                      "lookup",
                                      "dots",
                                      "create",
                                      "mkdir",
                                      "rename",
                                      "unlink",
                                      "renamed-lookup",
                                      "renamed-mkdir",
                                      "renamed-chdir",
                                      "renamed-file-link",
                                      "moved-file-link",
                                      "deleted-file-link",
                                      "removed-lookup",
                                      "removed-links",
                                      "removed-create",
                                      "removed-cwd",
                                      "file-notdir",
                                      "pipe-notdir",
                                      "closed-enoent",
                                      "dev-through",
                                      "cwd-root-other",
                                      "cwd-root-self",
                                      "fd-other-root",
                                      "fd-other-same-uid",
                                      "fd-other-eacces",
                                      "fd-dir-owner",
                                      "fd-dir-eacces",
                                      "fd-dir-ended",
                                      "long-link",
                                      "long-removal",
                                      "thread-self",
                                      "self-inodes",
                                      "self-other-ns",
                                      "cleanup"};
    unsigned i;

    report_begin(report_case(environ, "guest.default.proclinkmusl"));
    for (i = 0; i < sizeof(ids) / sizeof(ids[0]); i++) {
        report_plan(ids[i]);
    }
}

/**
 * Mount the tmpfs, run the rows in order, remove the tmpfs and print the
 * result.
 *
 * SIGPIPE is ignored: a held child that has died leaves the write that
 * releases it with no reader, and Linux sends SIGPIPE before EPIPE, which
 * would kill the probe before it reaps the child, removes the tmpfs and
 * prints its failure. Ignored, the write fails and the row says why.
 */
int main(int argc, char **argv)
{
    int d;
    int bad;

    probe_args(argc, argv);
    signal(SIGPIPE, SIG_IGN);
    plan();
    if (!is_dir("/proc/self/fd")) {
        return fail("/proc/self/fd is not a directory");
    }
    report_held();

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
          fd_dir_search() || long_paths() || thread_self() || self_inodes() || self_other_ns();
    close(d);
    if (umount2(TOP, 0) != 0 || rmdir(TOP) != 0) {
        return fail("the tmpfs's removal");
    }
    if (bad) {
        return 1;
    }
    report_held();
    report_end();
    printf("PROCLINKMUSL OK\n");
    return 0;
}
