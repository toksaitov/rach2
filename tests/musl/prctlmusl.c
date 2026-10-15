/**
 * prctlmusl: check the attributes of a process that prctl(2) sets and reads,
 * through musl, and the /proc/self/exe link.
 *
 * The program name starts as the exec's basename, is set cut to 15 bytes and
 * read back with its NUL, and shows in /proc/self/stat. A shorter name set
 * after the long one, and the name an exec after it gives, read back as all 16
 * bytes, the name padded with NULs. The dumpable flag starts at 1 and takes 0
 * and 1 and nothing else, and a thread that was already running sees the value
 * another thread set. The parent-death signal is kept and read back, a signal
 * past 64 fails with EINVAL, a fork does not pass it on, and a grandchild that
 * set it receives it when its parent exits. In a dumpable child with the signal
 * set, a change of the effective uid alone clears the signal and the dumpable
 * flag, and so does a change of the effective gid alone in another such child.
 * no_new_privs takes 1 with its other arguments 0 and nothing else, a fork
 * passes it on, and under it the set-user-ID `/suidhelper.suid` runs as a plain
 * program, which that helper's `plain` mode checks: every id of its caller,
 * user 1000, and AT_SECURE 0. An option this kernel does not have is EINVAL,
 * never ENOSYS.
 *
 * /proc/self/exe, which the name row executes, reaches the program's file: a
 * copy of the probe that renames itself reads its new path from the link, and
 * an exec of the link runs the copy and not a script put at its old path; the
 * image that exec runs removes the copy's name, and an open of the link still
 * reaches the copy's ELF header. A copy of the probe on a tmpfs that has
 * ended, which waitid with WNOWAIT reports, lets go of its file at its end, so
 * the tmpfs unmounts before the copy is reaped.
 *
 * The program runs itself in the modes `name`, `exe-moved`, `exe-ran` and
 * `exit-now`, named by its first argument. With none it runs the checks,
 * prints "PRCTLMUSL OK" and exits 0 on success, or prints "PRCTLMUSL FAIL
 * (detail)" and exits 1 at the first failure.
 */

#define _GNU_SOURCE
#include <errno.h>
#include <fcntl.h>
#include <poll.h>
#include <pthread.h>
#include <signal.h>
#include <stdio.h>
#include <string.h>
#include <sys/mount.h>
#include <sys/prctl.h>
#include <sys/stat.h>
#include <sys/wait.h>
#include <unistd.h>

/* The pipe a grandchild reports its parent-death signal on. */
static int report_fd = -1;

/* The copy of the probe the /proc/self/exe rows run, and the name the copy
 * moves itself to. */
#define EXE_COPY  "/tmp/prctlmusl.a"
#define EXE_MOVED "/tmp/prctlmusl.b"

/* The tmpfs the exit row mounts, and the copy of the probe it runs there. */
#define EXIT_MOUNT "/tmp/prctlmusl.mnt"
#define EXIT_COPY  "/tmp/prctlmusl.mnt/prctlmusl"

/** Report one failed check and return 1, the probe's failure status. */
static int fail(const char *what)
{
    printf("PRCTLMUSL FAIL (%s)\n", what);

    return 1;
}

/**
 * Run `test` in a forked child and return its exit status, or -1 when the
 * fork failed or the child did not exit.
 */
static int in_child(int (*test)(void))
{
    pid_t child = fork();
    int status;

    if (child < 0) {
        return -1;
    }
    if (child == 0) {
        _exit(test());
    }
    if (waitpid(child, &status, 0) != child || !WIFEXITED(status)) {
        return -1;
    }
    return WEXITSTATUS(status);
}

/**
 * Check that the program name reads back as "prctlmusl" padded with NULs to
 * all 16 bytes, with nothing left of a longer name before it. Returns 0 when
 * it does and 1 otherwise. `names` checks with it the name it sets back after
 * a longer one, and the image `exec_name` runs checks the name its exec gave
 * it.
 */
