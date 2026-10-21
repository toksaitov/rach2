/**
 * pathmaxmusl: check paths of up to PATH_MAX in every kind of call that takes
 * one. PATH_MAX is 4096 bytes with the terminator.
 *
 * The kernel copies each path into a page and joins a relative one to the
 * working directory or to a directory descriptor's path in that page, so the
 * cases are the edges of those two: a path of 4095 bytes works and one of
 * 4096 without a terminator fails with ENAMETOOLONG, and the same for the
 * joined path. The long paths are padded with `./` components, which leave
 * the file the same, so each call reaches something the image has.
 *
 * The working directory's path is kept in the process's fs_struct, the
 * record of its directories and umask that clone with CLONE_FS shares
 * between threads: in the record itself up to 63 bytes, and in the heap past
 * that. The probe makes a directory whose path is 89 bytes long, enters it,
 * reads it back with `getcwd`, forks a child, whose copy of the record must
 * read the same, and leaves it for a short one again, so both stores and
 * fork's copy of the long one are exercised.
 *
 * The rest reach the other copies: `execve` of the root's `hello` by a long
 * path, `symlink` with a target of 4095 and of 4096 bytes, `readlink` of
 * /proc/self, and the namespace calls.
 *
 * Prints `PATHMAXMUSL OK` and exits 0 on success, and prints
 * `PATHMAXMUSL FAIL (detail)` and exits 1 otherwise. Writes etude's report
 * records (../probe_report.h) for the case ETUDE_CASE names, an assertion for
 * each check `checks` lists; a failure leaves the checks after it not run.
 */

#define _GNU_SOURCE
#include <errno.h>
#include <fcntl.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/stat.h>
#include <sys/wait.h>
#include <unistd.h>

#include "../probe_report.h"

#define PATH_BYTES 4096

/* The directory the probe works in, and the 80-byte name under it that makes
 * the working directory's path too long to be kept in the record. */
#define WORK     "/pathmax"
#define LONGNAME "a123456789b123456789c123456789d123456789e123456789f123456789g123456789h123456789"
#define LONGDIR  WORK "/" LONGNAME

/* The long paths; a row uses two at most, and each has room for a path one
 * byte past the bound. */
static char spell_a[PATH_BYTES + 16];
static char spell_b[PATH_BYTES + 16];

/* The checks, one assertion each, in the order the program makes them, and
 * the text it prints for each that fails. */
enum {
    STAT_4095,
    OPEN_4095,
    READ_4095,
    ACCESS_4095,
    STAT_4096,
    OPEN_4096,
    DEVICE_4095,
    PROC_SELF_4095,
    PROC_SELF_PID,
    WORK_DIRS,
    CHDIR_LONG,
    GETCWD_LONG,
    GETCWD_ERANGE,
    RELATIVE_CREATE,
    RELATIVE_CREATE_LANDS,
    FORK_CWD,
    JOINED_4095,
    JOINED_4096,
    CHDIR_SHORT,
    CHDIR_ROOT,
    RELATIVE_4094,
    OPEN_LONG_DIR,
    OPENAT_4095,
    FSTATAT_4095,
    OPENAT_4096,
    MKDIR_4095,
    CREATE_4095,
    CHMOD_4095,
    RENAME_4095,
    UNLINK_4095,
    RMDIR_4095,
    SYMLINK_4095,
    SYMLINK_4096,
    EXECVE_4095,
    CHECKS
};

