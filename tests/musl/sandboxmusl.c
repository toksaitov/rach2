/**
 * sandboxmusl: check the calls a sandbox such as bubblewrap builds its root
 * with, and that what each of them denies stays denied.
 *
 * A child in a mount namespace of its own builds a root as bubblewrap does:
 * a tmpfs at /tmp/sbx, a bind of its newroot over itself, and pivot_root to
 * the tmpfs with the old root at oldroot, where the probe's own file then is.
 * It mounts proc at /newroot/proc, which shows its own status, and binds
 * /newroot/proc/sys read-only over itself, as bubblewrap does as root when
 * that directory can be written, and then binds /oldroot/proc/sys over it
 * read-only, as `--ro-bind /proc/sys /proc/sys` does: through each, a file
 * of /proc/sys reads and an open of it for writing fails with EROFS, as
 * access(2) of the directory for writing does. umount2 of
 * /oldroot with MNT_DETACH takes the old root away: the probe's file is gone
 * from under it and no line of mountinfo names it. Then, through a
 * descriptor of the tmpfs root opened first, it pivots to newroot with "."
 * and ".", enters the old root by fchdir, where getcwd fails with ENOENT, and
 * lets go of it by umount2 of "." with MNT_DETACH. In the new root /proc
 * works and /proc/sys stays read-only, /newroot and /oldroot are gone, a
 * file made at /tmp/sbx.out stays inside, and the probe's own directory
 * cannot be written, since it is not there. Outside, /tmp/sbx.out does not
 * exist and no mount of the namespace shows in mountinfo, and /proc/sys
 * can be written. User 1000, in a user and a mount namespace of its own
 * that map its own ids, as bubblewrap makes them, binds /proc/sys
 * read-only over itself, and it reads and refuses an open for writing with
 * EROFS.
 *
 * In the probe's own namespace a tmpfs at /tmp/sbxd with a file open for
 * writing is detached at once by MNT_DETACH: its file is gone from the path,
 * for this process, for a child and for a new mount namespace, mountinfo has
 * no line for it, and the descriptor still writes. Once the descriptor is
 * closed the mount is gone and its directory can be removed. MNT_FORCE is
 * EINVAL, and so is MNT_DETACH of a directory that is no mount.
 *
 * The bounding set has CAP_SYS_ADMIN, and capability 41 is EINVAL. User 1000
 * cannot drop a capability from it. Root drops CAP_SYS_ADMIN: the bounding
 * set lacks it, in prctl, in /proc/self/status and in a forked child, while
 * the sets held keep it; an exec then gives every capability but that one,
 * and the image cannot mount, unshare a mount namespace, pivot_root or take
 * CAP_SYS_ADMIN back by capset. capset keeps one capability: with
 * CAP_SYS_ADMIN alone a thread still unshares a mount namespace, cannot
 * widen its set and, after it gives that one up too, cannot mount; with
 * CAP_CHOWN alone it cannot mount, and an exec gives every capability back.
 * The ambient set starts empty: PR_CAP_AMBIENT_IS_SET answers 0,
 * PR_CAP_AMBIENT_RAISE of CAP_CHOWN, which is not inheritable, fails with
 * EPERM and PR_CAP_AMBIENT_CLEAR_ALL succeeds.
 *
 * A child in a mount namespace of its own masks a file as Claude Code's
 * bubblewrap does: it binds /tmp/sbxb/ws, a directory of a tmpfs, over
 * itself, makes .bashrc in it, binds /dev/null over that and opens the
 * bound /dev/null with O_PATH, as bubblewrap does to find the mount it made.
 * Once the child is gone, and its namespace with it, the directory, its
 * file and the tmpfs can be removed. When a step of this row fails, an
 * atexit handler unmounts the tmpfs and removes the directory instead.
 *
 * The program runs itself in the modes `bset` and `regain`, named by its
 * first argument, as the image an exec row runs. With none it runs the
 * rows, prints "SANDBOXMUSL OK" and exits 0 on success, or prints
 * "SANDBOXMUSL FAIL (detail)" and exits 1 at the first failure. User 1000
 * is the NEW of 1000 when a native run gives `ids=OLD:NEW,...`
 * (tests/native/Readme.md). etude's report (probe_report.h), for the case
 * ETUDE_CASE names, has an assertion for each row of `rows` in order; the
 * first failure fails its assertion and leaves the rest not run, and an
 * exec row's image writes none.
 */

