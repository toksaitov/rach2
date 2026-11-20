/**
 * lchownmusl: check lchown and the two flags of fchownat, as Linux has them.
 *
 * With no argument the rows run on the KRFFS boot root, which has no
 * symbolic links: lchown by both numbers and fchownat change a file;
 * fchownat through a descriptor of the directory, called from another
 * directory that has a file of the same name, changes the file the
 * descriptor's directory has and not the other; AT_EMPTY_PATH changes the
 * file a descriptor is open on, an O_PATH one included, and the working
 * directory; and each error of the flags comes before the path is read.
 * With a directory argument, which the machine.ext2 cases give as /tmp
 * on an ext2 root, the rows run there and a symbolic link is required: lchown
 * and AT_SYMLINK_NOFOLLOW change the link and leave its target alone, chown
 * changes the target, an O_PATH descriptor opened with O_NOFOLLOW refers to
 * the link for AT_EMPTY_PATH, and a dangling link can be changed and not
 * followed. A relative directory is taken from the working directory, as a
 * native run gives `.`, its workspace (tests/native/Readme.md), and a
 * NAME=VALUE argument, such as the `root=PATH` a native run appends, is not
 * the directory.
 *
 * Either way the rows run in lchownwork, a directory of mode 0755 the
 * program makes in the one named and removes. The links change owner to
 * accounts other than root, and in a sticky directory others may write, as
 * /tmp is, fs.protected_symlinks lets nobody but the link's owner or the
 * directory's follow such a link, root included, as Linux's may_follow_link
 * does; chown through it would be EACCES.
 *
 * The program runs as root. It prints `LCHOWNMUSL OK`, followed by
 * " (links)" when the link rows ran, and exits 0 on success, and prints
 * `LCHOWNMUSL FAIL (detail)` and exits 1 otherwise. Writes etude's report
 * records (../probe_report.h) for the case ETUDE_CASE names, an assertion for
 * each check `checks` lists, with the branch `links` present or absent as the
 * link rows run; a failure leaves the checks after it not run.
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

#include "../probe_args.h"
#include "../probe_report.h"

/* fchown of 32-bit ids: fchown32 on i386, whose fchown takes 16-bit ones,
 * and fchown on x86-64, which has only that. */
#ifndef SYS_fchown32
#define SYS_fchown32 SYS_fchown
#endif

/* The directory the rows run in, which the program makes in the one
 * named. */
static char work[256];

/* The names the rows make in `work`, and remove. */
static char file[sizeof(work) + 16], link_name[sizeof(work) + 16], dangling[sizeof(work) + 16],
    subdir[sizeof(work) + 16];

/* A second directory there, and in it a second lchown.tmp. */
static char decoy_dir[sizeof(work) + 16], decoy[sizeof(decoy_dir) + sizeof("/lchown.tmp")];

/* The checks, one assertion each, in the order the program makes them, and
 * the text it prints for each that fails. */
enum {
    WORKING_DIR,
    MAKE_FILE,
    SYMLINK,
    LCHOWN,
    LCHOWN_NUMBER,
    NOFOLLOW_FILE,
    THROUGH_DESCRIPTOR,
    EMPTY_PATH_O_PATH,
    EMPTY_PATH_FILE,
    EMPTY_PATH_CWD,
    FLAG_EINVAL,
    EMPTY_PATH_EBADF,
    PROC_SELF,
    DEV_NULL,
    SYNTHETIC,
    SYNTHETIC_ACCESS,
    LINK_OWNER,
    CHOWN_FOLLOWS,
    NOFOLLOW_LINK,
    FCHOWNAT_FOLLOWS,
    O_PATH_LINK,
    SETID_KEPT,
    SETID_CLEARED,
    DANGLING,
    CHECKS
};

