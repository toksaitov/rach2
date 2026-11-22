/**
 * The path and descriptor contract of the VFS.
 *
 * Each row is one promise: the errno a path answers, what a description
 * keeps across unlink and rename, what the synthetic mounts are and are
 * not, and the statfs identities, with the inode counts ext2 keeps and
 * KRFFS does not.
 *
 * `PHYSICAL_DOTDOT` marks the rows whose answer depends on how `..` is
 * resolved: through a missing or non-directory component a lexical resolver
 * succeeds and a physical one must not. The kernel's resolver is physical,
 * and the macro is 1.
 *
 * Needs: root privilege, a writable KRFFS or ext2 root, mounted /proc and
 * /dev, and absent /vp and /procx paths. The root type in /proc/mounts selects
 * the hard-link result, the statfs magic and the inode counts. An unknown
 * root type fails. /proc/mounts names a mount point from the reader's root,
 * so in the private root of a native run (probe_args.h) the root's line is
 * the line of / too.
 *
 * Prints "VFSPROBE OK". Freestanding; direct system calls (probe.h).
 *
 * etude's report (probe_report.h), for the case ETUDE_CASE names, has an
 * assertion for each row, a setup step joined to the row after it, and
 * `rows` gives each failure message its assertion. The first failure fails
 * its assertion and leaves the rest not run. The group `arch` is `i686`,
 * which checks the old stat of the root as `old-stat-root`, or `x86-64`,
 * which has no old call; the group `root` is `krffs`, whose hard link is
 * `link-krffs`, or `ext2`, whose is `link-ext2-names`, `link-ext2-read` and
 * `link-ext2-unlink`. A probe that cannot read the root type reports no
 * branch of `root`.
 */

#include "probe.h"
#include "probe_args.h"
#include "probe_report.h"

#define PHYSICAL_DOTDOT 1

#define O_RDONLY    0
#define O_WRONLY    1
#define O_RDWR      2
#define O_CREAT     0100
#define O_EXCL      0200
#define O_TRUNC     01000
#define O_DIRECTORY 0200000

#define AT_FDCWD (-100)

#define MNT_DETACH 2 /* umount2: detach now, free when unused */
#define WNOHANG    1
#define CLONE_FS   0x00000200
#define SIGCHLD    17

#define EPERM     1
#define ENOENT    2
#define EBADF     9
#define EEXIST    17
#define ENOTDIR   20
#define EISDIR    21
#define ENOTEMPTY 39

#define SEEK_SET 0
#define SEEK_END 2

/* The f_type statfs(2) reports for /proc, the KRFFS boot root, a KRFFS
 * volume mounted by mount(2), and ext2. */
#define STATFS_MAGIC_PROC         0x9FA0
#define STATFS_MAGIC_KRFFS        0x4B524646
#define STATFS_MAGIC_KRFFS_VOLUME 0x4B52
#define STATFS_MAGIC_EXT2         0xEF53

/** exit(2) with `status`; does not return. */
static void sys_exit(int status)
{
    syscall1(SYS_EXIT, status);
    __builtin_unreachable();
}

/** The length of the string `s`. */
static size_t strlen(const char *s)
{
    size_t n = 0;
    while (s[n]) {
        n++;
    }
    return n;
}

/** Write `s` and a newline to standard output. */
static void puts(const char *s)
{
    syscall3(SYS_WRITE, 1, (long)s, (int)strlen(s));
    syscall3(SYS_WRITE, 1, (long)"\n", 1);
}

/* A failure message of the probe and the assertion it fails, with the root
 * magic of the root type it is checked on, 0 for every type. */
struct row {
    const char *why;
    const char *id;
    unsigned root;
};

/* The rows in the order the probe checks them; an assertion's messages are
 * next to each other. */
