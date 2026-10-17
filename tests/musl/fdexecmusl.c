/**
 * fdexecmusl: check that execve of /proc/self/fd/N runs the file the
 * descriptor is open on, as glibc's fexecve does it without execveat.
 *
 * A copy of this program on the boot root runs through a descriptor opened
 * for reading, through one opened with O_PATH, and after its name is gone,
 * and /proc/self/exe in the new image names the copy. A copy on a tmpfs,
 * which keeps the path a descriptor was opened by, runs and is named by that
 * path, and a script there that names the copy as its interpreter runs the
 * copy, which /proc/self/exe then names. The rules a path meets are met
 * through a descriptor too: a directory, a file without an execute bit, a
 * file on a tmpfs mounted noexec and a pipe are refused with EACCES; a
 * set-user-ID copy owned by root gives uid 1000 an effective uid of 0, and
 * on a tmpfs mounted nosuid it does not.
 *
 * The program execs copies of itself. With no arguments it is the parent and
 * runs the checks, each exec in a forked child that exits 13 when execve
 * returns EACCES and 99 when it returns anything else. With `child EXE` it is
 * the new image, and exits 0 when /proc/self/exe reads EXE and 20 otherwise;
 * with `euid N` it exits 0 when its effective uid is N and 21 otherwise; and
 * run as a script's interpreter, whose first argument is the script's path,
 * it goes on as the arguments after that say. The parent runs as root, since
 * it mounts and changes owners. It prints `FDEXECMUSL OK` and exits 0 on
 * success, and prints `FDEXECMUSL FAIL (detail)` and exits 1 otherwise.
 */

#define _GNU_SOURCE
#include <errno.h>
#include <fcntl.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/mount.h>
#include <sys/stat.h>
#include <sys/wait.h>
#include <unistd.h>

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

/** Report one failed check and return 1, the probe's failure status. */
static int fail(const char *what)
{
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
 * Exec /proc/self/fd/`fd` in a forked child with the arguments `a1` and `a2`,
 * as uid 1000 when `uid` is not 0. The child's exit status, or -1 when the
 * fork or the wait failed or the child did not exit.
 */
static int run_fd(int fd, uid_t uid, const char *a1, const char *a2)
{
    char path[32];
    char *argv[4];
    char *envp[1];
    int status;
    pid_t pid;

    snprintf(path, sizeof(path), "/proc/self/fd/%d", fd);
    argv[0] = path;
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
    if ((st = run_path(ROOT_COPY, O_RDONLY, ROOT_COPY)) != 0) {
        return fail(st == ST_EACCES ? "a descriptor opened for reading: EACCES"
                                    : "a descriptor opened for reading");
    }
    if (run_path(ROOT_COPY, O_PATH, ROOT_COPY) != 0) {
        return fail("an O_PATH descriptor");
    }
    fd = open(ROOT_COPY, O_RDONLY | O_CLOEXEC);
    if (fd < 0 || unlink(ROOT_COPY) != 0) {
        return fail("open and unlink the copy");
    }
    st = run_fd(fd, 0, "child", "");
    close(fd);
    if (st != 0) {
        return fail("a descriptor of a file whose name is gone");
    }
    if (run_path("/", O_RDONLY | O_DIRECTORY, "") != ST_EACCES ||
        run_path("/", O_PATH | O_DIRECTORY, "") != ST_EACCES) {
        return fail("a directory was not refused with EACCES");
    }
    if (run_path(ROOT_PLAIN, O_RDONLY, "") != ST_EACCES ||
        run_path(ROOT_PLAIN, O_PATH, "") != ST_EACCES) {
        return fail("a file without an execute bit was not refused with EACCES");
    }
    if (pipe2(fds, O_CLOEXEC) != 0) {
        return fail("pipe2");
    }
    st = run_fd(fds[0], 0, "child", "");
    close(fds[0]);
    close(fds[1]);
    if (st != ST_EACCES) {
        return fail("a pipe was not refused with EACCES");
    }
    return 0;
}

/**
 * The program on a tmpfs, which /proc/self/exe names by the path its
 * descriptor was opened by; a script whose interpreter is that copy, which
 * /proc/self/exe then names; a copy on a tmpfs mounted noexec refused; and a
 * set-user-ID copy owned by root, which gives uid 1000 an effective uid of
 * 0 and on a tmpfs mounted nosuid does not. 0, or the probe's failure
 * status.
 */
static int on_tmpfs(void)
{
    int fd;
    int st;

    if (mount_tmpfs(MNT, 0) != 0 || mount_tmpfs(NOEXEC, MS_NOEXEC) != 0 ||
        mount_tmpfs(NOSUID, MS_NOSUID) != 0) {
        return fail("mount the tmpfs");
    }
    if (copy_self(MNT_COPY, 04755) != 0 || write_script(MNT_SCRIPT, MNT_COPY) != 0 ||
        copy_self(NOEXEC "/bin", 0755) != 0 || copy_self(NOSUID "/bin", 04755) != 0) {
        return fail("copy the program to the tmpfs");
    }
    if ((st = run_path(MNT_COPY, O_RDONLY, MNT_COPY)) != 0) {
        return fail(st == ST_EXE ? "/proc/self/exe of a tmpfs copy run by its descriptor"
                                 : "a tmpfs copy run by its descriptor");
    }
    if ((st = run_path(MNT_SCRIPT, O_RDONLY, MNT_COPY)) != 0) {
        return fail(st == ST_EXE ? "/proc/self/exe of a script run by its descriptor"
                                 : "a script run by its descriptor");
    }
    if (run_path(NOEXEC "/bin", O_RDONLY, "") != ST_EACCES) {
        return fail("a copy on a noexec mount was not refused with EACCES");
    }
    fd = open(MNT_COPY, O_PATH | O_CLOEXEC);
    st = fd < 0 ? -1 : run_fd(fd, 1000, "euid", "0");
    if (fd >= 0) {
        close(fd);
    }
    if (st != 0) {
        return fail("a set-user-ID copy run by its descriptor gave no effective uid 0");
    }
    fd = open(NOSUID "/bin", O_PATH | O_CLOEXEC);
    st = fd < 0 ? -1 : run_fd(fd, 1000, "euid", "1000");
    if (fd >= 0) {
        close(fd);
    }
    if (st != 0) {
        return fail("a set-user-ID copy on a nosuid mount changed the effective uid");
    }
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
    int rc;

    if (argc > 1) {
        return image(argc, argv);
    }
    clean();
    rc = on_root();
    if (rc == 0) {
        rc = on_tmpfs();
    }
    clean();
    if (rc == 0) {
        printf("FDEXECMUSL OK\n");
    }
    return rc;
}
