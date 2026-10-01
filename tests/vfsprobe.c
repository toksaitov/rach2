/**
 * The path and descriptor contract the VFS migration must preserve.
 *
 * Each row is one promise: the errno a path answers, what a description
 * keeps across unlink and rename, what the synthetic mounts are and are
 * not, and the statfs identities, with the inode counts ext2 keeps and
 * KRFFS does not. The rows run before the migration starts
 * and after every family it moves, so a family that changes an answer is
 * caught the commit it moves.
 *
 * `PHYSICAL_DOTDOT` marks the rows whose correct answer changes when `..`
 * stops being lexical: through a missing or non-directory component a
 * lexical resolver succeeds and a physical one must not. It flipped to 1
 * in the migration commit that removed the lexical step.
 *
 * Needs: root privilege, a writable KRFFS or ext2 root, mounted /proc and
 * /dev, and absent /vp and /procx paths. The root type in /proc/mounts selects
 * the hard-link result, the statfs magic and the inode counts. An unknown
 * root type fails.
 *
 * Prints "VFSPROBE OK". Freestanding; direct int 0x80.
 */

#define PHYSICAL_DOTDOT 1

typedef unsigned int size_t;

#define SYS_EXIT       1
#define SYS_READ       3
#define SYS_WRITE      4
#define SYS_OPEN       5
#define SYS_CLOSE      6
#define SYS_LINK       9
#define SYS_UNLINK     10
#define SYS_CHDIR      12
#define SYS_CHMOD      15
#define SYS_LSEEK      19
#define SYS_SYNC       36
#define SYS_RENAME     38
#define SYS_MKDIR      39
#define SYS_RMDIR      40
#define SYS_FTRUNCATE  93
#define SYS_FSYNC      118
#define SYS_SYNCFS     344
#define SYS_GETCWD     183
#define SYS_STAT64     195
#define SYS_GETDENTS64 220
#define SYS_STATFS64   268
#define SYS_OPENAT     295
#define SYS_UTIMENSAT  320

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

static int sys1(int n, int a)
{
    int r;
    __asm__ __volatile__("int $0x80" : "=a"(r) : "0"(n), "b"(a) : "memory");
    return r;
}

static int sys2(int n, int a, int b)
{
    int r;
    __asm__ __volatile__("int $0x80" : "=a"(r) : "0"(n), "b"(a), "c"(b) : "memory");
    return r;
}

static int sys3(int n, int a, int b, int c)
{
    int r;
    __asm__ __volatile__("int $0x80" : "=a"(r) : "0"(n), "b"(a), "c"(b), "d"(c) : "memory");
    return r;
}

static int sys4(int n, int a, int b, int c, int d)
{
    int r;
    __asm__ __volatile__("int $0x80" : "=a"(r) : "0"(n), "b"(a), "c"(b), "d"(c), "S"(d) : "memory");
    return r;
}

static void sys_exit(int status)
{
    sys1(SYS_EXIT, status);
    __builtin_unreachable();
}

static size_t strlen(const char *s)
{
    size_t n = 0;
    while (s[n]) {
        n++;
    }
    return n;
}

static void puts(const char *s)
{
    sys3(SYS_WRITE, 1, (int)s, (int)strlen(s));
    sys3(SYS_WRITE, 1, (int)"\n", 1);
}

static void fail(const char *why)
{
    sys3(SYS_WRITE, 1, (int)"VFSPROBE FAIL: ", 15);
    puts(why);
    sys_exit(1);
}

/**
 * Read the root type from the mount table independently of statfs.
 *
 * Match the mountpoint and type fields of a complete line. Missing or
 * unsupported roots fail before the probe creates files.
 */