static int padded_name(void)
{
    static const char want[16] = "prctlmusl";
    char name[16];

    memset(name, 0x55, sizeof(name));
    return prctl(PR_GET_NAME, name, 0, 0, 0) == 0 && memcmp(name, want, sizeof(name)) == 0 ? 0 : 1;
}

/**
 * Run in a child: take a longer name and exec the probe again, in the `name`
 * mode, to check the name it then has (`padded_name`). Returns 2 when the
 * name cannot be set and 3 when the exec fails.
 */
static int exec_name(void)
{
    if (prctl(PR_SET_NAME, "renamed-probe-too-long", 0, 0, 0) != 0) {
        return 2;
    }
    execl("/proc/self/exe", "prctlmusl", "name", (char *)0);
    return 3;
}

/**
 * Check the program name, set and read back, and as /proc/self/stat shows
 * it. Returns 0, or 1 after the failure is printed.
 */
static int names(void)
{
    char name[16];
    char stat[256];
    ssize_t n;
    int fd;

    memset(name, 0x55, sizeof(name));
    if (prctl(PR_GET_NAME, name, 0, 0, 0) != 0 || strcmp(name, "prctlmusl") != 0) {
        return fail("the program name is not the exec's basename");
    }
    if (prctl(PR_SET_NAME, "renamed-probe-too-long", 0, 0, 0) != 0) {
        return fail("PR_SET_NAME was refused");
    }
    memset(name, 0x55, sizeof(name));
    if (prctl(PR_GET_NAME, name, 0, 0, 0) != 0 || memcmp(name, "renamed-probe-t", 16) != 0) {
        return fail("the name was not cut to 15 bytes and read back with its NUL");
    }
    fd = open("/proc/self/stat", O_RDONLY);
    n = fd >= 0 ? read(fd, stat, sizeof(stat) - 1) : -1;
    if (fd >= 0) {
        close(fd);
    }
    if (n <= 0) {
        return fail("/proc/self/stat could not be read");
    }
    stat[n] = '\0';
    if (strstr(stat, "(renamed-probe-t)") == NULL) {
        return fail("/proc/self/stat does not show the new name");
    }
    if (prctl(PR_SET_NAME, "prctlmusl", 0, 0, 0) != 0) {
        return fail("the name could not be set back");
    }
    if (padded_name() != 0) {
        return fail("a short name set after a longer one was not read back padded with NULs");
    }
    if (in_child(exec_name) != 0) {
        return fail("an exec left the end of a longer name after the program name");
    }
    return 0;
}

/* The pipe that tells the thread of `dumpable` to read the flag. */
static int flag_pipe[2];

/**
 * Run as the thread of `dumpable`: read the dumpable flag once a byte comes
 * on `flag_pipe`. Returns the value read, or -1 when no byte came.
 */
static void *read_dumpable(void *arg)
{
    char c;

    (void)arg;
    if (read(flag_pipe[0], &c, 1) != 1) {
        return (void *)-1L;
    }
    return (void *)(long)prctl(PR_GET_DUMPABLE, 0, 0, 0, 0);
}

/**
 * Check the dumpable flag's values, and a running thread's view of a change.
 * Returns 0, or 1 after the failure is printed.
 */
