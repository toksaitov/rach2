/**
 * mknod and mknodat, and the kinds of node they make.
 *
 * On KRFFS, in /tmp: a FIFO made with the umask applied and one made by
 * mknodat from a directory descriptor, a socket file and a regular file,
 * which stat reports with their kinds and a listing with DT_FIFO, DT_SOCK
 * and DT_REG; an open of the socket file fails with ENXIO and a connect to
 * it with ECONNREFUSED; a device node is refused with EPERM, since KRFFS
 * has no field for its number. On a tmpfs mounted in /tmp: character and
 * block device nodes whose numbers stat reports, a small one and one in
 * Linux's wide encoding, listed as DT_CHR and DT_BLK; the node with
 * /dev/null's number opens as /dev/null does, and one with a number no
 * device has fails with ENXIO. Then the refusals, in Linux's order: EINVAL
 * for a kind that is none of the five, EPERM for a directory, both before a
 * null path's EFAULT, EEXIST for a name that exists, a symbolic link
 * included, ENOENT and ENOTDIR for a bad path, EROFS under /dev, and EPERM
 * for a device node from a caller who is not root, who may make a FIFO. A
 * device node on /dev/shm, which is nodev, does not open (EACCES).
 *
 * Prints "MKNODMUSL OK" on success and "MKNODMUSL FAIL (detail)" otherwise.
 */

#define _GNU_SOURCE
#include <dirent.h>
#include <errno.h>
#include <fcntl.h>
#include <stdio.h>
#include <string.h>
#include <sys/mount.h>
#include <sys/socket.h>
#include <sys/stat.h>
#include <sys/sysmacros.h>
#include <sys/un.h>
#include <sys/wait.h>
#include <unistd.h>

#define FIFO_PATH "/tmp/mknodmusl.p"
#define ATFIFO    "mknodmusl.q"
#define SOCK_PATH "/tmp/mknodmusl.s"
#define FILE_PATH "/tmp/mknodmusl.f"
#define MNT       "/tmp/mknodmusl.d"

/** Report one failed check and return 1, the probe's failure status. */
static int fail(const char *what)
{
    printf("MKNODMUSL FAIL (%s: errno %d)\n", what, errno);
    return 1;
}

/** Whether the call's -1 came with `err`. */
static int refused(int rc, int err)
{
    return rc == -1 && errno == err;
}

/** Whether directory `dir` lists `name` with the type `type`. */
static int lists(const char *dir, const char *name, unsigned char type)
{
    DIR *d = opendir(dir);
    struct dirent *e;
    int found = 0;

    if (!d) {
        return 0;
    }
    while (!found && (e = readdir(d)) != NULL) {
        found = strcmp(e->d_name, name) == 0 && e->d_type == type;
    }
    closedir(d);
    return found;
}

/** Whether `path` is a node of the mode `mode` and the device number `dev`. */
static int node_is(const char *path, mode_t mode, dev_t dev)
{
    struct stat st;

    return stat(path, &st) == 0 && st.st_mode == mode && st.st_rdev == dev;
}

static void clean(void)
{
    unlink(FIFO_PATH);
    unlink("/tmp/" ATFIFO);
    unlink(SOCK_PATH);
    unlink(FILE_PATH);
}

static int on_krffs(void)
{
    struct sockaddr_un a;
    int dirfd, s;

    clean();
    umask(022);
    if (mknod(FIFO_PATH, S_IFIFO | 0666, 0) != 0 || !node_is(FIFO_PATH, S_IFIFO | 0644, 0)) {
        return fail("a FIFO with the umask applied");
    }
    dirfd = open("/tmp", O_RDONLY | O_DIRECTORY);
    if (dirfd < 0 || mknodat(dirfd, ATFIFO, S_IFIFO | 0600, 0) != 0 ||
        !node_is("/tmp/" ATFIFO, S_IFIFO | 0600, 0)) {
        return fail("a FIFO by mknodat");
    }
    close(dirfd);
    if (mknod(SOCK_PATH, S_IFSOCK | 0600, 0) != 0 || !node_is(SOCK_PATH, S_IFSOCK | 0600, 0) ||
        mknod(FILE_PATH, 0644, 0) != 0 || !node_is(FILE_PATH, S_IFREG | 0644, 0)) {
        return fail("a socket file and a regular file");
    }
    if (!lists("/tmp", "mknodmusl.p", DT_FIFO) || !lists("/tmp", "mknodmusl.s", DT_SOCK) ||
        !lists("/tmp", "mknodmusl.f", DT_REG)) {
        return fail("the listing's types");
    }
    if (!refused(open(SOCK_PATH, O_RDWR), ENXIO)) {
        return fail("an open of a socket file");
    }
    memset(&a, 0, sizeof(a));
    a.sun_family = AF_UNIX;
    strcpy(a.sun_path, SOCK_PATH);
    s = socket(AF_UNIX, SOCK_STREAM, 0);
    if (s < 0 || !refused(connect(s, (struct sockaddr *)&a, sizeof(a)), ECONNREFUSED)) {
        return fail("a connect to a socket file with no socket");
    }
    close(s);
    if (!refused(mknod("/tmp/mknodmusl.c", S_IFCHR | 0600, makedev(1, 3)), EPERM)) {
        return fail("a device node on KRFFS");
    }
    clean();
    return 0;
}

