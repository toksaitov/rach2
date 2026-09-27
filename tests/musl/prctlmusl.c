/**
 * prctl(2)'s attributes of a process, read through musl.
 *
 * The program name starts as the exec's basename, is set cut to 15 bytes
 * and read back as 16 NUL-padded bytes, and shows in /proc/self/stat. The
 * dumpable flag starts at 1 and takes 0 and 1 and nothing else, and a thread
 * that was already running sees the value another thread set. The
 * parent-death signal is kept and read back, a signal past 64 is refused, a
 * fork does not pass it on, and a grandchild that set it receives it when its
 * parent exits. A change of the effective uid clears the signal and the
 * dumpable flag. no_new_privs takes 1 with its other arguments 0 and nothing
 * else, a fork passes it on, and under it the set-user-ID `/suidhelper.suid`
 * runs as a plain program, which that helper's `plain` mode checks: every id
 * of its caller, user 1000, and AT_SECURE 0. An option this kernel does not
 * have is EINVAL, never ENOSYS.
 *
 * Prints "PRCTLMUSL OK" on success and "PRCTLMUSL FAIL (detail)" otherwise.
 */

#define _GNU_SOURCE
#include <errno.h>
#include <fcntl.h>
#include <poll.h>
#include <pthread.h>
#include <signal.h>
#include <stdio.h>
#include <string.h>
#include <sys/prctl.h>
#include <sys/wait.h>
#include <unistd.h>

/* The pipe a grandchild reports its parent-death signal on. */
static int report_fd = -1;

/** Report one failed check and return 1, the probe's failure status. */
static int fail(const char *what)
{
    printf("PRCTLMUSL FAIL (%s)\n", what);

    return 1;
}

/** Run `test` in a child and return its exit status, or -1. */
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

/** The name, set and read back, and as /proc/self/stat shows it. */
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
    return 0;
}

/* The pipe that tells the thread of `dumpable` to read the flag. */
static int flag_pipe[2];

/** A thread that reads the dumpable flag once told to; the value it read. */
static void *read_dumpable(void *arg)
{
    char c;

    (void)arg;
    if (read(flag_pipe[0], &c, 1) != 1) {
        return (void *)-1L;
    }
    return (void *)(long)prctl(PR_GET_DUMPABLE, 0, 0, 0, 0);
}

/** The dumpable flag's values, and a running thread's view of a change. */
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

/** A child's view: no parent-death signal inherited. */
static int no_inherited_signal(void)
{
    int sig = -1;

    return prctl(PR_GET_PDEATHSIG, &sig, 0, 0, 0) == 0 && sig == 0 ? 0 : 1;
}

/** Tell the probe the parent-death signal arrived. */
static void on_parent_death(int sig)
{
    char c = sig == SIGUSR1 ? 'D' : 'X';

    write(report_fd, &c, 1);
    _exit(0);
}

/**
 * The middle generation: fork a grandchild that sets its parent-death
 * signal and says so, then exit, which must deliver that signal.
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

/** A child of root that sets the signal and becomes user 1000 loses the
 * signal and the dumpable flag. */
static int moved_ids(void)
{
    int sig = -1;

    if (prctl(PR_SET_PDEATHSIG, SIGUSR2, 0, 0, 0) != 0) {
        return 1;
    }
    if (setresgid(1000, 1000, 1000) != 0 || setresuid(1000, 1000, 1000) != 0) {
        return 2;
    }
    if (prctl(PR_GET_PDEATHSIG, &sig, 0, 0, 0) != 0 || sig != 0) {
        return 3;
    }
    return prctl(PR_GET_DUMPABLE, 0, 0, 0, 0) == 0 ? 0 : 4;
}

/** The parent-death signal: kept, refused past 64, not inherited,
 * delivered, and cleared by a move of the effective ids. */
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
    if (in_child(moved_ids) != 0) {
        return fail("a move of the effective uid did not clear the signal and the dumpable flag");
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

/** A caller of user 1000 under no_new_privs: its child has the bit, and a
 * set-user-ID exec runs with its ids. */
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

/** no_new_privs's arguments, and its effect on a set-user-ID exec. */
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

int main(void)
{
    if (names() != 0 || dumpable() != 0 || parent_death() != 0 || no_new_privs() != 0) {
        return 1;
    }
    printf("PRCTLMUSL OK\n");

    return 0;
}