static const struct row rows[] = {
    {"open /proc/mounts", "root-type", 0},
    {"/proc/mounts must list a krffs or ext2 root", "root-type", 0},
    {"mkdir /vp", "stat-file", 0},
    {"mkdir /vp/d", "stat-file", 0},
    {"create /vp/d/f", "stat-file", 0},
    {"write /vp/d/f", "stat-file", 0},
    {"stat of a real file", "stat-file", 0},
    {"a file used as a directory is ENOTDIR", "enotdir-component", 0},
    {"a missing directory is ENOENT", "enoent-component", 0},
    {"/procx is not /proc", "procx", 0},
    {"the synthetic roots stat", "synthetic-roots", 0},
    {"repeated slashes, dot and dot-dot through a directory", "slashes-dots", 0},
#if !defined __x86_64__
    {"stat of the root", "old-stat-root", 0},
    {"the old stat of the root", "old-stat-root", 0},
    {"the old stat of the root reports what stat of the root does", "old-stat-root", 0},
#else
    {"stat of the root", "dotdot-root", 0},
#endif
    {"dot-dot at the root is the root", "dotdot-root", 0},
    {"dot-dot through a missing directory (marked row)", "dotdot-missing", 0},
    {"dot-dot through a file (marked row)", "dotdot-file", 0},
    {"open the directory", "openat", 0},
    {"openat through the directory descriptor", "openat", 0},
    {"read through the openat descriptor", "openat", 0},
    {"close the openat directory", "openat", 0},
    {"O_CREAT|O_EXCL on an existing file is EEXIST", "excl-eexist", 0},
    {"a directory opened for writing is EISDIR", "dir-write-eisdir", 0},
    {"O_DIRECTORY on a file is ENOTDIR", "o-directory-enotdir", 0},
    {"create /vp/d/f2", "unlinked-name-gone", 0},
    {"reopen /vp/d/f2", "unlinked-name-gone", 0},
    {"unlink the open file", "unlinked-name-gone", 0},
    {"the unlinked name is gone", "unlinked-name-gone", 0},
    {"the description outlives the unlink", "unlink-description", 0},
    {"open before rename", "rename-description", 0},
    {"rename f to g", "rename-description", 0},
    {"the description outlives the rename", "rename-description", 0},
    {"rename g back to f", "rename-description", 0},
    {"lseek to 1", "lseek-read", 0},
    {"read after lseek", "lseek-read", 0},
    {"SEEK_END reports the size", "seek-end", 0},
    {"fsync", "fsync", 0},
    {"syncfs flushes through an open descriptor and refuses a closed one", "syncfs", 0},
    {"ftruncate to 1", "ftruncate", 0},
    {"the ftruncated size reads back", "ftruncate", 0},
    {"open with O_TRUNC", "o-trunc", 0},
    {"refill after O_TRUNC", "o-trunc", 0},
    {"the truncated file reads back what was refilled", "o-trunc", 0},
    {"create the file that will grow", "relocated-size", 0},
    {"create the neighbour", "relocated-size", 0},
    {"the second description on the growing file", "relocated-size", 0},
    {"grow the file past its neighbour", "relocated-size", 0},
    {"the second description sees the grown size", "relocated-size", 0},
    {"the second description reads the moved bytes", "relocated-bytes", 0},
    {"remove the growth files", "relocated-bytes", 0},
    {"getdents64 lists the directory's file", "getdents", 0},
    {"mkdir over a directory is EEXIST", "mkdir-eexist", 0},
    {"rmdir of a full directory is ENOTEMPTY", "rmdir-enotempty", 0},
    {"rmdir of a file is ENOTDIR", "rmdir-enotdir", 0},
    {"unlink of a directory is EISDIR", "unlink-eisdir", 0},
    {"chmod sets the mode stat reports", "chmod", 0},
    {"utimensat with the current time", "utimensat-now", 0},
    {"utimensat sets the time stat reports", "utimensat-set", 0},
    {"the link source has one name", "link-source", 0},
    {"link on krffs is EPERM and creates no name", "link-krffs", STATFS_MAGIC_KRFFS},
    {"link on ext2 succeeds", "link-ext2-names", STATFS_MAGIC_EXT2},
    {"both ext2 names report one inode with two links", "link-ext2-names", STATFS_MAGIC_EXT2},
    {"read through the ext2 hard link", "link-ext2-read", STATFS_MAGIC_EXT2},
    {"unlink leaves the ext2 source with one link", "link-ext2-unlink", STATFS_MAGIC_EXT2},
    {"ftruncate a new file to 64 KiB", "blocks-hole", 0},
    {"st_blocks of a file that is one hole", "blocks-hole", 0},
    {"st_blocks after a write into the hole", "blocks-write", 0},
    {"statfs64 of /proc", "statfs-proc", 0},
    {"statfs64 of the root", "statfs-root", 0},
    {"statfs64 of the root counts its inodes", "statfs-inodes", 0},
    {"chdir", "getcwd", 0},
    {"getcwd reports /vp/d", "getcwd", 0},
    {"a relative open in the working directory", "relative-open", 0},
    {"chdir back to the root", "relative-open", 0},
    {"chdir to /vp/d for the lock rows", "cwd-unlocked", STATFS_MAGIC_EXT2},
    {"read /sys/kernel/debug/bkl/calls", "cwd-unlocked", STATFS_MAGIC_EXT2},
    {"fstatat of a name relative to the working directory", "cwd-unlocked", STATFS_MAGIC_EXT2},
    {"a name relative to the working directory takes no hold of the big lock", "cwd-unlocked",
     STATFS_MAGIC_EXT2},
    {"mount a tmpfs elsewhere", "cwd-unlocked", STATFS_MAGIC_EXT2},
    {"the relative names after a mount take no hold of the big lock", "cwd-unlocked",
     STATFS_MAGIC_EXT2},
    {"unmount the tmpfs elsewhere", "cwd-unlocked", STATFS_MAGIC_EXT2},
    {"chdir back to the root after the lock rows", "cwd-unlocked", STATFS_MAGIC_EXT2},
    {"the files of the shared-record rows", "unlocked-shared", STATFS_MAGIC_EXT2},
    {"the child of the shared-record rows", "unlocked-shared", STATFS_MAGIC_EXT2},
    {"arm the walk gate", "unlocked-shared", STATFS_MAGIC_EXT2},
    {"read the walk gate", "unlocked-shared", STATFS_MAGIC_EXT2},
    {"the child's walk stops at the walk gate", "unlocked-shared", STATFS_MAGIC_EXT2},
    {"chroot while the child's walk is stopped", "unlocked-shared", STATFS_MAGIC_EXT2},
    {"release the walk gate", "unlocked-shared", STATFS_MAGIC_EXT2},
    {"a walk without the big lock that a chroot of a thread sharing the record came between "
     "goes again in the new root",
     "unlocked-shared", STATFS_MAGIC_EXT2},
    {"back to the old root after the shared-record chroot", "unlocked-shared", STATFS_MAGIC_EXT2},
    {"chdir while the child's walk is stopped", "unlocked-shared", STATFS_MAGIC_EXT2},
    {"a walk without the big lock that a chdir of a thread sharing the record came between "
     "goes again from the new directory",
     "unlocked-shared", STATFS_MAGIC_EXT2},
    {"the walk gate counts the one walk given up", "unlocked-shared", STATFS_MAGIC_EXT2},
    {"stop the walk gate", "unlocked-shared", STATFS_MAGIC_EXT2},
    {"remove the files of the shared-record rows", "unlocked-shared", STATFS_MAGIC_EXT2},
    {"sync", "sync", 0},
    {"cleanup", "cleanup", 0},
};

#define NROWS ((int)(sizeof(rows) / sizeof(rows[0])))

/* The root magic once `root_magic` has read it, and 0 before. */
static unsigned root_seen;

/** Whether the strings `a` and `b` are equal. */
static int same(const char *a, const char *b)
{
    while (*a != '\0' && *a == *b) {
        a++;
        b++;
    }
    return *a == *b;
}

/**
 * Report every assertion of the rows checked on this root and end the
 * report: those before the one the message `why` fails held, that one
 * failed, and those after it not run; with `why` null, every one held. A
 * message that no row gives fails the first assertion.
 */
static void report_rows(const char *why)
{
    const char *failed = why == 0 ? 0 : rows[0].id;
    const char *last = 0;
    int i, after = 0;

    for (i = 0; why != 0 && i < NROWS; i++) {
        if (same(rows[i].why, why) && (rows[i].root == 0 || rows[i].root == root_seen)) {
            failed = rows[i].id;
        }
    }
    for (i = 0; i < NROWS; i++) {
        if ((rows[i].root != 0 && rows[i].root != root_seen) ||
            (last != 0 && same(last, rows[i].id))) {
            continue;
        }
        last = rows[i].id;
        if (after) {
            report_notrun(last, "an earlier check failed");
        } else if (failed != 0 && same(failed, last)) {
            report_fail(last, why);
            after = 1;
        } else {
            report_pass(last);
        }
    }
    report_end();
}

static void (*fail_undo)(void); /* a row's undoing, which `fail` runs, or 0 */

/** Print "VFSPROBE FAIL: " and `why`, report it (`report_rows`), and exit
 * 1, once the failing row's undoing has run. */
static void fail(const char *why)
{
    void (*undo)(void) = fail_undo;

    fail_undo = 0;
    if (undo != 0) {
        undo();
    }
    syscall3(SYS_WRITE, 1, (long)"VFSPROBE FAIL: ", 15);
    puts(why);
    report_rows(why);
    sys_exit(1);
}