static const struct {
    const char *id;
    const char *text;
} checks[CHECKS] = {
    {"working-dir", "the working directory"},
    {"make-file", "make the directory and the file"},
    {"symlink", "a symbolic link where the file system has them"},
    {"lchown", "lchown on a file"},
    {"lchown-number", "lchown by number"},
    {"nofollow-file", "AT_SYMLINK_NOFOLLOW on a file"},
    {"through-descriptor",
     "fchownat through a directory descriptor, from a directory with a decoy"},
    {"empty-path-o-path", "AT_EMPTY_PATH on an O_PATH descriptor"},
    {"empty-path-file", "AT_EMPTY_PATH on an open file"},
    {"empty-path-cwd", "AT_EMPTY_PATH on the working directory"},
    {"flag-einval", "a flag Linux does not define, before the path"},
    {"empty-path-ebadf", "AT_EMPTY_PATH on a descriptor that is not open"},
    {"proc-self", "the /proc/self link"},
    {"dev-null", "an O_PATH descriptor on /dev/null changes the device's owner"},
    {"synthetic", "chmod, chown and utimensat of /proc, /sys and /dev nodes"},
    {"synthetic-access", "the mode root gives /proc and /sys nodes governs access"},
    {"link-owner", "lchown changes the link and not its target"},
    {"chown-follows", "chown follows the link"},
    {"nofollow-link", "AT_SYMLINK_NOFOLLOW changes the link"},
    {"fchownat-follows", "fchownat without the flag follows the link"},
    {"o-path-link", "an O_PATH descriptor on the link"},
    {"setid-kept", "the target keeps its set-id bits"},
    {"setid-cleared", "a change through the link clears the target's set-id bits"},
    {"dangling", "a dangling link"},
};

/* Whether the directory has symbolic links: 1 or 0 once `symlink` has
 * answered, and -1 before. The checks from `LINK_OWNER` on are those of the
 * branch `links` with them. */
static int links = -1;

/** The branch of the group `links`: whether `symlink` succeeded, or before it
 * answered, whether a directory was given, which requires links. */
static int with_links(void)
{
    return links >= 0 ? links : probe_words() > 0;
}

/** In the process that began the report, report the branch of the group
 * `links`, every check of it before `check` as held, `check` as failed with
 * its text, those after it as not run, and the report's end; with `check`
 * CHECKS, every check held. */