#define _GNU_SOURCE
#include <errno.h>
#include <fcntl.h>
#include <sched.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/mount.h>
#include <sys/prctl.h>
#include <sys/stat.h>
#include <sys/syscall.h>
#include <sys/wait.h>
#include <unistd.h>

#include "../probe_args.h"
#include "../probe_report.h"

/* The new root's tmpfs, the file the sandbox makes inside it, and the tmpfs
 * the detach rows mount. */
#define BASE     "/tmp/sbx"
#define OUTFILE  "/tmp/sbx.out"
#define DETACHED "/tmp/sbxd"

/* The tmpfs of the masking row, the directory it binds and the file it
 * masks. */
#define MASKED    "/tmp/sbxb"
#define MASKED_WS MASKED "/ws"
#define MASKED_RC MASKED_WS "/.bashrc"

/* _LINUX_CAPABILITY_VERSION_3, whose data is two cap_data, and the
 * capabilities the rows name. */
#define VERSION_3     0x20080522u
#define HIGH_WORD     0x1FFu
#define CAP_CHOWN     0
#define CAP_SETPCAP   8
#define CAP_SYS_ADMIN 21

/* One 32-bit word of each set, as Linux's __user_cap_data_struct. */
struct cap_data {
    unsigned effective;
    unsigned permitted;
    unsigned inheritable;
};

/* The assertions, in the order the rows check them. */
static const char *const rows[] = {
    "pivot-setup",    "pivot-put-old",   "proc-anywhere",  "proc-sys-cover",    "proc-sys-rebind",
    "detach-put-old", "pivot-stacked",   "pivot-new-root", "write-outside",     "namespace-unseen",
    "proc-sys-user",  "detach-refusals", "detach-busy",    "detach-every-view", "detach-reaped",
    "bset-read",      "bset-drop-eperm", "bset-drop",      "bset-fork",         "bset-exec",
    "capset-single",  "capset-chown",    "ambient",        "mask-removed",
};

#define NROWS ((int)(sizeof(rows) / sizeof(rows[0])))

/* The steps of `sandbox`, rows 0 to SANDBOX_STEPS - 1, and the first row of
 * the bounding set's. */
#define SANDBOX_STEPS 9
#define CAP_ROW       15

/* The probe's own file, read from /proc/self/exe before any pivot. */
static char exe[256];

/** Print the failure of `what`, end the report at it, and return 1. */
static int fail(const char *what)
{
    printf("SANDBOXMUSL FAIL (%s)\n", what);
    fflush(stdout);
    report_stop(what);
    return 1;
}

/** The status of the child `pid` once it ends: its exit code, or 128 and
 * the signal for one a signal ended, or -1 when waitpid fails. */
static int child_status(pid_t pid)
{
    int st;

    if (pid < 0 || waitpid(pid, &st, 0) != pid) {
        return -1;
    }
    return WIFEXITED(st) ? WEXITSTATUS(st) : 128 + WTERMSIG(st);
}

/** Write `text` to the file `path`, as to /proc/self/uid_map; 0 or -1. */
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

/** Whether `path` names nothing, which stat answers with ENOENT. */
static int absent(const char *path)
{
    struct stat st;

    return stat(path, &st) != 0 && errno == ENOENT;
}

/** Whether a line of the file `path` has `text` in it. */
static int has_line(const char *path, const char *text)
{
    char line[512];
    FILE *f = fopen(path, "r");
    int found = 0;

    if (!f) {
        return 0;
    }
    while (!found && fgets(line, sizeof(line), f)) {
        found = strstr(line, text) != NULL;
    }
    fclose(f);
    return found;
}

/** Whether `path`, a status file, has a "Pid:" line of this process. */
static int status_is_mine(const char *path)
{
    char want[32];

    snprintf(want, sizeof(want), "Pid:\t%d\n", (int)getpid());
    return has_line(path, want);
}