/** The length of the mount point `point` at `p` in the line that ends at
 * `end`, with the space after it, or 0 when the field is not `point`. */
static int mount_point_is(const char *p, int end, const char *point)
{
    int i;

    for (i = 0; i < end && point[i] != '\0' && p[i] == point[i]; i++) {
    }
    return i < end && point[i] == '\0' && p[i] == ' ' ? i + 1 : 0;
}

/**
 * Read the root type from the mount table, independently of statfs, and
 * return its statfs magic: the type of the line whose mount point is /.
 *
 * Match the mountpoint and type fields of a complete line. Missing or
 * unsupported roots fail before the probe creates files.
 */
static unsigned root_magic(void)
{
    char buf[2048];
    const char *point = "/";
    int fd, n, start, end, p, len;

    fd = syscall3(SYS_OPEN, (long)"/proc/mounts", O_RDONLY, 0);
    if (fd < 0) {
        fail("open /proc/mounts");
    }
    n = syscall3(SYS_READ, fd, (long)buf, sizeof(buf));
    syscall1(SYS_CLOSE, fd);
    for (start = 0; start < n; start = end + 1) {
        for (end = start; end < n && buf[end] != '\n'; end++) {
        }
        if (end == n) {
            break;
        }
        for (p = start; p < end && buf[p] != ' '; p++) {
        }
        len = p + 1 < end ? mount_point_is(buf + p + 1, end - p - 1, point) : 0;
        if (len == 0 || p + 1 + len >= end) {
            continue;
        }
        p += 1 + len;
        if (end - p >= 6 && buf[p] == 'k' && buf[p + 1] == 'r' && buf[p + 2] == 'f' &&
            buf[p + 3] == 'f' && buf[p + 4] == 's' && buf[p + 5] == ' ') {
            puts("vfsprobe: root: krffs");
            return STATFS_MAGIC_KRFFS;
        }
        if (end - p >= 5 && buf[p] == 'e' && buf[p + 1] == 'x' && buf[p + 2] == 't' &&
            buf[p + 3] == '2' && buf[p + 4] == ' ') {
            puts("vfsprobe: root: ext2");
            return STATFS_MAGIC_EXT2;
        }
        break;
    }
    fail("/proc/mounts must list a krffs or ext2 root");
    return 0;
}

static unsigned char stbuf[KSTAT_SIZE];

/* The fstatat calls of each round of `cwd_unlocked_rows`. */
#define CWD_ROUNDS 64

/** The holds of the big lock that system call `nr` began, by its line
 * "syscall NR calls wall_us holds ..." in debugfs's bkl/calls, 0 when it has
 * none. */
static unsigned call_holds(int nr)
{
    static char text[16384];
    char want[16] = "syscall ";
    int fd = syscall3(SYS_OPEN, (long)"/sys/kernel/debug/bkl/calls", O_RDONLY, 0);
    int n, got = 0, i, k = 8, field;
    const char *p;
    unsigned value = 0;

    if (fd < 0) {
        fail("read /sys/kernel/debug/bkl/calls");
    }
    do {
        n = syscall3(SYS_READ, fd, (long)(text + got), (int)sizeof(text) - 1 - got);
        got += n > 0 ? n : 0;
    } while (n > 0 && got < (int)sizeof(text) - 1);
    syscall1(SYS_CLOSE, fd);
    if (n < 0 || got == (int)sizeof(text) - 1) {
        fail("read /sys/kernel/debug/bkl/calls");
    }
    text[got] = '\0';
    if (nr >= 100) {
        want[k++] = (char)('0' + nr / 100);
    }
    if (nr >= 10) {
        want[k++] = (char)('0' + nr / 10 % 10);
    }
    want[k++] = (char)('0' + nr % 10);
    want[k++] = ' ';
    want[k] = '\0';
    for (p = text; *p; p++) {
        for (i = 0; want[i] && p[i] == want[i]; i++) {
        }
        if (!want[i] && (p == text || p[-1] == '\n')) {
            break;
        }
    }
    if (!*p) {
        return 0;
    }
    p += k;
    for (field = 0; field < 3; field++) {
        if (*p < '0' || *p > '9') {
            fail("read /sys/kernel/debug/bkl/calls");
        }
        value = 0;
        while (*p >= '0' && *p <= '9') {
            value = value * 10 + (unsigned)(*p++ - '0');
        }
        while (*p == ' ') {
            p++;
        }
    }
    return value;
}

/** `rounds` fstatat calls of the name f relative to the working directory,
 * failing at the first that does not answer 0. */
static void stat_relative(int rounds)
{
    int i;

    for (i = 0; i < rounds; i++) {
        if (syscall4(SYS_FSTATAT, AT_FDCWD, (long)"f", (long)stbuf, 0) != 0) {
            fail("fstatat of a name relative to the working directory");
        }
    }
}

/** Undo what `cwd_unlocked_rows` made, for `fail`: the tmpfs on /vp/m, its
 * directory and the working directory. */
static void cwd_unlocked_undo(void)
{
    syscall2(SYS_UMOUNT2, (long)"/vp/m", MNT_DETACH);
    syscall1(SYS_RMDIR, (long)"/vp/m");
    syscall1(SYS_CHDIR, (long)"/");
}

/**
 * On an ext2 root, whose names the name cache keeps, a name relative to the
 * working directory /vp/d takes no hold of the big lock, by bkl/calls's
 * count for fstatat: its walk starts at the directory the process holds and
 * the cache finishes it without the lock. A tmpfs mounted on /vp/m moves the
 * walk sequence, and the names after it take no hold either, since a walk
 * from a held directory has no path above it to check. A root of another
 * type, `magic`, has no such rows. A failure undoes what the rows made
 * (`cwd_unlocked_undo`).
 */
static void cwd_unlocked_rows(unsigned magic)
{
    unsigned before, after;

    if (magic != STATFS_MAGIC_EXT2) {
        return;
    }
    fail_undo = cwd_unlocked_undo;
    if (syscall1(SYS_CHDIR, (long)"/vp/d") != 0) {
        fail("chdir to /vp/d for the lock rows");
    }
    stat_relative(1);
    before = call_holds(SYS_FSTATAT);
    stat_relative(CWD_ROUNDS);
    after = call_holds(SYS_FSTATAT);
    if (after != before) {
        fail("a name relative to the working directory takes no hold of the big lock");
    }
    if (syscall2(SYS_MKDIR, (long)"/vp/m", 0755) != 0 ||
        syscall5(SYS_MOUNT, (long)"none", (long)"/vp/m", (long)"tmpfs", 0, 0) != 0) {
        fail("mount a tmpfs elsewhere");
    }
    before = call_holds(SYS_FSTATAT);
    stat_relative(CWD_ROUNDS);
    after = call_holds(SYS_FSTATAT);
    if (after != before) {
        fail("the relative names after a mount take no hold of the big lock");
    }
    if (syscall2(SYS_UMOUNT2, (long)"/vp/m", 0) != 0 || syscall1(SYS_RMDIR, (long)"/vp/m") != 0) {
        fail("unmount the tmpfs elsewhere");
    }
    if (syscall1(SYS_CHDIR, (long)"/") != 0) {
        fail("chdir back to the root after the lock rows");
    }
    fail_undo = 0;
}

