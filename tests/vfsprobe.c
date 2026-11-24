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
#define O_APPEND    02000

#define S_IFIFO 0010000
#define MS_BIND 4096

#define AT_FDCWD      (-100)
#define AT_EMPTY_PATH 0x1000

#define MNT_DETACH 2 /* umount2: detach now, free when unused */
#define WNOHANG    1
#define CLONE_FS   0x00000200
#define SIGCHLD    17

#define EPERM     1
#define ENOENT    2
#define EBADF     9
#define EIO       5
#define EACCES    13
#define EEXIST    17
#define ENOTDIR   20
#define EISDIR    21
#define EBUSY     16
#define ENOTEMPTY 39
#define ELOOP     40
#define ENXIO     6
#define ESTALE    116

#define O_NONBLOCK 04000

#define SEEK_SET 0
#define SEEK_END 2

/* inotify(7): the calls, which probe.h lacks, IN_NONBLOCK, the event of a
 * change of attributes, and the size of an event's fixed part. */
#if defined __x86_64__
#define SYS_INOTIFY_ADD_WATCH 254
#define SYS_INOTIFY_INIT1     294
#else
#define SYS_INOTIFY_ADD_WATCH 292
#define SYS_INOTIFY_INIT1     332
#endif
#define IN_NONBLOCK    04000
#define IN_ATTRIB      0x00000004
#define IN_EVENT_FIXED 16

/* The f_type statfs(2) reports for /proc, the KRFFS boot root, a KRFFS
 * volume mounted by mount(2), and ext2. */
#define STATFS_MAGIC_PROC         0x9FA0
#define STATFS_MAGIC_KRFFS        0x4B524646
#define STATFS_MAGIC_KRFFS_VOLUME 0x4B52
#define STATFS_MAGIC_EXT2         0xEF53

/* The offset of f_bfree in the buffer `probe_statfs` fills: x86-64's
 * struct statfs of longs, i386's statfs64 after two words and f_blocks. */
#if defined __x86_64__
#define KSTATFS_BFREE 24
#else
#define KSTATFS_BFREE 16
#endif

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
    {"a full walk that a chroot of a thread sharing the record came between starts again in the "
     "new root",
     "full-shared", STATFS_MAGIC_EXT2},
    {"a full walk that a chdir of a thread sharing the record came between starts again from the "
     "new directory",
     "full-shared", STATFS_MAGIC_EXT2},
    {"the walk gate counts the full walk's restart", "full-shared", STATFS_MAGIC_EXT2},
    {"a full walk that a chroot of a thread sharing the record came between at a .. starts again "
     "in the new root",
     "shared-points", STATFS_MAGIC_EXT2},
    {"a full walk that a chroot of a thread sharing the record came between at an absolute link "
     "starts again in the new root",
     "shared-points", STATFS_MAGIC_EXT2},
    {"a full walk that a chroot of a thread sharing the record came between at its end starts "
     "again in the new root",
     "shared-points", STATFS_MAGIC_EXT2},
    {"a change elsewhere while the child's walk is stopped", "shared-links", STATFS_MAGIC_EXT2},
    {"a walk of 40 links that the walk without the big lock gives to the full walk reaches its "
     "file",
     "shared-links", STATFS_MAGIC_EXT2},
    {"a walk of 41 links that the walk without the big lock gives to the full walk is ELOOP",
     "shared-links", STATFS_MAGIC_EXT2},
    {"a walk of 40 links, a jump through /proc among them, that the walk without the big lock "
     "gives to the full walk reaches its file",
     "shared-links", STATFS_MAGIC_EXT2},
    {"a walk of 41 links, a jump through /proc among them, that the walk without the big lock "
     "gives to the full walk is ELOOP",
     "shared-links", STATFS_MAGIC_EXT2},
    {"remove the files of the shared-record rows", "shared-links", STATFS_MAGIC_EXT2},
    {"a handle of an unlinked ext2 file whose inode a new file took is ESTALE", "handle-ext2",
     STATFS_MAGIC_EXT2},
    {"open_by_handle_at with O_RDONLY|O_TRUNC empties the file", "handle-ext2", STATFS_MAGIC_EXT2},
    {"open_by_handle_at of a file a FIFO replaced after its walk is ESTALE and opens no FIFO",
     "handle-ext2", STATFS_MAGIC_EXT2},
    {"remove the files of the handle rows", "handle-ext2", STATFS_MAGIC_EXT2},
    {"a file of a KRFFS root has a handle only with AT_HANDLE_FID, which opens nothing",
     "handle-krffs", STATFS_MAGIC_KRFFS},
    {"the files of the gated rows", "walk-restart", STATFS_MAGIC_EXT2},
    {"a walk that a rename came between at a lookup's sleep starts again and opens the new name",
     "walk-restart", STATFS_MAGIC_EXT2},
    {"a walk that an unmount came between at a mount's lock starts again and opens the covered "
     "name",
     "walk-restart", STATFS_MAGIC_EXT2},
    {"an unlink that a rename came between after its walk runs again and removes the new name",
     "walk-handoff", STATFS_MAGIC_EXT2},
    {"a walk without the big lock of a cached name finishes there", "walk-width",
     STATFS_MAGIC_EXT2},
    {"a walk without the big lock under which the sequence moved by 2^32 goes again under the "
     "big lock",
     "walk-width", STATFS_MAGIC_EXT2},
    {"a walk waits while one of two overlapping changes is under way and goes on once both end",
     "walk-overlap", STATFS_MAGIC_EXT2},
    {"a hand-back at the root of /proc that uses up nothing is EIO once", "walk-fault",
     STATFS_MAGIC_EXT2},
    {"a lookup that reads the disk, which a rename comes between while it loads the inode, opens "
     "the new name's file",
     "walk-cold", STATFS_MAGIC_EXT2},
    {"an O_TRUNC open that reads the disk, which a rename comes between while it loads the inode, "
     "empties the new name's file alone",
     "walk-cold", STATFS_MAGIC_EXT2},
    {"a lookup whose read of the disk fails is EIO and makes nothing, and the next walk reads the "
     "directory",
     "walk-cold-fail", STATFS_MAGIC_EXT2},
    {"an O_CREAT open through a dangling link whose read of the link fails after its first walk is "
     "EIO and makes nothing",
     "walk-link-fail", STATFS_MAGIC_EXT2},
    {"an O_CREAT open through a dangling link whose directory turns unsearchable after its first "
     "walk is EACCES and makes nothing",
     "walk-link-fail", STATFS_MAGIC_EXT2},
    {"an O_TRUNC open that a rename came between after its walk runs again and empties the new "
     "name's file alone",
     "walk-trunc", STATFS_MAGIC_EXT2},
    {"a creation that a rename came between after its walk runs again and makes the name in the "
     "new directory",
     "walk-handoffs", STATFS_MAGIC_EXT2},
    {"a mkdir that a rename came between after its walk runs again and makes the directory in the "
     "new directory",
     "walk-handoffs", STATFS_MAGIC_EXT2},
    {"a link that a rename came between after its walks runs again and links the new name's file",
     "walk-handoffs", STATFS_MAGIC_EXT2},
    {"a symlink that a rename came between after its walk runs again and makes the link in the "
     "new directory",
     "walk-handoffs", STATFS_MAGIC_EXT2},
    {"a mknod that a rename came between after its walk runs again and makes the FIFO in the new "
     "directory",
     "walk-handoffs", STATFS_MAGIC_EXT2},
    {"a rename that a rename came between after its walks runs again and renames the new name's "
     "file",
     "walk-handoffs", STATFS_MAGIC_EXT2},
    {"a chmod that a rename came between after its walk runs again and sets the new name's mode",
     "walk-handoffs", STATFS_MAGIC_EXT2},
    {"a mount that a rename came between after its walk runs again and covers the new name",
     "walk-handoffs", STATFS_MAGIC_EXT2},
    {"a bind that a rename came between after its walks runs again and covers the new name",
     "walk-handoffs", STATFS_MAGIC_EXT2},
    {"remove the files of the gated rows", "walk-handoffs", STATFS_MAGIC_EXT2},
    {"an rmdir of a directory a stopped chdir keeps is EBUSY, and the chdir enters it",
     "krffs-keep", STATFS_MAGIC_KRFFS},
    {"a keep of a file that moves follows it, and its end frees the moved file alone",
     "krffs-relocate", STATFS_MAGIC_KRFFS},
    {"a keep of a file that moves follows it, and its end after a failed call frees the moved "
     "file alone",
     "krffs-relocate", STATFS_MAGIC_KRFFS},
    {"remove the files of the keep rows", "krffs-relocate", STATFS_MAGIC_KRFFS},
    {"fchownat of the working directory by an empty path, after a rename, ends", "cwd-empty", 0},
    {"a walk follows the fixed /sys link of the PCI host bridge to its uevent", "walk-sys-link", 0},
    {"a walk climbs 1024 mounts stacked on /vp/c, 700 times over, and ends on the top one",
     "walk-climb", STATFS_MAGIC_KRFFS},
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

/* The message a failed read of the walk gate fails, which names the row
 * that reads it. */
static const char *gate_why = "read the walk gate";

/** The walk gate's count of `name`, by its line (debugfs.5), read from its
 * start, or fail `gate_why`. */
static unsigned gate_count(const char *name)
{
    char line[512];
    int n = syscall3(SYS_LSEEK, gate_rfd, 0, SEEK_SET) != 0
                ? -1
                : syscall3(SYS_READ, gate_rfd, (long)line, (int)sizeof(line) - 1);
    const char *p;
    unsigned value = 0;
    int i = 0;

    if (n <= 0) {
        fail(gate_why);
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
        fail(gate_why);
    }
    for (p += i + 2; *p >= '0' && *p <= '9'; p++) {
        value = value * 10u + (unsigned)(*p - '0');
    }
    return value;
}

/** Arm the walk gate to stop the thread `pid` at `event`, or nowhere for
 * "count", with its counts from zero, or fail `what`. */
static void gate_arm(int pid, const char *event, const char *what)
{
    char text[40] = "arm ", digits[12];
    int i = 4, k = 0;

    do {
        digits[k++] = (char)('0' + pid % 10);
        pid /= 10;
    } while (pid > 0);
    while (k > 0) {
        text[i++] = digits[--k];
    }
    text[i++] = ' ';
    for (k = 0; event[k] && i < (int)sizeof(text) - 1; k++) {
        text[i++] = event[k];
    }
    text[i] = '\0';
    gate_write(text, what);
}

/** Write `prefix` and the thread number `pid` to the walk gate, as its
 * `fault` and `ioerr` take them, or fail `what`. */
static void gate_pid_write(const char *prefix, int pid, const char *what)
{
    char text[24], digits[12];
    int i = 0, k = 0;

    while (prefix[i]) {
        text[i] = prefix[i];
        i++;
    }
    do {
        digits[k++] = (char)('0' + pid % 10);
        pid /= 10;
    } while (pid > 0);
    while (k > 0) {
        text[i++] = digits[--k];
    }
    text[i] = '\0';
    gate_write(text, what);
}

static int shared_root = -1; /* a descriptor of the root the rows began in, or -1 */
static int shared_child;     /* the child of a shared-record row, or 0 */
static int shared_pipe[2] = {-1, -1};

#define SHARED_LINKS 41 /* the links of `shared_links_row`, k1 to k41 */

/** Put in `buf` the path /vp/s/a/kN of link `n`, 1 to 99, of
 * `shared_links_row`. */