static void report_at(int check)
{
    int i;

    if (report_ended || report_getpid() != report_owner) {
        return;
    }
    report_branch("links", with_links() ? "present" : "absent");
    for (i = 0; i < CHECKS; i++) {
        if (i >= LINK_OWNER && !with_links()) {
            continue;
        }
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
    printf("LCHOWNMUSL FAIL (%s)\n", checks[check].text);
    report_at(check);

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
 * Whether fchownat of lchown.tmp through a descriptor of `dir`, called from
 * `decoy_dir`, which has a lchown.tmp of its own, gives `file` the new owner
 * and group and leaves the decoy's as they were. Makes the decoy and returns
 * with the working directory at `dir`.
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

/**
 * Check the forms that change the file itself, and the descriptor forms, in
 * `dir`, the working directory. Returns 0, or 1 once the failure is reported.
 */
static int file_rows(const char *dir)
{
    int pfd, ffd;

    if (lchown(file, 1000, 1001) != 0 || !owner_is(file, 1, 1000) || !group_is(file, 1001)) {
        return fail(LCHOWN);
    }
    if (syscall(SYS_lchown, file, 1100, 1101) != 0 || !owner_is(file, 1, 1100)) {
        return fail(LCHOWN_NUMBER);
    }
    if (fchownat(AT_FDCWD, file, 1200, (gid_t)-1, AT_SYMLINK_NOFOLLOW) != 0 ||
        !owner_is(file, 1, 1200) || !group_is(file, 1101)) {
        return fail(NOFOLLOW_FILE);
    }
    if (!through_descriptor(dir)) {
        return fail(THROUGH_DESCRIPTOR);
    }
    pfd = open(file, O_PATH);
    if (pfd < 0 || fchownat(pfd, "", 1400, 1400, AT_EMPTY_PATH) != 0 || !owner_is(file, 1, 1400) ||
        !refused(syscall(SYS_fchown32, pfd, 0, 0), EBADF)) {
        return fail(EMPTY_PATH_O_PATH);
    }
    close(pfd);
    ffd = open(file, O_RDONLY);
    if (ffd < 0 || fchownat(ffd, "", 1500, (gid_t)-1, AT_EMPTY_PATH) != 0 ||
        !owner_is(file, 1, 1500) || !refused(fchownat(ffd, "", 0, 0, 0), ENOENT)) {
        return fail(EMPTY_PATH_FILE);
    }
    close(ffd);
    if (mkdir(subdir, 0755) != 0 || chdir(subdir) != 0 ||
        fchownat(AT_FDCWD, "", 1600, 1600, AT_EMPTY_PATH) != 0 || chdir(dir) != 0 ||
        !owner_is(subdir, 1, 1600)) {
        return fail(EMPTY_PATH_CWD);
    }
    return 0;
}

/* A file of /sys the rows give an owner and set back. */
#define LOCKDOWN "/sys/kernel/security/lockdown"

/**
 * Check chmod, chown and utimensat of /proc, /sys and /dev nodes as Linux
 * 6.12 takes them: a process's file refuses a mode with EPERM, root's too,
 * as proc_setattr does, and a file of /proc/sys a mode and an owner, as
 * proc_sys_setattr does; /proc/version keeps a mode and times, the
 * lockdown file of /sys an owner, and /dev/null times, as their inodes
 * keep them; the mode and the owner are set back. Returns 0, or 1 once the
 * failure is reported.
 */
static int synthetic_rows(void)
{
    struct timespec times[2] = {{7, 0}, {8, 0}};
    struct stat st;
    int ok;

    if (!refused(chmod("/proc/self/status", 0600), EPERM) ||
        !refused(chmod("/proc/sys/kernel/hostname", 0600), EPERM) ||
        !refused(chown("/proc/sys/kernel/hostname", 0, 0), EPERM)) {
        return fail(SYNTHETIC);
    }
    ok = chmod("/proc/version", 0400) == 0 && stat("/proc/version", &st) == 0 &&
         (st.st_mode & 07777) == 0400;
    ok = chmod("/proc/version", 0444) == 0 && ok;
    ok = ok && utimensat(AT_FDCWD, "/proc/version", times, 0) == 0 &&
         stat("/proc/version", &st) == 0 && st.st_atime == 7 && st.st_mtime == 8;
    ok = ok && chown(LOCKDOWN, 1000, 1001) == 0 && owner_is(LOCKDOWN, 1, 1000) &&
         group_is(LOCKDOWN, 1001);
    ok = chown(LOCKDOWN, 0, 0) == 0 && ok;
    ok = ok && utimensat(AT_FDCWD, "/dev/null", NULL, 0) == 0;
    return ok ? 0 : fail(SYNTHETIC);
}

/** The checks an ordinary user's child makes after `synthetic_access_rows`
 * has made /proc/version 0400 and /sys/kernel 0700: open and access of the
 * one and a search of the other are EACCES. 0 when they all are. */
static int closed_to_user(void)
{
    struct stat st;

    return refused(open("/proc/version", O_RDONLY), EACCES) &&
                   refused(access("/proc/version", R_OK), EACCES) &&
                   refused(stat(LOCKDOWN, &st), EACCES)
               ? 0
               : 1;
}

/** The check an ordinary user's child makes once the lockdown file is
 * 0666: it opens for writing, and a word that names no level is EINVAL,
 * with no capability asked of the writer, as on Linux. 0 when both hold. */
static int open_to_user(void)
{
    int fd = open(LOCKDOWN, O_WRONLY);
    int ok;

    if (fd < 0) {
        return 1;
    }
    ok = refused(write(fd, "bogus\n", 6), EINVAL);
    close(fd);
    return ok ? 0 : 1;
}

/** Run `check` in a child with the ids 1000 and 1001, which have no
 * capabilities; whether it exited 0. */
static int as_user(int (*check)(void))
{
    int status;
    pid_t pid;

    fflush(stdout);
    pid = fork();
    if (pid == 0) {
        _exit(setgid(1001) == 0 && setuid(1000) == 0 ? check() : 2);
    }
    return pid > 0 && waitpid(pid, &status, 0) == pid && WIFEXITED(status) &&
           WEXITSTATUS(status) == 0;
}

/**
 * Check that the mode root gives a node of /proc or /sys governs the
 * access of others, as the mode Linux keeps on its proc_dir_entry or
 * kernfs inode does: with /proc/version 0400 an ordinary user can neither
 * open nor access it, with /sys/kernel 0700 not search it, and with the
 * lockdown file 0666 open that for writing, which its 0644 refuses, and
 * write it, a word that names no level being EINVAL. Every mode is set
 * back. Returns 0, or 1 once the failure is reported.
 */
static int synthetic_access_rows(void)
{
    struct stat ver, dir, lock;
    int ok;

    if (stat("/proc/version", &ver) != 0 || stat("/sys/kernel", &dir) != 0 ||
        stat(LOCKDOWN, &lock) != 0) {
        return fail(SYNTHETIC_ACCESS);
    }
    ok = !as_user(open_to_user);
    ok = ok && chmod("/proc/version", 0400) == 0 && chmod("/sys/kernel", 0700) == 0 &&
         as_user(closed_to_user);
    ok = chmod("/sys/kernel", dir.st_mode & 07777) == 0 && ok;
    ok = ok && chmod(LOCKDOWN, 0666) == 0 && as_user(open_to_user);
    ok = chmod(LOCKDOWN, lock.st_mode & 07777) == 0 && ok;
    ok = chmod("/proc/version", ver.st_mode & 07777) == 0 && ok;
    return ok ? 0 : fail(SYNTHETIC_ACCESS);
}

/**
 * Check the errors of the flags, and /proc/self and /dev/null, whose owner
 * root changes, as Linux keeps it on their inodes, and sets back; then
 * `synthetic_rows` and `synthetic_access_rows`. Returns 0, or 1 once the
 * failure is reported.
 */
static int refusal_rows(void)
{
    int fd;
    int ok;

    if (!refused(fchownat(AT_FDCWD, file, 0, 0, 0x200), EINVAL) ||
        !refused(fchownat(AT_FDCWD, file, 0, 0, 0x400), EINVAL) ||
        !refused(syscall(SYS_fchownat, AT_FDCWD, NULL, 0, 0, 0x200), EINVAL) ||
        !refused(syscall(SYS_fchownat, AT_FDCWD, NULL, 0, 0, 0), EFAULT)) {
        return fail(FLAG_EINVAL);
    }
    if (!refused(fchownat(999, "", 0, 0, AT_EMPTY_PATH), EBADF)) {
        return fail(EMPTY_PATH_EBADF);
    }
    ok = lchown("/proc/self", 1000, -1) == 0 && owner_is("/proc/self", 0, 1000);
    if (lchown("/proc/self", 0, 0) != 0 || !ok) {
        return fail(PROC_SELF);
    }
    fd = open("/dev/null", O_PATH);
    ok = fd >= 0 && fchownat(fd, "", 1000, (gid_t)-1, AT_EMPTY_PATH) == 0 &&
         owner_is("/dev/null", 1, 1000);
    ok = fd >= 0 && fchownat(fd, "", 0, 0, AT_EMPTY_PATH) == 0 && ok;
    close(fd);
    if (!ok) {
        return fail(DEV_NULL);
    }
    return synthetic_rows() || synthetic_access_rows();
}

/**
 * Check a symbolic link and its target, on a file system that has links.
 * Returns 0, or 1 once the failure is reported.
 */
static int link_rows(void)
{
    struct stat st;
    int fd;
    int ok;

    if (chown(file, 0, 0) != 0 || lchown(link_name, 2000, 2001) != 0 ||
        !owner_is(link_name, 0, 2000) || !group_is(link_name, 2001) || !owner_is(file, 1, 0)) {
        return fail(LINK_OWNER);
    }
    if (chown(link_name, 2100, (gid_t)-1) != 0 || !owner_is(file, 1, 2100) ||
        !owner_is(link_name, 0, 2000)) {
        return fail(CHOWN_FOLLOWS);
    }
    if (fchownat(AT_FDCWD, link_name, 2200, (gid_t)-1, AT_SYMLINK_NOFOLLOW) != 0 ||
        !owner_is(link_name, 0, 2200) || !owner_is(file, 1, 2100)) {
        return fail(NOFOLLOW_LINK);
    }
    if (fchownat(AT_FDCWD, link_name, 2300, (gid_t)-1, 0) != 0 || !owner_is(file, 1, 2300) ||
        !owner_is(link_name, 0, 2200)) {
        return fail(FCHOWNAT_FOLLOWS);
    }
    fd = open(link_name, O_PATH | O_NOFOLLOW);
    ok = fd >= 0 && fstat(fd, &st) == 0 && S_ISLNK(st.st_mode) &&
         fchownat(fd, "", 2400, (gid_t)-1, AT_EMPTY_PATH) == 0 && owner_is(link_name, 0, 2400) &&
         owner_is(file, 1, 2300);
    close(fd);
    if (!ok) {
        return fail(O_PATH_LINK);
    }
    if (chmod(file, 04755) != 0 || lchown(link_name, 2500, (gid_t)-1) != 0 ||
        stat(file, &st) != 0 || (st.st_mode & 07777) != 04755) {
        return fail(SETID_KEPT);
    }
    if (chown(link_name, 2600, (gid_t)-1) != 0 || stat(file, &st) != 0 ||
        (st.st_mode & 07777) != 0755) {
        return fail(SETID_CLEARED);
    }
    if (symlink("lchown.none", dangling) != 0 || lchown(dangling, 2700, (gid_t)-1) != 0 ||
        !owner_is(dangling, 0, 2700) || !refused(chown(dangling, 0, 0), ENOENT)) {
        return fail(DANGLING);
    }
    return 0;
}

/** Remove what the rows made, from the root, so that the working directory
 * is not in `work` when it goes. */
static void tidy(void)
{
    (void)chdir("/");
    unlink(dangling);
    unlink(link_name);
    unlink(file);
    rmdir(subdir);
    unlink(decoy);
    rmdir(decoy_dir);
    rmdir(work);
}

int main(int argc, char **argv)
{
    const char *dir;
    const char *sep;
    char where[200];
    int given;
    int fd;
    int rc;

    probe_args(argc, argv);
    report_begin(report_case(environ, "guest.default.lchownmusl"));
    given = probe_words() > 0;
    dir = given ? probe_word(1) : "/";
    if (dir[0] != '/') {
        if (getcwd(where, sizeof(where)) == NULL) {
            return fail(WORKING_DIR);
        }
        snprintf(where + strlen(where), sizeof(where) - strlen(where), "/%s", dir);
        dir = where;
    }
    sep = dir[strlen(dir) - 1] == '/' ? "" : "/";
    snprintf(work, sizeof(work), "%s%slchownwork", dir, sep);
    dir = work;
    snprintf(file, sizeof(file), "%s/lchown.tmp", dir);
    snprintf(link_name, sizeof(link_name), "%s/lchown.lnk", dir);
    snprintf(dangling, sizeof(dangling), "%s/lchown.dng", dir);
    snprintf(subdir, sizeof(subdir), "%s/lchowndir", dir);
    snprintf(decoy_dir, sizeof(decoy_dir), "%s/lchowndecoy", dir);
    snprintf(decoy, sizeof(decoy), "%s/lchown.tmp", decoy_dir);
    /* Made here or refused: an existing path, perhaps a link, is never
     * walked, and tidy removes only what this run made. */
    if (mkdir(work, 0755) != 0) {
        return fail(MAKE_FILE);
    }
    fd = open(file, O_WRONLY | O_CREAT | O_TRUNC, 0644);
    if (fd < 0 || chdir(dir) != 0) {
        tidy();
        return fail(MAKE_FILE);
    }
    close(fd);
    links = symlink("lchown.tmp", link_name) == 0;
    if (!links && (given || errno != EPERM)) {
        tidy();
        return fail(SYMLINK);
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
    report_at(CHECKS);
    printf("LCHOWNMUSL OK%s\n", links ? " (links)" : "");
    return 0;
}
