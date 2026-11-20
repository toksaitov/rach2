/**
 * nsmusl: check namespaces through musl's unshare(), setns() and the clone
 * call.
 *
 * A UTS namespace has its own host name, its /proc/self/ns link names the first
 * one before, and setns returns to it. A PID namespace numbers its first
 * process 1, whose parent it sees as 0, and that process's child 2; the first
 * process's status file shows it one number, and shows its parent, outside
 * the namespace, both; get_robust_list of pid 1 gives that process's own
 * list, and get_robust_list and kill of a number the namespace has not given
 * fail with ESRCH; the link of its descriptor of a /proc file names it 1, and
 * its entry in a listing of /proc has the inode of /proc/1; a name relative
 * to its descriptor of /proc/self/fd reaches its own descriptor; and a group
 * whose leader it has reaped keeps the leader's number there, which getpgid
 * of a member reads and kill of its negation reaches; getppid of a child whose
 * parent ends and is reaped while it calls gives the parent's number or 1,
 * never 0. The
 * caller of unshare keeps its own pid. When it ends, its namespace's init
 * adopts its children, including the init of a namespace below it and that
 * init's sibling. In a new network namespace a
 * connection to the loopback address fails with ENETUNREACH. A mount made in
 * a new mount namespace is not seen outside it, and a new IPC namespace has
 * none of the first's System V objects. Forty mount namespaces exist at
 * once, past the 32 Rach2 once had room for, each with a mount of its own
 * that it alone sees. A map of 340 extents, the most Linux takes, written by
 * root from outside the namespace as newuidmap writes one, is taken, a 341st
 * extent is EINVAL, the map reads back sorted by the first id, and ids map
 * through it both ways.
 *
 * As uid 1000, one clone makes a user, a PID and a network namespace, as
 * Chromium's sandbox does; in them the child is pid 1, with uid 65534 until a
 * map covers it. After unshare of a user namespace the process writes its
 * maps, is uid 0, reads its map back with its uid in the parent's terms,
 * cannot write the map twice, may chroot, and may make the other kinds. With
 * its own uid mapped to itself, it runs a set-uid program of root's as itself,
 * since its namespace does not map root. A helper that shares its directories
 * chroots to /proc/self/fdinfo and ends, as Chromium's sandbox has one do, and
 * the process is left with a root that has nothing in it, while a descriptor
 * of /proc it opened before still reaches /proc/self/fd. A file of
 * /proc/self/fdinfo has a descriptor's offset and flags. On x86-64 an unshare
 * of a flag past 32 bits is EINVAL.
 *
 * Each check that changes a namespace or an id runs in a child of its own (the
 * mount check forks its own child), so what it changes ends with the child.
 * Prints "NSMUSL OK" on success and "NSMUSL FAIL (detail)" otherwise.
 *
 * Writes etude's report records (probe_report.h) for the case ETUDE_CASE
 * names, an assertion for each check, as `assertion_id` lists them, all at
 * the end. A child exits with the number of its first failed row plus one,
 * which the parent takes as that row's failure; the rows before it held, and
 * those after it did not run. The branch of the group `arch` is `x86-64`,
 * with `wide-flag-einval`, or `i686`, which has no assertion of its own.
 *
 * Uid and gid 1000 are the NEW of 1000 when a native run gives
 * `ids=OLD:NEW,...` (tests/native/Readme.md), in the maps it writes and reads
 * back too; the argument is passed on to suidhelper. 65534 is the id outside
 * a user namespace's maps, which no argument changes.
 */

#define _GNU_SOURCE
#include <arpa/inet.h>
#include <dirent.h>
#include <errno.h>
#include <fcntl.h>
#include <netinet/in.h>
#include <sched.h>
#include <signal.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/ipc.h>
#include <sys/mount.h>
#include <sys/prctl.h>
#include <sys/shm.h>
#include <sys/socket.h>
#include <sys/stat.h>
#include <sys/syscall.h>
#include <sys/utsname.h>
#include <sys/wait.h>
#include <unistd.h>

#include "../probe_args.h"
#include "../probe_report.h"

extern char **environ;

/* The key of the System V segment the first IPC namespace has ("NSMU"). */
#define SHM_KEY 0x4e534d55

/* The uid and gid the unprivileged checks take, 1000 or its native NEW. */
#define NS_USER ((unsigned)probe_id(1000))

/* The rows, a check each, in the order the program makes them. */
enum row {
    R_SEGMENT,
#if defined __x86_64__
    R_WIDE,
#endif
    R_UTS_LINK,
    R_UTS_FIRST,
    R_UTS_NAME,
    R_UTS_SETNS,
    R_PID_KEEP,
    R_PID_ONE,
    R_PID_TWO,
    R_PID_NSPID,
    R_PID_ROBUST,
    R_PID_ROBUST_ESRCH,
    R_PID_KILL_ESRCH,
    R_PID_LINK,
    R_PID_INODE,
    R_PID_FD,
    R_PID_GROUP,
    R_PID_PARENT,
    R_ORPHAN_PARENT,
    R_ORPHAN_ADOPT,
    R_PID_BOTH,
    R_NET,
    R_IPC,
    R_MNT,
    R_MNT_OUTSIDE,
    R_MNT_INSIDE,
    R_MNT_LISTED,
    R_MNT_HELD,
    R_MNT_MANY,
    R_MNT_MANY_OWN,
    R_MAP_EXTRA,
    R_MAP_TAKEN,
    R_MAP_SORTED,
    R_MAP_IDS,
    R_SANDBOX_USER,
    R_SANDBOX,
    R_USER_BECOME,
    R_USER_UNMAPPED,
    R_USER_MAPS,
    R_USER_IDS,
    R_USER_BACK,
    R_USER_ONCE,
    R_USER_CHROOT,
    R_USER_KINDS,
    R_SETID_MAP,
    R_SETID_EXEC,
    R_FDINFO_OPEN,
    R_FDINFO,
    R_EMPTY_USER,
    R_EMPTY_HELPER,
    R_EMPTY_NOTHING,
    R_EMPTY_TAKES,
    R_EMPTY_PROC,
    ROWS
};

