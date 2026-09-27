/**
 * Capabilities, read and given up with capget and capset.
 *
 * The kernel keeps them whole. As root the probe has every one, 0 to 40,
 * effective and permitted, and none inheritable. An unknown version is
 * EINVAL, and with no data 0, with version 3 written back into the header;
 * a set between none and all is EINVAL; a header whose version ends a page
 * answers a question about the version. A child that gives them up, as
 * Chromium's zygote does in each process it forks, has none, in capget and
 * in /proc/self/status, and so has a child it forks; it may not bind a port
 * below 1024 nor read a file only another user may, may not take them back,
 * and may not open the memory of, attach to, or read the robust list of its
 * parent, which has them, but may still change the mode of a node root
 * owns. A child that keeps them may not make a parent that gave them up its
 * tracer, nor a parent whose first thread ended with them and whose live
 * thread gave them up, and a thread that gave them up still reads its own
 * process's exe link. An exec gives them back, and the image is then not dumpable, except
 * under no_new_privs or with another process sharing its directories; the
 * exec of a thread of user 1000 that gave up the none it had keeps its
 * parent-death signal and stays dumpable.
 *
 * The calls go through syscall by number, since musl has no wrapper.
 *
 * Prints "CAPMUSL OK" on success and "CAPMUSL FAIL (detail)" otherwise.
 */

#define _GNU_SOURCE
#include <arpa/inet.h>
#include <errno.h>
#include <fcntl.h>
#include <netinet/in.h>
#include <pthread.h>
#include <sched.h>
#include <signal.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/mman.h>
#include <sys/prctl.h>
#include <sys/ptrace.h>
#include <sys/socket.h>
#include <sys/stat.h>
#include <sys/syscall.h>
#include <sys/wait.h>
#include <time.h>
#include <unistd.h>

#define VERSION_3 0x20080522u
#define HIGH_WORD 0x1FFu /* capabilities 32 to 40 */
#define SECRET    "/tmp/capmusl.secret"

struct cap_data {
    unsigned effective;
    unsigned permitted;
    unsigned inheritable;
};

/* What the child of `dropped` found, by its exit status. */
static const char *const dropped_rows[] = {
    NULL,
    "a child could not report",
    "capset with every set empty failed",
    "a thread that gave up its capabilities still has them",
    "/proc/self/status did not show the capabilities given up",
    "a thread without capabilities bound a port below 1024",
    "a thread without capabilities read a file only another user may",
    "a thread without capabilities took them back",
    "a fork of a thread without capabilities has them",
    "a thread without capabilities opened the memory of a process that has them",
    "a thread without capabilities attached to a process that has them",
    "a thread without capabilities read the robust list of a process that has them",
    "a thread without capabilities could not change the mode of a node root owns",
};

/* The stack of the process that shares an exec's directories. */
static char sharer_stack[16384];

/** Report one failed check and return 1, the probe's failure status. */
static int fail(const char *what)
{
    printf("CAPMUSL FAIL (%s)\n", what);

    return 1;
}

/** Read the caller's two sets into `d`; 0, or -1 with errno set. The
 * header is the version and then a pid, 0 for the caller. */
static int get_caps(struct cap_data *d)
{
    unsigned h[2] = {VERSION_3, 0};

    memset(d, 0xAA, 2 * sizeof(*d));
    return (int)syscall(SYS_capget, h, d);
}

/** Set the caller's two sets to `d`; 0, or -1 with errno set. */
static int set_caps(const struct cap_data *d)
{
    unsigned h[2] = {VERSION_3, 0};

    return (int)syscall(SYS_capset, h, d);
}

/** Whether `d` is every capability, effective and permitted. */
static int all_of(const struct cap_data *d)
{
    return d[0].effective == 0xFFFFFFFFu && d[0].permitted == 0xFFFFFFFFu &&
           d[0].inheritable == 0 && d[1].effective == HIGH_WORD && d[1].permitted == HIGH_WORD &&
           d[1].inheritable == 0;
}