static int dumpable(void)
{
    pthread_t t;
    void *seen;

    if (prctl(PR_GET_DUMPABLE, 0, 0, 0, 0) != 1) {
        return fail("a process does not start dumpable");
    }
    if (pipe(flag_pipe) != 0 || pthread_create(&t, NULL, read_dumpable, NULL) != 0) {
        return fail("the thread that reads the dumpable flag");
    }
    if (prctl(PR_SET_DUMPABLE, 0, 0, 0, 0) != 0 || write(flag_pipe[1], "x", 1) != 1 ||
        pthread_join(t, &seen) != 0 || (long)seen != 0 || prctl(PR_SET_DUMPABLE, 1, 0, 0, 0) != 0) {
        return fail("a running thread did not see the dumpable flag another thread cleared");
    }
    close(flag_pipe[0]);
    close(flag_pipe[1]);
    if (prctl(PR_SET_DUMPABLE, 0, 0, 0, 0) != 0 || prctl(PR_GET_DUMPABLE, 0, 0, 0, 0) != 0 ||
        prctl(PR_SET_DUMPABLE, 1, 0, 0, 0) != 0 || prctl(PR_GET_DUMPABLE, 0, 0, 0, 0) != 1) {
        return fail("the dumpable flag did not take 0 and 1");
    }
    if (prctl(PR_SET_DUMPABLE, 2, 0, 0, 0) != -1 || errno != EINVAL) {
        return fail("a dumpable value of 2 was not refused with EINVAL");
    }
    return 0;
}

/**
 * Run in a child: return 0 when it inherited no parent-death signal, and 1
 * otherwise.
 */
static int no_inherited_signal(void)
{
    int sig = -1;

    return prctl(PR_GET_PDEATHSIG, &sig, 0, 0, 0) == 0 && sig == 0 ? 0 : 1;
}

/**
 * Write 'D' on `report_fd` for SIGUSR1, or 'X' for another signal, and exit.
 * Runs in the grandchild, as its parent-death signal's handler.
 */
static void on_parent_death(int sig)
{
    char c = sig == SIGUSR1 ? 'D' : 'X';

    write(report_fd, &c, 1);
    _exit(0);
}

/**
 * Run as the middle generation: fork a grandchild that sets its parent-death
 * signal and says so, then return, so that the child exits, which must
 * deliver that signal. Returns 0, or the number of the step that failed.
 */
static int parent_that_dies(void)
{
    int ready[2];
    pid_t grandchild;
    char c;

    if (pipe(ready) != 0) {
        return 1;
    }
    grandchild = fork();
    if (grandchild < 0) {
        return 2;
    }
    if (grandchild == 0) {
        struct sigaction sa;

        memset(&sa, 0, sizeof(sa));
        sa.sa_handler = on_parent_death;
        sigaction(SIGUSR1, &sa, NULL);
        prctl(PR_SET_PDEATHSIG, SIGUSR1, 0, 0, 0);
        write(ready[1], "R", 1);
        for (;;) {
            pause();
        }
    }
    if (read(ready[0], &c, 1) != 1) {
        return 3;
    }
    return 0;
}

/**
 * Check, in a dumpable child of root, that setting the signal and then moving
 * the effective uid (`uid` nonzero) or the effective gid alone to 1000 clears
 * the signal and the dumpable flag. Returns 0, or the number of the step
 * that failed.
 */
static int moved_id(int uid)
{
    int sig = -1;

    if (prctl(PR_GET_DUMPABLE, 0, 0, 0, 0) != 1 || prctl(PR_SET_PDEATHSIG, SIGUSR2, 0, 0, 0) != 0 ||
        prctl(PR_GET_PDEATHSIG, &sig, 0, 0, 0) != 0 || sig != SIGUSR2) {
        return 1;
    }
    if ((uid ? setresuid(1000, 1000, 1000) : setresgid(1000, 1000, 1000)) != 0) {
        return 2;
    }
    if (prctl(PR_GET_PDEATHSIG, &sig, 0, 0, 0) != 0 || sig != 0) {
        return 3;
    }
    return prctl(PR_GET_DUMPABLE, 0, 0, 0, 0) == 0 ? 0 : 4;
}

/** Run `moved_id` for the effective uid. */
static int moved_uid(void)
{
    return moved_id(1);
}

/** Run `moved_id` for the effective gid. */
static int moved_gid(void)
{
    return moved_id(0);
}

/**
 * Check the parent-death signal: kept, EINVAL past 64, not inherited,
 * delivered, and cleared by a move of the effective ids. Returns 0, or 1
 * after the failure is printed.
 */