/* The assertion of each row. */
static const char *const assertion_id[ROWS] = {"segment",
#if defined __x86_64__
                                               "wide-flag-einval",
#endif
                                               "uts-link-read",
                                               "uts-first-link",
                                               "uts-own-hostname",
                                               "uts-setns-back",
                                               "pid-caller-keeps-pid",
                                               "pid-first-is-1",
                                               "pid-child-is-2",
                                               "pid-status-one-number",
                                               "pid-robust-list",
                                               "pid-robust-list-esrch",
                                               "pid-kill-esrch",
                                               "pid-proc-link",
                                               "pid-proc-inode",
                                               "pid-own-fd-dir",
                                               "pid-reaped-leader-group",
                                               "pid-parent-reaped",
                                               "orphans-parent-exits",
                                               "orphans-adopted",
                                               "pid-status-both-numbers",
                                               "net-unreachable",
                                               "ipc-no-objects",
                                               "mount-in-namespace",
                                               "mount-unseen-outside",
                                               "mount-seen-inside",
                                               "mount-listed-by-pid",
                                               "mount-list-held",
                                               "mount-forty-namespaces",
                                               "mount-forty-own-views",
                                               "map-341st-einval",
                                               "map-340-extents",
                                               "map-reads-sorted",
                                               "map-ids-both-ways",
                                               "sandbox-uid-1000",
                                               "sandbox-clone",
                                               "user-uid-1000",
                                               "user-unmapped-65534",
                                               "user-write-maps",
                                               "user-mapped-ids",
                                               "user-map-reads-back",
                                               "user-map-once",
                                               "user-chroot",
                                               "user-other-kinds",
                                               "setid-own-map",
                                               "setid-runs-as-self",
                                               "fdinfo-open",
                                               "fdinfo-offset-flags",
                                               "empty-uid-1000",
                                               "empty-helper-chroot",
                                               "empty-root-no-proc",
                                               "empty-root-takes-nothing",
                                               "empty-proc-reaches-fd"};

/* The earliest row this process has seen fail, or -1, and the detail. */
static int failed_row = -1;
static const char *failed_what;

/** Take `row` as failed for `what` when no earlier row has failed. */
static void row_failed(int row, const char *what)
{
    if (failed_row < 0 || row < failed_row) {
        failed_row = row;
        failed_what = what;
    }
}

/** Report one failed check of `row` and return 1, a child's failure status. */
static int fail(int row, const char *what)
{
    printf("NSMUSL FAIL (%s: errno %d)\n", what, errno);
    fflush(stdout);
    row_failed(row, what);
    return 1;
}

/** The exit status of a child whose check failed: its failed row plus one. */
static int failed_status(void)
{
    return failed_row >= 0 ? failed_row + 1 : ROWS + 1;
}

/**
 * Take the exit status of a child whose checks have rows as its failure:
 * the row its status names, or `row` for a status no row gives. Returns 0
 * for a child that exited 0, and 1 otherwise.
 */
static int child_failed(int ok, int status, int row)
{
    int code = ok && WIFEXITED(status) ? WEXITSTATUS(status) : -1;

    if (code == 0) {
        return 0;
    }
    row_failed(code >= 1 && code <= ROWS ? code - 1 : row,
               "a check in a child failed; its NSMUSL FAIL line names it");
    return 1;
}

/** Run `fn`, whose first row is `row`, in a child; 0 when it exits 0. */
static int in_child(int (*fn)(void), int row)
{
    pid_t pid;
    int ok, status;

    fflush(stdout);
    pid = fork();
    if (pid == 0) {
        _exit(fn() ? failed_status() : 0);
    }
    ok = waitpid(pid, &status, 0) == pid;
    return child_failed(ok, status, row);
}

/** Report each row: held before the failed one, failed, or not run after. */
static void report_rows(void)
{
    int i;

    for (i = 0; i < ROWS; i++) {
        if (failed_row < 0 || i < failed_row) {
            report_pass(assertion_id[i]);
        } else if (i == failed_row) {
            report_fail(assertion_id[i], failed_what);
        } else {
            report_notrun(assertion_id[i], "an earlier check failed");
        }
    }
    report_end();
}

/** Write `text` to the file `path`; 0 when every byte is taken. */
static int put(const char *path, const char *text)
{
    int fd = open(path, O_WRONLY);
    ssize_t n;

    if (fd < 0) {
        return -1;
    }
    n = write(fd, text, strlen(text));
    close(fd);
    return n == (ssize_t)strlen(text) ? 0 : -1;
}

/**
 * Read the file `path` into `buf` of `cap` bytes, NUL-terminated; the byte
 * count, or -1 when the file cannot be opened or read.
 */
static int get(const char *path, char *buf, int cap)
{
    int fd = open(path, O_RDONLY);
    ssize_t n;

    if (fd < 0) {
        return -1;
    }
    n = read(fd, buf, (size_t)cap - 1);
    close(fd);
    buf[n > 0 ? n : 0] = '\0';
    return (int)n;
}

/**
 * Whether a TCP connection to the loopback address fails with ENETUNREACH, as
 * it does outside the first network namespace.
 */
static int unreachable(void)
{
    struct sockaddr_in sa;
    int fd = socket(AF_INET, SOCK_STREAM, 0);

    memset(&sa, 0, sizeof(sa));
    sa.sin_family = AF_INET;
    sa.sin_port = htons(22);
    sa.sin_addr.s_addr = htonl(INADDR_LOOPBACK);
    return fd >= 0 && connect(fd, (struct sockaddr *)&sa, sizeof(sa)) == -1 && errno == ENETUNREACH;
}

/**
 * Check the UTS namespace: the first one's link, a host name of a new one's
 * own, and setns back to the first. Returns 0, or 1 after printing the
 * failure.
 */
static int uts(void)
{
    struct utsname u;
    char before[65], link[64];
    int fd = open("/proc/self/ns/uts", O_RDONLY);
    ssize_t n = readlink("/proc/self/ns/uts", link, sizeof(link) - 1);

    if (fd < 0 || n <= 0) {
        return fail(R_UTS_LINK, "open and read the uts link");
    }
    link[n] = '\0';
    if (strcmp(link, "uts:[4026531838]") != 0) {
        return fail(R_UTS_FIRST, "the first uts namespace's link");
    }
    uname(&u);
    strcpy(before, u.nodename);
    if (unshare(CLONE_NEWUTS) != 0 || sethostname("nsbox", 5) != 0 || uname(&u) != 0 ||
        strcmp(u.nodename, "nsbox") != 0) {
        return fail(R_UTS_NAME, "a new uts namespace has its own host name");
    }
    if (setns(fd, CLONE_NEWUTS) != 0 || uname(&u) != 0 || strcmp(u.nodename, before) != 0) {
        return fail(R_UTS_SETNS, "setns returns to the first uts namespace");
    }
    return 0;
}

/** Whether the NSpid line of /proc/`who`/status has the numbers `want`. */
static int nspid_is(const char *who, const char *want)
{
    char path[64], buf[1024];
    const char *line, *nl;

    snprintf(path, sizeof(path), "/proc/%s/status", who);
    if (get(path, buf, sizeof(buf)) < 0) {
        return 0;
    }
    line = strstr(buf, "NSpid:\t");
    nl = line ? strchr(line, '\n') : NULL;
    return nl && (size_t)(nl - line - 7) == strlen(want) && !strncmp(line + 7, want, strlen(want));
}

/**
 * Check that a name relative to a descriptor of /proc/self/fd reaches the
 * caller's own descriptor, as Chromium's sandbox checks its descriptors from
 * inside its PID namespace. The descriptor is a pipe's, which no other process
 * has, where descriptor 0 would be the console that most processes share.
 * Returns 0, or 1 after printing the failure.
 */
