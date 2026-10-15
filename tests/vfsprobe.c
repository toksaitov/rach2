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
 * root type fails.
 *
 * Prints "VFSPROBE OK". Freestanding; direct system calls (probe.h).
 */

#include "probe.h"

#define PHYSICAL_DOTDOT 1

#define O_RDONLY    0
#define O_WRONLY    1
#define O_RDWR      2
#define O_CREAT     0100
#define O_EXCL      0200
#define O_TRUNC     01000
#define O_DIRECTORY 0200000

#define AT_FDCWD (-100)

#define EPERM     1
#define ENOENT    2
#define EBADF     9
#define EEXIST    17
#define ENOTDIR   20
#define EISDIR    21
#define ENOTEMPTY 39

#define SEEK_SET 0
#define SEEK_END 2

#define STATFS_MAGIC_PROC  0x9FA0
#define STATFS_MAGIC_KRFFS 0x4B524646
#define STATFS_MAGIC_EXT2  0xEF53

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

/** Print "VFSPROBE FAIL: " and `why`, and exit 1. */
static void fail(const char *why)
{
    syscall3(SYS_WRITE, 1, (long)"VFSPROBE FAIL: ", 15);
    puts(why);
    sys_exit(1);
}

/**
 * Read the root type from the mount table independently of statfs, and
 * return its statfs magic.
 *
 * Match the mountpoint and type fields of a complete line. Missing or
 * unsupported roots fail before the probe creates files.
 */
static unsigned root_magic(void)
{
    char buf[2048];
    int fd, n, start, end, p;

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
        if (p + 3 >= end || buf[p + 1] != '/' || buf[p + 2] != ' ') {
            continue;
        }
        p += 3;
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

/**
 * Make /vp/d/f and run the rows in order, failing at the first with its
 * name: what a path answers, open and openat, the description across unlink
 * and rename, offsets and sizes, a relocated file, directories, attributes,
 * the hard link and the blocks, statfs and the working directory; then
 * remove what it made. A relocated file keeps every description: a second
 * description on a file that grows past a neighbour made right after it,
 * so that it cannot grow in place, reads the grown size and the moved bytes.
 */
PROBE_START void _start(void)
{
    int fd, dfd, rc;
    char cwd[64];
    unsigned magic = root_magic();

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

        if (xstat("/") != 0) {
            fail("stat of the root");
        }
        root_ino = st_field(KSTAT_INO);
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
    if (probe_statfs("/", iobuf) != 0 || *(unsigned *)iobuf != magic) {
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

    if (syscall1(SYS_SYNC, 0) != 0) {
        fail("sync");
    }

    if (syscall1(SYS_UNLINK, (long)"/vp/d/f") != 0 || syscall1(SYS_RMDIR, (long)"/vp/d") != 0 ||
        syscall1(SYS_RMDIR, (long)"/vp") != 0) {
        fail("cleanup");
    }

    puts("VFSPROBE OK");
    sys_exit(0);
}
