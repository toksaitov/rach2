/**
 * tmpfsmusl: check tmpfs, mounted by a program and at /dev/shm, and binds.
 *
 * A tmpfs mounted with mode=0700 on a directory in /tmp has that mode at its
 * root and tmpfs's magic in statfs. A file is written and read back; a
 * directory, a hard link and a symbolic link are made and read; a rename
 * replaces a file whose other name keeps the old bytes. /proc/mounts has the
 * mount's line and /proc/filesystems has "nodev\ttmpfs". umount2 of the mount
 * fails with EBUSY while a file on it is open and succeeds after the close,
 * and a new mount there is empty. A second mount takes 512 nested directories
 * and is unmounted with them. /dev/shm is a directory of mode 1777 that /dev
 * lists as one, with tmpfs's magic and its line in /proc/mounts with nosuid
 * and nodev; a file is made, written, read and removed there as shm_open makes
 * one; a working directory there climbs to /dev by "..", /dev/pts/../shm is
 * the same directory, and umount2 of /dev/shm fails with EBUSY. A remount of
 * a tmpfs with a file of two pages takes a size= that they fit in, which
 * statfs gives, and fails with EINVAL for a smaller one.
 *
 * A bind of a directory of a tmpfs shows it at another directory: a file made
 * through the bind is in the source with the same device and inode, an
 * exclusive flock through one path blocks one through the other, ".." at the
 * bind's root climbs to the parent of the directory it covers, a write to a
 * bind remounted read-only fails while its source takes one, mountinfo gives
 * the bind its root within the file system, and the source's mount is busy
 * until the bind is unmounted. A bind of a directory of the KRFFS root shows a
 * file made through it as its source does after the file grows enough for
 * KRFFS to move it, and so does a descriptor opened before the growth, whose
 * fstat reaches the file where it is now; and the bind's root is busy to rmdir
 * until the bind is unmounted. A bind of the KRFFS root directory reports its
 * links, owner and times as / does. A recursive bind of /dev has its devices
 * and /dev/shm; ".." at its root, and a ".." that climbs out of it after a
 * component inside, reach the parent of the directory it covers; and it is
 * unmounted in turn with /dev/shm and /dev/mqueue.
 *
 * The program runs as root, since it mounts. It prints `TMPFSMUSL OK` and
 * exits 0 on success, and prints `TMPFSMUSL FAIL (detail)` and exits 1
 * otherwise.
 */

#define _GNU_SOURCE
#include <dirent.h>
#include <errno.h>
#include <fcntl.h>
#include <stdio.h>
#include <string.h>
#include <sys/file.h>
#include <sys/mount.h>
#include <sys/stat.h>
#include <sys/statfs.h>
#include <unistd.h>

#define DIR_PATH    "/tmp/tmpfsmusl"
#define DEEP_PATH   "/tmp/tmpfsmusl-d"
#define BIND_SRC    "/tmp/tmpfsmusl-s"
#define BIND_DST    "/tmp/tmpfsmusl-b"
#define BIND_DEV    "/tmp/tmpfsmusl-dev"
#define TMPFS_MAGIC 0x01021994

/** Report one failed check and return 1, the probe's failure status. */
static int fail(const char *what)
{
    printf("TMPFSMUSL FAIL (%s: errno %d)\n", what, errno);
    return 1;
}

