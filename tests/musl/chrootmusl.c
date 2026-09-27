/**
 * chroot: a process's own root.
 *
 * The refusals come first: a file that is not a directory is ENOTDIR, a
 * missing path ENOENT, /proc and /dev EINVAL, and a caller who is not root
 * EPERM, after the path's own errors; chroot("/") changes nothing. A child
 * that chroots into a KRFFS directory under /tmp keeps its working directory
 * there, under its path from the new root; reads the root's files by
 * absolute paths and finds nothing of the tree above it; stays at the root
 * on ".."; cannot use a directory descriptor opened outside; and passes the
 * root to a child of fork and to a copy of this program it execs from
 * inside. A child whose working directory is outside the new root finds it
 * moved to the root, and a second chroot inside the first gives the working
 * directory its path from the inner root. In a tmpfs jail an absolute
 * symbolic link and one that climbs past the root by ".." both resolve
 * inside, and the working directory has its path from the root there too.
 * While a process has its root in a directory, rmdir refuses the directory
 * with EBUSY, on KRFFS and on tmpfs, and umount2 refuses the tmpfs; both
 * succeed once the process is gone.
 *
 * Prints "CHROOTMUSL OK" on success and "CHROOTMUSL FAIL (detail)" otherwise.
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

#define SELF  "/musl/chrootmusl"
#define JAIL  "/tmp/chrootmusl"
#define EMPTY "/tmp/chrootmusl-e"
#define TJAIL "/tmp/chrootmusl-t"

/** Report one failed check and return 1, the probe's failure status. */
static int fail(const char *what)
{
    printf("CHROOTMUSL FAIL (%s: errno %d)\n", what, errno);
    return 1;
}

/** Report a child that failed at `step` and return 1. */
static int fail_step(const char *what, int step)
{
    printf("CHROOTMUSL FAIL (%s: step %d)\n", what, step);
    return 1;
}

/** Whether a call that returned `rc` failed with `want`. */
static int refused(int rc, int want)
{
    return rc == -1 && errno == want;
}

/** Whether the file at `path` holds exactly `want`. */
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

/** Write `text` to a new file at `path`. */
static int put(const char *path, const char *text)
{
    int fd = open(path, O_WRONLY | O_CREAT | O_TRUNC, 0644);
    int ok = fd >= 0 && write(fd, text, strlen(text)) == (ssize_t)strlen(text);

    if (fd >= 0) {
        close(fd);
    }
    return ok;
}

/** Copy the file `from` to a new file `to` of mode 0755. */
static int copy(const char *from, const char *to)
{
    char buf[4096];
    int in = open(from, O_RDONLY);
    int out = open(to, O_WRONLY | O_CREAT | O_TRUNC, 0755);
    ssize_t n = 0;
    int ok = in >= 0 && out >= 0;

    while (ok && (n = read(in, buf, sizeof(buf))) > 0) {
        ok = write(out, buf, (size_t)n) == n;
    }
    if (in >= 0) {
        close(in);
    }
    if (out >= 0) {
        close(out);
    }
    return ok && n == 0;
}

/** Whether the working directory is `want`. */
static int cwd_is(const char *want)
{
    char buf[64];

    return getcwd(buf, sizeof(buf)) != NULL && strcmp(buf, want) == 0;
}