/* The walk gate of debugfs (debugfs.5), which stops a chosen thread at a
 * walk event until a write lets it go. */
#define WALK_GATE "/sys/kernel/debug/vfs/walk_gate"

/* The descriptors the shared-record rows write and read the walk gate by,
 * opened before a row chroots away from its path, or -1. */
static int gate_wfd = -1, gate_rfd = -1;

/** Write `text` to the walk gate, or fail `what`. */
static void gate_write(const char *text, const char *what)
{
    int n = (int)strlen(text);

    if (syscall3(SYS_WRITE, gate_wfd, (long)text, n) != n) {
        fail(what);
    }
}

/** The walk gate's count of `name`, by its line (debugfs.5), read from its
 * start, or fail. */
static unsigned gate_count(const char *name)
{
    char line[160];
    int n = syscall3(SYS_LSEEK, gate_rfd, 0, SEEK_SET) != 0
                ? -1
                : syscall3(SYS_READ, gate_rfd, (long)line, (int)sizeof(line) - 1);
    const char *p;
    unsigned value = 0;
    int i = 0;

    if (n <= 0) {
        fail("read the walk gate");
    }
    line[n] = '\0';
    for (p = line; *p; p++) {
        for (i = 0; name[i] && p[i + 1] == name[i]; i++) {
        }
        if (p[0] == ' ' && !name[i] && p[i + 1] == ' ') {
            break;
        }
    }
    if (!*p) {
        fail("read the walk gate");
    }
    for (p += i + 2; *p >= '0' && *p <= '9'; p++) {
        value = value * 10u + (unsigned)(*p - '0');
    }
    return value;
}

static int shared_root = -1; /* a descriptor of the root the rows began in, or -1 */
static int shared_child;     /* the child of a shared-record row, or 0 */
static int shared_pipe[2] = {-1, -1};

/** Undo what `shared_rows` made, for `fail`: the gate, the child, the root
 * and working directory, the descriptors and the files. */
static void shared_undo(void)
{
    int st;

    syscall3(SYS_WRITE, gate_wfd, (long)"off", 3);
    syscall1(SYS_CLOSE, gate_wfd);
    syscall1(SYS_CLOSE, gate_rfd);
    gate_wfd = gate_rfd = -1;
    if (shared_child > 0) {
        syscall2(SYS_KILL, shared_child, 9);
        syscall4(SYS_WAIT4, shared_child, (long)&st, 0, 0);
        shared_child = 0;
    }
    if (shared_root >= 0) {
        syscall1(SYS_FCHDIR, shared_root);
        syscall1(SYS_CHROOT, (long)".");
        syscall1(SYS_CLOSE, shared_root);
        shared_root = -1;
    }
    syscall1(SYS_CHDIR, (long)"/");
    syscall1(SYS_CLOSE, shared_pipe[0]);
    syscall1(SYS_CLOSE, shared_pipe[1]);
    shared_pipe[0] = shared_pipe[1] = -1;
    syscall1(SYS_UNLINK, (long)"/vp/s/a/f");
    syscall1(SYS_UNLINK, (long)"/vp/s/b/f");
    syscall1(SYS_RMDIR, (long)"/vp/s/a");
    syscall1(SYS_RMDIR, (long)"/vp/s/b");
    syscall1(SYS_RMDIR, (long)"/vp/s");
}

/** Make the file `path` holding the `n` bytes of `text`, or fail. */
static void shared_file(const char *path, const char *text, int n)
{
    int fd = syscall3(SYS_OPEN, (long)path, O_WRONLY | O_CREAT | O_TRUNC, 0644);

    if (fd < 0 || syscall3(SYS_WRITE, fd, (long)text, n) != n) {
        fail("the files of the shared-record rows");
    }
    syscall1(SYS_CLOSE, fd);
}

/**
 * One shared-record row: a child that shares the working directory and the
 * root (CLONE_FS) opens f relative to the working directory /vp/s/a, whose f
 * holds 2 bytes, and exits with the count it reads. The walk gate stops the
 * child's walk without the big lock where it has taken its start and the
 * walk sequence, and the parent, from the gate's own count, chroots to
 * /vp/s/b, which moves the shared working directory there too, or with
 * `cd` changes to it, and lets the walk go on. The walk goes again under
 * the big lock, from the new directory, whose f holds 3 bytes, and the gate
 * counts that one walk given up; read past the change, the old directory's
 * pair would give 2. Returns with the working directory /vp/s/a again.
 */
static void shared_row(int cd)
{
    const char *why = cd ? "a walk without the big lock that a chdir of a thread sharing the "
                           "record came between goes again from the new directory"
                         : "a walk without the big lock that a chroot of a thread sharing the "
                           "record came between goes again in the new root";
    char text[32] = "arm ", byte = 0, digits[12];
    int pid, st, i = 4, k = 0, n;

    if (syscall1(SYS_PIPE, (long)shared_pipe) != 0) {
        fail("the child of the shared-record rows");
    }
    pid = syscall5(SYS_CLONE, CLONE_FS | SIGCHLD, 0, 0, 0, 0);
    if (pid == 0) {
        char got[8];
        int fd;

        if (syscall3(SYS_READ, shared_pipe[0], (long)&byte, 1) != 1) {
            sys_exit(100);
        }
        fd = syscall3(SYS_OPEN, (long)"f", O_RDONLY, 0);
        sys_exit(fd < 0 ? 101 : syscall3(SYS_READ, fd, (long)got, (int)sizeof(got)));
    }
    if (pid < 0) {
        fail("the child of the shared-record rows");
    }
    shared_child = pid;
    n = pid;
    do {
        digits[k++] = (char)('0' + n % 10);
        n /= 10;
    } while (n > 0);
    while (k > 0) {
        text[i++] = digits[--k];
    }
    for (k = 0; " unlocked"[k]; k++) {
        text[i++] = " unlocked"[k];
    }
    text[i] = '\0';
    gate_write(text, "arm the walk gate");
    if (syscall3(SYS_WRITE, shared_pipe[1], (long)&byte, 1) != 1) {
        fail("the child of the shared-record rows");
    }
    while (gate_count("waiting") != 1) {
        if (syscall4(SYS_WAIT4, pid, (long)&st, WNOHANG, 0) == pid) {
            shared_child = 0;
            fail("the child's walk stops at the walk gate");
        }
        syscall0(SYS_SCHED_YIELD);
    }
    if (cd ? syscall1(SYS_CHDIR, (long)"/vp/s/b") != 0
           : syscall1(SYS_CHROOT, (long)"/vp/s/b") != 0) {
        fail(cd ? "chdir while the child's walk is stopped"
                : "chroot while the child's walk is stopped");
    }
    gate_write("release", "release the walk gate");
    if (syscall4(SYS_WAIT4, pid, (long)&st, 0, 0) != pid) {
        fail(why);
    }
    shared_child = 0;
    if ((st & 0x7f) != 0 || ((st >> 8) & 0xff) != 3) {
        fail(why);
    }
    if (gate_count("miss") != 1) {
        fail("the walk gate counts the one walk given up");
    }
    gate_write("off", "stop the walk gate");
    syscall1(SYS_CLOSE, shared_pipe[0]);
    syscall1(SYS_CLOSE, shared_pipe[1]);
    shared_pipe[0] = shared_pipe[1] = -1;
    if (!cd && (syscall1(SYS_FCHDIR, shared_root) != 0 || syscall1(SYS_CHROOT, (long)".") != 0)) {
        fail("back to the old root after the shared-record chroot");
    }
    if (syscall1(SYS_CHDIR, (long)"/vp/s/a") != 0) {
        fail(cd ? "chdir while the child's walk is stopped"
                : "back to the old root after the shared-record chroot");
    }
}

