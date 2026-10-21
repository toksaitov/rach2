/**
 * fdexecmusl: check that execve of /proc/self/fd/N runs the file the
 * descriptor is open on, as glibc's fexecve does it without execveat.
 *
 * A copy of this program on the boot root runs through a descriptor opened
 * for reading, through one opened with O_PATH, and after its name is gone,
 * and /proc/self/exe in the new image names the copy. A copy on a tmpfs,
 * which keeps the path a descriptor was opened by, runs and is named by that
 * path, and a script there that names the copy as its interpreter runs the
 * copy, which /proc/self/exe then names. A run by a path through symbolic
 * links there, relative and absolute, one to a directory with a `..` after
 * it, is named by the file's path with no link in it, as realpath(3) gives
 * it. The rules a path meets are met
 * through a descriptor too: a directory, a file without an execute bit, a
 * file on a tmpfs mounted noexec and a pipe are refused with EACCES; a
 * set-user-ID copy owned by root gives uid 1000 an effective uid of 0, and
 * on a tmpfs mounted nosuid it does not.
 *
 * The program execs copies of itself. With no arguments, or with only
 * NAME=VALUE ones, it is the parent and runs the checks, each exec in a forked
 * child that exits 13 when execve returns EACCES and 99 when it returns
 * anything else. With `child EXE` it is the new image, and exits 0 when
 * /proc/self/exe reads EXE and 20 otherwise; with `euid N` it exits 0 when its
 * effective uid is N and 21 otherwise; and run as a script's interpreter, whose
 * first argument is the script's path, it goes on as the arguments after that
 * say. The parent runs as root, since it mounts and changes owners. It prints
 * `FDEXECMUSL OK` and exits 0 on success, and prints `FDEXECMUSL FAIL
 * (detail)` and exits 1 otherwise.
 *
 * Uid 1000 is the NEW of 1000 when a native run gives `ids=OLD:NEW,...`
 * (tests/native/Readme.md).
 *
 * The parent writes etude's report records (probe_report.h) for the case
 * ETUDE_CASE names, an assertion for each check, as `assertion_id` lists
 * them; a failed check fails its assertion and leaves those after it not
 * run. The new images write none.
 */

#define _GNU_SOURCE
#include <errno.h>
#include <fcntl.h>
#include <limits.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/mount.h>
#include <sys/stat.h>
#include <sys/wait.h>
#include <unistd.h>

#include "../probe_args.h"
#include "../probe_report.h"

extern char **environ;

static const char *const SELF = "/musl/fdexecmusl";

/* The copies on the boot root. */
#define ROOT_COPY  "/fdexec.bin"
#define ROOT_PLAIN "/fdexec.noexec"

/* The tmpfs mounts and the copies on them. */
#define MNT        "/tmp/fdexec"
#define MNT_COPY   MNT "/bin"
#define MNT_SCRIPT MNT "/script"
#define NOEXEC     "/tmp/fdexec-noexec"
#define NOSUID     "/tmp/fdexec-nosuid"

/* The exit statuses of the child and the new image. */
#define ST_EACCES 13
#define ST_OTHER  99
#define ST_EXE    20
#define ST_EUID   21

/* The assertions, a check each, in the order the parent makes them. */
static const char *const assertion_id[] = {"copy-root",     "read-descriptor", "opath-descriptor",
                                           "open-unlink",   "unlinked-run",    "dir-eacces",
                                           "no-x-eacces",   "pipe2",           "pipe-eacces",
                                           "mount-tmpfs",   "copy-tmpfs",      "make-links",
                                           "links-exe",     "tmpfs-exe",       "script-exe",
                                           "noexec-eacces", "setuid-euid",     "nosuid-euid"};

/** Report one failed check, end the report at the assertion that `what`
 * fails, and return 1, the probe's failure status. */
static int fail(const char *what)
{
    report_stop(what);
    printf("FDEXECMUSL FAIL (%s)\n", what);

    return 1;
}

/** Copy this program to `path` with `mode`. 0, or -1. */
static int copy_self(const char *path, mode_t mode)
{
    char buf[4096];
    int in = open(SELF, O_RDONLY | O_CLOEXEC);
    int out = open(path, O_WRONLY | O_CREAT | O_TRUNC | O_CLOEXEC, 0600);
    int rc = -1;

    if (in >= 0 && out >= 0) {
        ssize_t n;

        while ((n = read(in, buf, sizeof(buf))) > 0) {
            if (write(out, buf, (size_t)n) != n) {
                break;
            }
        }
        rc = n == 0 && fchmod(out, mode) == 0 ? 0 : -1;
    }
    if (in >= 0) {
        close(in);
    }
    if (out >= 0 && close(out) != 0) {
        rc = -1;
    }
    return rc;
}

/** Write a script of the one line `#!INTERP` to `path` with mode 0755. 0, or
 * -1. */
static int write_script(const char *path, const char *interp)
{
    char line[128];
    int fd = open(path, O_WRONLY | O_CREAT | O_TRUNC | O_CLOEXEC, 0755);
    int len = snprintf(line, sizeof(line), "#!%s\n", interp);
    int rc = fd >= 0 && write(fd, line, (size_t)len) == len ? 0 : -1;

    if (fd >= 0 && close(fd) != 0) {
        rc = -1;
    }
    return rc;
}

