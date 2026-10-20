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
 * The child subreaper rows run in a forked child S, since the probe may be
 * init, which adopts every orphan anyway. S's mark reads 0, then 1 after S
 * sets it, and a pointer of 0 to read it into is EFAULT; a fork does not
 * pass the mark on, and an exec keeps it. S forks M, which forks Z, lets it
 * end unreaped, and forks G, which spends three ticks of processor time,
 * touches 2 MiB, sets its parent-death signal and waits for it. While M
 * lives, /proc/self/task/S/children lists M alone, the file of another
 * thread of S is empty, and /proc/self/children does not exist. When M
 * ends, G finds that its parent is S, S's waitpid(-1, WNOHANG) reaps the
 * zombie Z at once, and the file lists G; S's waitpid(-1) then reaps G, whose
 * processor time and memory show in S's RUSAGE_CHILDREN, and the file is
 * empty. With the mark cleared, the orphans of a second M go to init: that
 * G's parent is not S, and S has no child left to wait for.
 *
 * The program runs itself in the modes `name`, `exe-moved`, `exe-ran`,
 * `exit-now` and `subreaper-kept`, named by its first argument. With none it
 * runs the checks, prints "PRCTLMUSL OK" and exits 0 on success, or prints
 * "PRCTLMUSL FAIL (detail)" and exits 1 at the first failure. User and group
 * 1000 are the NEW of 1000 when a native run gives `ids=OLD:NEW,...`
 * (tests/native/Readme.md), which it passes on to the helper.
 */

#define _GNU_SOURCE
#include <errno.h>
#include <fcntl.h>
#include <poll.h>
#include <pthread.h>
#include <signal.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/mount.h>
#include <sys/prctl.h>
#include <sys/resource.h>
#include <sys/stat.h>
#include <sys/syscall.h>
#include <sys/times.h>
#include <sys/wait.h>
#include <unistd.h>

#include "../probe_args.h"

