/**
 * protectprobe: the protections of Linux's fs.protected_* sysctls, in a
 * sticky directory that every user may write, as /tmp is.
 *
 * Linux 6.12 refuses four things there that the permission bits alone would
 * allow (fs/namei.c:1112-1291), and Debian and Ubuntu turn each on at boot:
 * a symbolic link at the end of a path is followed only by its owner or
 * when the directory's owner owns it (protected_symlinks, EACCES); an
 * O_CREAT open of an existing FIFO or regular file is refused when neither
 * the caller nor the directory's owner owns it (protected_fifos and
 * protected_regular, EACCES); and a hard link to another user's file needs
 * a regular file without set-id bits that the caller may read and write
 * (protected_hardlinks, EPERM). Each guards a root process too, which is
 * refused the same way, from a name another user planted.
 *
 * The probe works in `t`, a directory of mode 01777 owned by root, in the
 * root of a tmpfs: the directory `dir=PATH` names, or protect.d in the
 * working directory, on which the probe mounts one of its own. A child of
 * uid 1000 makes its files there: `afile` of mode 0600, `arw` of 0666, the
 * FIFO `afifo`, the link `alink` to arw and the link `adot` to the
 * directory itself; root has made the link `rlink` to arw. A child of uid
 * 1001 then meets each rule, and its control: the same open without
 * O_CREAT, a link not at the end of a path, which is followed whoever owns
 * it, and a link that the directory's owner owns. Root meets the rules
 * last, turns two of them off through /proc/sys/fs and back, and finds
 * values out of Linux's range refused. The probe puts back the values it
 * found on every exit.
 *
 * An argument `ids=OLD:NEW,...` gives the ids that stand for 1000 and 1001
 * in a native run (probe_args.h); without it they are those numbers.
 *
 * Prints "PROTECTPROBE OK" and exits 0 on success; prints the failing step
 * and exits 1 otherwise. Writes etude's report records (probe_report.h) for
 * the case ETUDE_CASE names, an assertion for each row, as `row_id` lists
 * them: a child's exit status gives the row of its that failed.
 * Freestanding; direct system calls (probe.h).
 */

#include "probe.h"
#include "probe_args.h"
#include "probe_report.h"

typedef unsigned int u32;

/* The id a native run gives in place of `id` (probe_args.h). */
#define ID(id) ((u32)probe_id(id))

#define STDOUT 1
#define EPERM  1
#define EACCES 13
#define EINVAL 22

#define O_RDONLY   0
#define O_WRONLY   01
#define O_CREAT    0100
#define O_NONBLOCK 04000
#define S_IFIFO    0010000
#define MNT_DETACH 2

/* The assertions, a row each, in the order the probe checks them: root's
 * fixtures, uid 1000's child, uid 1001's child, then root's rows. */
enum {
    R_DEFAULTS,
    R_FIXTURES,
    R_DROP_1000,
    R_OWN_FILES,
    R_OWN_FOLLOW,
    R_DROP_1001,
    R_FOLLOW_OTHER,
    R_LINK_READ,
    R_FOLLOW_DIR_OWNER,
    R_FOLLOW_MIDDLE,
    R_CREAT_REGULAR,
    R_OPEN_REGULAR,
    R_CREAT_FIFO,
    R_OPEN_FIFO,
    R_LINK_UNSAFE,
    R_LINK_SAFE,
    R_SYSCTL_USER,
    R_FOLLOW_ROOT,
    R_CREAT_ROOT,
    R_LINK_ROOT,
    R_SYSCTL_OFF,
    R_SYSCTL_RANGE,
    R_CLEANUP,
    R_COUNT
};

static const char *const row_id[R_COUNT] = {
    "defaults",      "fixtures",     "drop-1000",   "own-files",        "own-follow",
    "drop-1001",     "follow-other", "link-read",   "follow-dir-owner", "follow-middle",
    "creat-regular", "open-regular", "creat-fifo",  "open-fifo",        "link-unsafe",
    "link-safe",     "sysctl-user",  "follow-root", "creat-root",       "link-root",
    "sysctl-off",    "sysctl-range", "cleanup"};