static int own_fd_dir(void)
{
    struct stat entry, own;
    char name[16];
    int dir = open("/proc/self/fd", O_RDONLY | O_DIRECTORY);
    int p[2];

    if (dir < 0 || pipe(p) != 0) {
        return fail(R_PID_FD, "a descriptor of its /proc/self/fd reaches its own descriptors");
    }
    snprintf(name, sizeof(name), "%d", p[0]);
    if (fstatat(dir, name, &entry, 0) != 0 || fstat(p[0], &own) != 0 ||
        entry.st_ino != own.st_ino || entry.st_dev != own.st_dev) {
        return fail(R_PID_FD, "a descriptor of its /proc/self/fd reaches its own descriptors");
    }
    close(p[0]);
    close(p[1]);
    close(dir);
    return 0;
}

/**
 * Check that /proc names the caller, the first process of a PID namespace, by
 * its number 1 there: the link of its descriptor of /proc/self/task/1/stat is
 * /proc/1/task/1/stat, and the entry 1 of a listing of /proc has the inode
 * that stat of /proc/1 gives. Returns 0, or 1 after printing the failure.
 */
static int proc_own_number(void)
{
    char link[64];
    const struct dirent *e;
    struct stat st;
    ssize_t n = -1;
    ino_t listed = 0;
    DIR *d;
    int fd = open("/proc/self/task/1/stat", O_RDONLY);

    if (fd >= 0) {
        char path[32];

        snprintf(path, sizeof(path), "/proc/self/fd/%d", fd);
        n = readlink(path, link, sizeof(link) - 1);
        close(fd);
    }
    if (n >= 0) {
        link[n] = '\0';
    }
    if (n < 0 || strcmp(link, "/proc/1/task/1/stat") != 0) {
        return fail(R_PID_LINK, "the link of its /proc file names it 1");
    }
    if (stat("/proc/1", &st) != 0 || !(d = opendir("/proc"))) {
        return fail(R_PID_INODE, "the entry 1 of /proc has the inode of /proc/1");
    }
    while ((e = readdir(d)) != NULL) {
        if (strcmp(e->d_name, "1") == 0) {
            listed = e->d_ino;
        }
    }
    closedir(d);
    if (listed != st.st_ino) {
        return fail(R_PID_INODE, "the entry 1 of /proc has the inode of /proc/1");
    }
    return 0;
}

/**
 * Make two children in a new PID namespace, and send their outer pids to
 * `report`. Each child waits for EOF on its pipe, then checks that its parent
 * is outside its namespace. Returns 0 on success, 1 on a setup failure.
 */
static int make_pid_orphans(int report, int go[2][2])
{
    pid_t kids[2] = {0, 0};
    int i, rc = 0;

    if (unshare(CLONE_NEWPID) != 0) {
        return fail(R_ORPHAN_PARENT, "unshare for orphan adoption");
    }
    for (i = 0; i < 2; i++) {
        kids[i] = fork();
        if (kids[i] == 0) {
            char c;

            close(report);
            close(go[0][1]);
            close(go[1][1]);
            close(go[1 - i][0]);
            rc = read(go[i][0], &c, 1) != 0 || getppid() != 0;
            close(go[i][0]);
            _exit(
                rc ? fail(R_ORPHAN_ADOPT, "an adopted child's parent is outside its pid namespace")
                   : 20 + i);
        }
        if (kids[i] < 0) {
            rc = fail(R_ORPHAN_PARENT, "fork for orphan adoption");
            break;
        }
    }
    if (!rc && write(report, kids, sizeof(kids)) != (ssize_t)sizeof(kids)) {
        rc = fail(R_ORPHAN_PARENT, "report orphan pids");
    }
    if (rc) {
        for (i = 1; i >= 0; i--) {
            if (kids[i] > 0) {
                kill(kids[i], SIGKILL);
                waitpid(kids[i], NULL, 0);
            }
        }
    }
    return rc;
}

/**
 * As init, reap a parent that unshares its children's PID namespace. Each
 * child must be waitable here before it is released, including the new
 * namespace's init. Returns 0 on success, 1 on failure. All pipes
 * are closed and known children are ended and reaped on either path.
 */
static int pid_orphans(void)
{
    int report[2] = {-1, -1}, go[2][2] = {{-1, -1}, {-1, -1}};
    pid_t parent = -1, kids[2] = {0, 0};
    int i, j, status, rc = 0;

    if (pipe(report) != 0 || pipe(go[0]) != 0 || pipe(go[1]) != 0) {
        rc = fail(R_ORPHAN_PARENT, "pipes for orphan adoption");
        goto out;
    }
    parent = fork();
    if (parent == 0) {
        close(report[0]);
        _exit(make_pid_orphans(report[1], go));
    }
    close(report[1]);
    report[1] = -1;
    if (parent < 0 || read(report[0], kids, sizeof(kids)) != (ssize_t)sizeof(kids) ||
        waitpid(parent, &status, 0) != parent || !WIFEXITED(status) || WEXITSTATUS(status) != 0) {
        rc = fail(R_ORPHAN_PARENT, "the parent of the pid namespace's orphans exited");
        goto out;
    }
    parent = -1;
    for (i = 1; i >= 0; i--) {
        if (waitpid(kids[i], &status, WNOHANG) != 0) {
            rc = fail(R_ORPHAN_ADOPT, i == 0
                                          ? "the outer init adopted the namespace's init"
                                          : "the outer init adopted the namespace init's sibling");
        }
        close(go[i][1]);
        go[i][1] = -1;
        if (waitpid(kids[i], &status, 0) != kids[i] || !WIFEXITED(status) ||
            WEXITSTATUS(status) != 20 + i) {
            rc = fail(R_ORPHAN_ADOPT, "the outer init reaped the pid namespace's orphan");
        } else {
            kids[i] = 0;
        }
    }
out:
    if (parent > 0) {
        kill(parent, SIGKILL);
        waitpid(parent, NULL, 0);
    }
    for (i = 1; i >= 0; i--) {
        if (kids[i] > 0) {
            kill(kids[i], SIGKILL);
            waitpid(kids[i], NULL, 0);
        }
        for (j = 0; j < 2; j++) {
            if (go[i][j] >= 0) {
                close(go[i][j]);
            }
        }
        if (report[i] >= 0) {
            close(report[i]);
        }
    }
    return rc;
}

/**
 * Check, as the first process of a PID namespace, that a group keeps its
 * number there once its leader is reaped, as Linux's struct pid lasts while
 * a member has it: the leader makes its group, forks a member, which waits
 * for EOF on `go`, and ends, and this process reaps it; getpgid of the
 * member, now this process's child, reads the leader's number, and kill of
 * its negation reaches the group. Returns 0, or 1 after printing the
 * failure.
 */