/**
 * On an ext2 root, whose names a walk without the big lock finishes, a chroot
 * and a chdir by a thread that shares the record (CLONE_FS) between the
 * start of such a walk and its end send the walk again, under the big lock
 * (`shared_row`), as Linux's walk restarts when `fs->seq` or the mount
 * sequence moved (fs/namei.c `try_to_unlazy` in 6.12). A root of another
 * type, `magic`, has no such rows. A failure undoes what the rows made
 * (`shared_undo`).
 */
static void shared_rows(unsigned magic)
{
    if (magic != STATFS_MAGIC_EXT2) {
        return;
    }
    fail_undo = shared_undo;
    if (syscall2(SYS_MKDIR, (long)"/vp/s", 0755) != 0 ||
        syscall2(SYS_MKDIR, (long)"/vp/s/a", 0755) != 0 ||
        syscall2(SYS_MKDIR, (long)"/vp/s/b", 0755) != 0) {
        fail("the files of the shared-record rows");
    }
    shared_file("/vp/s/a/f", "aa", 2);
    shared_file("/vp/s/b/f", "bbb", 3);
    gate_wfd = syscall3(SYS_OPEN, (long)WALK_GATE, O_WRONLY, 0);
    gate_rfd = syscall3(SYS_OPEN, (long)WALK_GATE, O_RDONLY, 0);
    if (gate_wfd < 0 || gate_rfd < 0) {
        fail("read the walk gate");
    }
    shared_root = syscall3(SYS_OPEN, (long)"/", O_RDONLY | O_DIRECTORY, 0);
    if (shared_root < 0 || syscall1(SYS_CHDIR, (long)"/vp/s/a") != 0) {
        fail("the files of the shared-record rows");
    }
    stat_relative(2);
    shared_row(0);
    shared_row(1);
    syscall1(SYS_CLOSE, shared_root);
    syscall1(SYS_CLOSE, gate_wfd);
    syscall1(SYS_CLOSE, gate_rfd);
    shared_root = gate_wfd = gate_rfd = -1;
    if (syscall1(SYS_CHDIR, (long)"/") != 0 || syscall1(SYS_UNLINK, (long)"/vp/s/a/f") != 0 ||
        syscall1(SYS_UNLINK, (long)"/vp/s/b/f") != 0 || syscall1(SYS_RMDIR, (long)"/vp/s/a") != 0 ||
        syscall1(SYS_RMDIR, (long)"/vp/s/b") != 0 || syscall1(SYS_RMDIR, (long)"/vp/s") != 0) {
        fail("remove the files of the shared-record rows");
    }
    fail_undo = 0;
}

/** The rows of walks without the big lock on an ext2 root, the root `magic`
 * names: its holds (`cwd_unlocked_rows`) and the changes that send it again
 * (`shared_rows`). */
static void unlocked_rows(unsigned magic)
{
    cwd_unlocked_rows(magic);
    shared_rows(magic);
}

/** stat(2) of `path` into `stbuf`: 0, or a negated errno. */
static int xstat(const char *path)
{
    stbuf[0] = 0;
    return syscall2(SYS_STAT, (long)path, (long)stbuf);
}

/** The low 32 bits of a field of the last stat, at its KSTAT_ offset: the
 * mode, the link count, the blocks, the mtime, the inode. */
static unsigned st_field(int off)
{
    unsigned v;

    __builtin_memcpy(&v, stbuf + off, 4);
    return v;
}

#if !defined __x86_64__
/* Offsets in i386's old struct stat, 64 bytes, which SYS_OLDSTAT fills. */
#define OSTAT_SIZE  64
#define OSTAT_INO   4
#define OSTAT_MODE  8
#define OSTAT_UID   12
#define OSTAT_GID   14
#define OSTAT_MTIME 40

/**
 * Fail unless the old stat(2) of "/" reports the inode, mode, owner, group
 * and modification time that the stat of "/" just left in `stbuf` reports,
 * the ids narrowed to the old struct's 16 bits. The old call has a
 * shortcut of its own for the KRFFS root, which must not answer for a root
 * of another type.
 */
static void check_old_stat_root(void)
{
    unsigned char old[OSTAT_SIZE];
    unsigned ino, mtime;
    unsigned short mode, uid, gid;

    if (syscall2(SYS_OLDSTAT, (long)"/", (long)old) != 0) {
        fail("the old stat of the root");
    }
    __builtin_memcpy(&ino, old + OSTAT_INO, 4);
    __builtin_memcpy(&mode, old + OSTAT_MODE, 2);
    __builtin_memcpy(&uid, old + OSTAT_UID, 2);
    __builtin_memcpy(&gid, old + OSTAT_GID, 2);
    __builtin_memcpy(&mtime, old + OSTAT_MTIME, 4);
    if (ino != st_field(KSTAT_INO) || mode != (unsigned short)st_field(KSTAT_MODE) ||
        uid != (unsigned short)st_field(KSTAT_UID) || gid != (unsigned short)st_field(KSTAT_GID) ||
        mtime != st_field(KSTAT_MTIME)) {
        fail("the old stat of the root reports what stat of the root does");
    }
}
#endif