/** Whether `d` is no capability at all. */
static int none_of(const struct cap_data *d)
{
    return (d[0].effective | d[0].permitted | d[0].inheritable | d[1].effective | d[1].permitted |
            d[1].inheritable) == 0;
}

/** Give up every capability; 0, or -1 with errno set. */
static int give_up(void)
{
    struct cap_data d[2];

    memset(d, 0, sizeof(d));
    return set_caps(d);
}

/** Whether /proc/self/status gives an empty effective set. */
static int status_has_none(void)
{
    char buf[2048];
    const char *line;
    int fd = open("/proc/self/status", O_RDONLY);
    ssize_t n = fd < 0 ? -1 : read(fd, buf, sizeof(buf) - 1);

    if (fd >= 0) {
        close(fd);
    }
    if (n <= 0) {
        return 0;
    }
    buf[n] = '\0';
    line = strstr(buf, "CapEff:\t");
    return line && strncmp(line + 8, "0000000000000000\n", 17) == 0;
}

/** Whether a TCP socket may be bound to port 81 of the loopback, 1 if so,
 * 0 for EACCES and -1 for anything else. */
static int binds_low_port(void)
{
    struct sockaddr_in a;
    int s = socket(AF_INET, SOCK_STREAM, 0);
    int rc;

    if (s < 0) {
        return -1;
    }
    memset(&a, 0, sizeof(a));
    a.sin_family = AF_INET;
    a.sin_port = htons(81);
    a.sin_addr.s_addr = htonl(INADDR_LOOPBACK);
    rc = bind(s, (struct sockaddr *)&a, sizeof(a)) == 0 ? 1 : errno == EACCES ? 0 : -1;
    close(s);
    return rc;
}

/**
 * The rows about the parent, which has its capabilities, of a child that
 * gave them up: 0, or the index in `dropped_rows` of the row that failed.
 * The attach is a seize, which stops nothing, so a kernel that allows it
 * leaves the parent running.
 */
static int reaches_parent(void)
{
    char path[32];
    long head, len;
    pid_t parent = getppid();
    int fd;

    snprintf(path, sizeof(path), "/proc/%d/mem", (int)parent);
    fd = open(path, O_RDONLY);
    if (fd >= 0 || errno != EACCES) {
        return 9;
    }
    if (ptrace(PTRACE_SEIZE, parent, 0, 0) != -1 || errno != EPERM) {
        return 10;
    }
    if (syscall(SYS_get_robust_list, parent, &head, &len) != -1 || errno != EPERM) {
        return 11;
    }
    if (chmod("/dev/null", 0666) != 0) {
        return 12;
    }
    return 0;
}

/**
 * The rows of a child that gave up its capabilities: its exit status is 0,
 * or the index in `dropped_rows` of the row that failed.
 */
static int dropped(void)
{
    struct cap_data d[2];
    pid_t pid;
    int fd, status;

    if (give_up() != 0) {
        return 2;
    }
    if (get_caps(d) != 0 || !none_of(d)) {
        return 3;
    }
    if (!status_has_none()) {
        return 4;
    }
    if (binds_low_port() != 0) {
        return 5;
    }
    fd = open(SECRET, O_RDONLY);
    if (fd >= 0 || errno != EACCES) {
        return 6;
    }
    d[0].effective = d[0].permitted = 0xFFFFFFFFu;
    d[1].effective = d[1].permitted = HIGH_WORD;
    d[0].inheritable = d[1].inheritable = 0;
    if (set_caps(d) != -1 || errno != EPERM) {
        return 7;
    }
    pid = fork();
    if (pid == 0) {
        _exit(get_caps(d) == 0 && none_of(d) ? 0 : 1);
    }
    if (pid < 0 || waitpid(pid, &status, 0) != pid || !WIFEXITED(status) ||
        WEXITSTATUS(status) != 0) {
        return 8;
    }
    return reaches_parent();
}