static void shared_link_name(char *buf, int n)
{
    const char *dir = "/vp/s/a/k";
    int i;

    for (i = 0; dir[i]; i++) {
        buf[i] = dir[i];
    }
    if (n >= 10) {
        buf[i++] = (char)('0' + n / 10);
    }
    buf[i++] = (char)('0' + n % 10);
    buf[i] = '\0';
}

/** Remove the files and directories of `shared_rows`, those a failure may
 * not have made among them. */
static void shared_unlink(void)
{
    char name[16];
    int i;

    for (i = 1; i <= SHARED_LINKS; i++) {
        shared_link_name(name, i);
        syscall1(SYS_UNLINK, (long)name);
    }
    syscall1(SYS_UNLINK, (long)"/vp/s/a/m39");
    syscall1(SYS_UNLINK, (long)"/vp/s/a/m40");
    syscall1(SYS_UNLINK, (long)"/vp/s/a/p40");
    syscall1(SYS_UNLINK, (long)"/vp/s/a/p41");
    syscall1(SYS_RMDIR, (long)"/vp/s/z");
    syscall1(SYS_UNLINK, (long)"/vp/s/a/l");
    syscall1(SYS_UNLINK, (long)"/vp/s/b/l");
    syscall1(SYS_UNLINK, (long)"/vp/s/b/b/f");
    syscall1(SYS_UNLINK, (long)"/vp/s/b/b/g");
    syscall1(SYS_RMDIR, (long)"/vp/s/b/b");
    syscall1(SYS_UNLINK, (long)"/vp/s/a/f");
    syscall1(SYS_UNLINK, (long)"/vp/s/b/f");
    syscall1(SYS_UNLINK, (long)"/vp/s/a/g");
    syscall1(SYS_UNLINK, (long)"/vp/s/b/g");
    syscall1(SYS_RMDIR, (long)"/vp/s/a");
    syscall1(SYS_RMDIR, (long)"/vp/s/b");
    syscall1(SYS_RMDIR, (long)"/vp/s");
}

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
    shared_unlink();
}

/** Make the file `path` holding the `n` bytes of `text`, or fail `what`. */
static void shared_file(const char *path, const char *text, int n, const char *what)
{
    int fd = syscall3(SYS_OPEN, (long)path, O_WRONLY | O_CREAT | O_TRUNC, 0644);

    if (fd < 0 || syscall3(SYS_WRITE, fd, (long)text, n) != n) {
        fail(what);
    }
    syscall1(SYS_CLOSE, fd);
}

static unsigned shared_ino;     /* the inode the child must open */
static const char *shared_path; /* the path the child opens */
static int shared_cached = -1;  /* the links the walk without the big lock
                                   must follow, or -1 for any */

/** The child of a shared-record row: on its go from `in`, open
 * `shared_path` relative to the working directory, with O_CREAT when
 * `full`, and exit with the count it reads, 97 for ELOOP, 98 for a file
 * other than `shared_ino`, or 100 and more. */
static void shared_child_run(int in, int full)
{
    char byte, got[8];
    int fd;

    if (syscall3(SYS_READ, in, (long)&byte, 1) != 1) {
        sys_exit(100);
    }
    fd = syscall3(SYS_OPEN, (long)shared_path, full ? O_RDONLY | O_CREAT : O_RDONLY, 0644);
    if (fd == -ELOOP) {
        sys_exit(97);
    }
    if (fd < 0) {
        sys_exit(101);
    }
    if (syscall2(SYS_FSTAT, fd, (long)stbuf) != 0 || st_field(KSTAT_INO) != shared_ino) {
        sys_exit(98);
    }
    sys_exit(syscall3(SYS_READ, fd, (long)got, (int)sizeof(got)));
}

/* What the parent of a shared-record row does while the child's walk is
 * stopped (`shared_run`). */
#define SHARED_CHROOT 0 /* chroots to /vp/s/b, which moves the shared cwd */
#define SHARED_CHDIR  1 /* changes the shared cwd to /vp/s/b */
#define SHARED_TOUCH  2 /* makes and removes /vp/s/z, elsewhere */

/** Make the change `change` of a shared-record row: 0, or -1. */
static int shared_change(int change)
{
    if (change == SHARED_CHROOT) {
        return syscall1(SYS_CHROOT, (long)"/vp/s/b") == 0 ? 0 : -1;
    }
    if (change == SHARED_CHDIR) {
        return syscall1(SYS_CHDIR, (long)"/vp/s/b") == 0 ? 0 : -1;
    }
    return syscall2(SYS_MKDIR, (long)"/vp/s/z", 0755) == 0 &&
                   syscall1(SYS_RMDIR, (long)"/vp/s/z") == 0
               ? 0
               : -1;
}

/**
 * Run one shared-record row: a child that shares the working directory and
 * the root (CLONE_FS) opens `shared_path` from the working directory
 * /vp/s/a, with O_CREAT when `full`, which takes the full walk when the walk
 * without the big lock finds no file there; the walk gate stops the child
 * at `event`; the parent makes the change `change`
 * (`shared_change`) and lets the walk go on. The child must exit with
 * `want`, a count of bytes it read from `shared_ino` or 97 for ELOOP, and
 * the gate must count the full walk's restart, for `count` "restart", or
 * the one walk without the big lock given up, for "miss", and the links
 * that walk followed, when `shared_cached` is not -1; else the row fails
 * `why`. Returns with the root and the working directory /vp/s/a as before.
 */
static void shared_run(const char *event, int full, int change, int want, const char *count,
                       const char *why)
{
    char byte = 0;
    int pid, st;
    unsigned n;

    if (syscall1(SYS_PIPE, (long)shared_pipe) != 0) {
        fail("the child of the shared-record rows");
    }
    pid = syscall5(SYS_CLONE, CLONE_FS | SIGCHLD, 0, 0, 0, 0);
    if (pid == 0) {
        shared_child_run(shared_pipe[0], full);
    }
    if (pid < 0) {
        fail("the child of the shared-record rows");
    }
    shared_child = pid;
    gate_arm(pid, event, "arm the walk gate");
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
    if (shared_change(change) != 0) {
        fail(change == SHARED_CHDIR    ? "chdir while the child's walk is stopped"
             : change == SHARED_CHROOT ? "chroot while the child's walk is stopped"
                                       : "a change elsewhere while the child's walk is stopped");
    }
    gate_write("release", "release the walk gate");
    if (syscall4(SYS_WAIT4, pid, (long)&st, 0, 0) != pid) {
        fail(why);
    }
    shared_child = 0;
    if ((st & 0x7f) != 0 || ((st >> 8) & 0xff) != want) {
        fail(why);
    }
    n = gate_count(count);
    if (count[0] == 'r' ? n == 0 : n != 1) {
        fail(count[0] == 'r' ? "the walk gate counts the full walk's restart"
                             : "the walk gate counts the one walk given up");
    }
    if (shared_cached >= 0 && gate_count("cached") != (unsigned)shared_cached) {
        fail(why);
    }
    gate_write("off", "stop the walk gate");
    syscall1(SYS_CLOSE, shared_pipe[0]);
    syscall1(SYS_CLOSE, shared_pipe[1]);
    shared_pipe[0] = shared_pipe[1] = -1;
    if (change == SHARED_CHROOT &&
        (syscall1(SYS_FCHDIR, shared_root) != 0 || syscall1(SYS_CHROOT, (long)".") != 0)) {
        fail("back to the old root after the shared-record chroot");
    }
    if (syscall1(SYS_CHDIR, (long)"/vp/s/a") != 0) {
        fail("back to the old root after the shared-record chroot");
    }
}

/**
 * One shared-record row: the walk gate stops the child's walk of f where it
 * has taken its start and the walk sequence, the walk without the big lock
 * (`unlocked`), or with `full` the O_CREAT walk of g, which only /vp/s/b
 * holds, so that the walk without the big lock hands it to the full walk
 * under it (`full`); the parent chroots to /vp/s/b, which moves the shared
 * working directory there too, or with `cd` changes to it. The walk goes
 * again from the new directory, whose f or g holds 3 bytes and is the file
 * the child must open, by its inode: under the big lock, and the gate
 * counts that one walk given up, or for the full walk from its start, and
 * the gate counts the restart. Read past the change, the old directory's
 * pair would give f's 2 bytes, or make an empty g of another inode.
 */
static void shared_row(int cd, int full)
{
    static const char *const whys[4] = {
        "a walk without the big lock that a chroot of a thread sharing the record came between "
        "goes again in the new root",
        "a walk without the big lock that a chdir of a thread sharing the record came between "
        "goes again from the new directory",
        "a full walk that a chroot of a thread sharing the record came between starts again in "
        "the new root",
        "a full walk that a chdir of a thread sharing the record came between starts again from "
        "the new directory"};

    shared_path = full ? "g" : "f";
    if (xstat(full ? "/vp/s/b/g" : "/vp/s/b/f") != 0) {
        fail("the files of the shared-record rows");
    }
    shared_ino = st_field(KSTAT_INO);
    shared_run(full ? "full" : "unlocked", full, cd ? SHARED_CHDIR : SHARED_CHROOT, 3,
               full ? "restart" : "miss", whys[full * 2 + cd]);
}

/**
 * The shared-record rows at the later points of the full walk: a chroot to
 * /vp/s/b by the thread sharing the record, while the walk gate holds the
 * child's O_CREAT walk at a `..` (`dotdot`), of ../b/f, after it has read
 * the absolute link l (`follow`), and at its end before its last check
 * (`done`), of g, sends the walk round from its start, in the new root and
 * from the new working directory, its root: ../b/f and l, which is
 * /vp/s/b/l there, a link to /b/f, then reach /vp/s/b/b/f, 4 bytes, and g
 * reaches /vp/s/b/g, 3 bytes. Each is a full walk: a path that begins with
 * `..` never takes the walk without the big lock, and that walk finds no
 * file at /vp/s/a/l's target /vp/s/b/b/g or at /vp/s/a/g, which hands an
 * O_CREAT open to the full walk. Read on from the old root and directory,
 * the first would reach /vp/s/b/f, 3 bytes, and the others would make an
 * empty /vp/s/b/b/g and /vp/s/a/g.
 */
static void shared_point_rows(void)
{
    unsigned bb;

    if (syscall2(SYS_MKDIR, (long)"/vp/s/b/b", 0755) != 0 ||
        syscall2(SYS_SYMLINK, (long)"/vp/s/b/b/g", (long)"/vp/s/a/l") != 0 ||
        syscall2(SYS_SYMLINK, (long)"/b/f", (long)"/vp/s/b/l") != 0) {
        fail("the files of the shared-record rows");
    }
    shared_file("/vp/s/b/b/f", "bbbb", 4, "the files of the shared-record rows");
    if (xstat("/vp/s/b/b/f") != 0) {
        fail("the files of the shared-record rows");
    }
    bb = st_field(KSTAT_INO);
    shared_ino = bb;
    shared_path = "../b/f";
    shared_run("dotdot", 1, SHARED_CHROOT, 4, "restart",
               "a full walk that a chroot of a thread sharing the record came between at a .. "
               "starts again in the new root");
    shared_path = "l";
    shared_run("follow", 1, SHARED_CHROOT, 4, "restart",
               "a full walk that a chroot of a thread sharing the record came between at an "
               "absolute link starts again in the new root");
    if (xstat("/vp/s/b/g") != 0) {
        fail("the files of the shared-record rows");
    }
    shared_ino = st_field(KSTAT_INO);
    shared_path = "g";
    shared_run("done", 1, SHARED_CHROOT, 3, "restart",
               "a full walk that a chroot of a thread sharing the record came between at its end "
               "starts again in the new root");
}