static int pid_group_reaped(void)
{
    int go[2], rep[2], status, rc = 0;
    pid_t leader, member = -1;

    if (pipe(go) != 0 || pipe(rep) != 0) {
        return fail(R_PID_GROUP, "pipes for a reaped leader's group");
    }
    leader = fork();
    if (leader == 0) {
        pid_t m;
        char c;

        close(rep[0]);
        close(go[1]);
        if (setpgid(0, 0) != 0) {
            _exit(1);
        }
        m = fork();
        if (m == 0) {
            close(rep[1]);
            _exit(read(go[0], &c, 1) == 0 ? 0 : 1);
        }
        _exit(m > 0 && write(rep[1], &m, sizeof(m)) == (ssize_t)sizeof(m) ? 0 : 1);
    }
    close(rep[1]);
    close(go[0]);
    if (leader < 0 || read(rep[0], &member, sizeof(member)) != (ssize_t)sizeof(member) ||
        waitpid(leader, &status, 0) != leader || !WIFEXITED(status) || WEXITSTATUS(status) != 0) {
        rc = fail(R_PID_GROUP, "a leader that made its group and forked a member is reaped");
    } else if (getpgid(member) != leader || kill(-leader, 0) != 0) {
        rc = fail(R_PID_GROUP, "its group keeps the leader's number once the leader is reaped");
    }
    close(go[1]);
    close(rep[0]);
    if (member > 0) {
        waitpid(member, &status, 0);
    }
    return rc;
}

/* The rounds of `pid_parent_reaped`, and the getppid calls each child makes
 * once it has seen its new parent. */
#define PARENT_ROUNDS 16
#define PARENT_CALLS  2000

/**
 * The child of `parent_of_caller`: tell its parent `self` through `go` that
 * it has begun, then call getppid until it has seen 1, its new parent,
 * PARENT_CALLS times. Exits 0, 2 for a number that is neither `self` nor
 * 1, and 1 when it cannot write `go`.
 */
static void call_getppid(pid_t self, int go)
{
    int seen = 0;
    pid_t pp;

    if (write(go, "x", 1) != 1) {
        _exit(1);
    }
    while (seen < PARENT_CALLS) {
        pp = getppid();
        if (pp == 1) {
            seen++;
        } else if (pp != self) {
            _exit(2);
        }
    }
    _exit(0);
}

/**
 * The parent of `pid_parent_reaped`'s round: fork a child that calls
 * getppid, send its pid on `rep`, wait on `go` until it has begun, and end.
 * Exits 0, or 1 on a failure.
 */
static void parent_of_caller(int go[2], int rep)
{
    pid_t self = getpid(), c;
    char ch;

    c = fork();
    if (c == 0) {
        close(rep);
        close(go[0]);
        call_getppid(self, go[1]);
    }
    close(go[1]);
    _exit(c > 0 && write(rep, &c, sizeof(c)) == (ssize_t)sizeof(c) && read(go[0], &ch, 1) == 1 ? 0
                                                                                               : 1);
}

/**
 * Check, as the first process of a PID namespace, that getppid of a child
 * whose parent ends and is reaped while it calls gives the parent's number
 * or 1, the init that adopts it, and never 0, as Linux reads `real_parent`
 * and translates it in one RCU section. A parent forks a child, which tells
 * it that it has begun and calls getppid (`parent_of_caller`,
 * `call_getppid`); the parent then ends, and this process reaps it at once
 * while the child calls, and then reaps the child. The window lies between
 * the read of the parent's ID and its translation, so PARENT_ROUNDS rounds
 * run. Returns 0, or 1 after printing the failure.
 */
static int pid_parent_reaped(void)
{
    int round, go[2], rep[2], status, rc = 0;
    pid_t parent, child;

    for (round = 0; round < PARENT_ROUNDS && !rc; round++) {
        if (pipe(go) != 0) {
            return fail(R_PID_PARENT, "a pipe for a reaped parent's child");
        }
        if (pipe(rep) != 0) {
            close(go[0]);
            close(go[1]);
            return fail(R_PID_PARENT, "a pipe for a reaped parent's child");
        }
        parent = fork();
        if (parent == 0) {
            close(rep[0]);
            parent_of_caller(go, rep[1]);
        }
        close(go[0]);
        close(go[1]);
        close(rep[1]);
        child = -1;
        if (parent < 0 || read(rep[0], &child, sizeof(child)) != (ssize_t)sizeof(child) ||
            waitpid(parent, &status, 0) != parent || !WIFEXITED(status) ||
            WEXITSTATUS(status) != 0) {
            rc = fail(R_PID_PARENT, "a parent that forked a calling child ends and is reaped");
        }
        close(rep[0]);
        if (child > 0 &&
            (waitpid(child, &status, 0) != child || !WIFEXITED(status) ||
             WEXITSTATUS(status) != 0) &&
            !rc) {
            rc = fail(R_PID_PARENT, "getppid of a child whose parent is reaped gives its number "
                                    "or 1, never 0");
        }
    }
    return rc;
}

/** A new PID 1 cannot use its same-process exception on the old PID 1. */
static int old_fd_dir_denied(int fd)
{
    int denied;

    if (seteuid(NS_USER) != 0)
        return 0;
    errno = 0;
    denied =
        syscall(SYS_faccessat2, fd, "", W_OK, AT_EMPTY_PATH | AT_EACCESS) < 0 && errno == EACCES;
    return seteuid(0) == 0 && denied;
}

/** A restricted openat2 walk and its final open both use the held mount. */
static int proc_scoped_self(int dir, const char *outer, ssize_t len)
{
    unsigned long long how[3] = {O_PATH | O_NOFOLLOW, 0, 8}; /* RESOLVE_BENEATH */
    char number[32];
    int fd = (int)syscall(SYS_openat2, dir, "self", how, sizeof(how));
    int ok = fd >= 0 && readlinkat(fd, "", number, sizeof(number)) == len &&
             memcmp(number, outer, len) == 0;

    if (fd >= 0)
        close(fd);
    return ok;
}

/** A held directory keeps its mount after a chroot shortens its name. */
static int proc_chroot_place(const char *place)
{
    int status, dir;
    char before[32], after[32];
    ssize_t n;
    pid_t child = fork();

    if (child == 0) {
        dir = open(place, O_RDONLY | O_DIRECTORY);
        n = dir < 0 ? -1 : readlinkat(dir, "self", before, sizeof(before));
        _exit(n <= 0 || unshare(CLONE_NEWNS) != 0 || mount("tmpfs", place, "tmpfs", 0, NULL) != 0 ||
              chroot("/tmp") != 0 || chdir("/") != 0 ||
              readlinkat(dir, "self", after, sizeof(after)) != n || memcmp(before, after, n) != 0);
    }
    return child > 0 && waitpid(child, &status, 0) == child && WIFEXITED(status) &&
           WEXITSTATUS(status) == 0;
}

/** A proc mount has the directory name chosen by its caller. */
static int proc_other_place(void)
{
    char place[] = "/tmp/nsproc-XXXXXX";
    char cwd[64], number[16];
    int ok;

    if (!mkdtemp(place))
        return 0;
    ok = mount("proc", place, "proc", 0, NULL) == 0;
    if (ok) {
        ok = chdir(place) == 0 && getcwd(cwd, sizeof(cwd)) && strcmp(cwd, place) == 0 &&
             readlink("self", number, sizeof(number)) == 1 && number[0] == '1' &&
             proc_chroot_place(place);
        if (chdir("/") != 0 || umount(place) != 0)
            ok = 0;
    }
    return rmdir(place) == 0 && ok;
}