static int devices(void)
{
    const dev_t wide = makedev(4000, 70000);
    char c;
    int fd;

    mkdir(MNT, 0755);
    if (mount("none", MNT, "tmpfs", 0, NULL) != 0) {
        return fail("the tmpfs");
    }
    if (mknod(MNT "/null", S_IFCHR | 0666, makedev(1, 3)) != 0 ||
        !node_is(MNT "/null", S_IFCHR | 0644, makedev(1, 3)) ||
        mknod(MNT "/wide", S_IFCHR | 0600, wide) != 0 ||
        !node_is(MNT "/wide", S_IFCHR | 0600, wide) ||
        mknod(MNT "/blk", S_IFBLK | 0600, makedev(8, 1)) != 0 ||
        !node_is(MNT "/blk", S_IFBLK | 0600, makedev(8, 1))) {
        return fail("device nodes and their numbers");
    }
    if (!lists(MNT, "null", DT_CHR) || !lists(MNT, "blk", DT_BLK)) {
        return fail("the listing's device types");
    }
    fd = open(MNT "/null", O_RDWR);
    if (fd < 0 || write(fd, "x", 1) != 1 || read(fd, &c, 1) != 0) {
        return fail("the node of /dev/null's number");
    }
    close(fd);
    if (!refused(open(MNT "/wide", O_RDONLY), ENXIO) ||
        !refused(open(MNT "/blk", O_RDONLY), ENXIO)) {
        return fail("nodes of numbers no device has");
    }
    return 0;
}

static int refusals(void)
{
    const char *no_path = NULL;
    pid_t pid;
    int status;

    if (!refused(mknod(no_path, S_IFDIR | 0700, 0), EPERM) ||
        !refused(mknodat(AT_FDCWD, no_path, 0170000 | 0600, 0), EINVAL) ||
        !refused(mknod(no_path, S_IFIFO | 0600, 0), EFAULT)) {
        return fail("the kind before the path");
    }
    if (!refused(mknod(MNT "/x", 0170000 | 0600, 0), EINVAL) ||
        !refused(mknod(MNT "/x", S_IFDIR | 0700, 0), EPERM) || symlink("nowhere", MNT "/ln") != 0 ||
        !refused(mknod(MNT "/ln", S_IFIFO | 0600, 0), EEXIST) ||
        !refused(mknod(MNT "/null", S_IFIFO | 0600, 0), EEXIST) ||
        !refused(mknod(MNT "/none/x", S_IFIFO | 0600, 0), ENOENT) ||
        !refused(mknod(MNT "/null/x", S_IFIFO | 0600, 0), ENOTDIR) ||
        !refused(mknod("/dev/mknodmusl", S_IFIFO | 0600, 0), EROFS)) {
        return fail("the refusals");
    }
    pid = fork();
    if (pid == 0) {
        if (setgid(1000) != 0 || setuid(1000) != 0) {
            _exit(2);
        }
        if (!refused(mknod(MNT "/u", S_IFCHR | 0600, makedev(1, 3)), EPERM)) {
            _exit(3);
        }
        _exit(mknod(MNT "/u", S_IFIFO | 0600, 0) == 0 ? 0 : 4);
    }
    if (waitpid(pid, &status, 0) != pid || !WIFEXITED(status) || WEXITSTATUS(status) != 0) {
        return fail("a device node from a caller who is not root");
    }
    if (mknod("/dev/shm/mknodmusl", S_IFCHR | 0666, makedev(1, 3)) != 0 ||
        !refused(open("/dev/shm/mknodmusl", O_RDONLY), EACCES) ||
        unlink("/dev/shm/mknodmusl") != 0) {
        return fail("a device node on a nodev mount");
    }
    if (umount2(MNT, 0) != 0) {
        return fail("the unmount");
    }
    rmdir(MNT);
    return 0;
}

int main(void)
{
    if (on_krffs() || devices() || refusals()) {
        return 1;
    }
    printf("MKNODMUSL OK\n");
    return 0;
}
