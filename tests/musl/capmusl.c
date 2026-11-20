/**
 * capmusl: check capabilities, read and given up with capget and capset.
 *
 * As root the probe has every one, 0 to 40, effective and permitted, and none
 * inheritable. An unknown version is EINVAL, and with no data 0, with version
 * 3 written back into the header; a header whose version ends a page answers
 * a question about the version. A child of root keeps an inheritable set as
 * Linux does: capget reports the one capset gave, and a thread without
 * CAP_SETPCAP may keep an inheritable capability it does not hold but not add
 * one. PR_CAP_AMBIENT_RAISE takes a capability both held and inheritable and
 * refuses the others with EPERM; /proc/self/status shows the two sets;
 * capset that takes a capability out of the inheritable set takes it out of
 * the ambient set, and so does setuid to user 1000; and an exec under
 * no_new_privs of a thread that kept CAP_CHOWN alone, ambient, gives the
 * image CAP_CHOWN effective, permitted and ambient, as bubblewrap's --cap-add
 * needs. A child that gives them up, as
 * Chromium's zygote does in each process it forks, has none, in capget and in
 * /proc/self/status, and so has a child it forks; it may not bind a port below
 * 1024 nor read a file only another user may, may not take them back, and may
 * not open the memory of, attach to, or read the robust list of its parent,
 * which has them, but may still change the mode of a node root owns. A child
 * that keeps them may not make a parent that gave them up its tracer, nor a
 * parent whose first thread ended with them and whose live thread gave them up,
 * and a thread that gave them up still reads its own process's exe link. An
 * exec gives them back, and the image is then not dumpable, except under
 * no_new_privs or with another process sharing its directories; the exec of a
 * thread of user 1000 that gave up the none it had keeps its parent-death
 * signal and stays dumpable. User 1000, in a user namespace of its own that
 * maps its uid to itself, raises CAP_CHOWN ambient and execs this program,
 * whose image holds CAP_CHOWN effective, permitted and ambient though its
 * uid is not 0, as Linux gives it and as bubblewrap's --cap-add run by an
 * ordinary user needs. User 1000, in a user namespace whose map root writes
 * with uid 0 and its own, keeps CAP_SETUID and CAP_SYS_ADMIN ambient and
 * execs an image that holds them alone; that image calls setuid(0) and execs
 * one of root, which has every capability again, as Linux gives a root
 * image its bounding set, but under no_new_privs only the two it held.
 * capset keeps the effective set apart from the permitted one, and may
 * raise it again within that; under PR_SET_KEEPCAPS, setresuid to user 1000
 * keeps every capability permitted and none effective, and capset then
 * makes CAP_CHOWN effective, with which fchown gives user 1000 a file of
 * root's. setfsuid to user 1000 takes CAP_DAC_OVERRIDE out of root's
 * effective set, so that a file only root may read is refused, and capset
 * makes it effective again, which capget reports and the open then passes
 * by; under SECBIT_NO_SETUID_FIXUP setfsuid leaves it effective.
 *
 * The calls go through syscall by number, since musl has no wrapper.
 *
 * Run with positional arguments, the program is the image an exec row runs,
 * and judges its sets by the mode its first argument names. With none it runs
 * the rows, prints "CAPMUSL OK" and exits 0 on success, or prints "CAPMUSL
 * FAIL (detail)" and exits 1 at the first failure. User 1000 is the NEW of
 * 1000 when a native run gives `ids=OLD:NEW,...` (tests/native/Readme.md).
 * The rows write etude's report records (../probe_report.h) for the case
 * ETUDE_CASE names, an assertion for each check `checks` lists, the rows of
 * `dropped_rows` among them; a failure leaves the checks after it not run.
 * An exec row's image writes none.
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
#include <sys/fsuid.h>
#include <sys/mman.h>
#include <sys/prctl.h>
#include <sys/ptrace.h>
#include <sys/socket.h>
#include <sys/stat.h>
#include <sys/syscall.h>
#include <sys/wait.h>
#include <time.h>
#include <unistd.h>

#include "../probe_args.h"
#include "../probe_report.h"

/* _LINUX_CAPABILITY_VERSION_3, whose data is two cap_data: bits 0-31, 32-63. */
#define VERSION_3 0x20080522u
#define HIGH_WORD 0x1FFu /* capabilities 32 to 40 */
#define SECRET    "/tmp/capmusl.secret"
#define ROOT_ONLY "/tmp/capmusl.fs"

/* The capabilities the inheritable and ambient rows name, as bits of the
 * sets' first word. */
#define CHOWN       (1u << 0)
#define DAC         (1u << 1)
#define SETUID      (1u << 7)
#define SETPCAP     (1u << 8)
#define ADMIN       (1u << 21)
#define NET_RAW     (1u << 13)
#define CAP_CHOWN_  0
#define CAP_SETUID_ 7
#define CAP_RAW_    13
#define CAP_ADMIN_  21

/* One 32-bit word of each set, as Linux's __user_cap_data_struct. */
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

/* The checks, one assertion each, in the order the program makes them, and
 * the text it prints for each that fails. */