/* The four files of /proc/sys/fs, their values as the probe found them, and
 * whether they were read, so that every exit puts them back. */
static const char *const sysctl_file[4] = {
    "/proc/sys/fs/protected_hardlinks", "/proc/sys/fs/protected_symlinks",
    "/proc/sys/fs/protected_fifos", "/proc/sys/fs/protected_regular"};
static int sysctl_found[4];
static int sysctl_read;

/* The tmpfs: the directory, whether the probe made it and mounted it, and
 * the process that owns the fixtures. */
static const char *dir = "protect.d";
static int made_dir, mounted, fixture_owner;

/* A child's row, whose index plus 1 its exit status gives when the row
 * fails; -1 in the parent, which exits 1. */
static int child_row = -1;

/** The length of the string `s`. */
static u32 slen(const char *s)
{
    u32 n = 0;

    while (s[n]) {
        n++;
    }
    return n;
}

/** Write the string `s` to standard output. */
static void puts_(const char *s)
{
    syscall3(SYS_WRITE, STDOUT, (long)s, (int)slen(s));
}

/** The path of `rel` in the tmpfs, in one of four buffers used in turn, so
 * that a call may take two such paths. */
static const char *at(const char *rel)
{
    static char buf[4][256];
    static int next;
    char *p = buf[next++ & 3];
    const char *s = dir;
    u32 i = 0;

    for (; i < sizeof(buf[0]) - 1 && *s != '\0'; s++) {
        p[i++] = *s;
    }
    if (i < sizeof(buf[0]) - 1) {
        p[i++] = '/';
    }
    for (s = rel; i < sizeof(buf[0]) - 1 && *s != '\0'; s++) {
        p[i++] = *s;
    }
    p[i] = '\0';
    return p;
}

/** Write the decimal `v` and a newline to the /proc/sys file `path`: 0, or
 * the error of the open or the write. */
static long sysctl_put(const char *path, int v)
{
    char text[3];
    long fd, rc;

    text[0] = (char)('0' + v);
    text[1] = '\n';
    text[2] = '\0';
    fd = syscall3(SYS_OPEN, (long)path, O_WRONLY, 0);
    if (fd < 0) {
        return fd;
    }
    rc = syscall3(SYS_WRITE, fd, (long)text, 2);
    syscall1(SYS_CLOSE, fd);
    return rc < 0 ? rc : 0;
}

/** The value of the /proc/sys file `path`, one decimal digit and a newline,
 * or -1 when it cannot be read or reads otherwise. */
static int sysctl_get(const char *path)
{
    char text[8];
    long fd, n;

    fd = syscall3(SYS_OPEN, (long)path, O_RDONLY, 0);
    if (fd < 0) {
        return -1;
    }
    n = syscall3(SYS_READ, fd, (long)text, sizeof(text));
    syscall1(SYS_CLOSE, fd);
    if (n != 2 || text[0] < '0' || text[0] > '9' || text[1] != '\n') {
        return -1;
    }
    return text[0] - '0';
}

/** In the process that owns the fixtures, put back the values of /proc/sys
 * the probe found and remove what it made, the tmpfs among them: 0, or -1
 * when something could not be removed. */
static int cleanup(void)
{
    static const char *const files[] = {"t/afile", "t/arw",   "t/afifo", "t/alink",
                                        "t/adot",  "t/rlink", "t/blink", "t/rootlink"};
    int i, bad = 0;

    if (syscall1(SYS_GETPID, 0) != fixture_owner) {
        return 0;
    }
    for (i = 0; sysctl_read && i < 4; i++) {
        bad |= sysctl_put(sysctl_file[i], sysctl_found[i]) != 0;
    }
    for (i = 0; i < (int)(sizeof(files) / sizeof(files[0])); i++) {
        syscall1(SYS_UNLINK, (long)at(files[i]));
    }
    syscall1(SYS_RMDIR, (long)at("t"));
    if (mounted) {
        bad |= syscall2(SYS_UMOUNT2, (long)dir, MNT_DETACH) != 0;
        mounted = 0;
    }
    if (made_dir) {
        bad |= syscall1(SYS_RMDIR, (long)dir) != 0;
        made_dir = 0;
    }
    return bad ? -1 : 0;
}