/** The value of the line `key` of /proc/self/status, a set in hex. */
static unsigned long long status_set(const char *key)
{
    char line[128];
    unsigned long long v = ~0ULL;
    size_t kl = strlen(key);
    FILE *f = fopen("/proc/self/status", "r");

    if (!f) {
        return v;
    }
    while (fgets(line, sizeof(line), f)) {
        if (strncmp(line, key, kl) == 0 && line[kl] == ':') {
            v = strtoull(line + kl + 1, NULL, 16);
            break;
        }
    }
    fclose(f);
    return v;
}

/** capget of the caller into `d`, two words; 0 or -1. */
static int get_caps(struct cap_data *d)
{
    unsigned h[2] = {VERSION_3, 0};

    return (int)syscall(SYS_capget, h, d);
}

/** capset of the caller to `w0` and `w1`, effective and permitted alike, and
 * none inheritable; 0 or -1. */
static int set_caps(unsigned w0, unsigned w1)
{
    unsigned h[2] = {VERSION_3, 0};
    struct cap_data d[2];

    memset(d, 0, sizeof(d));
    d[0].effective = d[0].permitted = w0;
    d[1].effective = d[1].permitted = w1;
    return (int)syscall(SYS_capset, h, d);
}

/** Whether the caller holds `w0` and `w1`, effective and permitted alike. */
static int holds_exactly(unsigned w0, unsigned w1)
{
    struct cap_data d[2];

    return get_caps(d) == 0 && d[0].effective == w0 && d[0].permitted == w0 &&
           d[1].effective == w1 && d[1].permitted == w1 && d[0].inheritable == 0 &&
           d[1].inheritable == 0;
}

/** Whether `rc`, a call's result, is a failure with `err`. */
static int refused(long rc, int err)
{
    return rc == -1 && errno == err;
}

/** Bind `src` over `dst` with the mounts below it, and make the bind
 * read-only, nosuid and nodev, as bubblewrap's `--ro-bind` does; 0 or -1. */
static int ro_bind(const char *src, const char *dst)
{
    return mount(src, dst, NULL, MS_BIND | MS_REC, NULL) != 0 ||
                   mount("none", dst, NULL, MS_BIND | MS_REMOUNT | MS_RDONLY | MS_NOSUID | MS_NODEV,
                         NULL) != 0
               ? -1
               : 0;
}

/** Whether the /proc/sys at `dir` is read-only: its kernel/overflowuid reads
 * 65534, an open of it for writing fails with EROFS, and access(2) of `dir`
 * for writing with `err`, EROFS for root and EACCES for a user, whose
 * permission is judged first, as on Linux. */
static int sys_read_only(const char *dir, int err)
{
    char path[128], buf[16];
    ssize_t n;
    int fd;

    snprintf(path, sizeof(path), "%s/kernel/overflowuid", dir);
    fd = open(path, O_RDONLY);
    n = fd < 0 ? -1 : read(fd, buf, sizeof(buf) - 1);
    if (fd >= 0) {
        close(fd);
    }
    if (n != 6 || memcmp(buf, "65534\n", 6) != 0) {
        return 0;
    }
    return refused(open(path, O_WRONLY), EROFS) && refused(access(dir, W_OK), err);
}

/** Whether a mount of a tmpfs at /tmp fails with EPERM, as for a thread
 * without privilege. */
static int mount_refused(void)
{
    return refused(mount("none", "/tmp", "tmpfs", 0, NULL), EPERM);
}

/**
 * Build the sandbox as bubblewrap does and check each step, in a child in a
 * mount namespace of its own: the exit code is the number of the step that
 * failed, 1 to SANDBOX_STEPS, or 0.
 */