/** Walk through the held directory by its descriptor link and as the cwd. */
static int proc_fd_paths(int dir, const char *outer, ssize_t n)
{
    char via[64], held[32];
    int ok;

    snprintf(via, sizeof(via), "/proc/self/fd/%d/self", dir);
    ok = readlink(via, held, sizeof(held)) == n && memcmp(outer, held, n) == 0 &&
         fchdir(dir) == 0 && readlink("self", held, sizeof(held)) == n &&
         memcmp(outer, held, n) == 0;
    return chdir("/") == 0 && ok;
}

/** Check both proc mounts from the first child of a new PID namespace. */
static int proc_mount_numbers(void)
{
    char outer[32], held[32], inner[32];
    struct stat before, after;
    int dir = open("/proc", O_RDONLY | O_DIRECTORY);
    int fds = open("/proc/1/fd", O_RDONLY | O_DIRECTORY);
    ssize_t n = readlink("/proc/self", outer, sizeof(outer));
    int ok = dir >= 0 && fds >= 0 && n > 0 && (n != 1 || outer[0] != '1') &&
             fstat(fds, &before) == 0 && unshare(CLONE_NEWNS) == 0 &&
             mount("proc", "/proc", "proc", 0, NULL) == 0 &&
             readlink("/proc/self", inner, sizeof(inner)) == 1 && inner[0] == '1' &&
             readlinkat(dir, "self", held, sizeof(held)) == n && memcmp(outer, held, n) == 0 &&
             fstatat(fds, ".", &after, 0) == 0 && before.st_ino == after.st_ino;

    ok = ok && proc_fd_paths(dir, outer, n) && proc_scoped_self(dir, outer, n) &&
         old_fd_dir_denied(fds) && proc_other_place();
    if (dir >= 0)
        close(dir);
    if (fds >= 0)
        close(fds);
    return ok ? 0 : fail(R_PID_LINK, "inherited and new proc mounts keep their PID namespaces");
}

/**
 * Check, as the first process of a new PID namespace, that it is pid 1 with
 * parent 0, that its child is 2, that its NSpid line has the one number, that
 * get_robust_list of pid 1 gives the head it set, that get_robust_list and
 * kill of pid 99, which the namespace has not given, fail with ESRCH, and
 * then `proc_own_number`, `own_fd_dir`, `pid_group_reaped`,
 * `pid_parent_reaped` and `pid_orphans`. Returns 0, or 1 after printing the
 * failure.
 */
static int pid_first(void)
{
    static long head[3];
    long *got = NULL;
    size_t len = 0;
    pid_t pid;
    int status;

    if (getpid() != 1 || getppid() != 0) {
        return fail(R_PID_ONE, "the first process of a pid namespace is 1, its parent 0");
    }
    pid = fork();
    if (pid == 0) {
        _exit(getpid() == 2 && getppid() == 1 ? 0 : 1);
    }
    if (pid != 2 || waitpid(pid, &status, 0) != 2 || !WIFEXITED(status) ||
        WEXITSTATUS(status) != 0) {
        return fail(R_PID_TWO, "its child is 2");
    }
    if (proc_mount_numbers() != 0) {
        return 1;
    }
    if (!nspid_is("self", "1")) {
        return fail(R_PID_NSPID, "its status file shows it the one number");
    }
    head[0] = (long)head;
    if (syscall(SYS_set_robust_list, head, sizeof(head)) != 0 ||
        syscall(SYS_get_robust_list, 1, &got, &len) != 0 || got != head || len != sizeof(head)) {
        return fail(R_PID_ROBUST, "get_robust_list of pid 1 gives the first process's own list");
    }
    errno = 0;
    if (syscall(SYS_get_robust_list, 99, &got, &len) != -1 || errno != ESRCH) {
        return fail(R_PID_ROBUST_ESRCH,
                    "get_robust_list of a pid the namespace has not given is ESRCH");
    }
    errno = 0;
    if (kill(99, 0) != -1 || errno != ESRCH) {
        return fail(R_PID_KILL_ESRCH, "kill of a pid the namespace has not given is ESRCH");
    }
    return proc_own_number() || own_fd_dir() || pid_group_reaped() || pid_parent_reaped() ||
           pid_orphans();
}

/**
 * Check that unshare of a PID namespace keeps the caller's pid, run
 * `pid_first` in the namespace's first process, and check, while a pipe holds
 * that process, that its status file seen from outside shows both of its
 * numbers. Returns 0, or 1 after printing the failure.
 */
static int pid_ns(void)
{
    pid_t self = getpid(), first;
    char want[32], who[16], c = 'x';
    int go[2], ok, status, rc = 0;

    if (unshare(CLONE_NEWPID) != 0 || getpid() != self || pipe(go) != 0) {
        return fail(R_PID_KEEP, "unshare of a pid namespace keeps the caller's pid");
    }
    fflush(stdout);
    first = fork();
    if (first == 0) {
        rc = pid_first();
        read(go[0], &c, 1);
        _exit(rc ? failed_status() : 0);
    }
    snprintf(who, sizeof(who), "%d", (int)first);
    snprintf(want, sizeof(want), "%d\t1", (int)first);
    if (!nspid_is(who, want)) {
        rc = fail(R_PID_BOTH, "its parent's status file shows both numbers");
    }
    write(go[1], &c, 1);
    ok = waitpid(first, &status, 0) == first;
    if (child_failed(ok, status, R_PID_ONE)) {
        rc = 1;
    }
    return rc;
}

/** Check that a new network namespace reaches nothing; 0, or 1. */
static int net(void)
{
    if (unshare(CLONE_NEWNET) != 0 || !unreachable()) {
        return fail(R_NET, "a new network namespace reaches nothing");
    }
    return 0;
}

/** Check that a new IPC namespace lacks the segment at `SHM_KEY`; 0, or 1. */
static int ipc(void)
{
    if (unshare(CLONE_NEWIPC) != 0 || shmget(SHM_KEY, 0, 0) != -1 || errno != ENOENT) {
        return fail(R_IPC, "a new ipc namespace has none of the first's objects");
    }
    return 0;
}

/**
 * Whether /proc/PID/mounts and /proc/PID/mountinfo list the tmpfs `mnt`
 * mounts in the namespace of `pid`, and /proc/self/mounts does not, as
 * Linux lists a file's mounts from its process's namespace.
 */
static int listed_for(pid_t pid)
{
    char path[64], buf[4096];

    snprintf(path, sizeof(path), "/proc/%ld/mounts", (long)pid);
    if (get(path, buf, sizeof(buf)) <= 0 || !strstr(buf, "nsmusl /tmp/nsmnt tmpfs ")) {
        return 0;
    }
    snprintf(path, sizeof(path), "/proc/%ld/mountinfo", (long)pid);
    if (get(path, buf, sizeof(buf)) <= 0 || !strstr(buf, " /tmp/nsmnt ")) {
        return 0;
    }
    return get("/proc/self/mounts", buf, sizeof(buf)) > 0 && !strstr(buf, "/tmp/nsmnt");
}