/** Make the link `path` to `target`, in the working directory /vp/s/a, and
 * walk it once, so that the names and targets of its chain are in memory
 * for the walk without the big lock, or fail. */
static void shared_link_warm(const char *target, const char *path)
{
    long fd;

    if (target && syscall2(SYS_SYMLINK, (long)target, (long)path) != 0) {
        fail("the files of the shared-record rows");
    }
    fd = syscall3(SYS_OPEN, (long)path, O_RDONLY, 0);
    if (fd >= 0) {
        syscall1(SYS_CLOSE, fd);
    } else if (fd != -ELOOP) {
        fail("the files of the shared-record rows");
    }
}

/** One row of `shared_links_row`: the child opens `path`, the walk gate
 * stops it once the walk without the big lock has followed the chain's
 * first link, and the parent makes and removes /vp/s/z; the walk must end
 * with `want` (`shared_run`) once it has followed `cached` links there. */
static void shared_links_run(const char *path, int cached, int want, const char *why)
{
    shared_path = path;
    shared_cached = cached;
    shared_run("cached", 0, SHARED_TOUCH, want, "miss", why);
    shared_cached = -1;
}

/**
 * A walk of a chain of links that the walk without the big lock begins and
 * the full walk ends has 40 links in all, not 40 in each nor 40 after the
 * hand-over: k1 to k41 in /vp/s/a, each a link to the next and k41 to f; m39
 * to /proc/self/cwd/k6 and m40 to /proc/self/cwd/k5, jumps through /proc,
 * whose links only the full walk takes; and p40 to m39, p41 to m40. Each
 * chain is walked once first, so that its names and targets are in memory.
 * The walk gate stops the child's walk of k2, k1, p40 and p41 once the walk
 * without the big lock has followed the first link (`cached`), while the
 * parent makes and removes /vp/s/z elsewhere; that walk goes on, follows
 * the links it can, and gives the path to the full walk: at its end, since
 * the sequence moved, after 40 links for k2; at the 41st for k1; and at
 * /proc after p40 or p41 and the m link for the p chains. k2 reaches f's 2
 * bytes through 40 links, and p40 through p40, m39, /proc/self, the jump
 * through cwd and 36 more; k1 and p41, a link more each, are ELOOP.
 */
static void shared_links_row(void)
{
    char name[16], target[16];
    int i;

    for (i = 1; i <= SHARED_LINKS; i++) {
        shared_link_name(name, i);
        shared_link_name(target, i + 1);
        if (syscall2(SYS_SYMLINK, (long)(i < SHARED_LINKS ? target + 8 : "f"), (long)name) != 0) {
            fail("the files of the shared-record rows");
        }
    }
    if (xstat("/vp/s/a/f") != 0) {
        fail("the files of the shared-record rows");
    }
    shared_ino = st_field(KSTAT_INO);
    shared_link_warm(0, "k2");
    shared_link_warm(0, "k1");
    shared_link_warm("/proc/self/cwd/k6", "m39");
    shared_link_warm("/proc/self/cwd/k5", "m40");
    shared_link_warm("m39", "p40");
    shared_link_warm("m40", "p41");
    shared_links_run("k2", 40, 2,
                     "a walk of 40 links that the walk without the big lock gives to the full walk "
                     "reaches its file");
    shared_links_run("k1", 40, 97,
                     "a walk of 41 links that the walk without the big lock gives to the full walk "
                     "is ELOOP");
    shared_links_run("p40", 2, 2,
                     "a walk of 40 links, a jump through /proc among them, that the walk without "
                     "the big lock gives to the full walk reaches its file");
    shared_links_run("p41", 2, 97,
                     "a walk of 41 links, a jump through /proc among them, that the walk without "
                     "the big lock gives to the full walk is ELOOP");
}

/**
 * On an ext2 root, whose names a walk without the big lock finishes, a chroot
 * and a chdir by a thread that shares the record (CLONE_FS) between the
 * start of such a walk and its end send the walk again, under the big lock,
 * and the full walk again from its start (`shared_row`), as Linux's walk
 * restarts when `fs->seq` or the mount sequence moved (fs/namei.c
 * `try_to_unlazy` in 6.12). A root of another
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
    shared_file("/vp/s/a/f", "aa", 2, "the files of the shared-record rows");
    shared_file("/vp/s/b/f", "bbb", 3, "the files of the shared-record rows");
    shared_file("/vp/s/b/g", "ggg", 3, "the files of the shared-record rows");
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
    shared_row(0, 0);
    shared_row(1, 0);
    shared_row(0, 1);
    shared_row(1, 1);
    shared_point_rows();
    shared_links_row();
    syscall1(SYS_CLOSE, shared_root);
    syscall1(SYS_CLOSE, gate_wfd);
    syscall1(SYS_CLOSE, gate_rfd);
    shared_root = gate_wfd = gate_rfd = -1;
    if (syscall1(SYS_CHDIR, (long)"/") != 0) {
        fail("remove the files of the shared-record rows");
    }
    shared_unlink();
    if (xstat("/vp/s") != -ENOENT) {
        fail("remove the files of the shared-record rows");
    }
    fail_undo = 0;
}

/* name_to_handle_at(2) and open_by_handle_at(2), which probe.h lacks. */
#if defined __x86_64__
#define SYS_NAME_TO_HANDLE_AT 303
#define SYS_OPEN_BY_HANDLE_AT 304
#else
#define SYS_NAME_TO_HANDLE_AT 341
#define SYS_OPEN_BY_HANDLE_AT 342
#endif

#define AT_HANDLE_FID 0x200 /* name_to_handle_at: a handle that opens nothing */
#define EOPNOTSUPP    95
#define HANDLE_FILES  200 /* the files `handle_reuse` may make to reach an inode */

/* A file handle as Rach2 gives one: the size, the type, and twelve bytes. */
struct fhandle {
    unsigned bytes;
    int type;
    unsigned char f[12];
};

static int handle_child; /* the child of `handle_fifo_row`, or 0 */
static int handle_made;  /* the files /vp/h/nN `handle_reuse` made */

/** Put in `buf` the path /vp/h/nN of file `n`, 0 to 999. */
static void handle_name(char *buf, int n)
{
    const char *dir = "/vp/h/n";
    int i;

    for (i = 0; dir[i]; i++) {
        buf[i] = dir[i];
    }
    buf[i++] = (char)('0' + n / 100);
    buf[i++] = (char)('0' + n / 10 % 10);
    buf[i++] = (char)('0' + n % 10);
    buf[i] = '\0';
}

/** Undo what `handle_rows` made, for `fail` and their own end: the gate,
 * the child, the files and /vp/h. */
static void handle_undo(void)
{
    char name[16];
    int st;

    if (gate_wfd >= 0) {
        syscall3(SYS_WRITE, gate_wfd, (long)"off", 3);
    }
    if (handle_child > 0) {
        syscall2(SYS_KILL, handle_child, 9);
        syscall4(SYS_WAIT4, handle_child, (long)&st, 0, 0);
        handle_child = 0;
    }
    syscall1(SYS_CLOSE, gate_wfd);
    syscall1(SYS_CLOSE, gate_rfd);
    gate_wfd = gate_rfd = -1;
    while (handle_made > 0) {
        handle_name(name, --handle_made);
        syscall1(SYS_UNLINK, (long)name);
    }
    syscall1(SYS_UNLINK, (long)"/vp/h/f");
    syscall1(SYS_UNLINK, (long)"/vp/h/g");
    syscall1(SYS_UNLINK, (long)"/vp/h/p");
    syscall1(SYS_RMDIR, (long)"/vp/h");
}

/** The handle of `path` into `h`, of Linux's FILEID_INO64_GEN, or fail
 * `why`. */
static void handle_of(const char *path, struct fhandle *h, const char *why)
{
    int mnt;

    h->bytes = sizeof(h->f);
    if (syscall5(SYS_NAME_TO_HANDLE_AT, AT_FDCWD, (long)path, (long)h, (long)&mnt, 0) != 0 ||
        h->type != 0x51) {
        fail(why);
    }
}

/**
 * Make files in /vp/h until one takes the inode `ino`, which an unlink has
 * freed, and rename that one to /vp/h/g holding "ggg": ext2 gives a new
 * file the lowest free inode, so one of the first HANDLE_FILES does. Fail
 * `why` when none does.
 */
static void handle_reuse(unsigned ino, const char *why)
{
    char name[16];

    while (handle_made < HANDLE_FILES) {
        handle_name(name, handle_made);
        shared_file(name, "ggg", 3, why);
        handle_made++;
        if (xstat(name) != 0) {
            fail(why);
        }
        if (st_field(KSTAT_INO) == ino) {
            if (syscall2(SYS_RENAME, (long)name, (long)"/vp/h/g") != 0) {
                fail(why);
            }
            return;
        }
    }
    fail(why);
}

/**
 * A FIFO renamed over a handle's file after open_by_handle_at's walk found
 * the file is never opened: the walk gate stops the child's O_RDONLY|O_TRUNC
 * open of /vp/h/g by the handle `hg` where the open has its walk's answer
 * (`handoff`), the parent renames the FIFO /vp/h/p over /vp/h/g and lets it
 * go, and the open, whose truncation finds the tree changed and walks
 * again, answers ESTALE for the FIFO before it opens it. A child that
 * opened the FIFO would wait there for a writer, which the parent's
 * nonblocking open for writing would then find, where it otherwise gets
 * ENXIO.
 */
static void handle_fifo_row(struct fhandle *hg)
{
    const char *why = "open_by_handle_at of a file a FIFO replaced after its walk is ESTALE "
                      "and opens no FIFO";
    char byte = 0;
    long fd;
    int pid, st;

    if (syscall3(SYS_MKNOD, (long)"/vp/h/p", S_IFIFO | 0644, 0) != 0 ||
        syscall1(SYS_PIPE, (long)shared_pipe) != 0) {
        fail(why);
    }
    pid = syscall5(SYS_CLONE, SIGCHLD, 0, 0, 0, 0);
    if (pid == 0) {
        if (syscall3(SYS_READ, shared_pipe[0], (long)&byte, 1) != 1) {
            sys_exit(100);
        }
        fd = syscall3(SYS_OPEN_BY_HANDLE_AT, AT_FDCWD, (long)hg, O_RDONLY | O_TRUNC);
        sys_exit(fd == -ESTALE ? 0 : fd >= 0 ? 1 : 2);
    }
    if (pid < 0) {
        fail(why);
    }
    handle_child = pid;
    gate_arm(pid, "handoff", why);
    if (syscall3(SYS_WRITE, shared_pipe[1], (long)&byte, 1) != 1) {
        fail(why);
    }
    while (gate_count("waiting") != 1) {
        if (syscall4(SYS_WAIT4, pid, (long)&st, WNOHANG, 0) == pid) {
            handle_child = 0;
            fail(why);
        }
        syscall0(SYS_SCHED_YIELD);
    }
    if (syscall2(SYS_RENAME, (long)"/vp/h/p", (long)"/vp/h/g") != 0) {
        fail(why);
    }
    gate_write("release", why);
    while (syscall4(SYS_WAIT4, pid, (long)&st, WNOHANG, 0) != pid) {
        fd = syscall3(SYS_OPEN, (long)"/vp/h/g", O_WRONLY | O_NONBLOCK, 0);
        if (fd >= 0) {
            syscall1(SYS_CLOSE, fd);
            fail(why);
        }
        if (fd != -ENXIO) {
            fail(why);
        }
        syscall0(SYS_SCHED_YIELD);
    }
    handle_child = 0;
    if ((st & 0x7f) != 0 || ((st >> 8) & 0xff) != 0) {
        fail(why);
    }
    gate_write("off", why);
    syscall1(SYS_CLOSE, shared_pipe[0]);
    syscall1(SYS_CLOSE, shared_pipe[1]);
    shared_pipe[0] = shared_pipe[1] = -1;
}