static int parent_death(void)
{
    struct pollfd pfd;
    int report[2];
    int sig = -1;
    char c = 0;

    if (prctl(PR_GET_PDEATHSIG, &sig, 0, 0, 0) != 0 || sig != 0) {
        return fail("a process starts with a parent-death signal");
    }
    if (prctl(PR_SET_PDEATHSIG, SIGUSR2, 0, 0, 0) != 0 ||
        prctl(PR_GET_PDEATHSIG, &sig, 0, 0, 0) != 0 || sig != SIGUSR2) {
        return fail("the parent-death signal was not kept");
    }
    if (prctl(PR_SET_PDEATHSIG, 65, 0, 0, 0) != -1 || errno != EINVAL) {
        return fail("a parent-death signal of 65 was not refused with EINVAL");
    }
    if (in_child(no_inherited_signal) != 0) {
        return fail("a child inherited the parent-death signal");
    }
    prctl(PR_SET_PDEATHSIG, 0, 0, 0, 0);
    if (in_child(moved_uid) != 0) {
        return fail("a move of the effective uid alone kept the signal or the dumpable flag");
    }
    if (in_child(moved_gid) != 0) {
        return fail("a move of the effective gid alone kept the signal or the dumpable flag");
    }
    if (pipe(report) != 0) {
        return fail("pipe failed");
    }
    report_fd = report[1];
    if (in_child(parent_that_dies) != 0) {
        return fail("the grandchild did not set its parent-death signal");
    }
    pfd.fd = report[0];
    pfd.events = POLLIN;
    if (poll(&pfd, 1, 10000) != 1 || read(report[0], &c, 1) != 1 || c != 'D') {
        return fail("a grandchild did not receive its parent-death signal");
    }
    close(report[0]);
    close(report[1]);
    return 0;
}

/**
 * Run in a child of root: become user 1000 under no_new_privs, check that a
 * fork passes the bit on, and exec the set-user-ID helper in its `plain`
 * mode, which checks that it runs with this caller's ids and exits 0 when it
 * does. Returns 10 to 13 for the step that failed.
 */
static int unprivileged_exec(void)
{
    char *argv[3];
    int status;

    if (setresgid(1000, 1000, 1000) != 0 || setresuid(1000, 1000, 1000) != 0) {
        return 10;
    }
    if (prctl(PR_SET_NO_NEW_PRIVS, 1, 0, 0, 0) != 0 ||
        prctl(PR_GET_NO_NEW_PRIVS, 0, 0, 0, 0) != 1) {
        return 11;
    }
    if (fork() == 0) {
        _exit(prctl(PR_GET_NO_NEW_PRIVS, 0, 0, 0, 0) == 1 ? 0 : 1);
    }
    if (wait(&status) < 0 || !WIFEXITED(status) || WEXITSTATUS(status) != 0) {
        return 12;
    }
    argv[0] = (char *)"suidhelper";
    argv[1] = (char *)"plain";
    argv[2] = NULL;
    execv("/suidhelper.suid", argv);
    return 13;
}

/**
 * Check no_new_privs's arguments, its effect on a set-user-ID exec, and the
 * error for an unknown option. Returns 0, or 1 after the failure is printed.
 */
static int no_new_privs(void)
{
    if (prctl(PR_GET_NO_NEW_PRIVS, 0, 0, 0, 0) != 0) {
        return fail("a process starts with no_new_privs");
    }
    if (prctl(PR_GET_NO_NEW_PRIVS, 1, 0, 0, 0) != -1 || errno != EINVAL) {
        return fail("PR_GET_NO_NEW_PRIVS with an argument was not refused with EINVAL");
    }
    if (prctl(PR_SET_NO_NEW_PRIVS, 0, 0, 0, 0) != -1 || errno != EINVAL ||
        prctl(PR_SET_NO_NEW_PRIVS, 1, 1, 0, 0) != -1 || errno != EINVAL) {
        return fail("PR_SET_NO_NEW_PRIVS with other arguments was not refused with EINVAL");
    }
    if (in_child(unprivileged_exec) != 0) {
        return fail("a set-user-ID exec under no_new_privs did not run as its caller");
    }
    if (prctl(1000, 0, 0, 0, 0) != -1 || errno != EINVAL) {
        return fail("an unknown option was not refused with EINVAL");
    }
    return 0;
}

