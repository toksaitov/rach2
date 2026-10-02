/**
 * tmpfs, mounted by a program and at /dev/shm.
 *
 * A tmpfs mounted with mode=0700 on a directory in /tmp has that mode at its
 * root and tmpfs's magic in statfs. A file is written and read back; a
 * directory, a hard link and a symbolic link are made and read; a rename
 * replaces a file whose other name keeps the old bytes. /proc/mounts has the
 * mount's line and /proc/filesystems has "nodev\ttmpfs". umount2 refuses the
 * mount while a file on it is open and succeeds after the close, and a new
 * mount there is empty. A second mount takes 512 nested directories and is
 * unmounted with them. /dev/shm is a directory of mode 1777 that /dev lists
 * as one, with tmpfs's magic and its line in /proc/mounts with nosuid and
 * nodev; a file is made, written, read and removed there as shm_open makes
 * one; a working directory there climbs to /dev by "..", /dev/pts/../shm
 * is the same directory, and umount2 refuses /dev/shm with EBUSY.
 *
 * A bind of a directory of a tmpfs shows it at another directory: a file
 * made through the bind is in the source with the same device and inode,
 * an exclusive flock through one path refuses one through the other, ".."
 * at the bind's root climbs to the parent of the directory it covers, a
 * bind remounted read-only refuses a write while its source takes one,
 * mountinfo gives the bind its root within the file system, and the
 * source's mount is busy until the bind is unmounted. A recursive bind of
 * /dev has its devices and /dev/shm, which are unmounted in turn.
 *
 * Prints "TMPFSMUSL OK" on success and "TMPFSMUSL FAIL (detail)" otherwise.
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
 * A tree as deep as a working directory's path allows: 512 nested
 * directories, each made from the one above it, and an unmount that frees
 * them all.
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

/** A bind of a directory of a tmpfs: the file made through it is the
 * source's, and a lock through one path meets a lock through the other. */
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

/** The bind's place and flags: ".." at its root, a read-only remount of it
 * alone, its root in mountinfo, and its source's mount busy under it. */
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

/** A recursive bind of /dev has its devices and /dev/shm, and is unmounted
 * from the bottom. */
static int rbound(void)
{
    struct stat a, b;

    if (mkdir(BIND_DEV, 0755) != 0 || mount("/dev", BIND_DEV, NULL, MS_BIND | MS_REC, NULL) != 0 ||
        stat(BIND_DEV "/null", &a) != 0 || !S_ISCHR(a.st_mode) || stat(BIND_DEV "/shm", &b) != 0 ||
        (b.st_mode & 07777) != 01777) {
        return fail("a recursive bind of /dev has its devices and /dev/shm");
    }
    if (umount2(BIND_DEV, 0) != -1 || errno != EBUSY || umount2(BIND_DEV "/shm", 0) != 0 ||
        umount2(BIND_DEV, 0) != 0 || rmdir(BIND_DEV) != 0) {
        return fail("a recursive bind is unmounted from the bottom");
    }
    return 0;
}

int main(void)
{
    if (placed() || deep() || shm() || bound() || bound_mount() || rbound()) {
        return 1;
    }
    printf("TMPFSMUSL OK\n");
    return 0;
}