/**
 * On a KRFFS root, whose inode numbers a file made again under a name takes
 * over, a file's handle is EOPNOTSUPP without AT_HANDLE_FID, and the handle
 * AT_HANDLE_FID gives is ESTALE to open_by_handle_at.
 */
static void handle_krffs_row(void)
{
    const char *why = "a file of a KRFFS root has a handle only with AT_HANDLE_FID, which opens "
                      "nothing";
    struct fhandle h;
    int mnt;

    h.bytes = sizeof(h.f);
    if (syscall5(SYS_NAME_TO_HANDLE_AT, AT_FDCWD, (long)"/vp", (long)&h, (long)&mnt, 0) !=
        -EOPNOTSUPP) {
        fail(why);
    }
    h.bytes = sizeof(h.f);
    if (syscall5(SYS_NAME_TO_HANDLE_AT, AT_FDCWD, (long)"/vp", (long)&h, (long)&mnt,
                 AT_HANDLE_FID) != 0 ||
        syscall3(SYS_OPEN_BY_HANDLE_AT, AT_FDCWD, (long)&h, O_RDONLY) != -ESTALE) {
        fail(why);
    }
}

/**
 * On an ext2 root, whose inodes carry a generation, a handle names one file
 * and no other: the handle of /vp/h/f, once f is unlinked and a new file
 * takes its inode (`handle_reuse`), is ESTALE, while the new file's own
 * handle opens it; an O_RDONLY|O_TRUNC open by that handle empties it, as
 * open(2) would; and a FIFO renamed over it during the open is never opened
 * (`handle_fifo_row`). A KRFFS root has `handle_krffs_row` instead.
 */
static void handle_rows(unsigned magic)
{
    const char *gen = "a handle of an unlinked ext2 file whose inode a new file took is ESTALE";
    const char *trunc = "open_by_handle_at with O_RDONLY|O_TRUNC empties the file";
    struct fhandle hf, hg;
    unsigned ino;
    long fd;

    if (magic == STATFS_MAGIC_KRFFS) {
        handle_krffs_row();
    }
    if (magic != STATFS_MAGIC_EXT2) {
        return;
    }
    fail_undo = handle_undo;
    if (syscall2(SYS_MKDIR, (long)"/vp/h", 0755) != 0) {
        fail(gen);
    }
    shared_file("/vp/h/f", "ff", 2, gen);
    if (xstat("/vp/h/f") != 0) {
        fail(gen);
    }
    ino = st_field(KSTAT_INO);
    handle_of("/vp/h/f", &hf, gen);
    if (syscall1(SYS_UNLINK, (long)"/vp/h/f") != 0) {
        fail(gen);
    }
    handle_reuse(ino, gen);
    handle_of("/vp/h/g", &hg, gen);
    if (syscall3(SYS_OPEN_BY_HANDLE_AT, AT_FDCWD, (long)&hf, O_RDONLY) != -ESTALE) {
        fail(gen);
    }
    fd = syscall3(SYS_OPEN_BY_HANDLE_AT, AT_FDCWD, (long)&hg, O_RDONLY | O_TRUNC);
    if (fd < 0 || syscall2(SYS_FSTAT, fd, (long)stbuf) != 0 || st_field(KSTAT_INO) != ino ||
        st_field(KSTAT_SIZE_AT) != 0 || syscall1(SYS_CLOSE, fd) != 0) {
        fail(trunc);
    }
    gate_wfd = syscall3(SYS_OPEN, (long)WALK_GATE, O_WRONLY, 0);
    gate_rfd = syscall3(SYS_OPEN, (long)WALK_GATE, O_RDONLY, 0);
    if (gate_wfd < 0 || gate_rfd < 0) {
        fail("read the walk gate");
    }
    handle_fifo_row(&hg);
    handle_undo();
    if (xstat("/vp/h") != -ENOENT) {
        fail("remove the files of the handle rows");
    }
    fail_undo = 0;
}

/*
 * The gated rows run in /vp/g, which holds x/f ("xx"), y/f ("yyy") and m/f
 * ("mmm"), with a child rooted there whose walk the walk gate stops. Each
 * row's failure fails `gated_why`, its own message, and prints the step
 * that failed.
 */
static const char *gated_why;
static int gated_mounted; /* whether the tmpfs on /vp/g/m is mounted */

/* What the child of a gated row does with its path (`gated_child`), the
 * second path being `gated_path2`. */
#define GATED_CREATE  0  /* opens it with O_CREAT and reads it */
#define GATED_READ    1  /* opens it read-only and reads it */
#define GATED_UNLINK  2  /* unlinks it */
#define GATED_TRUNC   3  /* opens it with O_TRUNC */
#define GATED_EXCL    4  /* makes it with O_CREAT and O_EXCL */
#define GATED_MKDIR   5  /* makes it a directory */
#define GATED_LINK    6  /* links it to the second path */
#define GATED_SYMLINK 7  /* makes it a symbolic link to the second path */
#define GATED_MKNOD   8  /* makes it a FIFO */
#define GATED_RENAME  9  /* renames it to the second path */
#define GATED_CHMOD   10 /* sets its mode to 0600 */
#define GATED_MOUNT   11 /* mounts a tmpfs over it */
#define GATED_BIND    12 /* binds the second path over it */
#define GATED_USER    13 /* opens it with O_CREAT as uid 65534 */

static const char *gated_path2; /* the second path of a gated child */
static unsigned gated_ino;      /* the inode a child's open must reach, or 0 */
static const char *gated_text;  /* the bytes a child's open must read, or 0 */
static int gated_ioerr;         /* whether `gated_start` arms a failed read */

/** Print the step `what` of a gated row, and fail the row. */
static void gated_fail(const char *what)
{
    syscall3(SYS_WRITE, 1, (long)"vfsprobe: gated row: ", 21);
    puts(what);
    fail(gated_why);
}

/* The names the handoff rows make in x or y, which `gated_undo_made`
 * removes, and the directory d the mount rows mount over. */
static const char *const gated_made[] = {"c", "l", "s", "p", "r", "n"};

/** Remove from the directory `dir`, a path ending in a slash, whatever
 * the gated rows may have left in it: the mounts on its d, d itself, the
 * names of `gated_made`, and f once r is gone, for `gated_undo`. */
static void gated_undo_made(const char *dir)
{
    char path[32];
    int n = (int)strlen(dir), i, k;

    for (k = 0; k < n; k++) {
        path[k] = dir[k];
    }
    path[n] = 'd';
    path[n + 1] = '\0';
    syscall2(SYS_UMOUNT2, (long)path, MNT_DETACH);
    syscall2(SYS_UMOUNT2, (long)path, MNT_DETACH);
    syscall1(SYS_RMDIR, (long)path);
    for (i = 0; i < (int)(sizeof(gated_made) / sizeof(gated_made[0])); i++) {
        path[n] = gated_made[i][0];
        syscall1(SYS_UNLINK, (long)path);
    }
}

/** Undo what `gated_rows` made, for `fail`: the gate and the child, as
 * `shared_undo` does, the mounts and names the rows made, the tmpfs on
 * /vp/g/m, and the files, wherever a partial swap left them. */
static void gated_undo(void)
{
    shared_undo();
    if (gated_mounted) {
        syscall2(SYS_UMOUNT2, (long)"/vp/g/m", MNT_DETACH);
        gated_mounted = 0;
    }
    syscall1(SYS_UNLINK, (long)"/vp/g/x/f");
    syscall1(SYS_UNLINK, (long)"/vp/g/y/f");
    syscall1(SYS_UNLINK, (long)"/vp/g/m/f");
    syscall1(SYS_UNLINK, (long)"/vp/g/t/f");
    syscall1(SYS_UNLINK, (long)"/vp/g/l");
    syscall1(SYS_UNLINK, (long)"/vp/g/q/l");
    syscall1(SYS_RMDIR, (long)"/vp/g/q");
    gated_undo_made("/vp/g/x/");
    gated_undo_made("/vp/g/y/");
    gated_undo_made("/vp/g/t/");
    syscall1(SYS_RMDIR, (long)"/vp/g/t");
    syscall1(SYS_RMDIR, (long)"/vp/g/x");
    syscall1(SYS_RMDIR, (long)"/vp/g/y");
    syscall1(SYS_RMDIR, (long)"/vp/g/m");
    syscall1(SYS_RMDIR, (long)"/vp/g");
    gate_why = "read the walk gate";
}

/** The change of a gated child that is not an open: `how` on `path` and
 * `gated_path2`. 0, or the negated errno. */
static int gated_change(const char *path, int how)
{
    switch (how) {
    case GATED_UNLINK:
        return syscall1(SYS_UNLINK, (long)path);
    case GATED_MKDIR:
        return syscall2(SYS_MKDIR, (long)path, 0755);
    case GATED_LINK:
        return syscall2(SYS_LINK, (long)path, (long)gated_path2);
    case GATED_SYMLINK:
        return syscall2(SYS_SYMLINK, (long)gated_path2, (long)path);
    case GATED_MKNOD:
        return syscall3(SYS_MKNOD, (long)path, S_IFIFO | 0644, 0);
    case GATED_RENAME:
        return syscall2(SYS_RENAME, (long)path, (long)gated_path2);
    case GATED_CHMOD:
        return syscall2(SYS_CHMOD, (long)path, 0600);
    case GATED_MOUNT:
        return syscall5(SYS_MOUNT, (long)"none", (long)path, (long)"tmpfs", 0, 0);
    default:
        return syscall5(SYS_MOUNT, (long)gated_path2, (long)path, 0, MS_BIND, 0);
    }
}

/**
 * The child of a gated row: on its go from `in`, use `path` as `how` says.
 * An open checks that it reached the inode `gated_ino` and read the bytes
 * of `gated_text`, when they are set. The bytes it reads, 0 for a change or
 * an O_TRUNC open or one as uid 65534, 97 when it cannot become that uid,
 * 98 for another inode, 99 for other bytes, or 100 and the errno.
 */
static int gated_child(int in, const char *path, int how)
{
    static const int opens[] = {O_RDONLY | O_CREAT, O_RDONLY, 0, O_WRONLY | O_TRUNC,
                                O_WRONLY | O_CREAT | O_EXCL};
    char byte, got[8];
    int fd, n, i;

    if (syscall3(SYS_READ, in, (long)&byte, 1) != 1) {
        return 100;
    }
    if (how == GATED_USER) {
        if (syscall3(SYS_SETRESUID, 65534, 65534, 65534) != 0) {
            return 97;
        }
        fd = syscall3(SYS_OPEN, (long)path, O_RDONLY | O_CREAT, 0644);
        return fd < 0 ? 100 - fd : 0;
    }
    if (how == GATED_UNLINK || how > GATED_EXCL) {
        fd = gated_change(path, how);
        return fd < 0 ? 100 - fd : 0;
    }
    fd = syscall3(SYS_OPEN, (long)path, opens[how], 0644);
    if (fd < 0) {
        return 100 - fd;
    }
    if (gated_ino &&
        (syscall2(SYS_FSTAT, fd, (long)stbuf) != 0 || st_field(KSTAT_INO) != gated_ino)) {
        return 98;
    }
    if (how == GATED_TRUNC || how == GATED_EXCL) {
        return 0;
    }
    n = syscall3(SYS_READ, fd, (long)got, (int)sizeof(got));
    for (i = 0; gated_text && i < n; i++) {
        if (got[i] != gated_text[i]) {
            return 99;
        }
    }
    return gated_text && gated_text[n > 0 ? n : 0] != '\0' ? 99 : n;
}