/** stat(2) of "/", into `stbuf`, and on i686 the old stat(2) of it checked
 * against that (`check_old_stat_root`): the root's inode number. */
static unsigned stat_root(void)
{
    if (xstat("/") != 0) {
        fail("stat of the root");
    }
#if !defined __x86_64__
    check_old_stat_root();
#endif
    return st_field(KSTAT_INO);
}

/** Whether the getdents64 records in the first `n` bytes of `b` name `name`,
 * walked by d_reclen. */
static int dirents_contain(const unsigned char *b, int n, const char *name)
{
    int off = 0;

    while (off + 19 < n) {
        unsigned short reclen;
        const char *dn = (const char *)b + off + 19;
        int i = 0;

        __builtin_memcpy(&reclen, b + off + 16, 2);
        while (name[i] && dn[i] == name[i]) {
            i++;
        }
        if (!name[i] && !dn[i]) {
            return 1;
        }
        if (reclen == 0) {
            break;
        }
        off += reclen;
    }
    return 0;
}

static unsigned char iobuf[512];

/**
 * A file grown by ftruncate has a hole for its new bytes. Ext2 reports the
 * blocks it holds as st_blocks, none for the hole, so that du and cp see
 * it; KRFFS rounds the size up. st_blocks counts 512-byte units, at
 * KSTAT_BLOCKS.
 */
static void check_blocks(unsigned magic)
{
    int fd = syscall3(SYS_OPEN, (long)"/vp/d/s", O_RDWR | O_CREAT, 0644);

    if (fd < 0 || syscall2(SYS_FTRUNCATE, fd, 65536) != 0 || xstat("/vp/d/s") != 0) {
        fail("ftruncate a new file to 64 KiB");
    }
    if (st_field(KSTAT_BLOCKS) != (magic == STATFS_MAGIC_EXT2 ? 0u : 128u)) {
        fail("st_blocks of a file that is one hole");
    }
    if (syscall3(SYS_WRITE, fd, (long)"x", 1) != 1 || xstat("/vp/d/s") != 0 ||
        st_field(KSTAT_BLOCKS) == 0 ||
        (magic == STATFS_MAGIC_EXT2 && st_field(KSTAT_BLOCKS) >= 128)) {
        fail("st_blocks after a write into the hole");
    }
    syscall1(SYS_CLOSE, fd);
    syscall1(SYS_UNLINK, (long)"/vp/d/s");
}

/**
 * Check the root's hard-link contract and remove the extra name.
 *
 * Ext2 must preserve the inode and data through both names, then decrement
 * the link count on unlink. KRFFS must refuse the link without creating it.
 */
static void check_link(unsigned magic)
{
    unsigned ino;
    int fd;

    if (xstat("/vp/d/f") != 0 || st_field(KSTAT_NLINK) != 1) {
        fail("the link source has one name");
    }
    ino = st_field(KSTAT_INO);
    if (magic == STATFS_MAGIC_KRFFS) {
        if (syscall2(SYS_LINK, (long)"/vp/d/f", (long)"/vp/d/h") != -EPERM ||
            xstat("/vp/d/h") != -ENOENT) {
            fail("link on krffs is EPERM and creates no name");
        }
        return;
    }
    if (syscall2(SYS_LINK, (long)"/vp/d/f", (long)"/vp/d/h") != 0) {
        fail("link on ext2 succeeds");
    }
    if (xstat("/vp/d/h") != 0 || st_field(KSTAT_INO) != ino || st_field(KSTAT_NLINK) != 2 ||
        xstat("/vp/d/f") != 0 || st_field(KSTAT_INO) != ino || st_field(KSTAT_NLINK) != 2) {
        fail("both ext2 names report one inode with two links");
    }
    fd = syscall3(SYS_OPEN, (long)"/vp/d/h", O_RDONLY, 0);
    if (fd < 0 || syscall3(SYS_READ, fd, (long)iobuf, 4) != 3 || iobuf[0] != 'a' ||
        iobuf[1] != 'b' || iobuf[2] != 'c') {
        fail("read through the ext2 hard link");
    }
    syscall1(SYS_CLOSE, fd);
    if (syscall1(SYS_UNLINK, (long)"/vp/d/h") != 0 || xstat("/vp/d/h") != -ENOENT ||
        xstat("/vp/d/f") != 0 || st_field(KSTAT_INO) != ino || st_field(KSTAT_NLINK) != 1) {
        fail("unlink leaves the ext2 source with one link");
    }
}

int stat_marked_missing_dotdot(void);
int stat_marked_file_dotdot(void);

/** Whether `..` through a missing directory answers as `PHYSICAL_DOTDOT`
 * says: -ENOENT from a physical resolver, success from a lexical one. */
int stat_marked_missing_dotdot(void)
{
    int rc = xstat("/vp/nosuch/../d/f");
#if PHYSICAL_DOTDOT
    return rc == -ENOENT;
#else
    return rc == 0;
#endif
}

/** Whether `..` through a file answers as `PHYSICAL_DOTDOT` says: -ENOTDIR
 * from a physical resolver, success from a lexical one. */
int stat_marked_file_dotdot(void)
{
    int rc = xstat("/vp/d/f/../f");
#if PHYSICAL_DOTDOT
    return rc == -ENOTDIR;
#else
    return rc == 0;
#endif
}

/** Keep the arguments of the initial stack `sp` (probe_args.h), begin the
 * report with the branch of the architecture and that of the root type, and
 * return the root type's magic (`root_magic`). */
static unsigned start(unsigned long *sp)
{
    probe_args((int)sp[0], (char **)(sp + 1));
    report_begin(report_case((char **)(sp + 1) + sp[0] + 1, "guest.default.vfsprobe"));
#if defined __x86_64__
    report_branch("arch", "x86-64");
#else
    report_branch("arch", "i686");
#endif
    root_seen = root_magic();
    report_branch("root", root_seen == STATFS_MAGIC_KRFFS ? "krffs" : "ext2");
    return root_seen;
}

/** Report every assertion held (`report_rows`), print "VFSPROBE OK" and
 * exit 0. */
static void succeed(void)
{
    report_rows(0);
    puts("VFSPROBE OK");
    sys_exit(0);
}

/** Whether `got`, the root's f_type, is `magic`, the one the probe expects;
 * a KRFFS volume mounted by mount(2) counts as the KRFFS boot root. */
static int magic_matches(unsigned got, unsigned magic)
{
    return got == magic || (magic == STATFS_MAGIC_KRFFS && got == STATFS_MAGIC_KRFFS_VOLUME);
}

void probe_main(unsigned long *sp);

PROBE_SP_ENTRY;