static const struct {
    const char *id;
    const char *text;
} checks[CHECKS] = {
    {"stat-4095", "stat of a path of 4095 bytes"},
    {"open-4095", "open of a path of 4095 bytes"},
    {"read-4095", "the file a path of 4095 bytes names"},
    {"access-4095", "access with a path of 4095 bytes"},
    {"stat-4096", "a path of 4096 bytes without its terminator is not ENAMETOOLONG"},
    {"open-4096", "open of a path of 4096 bytes is not ENAMETOOLONG"},
    {"device-4095", "a synthetic device by a path of 4095 bytes"},
    {"proc-self-4095", "readlink of /proc/self by a path of 4095 bytes"},
    {"proc-self-pid", "readlink of /proc/self does not name this process"},
    {"work-dirs", "the working directories"},
    {"chdir-long", "chdir to a directory whose path is 89 bytes"},
    {"getcwd-long", "getcwd of a working directory of 89 bytes"},
    {"getcwd-erange", "getcwd into a buffer with no room for the terminator is not ERANGE"},
    {"relative-create", "a relative create in the long working directory"},
    {"relative-create-lands", "the relative create did not land in the long working directory"},
    {"fork-cwd", "a child does not inherit the long working directory"},
    {"joined-4095", "a relative path that joins to 4095 bytes"},
    {"joined-4096", "a relative path that joins to 4096 bytes is not ENAMETOOLONG"},
    {"chdir-short", "chdir back to a short working directory"},
    {"chdir-root", "chdir to the root"},
    {"relative-4094", "a relative path of 4094 bytes from the root"},
    {"open-long-dir", "open of the long directory"},
    {"openat-4095", "openat with a path that joins to 4095 bytes"},
    {"fstatat-4095", "fstatat with a path that joins to 4095 bytes"},
    {"openat-4096", "openat with a path that joins to 4096 bytes is not ENAMETOOLONG"},
    {"mkdir-4095", "mkdir by a path of 4095 bytes"},
    {"create-4095", "create by a path of 4095 bytes"},
    {"chmod-4095", "chmod by a path of 4095 bytes"},
    {"rename-4095", "rename between two paths of 4095 bytes"},
    {"unlink-4095", "unlink by a path of 4095 bytes"},
    {"rmdir-4095", "rmdir by a path of 4095 bytes"},
    {"symlink-4095", "symlink with a target of 4095 bytes does not reach the file system"},
    {"symlink-4096", "symlink with a target of 4096 bytes is not ENAMETOOLONG"},
    {"execve-4095", "execve by a path of 4095 bytes"},
};

/** In the process that began the report, report every check before `check`
 * as held, `check` as failed with its text, those after it as not run, and
 * the report's end; with `check` CHECKS, every check held. */