/**
 * Start the child of the gated row `why`, rooted at /vp/g with its working
 * directory there, to use `path` as `how` says; arm the walk gate to stop it
 * at `event`, or nowhere for "count"; begin `tokens` changes the gate holds;
 * and give the child its go. With a stop, wait until the child sleeps there.
 * The child's PID, with the probe back at its own root.
 */
static int gated_start(const char *why, const char *path, int how, const char *event, int tokens)
{
    char byte = 0;
    int pid, st;

    gated_why = gate_why = why;
    if (syscall1(SYS_PIPE, (long)shared_pipe) != 0 || syscall1(SYS_CHROOT, (long)"/vp/g") != 0 ||
        syscall1(SYS_CHDIR, (long)"/") != 0) {
        gated_fail("the child's root");
    }
    pid = syscall5(SYS_CLONE, SIGCHLD, 0, 0, 0, 0);
    if (pid == 0) {
        sys_exit(gated_child(shared_pipe[0], path, how));
    }
    shared_child = pid > 0 ? pid : 0;
    if (syscall1(SYS_FCHDIR, shared_root) != 0 || syscall1(SYS_CHROOT, (long)".") != 0 || pid < 0) {
        gated_fail("the child, and back to the probe's root");
    }
    gate_arm(pid, event, why);
    if (gated_ioerr) {
        gate_pid_write("ioerr ", pid, why);
    }
    while (tokens-- > 0) {
        gate_write("begin", why);
    }
    if (syscall3(SYS_WRITE, shared_pipe[1], (long)&byte, 1) != 1) {
        gated_fail("the child's go");
    }
    while (!same(event, "count") && gate_count("waiting") != 1) {
        if (syscall4(SYS_WAIT4, pid, (long)&st, WNOHANG, 0) == pid) {
            shared_child = 0;
            gated_fail("the child's walk stops at the walk gate");
        }
        syscall0(SYS_SCHED_YIELD);
    }
    return pid;
}

/** Let the child of a gated row go on and reap it: its exit status, or -1
 * when a signal ended it. */
static int gated_end(int pid)
{
    int st;

    gate_write("release", gated_why);
    if (syscall4(SYS_WAIT4, pid, (long)&st, 0, 0) != pid) {
        gated_fail("the child is reaped");
    }
    shared_child = 0;
    syscall1(SYS_CLOSE, shared_pipe[0]);
    syscall1(SYS_CLOSE, shared_pipe[1]);
    shared_pipe[0] = shared_pipe[1] = -1;
    return (st & 0x7f) != 0 ? -1 : (st >> 8) & 0xff;
}

/** Swap the directories /vp/g/x and /vp/g/y by way of /vp/g/t, while a
 * child's walk is stopped. */
static void gated_swap(void)
{
    if (syscall2(SYS_RENAME, (long)"/vp/g/x", (long)"/vp/g/t") != 0 ||
        syscall2(SYS_RENAME, (long)"/vp/g/y", (long)"/vp/g/x") != 0 ||
        syscall2(SYS_RENAME, (long)"/vp/g/t", (long)"/vp/g/y") != 0) {
        gated_fail("swap x and y while the child's walk is stopped");
    }
}

/**
 * A full walk that a change comes between at a sleep starts again from its
 * root, as Linux's does when its sequences fail (fs/namei.c `try_to_unlazy`
 * in 6.12). The gate stops a child's O_CREAT open of x/f where the lookup of
 * x sleeps, as a lookup of ext2 does when it reads the directory from the
 * disk, and the parent swaps x and y: the walk goes again and reads the 3
 * bytes of the new x's f, where going on in the directory it held would read
 * the 2 of the old. Then the gate stops an O_CREAT open of m/f, with a
 * tmpfs over m, where the walk has entered the tmpfs and waits for its
 * namespace lock, and the parent detaches the tmpfs: the walk goes again and
 * opens the 3 bytes of the f the tmpfs covered, where going on would make an
 * empty f in the detached tmpfs. The gate counts the restarts.
 */
static void gated_restart_rows(void)
{
    int pid = gated_start("a walk that a rename came between at a lookup's sleep starts again and "
                          "opens the new name",
                          "x/f", GATED_CREATE, "lookup", 0);

    gated_swap();
    if (gated_end(pid) != 3 || gate_count("restart") == 0) {
        fail(gated_why);
    }
    gated_why = "a walk that an unmount came between at a mount's lock starts again and opens "
                "the covered name";
    if (syscall5(SYS_MOUNT, (long)"none", (long)"/vp/g/m", (long)"tmpfs", 0, 0) != 0) {
        gated_fail("mount a tmpfs over /vp/g/m");
    }
    gated_mounted = 1;
    pid = gated_start(gated_why, "m/f", GATED_CREATE, "nslock", 0);
    if (syscall2(SYS_UMOUNT2, (long)"/vp/g/m", MNT_DETACH) != 0) {
        gated_fail("detach the tmpfs while the child's walk is stopped");
    }
    gated_mounted = 0;
    if (gated_end(pid) != 3 || gate_count("restart") == 0) {
        fail(gated_why);
    }
    gate_write("off", gated_why);
}

/**
 * A change that a rename comes between after its walk, while it takes its
 * locks, runs again from its walk. The gate stops a child's unlink of x/f
 * where the walk has given it x, and the parent swaps x and y again: the
 * unlink runs again (the gate's `retry`) and removes the f of the new x,
 * where going on would remove the f of the old x, which is y now; the
 * attempt that was sent round made no change (one `effect` in all).
 */
static void gated_handoff_row(void)
{
    int pid = gated_start("an unlink that a rename came between after its walk runs again and "
                          "removes the new name",
                          "x/f", GATED_UNLINK, "handoff", 0);

    gated_swap();
    if (gated_end(pid) != 0 || gate_count("retry") == 0 || gate_count("effect") != 1 ||
        xstat("/vp/g/x/f") != -ENOENT || xstat("/vp/g/y/f") != 0) {
        fail(gated_why);
    }
    gate_write("off", gated_why);
}

/**
 * The walk sequence is compared in 64 bits. A child's read-only open of
 * y/f, whose names the cache holds, finishes without the big lock (`miss`
 * 0); stopped before that walk, while the parent moves the sequence by
 * 2^32 (`advance`), which a 32-bit compare would not see, it misses once
 * and goes again under the big lock.
 */
static void gated_width_row(void)
{
    int pid = gated_start("a walk without the big lock of a cached name finishes there", "y/f",
                          GATED_READ, "count", 0);

    if (gated_end(pid) != 3 || gate_count("miss") != 0) {
        fail(gated_why);
    }
    pid = gated_start("a walk without the big lock under which the sequence moved by 2^32 goes "
                      "again under the big lock",
                      "y/f", GATED_READ, "unlocked", 0);
    gate_write("advance", gated_why);
    if (gated_end(pid) != 3 || gate_count("miss") != 1) {
        fail(gated_why);
    }
    gate_write("off", gated_why);
}

/**
 * A walk takes no snapshot while a change is under way, nor while one of
 * two overlapping changes is. The gate holds two changes open (`begin`)
 * before a child's read-only open of y/f, which waits (`wait`); once one
 * ends, the other is still under way (`active` 1) and the child still
 * waits; once both end, it reads the file. While the gate holds a change,
 * the probe walks no path, since its walk would wait too.
 */
static void gated_overlap_row(void)
{
    int pid = gated_start("a walk waits while one of two overlapping changes is under way and goes "
                          "on once both end",
                          "y/f", GATED_READ, "count", 2);
    int st, i;

    while (gate_count("wait") == 0) {
        if (syscall4(SYS_WAIT4, pid, (long)&st, WNOHANG, 0) == pid) {
            shared_child = 0;
            gated_fail("the child's walk waits for the changes");
        }
        syscall0(SYS_SCHED_YIELD);
    }
    gate_write("end", gated_why);
    for (i = 0; i < 8; i++) {
        syscall0(SYS_SCHED_YIELD);
    }
    if (gate_count("active") != 1 || syscall4(SYS_WAIT4, pid, (long)&st, WNOHANG, 0) != 0) {
        gated_fail("the child waits while the second change is under way");
    }
    gate_write("end", gated_why);
    if (gated_end(pid) != 3) {
        fail(gated_why);
    }
    gate_write("off", gated_why);
}

/**
 * A type's hand-back at its root that uses up nothing of the path is EIO,
 * not a walk that goes round for ever: the gate's `fault` makes the walk of
 * /proc/self/status, once, take /proc's answer for such a hand-back.
 */
static void gated_fault_row(void)
{
    gated_why = gate_why = "a hand-back at the root of /proc that uses up nothing is EIO once";
    gate_pid_write("fault ", syscall0(SYS_GETPID), gated_why);
    if (xstat("/proc/self/status") != -EIO || xstat("/proc/self/status") != 0) {
        fail(gated_why);
    }
    gate_write("off", gated_why);
}

/** The inode of `path`, or fail the gated row's step `what`. */
static unsigned gated_ino_of(const char *path, const char *what)
{
    if (xstat(path) != 0) {
        gated_fail(what);
    }
    return st_field(KSTAT_INO);
}

/** Whether the file `path` holds the `n` bytes of `text`, at most 512, and
 * no more. */
static int file_holds(const char *path, const char *text, int n)
{
    static char got[520];
    int fd = syscall3(SYS_OPEN, (long)path, O_RDONLY, 0);
    int k, i;

    if (fd < 0) {
        return 0;
    }
    k = syscall3(SYS_READ, fd, (long)got, (int)sizeof(got));
    syscall1(SYS_CLOSE, fd);
    for (i = 0; i < n && i < k; i++) {
        if (got[i] != text[i]) {
            return 0;
        }
    }
    return k == n;
}

/**
 * A lookup that reads the disk, which the gate's `cold` forces by writing
 * every dirty block and dropping the root's names and the clean blocks,
 * sleeps in ext2's load of the inode it found with the directory's lock let
 * go (`load`). The gate stops a child's open of x/f there, with x/f "xx"
 * and y/f "yyy", and the parent swaps x and y: the lookup and the walk go
 * again, and the open reaches the new x's f, by its inode and its bytes,
 * where going on would open the old. The same with O_TRUNC empties the new
 * x's f and leaves the old one's bytes. A cold lookup whose read the gate
 * fails (`ioerr`) is EIO, makes nothing, and leaves the directory to the
 * next walk. The gate counts the reads of the disk and the restarts.
 */
