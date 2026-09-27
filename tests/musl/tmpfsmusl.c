/**
 * tmpfs, mounted by a program and at /dev/shm.
 *
 * A tmpfs mounted with mode=0700 on a directory in /tmp has that mode at its
 * root and tmpfs's magic in statfs. A file is written and read back; a
 * directory, a hard link and a symbolic link are made and read; a rename
 * replaces a file whose other name keeps the old bytes. /proc/mounts has the
 * mount's line and /proc/filesystems has "nodev\ttmpfs". umount2 refuses the
 * mount while a file on it is open and succeeds after the close, and a new
 * mount there is empty. /dev/shm is a directory of mode 1777 that /dev lists
 * as one, with tmpfs's magic and its line in /proc/mounts with nosuid and
 * nodev; a file is made, written, read and removed there as shm_open makes
 * one; a working directory there climbs to /dev by "..", and umount2
 * refuses /dev/shm with EBUSY.
 *
 * Prints "TMPFSMUSL OK" on success and "TMPFSMUSL FAIL (detail)" otherwise.
 */

#define _GNU_SOURCE
#include <dirent.h>
#include <errno.h>
#include <fcntl.h>
#include <stdio.h>
#include <string.h>
#include <sys/mount.h>
#include <sys/stat.h>
#include <sys/statfs.h>
#include <unistd.h>

#define DIR_PATH    "/tmp/tmpfsmusl"
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
    char target[16];
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
        readlink(DIR_PATH "/l", target, sizeof(target)) != 1 || !holds(DIR_PATH "/l", "tmpfs")) {
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
    if (umount2("/dev/shm", 0) != -1 || errno != EBUSY) {
        return fail("an unmount of /dev/shm");
    }
    return 0;
}

int main(void)
{
    if (placed() || shm()) {
        return 1;
    }
    printf("TMPFSMUSL OK\n");
    return 0;
}