/** Copy the file at `from` to the file `to`, of mode 0755. Returns 0, or -1. */
static int copy_program(const char *from, const char *to)
{
    char buf[4096];
    ssize_t n = 0;
    int in = open(from, O_RDONLY);
    int out = open(to, O_WRONLY | O_CREAT | O_TRUNC, 0755);
    int ok = in >= 0 && out >= 0 && fchmod(out, 0755) == 0;

    while (ok && (n = read(in, buf, sizeof(buf))) > 0) {
        ok = write(out, buf, (size_t)n) == n;
    }
    if (in >= 0) {
        close(in);
    }
    if (out >= 0) {
        close(out);
    }
    return ok && n == 0 ? 0 : -1;
}

/**
 * Create the new file `path`, a script of mode 0755 that exits 7. Returns 0,
 * or -1.
 */
static int put_script(const char *path)
{
    static const char text[] = "#!/bin/sh\nexit 7\n";
    int fd = open(path, O_WRONLY | O_CREAT | O_EXCL, 0755);
    int ok = fd >= 0 && fchmod(fd, 0755) == 0 &&
             write(fd, text, sizeof(text) - 1) == (ssize_t)(sizeof(text) - 1);

    if (fd >= 0) {
        close(fd);
    }
    return ok ? 0 : -1;
}

/**
 * The image of the probe's copy at EXE_COPY, which `exe_follows` runs: it
 * renames its program to EXE_MOVED, where the link must now read, puts at the
 * old path a script that ends with 7, and executes /proc/self/exe, which must
 * run the program itself, in the "exe-ran" mode, and not the script. The
 * script is also the exec's input, since the default image's /bin/sh reads
 * its commands from its input and not from the script it is given, so a
 * shell the kernel wrongly runs ends with 7 either way. Returns the step
 * that failed, when the exec returns.
 */
static int exe_moved(void)
{
    char link[64];
    ssize_t n;
    int in;

    if (rename(EXE_COPY, EXE_MOVED) != 0) {
        return 2;
    }
    n = readlink("/proc/self/exe", link, sizeof(link) - 1);
    if (n <= 0 || (link[n] = '\0', strcmp(link, EXE_MOVED) != 0)) {
        return 3;
    }
    if (put_script(EXE_COPY) != 0) {
        return 2;
    }
    in = open(EXE_COPY, O_RDONLY);
    if (in < 0 || dup2(in, 0) != 0) {
        return 2;
    }
    if (in != 0) {
        close(in);
    }
    execl("/proc/self/exe", "prctlmusl", "exe-ran", (char *)0);
    return 5;
}

/**
 * The image `exe_moved` runs through /proc/self/exe, from EXE_MOVED: it
 * removes its program's name, and an open of /proc/self/exe must still reach
 * the program's ELF header. 0, or the step that failed.
 */
static int exe_ran(void)
{
    unsigned char head[4];
    int fd, ok;

    if (unlink(EXE_MOVED) != 0) {
        return 6;
    }
    fd = open("/proc/self/exe", O_RDONLY);
    ok = fd >= 0 && read(fd, head, sizeof(head)) == (ssize_t)sizeof(head) &&
         memcmp(head, "\177ELF", 4) == 0;
    if (fd >= 0) {
        close(fd);
    }
    return ok ? 0 : 4;
}

/**
 * Check that /proc/self/exe follows the program's file: a copy of the probe
 * that renames itself reads its new path from the link and runs itself, not
 * the script put at its old path, through the link; the image that runs
 * removes the copy's name and still reaches the copy through the link. The
 * copy's exit status says which step failed. Returns 0, or 1 after the
 * failure is printed.
 */