static void gated_cold_rows(void)
{
    int pid;

    gated_why = gate_why = "a lookup that reads the disk, which a rename comes between while it "
                           "loads the inode, opens the new name's file";
    shared_file("/vp/g/x/f", "xx", 2, gated_why);
    gated_ino = gated_ino_of("/vp/g/y/f", "the inode of y/f");
    gated_text = "yyy";
    syscall0(SYS_SYNC);
    gate_write("cold", gated_why);
    pid = gated_start(gated_why, "x/f", GATED_READ, "load", 0);
    gated_swap();
    if (gated_end(pid) != 3 || gate_count("read") == 0 || gate_count("restart") == 0) {
        fail(gated_why);
    }
    gated_why = gate_why =
        "an O_TRUNC open that reads the disk, which a rename comes between while "
        "it loads the inode, empties the new name's file alone";
    gated_ino = gated_ino_of("/vp/g/y/f", "the inode of y/f");
    gated_text = 0;
    gate_write("cold", gated_why);
    pid = gated_start(gated_why, "x/f", GATED_TRUNC, "load", 0);
    gated_swap();
    if (gated_end(pid) != 0 || gate_count("read") == 0 || gate_count("restart") == 0 ||
        !file_holds("/vp/g/x/f", "", 0) || !file_holds("/vp/g/y/f", "yyy", 3)) {
        fail(gated_why);
    }
    shared_file("/vp/g/x/f", "xx", 2, gated_why);
    gated_why = gate_why =
        "a lookup whose read of the disk fails is EIO and makes nothing, and the "
        "next walk reads the directory";
    gated_ino = 0;
    gate_write("cold", gated_why);
    gated_ioerr = 1;
    pid = gated_start(gated_why, "x/n", GATED_CREATE, "count", 0);
    gated_ioerr = 0;
    if (gated_end(pid) != 100 + EIO || gate_count("read") == 0 || xstat("/vp/g/x/n") != -ENOENT ||
        !file_holds("/vp/g/x/f", "xx", 2)) {
        fail(gated_why);
    }
    gate_write("off", gated_why);
}

/**
 * The preparation of an open that creates through a dangling link, after
 * its first walk found the name missing, where the gate stops it (`link`):
 * a failure there is the open's answer, and nothing is made. With the
 * caches dropped and the child's next read of the disk failed (`ioerr`),
 * the read of /vp/g/l, a link to x/n, is EIO; with q made unsearchable to
 * the child, uid 65534, the walk to q/l, a link to ../x/n, is EACCES. A
 * swallowed failure would go on to create x/n, which the first row's next
 * walk could read. Neither row reaches a change (`effect` 0).
 */
static void gated_link_rows(void)
{
    int pid;

    gated_why = gate_why = "an O_CREAT open through a dangling link whose read of the link fails "
                           "after its first walk is EIO and makes nothing";
    if (syscall2(SYS_SYMLINK, (long)"x/n", (long)"/vp/g/l") != 0) {
        gated_fail("make the link l");
    }
    pid = gated_start(gated_why, "l", GATED_CREATE, "link", 0);
    gate_write("cold", gated_why);
    gate_pid_write("ioerr ", pid, gated_why);
    if (gated_end(pid) != 100 + EIO || gate_count("read") == 0 || gate_count("effect") != 0 ||
        xstat("/vp/g/x/n") != -ENOENT || syscall1(SYS_UNLINK, (long)"/vp/g/l") != 0) {
        fail(gated_why);
    }
    gate_write("off", gated_why);
    gated_why = gate_why = "an O_CREAT open through a dangling link whose directory turns "
                           "unsearchable after its first walk is EACCES and makes nothing";
    if (syscall2(SYS_MKDIR, (long)"/vp/g/q", 0755) != 0 ||
        syscall2(SYS_SYMLINK, (long)"../x/n", (long)"/vp/g/q/l") != 0) {
        gated_fail("make q and the link q/l");
    }
    pid = gated_start(gated_why, "q/l", GATED_USER, "link", 0);
    if (syscall2(SYS_CHMOD, (long)"/vp/g/q", 0700) != 0) {
        gated_fail("make q unsearchable");
    }
    if (gated_end(pid) != 100 + EACCES || gate_count("effect") != 0 ||
        xstat("/vp/g/x/n") != -ENOENT || syscall1(SYS_UNLINK, (long)"/vp/g/q/l") != 0 ||
        syscall1(SYS_RMDIR, (long)"/vp/g/q") != 0) {
        fail(gated_why);
    }
    gate_write("off", gated_why);
}

/**
 * An open with O_TRUNC that a rename comes between after its walk runs
 * again from its walk (`retry`) and empties the new x's f, by its inode,
 * and leaves the bytes of the old x's f, which is y's now; the attempt
 * that was sent round truncated nothing (one `effect` in all). x/f holds "xx"
 * and y/f "yyy" before; the row leaves x/f "yyy" and y/f "xx".
 */
static void gated_trunc_row(void)
{
    int pid;

    gated_why = gate_why = "an O_TRUNC open that a rename came between after its walk runs again "
                           "and empties the new name's file alone";
    gated_ino = gated_ino_of("/vp/g/y/f", "the inode of y/f");
    pid = gated_start(gated_why, "x/f", GATED_TRUNC, "handoff", 0);
    gated_swap();
    if (gated_end(pid) != 0 || gate_count("retry") == 0 || gate_count("effect") != 1 ||
        !file_holds("/vp/g/x/f", "", 0) || !file_holds("/vp/g/y/f", "xx", 2)) {
        fail(gated_why);
    }
    gated_ino = 0;
    shared_file("/vp/g/x/f", "yyy", 3, gated_why);
    gate_write("off", gated_why);
}

/* A change of `gated_op_rows`: what the child does, with `path2`, and the
 * name it makes in x, or 0 for a change of x/f or of x/d. */
struct gated_op {
    const char *why;
    int how;
    const char *path;
    const char *path2;
    const char *made;
};

static const struct gated_op gated_ops[] = {
    {"a creation that a rename came between after its walk runs again and makes the name in the "
     "new directory",
     GATED_EXCL, "x/c", 0, "c"},
    {"a mkdir that a rename came between after its walk runs again and makes the directory in "
     "the new directory",
     GATED_MKDIR, "x/d", 0, "d"},
    {"a link that a rename came between after its walks runs again and links the new name's file",
     GATED_LINK, "x/f", "x/l", "l"},
    {"a symlink that a rename came between after its walk runs again and makes the link in the "
     "new directory",
     GATED_SYMLINK, "x/s", "f", "s"},
    {"a mknod that a rename came between after its walk runs again and makes the FIFO in the new "
     "directory",
     GATED_MKNOD, "x/p", 0, "p"},
    {"a rename that a rename came between after its walks runs again and renames the new name's "
     "file",
     GATED_RENAME, "x/f", "x/r", "r"},
    {"a chmod that a rename came between after its walk runs again and sets the new name's mode",
     GATED_CHMOD, "x/f", 0, 0},
    {"a mount that a rename came between after its walk runs again and covers the new name",
     GATED_MOUNT, "x/d", 0, 0},
    {"a bind that a rename came between after its walks runs again and covers the new name",
     GATED_BIND, "x/d", "m", 0},
};

#define GATED_OPS ((int)(sizeof(gated_ops) / sizeof(gated_ops[0])))

/** The path /vp/g/`dir`/`name` in `buf`, of 32 bytes. */
static const char *gated_path(char *buf, char dir, const char *name)
{
    static const char pre[] = "/vp/g/";
    int i = 0, k;

    for (k = 0; pre[k]; k++) {
        buf[i++] = pre[k];
    }
    buf[i++] = dir;
    buf[i++] = '/';
    for (k = 0; name[k] && i < 31; k++) {
        buf[i++] = name[k];
    }
    buf[i] = '\0';
    return buf;
}

/** lstat(2) of `path` into `stbuf`: 0, or a negated errno. */
static int xlstat(const char *path)
{
    return syscall2(SYS_LSTAT, (long)path, (long)stbuf);
}

/**
 * Whether the change `op` landed in the new x and not in y, the old x, once
 * a swap came after its walk; `d` holds the inodes of x/d and y/d before
 * the swap and `f` that of y/f, for the rows that cover d or change f.
 */
static int gated_op_landed(const struct gated_op *op, const unsigned d[2], unsigned f)
{
    char a[32], b[32];

    if (op->made) {
        if (xlstat(gated_path(a, 'x', op->made)) != 0 ||
            xlstat(gated_path(b, 'y', op->made)) == 0) {
            return 0;
        }
        if (op->how == GATED_LINK || op->how == GATED_RENAME) {
            return xlstat(a) == 0 && st_field(KSTAT_INO) == f &&
                   (op->how == GATED_LINK ||
                    (xlstat("/vp/g/x/f") != 0 && xlstat("/vp/g/y/f") == 0));
        }
        return 1;
    }
    if (op->how == GATED_CHMOD) {
        return xstat("/vp/g/x/f") == 0 && (st_field(KSTAT_MODE) & 0777) == 0600 &&
               xstat("/vp/g/y/f") == 0 && (st_field(KSTAT_MODE) & 0777) != 0600;
    }
    if (op->how == GATED_MOUNT) {
        return xstat("/vp/g/y/d") == 0 && st_field(KSTAT_INO) == d[0] && xstat("/vp/g/x/d") == 0 &&
               st_field(KSTAT_INO) != d[1];
    }
    return xstat("/vp/g/m/f") == 0 && (f = st_field(KSTAT_INO)) != 0 && xstat("/vp/g/x/d/f") == 0 &&
           st_field(KSTAT_INO) == f && xstat("/vp/g/y/d/f") == -ENOENT;
}

/** Undo the change `op` made in x, or fail its row. */
static void gated_op_undo(const struct gated_op *op)
{
    char a[32], b[32];
    int rc;

    switch (op->how) {
    case GATED_MKDIR:
        rc = syscall1(SYS_RMDIR, (long)"/vp/g/x/d");
        break;
    case GATED_RENAME:
        rc = syscall2(SYS_RENAME, (long)gated_path(a, 'x', "r"), (long)gated_path(b, 'x', "f"));
        break;
    case GATED_CHMOD:
        rc = syscall2(SYS_CHMOD, (long)"/vp/g/x/f", 0644);
        break;
    case GATED_MOUNT:
    case GATED_BIND:
        rc = syscall2(SYS_UMOUNT2, (long)"/vp/g/x/d", MNT_DETACH);
        break;
    default:
        rc = syscall1(SYS_UNLINK, (long)gated_path(a, 'x', op->made));
        break;
    }
    if (rc != 0) {
        gated_fail("undo the change");
    }
}

/**
 * Each change of `gated_ops` that a rename comes between after its walk,
 * where the gate stops it (`handoff`), while the parent swaps x and y, runs
 * again from its walk (`retry`) and lands in the new x: a creation, a
 * mkdir, a link to the new x's f, a symlink, a FIFO, a rename of the new
 * x's f, a chmod of it, and a mount of a tmpfs and a bind of m over the new
 * x's d; each is undone after its row. The attempt that was sent round
 * reached no change of the file system: one `effect` in all. The mount
 * rows begin with d in both.
 */