static int sandbox(void)
{
    char path[320];
    int oldroot, fd;

    if (unshare(CLONE_NEWNS) != 0 ||
        mount("tmpfs", BASE, "tmpfs", MS_NODEV | MS_NOSUID, NULL) != 0 || chdir(BASE) != 0 ||
        mkdir("newroot", 0755) != 0 ||
        mount("newroot", "newroot", NULL, MS_BIND | MS_REC, NULL) != 0 ||
        mkdir("oldroot", 0755) != 0) {
        return 1;
    }
    snprintf(path, sizeof(path), "/oldroot%s", exe);
    if (syscall(SYS_pivot_root, BASE, "oldroot") != 0 || chdir("/") != 0 ||
        access(path, R_OK) != 0 || !absent(exe)) {
        return 2;
    }
    if (mkdir("/newroot/proc", 0555) != 0 ||
        mount("proc", "/newroot/proc", "proc", MS_NOSUID | MS_NODEV | MS_NOEXEC, NULL) != 0 ||
        !status_is_mine("/newroot/proc/self/status")) {
        return 3;
    }
    if (access("/newroot/proc/sys", W_OK) != 0 ||
        ro_bind("/newroot/proc/sys", "/newroot/proc/sys") != 0 ||
        !sys_read_only("/newroot/proc/sys", EROFS) ||
        !has_line("/newroot/proc/self/mountinfo", " /newroot/proc/sys ")) {
        return 4;
    }
    if (ro_bind("/oldroot/proc/sys", "/newroot/proc/sys") != 0 ||
        !sys_read_only("/newroot/proc/sys", EROFS)) {
        return 5;
    }
    if (umount2("/oldroot", MNT_DETACH) != 0 || !absent(path) ||
        has_line("/proc/self/mountinfo", "/oldroot")) {
        return 6;
    }
    oldroot = open("/", O_RDONLY | O_DIRECTORY);
    if (oldroot < 0 || chdir("/newroot") != 0 || syscall(SYS_pivot_root, ".", ".") != 0 ||
        fchdir(oldroot) != 0 || getcwd(path, sizeof(path)) != NULL || errno != ENOENT ||
        umount2(".", MNT_DETACH) != 0 || chdir("/") != 0 || close(oldroot) != 0) {
        return 7;
    }
    if (!status_is_mine("/proc/self/status") || !sys_read_only("/proc/sys", EROFS) ||
        !absent("/newroot") || !absent("/oldroot") || getcwd(path, sizeof(path)) == NULL ||
        strcmp(path, "/") != 0) {
        return 8;
    }
    snprintf(path, sizeof(path), "%s.out", exe);
    if (!refused(open(path, O_WRONLY | O_CREAT, 0644), ENOENT) ||
        !refused(open(OUTFILE, O_WRONLY | O_CREAT, 0644), ENOENT) || mkdir("/tmp", 01777) != 0) {
        return 9;
    }
    fd = open("/../../tmp/sbx.out", O_WRONLY | O_CREAT | O_EXCL, 0644);
    if (fd < 0 || write(fd, "in\n", 3) != 3 || close(fd) != 0 || access(OUTFILE, R_OK) != 0) {
        return 9;
    }
    return 0;
}

/** As user 1000, in a user and a mount namespace of its own that map its own
 * ids: bind /proc/sys read-only over itself, as bubblewrap's `--ro-bind
 * /proc/sys /proc/sys` does for a user. The exit code is 0 or 1. */
static int user_sys_bind(void)
{
    char map[32];

    snprintf(map, sizeof(map), "%ld %ld 1\n", probe_id(1000), probe_id(1000));
    if (setgid((gid_t)probe_id(1000)) != 0 || setuid((uid_t)probe_id(1000)) != 0 ||
        prctl(PR_SET_DUMPABLE, 1) != 0 || unshare(CLONE_NEWUSER | CLONE_NEWNS) != 0 ||
        put("/proc/self/uid_map", map) != 0 || put("/proc/self/setgroups", "deny") != 0 ||
        put("/proc/self/gid_map", map) != 0) {
        return 1;
    }
    return ro_bind("/proc/sys", "/proc/sys") == 0 && sys_read_only("/proc/sys", EACCES) ? 0 : 1;
}

/** Run the sandbox's steps, rows 0 to SANDBOX_STEPS, and the user's bind of
 * /proc/sys. Returns 0, or 1 after the failure is reported. */
