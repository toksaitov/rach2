/**
 * Paths of up to PATH_MAX, 4096 bytes with the terminator, in every kind of
 * call that takes one.
 *
 * The kernel copies each path into a page and joins a relative one to the
 * working directory or to a directory descriptor's path in that page, so the
 * cases are the edges of those two: a path of 4095 bytes works and one of
 * 4096 without a terminator answers ENAMETOOLONG, and the same for the
 * joined path. The long spellings pad with `./` components, which name the
 * same file, so each call reaches something the image has.
 *
 * The working directory's path lives in the process record up to 63 bytes
 * and in the heap past that. The probe makes a directory whose path is 89
 * bytes long, enters it, reads it back with `getcwd`, forks a child that must
 * read the same, and leaves it for a short one again, so both stores and the
 * copy between them are exercised.
 *
 * The rest reach the other copies: `execve` of the root's `hello` by a long
 * spelling, `symlink` with a target of 4095 and of 4096 bytes, `readlink` of
 * /proc/self, and the namespace calls.
 *
 * Prints "PATHMAXMUSL OK" on success and "PATHMAXMUSL FAIL (detail)" otherwise.
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

#define PATH_BYTES 4096

/* The directory the probe works in, and the 80-byte name under it that makes
 * the working directory's path too long to be kept in the record. */
#define WORK     "/pathmax"
#define LONGNAME "a123456789b123456789c123456789d123456789e123456789f123456789g123456789h123456789"
#define LONGDIR  WORK "/" LONGNAME

/* Two spellings at once at most; each has room for a path past the bound. */
static char spell_a[PATH_BYTES + 16];
static char spell_b[PATH_BYTES + 16];

/** Report one failed check and return 1, the probe's failure status. */
static int fail(const char *what)
{
    printf("PATHMAXMUSL FAIL (%s)\n", what);

    return 1;
}

/**
 * `lead`, then `./` components, then `tail`, `total` bytes long without the
 * terminator, into `out`: one more slash makes up an odd count.
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

/** The rows on absolute paths from the root. */
static int absolute_rows(void)
{
    struct stat st;
    char buf[32];
    int fd;
    ssize_t n;

    spell(spell_a, "/", "hello.txt", PATH_BYTES - 1);
    if (stat(spell_a, &st) != 0 || !S_ISREG(st.st_mode) || st.st_size != 13) {
        return fail("stat of a path of 4095 bytes");
    }
    fd = open(spell_a, O_RDONLY);
    if (fd < 0) {
        return fail("open of a path of 4095 bytes");
    }
    n = read(fd, buf, sizeof(buf));
    close(fd);
    if (n != 13 || memcmp(buf, "hello, world\n", 13) != 0) {
        return fail("the file a path of 4095 bytes names");
    }
    if (access(spell_a, R_OK) != 0) {
        return fail("access with a path of 4095 bytes");
    }
    spell(spell_a, "/", "hello.txt", PATH_BYTES);
    if (!refused(stat(spell_a, &st), ENAMETOOLONG)) {
        return fail("a path of 4096 bytes without its terminator is not ENAMETOOLONG");
    }
    if (!refused(open(spell_a, O_RDONLY), ENAMETOOLONG)) {
        return fail("open of a path of 4096 bytes is not ENAMETOOLONG");
    }
    spell(spell_a, "/dev/", "null", PATH_BYTES - 1);
    if (stat(spell_a, &st) != 0 || !S_ISCHR(st.st_mode)) {
        return fail("a synthetic device by a path of 4095 bytes");
    }
    spell(spell_a, "/proc/", "self", PATH_BYTES - 1);
    n = readlink(spell_a, buf, sizeof(buf) - 1);
    if (n <= 0) {
        return fail("readlink of /proc/self by a path of 4095 bytes");
    }
    buf[n] = '\0';
    if (atoi(buf) != (int)getpid()) {
        return fail("readlink of /proc/self does not name this process");
    }
    return 0;
}