/** Whether `a` and `b` name one file. */
static int same_file(const char *a, const char *b)
{
    struct stat sa, sb;

    return stat(a, &sa) == 0 && stat(b, &sb) == 0 && sa.st_dev == sb.st_dev &&
           sa.st_ino == sb.st_ino;
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

/**
 * The checks of a process whose root is JAIL: the root's own file by an
 * absolute path, nothing of the tree above it, and ".." at the root staying
 * there. 0 when all pass, or the number of the one that failed.
 */
static int inside(void)
{
    if (!holds("/marker", "inside")) {
        return 1;
    }
    if (!refused(access("/musl", F_OK), ENOENT) || !refused(access("/tmp", F_OK), ENOENT)) {
        return 2;
    }
    if (!same_file("/..", "/") || !same_file("/dir/../..", "/") ||
        !same_file("/../../dir", "/dir")) {
        return 3;
    }
    return 0;
}

/** Make JAIL on the KRFFS root, with a copy of this program in it. */
static int build_jail(void)
{
    mkdir(JAIL, 0755);
    mkdir(JAIL "/dir", 0755);
    mkdir(JAIL "/dir/sub", 0755);
    mkdir(JAIL "/bin", 0755);
    if (!put(JAIL "/marker", "inside") || !copy(SELF, JAIL "/bin/probe")) {
        return fail("the KRFFS jail");
    }
    return 0;
}

/** The refusals, in Linux's order, and chroot("/"), which changes nothing. */
static int refusals(void)
{
    pid_t pid;
    int st;

    if (!refused(chroot(SELF), ENOTDIR) || !refused(chroot("/chrootmusl-none"), ENOENT) ||
        !refused(chroot("/proc"), EINVAL) || !refused(chroot("/dev"), EINVAL)) {
        return fail("the refusals");
    }
    if (chdir(JAIL "/dir") != 0 || chroot("/") != 0 || !cwd_is(JAIL "/dir") ||
        !holds(JAIL "/marker", "inside") || chdir("/") != 0) {
        return fail("chroot of /");
    }
    pid = fork();
    if (pid == 0) {
        if (setgid(1000) != 0 || setuid(1000) != 0) {
            _exit(2);
        }
        if (!refused(chroot("/chrootmusl-none"), ENOENT) || !refused(chroot(SELF), ENOTDIR)) {
            _exit(3);
        }
        _exit(refused(chroot(JAIL), EPERM) ? 0 : 4);
    }
    st = child_status(pid);
    return st == 0 ? 0 : fail_step("a caller who is not root", st);
}

/**
 * A root on KRFFS: the child keeps a working directory inside under its new
 * path, loses a directory descriptor from outside, stays at the root on
 * "..", and passes the root to a child of fork and to what it execs.
 */
static int krffs_root(void)
{
    pid_t pid = fork();
    int st;

    if (pid == 0) {
        int out = open("/musl", O_RDONLY | O_DIRECTORY);
        pid_t grandchild;

        if (out < 0 || chdir(JAIL "/dir") != 0 || chroot(JAIL) != 0) {
            _exit(2);
        }
        if (!cwd_is("/dir")) {
            _exit(3);
        }
        st = inside();
        if (st != 0) {
            _exit(10 + st);
        }
        if (!refused(fchdir(out), ENOENT) ||
            !refused(openat(out, "chrootmusl", O_RDONLY), ENOENT)) {
            _exit(4);
        }
        if (chdir("..") != 0 || !cwd_is("/") || chdir("..") != 0 || !cwd_is("/")) {
            _exit(5);
        }
        grandchild = fork();
        if (grandchild == 0) {
            _exit(inside());
        }
        if (child_status(grandchild) != 0) {
            _exit(6);
        }
        if (chdir("/dir") != 0) {
            _exit(7);
        }
        execl("/bin/probe", "probe", "inside", (char *)NULL);
        _exit(8);
    }
    st = child_status(pid);
    return st == 0 ? 0 : fail_step("a root on KRFFS", st);
}

/**
 * A working directory outside the new root moves to it, and a second chroot
 * inside the first leaves the working directory under its path from the
 * inner root.
 */
static int moved_cwd(void)
{
    pid_t pid = fork();
    int st;

    if (pid == 0) {
        if (chdir("/musl") != 0 || chroot(JAIL) != 0) {
            _exit(2);
        }
        if (!cwd_is("/") || !holds("marker", "inside")) {
            _exit(3);
        }
        if (chdir("/dir/sub") != 0 || chroot("/dir") != 0 || !cwd_is("/sub")) {
            _exit(4);
        }
        if (!refused(access("/marker", F_OK), ENOENT) || !same_file("/..", "/") ||
            !same_file("..", "/")) {
            _exit(5);
        }
        _exit(0);
    }
    st = child_status(pid);
    return st == 0 ? 0 : fail_step("a working directory outside the root", st);
}

/**
 * A root on a tmpfs: an absolute link and a relative one that climbs past
 * the root resolve inside, and the working directory has its path from the
 * root of a mounted type.
 */
static int tmpfs_root(void)
{
    char target[16];
    pid_t pid;
    int st;

    mkdir(TJAIL, 0755);
    if (mount("none", TJAIL, "tmpfs", 0, NULL) != 0 || mkdir(TJAIL "/a", 0755) != 0 ||
        mkdir(TJAIL "/b", 0755) != 0 || mkdir(TJAIL "/b/dir", 0755) != 0 ||
        mkdir(TJAIL "/b/dir/sub", 0755) != 0 || !put(TJAIL "/b/marker", "inside") ||
        symlink("/marker", TJAIL "/b/abs") != 0 ||
        symlink("../../../../marker", TJAIL "/b/dir/sub/up") != 0) {
        return fail("the tmpfs jail");
    }
    pid = fork();
    if (pid == 0) {
        if (chdir(TJAIL "/b/dir") != 0 || chroot(TJAIL "/b") != 0 || !cwd_is("/dir")) {
            _exit(2);
        }
        if (!holds("/abs", "inside") || readlink("/abs", target, sizeof(target)) != 7 ||
            memcmp(target, "/marker", 7) != 0) {
            _exit(3);
        }
        if (!holds("/dir/sub/up", "inside") || !holds("sub/up", "inside")) {
            _exit(4);
        }
        _exit(same_file("/..", "/") && same_file("../..", "/") ? 0 : 5);
    }
    st = child_status(pid);
    return st == 0 ? 0 : fail_step("a root on tmpfs", st);
}

/**
 * While a child has its root in `dir`, rmdir of `dir` is EBUSY, and so is
 * umount2 of `mnt` when it is not NULL; after the child ends both succeed.
 */
static int busy(const char *dir, const char *mnt)
{
    int ready[2], go[2];
    char c = 0;
    pid_t pid;
    int held;

    if (pipe(ready) != 0 || pipe(go) != 0) {
        return fail("the pipes");
    }
    pid = fork();
    if (pid == 0) {
        close(ready[0]);
        close(go[1]);
        if (chroot(dir) != 0) {
            _exit(2);
        }
        if (write(ready[1], &c, 1) != 1 || read(go[0], &c, 1) != 1) {
            _exit(3);
        }
        _exit(0);
    }
    close(ready[1]);
    close(go[0]);
    held = read(ready[0], &c, 1) == 1 && refused(rmdir(dir), EBUSY) &&
           (!mnt || refused(umount2(mnt, 0), EBUSY));
    if (write(go[1], &c, 1) != 1 || child_status(pid) != 0 || !held) {
        return fail(dir);
    }
    close(ready[0]);
    close(go[1]);
    if (rmdir(dir) != 0) {
        return fail("the rmdir after the process ended");
    }
    return 0;
}

/** Take the jails apart. */
static int clean(void)
{
    if (umount2(TJAIL, 0) != 0 || rmdir(TJAIL) != 0) {
        return fail("the tmpfs jail's unmount");
    }
    if (unlink(JAIL "/bin/probe") != 0 || unlink(JAIL "/marker") != 0 || rmdir(JAIL "/bin") != 0 ||
        rmdir(JAIL "/dir/sub") != 0 || rmdir(JAIL "/dir") != 0 || rmdir(JAIL) != 0) {
        return fail("the KRFFS jail's removal");
    }
    return 0;
}

int main(int argc, char **argv)
{
    if (argc == 2 && strcmp(argv[1], "inside") == 0) {
        int st = inside();

        if (st != 0 || !cwd_is("/dir")) {
            return fail_step("the program exec'd inside", st);
        }
        return 0;
    }
    mkdir(EMPTY, 0755);
    if (build_jail() || refusals() || krffs_root() || moved_cwd() || tmpfs_root() ||
        busy(EMPTY, NULL) || busy(TJAIL "/a", TJAIL) || clean()) {
        return 1;
    }
    printf("CHROOTMUSL OK\n");
    return 0;
}
