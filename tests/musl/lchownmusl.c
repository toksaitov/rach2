/**
 * lchown and the two flags of fchownat, with Linux's meaning.
 *
 * With no argument the rows run on the KRFFS boot root, which has no
 * symbolic links: lchown by both numbers and fchownat change a file;
 * fchownat through a descriptor of the directory, called from another
 * directory that has a file of the same name, changes the file the
 * descriptor's directory has and not the other; AT_EMPTY_PATH changes the
 * file a descriptor is open on, an O_PATH one included, and the working
 * directory; and each refusal of the flags comes before the path is read.
 * With a directory argument, which tests/system/suites/ext2.sh gives as /tmp
 * on an ext2 root, the rows run there and a symbolic link is required: lchown
 * and AT_SYMLINK_NOFOLLOW change the link and leave its target alone, chown
 * changes the target, an O_PATH descriptor opened with O_NOFOLLOW names the
 * link for AT_EMPTY_PATH, and a dangling link can be changed and not followed.
 *
 * Prints "LCHOWNMUSL OK" on success and "LCHOWNMUSL FAIL (detail)" otherwise.
 */

#define _GNU_SOURCE
#include <errno.h>
#include <fcntl.h>
#include <stdio.h>
#include <string.h>
#include <sys/stat.h>
#include <sys/syscall.h>
#include <unistd.h>

/* fchown of 32-bit ids: fchown32 on i386, whose fchown takes 16-bit ones,
 * and fchown on x86-64, which has only that. */
#ifndef SYS_fchown32
#define SYS_fchown32 SYS_fchown
#endif

/* The names the rows make in the directory, and remove. */
static char file[256], link_name[256], dangling[256], subdir[256];

/* A second directory there, and in it a second lchown.tmp. */
static char decoy_dir[256], decoy[sizeof(decoy_dir) + sizeof("/lchown.tmp")];

/** Report one failed check and return 1, the probe's failure status. */
static int fail(const char *what)
{
    printf("LCHOWNMUSL FAIL (%s)\n", what);

    return 1;
}

/** Whether a call returned -1 with `err`. */
static int refused(long rc, int err)
{
    return rc == -1 && errno == err;
}

/** Whether `path`, through a final link when `follow`, is owned by `uid`. */
static int owner_is(const char *path, int follow, uid_t uid)
{
    struct stat st;

    if ((follow ? stat(path, &st) : lstat(path, &st)) != 0) {
        return 0;
    }
    return st.st_uid == uid;
}

/** Whether `path`'s group is `gid`. */
static int group_is(const char *path, gid_t gid)
{
    struct stat st;

    return lstat(path, &st) == 0 && st.st_gid == gid;
}

/**
 * fchownat of lchown.tmp through a descriptor of `dir`, called from
 * `decoy_dir`, which has a lchown.tmp of its own: `file` takes the new owner
 * and group, and the decoy keeps its own.
 */
static int through_descriptor(const char *dir)
{
    struct stat before, after;
    int dfd, fd;
    int ok;

    if (mkdir(decoy_dir, 0755) != 0) {
        return 0;
    }
    fd = open(decoy, O_WRONLY | O_CREAT | O_TRUNC, 0644);
    if (fd < 0) {
        return 0;
    }
    close(fd);
    dfd = open(dir, O_RDONLY | O_DIRECTORY);
    ok = dfd >= 0 && lstat(decoy, &before) == 0 && chdir(decoy_dir) == 0 &&
         fchownat(dfd, "lchown.tmp", 1300, 1300, 0) == 0;
    if (dfd >= 0) {
        close(dfd);
    }
    return chdir(dir) == 0 && ok && owner_is(file, 1, 1300) && group_is(file, 1300) &&
           lstat(decoy, &after) == 0 && after.st_uid == before.st_uid &&
           after.st_gid == before.st_gid;
}

/** The forms that change the file itself, and the descriptor forms. */
static int file_rows(const char *dir)
{
    int pfd, ffd;

    if (lchown(file, 1000, 1001) != 0 || !owner_is(file, 1, 1000) || !group_is(file, 1001)) {
        return fail("lchown on a file");
    }
    if (syscall(SYS_lchown, file, 1100, 1101) != 0 || !owner_is(file, 1, 1100)) {
        return fail("lchown by call 16");
    }
    if (fchownat(AT_FDCWD, file, 1200, (gid_t)-1, AT_SYMLINK_NOFOLLOW) != 0 ||
        !owner_is(file, 1, 1200) || !group_is(file, 1101)) {
        return fail("AT_SYMLINK_NOFOLLOW on a file");
    }
    if (!through_descriptor(dir)) {
        return fail("fchownat through a directory descriptor, from a directory with a decoy");
    }
    pfd = open(file, O_PATH);
    if (pfd < 0 || fchownat(pfd, "", 1400, 1400, AT_EMPTY_PATH) != 0 || !owner_is(file, 1, 1400) ||
        !refused(syscall(SYS_fchown32, pfd, 0, 0), EBADF)) {
        return fail("AT_EMPTY_PATH on an O_PATH descriptor");
    }
    close(pfd);
    ffd = open(file, O_RDONLY);
    if (ffd < 0 || fchownat(ffd, "", 1500, (gid_t)-1, AT_EMPTY_PATH) != 0 ||
        !owner_is(file, 1, 1500) || !refused(fchownat(ffd, "", 0, 0, 0), ENOENT)) {
        return fail("AT_EMPTY_PATH on an open file");
    }
    close(ffd);
    if (mkdir(subdir, 0755) != 0 || chdir(subdir) != 0 ||
        fchownat(AT_FDCWD, "", 1600, 1600, AT_EMPTY_PATH) != 0 || chdir(dir) != 0 ||
        !owner_is(subdir, 1, 1600)) {
        return fail("AT_EMPTY_PATH on the working directory");
    }
    return 0;
}