/** The rows on the working directory and on paths joined to it. */
static int cwd_rows(void)
{
    char cwd[256];
    struct stat st;
    size_t room;
    pid_t pid;
    int fd;

    if (mkdir(WORK, 0755) != 0 || mkdir(LONGDIR, 0755) != 0) {
        return fail("the working directories");
    }
    if (chdir(LONGDIR) != 0) {
        return fail("chdir to a directory whose path is 89 bytes");
    }
    if (!getcwd(cwd, sizeof(cwd)) || strcmp(cwd, LONGDIR) != 0) {
        return fail("getcwd of a working directory of 89 bytes");
    }
    if (getcwd(cwd, strlen(LONGDIR)) != NULL || errno != ERANGE) {
        return fail("getcwd into a buffer with no room for the terminator is not ERANGE");
    }
    fd = open("made", O_WRONLY | O_CREAT | O_EXCL, 0644);
    if (fd < 0) {
        return fail("a relative create in the long working directory");
    }
    close(fd);
    if (stat(LONGDIR "/made", &st) != 0) {
        return fail("the relative create did not land in the long working directory");
    }
    pid = fork();
    if (pid == 0) {
        _exit(getcwd(cwd, sizeof(cwd)) && strcmp(cwd, LONGDIR) == 0 ? 0 : 1);
    }
    if (pid < 0 || status_of(pid) != 0) {
        return fail("a child does not inherit the long working directory");
    }

    /* The joined path's own bound: the working directory, a slash and the
     * relative path must fit 4095 bytes. */
    room = PATH_BYTES - 1 - strlen(LONGDIR) - 1;
    spell(spell_a, "", "made", room);
    if (stat(spell_a, &st) != 0) {
        return fail("a relative path that joins to 4095 bytes");
    }
    spell(spell_a, "", "made", room + 1);
    if (!refused(stat(spell_a, &st), ENAMETOOLONG)) {
        return fail("a relative path that joins to 4096 bytes is not ENAMETOOLONG");
    }

    if (chdir("..") != 0 || !getcwd(cwd, sizeof(cwd)) || strcmp(cwd, WORK) != 0) {
        return fail("chdir back to a short working directory");
    }
    if (chdir("/") != 0) {
        return fail("chdir to the root");
    }
    spell(spell_a, "", "hello.txt", PATH_BYTES - 2);
    if (stat(spell_a, &st) != 0) {
        return fail("a relative path of 4094 bytes from the root");
    }
    return 0;
}

/** The rows on a directory descriptor's path joined to a relative one. */
static int dirfd_rows(void)
{
    struct stat st;
    size_t room;
    int dirfd;
    int fd;

    dirfd = open(LONGDIR, O_RDONLY | O_DIRECTORY);
    if (dirfd < 0) {
        return fail("open of the long directory");
    }
    room = PATH_BYTES - 1 - strlen(LONGDIR) - 1;
    spell(spell_a, "", "made", room);
    fd = openat(dirfd, spell_a, O_RDONLY);
    if (fd < 0) {
        close(dirfd);
        return fail("openat with a path that joins to 4095 bytes");
    }
    close(fd);
    if (fstatat(dirfd, spell_a, &st, 0) != 0) {
        close(dirfd);
        return fail("fstatat with a path that joins to 4095 bytes");
    }
    spell(spell_a, "", "made", room + 1);
    if (!refused(openat(dirfd, spell_a, O_RDONLY), ENAMETOOLONG)) {
        close(dirfd);
        return fail("openat with a path that joins to 4096 bytes is not ENAMETOOLONG");
    }
    close(dirfd);
    return 0;
}

/** The namespace calls, each by a spelling of 4095 bytes. */
static int namespace_rows(void)
{
    struct stat st;
    int fd;

    spell(spell_a, WORK "/", "d", PATH_BYTES - 1);
    if (mkdir(spell_a, 0755) != 0 || stat(WORK "/d", &st) != 0 || !S_ISDIR(st.st_mode)) {
        return fail("mkdir by a path of 4095 bytes");
    }
    spell(spell_a, WORK "/", "f", PATH_BYTES - 1);
    fd = open(spell_a, O_WRONLY | O_CREAT | O_EXCL, 0644);
    if (fd < 0) {
        return fail("create by a path of 4095 bytes");
    }
    close(fd);
    if (chmod(spell_a, 0600) != 0 || stat(WORK "/f", &st) != 0 || (st.st_mode & 0777) != 0600) {
        return fail("chmod by a path of 4095 bytes");
    }
    spell(spell_b, WORK "/", "g", PATH_BYTES - 1);
    if (rename(spell_a, spell_b) != 0 || stat(WORK "/g", &st) != 0) {
        return fail("rename between two paths of 4095 bytes");
    }
    if (unlink(spell_b) != 0 || stat(WORK "/g", &st) == 0) {
        return fail("unlink by a path of 4095 bytes");
    }
    spell(spell_a, WORK "/", "d", PATH_BYTES - 1);
    if (rmdir(spell_a) != 0 || stat(WORK "/d", &st) == 0) {
        return fail("rmdir by a path of 4095 bytes");
    }

    /* A target is copied, not resolved, so its bound is the copy's: the root
     * has no links, so a target that fits reaches the file system and is
     * refused there, and one that does not is refused first. */
    spell(spell_a, "", "t", PATH_BYTES - 1);
    if (!refused(symlink(spell_a, WORK "/link"), EPERM)) {
        return fail("symlink with a target of 4095 bytes does not reach the file system");
    }
    spell(spell_a, "", "t", PATH_BYTES);
    if (!refused(symlink(spell_a, WORK "/link"), ENAMETOOLONG)) {
        return fail("symlink with a target of 4096 bytes is not ENAMETOOLONG");
    }
    return 0;
}

/** `execve` of the root's hello program by a spelling of 4095 bytes. */
static int exec_row(void)
{
    char *argv[2];
    pid_t pid;

    spell(spell_a, "/", "hello", PATH_BYTES - 1);
    pid = fork();
    if (pid == 0) {
        argv[0] = (char *)"hello";
        argv[1] = NULL;
        execv(spell_a, argv);
        _exit(3);
    }
    if (pid < 0 || status_of(pid) != 0) {
        return fail("execve by a path of 4095 bytes");
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
        printf("PATHMAXMUSL OK\n");
    }
    return rc;
}