static int sandbox_rows(void)
{
    pid_t pid;
    int st, i;

    if (mkdir(BASE, 0755) != 0 && errno != EEXIST) {
        return fail("mkdir " BASE);
    }
    pid = fork();
    if (pid == 0) {
        _exit(sandbox());
    }
    st = child_status(pid);
    if (st < 0 || st > SANDBOX_STEPS) {
        return fail("the sandbox's child ended");
    }
    for (i = 1; i <= SANDBOX_STEPS; i++) {
        if (st == i) {
            return fail(rows[i - 1]);
        }
        report_held();
    }
    if (!absent(OUTFILE) || !absent(BASE "/newroot") || has_line("/proc/self/mountinfo", BASE) ||
        has_line("/proc/self/mountinfo", "/proc/sys") || access("/proc/sys", W_OK) != 0) {
        return fail("the sandbox's mounts or its file seen outside");
    }
    report_held();
    if (rmdir(BASE) != 0) {
        return fail("rmdir " BASE);
    }
    pid = fork();
    if (pid == 0) {
        _exit(user_sys_bind());
    }
    if (child_status(pid) != 0) {
        return fail(rows[SANDBOX_STEPS + 1]);
    }
    report_held();
    return 0;
}

/** In a child: whether DETACHED/f is gone, with a new mount namespace too.
 * The exit code is 0 or 1. */
static int gone_for_child(void)
{
    if (!absent(DETACHED "/f")) {
        return 1;
    }
    if (unshare(CLONE_NEWNS) != 0 || !absent(DETACHED "/f")) {
        return 1;
    }
    return 0;
}

/** Run the rows of MNT_DETACH in the probe's own namespace, 11 to 14.
 * Returns 0, or 1 after the failure is reported. */
static int detach_rows(void)
{
    struct stat st;
    pid_t pid;
    int fd;

    if (mkdir(DETACHED, 0755) != 0 && errno != EEXIST) {
        return fail("mkdir " DETACHED);
    }
    if (!refused(umount2(DETACHED, MNT_DETACH), EINVAL)) {
        return fail("MNT_DETACH of a directory that is no mount was not EINVAL");
    }
    if (mount("tmpfs", DETACHED, "tmpfs", 0, NULL) != 0) {
        return fail("mount " DETACHED);
    }
    if (!refused(umount2(DETACHED, MNT_FORCE), EINVAL)) {
        return fail("MNT_FORCE was not EINVAL");
    }
    report_held();
    fd = open(DETACHED "/f", O_WRONLY | O_CREAT, 0644);
    if (fd < 0 || umount2(DETACHED, MNT_DETACH) != 0 || !absent(DETACHED "/f") ||
        write(fd, "after\n", 6) != 6 || fstat(fd, &st) != 0 || st.st_size != 6) {
        return fail("a busy tmpfs was not detached, or its descriptor stopped working");
    }
    report_held();
    pid = fork();
    if (pid == 0) {
        _exit(gone_for_child());
    }
    if (child_status(pid) != 0 || has_line("/proc/self/mountinfo", DETACHED)) {
        return fail("a detached mount was still seen");
    }
    report_held();
    if (close(fd) != 0 || rmdir(DETACHED) != 0) {
        return fail("the detached mount stayed once its descriptor closed");
    }
    report_held();
    return 0;
}

/** As user 1000: whether dropping CAP_CHOWN from the bounding set fails
 * with EPERM. The exit code is 0 or 1. */
static int drop_as_user(void)
{
    if (setgid((gid_t)probe_id(1000)) != 0 || setuid((uid_t)probe_id(1000)) != 0) {
        return 1;
    }
    return refused(prctl(PR_CAPBSET_DROP, CAP_CHOWN, 0, 0, 0), EPERM) &&
                   prctl(PR_CAPBSET_READ, CAP_CHOWN, 0, 0, 0) == 1
               ? 0
               : 1;
}

/** As root: drop CAP_SYS_ADMIN from the bounding set, check it, check that a
 * forked child has it dropped, and exec the probe in the mode `bset`. The
 * exit code is 1 for the drop, 2 for the fork, 3 for the exec, or the
 * image's. */
static int drop_and_exec(void)
{
    char *argv[3];
    pid_t pid;

    if (prctl(PR_CAPBSET_DROP, CAP_SYS_ADMIN, 0, 0, 0) != 0 ||
        prctl(PR_CAPBSET_READ, CAP_SYS_ADMIN, 0, 0, 0) != 0 ||
        prctl(PR_CAPBSET_READ, CAP_SETPCAP, 0, 0, 0) != 1 ||
        status_set("CapBnd") != (0x1FFFFFFFFFFULL & ~(1ULL << CAP_SYS_ADMIN)) ||
        !holds_exactly(0xFFFFFFFFu, HIGH_WORD)) {
        return 1;
    }
    pid = fork();
    if (pid == 0) {
        _exit(prctl(PR_CAPBSET_READ, CAP_SYS_ADMIN, 0, 0, 0) == 0 ? 0 : 1);
    }
    if (child_status(pid) != 0) {
        return 2;
    }
    argv[0] = exe;
    argv[1] = "bset";
    argv[2] = NULL;
    execv(exe, argv);
    return 3;
}