/** Print "PROTECTPROBE FAIL: `step`", end the report at the assertion that
 * `step` fails, clean up, and exit: 1, or in a child its row's index plus
 * 1. */
static void fail(const char *step)
{
    report_stop(step);
    puts_("PROTECTPROBE FAIL: ");
    puts_(step);
    puts_("\n");
    cleanup();
    syscall3(SYS_EXIT, child_row >= 0 ? child_row + 1 : 1, 0, 0);
}

/** Drop the groups and the three ids to `id`, as the row `row`. */
static void drop_to(u32 id, int row)
{
    child_row = row;
    if (syscall3(SYS_SETGROUPS, 0, 0, 0) != 0 || syscall3(SYS_SETRESGID, id, id, id) != 0 ||
        syscall3(SYS_SETRESUID, id, id, id) != 0) {
        fail("drop the ids");
    }
}

/** Open `path` with `flags` and close it: the open's result. */
static long open_close(const char *path, int flags)
{
    long fd = syscall3(SYS_OPEN, (long)path, flags, 0);

    if (fd >= 0) {
        syscall1(SYS_CLOSE, fd);
    }
    return fd;
}

/** The rows of uid 1000, the owner of the files: it makes them and follows
 * its own link. */
static void owner_rows(void)
{
    long fd;

    drop_to(ID(1000), R_DROP_1000);
    child_row = R_OWN_FILES;
    fd = syscall3(SYS_OPEN, (long)at("t/afile"), O_WRONLY | O_CREAT, 0600);
    if (fd < 0) {
        fail("uid 1000 makes afile");
    }
    syscall1(SYS_CLOSE, fd);
    fd = syscall3(SYS_OPEN, (long)at("t/arw"), O_WRONLY | O_CREAT, 0666);
    if (fd < 0) {
        fail("uid 1000 makes arw");
    }
    syscall1(SYS_CLOSE, fd);
    if (syscall3(SYS_MKNOD, (long)at("t/afifo"), S_IFIFO | 0666, 0) != 0 ||
        syscall2(SYS_SYMLINK, (long)"arw", (long)at("t/alink")) != 0 ||
        syscall2(SYS_SYMLINK, (long)".", (long)at("t/adot")) != 0) {
        fail("uid 1000 makes afifo, alink and adot");
    }
    child_row = R_OWN_FOLLOW;
    if (open_close(at("t/alink"), O_RDONLY) < 0) {
        fail("uid 1000 follows its own link");
    }
}

/** The rows of uid 1001, which owns nothing in the directory: each rule
 * refuses it, and each control lets it through. */