enum {
    ROOT_ALL,
    NO_DATA,
    UNKNOWN_VERSION,
    VERSION_ENDS_PAGE,
    INHERITABLE_SET,
    INHERITABLE_CAPPED,
    AMBIENT_RAISE,
    AMBIENT_STATUS,
    AMBIENT_CAPSET,
    AMBIENT_SETUID,
    EXEC_AMBIENT,
    MAKE_SECRET,
    DROPPED_CHILD,
    GIVE_UP,
    NONE_LEFT,
    STATUS_NONE,
    LOW_PORT,
    SECRET_REFUSED,
    TAKE_BACK,
    FORK_NONE,
    PARENT_MEM,
    PARENT_SEIZE,
    PARENT_ROBUST,
    CHMOD_ROOT_NODE,
    EXEC_ALL,
    EXEC_UNDUMPABLE,
    EXEC_NNP,
    EXEC_SHARED,
    EXEC_KEPT,
    EXEC_AMBIENT_USER,
    EXEC_AMBIENT_ROOT,
    EXEC_AMBIENT_NNP,
    TRACEME,
    TRACEME_LEADER,
    OWN_EXE,
    EFFECTIVE_APART,
    KEEP_CAPS,
    FSUID_CAPSET,
    FSUID_NO_FIXUP,
    CHECKS
};

static const struct {
    const char *id;
    const char *text;
} checks[CHECKS] = {
    {"root-all", "root does not have every capability"},
    {"no-data", "an unknown version with no data was not 0 with version 3 written back"},
    {"unknown-version", "an unknown version was not EINVAL"},
    {"version-ends-page", "a header whose version ends a page was not asked the version alone"},
    {"inheritable-set", "capget did not report the inheritable set capset gave"},
    {"inheritable-capped",
     "a thread without CAP_SETPCAP could not keep an inheritable capability or added one"},
    {"ambient-raise", "PR_CAP_AMBIENT did not raise a capability held and inheritable alone"},
    {"ambient-status", "/proc/self/status did not show the inheritable and ambient sets"},
    {"ambient-capset", "capset that took a capability out of the inheritable set left it ambient"},
    {"ambient-setuid", "setuid to user 1000 left the ambient set"},
    {"exec-ambient", "an exec under no_new_privs did not keep the ambient capability"},
    {"secret", "the file only user 1000 may read"},
    {"dropped-child", "a child could not report"},
    {"give-up", "capset with every set empty failed"},
    {"none-left", "a thread that gave up its capabilities still has them"},
    {"status-none", "/proc/self/status did not show the capabilities given up"},
    {"low-port", "a thread without capabilities bound a port below 1024"},
    {"secret-refused", "a thread without capabilities read a file only another user may"},
    {"take-back", "a thread without capabilities took them back"},
    {"fork-none", "a fork of a thread without capabilities has them"},
    {"parent-mem", "a thread without capabilities opened the memory of a process that has them"},
    {"parent-seize", "a thread without capabilities attached to a process that has them"},
    {"parent-robust",
     "a thread without capabilities read the robust list of a process that has them"},
    {"chmod-root-node",
     "a thread without capabilities could not change the mode of a node root owns"},
    {"exec-all", "an exec as root did not give the capabilities back"},
    {"exec-undumpable", "an exec that gave the capabilities back left the image dumpable"},
    {"exec-nnp", "an exec under no_new_privs gave the capabilities back"},
    {"exec-shared", "an exec whose directories another process shares gave the capabilities back"},
    {"exec-kept", "an exec that regained nothing cleared the parent-death signal"},
    {"exec-ambient-user", "an exec of user 1000 did not keep its ambient capability"},
    {"exec-ambient-root", "an exec of root after an ambient exec did not give every capability"},
    {"exec-ambient-nnp",
     "an exec of root under no_new_privs after an ambient exec gave capabilities it lacked"},
    {"traceme", "a child with capabilities made a parent without them its tracer"},
    {"traceme-leader", "a privileged child made its tracer a parent whose live thread was not"},
    {"own-exe", "a thread without capabilities could not read its own process's exe link"},
    {"effective-apart", "capset did not keep the effective set apart from the permitted one"},
    {"keep-caps", "PR_SET_KEEPCAPS did not keep the permitted set through setresuid to user 1000"},
    {"fsuid-capset",
     "capset could not make CAP_DAC_OVERRIDE effective again after setfsuid took it from root"},
    {"fsuid-no-fixup", "setfsuid under SECBIT_NO_SETUID_FIXUP moved the effective set"},
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
    printf("CAPMUSL FAIL (%s)\n", checks[check].text);
    report_at(check);

    return 1;
}

/**
 * Read the caller's sets into the two words of `d`. Returns 0, or -1 with
 * errno set. The header is the version and then a pid, 0 for the caller.
 */
static int get_caps(struct cap_data *d)
{
    unsigned h[2] = {VERSION_3, 0};

    memset(d, 0xAA, 2 * sizeof(*d));
    return (int)syscall(SYS_capget, h, d);
}

/**
 * Set the caller's sets to the two words of `d`. Returns 0, or -1 with errno
 * set.
 */
static int set_caps(const struct cap_data *d)
{
    unsigned h[2] = {VERSION_3, 0};

    return (int)syscall(SYS_capset, h, d);
}

/**
 * Return 1 when `d` is every capability, effective and permitted, and none
 * inheritable, and 0 otherwise.
 */
static int all_of(const struct cap_data *d)
{
    return d[0].effective == 0xFFFFFFFFu && d[0].permitted == 0xFFFFFFFFu &&
           d[0].inheritable == 0 && d[1].effective == HIGH_WORD && d[1].permitted == HIGH_WORD &&
           d[1].inheritable == 0;
}