/** The image of `drop_and_exec`: it holds every capability but
 * CAP_SYS_ADMIN, and what that one guards is refused. The exit code is 0, or
 * 4 to 6. */
static int bset_image(void)
{
    if (!holds_exactly(0xFFFFFFFFu & ~(1u << CAP_SYS_ADMIN), HIGH_WORD) ||
        prctl(PR_CAPBSET_READ, CAP_SYS_ADMIN, 0, 0, 0) != 0) {
        return 4;
    }
    if (!mount_refused() || !refused(unshare(CLONE_NEWNS), EPERM) ||
        !refused(syscall(SYS_pivot_root, "/tmp", "/tmp"), EPERM)) {
        return 5;
    }
    if (!refused(set_caps(0xFFFFFFFFu, HIGH_WORD), EPERM)) {
        return 6;
    }
    return 0;
}

/** As root: keep CAP_SYS_ADMIN alone, then give it up. The exit code is 0,
 * or the number of the check that failed. */
static int keep_admin(void)
{
    if (set_caps(1u << CAP_SYS_ADMIN, 0) != 0 || !holds_exactly(1u << CAP_SYS_ADMIN, 0) ||
        status_set("CapEff") != 1ULL << CAP_SYS_ADMIN) {
        return 1;
    }
    if (unshare(CLONE_NEWNS) != 0) {
        return 2;
    }
    if (!refused(set_caps((1u << CAP_SYS_ADMIN) | (1u << CAP_CHOWN), 0), EPERM)) {
        return 3;
    }
    if (set_caps(0, 0) != 0 || !holds_exactly(0, 0) || !mount_refused()) {
        return 4;
    }
    return 0;
}

/** As root: keep CAP_CHOWN alone, check that mount is refused, and exec the
 * probe in the mode `regain`. The exit code is 1, 2 for the exec, or the
 * image's. */
static int keep_chown(void)
{
    char *argv[3];

    if (set_caps(1u << CAP_CHOWN, 0) != 0 || !holds_exactly(1u << CAP_CHOWN, 0) ||
        !mount_refused()) {
        return 1;
    }
    argv[0] = exe;
    argv[1] = "regain";
    argv[2] = NULL;
    execv(exe, argv);
    return 2;
}

/** Fork a child that runs `fn` and exits with its code; that code, or -1. */
static int in_child(int (*fn)(void))
{
    pid_t pid = fork();

    if (pid == 0) {
        _exit(fn());
    }
    return child_status(pid);
}

/** Run the rows of the bounding set, capset and the ambient set, 15 to 22.
 * Returns 0, or 1 after the failure is reported. */
static int cap_rows(void)
{
    int st;

    if (prctl(PR_CAPBSET_READ, CAP_SYS_ADMIN, 0, 0, 0) != 1 ||
        !refused(prctl(PR_CAPBSET_READ, 41, 0, 0, 0), EINVAL)) {
        return fail(rows[CAP_ROW]);
    }
    report_held();
    if (in_child(drop_as_user) != 0) {
        return fail(rows[CAP_ROW + 1]);
    }
    report_held();
    st = in_child(drop_and_exec);
    if (st == 1) {
        return fail(rows[CAP_ROW + 2]);
    }
    report_held();
    if (st == 2) {
        return fail(rows[CAP_ROW + 3]);
    }
    report_held();
    if (st != 0) {
        return fail(rows[CAP_ROW + 4]);
    }
    report_held();
    if (in_child(keep_admin) != 0) {
        return fail(rows[CAP_ROW + 5]);
    }
    report_held();
    if (in_child(keep_chown) != 0) {
        return fail(rows[CAP_ROW + 6]);
    }
    report_held();
    if (prctl(PR_CAP_AMBIENT, PR_CAP_AMBIENT_IS_SET, CAP_CHOWN, 0, 0) != 0 ||
        !refused(prctl(PR_CAP_AMBIENT, PR_CAP_AMBIENT_RAISE, CAP_CHOWN, 0, 0), EPERM) ||
        prctl(PR_CAP_AMBIENT, PR_CAP_AMBIENT_CLEAR_ALL, 0, 0, 0) != 0) {
        return fail(rows[CAP_ROW + 7]);
    }
    report_held();
    return 0;
}