/* The unprivileged user and group, 1000 or its native NEW. */
#define OTHER_ID ((unsigned)probe_id(1000))

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
    unsigned id = OTHER_ID;
    int sig = -1;

    if (prctl(PR_GET_DUMPABLE, 0, 0, 0, 0) != 1 || prctl(PR_SET_PDEATHSIG, SIGUSR2, 0, 0, 0) != 0 ||
        prctl(PR_GET_PDEATHSIG, &sig, 0, 0, 0) != 0 || sig != SIGUSR2) {
        return 1;
    }
    if ((uid ? setresuid(id, id, id) : setresgid(id, id, id)) != 0) {
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
    const char *ids = probe_arg("ids", NULL);
    char ids_arg[256];
    char *argv[4];
    unsigned id = OTHER_ID;
    int status;

    if (setresgid(id, id, id) != 0 || setresuid(id, id, id) != 0) {
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
    argv[3] = NULL;
    if (ids != NULL) {
        snprintf(ids_arg, sizeof(ids_arg), "ids=%s", ids);
        argv[2] = ids_arg;
    }
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

/* The processor time, in microseconds, and the memory, in bytes, an orphan
 * of the subreaper rows uses, and the most ticks since boot it may spin for
 * that time. */
#define ORPHAN_USEC  30000
#define ORPHAN_HEAP  (2 * 1024 * 1024)
#define ORPHAN_TICKS 1000

/*
 * The processes of one orphan row and their pipes: M, the child of S, forks
 * Z and G and ends when S writes on `middle_go`; M writes Z's pid and G its
 * own on `ready`, and G its report on `report`, and G ends when S writes on
 * `orphan_go`. Each process closes the ends it does not use, so that the
 * end of S lets M and G end too.
 */
struct orphan {
    pid_t middle;
    pid_t zombie;
    pid_t orphan;
    int ready[2];
    int report[2];
    int middle_go[2];
    int orphan_go[2];
};

/* What G reports once its parent has ended: the parent it then has, and the
 * processor time, in microseconds, and the most memory, in kilobytes, that
 * getrusage(RUSAGE_SELF) gives it. */
struct orphan_report {
    pid_t parent;
    long long usec;
    long maxrss;
};

/** The user and system time of `ru` together, in microseconds. */
static long long usec_of(const struct rusage *ru)
{
    return (long long)(ru->ru_utime.tv_sec + ru->ru_stime.tv_sec) * 1000000 + ru->ru_utime.tv_usec +
           ru->ru_stime.tv_usec;
}

/**
 * Spend a while in user mode without a system call. Returns the sum the loop
 * computed, so that the compiler keeps the loop.
 */
static unsigned int spin(void)
{
    volatile unsigned int x = 0;
    unsigned int i;

    for (i = 0; i < 200000u; i++) {
        x += i;
    }
    return x;
}

/** Run in a child of S: 0 when the child subreaper mark reads 0, and 1
 * otherwise. */
static int no_subreaper_mark(void)
{
    int mark = -1;

    return prctl(PR_GET_CHILD_SUBREAPER, &mark, 0, 0, 0) == 0 && mark == 0 ? 0 : 1;
}

/** The image `exec_subreaper` runs: 0 when the mark set before the exec
 * reads 1, and 1 otherwise. */
static int subreaper_kept(void)
{
    int mark = -1;

    return prctl(PR_GET_CHILD_SUBREAPER, &mark, 0, 0, 0) == 0 && mark == 1 ? 0 : 1;
}

/** Run in a child of S: set the mark and exec the probe in the
 * `subreaper-kept` mode. Returns 2 when the exec fails. */
static int exec_subreaper(void)
{
    if (prctl(PR_SET_CHILD_SUBREAPER, 1, 0, 0, 0) != 0) {
        return 1;
    }
    execl("/proc/self/exe", "prctlmusl", "subreaper-kept", (char *)0);
    return 2;
}

/** Whether the text of /proc/self/task/`tid`/children is `want`. */
static int children_are(pid_t tid, const char *want)
{
    char path[64];
    char buf[64];
    ssize_t n;
    int fd;

    snprintf(path, sizeof(path), "/proc/self/task/%d/children", (int)tid);
    fd = open(path, O_RDONLY);
    n = fd >= 0 ? read(fd, buf, sizeof(buf) - 1) : -1;
    if (fd >= 0) {
        close(fd);
    }
    if (n < 0) {
        return 0;
    }
    buf[n] = '\0';
    return strcmp(buf, want) == 0;
}

/** Run as a second thread of S: whether its own children file is empty
 * while S has a child. */
static void *thread_children(void *arg)
{
    (void)arg;
    return (void *)(long)children_are((pid_t)syscall(SYS_gettid), "");
}

/**
 * Run as G: spend ORPHAN_USEC of processor time and touch ORPHAN_HEAP bytes,
 * set SIGUSR1 as the parent-death signal, say so, and wait for the signal;
 * then report the parent it has and its usage, and end when S says so or
 * ends.
 */
static void orphan_main(struct orphan *o)
{
    struct orphan_report r;
    struct rusage ru;
    struct tms tms;
    sigset_t usr1;
    clock_t start = times(&tms);
    pid_t self = getpid();
    char *heap = malloc(ORPHAN_HEAP);
    char c;
    int sig;

    sigemptyset(&usr1);
    sigaddset(&usr1, SIGUSR1);
    sigprocmask(SIG_BLOCK, &usr1, NULL);
    if (!heap || prctl(PR_SET_PDEATHSIG, SIGUSR1, 0, 0, 0) != 0) {
        _exit(1);
    }
    memset(heap, 1, ORPHAN_HEAP);
    do {
        spin();
        getrusage(RUSAGE_SELF, &ru);
    } while (usec_of(&ru) < ORPHAN_USEC && times(&tms) - start < ORPHAN_TICKS);
    if (write(o->ready[1], &self, sizeof(self)) != (ssize_t)sizeof(self) ||
        sigwait(&usr1, &sig) != 0) {
        _exit(1);
    }
    memset(&r, 0, sizeof(r));
    r.parent = getppid();
    getrusage(RUSAGE_SELF, &ru);
    r.usec = usec_of(&ru);
    r.maxrss = ru.ru_maxrss;
    write(o->report[1], &r, sizeof(r));
    read(o->orphan_go[0], &c, 1);
    _exit(0);
}

/**
 * Run as M: fork Z, which ends at once, and leave it unreaped; fork G
 * (`orphan_main`); and end when S writes on `middle_go` or ends.
 */
static void middle_main(struct orphan *o)
{
    siginfo_t si;
    char c;

    close(o->ready[0]);
    close(o->report[0]);
    close(o->middle_go[1]);
    close(o->orphan_go[1]);
    o->zombie = fork();
    if (o->zombie == 0) {
        _exit(0);
    }
    if (o->zombie < 0 || waitid(P_PID, (id_t)o->zombie, &si, WEXITED | WNOWAIT) != 0 ||
        write(o->ready[1], &o->zombie, sizeof(o->zombie)) != (ssize_t)sizeof(o->zombie)) {
        _exit(1);
    }
    if (fork() == 0) {
        close(o->middle_go[0]);
        orphan_main(o);
    }
    read(o->middle_go[0], &c, 1);
    _exit(0);
}

/**
 * Start an orphan row from S: fork M, which forks Z and G, and read the pids
 * of Z and G, the second once G is ready. Returns 0, or -1.
 */
static int orphan_start(struct orphan *o)
{
    memset(o, 0, sizeof(*o));
    if (pipe(o->ready) != 0 || pipe(o->report) != 0 || pipe(o->middle_go) != 0 ||
        pipe(o->orphan_go) != 0) {
        return -1;
    }
    o->middle = fork();
    if (o->middle == 0) {
        middle_main(o);
    }
    close(o->ready[1]);
    close(o->report[1]);
    close(o->middle_go[0]);
    close(o->orphan_go[0]);
    if (o->middle < 0 ||
        read(o->ready[0], &o->zombie, sizeof(o->zombie)) != (ssize_t)sizeof(o->zombie) ||
        read(o->ready[0], &o->orphan, sizeof(o->orphan)) != (ssize_t)sizeof(o->orphan)) {
        return -1;
    }
    return 0;
}

/**
 * End M, reap it, and read G's report into `r`, which G writes once M has
 * ended; the wait is bounded by poll's ten seconds. Returns 0, or -1.
 */
static int orphan_end_middle(struct orphan *o, struct orphan_report *r)
{
    struct pollfd pfd;
    int status;

    if (write(o->middle_go[1], "x", 1) != 1 || waitpid(o->middle, &status, 0) != o->middle) {
        return -1;
    }
    pfd.fd = o->report[0];
    pfd.events = POLLIN;
    if (poll(&pfd, 1, 10000) != 1 || read(o->report[0], r, sizeof(*r)) != (ssize_t)sizeof(*r)) {
        return -1;
    }
    return 0;
}

/** Let G end, and close S's ends of the row's pipes. */
static void orphan_close(struct orphan *o)
{
    write(o->orphan_go[1], "x", 1);
    close(o->ready[0]);
    close(o->report[0]);
    close(o->middle_go[1]);
    close(o->orphan_go[1]);
}

/**
 * Run as S: set the child subreaper mark and check how it reads, that a
 * fork does not pass it on and an exec keeps it, and that the process has no
 * children file of its own. Returns 0, or the step that failed, 1 to 6.
 */
static int mark_rows(void)
{
    int mark = -1;

    if (prctl(PR_GET_CHILD_SUBREAPER, &mark, 0, 0, 0) != 0 || mark != 0) {
        return 1;
    }
    if (prctl(PR_SET_CHILD_SUBREAPER, 1, 0, 0, 0) != 0 ||
        prctl(PR_GET_CHILD_SUBREAPER, &mark, 0, 0, 0) != 0 || mark != 1) {
        return 2;
    }
    if (prctl(PR_GET_CHILD_SUBREAPER, (int *)0, 0, 0, 0) != -1 || errno != EFAULT) {
        return 3;
    }
    if (in_child(no_subreaper_mark) != 0) {
        return 4;
    }
    if (in_child(exec_subreaper) != 0) {
        return 5;
    }
    if (open("/proc/self/children", O_RDONLY) != -1 || errno != ENOENT) {
        return 6;
    }
    return 0;
}

/**
 * Run as S, marked: check that the orphans of M come to S, the zombie Z and
 * the living G, that S reaps both and counts G's usage, and what the
 * children file lists meanwhile. Returns 0, or the step that failed, 7 to
 * 16.
 */
static int adoption_rows(void)
{
    struct orphan_report r;
    struct orphan o;
    struct rusage before, after;
    pthread_t t;
    void *empty;
    char want[32];
    pid_t self = getpid();
    int status;

    if (orphan_start(&o) != 0) {
        return 7;
    }
    snprintf(want, sizeof(want), "%d ", (int)o.middle);
    if (!children_are(self, want)) {
        return 8;
    }
    if (pthread_create(&t, NULL, thread_children, NULL) != 0 || pthread_join(t, &empty) != 0 ||
        empty == NULL) {
        return 9;
    }
    if (orphan_end_middle(&o, &r) != 0) {
        return 10;
    }
    if (r.parent != self) {
        return 11;
    }
    if (waitpid(-1, &status, WNOHANG) != o.zombie) {
        return 12;
    }
    snprintf(want, sizeof(want), "%d ", (int)o.orphan);
    if (!children_are(self, want)) {
        return 13;
    }
    getrusage(RUSAGE_CHILDREN, &before);
    orphan_close(&o);
    if (waitpid(-1, &status, 0) != o.orphan || !WIFEXITED(status) || WEXITSTATUS(status) != 0) {
        return 14;
    }
    getrusage(RUSAGE_CHILDREN, &after);
    if (r.usec < ORPHAN_USEC || usec_of(&after) - usec_of(&before) < r.usec ||
        after.ru_maxrss < r.maxrss) {
        return 15;
    }
    return children_are(self, "") ? 0 : 16;
}

/**
 * Run as S, once every child is reaped: clear the mark, and check that the
 * orphans of a second M do not come to S. Returns 0, or the step that
 * failed, 7, 17 or 18.
 */
static int cleared_rows(void)
{
    struct orphan_report r;
    struct orphan o;
    int mark = -1;
    int status;

    if (prctl(PR_SET_CHILD_SUBREAPER, 0, 0, 0, 0) != 0 ||
        prctl(PR_GET_CHILD_SUBREAPER, &mark, 0, 0, 0) != 0 || mark != 0) {
        return 17;
    }
    if (orphan_start(&o) != 0 || orphan_end_middle(&o, &r) != 0) {
        return 7;
    }
    orphan_close(&o);
    if (r.parent == getpid() || waitpid(-1, &status, WNOHANG) != -1 || errno != ECHILD) {
        return 18;
    }
    return 0;
}

/**
 * Run as S: the child subreaper rows the header describes. Returns 0, or the
 * number of the step that failed, which `subreaper` gives in words. On a
 * failure S returns at once, and its end lets the row's processes end.
 */
static int subreaper_rows(void)
{
    int step = mark_rows();

    if (step == 0) {
        step = adoption_rows();
    }
    return step != 0 ? step : cleared_rows();
}

/**
 * Check the child subreaper mark and the children file in a child S
 * (`subreaper_rows`). Returns 0, or 1 after the failure is printed.
 */
static int subreaper(void)
{
    static const char *const steps[] = {
        NULL,
        "a process starts as a child subreaper",
        "PR_SET_CHILD_SUBREAPER did not set the mark PR_GET_CHILD_SUBREAPER reads",
        "PR_GET_CHILD_SUBREAPER with a pointer of 0 was not refused with EFAULT",
        "a child inherited the child subreaper mark",
        "an exec did not keep the child subreaper mark",
        "/proc/self/children exists outside a thread's directory",
        "the processes of an orphan row did not start or report",
        "the children file did not list the one child",
        "another thread's children file was not empty",
        "the orphan did not report after its parent ended",
        "an orphan did not come to the child subreaper",
        "an orphan that had ended did not come to the child subreaper",
        "the children file did not list the adopted orphan",
        "the child subreaper's waitpid(-1) did not reap the adopted orphan",
        "an adopted orphan's usage did not show in RUSAGE_CHILDREN",
        "the children file was not empty once every child was reaped",
        "PR_SET_CHILD_SUBREAPER of 0 did not clear the mark",
        "an orphan came to a process whose mark was cleared",
        "the child subreaper ended another way",
    };
    int status = in_child(subreaper_rows);

    if (status < 0 || status > 19) {
        status = 19;
    }
    return status != 0 ? fail(steps[status]) : 0;
}

int main(int argc, char **argv)
{
    probe_args(argc, argv);
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
    if (argc > 1 && strcmp(argv[1], "subreaper-kept") == 0) {
        return subreaper_kept();
    }
    if (names() != 0 || dumpable() != 0 || parent_death() != 0 || no_new_privs() != 0 ||
        exe_follows() != 0 || exit_lets_go() != 0 || subreaper() != 0) {
        return 1;
    }
    printf("PRCTLMUSL OK\n");

    return 0;
}