/** Return 1 when `d` has no capability at all, and 0 otherwise. */
static int none_of(const struct cap_data *d)
{
    return (d[0].effective | d[0].permitted | d[0].inheritable | d[1].effective | d[1].permitted |
            d[1].inheritable) == 0;
}

/** Give up every capability. Returns 0, or -1 with errno set. */
static int give_up(void)
{
    struct cap_data d[2];

    memset(d, 0, sizeof(d));
    return set_caps(d);
}

/**
 * Return 1 when /proc/self/status has the line of the set `name`, such as
 * "CapEff", with the 16 hex digits `digits`, else 0.
 */
static int status_set_is(const char *name, const char *digits)
{
    char buf[4096], want[40];
    int fd = open("/proc/self/status", O_RDONLY);
    ssize_t n = fd < 0 ? -1 : read(fd, buf, sizeof(buf) - 1);

    if (fd >= 0) {
        close(fd);
    }
    if (n <= 0) {
        return 0;
    }
    buf[n] = '\0';
    snprintf(want, sizeof(want), "%s:\t%s\n", name, digits);
    return strstr(buf, want) != NULL;
}

/** Return 1 when /proc/self/status shows no effective capability, else 0. */
static int status_has_none(void)
{
    return status_set_is("CapEff", "0000000000000000");
}

/** Return 1 when `cap` is in the ambient set, 0 when not, -1 on error. */
static int ambient(int cap)
{
    return (int)prctl(PR_CAP_AMBIENT, PR_CAP_AMBIENT_IS_SET, cap, 0, 0);
}

/**
 * Try to bind a TCP socket to port 81 of the loopback. Returns 1 when the
 * bind succeeds, 0 for EACCES and -1 for anything else.
 */
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
 * Run the rows about the parent, which has its capabilities, in a child that
 * gave them up. Returns 0, or the index in `dropped_rows` of the row that
 * failed. The attach is a seize, which stops nothing, so a kernel that allows
 * it leaves the parent running.
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
 * Give up the capabilities and run the rows of a child without them. Returns
 * the child's exit status: 0, or the index in `dropped_rows` of the row that
 * failed.
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

/**
 * Run `dropped` in a forked child. Returns 0, or the index in `dropped_rows`
 * of the row that failed; 1 when the child did not report.
 */
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

/** Run as the process that shares an exec's directories: wait to be killed. */
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
 * "undumpable", every capability and an image that is not dumpable. Returns 1
 * when that image found them so, and 0 otherwise; the image kills the sharer,
 * whose pid it is given.
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
 * Check that a child that keeps its capabilities, whose parent gave its up
 * after the fork, may not make that parent its tracer with PTRACE_TRACEME.
 * Returns 1 when the call failed with EPERM, and 0 otherwise; a traceme that
 * succeeded stops nothing, so the child still exits.
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

/**
 * Return 1 when this process's first thread, whose id is its pid, has ended,
 * with Z as its state in its stat under the task directory, and 0 otherwise.
 */
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
 * and ends the process with exit status 0 when the child's call failed with
 * EPERM, 1 when it did not, and 2 when a step of its own failed.
 */
static void *last_thread(void *arg)
{
    struct timespec tick = {0, 10000000};
    int status;

    (void)arg;
    if (give_up() != 0) {
        _exit(2);
    }
    while (!leader_ended()) {
        /* interval: the first thread's state is looked at again every 10 ms */
        nanosleep(&tick, NULL);
    }
    if (write(leader_go[1], "x", 1) != 1) {
        _exit(2);
    }
    _exit(waitpid(leader_child, &status, 0) == leader_child && WIFEXITED(status) &&
                  WEXITSTATUS(status) == 0
              ? 0
              : 1);
    return NULL;
}

/**
 * Check that a child that keeps its capabilities may not make its parent
 * process its tracer when the process's live thread gave its up, though its
 * first thread, which ended with them, keeps its record. Returns 1 when the
 * call failed with EPERM, and 0 otherwise.
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

/**
 * Run as a thread that gives up its capabilities and reads its process's exe
 * link. Stores in the int at `arg` 1 when the read succeeded, and 0
 * otherwise.
 */
static void *reads_own_exe(void *arg)
{
    char link[256];

    *(int *)arg = give_up() == 0 && readlink("/proc/self/exe", link, sizeof(link)) > 0;
    return NULL;
}

/**
 * Return 1 when a thread without capabilities read its own process's exe
 * link while the first thread kept them, and 0 otherwise.
 */
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
 * it keeps the signal and stays dumpable. Returns 1 when it did, and 0
 * otherwise.
 */