/**
 * Make /vp/d/f and run the rows in order, failing at the first with its
 * name: what a path answers, open and openat, the description across unlink
 * and rename, offsets and sizes, a relocated file, directories, attributes,
 * the hard link and the blocks, statfs and the working directory, with, on
 * an ext2 root, the big lock a name relative to it takes; then remove what
 * it made. A relocated file keeps every description: a second
 * description on a file that grows past a neighbour made right after it,
 * so that it cannot grow in place, reads the grown size and the moved bytes.
 */
void probe_main(unsigned long *sp)
{
    int fd, dfd, rc;
    char cwd[64];
    unsigned magic = start(sp);

    if (syscall2(SYS_MKDIR, (long)"/vp", 0755) != 0) {
        fail("mkdir /vp");
    }
    if (syscall2(SYS_MKDIR, (long)"/vp/d", 0755) != 0) {
        fail("mkdir /vp/d");
    }
    fd = syscall3(SYS_OPEN, (long)"/vp/d/f", O_CREAT | O_RDWR, 0644);
    if (fd < 0) {
        fail("create /vp/d/f");
    }
    if (syscall3(SYS_WRITE, fd, (long)"abc", 3) != 3) {
        fail("write /vp/d/f");
    }
    syscall1(SYS_CLOSE, fd);

    if (xstat("/vp/d/f") != 0) {
        fail("stat of a real file");
    }
    if (xstat("/vp/d/f/x") != -ENOTDIR) {
        fail("a file used as a directory is ENOTDIR");
    }
    if (xstat("/vp/nosuch/f") != -ENOENT) {
        fail("a missing directory is ENOENT");
    }
    if (xstat("/procx") != -ENOENT) {
        fail("/procx is not /proc");
    }
    if (xstat("/proc") != 0 || xstat("/dev") != 0) {
        fail("the synthetic roots stat");
    }
    /* Spelled in pieces: the style check refuses two slashes in a row. */
    if (xstat("/"
              "/vp/"
              "/d/"
              "/f") != 0 ||
        xstat("/vp/./d/./f") != 0 || xstat("/vp/d/../d/f") != 0) {
        fail("repeated slashes, dot and dot-dot through a directory");
    }
    {
        unsigned root_ino;

        root_ino = stat_root();
        if (xstat("/..") != 0 || st_field(KSTAT_INO) != root_ino || xstat("/vp/..") != 0 ||
            st_field(KSTAT_INO) != root_ino) {
            fail("dot-dot at the root is the root");
        }
    }
    if (!stat_marked_missing_dotdot()) {
        fail("dot-dot through a missing directory (marked row)");
    }
    if (!stat_marked_file_dotdot()) {
        fail("dot-dot through a file (marked row)");
    }

    dfd = syscall3(SYS_OPEN, (long)"/vp/d", O_RDONLY, 0);
    if (dfd < 0) {
        fail("open the directory");
    }
    fd = syscall4(SYS_OPENAT, dfd, (long)"f", O_RDONLY, 0);
    if (fd < 0) {
        fail("openat through the directory descriptor");
    }
    if (syscall3(SYS_READ, fd, (long)iobuf, 8) != 3 || iobuf[0] != 'a') {
        fail("read through the openat descriptor");
    }
    syscall1(SYS_CLOSE, fd);
    if (syscall1(SYS_CLOSE, dfd) != 0) {
        fail("close the openat directory");
    }
    if (syscall3(SYS_OPEN, (long)"/vp/d/f", O_CREAT | O_EXCL | O_RDWR, 0644) != -EEXIST) {
        fail("O_CREAT|O_EXCL on an existing file is EEXIST");
    }
    if (syscall3(SYS_OPEN, (long)"/vp/d", O_WRONLY, 0) != -EISDIR) {
        fail("a directory opened for writing is EISDIR");
    }
    if (syscall3(SYS_OPEN, (long)"/vp/d/f", O_RDONLY | O_DIRECTORY, 0) != -ENOTDIR) {
        fail("O_DIRECTORY on a file is ENOTDIR");
    }

    fd = syscall3(SYS_OPEN, (long)"/vp/d/f2", O_CREAT | O_RDWR, 0644);
    if (fd < 0 || syscall3(SYS_WRITE, fd, (long)"zz", 2) != 2) {
        fail("create /vp/d/f2");
    }
    syscall1(SYS_CLOSE, fd);
    fd = syscall3(SYS_OPEN, (long)"/vp/d/f2", O_RDONLY, 0);
    if (fd < 0) {
        fail("reopen /vp/d/f2");
    }
    if (syscall1(SYS_UNLINK, (long)"/vp/d/f2") != 0) {
        fail("unlink the open file");
    }
    if (xstat("/vp/d/f2") != -ENOENT) {
        fail("the unlinked name is gone");
    }
    if (syscall3(SYS_READ, fd, (long)iobuf, 8) != 2 || iobuf[0] != 'z') {
        fail("the description outlives the unlink");
    }
    syscall1(SYS_CLOSE, fd);

    fd = syscall3(SYS_OPEN, (long)"/vp/d/f", O_RDONLY, 0);
    if (fd < 0) {
        fail("open before rename");
    }
    if (syscall2(SYS_RENAME, (long)"/vp/d/f", (long)"/vp/d/g") != 0) {
        fail("rename f to g");
    }
    if (syscall3(SYS_READ, fd, (long)iobuf, 8) != 3 || iobuf[2] != 'c') {
        fail("the description outlives the rename");
    }
    syscall1(SYS_CLOSE, fd);
    if (syscall2(SYS_RENAME, (long)"/vp/d/g", (long)"/vp/d/f") != 0) {
        fail("rename g back to f");
    }

    fd = syscall3(SYS_OPEN, (long)"/vp/d/f", O_RDONLY, 0);
    if (syscall3(SYS_LSEEK, fd, 1, SEEK_SET) != 1) {
        fail("lseek to 1");
    }
    if (syscall3(SYS_READ, fd, (long)iobuf, 1) != 1 || iobuf[0] != 'b') {
        fail("read after lseek");
    }
    if (syscall3(SYS_LSEEK, fd, 0, SEEK_END) != 3) {
        fail("SEEK_END reports the size");
    }
    if (syscall1(SYS_FSYNC, fd) != 0) {
        fail("fsync");
    }
    if (syscall1(SYS_SYNCFS, fd) != 0 || syscall1(SYS_SYNCFS, 999) != -EBADF) {
        fail("syncfs flushes through an open descriptor and refuses a closed one");
    }
    syscall1(SYS_CLOSE, fd);
    fd = syscall3(SYS_OPEN, (long)"/vp/d/f", O_RDWR, 0);
    if (syscall2(SYS_FTRUNCATE, fd, 1) != 0) {
        fail("ftruncate to 1");
    }
    if (syscall3(SYS_LSEEK, fd, 0, SEEK_END) != 1) {
        fail("the ftruncated size reads back");
    }
    syscall1(SYS_CLOSE, fd);
    fd = syscall3(SYS_OPEN, (long)"/vp/d/f", O_WRONLY | O_TRUNC, 0);
    if (fd < 0) {
        fail("open with O_TRUNC");
    }
    if (syscall3(SYS_WRITE, fd, (long)"abc", 3) != 3) {
        fail("refill after O_TRUNC");
    }
    syscall1(SYS_CLOSE, fd);
    fd = syscall3(SYS_OPEN, (long)"/vp/d/f", O_RDONLY, 0);
    if (syscall3(SYS_READ, fd, (long)iobuf, 8) != 3 || iobuf[0] != 'a' || iobuf[1] != 'b' ||
        iobuf[2] != 'c') {
        fail("the truncated file reads back what was refilled");
    }
    syscall1(SYS_CLOSE, fd);

    fd = syscall3(SYS_OPEN, (long)"/vp/d/grow", O_CREAT | O_RDWR, 0644);
    if (fd < 0 || syscall3(SYS_WRITE, fd, (long)"seed", 4) != 4) {
        fail("create the file that will grow");
    }
    dfd = syscall3(SYS_OPEN, (long)"/vp/d/wall", O_CREAT | O_RDWR, 0644);
    if (dfd < 0 || syscall3(SYS_WRITE, dfd, (long)iobuf, 512) != 512) {
        fail("create the neighbour");
    }
    syscall1(SYS_CLOSE, dfd);
    dfd = syscall3(SYS_OPEN, (long)"/vp/d/grow", O_RDONLY, 0);
    if (dfd < 0) {
        fail("the second description on the growing file");
    }
    for (rc = 0; rc < 128; rc++) {
        iobuf[0] = (unsigned char)rc;
        if (syscall3(SYS_WRITE, fd, (long)iobuf, 512) != 512) {
            fail("grow the file past its neighbour");
        }
    }
    if (syscall3(SYS_LSEEK, dfd, 0, SEEK_END) != 4 + 128 * 512) {
        fail("the second description sees the grown size");
    }
    if (syscall3(SYS_LSEEK, dfd, 4 + 127 * 512, SEEK_SET) < 0 ||
        syscall3(SYS_READ, dfd, (long)iobuf, 1) != 1 || iobuf[0] != 127) {
        fail("the second description reads the moved bytes");
    }
    syscall1(SYS_CLOSE, dfd);
    syscall1(SYS_CLOSE, fd);
    if (syscall1(SYS_UNLINK, (long)"/vp/d/grow") != 0 ||
        syscall1(SYS_UNLINK, (long)"/vp/d/wall") != 0) {
        fail("remove the growth files");
    }

    dfd = syscall3(SYS_OPEN, (long)"/vp/d", O_RDONLY, 0);
    rc = syscall3(SYS_GETDENTS64, dfd, (long)iobuf, sizeof(iobuf));
    if (rc <= 0 || !dirents_contain(iobuf, rc, "f")) {
        fail("getdents64 lists the directory's file");
    }
    syscall1(SYS_CLOSE, dfd);
    if (syscall2(SYS_MKDIR, (long)"/vp/d", 0755) != -EEXIST) {
        fail("mkdir over a directory is EEXIST");
    }
    if (syscall1(SYS_RMDIR, (long)"/vp/d") != -ENOTEMPTY) {
        fail("rmdir of a full directory is ENOTEMPTY");
    }
    if (syscall1(SYS_RMDIR, (long)"/vp/d/f") != -ENOTDIR) {
        fail("rmdir of a file is ENOTDIR");
    }
    if (syscall1(SYS_UNLINK, (long)"/vp/d") != -EISDIR) {
        fail("unlink of a directory is EISDIR");
    }

    if (syscall2(SYS_CHMOD, (long)"/vp/d/f", 0640) != 0 || xstat("/vp/d/f") != 0 ||
        (st_field(KSTAT_MODE) & 0777) != 0640) {
        fail("chmod sets the mode stat reports");
    }
    if (syscall4(SYS_UTIMENSAT, AT_FDCWD, (long)"/vp/d/f", 0, 0) != 0) {
        fail("utimensat with the current time");
    }
    {
        long ts[4];

        ts[0] = 1000000000; /* atime */
        ts[1] = 0;
        ts[2] = 1234567890; /* mtime */
        ts[3] = 0;
        if (syscall4(SYS_UTIMENSAT, AT_FDCWD, (long)"/vp/d/f", (long)ts, 0) != 0 ||
            xstat("/vp/d/f") != 0 || st_field(KSTAT_MTIME) != 1234567890u) {
            fail("utimensat sets the time stat reports");
        }
    }

    check_link(magic);
    check_blocks(magic);

    if (probe_statfs("/proc", iobuf) != 0 || *(unsigned *)iobuf != STATFS_MAGIC_PROC) {
        fail("statfs64 of /proc");
    }
    if (probe_statfs("/", iobuf) != 0 || !magic_matches(*(unsigned *)iobuf, magic)) {
        fail("statfs64 of the root");
    }
    if (magic == STATFS_MAGIC_EXT2
            ? *(unsigned *)(iobuf + KSTATFS_FILES) == 0 ||
                  *(unsigned *)(iobuf + KSTATFS_FFREE) > *(unsigned *)(iobuf + KSTATFS_FILES)
            : *(unsigned *)(iobuf + KSTATFS_FILES) != 0) {
        fail("statfs64 of the root counts its inodes");
    }

    if (syscall1(SYS_CHDIR, (long)"/vp/d") != 0) {
        fail("chdir");
    }
    if (syscall2(SYS_GETCWD, (long)cwd, sizeof(cwd)) <= 0 || cwd[0] != '/' || cwd[1] != 'v' ||
        cwd[2] != 'p' || cwd[3] != '/' || cwd[4] != 'd' || cwd[5] != 0) {
        fail("getcwd reports /vp/d");
    }
    fd = syscall3(SYS_OPEN, (long)"f", O_RDONLY, 0);
    if (fd < 0) {
        fail("a relative open in the working directory");
    }
    syscall1(SYS_CLOSE, fd);
    if (syscall1(SYS_CHDIR, (long)"/") != 0) {
        fail("chdir back to the root");
    }
    unlocked_rows(magic);

    if (syscall1(SYS_SYNC, 0) != 0) {
        fail("sync");
    }

    if (syscall1(SYS_UNLINK, (long)"/vp/d/f") != 0 || syscall1(SYS_RMDIR, (long)"/vp/d") != 0 ||
        syscall1(SYS_RMDIR, (long)"/vp") != 0) {
        fail("cleanup");
    }

    succeed();
}