/** Whether the text file `path` has a line that begins with `start`. */
static int has_line(const char *path, const char *start)
{
    char line[256];
    FILE *f = fopen(path, "r");
    int found = 0;

    if (!f) {
        return 0;
    }
    while (!found && fgets(line, sizeof(line), f)) {
        found = strncmp(line, start, strlen(start)) == 0;
    }
    fclose(f);
    return found;
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

/** The number of entries directory `dir` lists besides "." and "..". */
static int entries(const char *dir)
{
    DIR *d = opendir(dir);
    struct dirent *e;
    int n = 0;

    if (!d) {
        return -1;
    }
    while ((e = readdir(d)) != NULL) {
        n += strcmp(e->d_name, ".") != 0 && strcmp(e->d_name, "..") != 0;
    }
    closedir(d);
    return n;
}

/** Whether the file at `path` has exactly `want` in it. */
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

/** Whether readlink of `path` gives exactly `want`, into a buffer marked
 * first, so the bytes it compares are the call's. */
static int link_is(const char *path, const char *want)
{
    char target[16];
    ssize_t n;

    memset(target, '#', sizeof(target));
    n = readlink(path, target, sizeof(target));
    return n == (ssize_t)strlen(want) && memcmp(target, want, (size_t)n) == 0;
}

/** Write `text` to a new file at `path`. Returns 1, or 0 on failure. */
static int put(const char *path, const char *text)
{
    int fd = open(path, O_WRONLY | O_CREAT | O_TRUNC, 0644);
    int ok = fd >= 0 && write(fd, text, strlen(text)) == (ssize_t)strlen(text);

    if (fd >= 0) {
        close(fd);
    }
    return ok;
}

/**
 * Mount a tmpfs with mode=0700 on `DIR_PATH` and check its root, a file, a
 * directory, the two kinds of link, a rename, the /proc files and the
 * unmounts. Returns 0, or 1 once the failure is reported.
 */
static int placed(void)
{
    struct stat st;
    struct statfs sf;
    int fd;

    mkdir(DIR_PATH, 0755);
    if (mount("none", DIR_PATH, "tmpfs", 0, "mode=0700") != 0) {
        return fail("mount -t tmpfs");
    }
    if (stat(DIR_PATH, &st) != 0 || !S_ISDIR(st.st_mode) || (st.st_mode & 07777) != 0700 ||
        statfs(DIR_PATH, &sf) != 0 || sf.f_type != TMPFS_MAGIC) {
        return fail("the root's mode and the magic");
    }
    if (!put(DIR_PATH "/f", "tmpfs") || !holds(DIR_PATH "/f", "tmpfs") ||
        stat(DIR_PATH "/f", &st) != 0 || st.st_size != 5) {
        return fail("a file");
    }
    if (mkdir(DIR_PATH "/d", 0755) != 0 || link(DIR_PATH "/f", DIR_PATH "/d/g") != 0 ||
        stat(DIR_PATH "/f", &st) != 0 || st.st_nlink != 2 || symlink("f", DIR_PATH "/l") != 0 ||
        !link_is(DIR_PATH "/l", "f") || !holds(DIR_PATH "/l", "tmpfs")) {
        return fail("a directory and the two links");
    }
    if (!put(DIR_PATH "/e", "E") || rename(DIR_PATH "/e", DIR_PATH "/f") != 0 ||
        !holds(DIR_PATH "/f", "E") || !holds(DIR_PATH "/d/g", "tmpfs")) {
        return fail("a rename over a linked file");
    }
    if (!has_line("/proc/mounts", "none " DIR_PATH " tmpfs rw") ||
        !has_line("/proc/filesystems", "nodev\ttmpfs")) {
        return fail("the /proc files");
    }
    fd = open(DIR_PATH "/f", O_RDONLY);
    if (fd < 0 || umount2(DIR_PATH, 0) != -1 || errno != EBUSY) {
        return fail("an unmount with a file open");
    }
    close(fd);
    if (umount2(DIR_PATH, 0) != 0 || entries(DIR_PATH) != 0) {
        return fail("the unmount");
    }
    if (mount("tmpfs", DIR_PATH, "tmpfs", 0, NULL) != 0 || entries(DIR_PATH) != 0 ||
        stat(DIR_PATH, &st) != 0 || (st.st_mode & 07777) != 01777 || umount2(DIR_PATH, 0) != 0) {
        return fail("a second mount, empty");
    }
    rmdir(DIR_PATH);
    return 0;
}

/**
 * Check a tree as deep as a working directory's path allows: 512 nested
 * directories, each made from the one above it, and an unmount that frees
 * them all. Returns 0, or 1 once the failure is reported.
 */
static int deep(void)
{
    int i;

    mkdir(DEEP_PATH, 0755);
    if (mount("none", DEEP_PATH, "tmpfs", 0, NULL) != 0 || chdir(DEEP_PATH) != 0) {
        return fail("the deep tree's mount");
    }
    for (i = 0; i < 512; i++) {
        if (mkdir("a", 0755) != 0 || chdir("a") != 0) {
            return fail("a level of the deep tree");
        }
    }
    if (chdir("/") != 0 || umount2(DEEP_PATH, 0) != 0 || rmdir(DEEP_PATH) != 0) {
        return fail("the deep tree's unmount");
    }
    return 0;
}

/**
 * Check /dev/shm: its kind, mode and magic, its line in /proc/mounts, a file
 * in it, ".." from it, and that it does not unmount. Returns 0, or 1 once the
 * failure is reported.
 */
static int shm(void)
{
    struct stat st;
    struct statfs sf;
    char cwd[32];
    int fd;

    if (stat("/dev/shm", &st) != 0 || !S_ISDIR(st.st_mode) || (st.st_mode & 07777) != 01777 ||
        !lists("/dev", "shm", DT_DIR) || statfs("/dev/shm", &sf) != 0 || sf.f_type != TMPFS_MAGIC) {
        return fail("/dev/shm's kind, mode and magic");
    }
    if (!has_line("/proc/mounts", "tmpfs /dev/shm tmpfs rw,nosuid,nodev")) {
        return fail("/dev/shm in /proc/mounts");
    }
    unlink("/dev/shm/tmpfsmusl");
    fd = open("/dev/shm/tmpfsmusl", O_RDWR | O_CREAT | O_EXCL | O_CLOEXEC, 0600);
    if (fd < 0) {
        return fail("a file in /dev/shm");
    }
    close(fd);
    if (!put("/dev/shm/tmpfsmusl", "shared") || !holds("/dev/shm/tmpfsmusl", "shared") ||
        !lists("/dev/shm", "tmpfsmusl", DT_REG) || unlink("/dev/shm/tmpfsmusl") != 0) {
        return fail("the file's bytes and its removal");
    }
    if (chdir("/dev/shm") != 0 || !getcwd(cwd, sizeof(cwd)) || strcmp(cwd, "/dev/shm") != 0 ||
        stat("../null", &st) != 0 || !S_ISCHR(st.st_mode) || chdir("/") != 0) {
        return fail("a working directory in /dev/shm");
    }
    if (stat("/dev/pts/../shm", &st) != 0 || !S_ISDIR(st.st_mode) ||
        chdir("/dev/pts/../shm") != 0 || !getcwd(cwd, sizeof(cwd)) ||
        strcmp(cwd, "/dev/shm") != 0 || chdir("/") != 0) {
        return fail("/dev/shm by way of /dev/pts/..");
    }
    if (umount2("/dev/shm", 0) != -1 || errno != EBUSY) {
        return fail("an unmount of /dev/shm");
    }
    return 0;
}

/** Whether a line of /proc/self/mountinfo has `text` in it. */
static int mountinfo_has(const char *text)
{
    char line[256];
    FILE *f = fopen("/proc/self/mountinfo", "r");
    int found = 0;

    if (!f) {
        return 0;
    }
    while (!found && fgets(line, sizeof(line), f)) {
        found = strstr(line, text) != NULL;
    }
    fclose(f);
    return found;
}

/**
 * Mount a tmpfs on `BIND_SRC` and bind a directory of it on `BIND_DST`, and
 * check that a file made through the bind is the source's and that a lock
 * through one path blocks a lock through the other. The mounts stay for
 * `bound_mount`. Returns 0, or 1 once the failure is reported.
 */
static int bound(void)
{
    struct stat a, b;
    int one, two;

    if (mkdir(BIND_SRC, 0755) != 0 || mkdir(BIND_DST, 0755) != 0 ||
        mount("tmpfsmusl", BIND_SRC, "tmpfs", 0, NULL) != 0 || mkdir(BIND_SRC "/sub", 0755) != 0) {
        return fail("the source of a bind");
    }
    if (mount(BIND_SRC "/sub", BIND_DST, NULL, MS_BIND, NULL) != 0) {
        return fail("a bind of a directory of a tmpfs");
    }
    if (!put(BIND_DST "/f", "through") || !holds(BIND_SRC "/sub/f", "through") ||
        stat(BIND_DST "/f", &a) != 0 || stat(BIND_SRC "/sub/f", &b) != 0 || a.st_dev != b.st_dev ||
        a.st_ino != b.st_ino) {
        return fail("a file made through a bind is the source's file");
    }
    one = open(BIND_DST "/f", O_RDONLY);
    two = open(BIND_SRC "/sub/f", O_RDONLY);
    if (one < 0 || two < 0 || flock(one, LOCK_EX) != 0 || flock(two, LOCK_EX | LOCK_NB) != -1 ||
        errno != EWOULDBLOCK) {
        return fail("a lock through a bind meets one through its source");
    }
    close(one);
    close(two);
    return 0;
}

/**
 * Check the bind's place and flags: ".." at its root, a read-only remount of
 * it alone, its root in mountinfo, and its source's mount busy under it; then
 * unmount both. Returns 0, or 1 once the failure is reported.
 */
static int bound_mount(void)
{
    char cwd[64];

    if (chdir(BIND_DST) != 0 || chdir("..") != 0 || !getcwd(cwd, sizeof(cwd)) ||
        strcmp(cwd, "/tmp") != 0 || chdir("/") != 0) {
        return fail("\"..\" at a bind's root climbs to the directory it covers");
    }
    if (mount(NULL, BIND_DST, NULL, MS_REMOUNT | MS_BIND | MS_RDONLY, NULL) != 0 ||
        put(BIND_DST "/g", "no") || errno != EROFS || !put(BIND_SRC "/sub/g", "yes")) {
        return fail("a bind remounted read-only refuses a write its source takes");
    }
    if (!mountinfo_has(" /sub " BIND_DST " ")) {
        return fail("mountinfo gives a bind its root within the file system");
    }
    if (umount2(BIND_SRC, 0) != -1 || errno != EBUSY || umount2(BIND_DST, 0) != 0 ||
        umount2(BIND_SRC, 0) != 0 || rmdir(BIND_DST) != 0 || rmdir(BIND_SRC) != 0) {
        return fail("the source's mount is busy until the bind is unmounted");
    }
    return 0;
}

/**
 * Check that a recursive bind of /dev has its devices and /dev/shm, that
 * ".." above its root is taken in the parent of the directory it covers, and
 * that it is unmounted from the bottom, /dev/shm and /dev/mqueue first.
 * Returns 0, or 1 once the failure is reported.
 */
static int rbound(void)
{
    struct stat a, b;

    if (mkdir(BIND_DEV, 0755) != 0 || mount("/dev", BIND_DEV, NULL, MS_BIND | MS_REC, NULL) != 0 ||
        stat(BIND_DEV "/null", &a) != 0 || !S_ISCHR(a.st_mode) || stat(BIND_DEV "/shm", &b) != 0 ||
        (b.st_mode & 07777) != 01777) {
        return fail("a recursive bind of /dev has its devices and /dev/shm");
    }
    if (stat("/tmp", &a) != 0 || stat(BIND_DEV "/..", &b) != 0 || a.st_dev != b.st_dev ||
        a.st_ino != b.st_ino || stat(BIND_DEV "/pts/../../tmpfsmusl-dev/null", &b) != 0 ||
        !S_ISCHR(b.st_mode)) {
        return fail("\"..\" above the root of a bind of /dev is the parent of what it covers");
    }
    if (umount2(BIND_DEV, 0) != -1 || errno != EBUSY || umount2(BIND_DEV "/shm", 0) != 0 ||
        umount2(BIND_DEV "/mqueue", 0) != 0 || umount2(BIND_DEV, 0) != 0 || rmdir(BIND_DEV) != 0) {
        return fail("a recursive bind is unmounted from the bottom");
    }
    return 0;
}

/**
 * Check a remount of a tmpfs with a file of two pages: it takes a size= that
 * they fit in, which statfs gives, and fails with EINVAL for a smaller one.
 * Returns 0, or 1 once the failure is reported.
 */
static int resized(void)
{
    static char two[8192];
    struct statfs sf;
    int fd;

    memset(two, 'p', sizeof(two));
    mkdir(DIR_PATH, 0755);
    if (mount("none", DIR_PATH, "tmpfs", 0, NULL) != 0) {
        return fail("a tmpfs for the remount");
    }
    fd = open(DIR_PATH "/two", O_CREAT | O_WRONLY | O_TRUNC, 0644);
    if (fd < 0 || write(fd, two, sizeof(two)) != (ssize_t)sizeof(two) || close(fd) != 0) {
        return fail("a file of two pages");
    }
    if (mount(NULL, DIR_PATH, NULL, MS_REMOUNT, "size=4k") != -1 || errno != EINVAL ||
        mount(NULL, DIR_PATH, NULL, MS_REMOUNT, "size=16k") != 0 || statfs(DIR_PATH, &sf) != 0 ||
        sf.f_blocks != 4 || sf.f_bfree != 2) {
        return fail("a remount with size=");
    }
    if (unlink(DIR_PATH "/two") != 0 || umount2(DIR_PATH, 0) != 0) {
        return fail("the remounted tmpfs unmounts");
    }
    return 0;
}

/**
 * Check a bind of a directory of the KRFFS root, whose files move as they
 * grow. Returns 0, or 1 once the failure is reported.
 */
static int kbound(void)
{
    static char big[65536];
    char buf[8];
    struct stat a;
    int fd, wfd;

    if (mkdir("/tmp/kbsrc", 0755) != 0 || mkdir("/tmp/kbdst", 0755) != 0 ||
        mount("/tmp/kbsrc", "/tmp/kbdst", NULL, MS_BIND, NULL) != 0) {
        return fail("a bind of a directory of the KRFFS root");
    }
    fd = put("/tmp/kbdst/f", "small") ? open("/tmp/kbdst/f", O_RDONLY) : -1;
    memset(big, 'x', sizeof(big));
    wfd = open("/tmp/kbsrc/f", O_WRONLY | O_APPEND);
    if (fd < 0 || wfd < 0 || write(wfd, big, sizeof(big)) != (ssize_t)sizeof(big) ||
        close(wfd) != 0 || stat("/tmp/kbdst/f", &a) != 0 || a.st_size != 5 + 65536 ||
        pread(fd, buf, 6, 0) != 6 || memcmp(buf, "smallx", 6) != 0 ||
        pread(fd, buf, 1, 5 + 65535) != 1 || buf[0] != 'x') {
        return fail("a KRFFS file grown through its source reads alike through the bind");
    }
    if (fstat(fd, &a) != 0 || a.st_size != 5 + 65536) {
        return fail("a description opened through the bind of KRFFS follows its moved file");
    }
    close(fd);
    if (rmdir("/tmp/kbsrc") != -1 || errno != EBUSY) {
        return fail("the root of a bind of KRFFS is busy to rmdir");
    }
    if (umount2("/tmp/kbdst", 0) != 0 || unlink("/tmp/kbsrc/f") != 0 || rmdir("/tmp/kbsrc") != 0 ||
        rmdir("/tmp/kbdst") != 0) {
        return fail("the bind of KRFFS unmounts and its directories go");
    }
    return 0;
}

/**
 * Check that a bind of the KRFFS root directory reports what / does for it.
 * Returns 0, or 1 once the failure is reported.
 */
static int krbound(void)
{
    struct stat a, b;

    if (mkdir("/tmp/kbroot", 0755) != 0 || mount("/", "/tmp/kbroot", NULL, MS_BIND, NULL) != 0) {
        return fail("a bind of the KRFFS root directory");
    }
    if (stat("/", &a) != 0 || stat("/tmp/kbroot", &b) != 0 || a.st_ino != b.st_ino ||
        a.st_mode != b.st_mode || a.st_nlink != b.st_nlink || a.st_uid != b.st_uid ||
        a.st_gid != b.st_gid || a.st_mtim.tv_sec != b.st_mtim.tv_sec ||
        a.st_mtim.tv_nsec != b.st_mtim.tv_nsec || a.st_ctim.tv_sec != b.st_ctim.tv_sec ||
        a.st_ctim.tv_nsec != b.st_ctim.tv_nsec || a.st_atim.tv_sec != b.st_atim.tv_sec) {
        return fail("a bind of the KRFFS root reports its links, owner and times as / does");
    }
    if (umount2("/tmp/kbroot", 0) != 0 || rmdir("/tmp/kbroot") != 0) {
        return fail("the bind of the KRFFS root unmounts and its directory goes");
    }
    return 0;
}

int main(void)
{
    if (placed() || resized() || deep() || shm() || bound() || bound_mount() || rbound() ||
        kbound() || krbound()) {
        return 1;
    }
    printf("TMPFSMUSL OK\n");
    return 0;
}