/** Run `dropped` in a child; 0, or the index of the row that failed. */
static int in_dropped_child(void)
{
    pid_t pid = fork();
    int status;

    if (pid == 0) {
        _exit(dropped());
    }
    if (pid < 0 || waitpid(pid, &status, 0) != pid || !WIFEXITED(status)) {
        return 1;
    }
    return WEXITSTATUS(status) < (int)(sizeof(dropped_rows) / sizeof(dropped_rows[0]))
               ? WEXITSTATUS(status)
               : 1;
}

/** The process that shares an exec's directories: it waits to be killed. */
static int sharer(void *arg)
{
    (void)arg;
    for (;;) {
        pause();
    }
    return 0;
}

/**
 * Give up every capability in a child, under no_new_privs when `nnp` says
 * so and with another process sharing its directories when `shared` does,
 * and exec this program to judge its sets with `mode`: "all", "none", or
 * "undumpable", every capability and an image that is not dumpable. Whether
 * that image found them so; it kills the sharer, whose pid it is given.
 */
static int exec_finds(int nnp, int shared, const char *mode)
{
    pid_t pid = fork();
    int status;

    if (pid == 0) {
        char other[16] = "0";

        if ((nnp && prctl(PR_SET_NO_NEW_PRIVS, 1, 0, 0, 0) != 0) || give_up() != 0) {
            _exit(2);
        }
        if (shared) {
            int t = clone(sharer, sharer_stack + sizeof(sharer_stack), CLONE_FS | SIGCHLD, NULL);

            if (t < 0) {
                _exit(4);
            }
            snprintf(other, sizeof(other), "%d", t);
        }
        execl("/proc/self/exe", "capmusl", mode, other, (char *)NULL);
        _exit(3);
    }
    return pid > 0 && waitpid(pid, &status, 0) == pid && WIFEXITED(status) &&
           WEXITSTATUS(status) == 0;
}

/**
 * A child that keeps its capabilities, whose parent gave its up after the
 * fork, may not make that parent its tracer with PTRACE_TRACEME. Whether
 * the call was refused; a traceme the kernel allowed stops nothing.
 */
static int traceme_refused(void)
{
    pid_t pid = fork();
    int status;

    if (pid == 0) {
        int go[2];
        pid_t g;
        char c;

        if (pipe(go) != 0) {
            _exit(2);
        }
        g = fork();
        if (g == 0) {
            _exit(read(go[0], &c, 1) == 1 && ptrace(PTRACE_TRACEME, 0, 0, 0) == -1 && errno == EPERM
                      ? 0
                      : 1);
        }
        if (g < 0 || give_up() != 0 || write(go[1], "x", 1) != 1) {
            _exit(2);
        }
        _exit(waitpid(g, &status, 0) == g && WIFEXITED(status) && WEXITSTATUS(status) == 0 ? 0 : 1);
    }
    return pid > 0 && waitpid(pid, &status, 0) == pid && WIFEXITED(status) &&
           WEXITSTATUS(status) == 0;
}

/* The pipe the child of `traceme_leader_ended_refused` waits on, and that
 * child. */
static int leader_go[2];
static pid_t leader_child;

/** Whether this process's first thread, whose id is its pid, has ended: its
 * stat under the task directory says Z. */
static int leader_ended(void)
{
    char path[64], buf[512];
    const char *end;
    int fd;
    ssize_t n;

    snprintf(path, sizeof(path), "/proc/self/task/%d/stat", (int)getpid());
    fd = open(path, O_RDONLY);
    n = fd < 0 ? -1 : read(fd, buf, sizeof(buf) - 1);
    if (fd >= 0) {
        close(fd);
    }
    if (n <= 0) {
        return 0;
    }
    buf[n] = '\0';
    end = strrchr(buf, ')');
    return end != NULL && end[1] == ' ' && end[2] == 'Z';
}

/**
 * The thread left once the first has ended: it gives up its capabilities,
 * waits to see the first thread ended, lets the child try PTRACE_TRACEME,
 * and ends the process with the child's verdict.
 */
