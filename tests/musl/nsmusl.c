/**
 * nsmusl: check namespaces through musl's unshare(), setns() and the clone
 * call.
 *
 * A UTS namespace has its own host name, its /proc/self/ns link names the first
 * one before, and setns returns to it. A PID namespace numbers its first
 * process 1, whose parent it sees as 0, and that process's child 2; the first
 * process's status file shows it one number, and shows its parent, outside
 * the namespace, both; get_robust_list of pid 1 gives that process's own
 * list; and a name relative to its descriptor of /proc/self/fd reaches its
 * own descriptor. The caller of unshare keeps its own pid. In a
 * new network namespace a connection to the loopback address fails with
 * ENETUNREACH. A mount made in a new mount namespace is not seen outside it,
 * and a new IPC namespace has none of the first's System V objects.
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

/* The key of the System V segment the first IPC namespace has ("NSMU"). */
#define SHM_KEY 0x4e534d55

/** Report one failed check and return 1, a child's failure status. */
static int fail(const char *what)
{
    printf("NSMUSL FAIL (%s: errno %d)\n", what, errno);
    fflush(stdout);
    return 1;
}

/** Run `fn` in a child; 0 when it exits with status 0. */
static int in_child(int (*fn)(void))
{
    pid_t pid;
    int status;

    fflush(stdout);
    pid = fork();
    if (pid == 0) {
        _exit(fn());
    }
    return waitpid(pid, &status, 0) == pid && WIFEXITED(status) && WEXITSTATUS(status) == 0 ? 0 : 1;
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
        return fail("open and read the uts link");
    }
    link[n] = '\0';
    if (strcmp(link, "uts:[4026531838]") != 0) {
        return fail("the first uts namespace's link");
    }
    uname(&u);
    strcpy(before, u.nodename);
    if (unshare(CLONE_NEWUTS) != 0 || sethostname("nsbox", 5) != 0 || uname(&u) != 0 ||
        strcmp(u.nodename, "nsbox") != 0) {
        return fail("a new uts namespace has its own host name");
    }
    if (setns(fd, CLONE_NEWUTS) != 0 || uname(&u) != 0 || strcmp(u.nodename, before) != 0) {
        return fail("setns returns to the first uts namespace");
    }
    return 0;
}

/** Whether the NSpid line of /proc/`who`/status has the numbers `want`. */
static int nspid_is(const char *who, const char *want)
{
    char path[64], buf[1024];
    const char *line, *nl;

    snprintf(path, sizeof(path), "/proc/%s/status", who);
    get(path, buf, sizeof(buf));
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
        return fail("a descriptor of its /proc/self/fd reaches its own descriptors");
    }
    snprintf(name, sizeof(name), "%d", p[0]);
    if (fstatat(dir, name, &entry, 0) != 0 || fstat(p[0], &own) != 0 ||
        entry.st_ino != own.st_ino || entry.st_dev != own.st_dev) {
        return fail("a descriptor of its /proc/self/fd reaches its own descriptors");
    }
    close(p[0]);
    close(p[1]);
    close(dir);
    return 0;
}

/**
 * Check, as the first process of a new PID namespace, that it is pid 1 with
 * parent 0, that its child is 2, that its NSpid line has the one number, that
 * get_robust_list of pid 1 gives the head it set, and then `own_fd_dir`.
 * Returns 0, or 1 after printing the failure.
 */
static int pid_first(void)
{
    static long head[3];
    long *got = NULL;
    size_t len = 0;
    pid_t pid;
    int status;

    if (getpid() != 1 || getppid() != 0) {
        return fail("the first process of a pid namespace is 1, its parent 0");
    }
    pid = fork();
    if (pid == 0) {
        _exit(getpid() == 2 && getppid() == 1 ? 0 : 1);
    }
    if (pid != 2 || waitpid(pid, &status, 0) != 2 || !WIFEXITED(status) ||
        WEXITSTATUS(status) != 0) {
        return fail("its child is 2");
    }
    if (!nspid_is("self", "1")) {
        return fail("its status file shows it the one number");
    }
    head[0] = (long)head;
    if (syscall(SYS_set_robust_list, head, sizeof(head)) != 0 ||
        syscall(SYS_get_robust_list, 1, &got, &len) != 0 || got != head || len != sizeof(head)) {
        return fail("get_robust_list of pid 1 gives the first process's own list");
    }
    return own_fd_dir();
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
        return fail("unshare of a pid namespace keeps the caller's pid");
    }
    fflush(stdout);
    first = fork();
    if (first == 0) {
        rc = pid_first();
        read(go[0], &c, 1);
        _exit(rc);
    }
    snprintf(who, sizeof(who), "%d", (int)first);
    snprintf(want, sizeof(want), "%d\t1", (int)first);
    if (!nspid_is(who, want)) {
        rc = fail("its parent's status file shows both numbers");
    }
    write(go[1], &c, 1);
    if (waitpid(first, &status, 0) != first || !WIFEXITED(status) || WEXITSTATUS(status) != 0) {
        rc = 1;
    }
    return rc;
}