static void gated_op_rows(void)
{
    unsigned d[2] = {0, 0}, f;
    int i, pid;

    for (i = 0; i < GATED_OPS; i++) {
        const struct gated_op *op = &gated_ops[i];

        gated_why = gate_why = op->why;
        if (op->how == GATED_MOUNT && (syscall2(SYS_MKDIR, (long)"/vp/g/x/d", 0755) != 0 ||
                                       syscall2(SYS_MKDIR, (long)"/vp/g/y/d", 0755) != 0)) {
            gated_fail("make x/d and y/d");
        }
        if (op->how >= GATED_MOUNT) {
            d[0] = gated_ino_of("/vp/g/x/d", "the inode of x/d");
            d[1] = gated_ino_of("/vp/g/y/d", "the inode of y/d");
        }
        f = gated_ino_of("/vp/g/y/f", "the inode of y/f");
        gated_path2 = op->path2;
        pid = gated_start(op->why, op->path, op->how, "handoff", 0);
        gated_swap();
        if (gated_end(pid) != 0 || gate_count("retry") == 0 || gate_count("effect") != 1 ||
            !gated_op_landed(op, d, f)) {
            fail(op->why);
        }
        gated_op_undo(op);
        gate_write("off", op->why);
    }
    gated_path2 = 0;
    gated_why = gate_why = gated_ops[GATED_OPS - 1].why;
    if (syscall1(SYS_RMDIR, (long)"/vp/g/x/d") != 0 ||
        syscall1(SYS_RMDIR, (long)"/vp/g/y/d") != 0) {
        gated_fail("remove x/d and y/d");
    }
}

/**
 * On an ext2 root, the rows that stop a walk at the walk gate and change
 * the tree under it: a full walk's restarts (`gated_restart_rows`), a
 * change's retry after its walk (`gated_handoff_row`), the sequence's width
 * (`gated_width_row`), overlapping changes (`gated_overlap_row`), the
 * fault of a hand-back (`gated_fault_row`), cold lookups and a failed read
 * (`gated_cold_rows`), failures in an open's preparation of a link to
 * create through (`gated_link_rows`), O_TRUNC (`gated_trunc_row`), and the
 * other changes'
 * retries (`gated_op_rows`). A root of another type, `magic`, has no such
 * rows. A failure undoes them (`gated_undo`).
 */
static void gated_rows(unsigned magic)
{
    if (magic != STATFS_MAGIC_EXT2) {
        return;
    }
    fail_undo = gated_undo;
    gated_why = "the files of the gated rows";
    if (syscall2(SYS_MKDIR, (long)"/vp/g", 0755) != 0 ||
        syscall2(SYS_MKDIR, (long)"/vp/g/x", 0755) != 0 ||
        syscall2(SYS_MKDIR, (long)"/vp/g/y", 0755) != 0 ||
        syscall2(SYS_MKDIR, (long)"/vp/g/m", 0755) != 0) {
        fail(gated_why);
    }
    shared_file("/vp/g/x/f", "xx", 2, gated_why);
    shared_file("/vp/g/y/f", "yyy", 3, gated_why);
    shared_file("/vp/g/m/f", "mmm", 3, gated_why);
    gate_wfd = syscall3(SYS_OPEN, (long)WALK_GATE, O_WRONLY, 0);
    gate_rfd = syscall3(SYS_OPEN, (long)WALK_GATE, O_RDONLY, 0);
    shared_root = syscall3(SYS_OPEN, (long)"/", O_RDONLY | O_DIRECTORY, 0);
    if (gate_wfd < 0 || gate_rfd < 0 || shared_root < 0 || xstat("/vp/g/y/f") != 0) {
        fail(gated_why);
    }
    gated_restart_rows();
    gated_handoff_row();
    gated_width_row();
    gated_overlap_row();
    gated_fault_row();
    gated_cold_rows();
    gated_link_rows();
    gated_trunc_row();
    gated_op_rows();
    syscall1(SYS_CLOSE, shared_root);
    syscall1(SYS_CLOSE, gate_wfd);
    syscall1(SYS_CLOSE, gate_rfd);
    shared_root = gate_wfd = gate_rfd = -1;
    gate_why = "read the walk gate";
    if (syscall1(SYS_UNLINK, (long)"/vp/g/x/f") != 0 ||
        syscall1(SYS_UNLINK, (long)"/vp/g/y/f") != 0 ||
        syscall1(SYS_UNLINK, (long)"/vp/g/m/f") != 0 || syscall1(SYS_RMDIR, (long)"/vp/g/x") != 0 ||
        syscall1(SYS_RMDIR, (long)"/vp/g/y") != 0 || syscall1(SYS_RMDIR, (long)"/vp/g/m") != 0 ||
        syscall1(SYS_RMDIR, (long)"/vp/g") != 0) {
        fail("remove the files of the gated rows");
    }
    fail_undo = 0;
}

/* The child of a KRFFS keep row, whose call the gate stops, or 0, and the
 * second child, whose keep shows where the new file lies. */
static int keep_child[2];

/** Undo what `keep_rows` made, for `fail`: the gate, the children, and the
 * files and directories under /vp/k. */
static void keep_undo(void)
{
    int st, i;

    syscall3(SYS_WRITE, gate_wfd, (long)"off", 3);
    for (i = 0; i < 2; i++) {
        if (keep_child[i] > 0) {
            syscall2(SYS_KILL, keep_child[i], 9);
            syscall4(SYS_WAIT4, keep_child[i], (long)&st, 0, 0);
            keep_child[i] = 0;
        }
    }
    syscall1(SYS_CLOSE, gate_wfd);
    syscall1(SYS_CLOSE, gate_rfd);
    gate_wfd = gate_rfd = -1;
    syscall1(SYS_UNLINK, (long)"/vp/k/g");
    syscall1(SYS_UNLINK, (long)"/vp/k/h");
    syscall1(SYS_UNLINK, (long)"/vp/k/n");
    syscall1(SYS_RMDIR, (long)"/vp/k/d");
    syscall1(SYS_RMDIR, (long)"/vp/k");
    gate_why = "read the walk gate";
}

/* What the child of a keep row does (`keep_start`). */
#define KEEP_CHDIR   0 /* chdir to /vp/k/d, and getcwd must name it */
#define KEEP_CHMOD   1 /* chmod /vp/k/g to 0600 */
#define KEEP_NOTDIR  2 /* open /vp/k/g with O_DIRECTORY, which is ENOTDIR */
#define KEEP_CHMOD_H 3 /* chmod /vp/k/h to 0640 */

/** The call of the child of a keep row, `how`: 0 when it answers what the
 * row expects, or 100 and the errno, 99 for another answer. */
static int keep_call(int how)
{
    char cwd[16] = {0};
    int rc;

    switch (how) {
    case KEEP_CHDIR:
        rc = syscall1(SYS_CHDIR, (long)"/vp/k/d");
        if (rc == 0 &&
            (syscall2(SYS_GETCWD, (long)cwd, sizeof(cwd)) != 8 || !same(cwd, "/vp/k/d"))) {
            return 99;
        }
        return rc < 0 ? 100 - rc : 0;
    case KEEP_CHMOD:
        rc = syscall2(SYS_CHMOD, (long)"/vp/k/g", 0600);
        return rc < 0 ? 100 - rc : 0;
    case KEEP_NOTDIR:
        rc = syscall3(SYS_OPEN, (long)"/vp/k/g", O_RDONLY | O_DIRECTORY, 0);
        return rc == -ENOTDIR ? 0 : rc < 0 ? 100 - rc : 99;
    default:
        rc = syscall2(SYS_CHMOD, (long)"/vp/k/h", 0640);
        return rc < 0 ? 100 - rc : 0;
    }
}

/** Start the child `i` of a keep row, which makes the call `how` and stops
 * at the gate where it has let go of its walk's lock (`kept`), and wait
 * until it is there; fail `why` when it ends first. */
static void keep_start(int i, int how, const char *why)
{
    char byte = 0;
    int pid, st;

    if (syscall1(SYS_PIPE, (long)shared_pipe) != 0) {
        fail(why);
    }
    pid = syscall0(SYS_FORK);
    if (pid == 0) {
        sys_exit(syscall3(SYS_READ, shared_pipe[0], (long)&byte, 1) != 1 ? 100 : keep_call(how));
    }
    if (pid < 0) {
        fail(why);
    }
    keep_child[i] = pid;
    gate_arm(pid, "kept", why);
    if (syscall3(SYS_WRITE, shared_pipe[1], (long)&byte, 1) != 1) {
        fail(why);
    }
    syscall1(SYS_CLOSE, shared_pipe[0]);
    syscall1(SYS_CLOSE, shared_pipe[1]);
    shared_pipe[0] = shared_pipe[1] = -1;
    while (gate_count("kept") == 0) {
        if (syscall4(SYS_WAIT4, pid, (long)&st, WNOHANG, 0) == pid) {
            keep_child[i] = 0;
            fail(why);
        }
        syscall0(SYS_SCHED_YIELD);
    }
}

/** Let the children of a keep row go on and reap them: 0 when each that
 * was started exited 0, or fail `why`. */
static void keep_end(const char *why)
{
    int st, i;

    gate_write("release", why);
    for (i = 0; i < 2; i++) {
        if (keep_child[i] > 0) {
            if (syscall4(SYS_WAIT4, keep_child[i], (long)&st, 0, 0) != keep_child[i]) {
                fail(why);
            }
            keep_child[i] = 0;
            if (st != 0) {
                fail(why);
            }
        }
    }
    gate_write("off", why);
}

/** The free KiB statfs gives for the root, or fail `why`. */
static unsigned root_bfree(const char *why)
{
    static unsigned char buf[KSTATFS_SIZE];

    if (probe_statfs("/", buf) != 0) {
        fail(why);
    }
    return *(unsigned *)(buf + KSTATFS_BFREE);
}

/** The little-endian 32-bit word at `p`. */
static unsigned word_at(const char *p)
{
    const unsigned char *b = (const unsigned char *)p;

    return (unsigned)b[0] | (unsigned)b[1] << 8 | (unsigned)b[2] << 16 | (unsigned)b[3] << 24;
}

/** Count into `*g` and `*h` the IN_ATTRIB events the inotify descriptor
 * `fd`, which does not block, has for the names g and h, reading every
 * event it has. */
static void attrib_counts(int fd, int *g, int *h)
{
    static char buf[1024];
    int n, i;

    *g = *h = 0;
    while ((n = syscall3(SYS_READ, fd, (long)buf, (int)sizeof(buf))) > 0) {
        for (i = 0; i + IN_EVENT_FIXED <= n; i += IN_EVENT_FIXED + (int)word_at(buf + i + 12)) {
            const char *name = buf + i + IN_EVENT_FIXED;

            if ((word_at(buf + i + 4) & IN_ATTRIB) && word_at(buf + i + 12) > 0 &&
                name[1] == '\0') {
                *g += name[0] == 'g';
                *h += name[0] == 'h';
            }
        }
    }
}

/** An inotify descriptor, which does not block, that watches /vp/k for
 * changes of attributes; fail `why` when there is none. */
static int keep_watch(const char *why)
{
    int fd = syscall1(SYS_INOTIFY_INIT1, IN_NONBLOCK);

    if (fd < 0 || syscall3(SYS_INOTIFY_ADD_WATCH, fd, (long)"/vp/k", IN_ATTRIB) < 0) {
        fail(why);
    }
    return fd;
}

/** Close `watch`, which `keep_watch` made, once it has heard one change
 * of attributes for g and one for h, and remove g, which has mode 0600; or
 * fail `why`. */
static void keep_watched(int watch, const char *why)
{
    int g, h;

    attrib_counts(watch, &g, &h);
    syscall1(SYS_CLOSE, watch);
    if (g != 1 || h != 1 || xstat("/vp/k/g") != 0 || (st_field(KSTAT_MODE) & 0777) != 0600 ||
        syscall1(SYS_UNLINK, (long)"/vp/k/g") != 0) {
        fail(why);
    }
}