static int exe_follows(void)
{
    static const char *const steps[] = {
        NULL,
        "the copy of the probe did not start",
        "the copy of the probe could not move itself or place the script",
        "/proc/self/exe did not read the program's path after a rename",
        "an open of /proc/self/exe did not reach the program once its name was gone",
        "an exec of /proc/self/exe failed once the program was renamed",
        "the program's name could not be removed",
        "an exec of /proc/self/exe ran the file put at the program's old path",
        "the copy of the probe ended another way",
    };
    int status = 0;
    pid_t child;

    unlink(EXE_COPY);
    unlink(EXE_MOVED);
    if (copy_program("/proc/self/exe", EXE_COPY) != 0) {
        return fail("a copy of the probe could not be made");
    }
    child = fork();
    if (child == 0) {
        execl(EXE_COPY, "prctlmusl", "exe-moved", (char *)0);
        _exit(1);
    }
    if (child < 0 || waitpid(child, &status, 0) != child || !WIFEXITED(status) ||
        WEXITSTATUS(status) > 8) {
        status = 8 << 8;
    }
    unlink(EXE_COPY);
    unlink(EXE_MOVED);
    if (WEXITSTATUS(status) != 0) {
        return fail(steps[WEXITSTATUS(status)]);
    }
    return 0;
}

/**
 * Check that the end of a process lets go of its program's file, and of the
 * mount the file is on, with the rest of its address space, before its parent
 * reaps it, as Linux's exit_mm does: a copy of the probe on a tmpfs runs and
 * ends, and with its exit seen by waitid with WNOWAIT and the copy not yet
 * reaped, the tmpfs must unmount. Returns 0, or 1 after the failure is
 * printed.
 */
static int exit_lets_go(void)
{
    siginfo_t si;
    pid_t child;
    int ended, freed = 0;

    mkdir(EXIT_MOUNT, 0755);
    if (mount("tmpfs", EXIT_MOUNT, "tmpfs", 0, NULL) != 0) {
        rmdir(EXIT_MOUNT);
        return fail("a tmpfs for the exit row could not be mounted");
    }
    if (copy_program("/proc/self/exe", EXIT_COPY) != 0) {
        umount2(EXIT_MOUNT, 0);
        rmdir(EXIT_MOUNT);
        return fail("a copy of the probe on the tmpfs could not be made");
    }
    child = fork();
    if (child == 0) {
        execl(EXIT_COPY, "prctlmusl", "exit-now", (char *)0);
        _exit(1);
    }
    ended = child > 0 && waitid(P_PID, (id_t)child, &si, WEXITED | WNOWAIT) == 0 &&
            si.si_code == CLD_EXITED && si.si_status == 0;
    if (ended) {
        freed = umount2(EXIT_MOUNT, 0) == 0;
    }
    if (child > 0) {
        waitpid(child, NULL, 0);
    }
    if (!freed) {
        unlink(EXIT_COPY);
        umount2(EXIT_MOUNT, 0);
    }
    rmdir(EXIT_MOUNT);
    if (!ended) {
        return fail("the copy of the probe on the tmpfs did not end with 0");
    }
    if (!freed) {
        return fail("an ended child's program kept its file system busy until the reap");
    }
    return 0;
}

int main(int argc, char **argv)
{
    if (argc > 1 && strcmp(argv[1], "name") == 0) {
        return padded_name();
    }
    if (argc > 1 && strcmp(argv[1], "exe-moved") == 0) {
        return exe_moved();
    }
    if (argc > 1 && strcmp(argv[1], "exe-ran") == 0) {
        return exe_ran();
    }
    if (argc > 1 && strcmp(argv[1], "exit-now") == 0) {
        return 0;
    }
    if (names() != 0 || dumpable() != 0 || parent_death() != 0 || no_new_privs() != 0 ||
        exe_follows() != 0 || exit_lets_go() != 0) {
        return 1;
    }
    printf("PRCTLMUSL OK\n");

    return 0;
}