static void *last_thread(void *arg)
{
    struct timespec tick = {0, 10000000};
    int tries, status;

    (void)arg;
    if (give_up() != 0) {
        _exit(2);
    }
    for (tries = 0; tries < 1000 && !leader_ended(); tries++) {
        nanosleep(&tick, NULL);
    }
    if (tries == 1000 || write(leader_go[1], "x", 1) != 1) {
        _exit(2);
    }
    _exit(waitpid(leader_child, &status, 0) == leader_child && WIFEXITED(status) &&
                  WEXITSTATUS(status) == 0
              ? 0
              : 1);
    return NULL;
}

/**
 * A child that keeps its capabilities may not make its parent process its
 * tracer when the process's live thread gave its up, though its first
 * thread, which ended with them, keeps its record. Whether the call was
 * refused.
 */
static int traceme_leader_ended_refused(void)
{
    pid_t pid = fork();
    int status;

    if (pid == 0) {
        pthread_t t;
        char c;

        if (pipe(leader_go) != 0) {
            _exit(2);
        }
        leader_child = fork();
        if (leader_child == 0) {
            _exit(read(leader_go[0], &c, 1) == 1 && ptrace(PTRACE_TRACEME, 0, 0, 0) == -1 &&
                          errno == EPERM
                      ? 0
                      : 1);
        }
        if (leader_child < 0 || pthread_create(&t, NULL, last_thread, NULL) != 0) {
            _exit(2);
        }
        pthread_exit(NULL);
    }
    return pid > 0 && waitpid(pid, &status, 0) == pid && WIFEXITED(status) &&
           WEXITSTATUS(status) == 0;
}

/** A thread that gives up its capabilities and reads its process's exe
 * link, into the int at `arg`: whether the read succeeded. */
static void *reads_own_exe(void *arg)
{
    char link[256];

    *(int *)arg = give_up() == 0 && readlink("/proc/self/exe", link, sizeof(link)) > 0;
    return NULL;
}

/** Whether a thread without capabilities read its own process's exe link
 * while the first thread kept them. */
static int own_exe_readable(void)
{
    pthread_t t;
    int ok = 0;

    if (pthread_create(&t, NULL, reads_own_exe, &ok) != 0 || pthread_join(t, NULL) != 0) {
        return 0;
    }
    return ok;
}

/**
 * The exec of a thread of user 1000, which gave up the capabilities it did
 * not have and set a parent-death signal: the image has none to regain, so
 * it keeps the signal and stays dumpable. Whether it did.
 */
static int nothing_regained(void)
{
    pid_t pid = fork();
    int status;

    if (pid == 0) {
        if (setuid(1000) != 0 || give_up() != 0 || prctl(PR_SET_PDEATHSIG, SIGUSR1, 0, 0, 0) != 0) {
            _exit(2);
        }
        execl("/proc/self/exe", "capmusl", "kept", "0", (char *)NULL);
        _exit(3);
    }
    return pid > 0 && waitpid(pid, &status, 0) == pid && WIFEXITED(status) &&
           WEXITSTATUS(status) == 0;
}

/**
 * A capget of a header whose version is the last four bytes of a page with
 * no page after it: with no data it answers the version alone, as on Linux,
 * and reads no pid. Whether it did.
 */
static int version_ends_page(void)
{
    long page = sysconf(_SC_PAGESIZE);
    char *two = mmap(NULL, 2 * page, PROT_READ | PROT_WRITE, MAP_PRIVATE | MAP_ANONYMOUS, -1, 0);
    unsigned *version;
    int ok;

    if (two == MAP_FAILED || munmap(two + page, page) != 0) {
        return 0;
    }
    version = (unsigned *)(two + page - 4);
    *version = VERSION_3;
    ok = syscall(SYS_capget, version, NULL) == 0;
    *version = 0x12345678u;
    ok = ok && syscall(SYS_capget, version, NULL) == 0 && *version == VERSION_3;
    munmap(two, page);
    return ok;
}

/** Make the file only user 1000 may read. */
static int make_secret(void)
{
    int fd = open(SECRET, O_WRONLY | O_CREAT | O_TRUNC, 0600);
    int ok = fd >= 0 && write(fd, "secret\n", 7) == 7 && fchown(fd, 1000, 1000) == 0;

    if (fd >= 0) {
        close(fd);
    }
    return ok;
}

