/**
 * The path and descriptor contract the VFS migration must preserve.
 *
 * Each row is one promise: the errno a path answers, what a description
 * keeps across unlink and rename, what the synthetic mounts are and are
 * not, and the statfs identities. The rows run before the migration starts
 * and after every family it moves, so a family that changes an answer is
 * caught the commit it moves.
 *
 * `PHYSICAL_DOTDOT` marks the rows whose correct answer changes when `..`
 * stops being lexical: through a missing or non-directory component a
 * lexical resolver succeeds and a physical one must not. It flipped to 1
 * in the migration commit that removed the lexical step.
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
#define SYS_CREAT      8
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
#define SYS_GETCWD     183
#define SYS_STAT64     195
#define SYS_LSTAT64    196
#define SYS_FSTAT64    197
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
#define EEXIST    17
#define ENOTDIR   20
#define EISDIR    21
#define ENOTEMPTY 39

#define SEEK_SET 0
#define SEEK_END 2

#define STATFS_MAGIC_PROC  0x9FA0
#define STATFS_MAGIC_KRFFS 0x4B524646

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

static int xstat(const char *path)
{
    char st[104];

    st[0] = 0;
    return sys2(SYS_STAT64, (int)path, (int)st);
}

static unsigned char iobuf[512];

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
    if (sys3(SYS_READ, fd, (int)iobuf, 8) != 3) {
        fail("the truncated file reads back what was refilled");
    }
    sys1(SYS_CLOSE, fd);

    /* ---- directories ---- */
    dfd = sys3(SYS_OPEN, (int)"/vp/d", O_RDONLY, 0);
    rc = sys3(SYS_GETDENTS64, dfd, (int)iobuf, sizeof(iobuf));
    if (rc <= 0) {
        fail("getdents64 lists the directory");
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
    if (sys2(SYS_CHMOD, (int)"/vp/d/f", 0640) != 0) {
        fail("chmod");
    }
    if (sys4(SYS_UTIMENSAT, AT_FDCWD, (int)"/vp/d/f", 0, 0) != 0) {
        fail("utimensat with the current time");
    }

    /* ---- links ---- */
    if (sys2(SYS_LINK, (int)"/vp/d/f", (int)"/vp/d/h") != -EPERM) {
        fail("link on a file system without links is EPERM");
    }

    /* ---- statfs identities ---- */
    if (sys3(SYS_STATFS64, (int)"/proc", 84, (int)iobuf) != 0 ||
        *(unsigned *)iobuf != STATFS_MAGIC_PROC) {
        fail("statfs64 of /proc");
    }
    if (sys3(SYS_STATFS64, (int)"/", 84, (int)iobuf) != 0 ||
        *(unsigned *)iobuf != STATFS_MAGIC_KRFFS) {
        fail("statfs64 of the root");
    }

    /* ---- the working directory ---- */
    if (sys1(SYS_CHDIR, (int)"/vp/d") != 0) {
        fail("chdir");
    }
    if (sys2(SYS_GETCWD, (int)cwd, sizeof(cwd)) <= 0 || cwd[0] != '/' || cwd[1] != 'v' ||
        cwd[4] != 'd') {
        fail("getcwd reports the directory");
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