/** The refusals, and the nodes of fixed ownership. */
static int refusal_rows(void)
{
    int fd;
    int ok;

    if (!refused(fchownat(AT_FDCWD, file, 0, 0, 0x200), EINVAL) ||
        !refused(fchownat(AT_FDCWD, file, 0, 0, 0x400), EINVAL) ||
        !refused(syscall(SYS_fchownat, AT_FDCWD, NULL, 0, 0, 0x200), EINVAL) ||
        !refused(syscall(SYS_fchownat, AT_FDCWD, NULL, 0, 0, 0), EFAULT)) {
        return fail("a flag Linux does not define, before the path");
    }
    if (!refused(fchownat(999, "", 0, 0, AT_EMPTY_PATH), EBADF)) {
        return fail("AT_EMPTY_PATH on a descriptor that is not open");
    }
    if (lchown("/proc/self", 0, 0) != 0 || !refused(lchown("/proc/self", 1000, -1), EPERM)) {
        return fail("the /proc/self link");
    }
    fd = open("/dev/null", O_PATH);
    ok = fd >= 0 && fchownat(fd, "", 0, 0, AT_EMPTY_PATH) == 0 &&
         refused(fchownat(fd, "", 1000, (gid_t)-1, AT_EMPTY_PATH), EPERM);
    close(fd);
    if (!ok) {
        return fail("an O_PATH descriptor on /dev/null");
    }
    return 0;
}

/** A symbolic link and its target, where the file system has links. */
static int link_rows(void)
{
    struct stat st;
    int fd;
    int ok;

    if (chown(file, 0, 0) != 0 || lchown(link_name, 2000, 2001) != 0 ||
        !owner_is(link_name, 0, 2000) || !group_is(link_name, 2001) || !owner_is(file, 1, 0)) {
        return fail("lchown changes the link and not its target");
    }
    if (chown(link_name, 2100, (gid_t)-1) != 0 || !owner_is(file, 1, 2100) ||
        !owner_is(link_name, 0, 2000)) {
        return fail("chown follows the link");
    }
    if (fchownat(AT_FDCWD, link_name, 2200, (gid_t)-1, AT_SYMLINK_NOFOLLOW) != 0 ||
        !owner_is(link_name, 0, 2200) || !owner_is(file, 1, 2100)) {
        return fail("AT_SYMLINK_NOFOLLOW changes the link");
    }
    if (fchownat(AT_FDCWD, link_name, 2300, (gid_t)-1, 0) != 0 || !owner_is(file, 1, 2300) ||
        !owner_is(link_name, 0, 2200)) {
        return fail("fchownat without the flag follows the link");
    }
    fd = open(link_name, O_PATH | O_NOFOLLOW);
    ok = fd >= 0 && fstat(fd, &st) == 0 && S_ISLNK(st.st_mode) &&
         fchownat(fd, "", 2400, (gid_t)-1, AT_EMPTY_PATH) == 0 && owner_is(link_name, 0, 2400) &&
         owner_is(file, 1, 2300);
    close(fd);
    if (!ok) {
        return fail("an O_PATH descriptor on the link");
    }
    if (chmod(file, 04755) != 0 || lchown(link_name, 2500, (gid_t)-1) != 0 ||
        stat(file, &st) != 0 || (st.st_mode & 07777) != 04755) {
        return fail("the target keeps its set-id bits");
    }
    if (chown(link_name, 2600, (gid_t)-1) != 0 || stat(file, &st) != 0 ||
        (st.st_mode & 07777) != 0755) {
        return fail("a change through the link clears the target's set-id bits");
    }
    if (symlink("lchown.none", dangling) != 0 || lchown(dangling, 2700, (gid_t)-1) != 0 ||
        !owner_is(dangling, 0, 2700) || !refused(chown(dangling, 0, 0), ENOENT)) {
        return fail("a dangling link");
    }
    return 0;
}

/** Remove what the rows made. */
static void tidy(void)
{
    unlink(dangling);
    unlink(link_name);
    unlink(file);
    rmdir(subdir);
    unlink(decoy);
    rmdir(decoy_dir);
}

int main(int argc, char **argv)
{
    const char *dir = argc > 1 ? argv[1] : "/";
    const char *sep = dir[strlen(dir) - 1] == '/' ? "" : "/";
    int links;
    int fd;
    int rc;

    snprintf(file, sizeof(file), "%s%slchown.tmp", dir, sep);
    snprintf(link_name, sizeof(link_name), "%s%slchown.lnk", dir, sep);
    snprintf(dangling, sizeof(dangling), "%s%slchown.dng", dir, sep);
    snprintf(subdir, sizeof(subdir), "%s%slchowndir", dir, sep);
    snprintf(decoy_dir, sizeof(decoy_dir), "%s%slchowndecoy", dir, sep);
    snprintf(decoy, sizeof(decoy), "%s/lchown.tmp", decoy_dir);
    tidy();
    fd = open(file, O_WRONLY | O_CREAT | O_TRUNC, 0644);
    if (fd < 0 || chdir(dir) != 0) {
        return fail("make the file");
    }
    close(fd);
    links = symlink("lchown.tmp", link_name) == 0;
    if (!links && (argc > 1 || errno != EPERM)) {
        tidy();
        return fail("a symbolic link where the file system has them");
    }
    rc = file_rows(dir);
    if (rc == 0) {
        rc = refusal_rows();
    }
    if (rc == 0 && links) {
        rc = link_rows();
    }
    tidy();
    if (rc != 0) {
        return rc;
    }
    printf("LCHOWNMUSL OK%s\n", links ? " (links)" : "");
    return 0;
}