/** Check that a new network namespace reaches nothing; 0, or 1. */
static int net(void)
{
    if (unshare(CLONE_NEWNET) != 0 || !unreachable()) {
        return fail("a new network namespace reaches nothing");
    }
    return 0;
}

/** Check that a new IPC namespace lacks the segment at `SHM_KEY`; 0, or 1. */
static int ipc(void)
{
    if (unshare(CLONE_NEWIPC) != 0 || shmget(SHM_KEY, 0, 0) != -1 || errno != ENOENT) {
        return fail("a new ipc namespace has none of the first's objects");
    }
    return 0;
}

/**
 * Check that a tmpfs mounted in a child's new mount namespace, and a file
 * made in it, are seen by the child and not by this process. Two pipes keep
 * the child in place while this process looks. Returns 0, or 1 after printing
 * the failure.
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
        return fail("pipe");
    }
    fflush(stdout);
    pid = fork();
    if (pid == 0) {
        if (unshare(CLONE_NEWNS) != 0 || mount("nsmusl", "/tmp/nsmnt", "tmpfs", 0, NULL) != 0 ||
            (fd = open("/tmp/nsmnt/inner", O_CREAT | O_WRONLY, 0644)) < 0) {
            _exit(fail("mount tmpfs in a new mount namespace"));
        }
        close(fd);
        write(up[1], &c, 1);
        read(down[0], &c, 1);
        _exit(stat("/tmp/nsmnt/inner", &st) == 0 ? 0 : 1);
    }
    if (read(up[0], &c, 1) != 1 || stat("/tmp/nsmnt/inner", &st) == 0) {
        rc = fail("the mount is not seen outside its namespace");
    }
    write(down[1], &c, 1);
    if (waitpid(pid, &status, 0) != pid || !WIFEXITED(status) || WEXITSTATUS(status) != 0) {
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

    if (setgid(1000) != 0 || setuid(1000) != 0 || prctl(PR_SET_DUMPABLE, 1) != 0) {
        return fail("become uid 1000");
    }
    fflush(stdout);
    pid = syscall(SYS_clone, CLONE_NEWUSER | CLONE_NEWPID | CLONE_NEWNET | SIGCHLD, 0, 0, 0, 0);
    if (pid == 0) {
        _exit(getpid() == 1 && getuid() == 65534 && unreachable() ? 0 : 1);
    }
    if (pid < 0 || waitpid((pid_t)pid, &status, 0) != pid || !WIFEXITED(status) ||
        WEXITSTATUS(status) != 0) {
        return fail("one clone makes a user, a pid and a network namespace");
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
    char buf[128];

    if (setgid(1000) != 0 || setuid(1000) != 0 || prctl(PR_SET_DUMPABLE, 1) != 0) {
        return fail("become uid 1000");
    }
    if (unshare(CLONE_NEWUSER) != 0 || getuid() != 65534 || getgid() != 65534) {
        return fail("ids outside the maps read as 65534");
    }
    if (put("/proc/self/uid_map", "0 1000 1\n") != 0 || put("/proc/self/setgroups", "deny") != 0 ||
        put("/proc/self/gid_map", "0 1000 1\n") != 0) {
        return fail("write the maps");
    }
    if (getuid() != 0 || geteuid() != 0 || getgid() != 0) {
        return fail("the mapped ids");
    }
    if (get("/proc/self/uid_map", buf, sizeof(buf)) <= 0 ||
        strcmp(buf, "         0       1000          1\n") != 0) {
        return fail("the map reads back");
    }
    if (put("/proc/self/uid_map", "0 1000 1\n") == 0 || errno != EPERM) {
        return fail("a map is written once");
    }
    if (chroot("/") != 0) {
        return fail("the user namespace's root may chroot");
    }
    if (unshare(CLONE_NEWUTS | CLONE_NEWIPC | CLONE_NEWNET) != 0 || sethostname("inner", 5) != 0) {
        return fail("privilege in the user namespace makes the other kinds");
    }
    return 0;
}

/**
 * Check that a set-uid program of root's, run in a user namespace that maps
 * only the caller's own ids, runs with those ids: suidhelper's `plain` exits
 * 0 when every id is 1000. Returns 0, or 1 after printing the failure.
 */