/**
 * Check that a tmpfs mounted in a child's new mount namespace, and a file
 * made in it, are seen by the child and not by this process. Two pipes keep
 * the child in place while this process looks; a child that fails before its
 * write closes the only writer of the first, and its status is the failure.
 * Returns 0, or 1 after printing the failure.
 */
static int mnt(void)
{
    int up[2], down[2];
    pid_t pid;
    int fd, held = -1, ok, status, rc = 0;
    char c = 'x', path[64], buf[4096];
    struct stat st;
    ssize_t n;

    mkdir("/tmp/nsmnt", 0755);
    if (pipe(up) != 0 || pipe(down) != 0) {
        return fail(R_MNT, "pipe");
    }
    fflush(stdout);
    pid = fork();
    if (pid == 0) {
        if (unshare(CLONE_NEWNS) != 0 || mount("nsmusl", "/tmp/nsmnt", "tmpfs", 0, NULL) != 0 ||
            (fd = open("/tmp/nsmnt/inner", O_CREAT | O_WRONLY, 0644)) < 0) {
            fail(R_MNT, "mount tmpfs in a new mount namespace");
            _exit(failed_status());
        }
        close(fd);
        write(up[1], &c, 1);
        read(down[0], &c, 1);
        _exit(stat("/tmp/nsmnt/inner", &st) == 0 ? 0 : R_MNT_INSIDE + 1);
    }
    close(up[1]);
    close(down[0]);
    if (read(up[0], &c, 1) == 1) {
        if (stat("/tmp/nsmnt/inner", &st) == 0) {
            rc = fail(R_MNT_OUTSIDE, "the mount is not seen outside its namespace");
        } else if (!listed_for(pid)) {
            rc = fail(R_MNT_LISTED, "/proc/PID/mounts lists the mounts of PID's namespace");
        }
        snprintf(path, sizeof(path), "/proc/%ld/mounts", (long)pid);
        held = open(path, O_RDONLY);
        if (held >= 0 && read(held, buf, sizeof(buf) - 1) <= 0) {
            close(held);
            held = -1;
        }
        write(down[1], &c, 1);
    }
    close(up[0]);
    close(down[1]);
    ok = waitpid(pid, &status, 0) == pid;
    if (child_failed(ok, status, R_MNT)) {
        rc = 1;
    }
    /* The open kept the namespace and the root; a reread at 0 lists them. */
    n = held >= 0 && ok ? pread(held, buf, sizeof(buf) - 1, 0) : -1;
    buf[n > 0 ? n : 0] = '\0';
    if (n <= 0 || !strstr(buf, "nsmusl /tmp/nsmnt tmpfs ")) {
        rc = fail(R_MNT_HELD, "an open /proc/PID/mounts lists PID's namespace after PID is reaped");
    }
    if (held >= 0) {
        close(held);
    }
    return rc;
}

/* The mount namespaces `many_mnt` makes at once. */
#define MANY 40

/** The entries of the directory `path` other than . and .., or -1. */
static int dir_entries(const char *path)
{
    DIR *d = opendir(path);
    struct dirent *de;
    int n = 0;

    if (!d) {
        return -1;
    }
    while ((de = readdir(d)) != NULL) {
        n += strcmp(de->d_name, ".") != 0 && strcmp(de->d_name, "..") != 0;
    }
    closedir(d);
    return n;
}

/**
 * One of `many_mnt`'s children, number `i`: unshare a mount namespace, mount
 * a tmpfs on /tmp/nsmany, make a file named by its number there, say so on
 * `ready` ('x', or 'n' on a failure), and once `go` closes, exit 0 when its
 * file is the only entry it sees.
 */
static void many_child(int i, int ready, int go)
{
    char path[64], c = 'x';
    int fd;

    snprintf(path, sizeof(path), "/tmp/nsmany/%d", i);
    if (unshare(CLONE_NEWNS) != 0 || mount("nsmany", "/tmp/nsmany", "tmpfs", 0, NULL) != 0 ||
        (fd = open(path, O_CREAT | O_WRONLY, 0644)) < 0) {
        c = 'n';
        write(ready, &c, 1);
        _exit(1);
    }
    close(fd);
    write(ready, &c, 1);
    read(go, &c, 1);
    _exit(dir_entries("/tmp/nsmany") == 1 && access(path, F_OK) == 0 ? 0 : 1);
}

/**
 * Check MANY mount namespaces at once, each with a tmpfs on /tmp/nsmany and a
 * file of its own there: every one is made, the caller sees none of their
 * files, and each sees its own alone. Returns 0, or 1 after printing the
 * failure.
 */
static int many_mnt(void)
{
    int ready[2], go[2], i, status, made = 0, own = 1;
    pid_t pids[MANY];
    char c;

    mkdir("/tmp/nsmany", 0755);
    if (pipe(ready) != 0 || pipe(go) != 0) {
        return fail(R_MNT_MANY, "pipe");
    }
    for (i = 0; i < MANY; i++) {
        fflush(stdout);
        pids[i] = fork();
        if (pids[i] == 0) {
            close(ready[0]);
            close(go[1]);
            many_child(i, ready[1], go[0]);
        }
    }
    close(ready[1]);
    close(go[0]);
    for (i = 0; i < MANY; i++) {
        made += pids[i] > 0 && read(ready[0], &c, 1) == 1 && c == 'x';
    }
    if (made == MANY && dir_entries("/tmp/nsmany") != 0) {
        made = -1;
    }
    close(go[1]);
    for (i = 0; i < MANY; i++) {
        if (pids[i] > 0) {
            own &= waitpid(pids[i], &status, 0) == pids[i] && WIFEXITED(status) &&
                   WEXITSTATUS(status) == 0;
        }
    }
    close(ready[0]);
    rmdir("/tmp/nsmany");
    if (made != MANY) {
        return fail(R_MNT_MANY, made < 0 ? "a namespace's mount is seen outside it"
                                         : "40 mount namespaces at once, each with a tmpfs");
    }
    return own ? 0 : fail(R_MNT_MANY_OWN, "each of 40 namespaces sees its own mount alone");
}

/* The extents of the long map, the most Linux takes. */
#define EXTENTS 340

/** The text of a map of `n` extents, `2k 3k 1` for k from n - 1 down to 0,
 * into `out` of `cap` bytes. */
static void descending(char *out, int cap, int n)
{
    int k, at = 0;

    for (k = n - 1; k >= 0 && at < cap; k--) {
        at += snprintf(out + at, (size_t)(cap - at), "%d %d 1\n", 2 * k, 3 * k);
    }
}

/** Read the whole of the file `path` into `buf` of `cap` bytes, by as many
 * reads as it takes, NUL-terminated; the byte count, or -1. */
static int slurp(const char *path, char *buf, int cap)
{
    int fd = open(path, O_RDONLY), at = 0;
    ssize_t n = 0;

    if (fd < 0) {
        return -1;
    }
    while (at < cap - 1 && (n = read(fd, buf + at, (size_t)(cap - 1 - at))) > 0) {
        at += (int)n;
    }
    close(fd);
    buf[at] = '\0';
    return n < 0 ? -1 : at;
}