static void report_at(int check)
{
    int i;

    if (report_ended || report_getpid() != report_owner) {
        return;
    }
    for (i = 0; i < CHECKS; i++) {
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
    printf("PATHMAXMUSL FAIL (%s)\n", checks[check].text);
    report_at(check);

    return 1;
}

/**
 * Write into `out` the path `lead`, then `./` components, then `tail`, of
 * `total` bytes without the terminator; one more slash makes up an odd count.
 * Returns `out`, which must have room for `total + 1` bytes.
 */
static char *spell(char *out, const char *lead, const char *tail, size_t total)
{
    size_t lead_len = strlen(lead);
    size_t tail_len = strlen(tail);
    size_t at = lead_len;

    memcpy(out, lead, lead_len);
    while (at + 2 + tail_len <= total) {
        out[at++] = '.';
        out[at++] = '/';
    }
    while (at + tail_len < total) {
        out[at++] = '/';
    }
    memcpy(out + at, tail, tail_len + 1);
    return out;
}

/** Whether a call returned -1 with `err`. */
static int refused(int rc, int err)
{
    return rc == -1 && errno == err;
}

/** Wait for `pid` and return its exit status, or -1 if it did not exit. */
static int status_of(pid_t pid)
{
    int status;

    if (waitpid(pid, &status, 0) != pid || !WIFEXITED(status)) {
        return -1;
    }
    return WEXITSTATUS(status);
}

/**
 * Check absolute paths of 4095 and 4096 bytes to a file, a device and a
 * /proc link. Returns 0, or 1 once the failure is reported.
 */
static int absolute_rows(void)
{
    struct stat st;
    char buf[32];
    int fd;
    ssize_t n;

    spell(spell_a, "/", "hello.txt", PATH_BYTES - 1);
    if (stat(spell_a, &st) != 0 || !S_ISREG(st.st_mode) || st.st_size != 13) {
        return fail(STAT_4095);
    }
    fd = open(spell_a, O_RDONLY);
    if (fd < 0) {
        return fail(OPEN_4095);
    }
    n = read(fd, buf, sizeof(buf));
    close(fd);
    if (n != 13 || memcmp(buf, "hello, world\n", 13) != 0) {
        return fail(READ_4095);
    }
    if (access(spell_a, R_OK) != 0) {
        return fail(ACCESS_4095);
    }
    spell(spell_a, "/", "hello.txt", PATH_BYTES);
    if (!refused(stat(spell_a, &st), ENAMETOOLONG)) {
        return fail(STAT_4096);
    }
    if (!refused(open(spell_a, O_RDONLY), ENAMETOOLONG)) {
        return fail(OPEN_4096);
    }
    spell(spell_a, "/dev/", "null", PATH_BYTES - 1);
    if (stat(spell_a, &st) != 0 || !S_ISCHR(st.st_mode)) {
        return fail(DEVICE_4095);
    }
    spell(spell_a, "/proc/", "self", PATH_BYTES - 1);
    n = readlink(spell_a, buf, sizeof(buf) - 1);
    if (n <= 0) {
        return fail(PROC_SELF_4095);
    }
    buf[n] = '\0';
    if (atoi(buf) != (int)getpid()) {
        return fail(PROC_SELF_PID);
    }
    return 0;
}

/**
 * Check a working directory whose path is too long for the record, a fork's
 * copy of it, and relative paths joined to it. Leaves the working directory
 * at the root. Returns 0, or 1 once the failure is reported.
 */
static int cwd_rows(void)
{
    char cwd[256];
    struct stat st;
    size_t room;
    pid_t pid;
    int fd;

    if (mkdir(WORK, 0755) != 0 || mkdir(LONGDIR, 0755) != 0) {
        return fail(WORK_DIRS);
    }
    if (chdir(LONGDIR) != 0) {
        return fail(CHDIR_LONG);
    }
    if (!getcwd(cwd, sizeof(cwd)) || strcmp(cwd, LONGDIR) != 0) {
        return fail(GETCWD_LONG);
    }
    if (getcwd(cwd, strlen(LONGDIR)) != NULL || errno != ERANGE) {
        return fail(GETCWD_ERANGE);
    }
    fd = open("made", O_WRONLY | O_CREAT | O_EXCL, 0644);
    if (fd < 0) {
        return fail(RELATIVE_CREATE);
    }
    close(fd);
    if (stat(LONGDIR "/made", &st) != 0) {
        return fail(RELATIVE_CREATE_LANDS);
    }
    pid = fork();
    if (pid == 0) {
        _exit(getcwd(cwd, sizeof(cwd)) && strcmp(cwd, LONGDIR) == 0 ? 0 : 1);
    }
    if (pid < 0 || status_of(pid) != 0) {
        return fail(FORK_CWD);
    }

    /* The joined path's own bound: the working directory, a slash and the
     * relative path must fit 4095 bytes. */
    room = PATH_BYTES - 1 - strlen(LONGDIR) - 1;
    spell(spell_a, "", "made", room);
    if (stat(spell_a, &st) != 0) {
        return fail(JOINED_4095);
    }
    spell(spell_a, "", "made", room + 1);
    if (!refused(stat(spell_a, &st), ENAMETOOLONG)) {
        return fail(JOINED_4096);
    }

    if (chdir("..") != 0 || !getcwd(cwd, sizeof(cwd)) || strcmp(cwd, WORK) != 0) {
        return fail(CHDIR_SHORT);
    }
    if (chdir("/") != 0) {
        return fail(CHDIR_ROOT);
    }
    spell(spell_a, "", "hello.txt", PATH_BYTES - 2);
    if (stat(spell_a, &st) != 0) {
        return fail(RELATIVE_4094);
    }
    return 0;
}

/**
 * Check relative paths joined to the path of a descriptor on the long
 * directory. Returns 0, or 1 once the failure is reported.
 */
static int dirfd_rows(void)
{
    struct stat st;
    size_t room;
    int dirfd;
    int fd;

    dirfd = open(LONGDIR, O_RDONLY | O_DIRECTORY);
    if (dirfd < 0) {
        return fail(OPEN_LONG_DIR);
    }
    room = PATH_BYTES - 1 - strlen(LONGDIR) - 1;
    spell(spell_a, "", "made", room);
    fd = openat(dirfd, spell_a, O_RDONLY);
    if (fd < 0) {
        close(dirfd);
        return fail(OPENAT_4095);
    }
    close(fd);
    if (fstatat(dirfd, spell_a, &st, 0) != 0) {
        close(dirfd);
        return fail(FSTATAT_4095);
    }
    spell(spell_a, "", "made", room + 1);
    if (!refused(openat(dirfd, spell_a, O_RDONLY), ENAMETOOLONG)) {
        close(dirfd);
        return fail(OPENAT_4096);
    }
    close(dirfd);
    return 0;
}

/**
 * Check the namespace calls, each by a path of 4095 bytes, and symlink with
 * targets of 4095 and 4096 bytes. Returns 0, or 1 once the failure is
 * reported.
 */
static int namespace_rows(void)
{
    struct stat st;
    int fd;

    spell(spell_a, WORK "/", "d", PATH_BYTES - 1);
    if (mkdir(spell_a, 0755) != 0 || stat(WORK "/d", &st) != 0 || !S_ISDIR(st.st_mode)) {
        return fail(MKDIR_4095);
    }
    spell(spell_a, WORK "/", "f", PATH_BYTES - 1);
    fd = open(spell_a, O_WRONLY | O_CREAT | O_EXCL, 0644);
    if (fd < 0) {
        return fail(CREATE_4095);
    }
    close(fd);
    if (chmod(spell_a, 0600) != 0 || stat(WORK "/f", &st) != 0 || (st.st_mode & 0777) != 0600) {
        return fail(CHMOD_4095);
    }
    spell(spell_b, WORK "/", "g", PATH_BYTES - 1);
    if (rename(spell_a, spell_b) != 0 || stat(WORK "/g", &st) != 0) {
        return fail(RENAME_4095);
    }
    if (unlink(spell_b) != 0 || stat(WORK "/g", &st) == 0) {
        return fail(UNLINK_4095);
    }
    spell(spell_a, WORK "/", "d", PATH_BYTES - 1);
    if (rmdir(spell_a) != 0 || stat(WORK "/d", &st) == 0) {
        return fail(RMDIR_4095);
    }

    /* A target is copied, not resolved; the KRFFS root has no links (EPERM). */
    spell(spell_a, "", "t", PATH_BYTES - 1);
    if (!refused(symlink(spell_a, WORK "/link"), EPERM)) {
        return fail(SYMLINK_4095);
    }
    spell(spell_a, "", "t", PATH_BYTES);
    if (!refused(symlink(spell_a, WORK "/link"), ENAMETOOLONG)) {
        return fail(SYMLINK_4096);
    }
    return 0;
}

/**
 * Check `execve` of the root's hello program by a path of 4095 bytes, in a
 * forked child whose standard output is its standard error, since hello
 * writes report records of its own. Returns 0, or 1 once the failure is
 * reported.
 */
static int exec_row(void)
{
    char *argv[2];
    pid_t pid;

    spell(spell_a, "/", "hello", PATH_BYTES - 1);
    pid = fork();
    if (pid == 0) {
        argv[0] = (char *)"hello";
        argv[1] = NULL;
        dup2(2, 1);
        execv(spell_a, argv);
        _exit(3);
    }
    if (pid < 0 || status_of(pid) != 0) {
        return fail(EXECVE_4095);
    }
    return 0;
}

/** Remove what the rows made, whatever they got to. */
static void clean(void)
{
    unlink(LONGDIR "/made");
    rmdir(LONGDIR);
    unlink(WORK "/f");
    unlink(WORK "/g");
    rmdir(WORK "/d");
    rmdir(WORK);
}

int main(void)
{
    int rc;

    report_begin(report_case(environ, "guest.default.pathmaxmusl"));
    clean();
    rc = absolute_rows();
    if (rc == 0) {
        rc = cwd_rows();
    }
    if (rc == 0) {
        rc = dirfd_rows();
    }
    if (rc == 0) {
        rc = namespace_rows();
    }
    if (rc == 0) {
        rc = exec_row();
    }
    chdir("/");
    clean();
    if (rc == 0) {
        report_at(CHECKS);
        printf("PATHMAXMUSL OK\n");
    }
    return rc;
}