static int setid(void)
{
    pid_t pid;
    int status;

    if (setgid(1000) != 0 || setuid(1000) != 0 || prctl(PR_SET_DUMPABLE, 1) != 0 ||
        unshare(CLONE_NEWUSER) != 0 || put("/proc/self/uid_map", "1000 1000 1\n") != 0 ||
        put("/proc/self/setgroups", "deny") != 0 ||
        put("/proc/self/gid_map", "1000 1000 1\n") != 0) {
        return fail("map the caller's own ids");
    }
    fflush(stdout);
    pid = fork();
    if (pid == 0) {
        execl("/suidhelper.suid", "suidhelper.suid", "plain", (char *)NULL);
        _exit(127);
    }
    if (waitpid(pid, &status, 0) != pid || !WIFEXITED(status) || WEXITSTATUS(status) != 0) {
        return fail("an exec in a user namespace ignores a set-id bit of an owner it does not map");
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
    lseek(fd, 5, SEEK_SET);
    snprintf(path, sizeof(path), "/proc/self/fdinfo/%d", fd);
    if (fd < 0 || get(path, buf, sizeof(buf)) <= 0 || strcmp(buf, "pos:\t5\nflags:\t02000000\n")) {
        return fail("a file of fdinfo has the offset and the flags");
    }
    if (setgid(1000) != 0 || setuid(1000) != 0 || prctl(PR_SET_DUMPABLE, 1) != 0 ||
        unshare(CLONE_NEWUSER) != 0 || (proc = open("/proc", O_RDONLY | O_DIRECTORY)) < 0) {
        return fail("become uid 1000 in a user namespace");
    }
    fflush(stdout);
    pid = syscall(SYS_clone, CLONE_FS | SIGCHLD, 0, 0, 0, 0);
    if (pid == 0) {
        _exit(fdinfo_helper());
    }
    if (pid < 0 || waitpid((pid_t)pid, &status, 0) != pid || !WIFEXITED(status) ||
        WEXITSTATUS(status) != 0) {
        return fail("a helper sharing the directories chroots to /proc/self/fdinfo");
    }
    if (access("/proc", F_OK) == 0 || !(d = opendir("/"))) {
        return fail("the root holds nothing");
    }
    while ((de = readdir(d)) != NULL) {
        entries += strcmp(de->d_name, ".") != 0 && strcmp(de->d_name, "..") != 0;
    }
    closedir(d);
    if (entries != 0 || mkdir("/x", 0755) == 0) {
        return fail("the root holds nothing and takes nothing");
    }
    fd = openat(proc, "self/fd/", O_RDONLY | O_DIRECTORY);
    if (fd < 0 || close(fd) != 0) {
        return fail("a descriptor of /proc opened before the chroot reaches self/fd");
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
        return fail("unshare of a flag past 32 bits is not EINVAL");
    }
#endif
    return 0;
}

/**
 * Make the System V segment the IPC check looks for, run the checks, and
 * remove the segment. Returns 0 when every check passes, and 1 otherwise.
 */
int main(void)
{
    int id = shmget(SHM_KEY, 4096, IPC_CREAT | 0600);
    int rc;

    if (id < 0) {
        return fail("shmget");
    }
    rc = wide_flags() || in_child(uts) || in_child(pid_ns) || in_child(net) || in_child(ipc) ||
         mnt() || in_child(sandbox) || in_child(user) || in_child(setid) || in_child(empty_root);
    shmctl(id, IPC_RMID, NULL);
    if (rc) {
        return 1;
    }
    printf("NSMUSL OK\n");
    return 0;
}