/** The kernel's real uid of the process `pid`, from its status file, or -1. */
static long status_uid(pid_t pid)
{
    char path[64], buf[2048], *line;

    snprintf(path, sizeof(path), "/proc/%d/status", (int)pid);
    if (slurp(path, buf, sizeof(buf)) <= 0 || !(line = strstr(buf, "\nUid:"))) {
        return -1;
    }
    return strtol(line + 5, NULL, 10);
}

/**
 * The child of `long_map`: unshare a user namespace, say so on `up` ('u',
 * or 'n'), wait for its map on `down`, then set its uid to 400 in the
 * namespace and say so ('s' when its uid read 0 before and 400 after, 'f'
 * otherwise), and wait on `down` again while its status is read.
 */
static void map_child(int up, int down)
{
    char c = 'u';

    if (unshare(CLONE_NEWUSER) != 0) {
        c = 'n';
    }
    write(up, &c, 1);
    if (c == 'n' || read(down, &c, 1) != 1) {
        _exit(1);
    }
    c = getuid() == 0 && setuid(400) == 0 && getuid() == 400 ? 's' : 'f';
    write(up, &c, 1);
    read(down, &c, 1);
    _exit(0);
}

/** Whether `text` has the line of the extent `first lower count` at line
 * `at`, in /proc/PID/uid_map's ten-column form. */
static int map_line(const char *text, int at, unsigned first, unsigned lower, unsigned count)
{
    char want[40];

    snprintf(want, sizeof(want), "%10u %10u %10u\n", first, lower, count);
    return strncmp(text + at * 33, want, 33) == 0;
}

/**
 * The rows of `long_map` on the user namespace of `pid`, whose child talks
 * on `up` and `down`: one more extent than EXTENTS is EINVAL, EXTENTS in
 * descending order are taken and read back sorted by the first id, and the
 * namespace's 0 and 400 are the kernel's 0 and 600. Returns 0, or 1 after
 * printing the failure.
 */
static int map_rows(pid_t pid, int up, int down)
{
    static char text[8192], back[EXTENTS * 33 + 64];
    char path[64], c = 0;

    snprintf(path, sizeof(path), "/proc/%d/uid_map", (int)pid);
    if (read(up, &c, 1) != 1 || c != 'u') {
        return fail(R_MAP_EXTRA, "unshare a user namespace as root");
    }
    descending(text, sizeof(text), EXTENTS + 1);
    if (put(path, text) == 0 || errno != EINVAL) {
        return fail(R_MAP_EXTRA, "a map of 341 extents is not EINVAL");
    }
    descending(text, sizeof(text), EXTENTS);
    if (put(path, text) != 0) {
        return fail(R_MAP_TAKEN, "a map of 340 extents");
    }
    if (slurp(path, back, sizeof(back)) != EXTENTS * 33 || !map_line(back, 0, 0, 0, 1) ||
        !map_line(back, 200, 400, 600, 1)) {
        return fail(R_MAP_SORTED, "the map reads back sorted by the first id");
    }
    if (write(down, &c, 1) != 1 || read(up, &c, 1) != 1 || c != 's' || status_uid(pid) != 600) {
        return fail(R_MAP_IDS, "the namespace's 0 and 400 are the kernel's 0 and 600");
    }
    return 0;
}

/**
 * Check a uid map of EXTENTS extents written by root from outside the
 * namespace, as newuidmap writes one, on a child that unshares a user
 * namespace (`map_rows`). Returns 0, or 1 after printing the failure.
 */
static int long_map(void)
{
    int up[2], down[2], rc, status;
    pid_t pid;

    if (pipe(up) != 0 || pipe(down) != 0) {
        return fail(R_MAP_EXTRA, "pipe");
    }
    fflush(stdout);
    pid = fork();
    if (pid == 0) {
        close(up[0]);
        close(down[1]);
        map_child(up[1], down[0]);
    }
    close(up[1]);
    close(down[0]);
    rc = pid < 0 ? fail(R_MAP_EXTRA, "fork") : map_rows(pid, up[0], down[1]);
    close(down[1]);
    close(up[0]);
    if (pid > 0) {
        waitpid(pid, &status, 0);
    }
    return rc;
}

/**
 * Check Chromium's sandbox clone as uid 1000: user, PID and network
 * namespaces at once, the child pid 1 with uid 65534 and no network. Returns
 * 0, or 1 after printing the failure.
 */
static int sandbox(void)
{
    long pid;
    int status;

    if (setgid(NS_USER) != 0 || setuid(NS_USER) != 0 || prctl(PR_SET_DUMPABLE, 1) != 0) {
        return fail(R_SANDBOX_USER, "become uid 1000");
    }
    fflush(stdout);
    pid = syscall(SYS_clone, CLONE_NEWUSER | CLONE_NEWPID | CLONE_NEWNET | SIGCHLD, 0, 0, 0, 0);
    if (pid == 0) {
        _exit(getpid() == 1 && getuid() == 65534 && unreachable() ? 0 : 1);
    }
    if (pid < 0 || waitpid((pid_t)pid, &status, 0) != pid || !WIFEXITED(status) ||
        WEXITSTATUS(status) != 0) {
        return fail(R_SANDBOX, "one clone makes a user, a pid and a network namespace");
    }
    return 0;
}

/**
 * Check a user namespace made by uid 1000: unmapped ids read as 65534, the
 * maps can be written once and read back, and the mapped root may chroot and
 * make namespaces of the other kinds. Returns 0, or 1 after printing the
 * failure.
 */
static int user(void)
{
    char buf[128], map[32], back[40];

    snprintf(map, sizeof(map), "0 %u 1\n", NS_USER);
    snprintf(back, sizeof(back), "%10u %10u %10u\n", 0u, NS_USER, 1u);
    if (setgid(NS_USER) != 0 || setuid(NS_USER) != 0 || prctl(PR_SET_DUMPABLE, 1) != 0) {
        return fail(R_USER_BECOME, "become uid 1000");
    }
    if (unshare(CLONE_NEWUSER) != 0 || getuid() != 65534 || getgid() != 65534) {
        return fail(R_USER_UNMAPPED, "ids outside the maps read as 65534");
    }
    if (put("/proc/self/uid_map", map) != 0 || put("/proc/self/setgroups", "deny") != 0 ||
        put("/proc/self/gid_map", map) != 0) {
        return fail(R_USER_MAPS, "write the maps");
    }
    if (getuid() != 0 || geteuid() != 0 || getgid() != 0) {
        return fail(R_USER_IDS, "the mapped ids");
    }
    if (get("/proc/self/uid_map", buf, sizeof(buf)) <= 0 || strcmp(buf, back) != 0) {
        return fail(R_USER_BACK, "the map reads back");
    }
    if (put("/proc/self/uid_map", map) == 0 || errno != EPERM) {
        return fail(R_USER_ONCE, "a map is written once");
    }
    if (chroot("/") != 0) {
        return fail(R_USER_CHROOT, "the user namespace's root may chroot");
    }
    if (unshare(CLONE_NEWUTS | CLONE_NEWIPC | CLONE_NEWNET) != 0 || sethostname("inner", 5) != 0) {
        return fail(R_USER_KINDS, "privilege in the user namespace makes the other kinds");
    }
    return 0;
}