static void other_rows(void)
{
    char text[8];

    drop_to(ID(1001), R_DROP_1001);
    child_row = R_FOLLOW_OTHER;
    if (open_close(at("t/alink"), O_RDONLY) != -EACCES) {
        fail("uid 1001 follows uid 1000's link: not EACCES");
    }
    child_row = R_LINK_READ;
    if (syscall3(SYS_READLINK, (long)at("t/alink"), (long)text, sizeof(text)) != 3 ||
        text[0] != 'a' || text[1] != 'r' || text[2] != 'w') {
        fail("uid 1001 reads uid 1000's link");
    }
    child_row = R_FOLLOW_DIR_OWNER;
    if (open_close(at("t/rlink"), O_RDONLY) < 0) {
        fail("uid 1001 follows the directory owner's link");
    }
    child_row = R_FOLLOW_MIDDLE;
    if (open_close(at("t/adot/arw"), O_RDONLY) < 0) {
        fail("uid 1001 goes through uid 1000's link in the middle of a path");
    }
    child_row = R_CREAT_REGULAR;
    if (open_close(at("t/arw"), O_WRONLY | O_CREAT) != -EACCES) {
        fail("uid 1001 opens uid 1000's file with O_CREAT: not EACCES");
    }
    child_row = R_OPEN_REGULAR;
    if (open_close(at("t/arw"), O_WRONLY) < 0) {
        fail("uid 1001 opens uid 1000's file without O_CREAT");
    }
    child_row = R_CREAT_FIFO;
    if (open_close(at("t/afifo"), O_RDONLY | O_NONBLOCK | O_CREAT) != -EACCES) {
        fail("uid 1001 opens uid 1000's FIFO with O_CREAT: not EACCES");
    }
    child_row = R_OPEN_FIFO;
    if (open_close(at("t/afifo"), O_RDONLY | O_NONBLOCK) < 0) {
        fail("uid 1001 opens uid 1000's FIFO without O_CREAT");
    }
    child_row = R_LINK_UNSAFE;
    if (syscall2(SYS_LINK, (long)at("t/afile"), (long)at("t/blink")) != -EPERM) {
        fail("uid 1001 links uid 1000's 0600 file: not EPERM");
    }
    child_row = R_LINK_SAFE;
    if (syscall2(SYS_LINK, (long)at("t/arw"), (long)at("t/blink")) != 0) {
        fail("uid 1001 links uid 1000's 0666 file");
    }
    child_row = R_SYSCTL_USER;
    if (open_close(sysctl_file[1], O_WRONLY) != -EACCES) {
        fail("uid 1001 opens protected_symlinks for writing: not EACCES");
    }
}

/** Fork a child that runs `rows` and exits 0, wait for it, and report its
 * rows from `first` to before `end` from its wait status: those before a
 * failed row held and that one failed. */
static void run_child(void (*rows)(void), int first, int end)
{
    int pid, got, status = 0, code, i;

    pid = syscall3(SYS_FORK, 0, 0, 0);
    if (pid < 0) {
        fail("fork");
    }
    if (pid == 0) {
        rows();
        syscall3(SYS_EXIT, 0, 0, 0);
    }
    got = syscall3(SYS_WAITPID, pid, (long)&status, 0);
    code = (status >> 8) & 0xFF;
    if (got == pid && status == 0) {
        for (i = first; i < end; i++) {
            report_held();
        }
        return;
    }
    if (got != pid) {
        report_stop("waitpid of the child failed");
    } else if ((status & 0x7F) != 0) {
        report_stop("the child was killed by a signal");
    } else if (code > first && code <= end) {
        for (i = first + 1; i < code; i++) {
            report_held();
        }
        report_stop("a row of the child failed; its PROTECTPROBE FAIL line names it");
    } else {
        report_stop("the child exited with a status no row gives");
    }
    fail("child walk");
}

/** Read the four files of /proc/sys/fs, which must hold Debian's values:
 * protected_hardlinks, protected_symlinks and protected_fifos 1, and
 * protected_regular 2. */
static void default_rows(void)
{
    static const int want[4] = {1, 1, 1, 2};
    int i;

    for (i = 0; i < 4; i++) {
        sysctl_found[i] = sysctl_get(sysctl_file[i]);
        if (sysctl_found[i] < 0) {
            fail("read a protected_* file");
        }
    }
    sysctl_read = 1;
    for (i = 0; i < 4; i++) {
        if (sysctl_found[i] != want[i]) {
            fail("a protected_* file does not hold its default");
        }
    }
    report_held();
}

/** Make the tmpfs unless `dir=` named one, and in it `t`, 01777 and root's,
 * with root's link `rlink` to arw. */