static unsigned root_magic(void)
{
    char buf[2048];
    int fd, n, start, end, p;

    fd = sys3(SYS_OPEN, (int)"/proc/mounts", O_RDONLY, 0);
    if (fd < 0) {
        fail("open /proc/mounts");
    }
    n = sys3(SYS_READ, fd, (int)buf, sizeof(buf));
    sys1(SYS_CLOSE, fd);
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

static unsigned char stbuf[104];

static int xstat(const char *path)
{
    stbuf[0] = 0;
    return sys2(SYS_STAT64, (int)path, (int)stbuf);
}

/* A field of the last stat64: the mode at 16, the mtime at 72, the 64-bit
   inode's low word at 88, in the i386 layout. */
static unsigned st_field(int off)
{
    unsigned v;

    __builtin_memcpy(&v, stbuf + off, 4);
    return v;
}

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
 * it; KRFFS rounds the size up. st_blocks is the stat64 field at 56.
 */
static void check_blocks(unsigned magic)
{
    int fd = sys3(SYS_OPEN, (int)"/vp/d/s", O_RDWR | O_CREAT, 0644);

    if (fd < 0 || sys2(SYS_FTRUNCATE, fd, 65536) != 0 || xstat("/vp/d/s") != 0) {
        fail("ftruncate a new file to 64 KiB");
    }
    if (st_field(56) != (magic == STATFS_MAGIC_EXT2 ? 0u : 128u)) {
        fail("st_blocks of a file that is one hole");
    }
    if (sys3(SYS_WRITE, fd, (int)"x", 1) != 1 || xstat("/vp/d/s") != 0 || st_field(56) == 0 ||
        (magic == STATFS_MAGIC_EXT2 && st_field(56) >= 128)) {
        fail("st_blocks after a write into the hole");
    }
    sys1(SYS_CLOSE, fd);
    sys1(SYS_UNLINK, (int)"/vp/d/s");
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

    if (xstat("/vp/d/f") != 0 || st_field(20) != 1) {
        fail("the link source has one name");
    }
    ino = st_field(88);
    if (magic == STATFS_MAGIC_KRFFS) {
        if (sys2(SYS_LINK, (int)"/vp/d/f", (int)"/vp/d/h") != -EPERM ||
            xstat("/vp/d/h") != -ENOENT) {
            fail("link on krffs is EPERM and creates no name");
        }
        return;
    }
    if (sys2(SYS_LINK, (int)"/vp/d/f", (int)"/vp/d/h") != 0) {
        fail("link on ext2 succeeds");
    }
    if (xstat("/vp/d/h") != 0 || st_field(88) != ino || st_field(20) != 2 ||
        xstat("/vp/d/f") != 0 || st_field(88) != ino || st_field(20) != 2) {
        fail("both ext2 names report one inode with two links");
    }
    fd = sys3(SYS_OPEN, (int)"/vp/d/h", O_RDONLY, 0);
    if (fd < 0 || sys3(SYS_READ, fd, (int)iobuf, 4) != 3 || iobuf[0] != 'a' || iobuf[1] != 'b' ||
        iobuf[2] != 'c') {
        fail("read through the ext2 hard link");
    }
    sys1(SYS_CLOSE, fd);
    if (sys1(SYS_UNLINK, (int)"/vp/d/h") != 0 || xstat("/vp/d/h") != -ENOENT ||
        xstat("/vp/d/f") != 0 || st_field(88) != ino || st_field(20) != 1) {
        fail("unlink leaves the ext2 source with one link");
    }
}

int stat_marked_missing_dotdot(void);
int stat_marked_file_dotdot(void);

/* The two rows the flip commit changes: `..` through a component that does
 * not resolve. Kept as functions so the marked expectation reads in one
 * place. */
int stat_marked_missing_dotdot(void)
{
    int rc = xstat("/vp/nosuch/../d/f");
#if PHYSICAL_DOTDOT
    return rc == -ENOENT;
#else
    return rc == 0;
#endif
}

int stat_marked_file_dotdot(void)
{
    int rc = xstat("/vp/d/f/../f");
#if PHYSICAL_DOTDOT
    return rc == -ENOTDIR;
#else
    return rc == 0;
#endif
}

void _start(void)
{
    int fd, dfd, rc;
    char cwd[64];
    unsigned magic = root_magic();

    /* ---- the tree the rows use ---- */
    if (sys2(SYS_MKDIR, (int)"/vp", 0755) != 0) {
        fail("mkdir /vp");
    }
    if (sys2(SYS_MKDIR, (int)"/vp/d", 0755) != 0) {
        fail("mkdir /vp/d");
    }
    fd = sys3(SYS_OPEN, (int)"/vp/d/f", O_CREAT | O_RDWR, 0644);
    if (fd < 0) {
        fail("create /vp/d/f");
    }
    if (sys3(SYS_WRITE, fd, (int)"abc", 3) != 3) {
        fail("write /vp/d/f");
    }
    sys1(SYS_CLOSE, fd);

    /* ---- what a path answers ---- */
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
    /* The doubled slashes are spelled in pieces: the source convention
       forbids the two-slash comment marker anywhere. */
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
        root_ino = st_field(88);
        if (xstat("/..") != 0 || st_field(88) != root_ino || xstat("/vp/..") != 0 ||
            st_field(88) != root_ino) {
            fail("dot-dot at the root is the root");
        }
    }
    if (!stat_marked_missing_dotdot()) {
        fail("dot-dot through a missing directory (marked row)");
    }
    if (!stat_marked_file_dotdot()) {
        fail("dot-dot through a file (marked row)");
    }

    /* ---- open and openat ---- */
    dfd = sys3(SYS_OPEN, (int)"/vp/d", O_RDONLY, 0);
    if (dfd < 0) {
        fail("open the directory");
    }
    fd = sys4(SYS_OPENAT, dfd, (int)"f", O_RDONLY, 0);
    if (fd < 0) {
        fail("openat through the directory descriptor");
    }
    if (sys3(SYS_READ, fd, (int)iobuf, 8) != 3 || iobuf[0] != 'a') {
        fail("read through the openat descriptor");
    }
    sys1(SYS_CLOSE, fd);
    if (sys1(SYS_CLOSE, dfd) != 0) {
        fail("close the openat directory");
    }
    if (sys3(SYS_OPEN, (int)"/vp/d/f", O_CREAT | O_EXCL | O_RDWR, 0644) != -EEXIST) {
        fail("O_CREAT|O_EXCL on an existing file is EEXIST");
    }
    if (sys3(SYS_OPEN, (int)"/vp/d", O_WRONLY, 0) != -EISDIR) {
        fail("a directory opened for writing is EISDIR");
    }
    if (sys3(SYS_OPEN, (int)"/vp/d/f", O_RDONLY | O_DIRECTORY, 0) != -ENOTDIR) {
        fail("O_DIRECTORY on a file is ENOTDIR");
    }

    /* ---- the description keeps the file ---- */
    fd = sys3(SYS_OPEN, (int)"/vp/d/f2", O_CREAT | O_RDWR, 0644);
    if (fd < 0 || sys3(SYS_WRITE, fd, (int)"zz", 2) != 2) {
        fail("create /vp/d/f2");
    }
    sys1(SYS_CLOSE, fd);
    fd = sys3(SYS_OPEN, (int)"/vp/d/f2", O_RDONLY, 0);
    if (fd < 0) {
        fail("reopen /vp/d/f2");
    }
    if (sys1(SYS_UNLINK, (int)"/vp/d/f2") != 0) {
        fail("unlink the open file");
    }
    if (xstat("/vp/d/f2") != -ENOENT) {
        fail("the unlinked name is gone");
    }
    if (sys3(SYS_READ, fd, (int)iobuf, 8) != 2 || iobuf[0] != 'z') {
        fail("the description outlives the unlink");
    }
    sys1(SYS_CLOSE, fd);

    /* ---- rename under an open description ---- */
    fd = sys3(SYS_OPEN, (int)"/vp/d/f", O_RDONLY, 0);
    if (fd < 0) {
        fail("open before rename");
    }
    if (sys2(SYS_RENAME, (int)"/vp/d/f", (int)"/vp/d/g") != 0) {
        fail("rename f to g");
    }
    if (sys3(SYS_READ, fd, (int)iobuf, 8) != 3 || iobuf[2] != 'c') {
        fail("the description outlives the rename");
    }
    sys1(SYS_CLOSE, fd);
    if (sys2(SYS_RENAME, (int)"/vp/d/g", (int)"/vp/d/f") != 0) {
        fail("rename g back to f");
    }

    /* ---- offsets and sizes ---- */
    fd = sys3(SYS_OPEN, (int)"/vp/d/f", O_RDONLY, 0);
    if (sys3(SYS_LSEEK, fd, 1, SEEK_SET) != 1) {
        fail("lseek to 1");
    }
    if (sys3(SYS_READ, fd, (int)iobuf, 1) != 1 || iobuf[0] != 'b') {
        fail("read after lseek");
    }
    if (sys3(SYS_LSEEK, fd, 0, SEEK_END) != 3) {
        fail("SEEK_END reports the size");
    }
    if (sys1(SYS_FSYNC, fd) != 0) {
        fail("fsync");
    }
    if (sys1(SYS_SYNCFS, fd) != 0 || sys1(SYS_SYNCFS, 999) != -EBADF) {
        fail("syncfs flushes through an open descriptor and refuses a closed one");
    }
    sys1(SYS_CLOSE, fd);
    fd = sys3(SYS_OPEN, (int)"/vp/d/f", O_RDWR, 0);
    if (sys2(SYS_FTRUNCATE, fd, 1) != 0) {
        fail("ftruncate to 1");
    }
    if (sys3(SYS_LSEEK, fd, 0, SEEK_END) != 1) {
        fail("the ftruncated size reads back");
    }
    sys1(SYS_CLOSE, fd);
    fd = sys3(SYS_OPEN, (int)"/vp/d/f", O_WRONLY | O_TRUNC, 0);
    if (fd < 0) {
        fail("open with O_TRUNC");
    }
    if (sys3(SYS_WRITE, fd, (int)"abc", 3) != 3) {
        fail("refill after O_TRUNC");
    }
    sys1(SYS_CLOSE, fd);
    fd = sys3(SYS_OPEN, (int)"/vp/d/f", O_RDONLY, 0);
    if (sys3(SYS_READ, fd, (int)iobuf, 8) != 3 || iobuf[0] != 'a' || iobuf[1] != 'b' ||
        iobuf[2] != 'c') {
        fail("the truncated file reads back what was refilled");
    }
    sys1(SYS_CLOSE, fd);

    /* ---- a relocated file keeps every description ---- */
    /* Two descriptions on one file; a neighbour is placed right after it so
       the growth cannot extend in place and the node moves. The second
       description must follow the move: its size and its far bytes are the
       new node's. */
    fd = sys3(SYS_OPEN, (int)"/vp/d/grow", O_CREAT | O_RDWR, 0644);
    if (fd < 0 || sys3(SYS_WRITE, fd, (int)"seed", 4) != 4) {
        fail("create the file that will grow");
    }
    dfd = sys3(SYS_OPEN, (int)"/vp/d/wall", O_CREAT | O_RDWR, 0644);
    if (dfd < 0 || sys3(SYS_WRITE, dfd, (int)iobuf, 512) != 512) {
        fail("create the neighbour");
    }
    sys1(SYS_CLOSE, dfd);
    dfd = sys3(SYS_OPEN, (int)"/vp/d/grow", O_RDONLY, 0);
    if (dfd < 0) {
        fail("the second description on the growing file");
    }
    for (rc = 0; rc < 128; rc++) {
        iobuf[0] = (unsigned char)rc;
        if (sys3(SYS_WRITE, fd, (int)iobuf, 512) != 512) {
            fail("grow the file past its neighbour");
        }
    }
    if (sys3(SYS_LSEEK, dfd, 0, SEEK_END) != 4 + 128 * 512) {
        fail("the second description sees the grown size");
    }
    if (sys3(SYS_LSEEK, dfd, 4 + 127 * 512, SEEK_SET) < 0 ||
        sys3(SYS_READ, dfd, (int)iobuf, 1) != 1 || iobuf[0] != 127) {
        fail("the second description reads the moved bytes");
    }
    sys1(SYS_CLOSE, dfd);
    sys1(SYS_CLOSE, fd);
    if (sys1(SYS_UNLINK, (int)"/vp/d/grow") != 0 || sys1(SYS_UNLINK, (int)"/vp/d/wall") != 0) {
        fail("remove the growth files");
    }

    /* ---- directories ---- */
    dfd = sys3(SYS_OPEN, (int)"/vp/d", O_RDONLY, 0);
    rc = sys3(SYS_GETDENTS64, dfd, (int)iobuf, sizeof(iobuf));
    if (rc <= 0 || !dirents_contain(iobuf, rc, "f")) {
        fail("getdents64 lists the directory's file");
    }
    sys1(SYS_CLOSE, dfd);
    if (sys2(SYS_MKDIR, (int)"/vp/d", 0755) != -EEXIST) {
        fail("mkdir over a directory is EEXIST");
    }
    if (sys1(SYS_RMDIR, (int)"/vp/d") != -ENOTEMPTY) {
        fail("rmdir of a full directory is ENOTEMPTY");
    }
    if (sys1(SYS_RMDIR, (int)"/vp/d/f") != -ENOTDIR) {
        fail("rmdir of a file is ENOTDIR");
    }
    if (sys1(SYS_UNLINK, (int)"/vp/d") != -EISDIR) {
        fail("unlink of a directory is EISDIR");
    }

    /* ---- attributes ---- */
    if (sys2(SYS_CHMOD, (int)"/vp/d/f", 0640) != 0 || xstat("/vp/d/f") != 0 ||
        (st_field(16) & 0777) != 0640) {
        fail("chmod sets the mode stat reports");
    }
    if (sys4(SYS_UTIMENSAT, AT_FDCWD, (int)"/vp/d/f", 0, 0) != 0) {
        fail("utimensat with the current time");
    }
    {
        int ts[4];

        ts[0] = 1000000000; /* atime */
        ts[1] = 0;
        ts[2] = 1234567890; /* mtime */
        ts[3] = 0;
        if (sys4(SYS_UTIMENSAT, AT_FDCWD, (int)"/vp/d/f", (int)ts, 0) != 0 ||
            xstat("/vp/d/f") != 0 || st_field(72) != 1234567890u) {
            fail("utimensat sets the time stat reports");
        }
    }

    check_link(magic);
    check_blocks(magic);

    /* ---- statfs identities ---- */
    if (sys3(SYS_STATFS64, (int)"/proc", 84, (int)iobuf) != 0 ||
        *(unsigned *)iobuf != STATFS_MAGIC_PROC) {
        fail("statfs64 of /proc");
    }
    if (sys3(SYS_STATFS64, (int)"/", 84, (int)iobuf) != 0 || *(unsigned *)iobuf != magic) {
        fail("statfs64 of the root");
    }
    /* f_files at 32 and f_ffree at 40: counted on ext2, not on KRFFS. */
    if (magic == STATFS_MAGIC_EXT2 ? *(unsigned *)(iobuf + 32) == 0 ||
                                         *(unsigned *)(iobuf + 40) > *(unsigned *)(iobuf + 32)
                                   : *(unsigned *)(iobuf + 32) != 0) {
        fail("statfs64 of the root counts its inodes");
    }

    /* ---- the working directory ---- */
    if (sys1(SYS_CHDIR, (int)"/vp/d") != 0) {
        fail("chdir");
    }
    if (sys2(SYS_GETCWD, (int)cwd, sizeof(cwd)) <= 0 || cwd[0] != '/' || cwd[1] != 'v' ||
        cwd[2] != 'p' || cwd[3] != '/' || cwd[4] != 'd' || cwd[5] != 0) {
        fail("getcwd reports /vp/d");
    }
    fd = sys3(SYS_OPEN, (int)"f", O_RDONLY, 0);
    if (fd < 0) {
        fail("a relative open in the working directory");
    }
    sys1(SYS_CLOSE, fd);
    if (sys1(SYS_CHDIR, (int)"/") != 0) {
        fail("chdir back to the root");
    }

    if (sys1(SYS_SYNC, 0) != 0) {
        fail("sync");
    }

    /* ---- leave the image as it was found ---- */
    if (sys1(SYS_UNLINK, (int)"/vp/d/f") != 0 || sys1(SYS_RMDIR, (int)"/vp/d") != 0 ||
        sys1(SYS_RMDIR, (int)"/vp") != 0) {
        fail("cleanup");
    }

    puts("VFSPROBE OK");
    sys_exit(0);
}