/**
 * Exec `path` in a forked child with the arguments `a1` and `a2`, as uid 1000
 * when `uid` is not 0. The child's exit status, or -1 when the fork or the
 * wait failed or the child did not exit.
 */
static int run_exec(const char *path, uid_t uid, const char *a1, const char *a2)
{
    char *argv[4];
    char *envp[1];
    int status;
    pid_t pid;

    argv[0] = (char *)path;
    argv[1] = (char *)a1;
    argv[2] = (char *)a2;
    argv[3] = NULL;
    envp[0] = NULL;
    pid = fork();
    if (pid < 0) {
        return -1;
    }
    if (pid == 0) {
        if (uid != 0 && (setgid(uid) != 0 || setuid(uid) != 0)) {
            _exit(ST_OTHER);
        }
        execve(path, argv, envp);
        _exit(errno == EACCES ? ST_EACCES : ST_OTHER);
    }
    if (waitpid(pid, &status, 0) != pid || !WIFEXITED(status)) {
        return -1;
    }
    return WEXITSTATUS(status);
}

/** Exec /proc/self/fd/`fd` as `run_exec` does. */
static int run_fd(int fd, uid_t uid, const char *a1, const char *a2)
{
    char path[32];

    snprintf(path, sizeof(path), "/proc/self/fd/%d", fd);
    return run_exec(path, uid, a1, a2);
}

/** Open `path` with `flags` and run it as `run_fd` does, as root with
 * `child EXE`. The status, or -1. */
static int run_path(const char *path, int flags, const char *exe)
{
    int fd = open(path, flags | O_CLOEXEC);
    int st;

    if (fd < 0) {
        return -1;
    }
    st = run_fd(fd, 0, "child", exe);
    close(fd);
    return st;
}

/** Remove what an earlier run of the checks may have left. */
static void clean(void)
{
    umount(MNT);
    umount(NOEXEC);
    umount(NOSUID);
    rmdir(MNT);
    rmdir(NOEXEC);
    rmdir(NOSUID);
    unlink(ROOT_COPY);
    unlink(ROOT_PLAIN);
}

/** Mount a tmpfs with `flags` on a new directory `dir`. 0, or -1. */
static int mount_tmpfs(const char *dir, unsigned long flags)
{
    return mkdir(dir, 0755) == 0 && mount("fdexecmusl", dir, "tmpfs", flags, NULL) == 0 ? 0 : -1;
}

/**
 * The program on the boot root: run through a descriptor opened for reading,
 * through an O_PATH one, and after its name is gone; a directory, by both
 * kinds of descriptor, a copy with no execute bit and a pipe refused. 0, or
 * the probe's failure status.
 */
static int on_root(void)
{
    int fds[2];
    int fd;
    int st;

    if (copy_self(ROOT_COPY, 0755) != 0 || copy_self(ROOT_PLAIN, 0644) != 0) {
        return fail("copy the program to the boot root");
    }
    report_held();
    if ((st = run_path(ROOT_COPY, O_RDONLY, ROOT_COPY)) != 0) {
        return fail(st == ST_EACCES ? "a descriptor opened for reading: EACCES"
                                    : "a descriptor opened for reading");
    }
    report_held();
    if (run_path(ROOT_COPY, O_PATH, ROOT_COPY) != 0) {
        return fail("an O_PATH descriptor");
    }
    report_held();
    fd = open(ROOT_COPY, O_RDONLY | O_CLOEXEC);
    if (fd < 0 || unlink(ROOT_COPY) != 0) {
        return fail("open and unlink the copy");
    }
    report_held();
    st = run_fd(fd, 0, "child", "");
    close(fd);
    if (st != 0) {
        return fail("a descriptor of a file whose name is gone");
    }
    report_held();
    if (run_path("/", O_RDONLY | O_DIRECTORY, "") != ST_EACCES ||
        run_path("/", O_PATH | O_DIRECTORY, "") != ST_EACCES) {
        return fail("a directory was not refused with EACCES");
    }
    report_held();
    if (run_path(ROOT_PLAIN, O_RDONLY, "") != ST_EACCES ||
        run_path(ROOT_PLAIN, O_PATH, "") != ST_EACCES) {
        return fail("a file without an execute bit was not refused with EACCES");
    }
    report_held();
    if (pipe2(fds, O_CLOEXEC) != 0) {
        return fail("pipe2");
    }
    report_held();
    st = run_fd(fds[0], 0, "child", "");
    close(fds[0]);
    close(fds[1]);
    if (st != ST_EACCES) {
        return fail("a pipe was not refused with EACCES");
    }
    report_held();
    return 0;
}

/**
 * The copy on the tmpfs run by a path through symbolic links: a link to the
 * mount's directory, a relative one to a directory with a `..` after it, and
 * a relative one to the copy with a `..` in its target. /proc/self/exe names
 * the copy by its path with no link in it, as realpath(3) gives it. 0, or the
 * probe's failure status.
 */