/**
 * The image an exec row runs, which judges its sets by `mode`: "all",
 * "none", "undumpable" for every capability in an image that is not
 * dumpable, or "kept" for none, the parent-death signal SIGUSR1 and a
 * dumpable image. It kills the process `other` names first, when it names
 * one. Its exit status: 0 when the image is as `mode` says.
 */
static int judge_image(const char *mode, const char *other)
{
    struct cap_data d[2];
    int found = get_caps(d) == 0;
    int sig = 0;

    if (other != NULL && atoi(other) > 0) {
        kill(atoi(other), SIGKILL);
    }
    if (strcmp(mode, "undumpable") == 0) {
        return !(found && all_of(d) && prctl(PR_GET_DUMPABLE, 0, 0, 0, 0) == 0);
    }
    if (strcmp(mode, "kept") == 0) {
        return !(found && none_of(d) && prctl(PR_GET_PDEATHSIG, &sig, 0, 0, 0) == 0 &&
                 sig == SIGUSR1 && prctl(PR_GET_DUMPABLE, 0, 0, 0, 0) == 1);
    }
    return !(found && (strcmp(mode, "all") == 0 ? all_of(d) : none_of(d)));
}

/** The rows of the calls themselves, run as root: 0, or 1 once the failure
 * is printed. */
static int call_rows(void)
{
    unsigned h[2] = {0x12345678u, 0};
    struct cap_data d[2];

    if (get_caps(d) != 0 || !all_of(d)) {
        return fail("root does not have every capability");
    }
    if (syscall(SYS_capget, h, NULL) != 0 || h[0] != VERSION_3) {
        return fail("an unknown version with no data was not 0 with version 3 written back");
    }
    h[0] = 0x12345678u;
    if (syscall(SYS_capget, h, d) != -1 || errno != EINVAL) {
        return fail("an unknown version was not EINVAL");
    }
    if (!version_ends_page()) {
        return fail("a header whose version ends a page was not asked the version alone");
    }
    memset(d, 0, sizeof(d));
    d[0].effective = d[0].permitted = 1;
    if (set_caps(d) != -1 || errno != EINVAL) {
        return fail("a set between none and all was not EINVAL");
    }
    return 0;
}

/** The rows of the execs, the traceme and a thread's own exe link: 0, or 1
 * once the failure is printed. */
static int later_rows(void)
{
    if (!exec_finds(0, 0, "all")) {
        return fail("an exec as root did not give the capabilities back");
    }
    if (!exec_finds(0, 0, "undumpable")) {
        return fail("an exec that gave the capabilities back left the image dumpable");
    }
    if (!exec_finds(1, 0, "none")) {
        return fail("an exec under no_new_privs gave the capabilities back");
    }
    if (!exec_finds(0, 1, "none")) {
        return fail("an exec whose directories another process shares gave the capabilities back");
    }
    if (!nothing_regained()) {
        return fail("an exec that regained nothing cleared the parent-death signal");
    }
    if (!traceme_refused()) {
        return fail("a child with capabilities made a parent without them its tracer");
    }
    if (!traceme_leader_ended_refused()) {
        return fail("a privileged child made its tracer a parent whose live thread was not");
    }
    if (!own_exe_readable()) {
        return fail("a thread without capabilities could not read its own process's exe link");
    }
    return 0;
}

int main(int argc, char **argv)
{
    int row;

    if (argc > 1) {
        return judge_image(argv[1], argc > 2 ? argv[2] : NULL);
    }
    if (call_rows() != 0) {
        return 1;
    }
    if (!make_secret()) {
        return fail("the file only user 1000 may read");
    }
    row = in_dropped_child();
    unlink(SECRET);
    if (row != 0) {
        return fail(dropped_rows[row]);
    }
    if (later_rows() != 0) {
        return 1;
    }

    printf("CAPMUSL OK\n");

    return 0;
}
