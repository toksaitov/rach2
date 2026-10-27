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
 * its entry in a listing of /proc has the inode of /proc/1; and a name
 * relative to its descriptor of /proc/self/fd reaches its own descriptor. The
 * caller of unshare keeps its own pid. When it ends, its namespace's init
 * adopts its children, including the init of a namespace below it and that
 * init's sibling. In a new network namespace a
 * connection to the loopback address fails with ENETUNREACH. A mount made in
 * a new mount namespace is not seen outside it, and a new IPC namespace has
 * none of the first's System V objects.
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
    R_ORPHAN_PARENT,
    R_ORPHAN_ADOPT,
    R_PID_BOTH,
    R_NET,
    R_IPC,
    R_MNT,
    R_MNT_OUTSIDE,
    R_MNT_INSIDE,
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
                                               "orphans-parent-exits",
                                               "orphans-adopted",
                                               "pid-status-both-numbers",
                                               "net-unreachable",
                                               "ipc-no-objects",
                                               "mount-in-namespace",
                                               "mount-unseen-outside",
                                               "mount-seen-inside",
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
    int status;

    fflush(stdout);
    pid = fork();
    if (pid == 0) {
        _exit(fn() ? failed_status() : 0);
    }
    return child_failed(waitpid(pid, &status, 0) == pid, status, row);
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
 * Check, as the first process of a new PID namespace, that it is pid 1 with
 * parent 0, that its child is 2, that its NSpid line has the one number, that
 * get_robust_list of pid 1 gives the head it set, that get_robust_list and
 * kill of pid 99, which the namespace has not given, fail with ESRCH, and
 * then `proc_own_number` and `own_fd_dir`. Returns 0, or 1 after printing
 * the failure.
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
    return proc_own_number() || own_fd_dir() || pid_orphans();
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
    int go[2], status, rc = 0;

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
    if (child_failed(waitpid(first, &status, 0) == first, status, R_PID_ONE)) {
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
    int fd, status, rc = 0;
    char c = 'x';
    struct stat st;

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
        }
        write(down[1], &c, 1);
    }
    close(up[0]);
    close(down[1]);
    if (child_failed(waitpid(pid, &status, 0) == pid, status, R_MNT)) {
        rc = 1;
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

/** The helper of `empty_root`: chroot to its own fdinfo directory and end. */
static int fdinfo_helper(void)
{
    return chroot("/proc/self/fdinfo/") == 0 && chdir("/") == 0 ? 0 : 1;
}

/**
 * Check a file of /proc/self/fdinfo, and then, as uid 1000 in a user
 * namespace of its own, Chromium's chroot into that directory by a CLONE_FS
 * helper: the root left has no entries and takes none, and a descriptor of
 * /proc opened before still reaches self/fd. Returns 0, or 1 after printing
 * the failure.
 */
static int empty_root(void)
{
    char buf[128], path[64];
    DIR *d;
    struct dirent *de;
    int fd, proc, entries = 0, status;
    long pid;

    fd = open("/proc/self/status", O_RDONLY | O_CLOEXEC);
    if (fd < 0 || lseek(fd, 5, SEEK_SET) != 5) {
        return fail(R_FDINFO_OPEN, "open and seek a file for its fdinfo");
    }
    snprintf(path, sizeof(path), "/proc/self/fdinfo/%d", fd);
    if (get(path, buf, sizeof(buf)) <= 0 || strcmp(buf, "pos:\t5\nflags:\t02000000\n")) {
        return fail(R_FDINFO, "a file of fdinfo has the offset and the flags");
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
    if (access("/proc", F_OK) == 0 || !(d = opendir("/"))) {
        return fail(R_EMPTY_NOTHING, "the root holds nothing");
    }
    while ((de = readdir(d)) != NULL) {
        entries += strcmp(de->d_name, ".") != 0 && strcmp(de->d_name, "..") != 0;
    }
    closedir(d);
    if (entries != 0 || mkdir("/x", 0755) == 0) {
        return fail(R_EMPTY_TAKES, "the root holds nothing and takes nothing");
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
         in_child(net, R_NET) || in_child(ipc, R_IPC) || mnt() ||
         in_child(sandbox, R_SANDBOX_USER) || in_child(user, R_USER_BECOME) ||
         in_child(setid, R_SETID_MAP) || in_child(empty_root, R_FDINFO_OPEN);
    shmctl(id, IPC_RMID, NULL);
    report_rows();
    if (rc) {
        return 1;
    }
    printf("NSMUSL OK\n");
    return 0;
}