/**
 * A file of the KRFFS root that a stopped call's walk keeps, by `how`, a
 * chmod that succeeds or an open that fails: the probe grows the file past
 * its neighbour, made right after it and larger, so that no earlier room
 * takes it and the file cannot grow in place, which moves the file, and
 * the gate's `keep` follows it; and makes a file of the old size, which
 * takes the old place, as a second stopped call's keep shows. Once the
 * calls end, the new file holds its bytes and the mode its call set, and
 * once the files are removed the root has the room it had: the keep freed
 * the moved file and nothing at its old place.
 *
 * The chmod is stopped where it has let go of its walk's lock and still
 * uses its answer, to tell inotify (`attr_told`), which reads the
 * attributes of the file it kept: a watch of the directory hears one change
 * of attributes for g, which it hears only when that read reaches the moved
 * g and not the new file at g's old place, and one for h, from h's own
 * chmod; g has the mode the chmod set. A change of a file on the KRFFS root
 * comes under the namespace lock its walk returns with, which a relocation
 * takes too, so the use after the lock is let go is the part of a call that
 * a relocation can come before. The failing open's file is unlinked while
 * it is kept, which leaves it nameless until the keep ends and frees it.
 */
static void keep_relocate_row(int how)
{
    const char *why = how == KEEP_CHMOD ? "a keep of a file that moves follows it, and its end "
                                          "frees the moved file alone"
                                        : "a keep of a file that moves follows it, and its end "
                                          "after a failed call frees the moved file alone";
    static char bytes[1536];
    unsigned room, was, moved, reused;
    int fd, i, watch = -1;

    gate_why = why;
    for (i = 0; i < (int)sizeof(bytes); i++) {
        bytes[i] = (char)('a' + i % 26);
    }
    room = root_bfree(why);
    shared_file("/vp/k/g", bytes, 512, why);
    shared_file("/vp/k/n", bytes, 1024, why);
    if (how == KEEP_CHMOD) {
        watch = keep_watch(why);
    }
    keep_start(0, how, why);
    was = gate_count("keep");
    fd = syscall3(SYS_OPEN, (long)"/vp/k/g", O_WRONLY | O_APPEND, 0);
    if (was == 0 || fd < 0 ||
        syscall3(SYS_WRITE, fd, (long)bytes, (int)sizeof(bytes)) != (int)sizeof(bytes)) {
        fail(why);
    }
    syscall1(SYS_CLOSE, fd);
    moved = gate_count("keep");
    if (moved == 0 || moved == was ||
        (how == KEEP_NOTDIR && syscall1(SYS_UNLINK, (long)"/vp/k/g") != 0)) {
        fail(why);
    }
    shared_file("/vp/k/h", bytes + 7, 512, why);
    keep_start(1, KEEP_CHMOD_H, why);
    reused = gate_count("keep");
    keep_end(why);
    if (how == KEEP_CHMOD) {
        keep_watched(watch, why);
    }
    if (reused != was || !file_holds("/vp/k/h", bytes + 7, 512) || xstat("/vp/k/h") != 0 ||
        (st_field(KSTAT_MODE) & 0777) != 0640 || xstat("/vp/k/g") != -ENOENT ||
        syscall1(SYS_UNLINK, (long)"/vp/k/h") != 0 || syscall1(SYS_UNLINK, (long)"/vp/k/n") != 0 ||
        root_bfree(why) != room) {
        fail(why);
    }
}

/**
 * On a KRFFS root, which moves a file that grows and gives its old place to
 * the next file made, the answer a call's walk found is kept until the call
 * ends (`struct walk_keep` in vfs.c), past the point where the call lets go
 * of its walk's lock, where the gate stops it (`kept`): an rmdir of the
 * directory a stopped chdir keeps is EBUSY, and the chdir then enters it;
 * and a kept file follows a move (`keep_relocate_row`). A root of another
 * type, `magic`, has no such rows. A failure undoes them (`keep_undo`).
 */
static void keep_rows(unsigned magic)
{
    const char *why = "an rmdir of a directory a stopped chdir keeps is EBUSY, and the chdir "
                      "enters it";

    if (magic != STATFS_MAGIC_KRFFS) {
        return;
    }
    fail_undo = keep_undo;
    gate_why = why;
    if (syscall2(SYS_MKDIR, (long)"/vp/k", 0755) != 0 ||
        syscall2(SYS_MKDIR, (long)"/vp/k/d", 0755) != 0) {
        fail(why);
    }
    gate_wfd = syscall3(SYS_OPEN, (long)WALK_GATE, O_WRONLY, 0);
    gate_rfd = syscall3(SYS_OPEN, (long)WALK_GATE, O_RDONLY, 0);
    if (gate_wfd < 0 || gate_rfd < 0) {
        fail(why);
    }
    keep_start(0, KEEP_CHDIR, why);
    if (syscall1(SYS_RMDIR, (long)"/vp/k/d") != -EBUSY) {
        fail(why);
    }
    keep_end(why);
    if (syscall1(SYS_RMDIR, (long)"/vp/k/d") != 0) {
        fail(why);
    }
    keep_relocate_row(KEEP_CHMOD);
    keep_relocate_row(KEEP_NOTDIR);
    keep_undo();
    if (xstat("/vp/k") != -ENOENT) {
        fail("remove the files of the keep rows");
    }
    fail_undo = 0;
}

/** Undo `cwd_empty_row`, for `fail` and its own end. */
static void cwd_empty_undo(void)
{
    syscall1(SYS_CHDIR, (long)"/");
    syscall1(SYS_UNLINK, (long)"/vp/e/a");
    syscall1(SYS_UNLINK, (long)"/vp/e/b");
    syscall1(SYS_RMDIR, (long)"/vp/e");
}

/**
 * A call on the working directory by an empty path ends when what a path
 * names has changed since the thread's last walk: fchownat of "" with
 * AT_EMPTY_PATH, after a walk and then a file's rename in /vp/e, takes the
 * working directory by its pair with a stamp of its own, which its change
 * checks, and does not go round on the stamp the walk before the rename
 * left. The same call after a chdir to /, which on a KRFFS root leaves the
 * working directory no pair, walks its path and ends too.
 */
static void cwd_empty_row(void)
{
    const char *why = "fchownat of the working directory by an empty path, after a rename, ends";
    long uid = syscall0(SYS_GETUID);
    long fd;

    fail_undo = cwd_empty_undo;
    if (syscall2(SYS_MKDIR, (long)"/vp/e", 0755) != 0) {
        fail(why);
    }
    fd = syscall3(SYS_OPEN, (long)"/vp/e/a", O_CREAT | O_WRONLY, 0644);
    if (fd < 0 || syscall1(SYS_CLOSE, fd) != 0 || syscall1(SYS_CHDIR, (long)"/vp/e") != 0 ||
        xstat("/vp/e/a") != 0 || syscall2(SYS_RENAME, (long)"/vp/e/a", (long)"/vp/e/b") != 0 ||
        syscall5(SYS_FCHOWNAT, AT_FDCWD, (long)"", uid, -1, AT_EMPTY_PATH) != 0 ||
        syscall1(SYS_CHDIR, (long)"/") != 0 ||
        syscall5(SYS_FCHOWNAT, AT_FDCWD, (long)"", uid, -1, AT_EMPTY_PATH) != 0) {
        fail(why);
    }
    cwd_empty_undo();
    fail_undo = 0;
}

/**
 * A walk follows a fixed link of /sys, one no driver registered: the PCI
 * host bridge's /sys/bus/pci/devices/0000:00:00.0, which reads as Linux's
 * ../../../devices/pci0000:00/0000:00:00.0, leads to the device's uevent.
 */
static void sys_link_row(void)
{
    const char *why = "a walk follows the fixed /sys link of the PCI host bridge to its uevent";
    const char *want = "../../../devices/";
    char text[64];
    long n =
        syscall3(SYS_READLINK, (long)"/sys/bus/pci/devices/0000:00:00.0", (long)text, sizeof(text));
    int i;

    for (i = 0; want[i]; i++) {
        if (i >= n || text[i] != want[i]) {
            fail(why);
        }
    }
    if (xstat("/sys/bus/pci/devices/0000:00:00.0/uevent") != 0) {
        fail(why);
    }
}

#define CLIMB_MOUNTS 1024 /* the tmpfs mounts `climb_row` stacks */
#define CLIMB_TIMES  700  /* the times its path climbs them */

static int climb_made; /* the mounts `climb_row` has stacked on /vp/c */

/** Undo `climb_row`, for `fail` and its own end: every mount it stacked on
 * /vp/c, then /vp/c. */
static void climb_undo(void)
{
    while (climb_made > 0) {
        syscall2(SYS_UMOUNT2, (long)"/vp/c", MNT_DETACH);
        climb_made--;
    }
    syscall1(SYS_RMDIR, (long)"/vp/c");
}

/**
 * A walk is bounded by the links it follows alone, so a path with no link
 * that climbs a stack of mounts many times is walked whole: with
 * CLIMB_MOUNTS tmpfs mounts stacked on /vp/c, "/vp/c" and CLIMB_TIMES
 * "/../c" after it, 3,505 bytes, each ".." climbing every mount of the
 * stack to /vp and each "c" entering its top, ends on the top mount's root,
 * by its device and inode. It runs on a KRFFS root alone: the kernel logs
 * each mount on the console, and the ext2 machine's serial console takes
 * the 1024 lines past its case's gap bound.
 */
static void climb_row(unsigned magic)
{
    const char *why = "a walk climbs 1024 mounts stacked on /vp/c, 700 times over, and ends on "
                      "the top one";
    static char path[4096];
    unsigned dev, ino;
    int i, n;

    if (magic != STATFS_MAGIC_KRFFS) {
        return;
    }
    fail_undo = climb_undo;
    if (syscall2(SYS_MKDIR, (long)"/vp/c", 0755) != 0) {
        fail(why);
    }
    for (i = 0; i < CLIMB_MOUNTS; i++) {
        if (syscall5(SYS_MOUNT, (long)"none", (long)"/vp/c", (long)"tmpfs", 0, 0) != 0) {
            fail(why);
        }
        climb_made++;
    }
    if (xstat("/vp/c") != 0) {
        fail(why);
    }
    dev = st_field(KSTAT_DEV);
    ino = st_field(KSTAT_INO);
    n = 0;
    for (i = 0; "/vp/c"[i]; i++) {
        path[n++] = "/vp/c"[i];
    }
    for (i = 0; i < CLIMB_TIMES; i++) {
        path[n++] = '/';
        path[n++] = '.';
        path[n++] = '.';
        path[n++] = '/';
        path[n++] = 'c';
    }
    path[n] = '\0';
    if (xstat(path) != 0 || st_field(KSTAT_DEV) != dev || st_field(KSTAT_INO) != ino) {
        fail(why);
    }
    climb_undo();
    fail_undo = 0;
    if (xstat("/vp/c") != -ENOENT) {
        fail(why);
    }
}

/** The rows of walks, by the root `magic` names: on ext2 the holds of a
 * walk without the big lock (`cwd_unlocked_rows`), the changes that send
 * it again (`shared_rows`), and the changes that send a walk or a call again
 * while the walk gate stops it (`gated_rows`); on KRFFS the keeps of a
 * call's walk (`keep_rows`) and a climb of stacked mounts (`climb_row`). */
static void unlocked_rows(unsigned magic)
{
    cwd_unlocked_rows(magic);
    shared_rows(magic);
    handle_rows(magic);
    gated_rows(magic);
    keep_rows(magic);
    cwd_empty_row();
    sys_link_row();
    climb_row(magic);
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