static void fixture_rows(void)
{
    dir = probe_arg("dir", dir);
    if (!probe_arg("dir", 0)) {
        if (syscall2(SYS_MKDIR, (long)dir, 0755) != 0) {
            fail("make protect.d");
        }
        made_dir = 1;
        if (syscall5(SYS_MOUNT, (long)"tmpfs", (long)dir, (long)"tmpfs", 0, 0) != 0) {
            fail("mount a tmpfs on protect.d");
        }
        mounted = 1;
    }
    if (syscall2(SYS_MKDIR, (long)at("t"), 0777) != 0 ||
        syscall2(SYS_CHMOD, (long)at("t"), 01777) != 0 ||
        syscall3(SYS_CHOWN, (long)at("t"), 0, 0) != 0 ||
        syscall2(SYS_SYMLINK, (long)"arw", (long)at("t/rlink")) != 0) {
        fail("make t and rlink");
    }
    report_held();
}

/** The rows of root: each rule refuses it too, but for the link, which
 * CAP_FOWNER makes; with protected_symlinks and protected_regular at 0 it
 * follows and opens; and a value past Linux's range is EINVAL. */
static void root_rows(void)
{
    if (open_close(at("t/alink"), O_RDONLY) != -EACCES) {
        fail("root follows uid 1000's link: not EACCES");
    }
    report_held();
    if (open_close(at("t/arw"), O_WRONLY | O_CREAT) != -EACCES) {
        fail("root opens uid 1000's file with O_CREAT: not EACCES");
    }
    report_held();
    if (syscall2(SYS_LINK, (long)at("t/afile"), (long)at("t/rootlink")) != 0) {
        fail("root links uid 1000's 0600 file");
    }
    report_held();
    if (sysctl_put(sysctl_file[1], 0) != 0 || sysctl_put(sysctl_file[3], 0) != 0 ||
        sysctl_get(sysctl_file[1]) != 0 || sysctl_get(sysctl_file[3]) != 0) {
        fail("turn protected_symlinks and protected_regular off");
    }
    if (open_close(at("t/alink"), O_RDONLY) < 0 ||
        open_close(at("t/arw"), O_WRONLY | O_CREAT) < 0) {
        fail("root follows and opens with the two off");
    }
    if (sysctl_put(sysctl_file[1], 1) != 0 || sysctl_put(sysctl_file[3], 2) != 0 ||
        open_close(at("t/alink"), O_RDONLY) != -EACCES) {
        fail("turn protected_symlinks and protected_regular on again");
    }
    report_held();
    if (sysctl_put(sysctl_file[1], 2) != -EINVAL || sysctl_put(sysctl_file[3], 3) != -EINVAL ||
        sysctl_get(sysctl_file[1]) != 1 || sysctl_get(sysctl_file[3]) != 2) {
        fail("a value past Linux's range is not EINVAL");
    }
    report_held();
}

void probe_main(unsigned long *sp);

PROBE_SP_ENTRY;

/**
 * The probe's entry, root throughout: the defaults and the fixtures, the
 * two children's rows, root's rows, and the cleanup, which puts back the
 * values of /proc/sys and removes the tmpfs.
 */
void probe_main(unsigned long *sp)
{
    int i;

    probe_args((int)sp[0], (char **)(sp + 1));
    report_begin(report_case((char **)(sp + 1) + sp[0] + 1, "guest.default.protectprobe"));
    for (i = 0; i < R_COUNT; i++) {
        report_plan(row_id[i]);
    }
    fixture_owner = syscall1(SYS_GETPID, 0);
    syscall1(SYS_UMASK, 0);
    default_rows();
    fixture_rows();
    run_child(owner_rows, R_DROP_1000, R_DROP_1001);
    run_child(other_rows, R_DROP_1001, R_FOLLOW_ROOT);
    root_rows();
    syscall1(SYS_UMASK, 022);
    if (cleanup() != 0) {
        fail("cleanup: the values or the tmpfs were not put back");
    }
    report_held();
    report_end();
    puts_("PROTECTPROBE OK\n");
    syscall3(SYS_EXIT, 0, 0, 0);
}