/**
 * Check that a set-uid program of root's, run in a user namespace that maps
 * only the caller's own ids, runs with those ids: suidhelper's `plain` exits
 * 0 when every id is 1000, or the NEW of 1000 in the `ids` argument passed on
 * to it. Returns 0, or 1 after printing the failure.
 */
static int setid(void)
{
    const char *ids = probe_arg("ids", NULL);
    char map[32], ids_arg[256];
    pid_t pid;
    int status;

    snprintf(map, sizeof(map), "%u %u 1\n", NS_USER, NS_USER);
    if (setgid(NS_USER) != 0 || setuid(NS_USER) != 0 || prctl(PR_SET_DUMPABLE, 1) != 0 ||
        unshare(CLONE_NEWUSER) != 0 || put("/proc/self/uid_map", map) != 0 ||
        put("/proc/self/setgroups", "deny") != 0 || put("/proc/self/gid_map", map) != 0) {
        return fail(R_SETID_MAP, "map the caller's own ids");
    }
    if (ids != NULL) {
        snprintf(ids_arg, sizeof(ids_arg), "ids=%s", ids);
    }
    fflush(stdout);
    pid = fork();
    if (pid == 0) {
        execl("/suidhelper.suid", "suidhelper.suid", "plain", ids != NULL ? ids_arg : (char *)NULL,
              (char *)NULL);
        _exit(127);
    }
    if (waitpid(pid, &status, 0) != pid || !WIFEXITED(status) || WEXITSTATUS(status) != 0) {
        return fail(R_SETID_EXEC,
                    "an exec in a user namespace ignores a set-id bit of an owner it does not map");
    }
    return 0;
}

/* The lines of a file's fdinfo before its mount id's value, Linux's order. */
#define FDINFO_HEAD "pos:\t5\nflags:\t02000000\nmnt_id:\t"

/** The helper of `empty_root`: chroot to its own fdinfo directory and end. */
static int fdinfo_helper(void)
{
    return chroot("/proc/self/fdinfo/") == 0 && chdir("/") == 0 ? 0 : 1;
}

/**
 * Check a file of /proc/self/fdinfo, its offset, flags, mount id and inode
 * as Linux gives them, and then, as uid 1000 in a user
 * namespace of its own, Chromium's chroot into that directory by a CLONE_FS
 * helper: the root left is the fdinfo directory of a process that has
 * ended, which refuses a lookup in it and its opening with ESRCH, as
 * Linux's proc_fdinfo_permission does, and takes no name, and a descriptor
 * of /proc opened before still reaches self/fd. Returns 0, or 1 after
 * printing the failure.
 */
static int empty_root(void)
{
    char buf[128], path[64];
    int fd, proc, status;
    long pid;

    fd = open("/proc/self/status", O_RDONLY | O_CLOEXEC);
    if (fd < 0 || lseek(fd, 5, SEEK_SET) != 5) {
        return fail(R_FDINFO_OPEN, "open and seek a file for its fdinfo");
    }
    snprintf(path, sizeof(path), "/proc/self/fdinfo/%d", fd);
    if (get(path, buf, sizeof(buf)) <= 0 ||
        strncmp(buf, FDINFO_HEAD, sizeof(FDINFO_HEAD) - 1) != 0 || !strstr(buf, "\nino:\t")) {
        return fail(R_FDINFO,
                    "a file of fdinfo has the offset, the flags, the mount and the inode");
    }
    if (setgid(NS_USER) != 0 || setuid(NS_USER) != 0 || prctl(PR_SET_DUMPABLE, 1) != 0 ||
        unshare(CLONE_NEWUSER) != 0 || (proc = open("/proc", O_RDONLY | O_DIRECTORY)) < 0) {
        return fail(R_EMPTY_USER, "become uid 1000 in a user namespace");
    }
    fflush(stdout);
    pid = syscall(SYS_clone, CLONE_FS | SIGCHLD, 0, 0, 0, 0);
    if (pid == 0) {
        _exit(fdinfo_helper());
    }
    if (pid < 0 || waitpid((pid_t)pid, &status, 0) != pid || !WIFEXITED(status) ||
        WEXITSTATUS(status) != 0) {
        return fail(R_EMPTY_HELPER,
                    "a helper sharing the directories chroots to /proc/self/fdinfo");
    }
    if (access("/proc", F_OK) == 0 || errno != ESRCH) {
        return fail(R_EMPTY_NOTHING, "a lookup in the root is ESRCH");
    }
    if (open("/", O_RDONLY | O_DIRECTORY) >= 0 || errno != ESRCH || mkdir("/x", 0755) == 0) {
        return fail(R_EMPTY_TAKES, "the root does not open and takes no name");
    }
    fd = openat(proc, "self/fd/", O_RDONLY | O_DIRECTORY);
    if (fd < 0 || close(fd) != 0) {
        return fail(R_EMPTY_PROC, "a descriptor of /proc opened before the chroot reaches self/fd");
    }
    return 0;
}

/**
 * Check that on x86-64, where unshare takes a long, a flag in its upper half
 * is EINVAL and unshares nothing. Returns 0, or 1 after printing the failure.
 */
static int wide_flags(void)
{
#if defined __x86_64__
    if (syscall(SYS_unshare, 1UL << 32) != -1 || errno != EINVAL) {
        return fail(R_WIDE, "unshare of a flag past 32 bits is not EINVAL");
    }
#endif
    return 0;
}

/**
 * Make the System V segment the IPC check looks for, run the checks, remove
 * the segment, and report the rows. Returns 0 when every check passes, and 1
 * otherwise.
 */
int main(int argc, char **argv)
{
    int id = shmget(SHM_KEY, 4096, IPC_CREAT | 0600);
    int rc;

    probe_args(argc, argv);
    report_begin(report_case(environ, "guest.default.nsmusl"));
#if defined __x86_64__
    report_branch("arch", "x86-64");
#else
    report_branch("arch", "i686");
#endif
    if (id < 0) {
        fail(R_SEGMENT, "shmget");
        report_rows();
        return 1;
    }
    rc = wide_flags() || in_child(uts, R_UTS_LINK) || in_child(pid_ns, R_PID_KEEP) ||
         in_child(net, R_NET) || in_child(ipc, R_IPC) || mnt() || in_child(many_mnt, R_MNT_MANY) ||
         in_child(long_map, R_MAP_EXTRA) || in_child(sandbox, R_SANDBOX_USER) ||
         in_child(user, R_USER_BECOME) || in_child(setid, R_SETID_MAP) ||
         in_child(empty_root, R_FDINFO_OPEN);
    shmctl(id, IPC_RMID, NULL);
    report_rows();
    if (rc) {
        return 1;
    }
    printf("NSMUSL OK\n");
    return 0;
}