/** In a child, in a mount namespace of its own that receives no mount back
 * from the probe's: bind MASKED_WS over itself, make MASKED_RC through the
 * bind, bind /dev/null over it and open that with O_PATH. The exit code is 0
 * or 1. */
static int mask_file(void)
{
    int fd;

    if (unshare(CLONE_NEWNS) != 0 || mount(NULL, "/", NULL, MS_SLAVE | MS_REC, NULL) != 0 ||
        mount(MASKED_WS, MASKED_WS, NULL, MS_BIND | MS_REC, NULL) != 0) {
        return 1;
    }
    fd = open(MASKED_RC, O_WRONLY | O_CREAT, 0444);
    if (fd < 0 || close(fd) != 0 || mount("/dev/null", MASKED_RC, NULL, MS_BIND, NULL) != 0) {
        return 1;
    }
    fd = open(MASKED_RC, O_PATH);
    return fd < 0 || close(fd) != 0;
}

/* What mask_row has made and not yet removed, for mask_cleanup. */
static int mask_made;    /* MASKED was made by this run */
static int mask_mounted; /* MASKED holds the tmpfs mask_row mounted */

/** Remove the tmpfs on MASKED and the directory, as far as mask_row made
 * them. atexit runs it, so that a row that fails leaves nothing behind;
 * the child's binds went with its namespace. */
static void mask_cleanup(void)
{
    if (mask_mounted && umount2(MASKED, MNT_DETACH) == 0) {
        mask_mounted = 0;
    }
    if (mask_made && !mask_mounted && rmdir(MASKED) == 0) {
        mask_made = 0;
    }
}

/** Run the row of a masked file, the last. Returns 0, or 1 after the
 * failure is reported. */
static int mask_row(void)
{
    if (atexit(mask_cleanup) != 0) {
        return fail("atexit");
    }
    if (mkdir(MASKED, 0755) == 0) {
        mask_made = 1;
    } else if (errno != EEXIST) {
        return fail("mount " MASKED);
    }
    if (mount("tmpfs", MASKED, "tmpfs", 0, NULL) != 0) {
        return fail("mount " MASKED);
    }
    mask_mounted = 1;
    if (mkdir(MASKED_WS, 0755) != 0) {
        return fail("mount " MASKED);
    }
    if (in_child(mask_file) != 0) {
        return fail("a file of a bound directory was not masked with /dev/null");
    }
    if (unlink(MASKED_RC) != 0 || rmdir(MASKED_WS) != 0 || umount(MASKED) != 0) {
        return fail("the directory a namespace that ended bound could not be removed");
    }
    mask_mounted = 0;
    if (rmdir(MASKED) != 0) {
        return fail("the directory a namespace that ended bound could not be removed");
    }
    mask_made = 0;
    report_held();
    return 0;
}

int main(int argc, char **argv)
{
    ssize_t n;
    int i;

    probe_args(argc, argv);
    n = readlink("/proc/self/exe", exe, sizeof(exe) - 1);
    if (n <= 0) {
        printf("SANDBOXMUSL FAIL (/proc/self/exe)\n");
        return 1;
    }
    exe[n] = '\0';
    if (argc > 1 && strcmp(argv[1], "bset") == 0) {
        return bset_image();
    }
    if (argc > 1 && strcmp(argv[1], "regain") == 0) {
        return holds_exactly(0xFFFFFFFFu, HIGH_WORD) ? 0 : 3;
    }
    report_begin(report_case(environ, "guest.default.sandboxmusl"));
    for (i = 0; i < NROWS; i++) {
        report_plan(rows[i]);
    }
    if (sandbox_rows() != 0 || detach_rows() != 0 || cap_rows() != 0 || mask_row() != 0) {
        return 1;
    }
    report_end();
    printf("SANDBOXMUSL OK\n");
    return 0;
}