static int through_links(void)
{
    char real[PATH_MAX];
    int st;

    if (mkdir(MNT "/d", 0755) != 0 || mkdir(MNT "/d/e", 0755) != 0 ||
        symlink(MNT, MNT "/top") != 0 || symlink("../bin", MNT "/d/rel") != 0 ||
        symlink("d/e", MNT "/de") != 0 || realpath(MNT_COPY, real) == NULL) {
        return fail("make the links on the tmpfs");
    }
    report_held();
    if ((st = run_exec(MNT "/top/de/../rel", 0, "child", real)) != 0) {
        return fail(st == ST_EXE ? "/proc/self/exe of a copy run through links"
                                 : "a copy run through links");
    }
    report_held();
    return 0;
}

/**
 * The program on a tmpfs, which /proc/self/exe names by the path its
 * descriptor was opened by, and by its path with no link in it when a path
 * through links runs it; a script whose interpreter is that copy, which
 * /proc/self/exe then names; a copy on a tmpfs mounted noexec refused; and a
 * set-user-ID copy owned by root, which gives uid 1000 an effective uid of
 * 0 and on a tmpfs mounted nosuid does not. 0, or the probe's failure
 * status.
 */
static int on_tmpfs(void)
{
    uid_t other = (uid_t)probe_id(1000);
    char other_text[16];
    int fd;
    int st;

    snprintf(other_text, sizeof(other_text), "%lu", (unsigned long)other);
    if (mount_tmpfs(MNT, 0) != 0 || mount_tmpfs(NOEXEC, MS_NOEXEC) != 0 ||
        mount_tmpfs(NOSUID, MS_NOSUID) != 0) {
        return fail("mount the tmpfs");
    }
    report_held();
    if (copy_self(MNT_COPY, 04755) != 0 || write_script(MNT_SCRIPT, MNT_COPY) != 0 ||
        copy_self(NOEXEC "/bin", 0755) != 0 || copy_self(NOSUID "/bin", 04755) != 0) {
        return fail("copy the program to the tmpfs");
    }
    report_held();
    if ((st = through_links()) != 0) {
        return st;
    }
    if ((st = run_path(MNT_COPY, O_RDONLY, MNT_COPY)) != 0) {
        return fail(st == ST_EXE ? "/proc/self/exe of a tmpfs copy run by its descriptor"
                                 : "a tmpfs copy run by its descriptor");
    }
    report_held();
    if ((st = run_path(MNT_SCRIPT, O_RDONLY, MNT_COPY)) != 0) {
        return fail(st == ST_EXE ? "/proc/self/exe of a script run by its descriptor"
                                 : "a script run by its descriptor");
    }
    report_held();
    if (run_path(NOEXEC "/bin", O_RDONLY, "") != ST_EACCES) {
        return fail("a copy on a noexec mount was not refused with EACCES");
    }
    report_held();
    fd = open(MNT_COPY, O_PATH | O_CLOEXEC);
    st = fd < 0 ? -1 : run_fd(fd, other, "euid", "0");
    if (fd >= 0) {
        close(fd);
    }
    if (st != 0) {
        return fail("a set-user-ID copy run by its descriptor gave no effective uid 0");
    }
    report_held();
    fd = open(NOSUID "/bin", O_PATH | O_CLOEXEC);
    st = fd < 0 ? -1 : run_fd(fd, other, "euid", other_text);
    if (fd >= 0) {
        close(fd);
    }
    if (st != 0) {
        return fail("a set-user-ID copy on a nosuid mount changed the effective uid");
    }
    report_held();
    return 0;
}

/** The new image: check what its arguments ask and exit with the status. */
static int image(int argc, char **argv)
{
    char exe[256];
    ssize_t n;

    if (argc >= 2 && strncmp(argv[1], "/proc/self/fd/", 14) == 0) {
        argc--;
        argv++;
    }
    if (argc == 3 && strcmp(argv[1], "euid") == 0) {
        return geteuid() == (uid_t)atoi(argv[2]) ? 0 : ST_EUID;
    }
    if (argc != 3 || strcmp(argv[1], "child") != 0) {
        return ST_OTHER;
    }
    if (argv[2][0] == '\0') {
        return 0;
    }
    n = readlink("/proc/self/exe", exe, sizeof(exe) - 1);
    if (n < 0) {
        return ST_EXE;
    }
    exe[n] = '\0';
    return strcmp(exe, argv[2]) == 0 ? 0 : ST_EXE;
}

int main(int argc, char **argv)
{
    unsigned int i;
    int rc;

    probe_args(argc, argv);
    if (probe_words() > 0) {
        return image(argc, argv);
    }
    report_begin(report_case(environ, "guest.default.fdexecmusl"));
    for (i = 0; i < sizeof(assertion_id) / sizeof(assertion_id[0]); i++) {
        report_plan(assertion_id[i]);
    }
    clean();
    rc = on_root();
    if (rc == 0) {
        rc = on_tmpfs();
    }
    clean();
    if (rc == 0) {
        report_end();
        printf("FDEXECMUSL OK\n");
    }
    return rc;
}