static int nothing_regained(void)
{
    pid_t pid = fork();
    int status;

    if (pid == 0) {
        if (setuid((uid_t)probe_id(1000)) != 0 || give_up() != 0 ||
            prctl(PR_SET_PDEATHSIG, SIGUSR1, 0, 0, 0) != 0) {
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
 * and reads no pid. Returns 1 when it did, and 0 otherwise.
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

/** Make the file only user 1000 may read. Returns 1, or 0 on failure. */
static int make_secret(void)
{
    int fd = open(SECRET, O_WRONLY | O_CREAT | O_TRUNC, 0600);
    int ok = fd >= 0 && write(fd, "secret\n", 7) == 7 &&
             fchown(fd, (uid_t)probe_id(1000), (gid_t)probe_id(1000)) == 0;

    if (fd >= 0) {
        close(fd);
    }
    return ok;
}

/**
 * The first stage of `ambient_root_finds`, in the image that found the sets
 * `d`: its uid is not 0 and its sets are CAP_SETUID and CAP_SYS_ADMIN
 * alone, effective and permitted; it sets no_new_privs when `nnp`, calls
 * setuid(0) and execs "amb-root" or "amb-root-nnp". Returns 1 when it
 * cannot.
 */
static int ambient_setuid_stage(const struct cap_data *d, int nnp)
{
    if (getuid() == 0 || geteuid() == 0 || d[0].effective != (SETUID | ADMIN) ||
        d[0].permitted != (SETUID | ADMIN) || d[1].effective != 0 || d[1].permitted != 0 ||
        (nnp && prctl(PR_SET_NO_NEW_PRIVS, 1, 0, 0, 0) != 0) || setuid(0) != 0) {
        return 1;
    }
    execl("/proc/self/exe", "capmusl", nnp ? "amb-root-nnp" : "amb-root", "0", (char *)NULL);
    return 1;
}

/**
 * A stage of `ambient_root_finds` in the image of mode `mode`, which found
 * the sets `d` when `found`: "amb-setuid" or "amb-setuid-nnp", whose uid is
 * not 0 and whose sets are CAP_SETUID and CAP_SYS_ADMIN alone, effective,
 * permitted, inheritable and ambient, sets no_new_privs for the second, calls
 * setuid(0) and execs "amb-root" or "amb-root-nnp"; those, of uid 0, check
 * every capability or the two alone, effective and permitted, and the two
 * still ambient. Returns 0 when the image is so, else 1.
 */
static int ambient_root_stage(const char *mode, const struct cap_data *d, int found)
{
    int nnp = strstr(mode, "-nnp") != NULL;
    unsigned want0 = nnp ? SETUID | ADMIN : 0xFFFFFFFFu;
    unsigned want1 = nnp ? 0 : HIGH_WORD;

    if (!found || d[0].inheritable != (SETUID | ADMIN) || d[1].inheritable != 0 ||
        ambient(CAP_SETUID_) != 1 || ambient(CAP_ADMIN_) != 1 ||
        !status_set_is("CapAmb", "0000000000200080")) {
        return 1;
    }
    if (strncmp(mode, "amb-setuid", 10) == 0) {
        return ambient_setuid_stage(d, nnp);
    }
    return !(getuid() == 0 && geteuid() == 0 && d[0].effective == want0 &&
             d[0].permitted == want0 && d[1].effective == want1 && d[1].permitted == want1 &&
             status_set_is("CapPrm", nnp ? "0000000000200080" : "000001ffffffffff"));
}

/**
 * Whether the sets `d` are those of the image mode "ambient": CAP_CHOWN
 * alone effective, permitted and ambient, in capget, prctl and
 * /proc/self/status, with CAP_CHOWN and CAP_NET_RAW inheritable.
 */
static int ambient_image(const struct cap_data *d)
{
    return d[0].effective == CHOWN && d[0].permitted == CHOWN &&
           d[0].inheritable == (CHOWN | NET_RAW) && d[1].effective == 0 && d[1].permitted == 0 &&
           d[1].inheritable == 0 && ambient(CAP_CHOWN_) == 1 && ambient(CAP_RAW_) == 0 &&
           status_set_is("CapEff", "0000000000000001") &&
           status_set_is("CapAmb", "0000000000000001");
}

/**
 * Whether the sets `d` are those of the image mode "ambient-user": a uid
 * that is not 0, and CAP_CHOWN alone effective, permitted, inheritable and
 * ambient.
 */
static int ambient_user_image(const struct cap_data *d)
{
    return getuid() != 0 && geteuid() != 0 && d[0].effective == CHOWN && d[0].permitted == CHOWN &&
           d[0].inheritable == CHOWN && d[1].effective == 0 && d[1].permitted == 0 &&
           d[1].inheritable == 0 && ambient(CAP_CHOWN_) == 1 &&
           status_set_is("CapEff", "0000000000000001") &&
           status_set_is("CapPrm", "0000000000000001") &&
           status_set_is("CapAmb", "0000000000000001");
}

/**
 * The image an exec row runs, which judges its sets by `mode`: "all",
 * "none", "undumpable" for every capability in an image that is not
 * dumpable, "kept" for none, the parent-death signal SIGUSR1 and a
 * dumpable image, "ambient" for CAP_CHOWN alone effective, permitted and
 * ambient, in capget, prctl and /proc/self/status, with CAP_CHOWN and
 * CAP_NET_RAW inheritable, or "ambient-user" for the same with CAP_CHOWN
 * alone inheritable and a uid that is not 0, or one of the three stages of
 * `ambient_root_finds`: "amb-setuid" and "amb-setuid-nnp" for CAP_SETUID and
 * CAP_SYS_ADMIN alone and a uid that is not 0, then setuid(0) and the exec of
 * "amb-root" or "amb-root-nnp", every capability or those two alone, with
 * uid 0 and the two ambient. It kills the process `other` names first, when
 * it names one. Its exit status: 0 when the image is as `mode` says; for
 * "ambient" the row of `ambient_rows` that failed otherwise.
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
    if (strcmp(mode, "ambient") == 0) {
        return found && ambient_image(d) ? 0 : EXEC_AMBIENT - INHERITABLE_SET + 1;
    }
    if (strcmp(mode, "ambient-user") == 0) {
        return !(found && ambient_user_image(d));
    }
    if (strncmp(mode, "amb-", 4) == 0) {
        return ambient_root_stage(mode, d, found);
    }
    if (strcmp(mode, "kept") == 0) {
        return !(found && none_of(d) && prctl(PR_GET_PDEATHSIG, &sig, 0, 0, 0) == 0 &&
                 sig == SIGUSR1 && prctl(PR_GET_DUMPABLE, 0, 0, 0, 0) == 1);
    }
    return !(found && (strcmp(mode, "all") == 0 ? all_of(d) : none_of(d)));
}

/**
 * Set the caller's effective and permitted sets to `held` and its
 * inheritable set to `inh`, each the first word, the second empty. Returns
 * 0, or -1 with errno set.
 */
static int set_first(unsigned held, unsigned inh)
{
    struct cap_data d[2];

    memset(d, 0, sizeof(d));
    d[0].effective = d[0].permitted = held;
    d[0].inheritable = inh;
    return set_caps(d);
}

/** Return 1 when the prctl `rc` failed with `err`, else 0. */
static int refused_with(int rc, int err)
{
    return rc == -1 && errno == err;
}

/**
 * In a child of root, make CAP_CHOWN and CAP_NET_RAW inheritable and give up
 * CAP_SETPCAP, CAP_DAC_OVERRIDE and CAP_NET_RAW. Returns 0, or 1 to 2 for
 * the row of the inheritable set that failed.
 */
static int inheritable_rows(void)
{
    struct cap_data d[2];
    unsigned rest = 0xFFFFFFFFu & ~(SETPCAP | DAC | NET_RAW);

    if (get_caps(d) != 0) {
        return 1;
    }
    d[0].inheritable = CHOWN | NET_RAW;
    if (set_caps(d) != 0 || get_caps(d) != 0 || d[0].inheritable != (CHOWN | NET_RAW) ||
        d[1].inheritable != 0) {
        return 1;
    }
    d[0].effective = d[0].permitted = rest;
    if (set_caps(d) != 0) {
        return 2;
    }
    d[0].inheritable = CHOWN | NET_RAW | DAC;
    if (!refused_with(set_caps(d), EPERM)) {
        return 2;
    }
    return 0;
}

/**
 * The rows of the ambient set, in a child of root whose permitted set is
 * `rest`, with CAP_CHOWN and CAP_NET_RAW inheritable: PR_CAP_AMBIENT's
 * raise and its refusals, the sets in /proc/self/status, the lower, and the
 * loss of the ambient set to a capset that drops its inheritable bit.
 * Returns 0, or 3 to 5 for the row that failed, ending with CAP_CHOWN
 * ambient again.
 */
static int ambient_set_rows(unsigned rest)
{
    if (!refused_with(prctl(PR_CAP_AMBIENT, PR_CAP_AMBIENT_RAISE, CAP_ADMIN_, 0, 0), EPERM) ||
        !refused_with(prctl(PR_CAP_AMBIENT, PR_CAP_AMBIENT_RAISE, CAP_RAW_, 0, 0), EPERM) ||
        !refused_with(prctl(PR_CAP_AMBIENT, PR_CAP_AMBIENT_RAISE, 41, 0, 0), EINVAL) ||
        !refused_with(prctl(PR_CAP_AMBIENT, PR_CAP_AMBIENT_RAISE, CAP_CHOWN_, 1, 0), EINVAL) ||
        prctl(PR_CAP_AMBIENT, PR_CAP_AMBIENT_RAISE, CAP_CHOWN_, 0, 0) != 0 ||
        ambient(CAP_CHOWN_) != 1 || ambient(CAP_RAW_) != 0) {
        return 3;
    }
    if (!status_set_is("CapInh", "0000000000002001") ||
        !status_set_is("CapAmb", "0000000000000001")) {
        return 4;
    }
    if (prctl(PR_CAP_AMBIENT, PR_CAP_AMBIENT_LOWER, CAP_CHOWN_, 0, 0) != 0 ||
        ambient(CAP_CHOWN_) != 0 ||
        prctl(PR_CAP_AMBIENT, PR_CAP_AMBIENT_RAISE, CAP_CHOWN_, 0, 0) != 0 ||
        set_first(rest, NET_RAW) != 0 || ambient(CAP_CHOWN_) != 0 ||
        set_first(rest, CHOWN | NET_RAW) != 0 ||
        prctl(PR_CAP_AMBIENT, PR_CAP_AMBIENT_RAISE, CAP_CHOWN_, 0, 0) != 0) {
        return 5;
    }
    return 0;
}

/**
 * Run the rows of the inheritable and ambient sets in a child of root, which
 * ends in the exec of this program in the mode "ambient". Its exit status: 0,
 * or 1 and up for the row that failed, counted from INHERITABLE_SET.
 */
static int ambient_rows(void)
{
    unsigned rest = 0xFFFFFFFFu & ~(SETPCAP | DAC | NET_RAW);
    pid_t pid;
    int row = inheritable_rows(), status;

    if (row == 0) {
        row = ambient_set_rows(rest);
    }
    if (row != 0) {
        return row;
    }
    pid = fork();
    if (pid == 0) {
        _exit(setuid((uid_t)probe_id(1000)) == 0 && ambient(CAP_CHOWN_) == 0 ? 0 : 1);
    }
    if (pid < 0 || waitpid(pid, &status, 0) != pid || !WIFEXITED(status) ||
        WEXITSTATUS(status) != 0) {
        return 6;
    }
    if (set_first(CHOWN, CHOWN | NET_RAW) != 0 || ambient(CAP_CHOWN_) != 1 ||
        prctl(PR_SET_NO_NEW_PRIVS, 1, 0, 0, 0) != 0) {
        return 7;
    }
    execl("/proc/self/exe", "capmusl", "ambient", "0", (char *)NULL);
    return 7;
}

/**
 * Run `ambient_rows` in a forked child. Returns 0, or the row that failed,
 * counted from 1 at INHERITABLE_SET; 1 when the child did not report.
 */
static int in_ambient_child(void)
{
    pid_t pid = fork();
    int status;

    if (pid == 0) {
        _exit(ambient_rows());
    }
    if (pid < 0 || waitpid(pid, &status, 0) != pid || !WIFEXITED(status)) {
        return 1;
    }
    return WEXITSTATUS(status) <= EXEC_AMBIENT - INHERITABLE_SET + 1 ? WEXITSTATUS(status) : 1;
}

/**
 * Write the string `text` to the file `path`, as a user namespace's map
 * files take it. Returns 0, or -1 on failure.
 */
static int put(const char *path, const char *text)
{
    int fd = open(path, O_WRONLY);
    ssize_t len = (ssize_t)strlen(text);
    int ok = fd >= 0 && write(fd, text, (size_t)len) == len;

    if (fd >= 0) {
        close(fd);
    }
    return ok ? 0 : -1;
}

/**
 * The exec of user 1000 in a user namespace of its own that maps its ids to
 * themselves, as bubblewrap's --unshare-user makes it: the namespace gives
 * it every capability there, it keeps CAP_CHOWN alone, inheritable and
 * ambient, and the image, whose uid is not 0, finds CAP_CHOWN effective,
 * permitted and ambient. Returns 1 when it did, and 0 otherwise.
 */
static int ambient_kept_by_user(void)
{
    pid_t pid = fork();
    int status;

    if (pid == 0) {
        char map[32];

        snprintf(map, sizeof(map), "%ld %ld 1\n", probe_id(1000), probe_id(1000));
        if (setgid((gid_t)probe_id(1000)) != 0 || setuid((uid_t)probe_id(1000)) != 0 ||
            prctl(PR_SET_DUMPABLE, 1, 0, 0, 0) != 0 || unshare(CLONE_NEWUSER) != 0 ||
            put("/proc/self/uid_map", map) != 0 || put("/proc/self/setgroups", "deny") != 0 ||
            put("/proc/self/gid_map", map) != 0 || set_first(CHOWN, CHOWN) != 0 ||
            prctl(PR_CAP_AMBIENT, PR_CAP_AMBIENT_RAISE, CAP_CHOWN_, 0, 0) != 0) {
            _exit(2);
        }
        execl("/proc/self/exe", "capmusl", "ambient-user", "0", (char *)NULL);
        _exit(3);
    }
    return pid > 0 && waitpid(pid, &status, 0) == pid && WIFEXITED(status) &&
           WEXITSTATUS(status) == 0;
}

/**
 * The case of a bubblewrap run by an ordinary user that keeps CAP_SETUID and
 * CAP_SYS_ADMIN: user 1000 makes a user namespace whose map root writes, uid
 * 0 and 1000 each to itself, keeps the two inheritable and ambient, and
 * execs an image whose uid is not 0 and which holds them alone, as
 * `ambient_kept_by_user` checks; that image calls setuid(0) and execs this
 * program as root, under no_new_privs when `nnp` says so, in the stages of
 * `ambient_root_stage`. Linux gives that image of root every capability of
 * its bounding set, and under no_new_privs keeps it to the two its caller
 * held. Returns 1 when the last image found its sets so, and 0 otherwise.
 */
static int ambient_root_finds(int nnp)
{
    int ready[2], go[2], status, ok;
    char path[40], map[48], c = 0;
    pid_t pid;

    if (pipe(ready) != 0) {
        return 0;
    }
    if (pipe(go) != 0) {
        close(ready[0]);
        close(ready[1]);
        return 0;
    }
    pid = fork();
    if (pid == 0) {
        close(ready[0]);
        close(go[1]);
        if (setgid((gid_t)probe_id(1000)) != 0 || setuid((uid_t)probe_id(1000)) != 0 ||
            prctl(PR_SET_DUMPABLE, 1, 0, 0, 0) != 0 || unshare(CLONE_NEWUSER) != 0 ||
            write(ready[1], "r", 1) != 1 || read(go[0], &c, 1) != 1 || c != 'g' ||
            set_first(SETUID | ADMIN, SETUID | ADMIN) != 0 ||
            prctl(PR_CAP_AMBIENT, PR_CAP_AMBIENT_RAISE, CAP_SETUID_, 0, 0) != 0 ||
            prctl(PR_CAP_AMBIENT, PR_CAP_AMBIENT_RAISE, CAP_ADMIN_, 0, 0) != 0) {
            _exit(2);
        }
        execl("/proc/self/exe", "capmusl", nnp ? "amb-setuid-nnp" : "amb-setuid", "0",
              (char *)NULL);
        _exit(3);
    }
    close(ready[1]);
    close(go[0]);
    ok = pid > 0 && read(ready[0], &c, 1) == 1;
    if (ok) {
        snprintf(path, sizeof(path), "/proc/%d/uid_map", (int)pid);
        snprintf(map, sizeof(map), "0 0 1\n%ld %ld 1\n", probe_id(1000), probe_id(1000));
        ok = put(path, map) == 0;
    }
    if (ok) {
        ok = write(go[1], "g", 1) == 1;
    }
    close(ready[0]);
    close(go[1]);
    return pid > 0 && waitpid(pid, &status, 0) == pid && ok && WIFEXITED(status) &&
           WEXITSTATUS(status) == 0;
}

/**
 * Run the rows of the calls themselves, as root. Returns 0, or 1 after the
 * failure is printed.
 */
static int call_rows(void)
{
    unsigned h[2] = {0x12345678u, 0};
    struct cap_data d[2];
    int row;

    if (get_caps(d) != 0 || !all_of(d)) {
        return fail(ROOT_ALL);
    }
    if (syscall(SYS_capget, h, NULL) != 0 || h[0] != VERSION_3) {
        return fail(NO_DATA);
    }
    h[0] = 0x12345678u;
    if (syscall(SYS_capget, h, d) != -1 || errno != EINVAL) {
        return fail(UNKNOWN_VERSION);
    }
    if (!version_ends_page()) {
        return fail(VERSION_ENDS_PAGE);
    }
    row = in_ambient_child();
    if (row != 0) {
        return fail(INHERITABLE_SET + row - 1);
    }
    return 0;
}

/**
 * Run the rows of the execs, the traceme and a thread's own exe link.
 * Returns 0, or 1 after the failure is printed.
 */
static int later_rows(void)
{
    if (!exec_finds(0, 0, "all")) {
        return fail(EXEC_ALL);
    }
    if (!exec_finds(0, 0, "undumpable")) {
        return fail(EXEC_UNDUMPABLE);
    }
    if (!exec_finds(1, 0, "none")) {
        return fail(EXEC_NNP);
    }
    if (!exec_finds(0, 1, "none")) {
        return fail(EXEC_SHARED);
    }
    if (!nothing_regained()) {
        return fail(EXEC_KEPT);
    }
    if (!ambient_kept_by_user()) {
        return fail(EXEC_AMBIENT_USER);
    }
    if (!ambient_root_finds(0)) {
        return fail(EXEC_AMBIENT_ROOT);
    }
    if (!ambient_root_finds(1)) {
        return fail(EXEC_AMBIENT_NNP);
    }
    if (!traceme_refused()) {
        return fail(TRACEME);
    }
    if (!traceme_leader_ended_refused()) {
        return fail(TRACEME_LEADER);
    }
    if (!own_exe_readable()) {
        return fail(OWN_EXE);
    }
    return 0;
}

/** Whether the two words of `d` have `eff` and `perm` as their first
 * effective and permitted words, and nothing in the second. */
static int sets_are(const struct cap_data *d, unsigned eff, unsigned perm)
{
    return d[0].effective == eff && d[0].permitted == perm && d[1].effective == 0 &&
           d[1].permitted == 0;
}

/**
 * In a child of root: capset keeps CAP_CHOWN effective and CAP_CHOWN and
 * CAP_DAC_OVERRIDE permitted, and capget reports the two sets apart; a
 * second capset raises CAP_DAC_OVERRIDE into the effective set again.
 * Returns 1 when all of it holds, else 0.
 */
static int effective_apart(void)
{
    struct cap_data d[2];

    memset(d, 0, sizeof(d));
    d[0].effective = CHOWN;
    d[0].permitted = CHOWN | DAC;
    if (set_caps(d) != 0 || get_caps(d) != 0 || !sets_are(d, CHOWN, CHOWN | DAC)) {
        return 0;
    }
    memset(d, 0, sizeof(d));
    d[0].effective = d[0].permitted = CHOWN | DAC;
    return set_caps(d) == 0 && get_caps(d) == 0 && sets_are(d, CHOWN | DAC, CHOWN | DAC);
}

/**
 * In a child of root: under PR_SET_KEEPCAPS, which PR_GET_KEEPCAPS and
 * PR_GET_SECUREBITS (SECBIT_KEEP_CAPS, 0x10) then report, setresuid to user
 * 1000 keeps every capability permitted and none effective; capset makes
 * CAP_CHOWN effective again, and fchown of a file root made gives it to
 * user 1000. Returns 1 when all of it holds, else 0.
 */
static int keeps_caps(int fd)
{
    struct cap_data d[2];
    uid_t user = (uid_t)probe_id(1000);

    if (prctl(PR_SET_KEEPCAPS, 1, 0, 0, 0) != 0 || prctl(PR_GET_KEEPCAPS, 0, 0, 0, 0) != 1 ||
        prctl(PR_GET_SECUREBITS, 0, 0, 0, 0) != 0x10 || setresuid(user, user, user) != 0) {
        return 0;
    }
    if (get_caps(d) != 0 || d[0].effective != 0 || d[1].effective != 0 ||
        d[0].permitted != 0xFFFFFFFFu || d[1].permitted != HIGH_WORD) {
        return 0;
    }
    if (fchown(fd, user, (gid_t)-1) != -1 || errno != EPERM) {
        return 0;
    }
    memset(d, 0, sizeof(d));
    d[0].effective = d[0].permitted = CHOWN;
    return set_caps(d) == 0 && get_caps(d) == 0 && sets_are(d, CHOWN, CHOWN) &&
           fchown(fd, user, (gid_t)-1) == 0;
}

/** Whether the caller may open ROOT_ONLY, mode 0600 and root's, to read. */
static int reads_root_only(void)
{
    int fd = open(ROOT_ONLY, O_RDONLY);

    if (fd < 0) {
        return 0;
    }
    close(fd);
    return 1;
}

/**
 * In a child of root: setfsuid to user 1000 leaves CAP_DAC_OVERRIDE
 * permitted but not effective, and CAP_SYS_ADMIN effective, so that
 * ROOT_ONLY is refused with EACCES; capset of the permitted set as the
 * effective one makes CAP_DAC_OVERRIDE effective again though the
 * file-system user id stays 1000, as on Linux, and capget reports it and
 * the open passes. Returns 1 when all of it holds, else 0.
 */
static int fsuid_capset(void)
{
    struct cap_data d[2];
    uid_t user = (uid_t)probe_id(1000);

    setfsuid(user);
    if ((uid_t)setfsuid((uid_t)-1) != user || get_caps(d) != 0 || (d[0].effective & DAC) ||
        !(d[0].permitted & DAC) || !(d[0].effective & ADMIN)) {
        return 0;
    }
    if (reads_root_only() || errno != EACCES) {
        return 0;
    }
    d[0].effective = d[0].permitted;
    d[1].effective = d[1].permitted;
    return set_caps(d) == 0 && get_caps(d) == 0 && (d[0].effective & DAC) && reads_root_only();
}

/**
 * In a child of root: under SECBIT_NO_SETUID_FIXUP, which root may set,
 * setfsuid to user 1000 leaves the effective set whole, and ROOT_ONLY opens.
 * Returns 1 when all of it holds, else 0.
 */
static int fsuid_no_fixup(void)
{
    struct cap_data d[2];
    uid_t user = (uid_t)probe_id(1000);

    if (prctl(PR_SET_SECUREBITS, 0x04, 0, 0, 0) != 0) {
        return 0;
    }
    setfsuid(user);
    return (uid_t)setfsuid((uid_t)-1) == user && get_caps(d) == 0 && all_of(d) && reads_root_only();
}

/** Run `row` in a forked child. Returns 1 when it held there, else 0. */
static int in_child(int (*row)(void))
{
    pid_t pid = fork();
    int status;

    if (pid == 0) {
        _exit(row() ? 0 : 1);
    }
    return pid > 0 && waitpid(pid, &status, 0) == pid && WIFEXITED(status) &&
           WEXITSTATUS(status) == 0;
}

/**
 * Run `fsuid_capset` and `fsuid_no_fixup` each in a forked child, with
 * ROOT_ONLY made for them and removed after. Returns 0, or 1 after the
 * failure is printed.
 */
static int fsuid_rows(void)
{
    int fd = open(ROOT_ONLY, O_RDWR | O_CREAT | O_TRUNC, 0600), check = CHECKS;

    if (fd < 0 || fchmod(fd, 0600) != 0) {
        check = FSUID_CAPSET;
    } else if (!in_child(fsuid_capset)) {
        check = FSUID_CAPSET;
    } else if (!in_child(fsuid_no_fixup)) {
        check = FSUID_NO_FIXUP;
    }
    if (fd >= 0) {
        close(fd);
    }
    unlink(ROOT_ONLY);
    return check == CHECKS ? 0 : fail(check);
}

/**
 * Run `effective_apart` and `keeps_caps` each in a forked child. Returns 0,
 * or 1 after the failure is printed.
 */
static int sets_rows(void)
{
    const char *path = "/tmp/capmusl.keep";
    pid_t pid = fork();
    int status, fd;

    if (pid == 0) {
        _exit(effective_apart() ? 0 : 1);
    }
    if (pid < 0 || waitpid(pid, &status, 0) != pid || !WIFEXITED(status) ||
        WEXITSTATUS(status) != 0) {
        return fail(EFFECTIVE_APART);
    }
    fd = open(path, O_RDWR | O_CREAT | O_TRUNC, 0600);
    if (fd < 0) {
        return fail(KEEP_CAPS);
    }
    pid = fork();
    if (pid == 0) {
        _exit(keeps_caps(fd) ? 0 : 1);
    }
    close(fd);
    unlink(path);
    if (pid < 0 || waitpid(pid, &status, 0) != pid || !WIFEXITED(status) ||
        WEXITSTATUS(status) != 0) {
        return fail(KEEP_CAPS);
    }
    return 0;
}

int main(int argc, char **argv)
{
    int row;

    probe_args(argc, argv);
    if (probe_words() > 0) {
        return judge_image(probe_word(1), probe_word(2));
    }
    report_begin(report_case(environ, "guest.default.capmusl"));
    if (call_rows() != 0) {
        return 1;
    }
    if (!make_secret()) {
        return fail(MAKE_SECRET);
    }
    row = in_dropped_child();
    unlink(SECRET);
    if (row != 0) {
        return fail(DROPPED_CHILD + row - 1);
    }
    if (later_rows() != 0) {
        return 1;
    }
    if (sets_rows() != 0 || fsuid_rows() != 0) {
        return 1;
    }

    report_at(CHECKS);
    printf("CAPMUSL OK\n");

    return 0;
}
