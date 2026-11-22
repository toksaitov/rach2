/**
 * mount(2) and umount2(2) on a FAT32 partition, and the lifetime of a mount.
 *
 * The suite's disk has a FAT32 volume on /dev/disk0p2 holding `hello.txt`
 * and a copy of the `hello` program. The rows: a writable mount is placed
 * and taken away, then a read-only one is placed, and a tmpfs over it hides
 * its files until it is taken away; the files on it stat, open, read and
 * list, its statfs is vfat's, and a symlink onto a name that exists there
 * is EEXIST before the mount's EROFS; a program on a `noexec` mount is
 * refused with EACCES though its mode allows it, and a readable mapping of
 * it is refused execute by mprotect with EACCES; a mount is busy while any
 * description on it lives, in this process or a forked one, through `dup`,
 * `dup2` and `F_DUPFD`, and a close-on-exec descriptor is dropped by
 * `execve`; a working directory inside it is busy too; MNT_DETACH takes a
 * busy mount from view and its last close unmounts it; the refused flags
 * and targets answer EINVAL, a bind of a device ENOTDIR, a long target
 * ENAMETOOLONG, a rename across EXDEV, and the mount point refuses `rmdir`,
 * `unlink` and `rename` with EBUSY; an unprivileged caller gets EPERM; `..`
 * climbs out of the mount physically; /proc/mounts and /proc/filesystems
 * say what is mounted; the kernel's record of an unmount counts
 * descriptions, not descriptors; a mount namespace has its own copy of
 * each mount it inherits, which it remounts and unmounts alone
 * (`rows_namespaces`); and a name relative to a descriptor or the working
 * directory is walked from that directory though a mount made since covers
 * it (`rows_covered`), /dev/pts among them, and though one comes while the
 * walk is under way, and a walk follows 40 links and no more (`rows_walks`).
 * `mountprobe claim` runs the boot partition's rows alone (`rows_claim`),
 * and `mountprobe reloc` the rows of a bind whose KRFFS source moves while
 * it waits (`rows_reloc`), on disks of their own.
 *
 * Prints "MOUNTPROBE OK" or "MOUNTPROBE FAIL: reason", or CLAIMPROBE in the
 * `claim` mode. Freestanding; direct system calls (probe.h).
 *
 * Given `machine=native` (probe_args.h), on a machine that is already up, the
 * probe runs every row but the `claim` mode's in its working directory, a
 * workspace whose absolute path is short enough for a mount's record, with
 * `hello` and `stdintest` in it: the mount points are `mnt` and `mnt2` there,
 * and the long target a name there. The vfat source is the first device of
 * `loops=DEVICE,DEVICE`, an empty FAT32 volume attached and not mounted, on
 * which the writable mount writes `hello.txt` and a copy of `hello`; the
 * second is a KRFFS volume, in place of /dev/disk0p1. The unprivileged
 * caller is the uid `ids=OLD:NEW,...` gives for 1000; the exec child reads a
 * pipe the probe holds open in place of the console; and the unmount record
 * is looked for in the whole of the kernel's ring.
 */

#include "probe.h"
#include "probe_args.h"

#define O_CREAT     0100
#define O_RDONLY    0
#define O_WRONLY    1
#define O_TRUNC     01000
#define O_CLOEXEC   02000000
#define O_NONBLOCK  04000
#define O_DIRECTORY 0200000
#define AT_FDCWD    (-100)
#define F_DUPFD     0
#define S_IFIFO     0010000
#define WNOHANG     1

#define CLONE_NEWNS 0x00020000
#define CLONE_FILES 0x00000400
#define CLONE_FS    0x00000200
#define SIGCHLD     17

/* setns(2), which probe.h does not name, and the offset of f_flags in the
 * statfs `probe_statfs` fills, i386's statfs64 or x86-64's statfs. */
#if defined __x86_64__
#define SYS_SETNS     308
#define KSTATFS_FLAGS 80
#else
#define SYS_SETNS     346
#define KSTATFS_FLAGS 64
#endif
#define ST_RDONLY 1

#define MS_RDONLY       1
#define MS_NOSUID       2
#define MS_NOEXEC       8
#define MS_REMOUNT      32
#define MS_BIND         4096
#define MS_MOVE         8192
#define UMOUNT_NOFOLLOW 8
#define MNT_FORCE       1
#define MNT_DETACH      2

#define EPERM         1
#define ENOENT        2
#define EACCES        13
#define EMFILE        24
#define RLIMIT_NOFILE 7
#define EBADF         9
#define EBUSY         16
#define EEXIST        17
#define EXDEV         18
#define ENOTDIR       20
#define EINVAL        22
#define EROFS         30
#define ENAMETOOLONG  36
#define ELOOP         40

#define STATFS_MAGIC_VFAT  0x4d44
#define STATFS_MAGIC_KRFFS 0x4B524646u

#define PROT_READ   1
#define PROT_EXEC   4
#define MAP_SHARED  1
#define MAP_PRIVATE 2

/** exit(2) with `status`; does not return. */
static void sys_exit(int status) __attribute__((noreturn));
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

static int claim_mode;         /* the `claim` mode, whose lines say CLAIMPROBE */
static int row_child;          /* a row's child that `fail` kills and reaps, or 0 */
static void (*row_undo)(void); /* a row's undoing, which `fail` runs, or 0 */

/** Print the mode's FAIL line with `why`, and exit 1, once the row's child
 * is killed and reaped and what it made is undone. */
static void fail(const char *why) __attribute__((noreturn));
static void fail(const char *why)
{
    void (*undo)(void) = row_undo;
    int st;

    row_undo = 0;
    if (row_child > 0) {
        syscall2(SYS_KILL, row_child, 9);
        syscall4(SYS_WAIT4, row_child, (long)&st, 0, 0);
        row_child = 0;
    }
    if (undo) {
        undo();
    }
    if (claim_mode) {
        syscall3(SYS_WRITE, 1, (long)"CLAIMPROBE FAIL: ", 17);
    } else {
        syscall3(SYS_WRITE, 1, (long)"MOUNTPROBE FAIL: ", 17);
    }
    puts(why);
    sys_exit(1);
}

/** Whether `needle` occurs in the string `hay`. */
static int contains(const char *hay, const char *needle)
{
    const char *h;

    for (h = hay; *h; h++) {
        const char *a = h;
        const char *b = needle;

        while (*a && *b && *a == *b) {
            a++;
            b++;
        }
        if (!*b) {
            return 1;
        }
    }
    return 0;
}

/** Whether the getdents64 records in the first `n` bytes of `b` name `name`:
 * the records are binary, so they are walked by d_reclen rather than read as
 * text. */
static int dirents_contain(const unsigned char *b, int n, const char *name)
{
    int at = 0;

    while (at + 19 < n) {
        int reclen = b[at + 16] | (b[at + 17] << 8);
        const char *dn = (const char *)b + at + 19;
        const char *a = dn;
        const char *c = name;

        while (*a && *c && *a == *c) {
            a++;
            c++;
        }
        if (!*a && !*c) {
            return 1;
        }
        if (reclen <= 0) {
            break;
        }
        at += reclen;
    }
    return 0;
}

/** stat(2) of `path`: 0 when it exists, or a negated errno. */
static int xstat(const char *path)
{
    char st[KSTAT_SIZE];

    st[0] = 0;
    return syscall2(SYS_STAT, (long)path, (long)st);
}

/** mount(2) of `src` on `target` as vfat with `flags` and `data`: 0, or a
 * negated errno. */
static int do_mount(const char *src, const char *target, unsigned flags, const char *data)
{
    return syscall5(SYS_MOUNT, (long)src, (long)target, (long)"vfat", (int)flags, (long)data);
}

/** Sleep `ms` milliseconds. */
static void sleep_ms(int ms)
{
    long ts[2];

    ts[0] = ms / 1000;
    ts[1] = (ms % 1000) * 1000000;
    /* interval: the child's name is read again every 10 ms */
    syscall2(SYS_NANOSLEEP, (long)ts, 0);
}

/** Reap `pid` and return its wait status; fail when waitpid does. */
static int wait_status(int pid)
{
    int status = 0;

    if (syscall3(SYS_WAITPID, pid, (long)&status, 0) != pid) {
        fail("waitpid");
    }
    return status;
}

/**
 * Whether process `pid` runs the program `name`, by the name in its
 * /proc/<pid>/stat, which execve sets after it has dropped the close-on-exec
 * descriptors: 1 when it does, 0 when it runs another, and -1 when the
 * process has ended, its file gone or its state Z.
 */
static int runs_program(int pid, const char *name)
{
    char path[32] = "/proc/";
    char digits[12], line[256];
    int i = 0, j = 6, fd, n;

    do {
        digits[i++] = (char)('0' + pid % 10);
        pid /= 10;
    } while (pid > 0);
    while (i > 0) {
        path[j++] = digits[--i];
    }
    for (i = 0; "/stat"[i] != '\0'; i++) {
        path[j++] = "/stat"[i];
    }
    path[j] = '\0';
    fd = syscall3(SYS_OPEN, (long)path, O_RDONLY, 0);
    if (fd < 0) {
        return -1;
    }
    n = syscall3(SYS_READ, fd, (long)line, (int)sizeof(line) - 1);
    syscall1(SYS_CLOSE, fd);
    for (i = n - 1; i >= 0 && line[i] != ')'; i--) {
    }
    if (i < 0 || i + 2 >= n || line[i + 2] == 'Z') {
        return -1;
    }
    for (j = 0; j < i && line[j] != '('; j++) {
    }
    for (j++; j < i && *name && line[j] == *name; j++) {
        name++;
    }
    return j == i && !*name;
}

static unsigned char buf[4096];

#define SRC "/dev/disk0p2"
#define MNT "/mnt"

/* The paths the rows use: the test image's, or a native run's (`native_paths`).
 * `mnt_abs` is the mount point's absolute path, as /proc/mounts and the
 * unmount record write it, and has the room a mount's record has. */
static const char *src = SRC;
static const char *krffs_src = "/dev/disk0p1";
static const char *mnt = MNT;
static const char *mnt2 = "/mnt2";
static char mnt_abs[64] = MNT;

/** `a`, `b` and `c` joined in `out` of `cap` bytes, cut to fit; returns
 * `out`. */
static char *join3(char *out, int cap, const char *a, const char *b, const char *c)
{
    const char *parts[3];
    int k = 0, i, j;

    parts[0] = a;
    parts[1] = b;
    parts[2] = c;
    for (j = 0; j < 3; j++) {
        for (i = 0; parts[j][i] != '\0' && k < cap - 1; i++) {
            out[k++] = parts[j][i];
        }
    }
    out[k] = '\0';
    return out;
}

/** The mount point's path followed by `rest`, in the next of four buffers,
 * so that a call may take two of them. */
static const char *in_mnt(const char *rest)
{
    static char paths[4][96];
    static int next;
    char *p = paths[next];

    next = (next + 1) % 4;
    return join3(p, (int)sizeof(paths[0]), mnt, rest, "");
}

/** A name in the file system above the mount reached by `..` from its root:
 * the test image's /etc, or a native run's staged `hello`. */
static const char *above_mnt(void)
{
    return in_mnt(probe_native() ? "/../hello" : "/../etc");
}

/** The path of `name`, a file at the root of the test image or in the
 * workspace of a native run. */
static const char *at_top(const char *name)
{
    static char paths[2][64];
    static int next;
    char *p = paths[next];

    next = (next + 1) % 2;
    return join3(p, (int)sizeof(paths[0]), probe_native() ? "" : "/", name, "");
}

/** Copy the `n`th device of `loops` to `out`, of `cap` bytes; fail when
 * there is none. */
static const char *loop_device(int n, char *out, int cap)
{
    const char *s = probe_arg("loops", "");
    int k = 0;

    for (; n > 0 && *s != '\0'; s++) {
        if (*s == ',') {
            n--;
        }
    }
    while (*s != '\0' && *s != ',' && k < cap - 1) {
        out[k++] = *s++;
    }
    out[k] = '\0';
    if (k == 0) {
        fail("loops= names no device for a volume");
    }
    return out;
}

/** Set the paths of a native run: the mount points in the working directory,
 * and the two volumes from `loops`. */
static void native_paths(void)
{
    static char vfat_dev[48], other_dev[48];
    char cwd[64] = {0};
    int n = syscall2(SYS_GETCWD, (long)cwd, (int)sizeof(cwd));

    if (n <= 0 || (int)strlen(cwd) + 5 > (int)sizeof(mnt_abs)) {
        fail("the working directory's path is too long for a mount's record");
    }
    join3(mnt_abs, (int)sizeof(mnt_abs), cwd, "/mnt", "");
    mnt = "mnt";
    mnt2 = "mnt2";
    src = loop_device(0, vfat_dev, (int)sizeof(vfat_dev));
    krffs_src = loop_device(1, other_dev, (int)sizeof(other_dev));
}

/** Write `hello.txt` and a copy of the program `hello` on the writable mount
 * of a native run's empty volume, as the mount suite puts them on its own;
 * without `machine=native` do nothing, since the suite's volume has them. */
static void fill_volume(void)
{
    static const char text[] = "hello from the mount suite\n";
    int in, out, n;

    if (!probe_native()) {
        return;
    }
    out = syscall3(SYS_OPEN, (long)in_mnt("/hello.txt"), O_WRONLY | O_CREAT | O_TRUNC, 0644);
    if (out < 0 || syscall3(SYS_WRITE, out, (long)text, (int)sizeof(text) - 1) != 27) {
        fail("hello.txt is written on the writable mount");
    }
    syscall1(SYS_CLOSE, out);
    in = syscall3(SYS_OPEN, (long)"hello", O_RDONLY, 0);
    out = syscall3(SYS_OPEN, (long)in_mnt("/hello"), O_WRONLY | O_CREAT | O_TRUNC, 0755);
    if (in < 0 || out < 0) {
        fail("hello is copied to the writable mount");
    }
    while ((n = syscall3(SYS_READ, in, (long)buf, (int)sizeof(buf))) > 0) {
        if (syscall3(SYS_WRITE, out, (long)buf, n) != n) {
            fail("hello is copied to the writable mount");
        }
    }
    syscall1(SYS_CLOSE, in);
    syscall1(SYS_CLOSE, out);
    if (n < 0) {
        fail("hello is copied to the writable mount");
    }
}

/** Read the /proc file `path` into `buf` as text, with one read of at most
 * 4095 bytes; fail when it does not open or read. */
static void read_text(const char *path)
{
    int fd = syscall3(SYS_OPEN, (long)path, O_RDONLY, 0);
    int n;

    if (fd < 0) {
        fail(path);
    }
    n = syscall3(SYS_READ, fd, (long)buf, sizeof(buf) - 1);
    if (n < 0) {
        fail("reading a /proc file");
    }
    buf[n] = 0;
    syscall1(SYS_CLOSE, fd);
}

/**
 * The row of a directory renamed on the writable mount: a descriptor of it
 * opened before the rename makes a file in it after, since the type rebuilds
 * the path from the volume, and the file and the directory are removed.
 */
static void row_renamed_dir(void)
{
    int rfd, xfd;

    if (syscall2(SYS_MKDIR, (long)in_mnt("/rdir"), 0755) != 0) {
        fail("mkdir on the writable mount");
    }
    rfd = syscall3(SYS_OPEN, (long)in_mnt("/rdir"), O_RDONLY, 0);
    if (rfd < 0 || syscall2(SYS_RENAME, (long)in_mnt("/rdir"), (long)in_mnt("/rdir2")) != 0) {
        fail("open and rename a directory on the mount");
    }
    xfd = syscall4(SYS_OPENAT, rfd, (long)"x", O_WRONLY | O_CREAT, 0644);
    if (xfd < 0 || xstat(in_mnt("/rdir2/x")) != 0) {
        fail("a directory descriptor follows its directory's rename");
    }
    syscall1(SYS_CLOSE, xfd);
    syscall1(SYS_CLOSE, rfd);
    if (syscall1(SYS_UNLINK, (long)in_mnt("/rdir2/x")) != 0) {
        fail("unlink on the writable mount");
    }
    if (syscall1(SYS_RMDIR, (long)in_mnt("/rdir2")) != 0) {
        fail("unlink and rmdir on the writable mount");
    }
}

/**
 * The rows that place and refuse mounts: a writable mount, on which a
 * directory descriptor follows a rename of its directory
 * (`row_renamed_dir`); its removal; the refused flags,
 * targets, sources and options; and the read-only mount the later rows use,
 * a tmpfs stacked over it and taken away, and the second mount it makes busy.
 *
 * F_DUPFD is tried on the root before any mount, so that a later failure of
 * it is the mount's and not the table's. A second mount on the same target
 * goes over the first and hides its files, as on Linux, and an unmount takes
 * the top one.
 */
static void rows_placing(void)
{
    int fd, d;

    fd = syscall3(SYS_OPEN, (long)at_top("hello"), O_RDONLY, 0);
    d = syscall3(SYS_FCNTL, fd, F_DUPFD, 10);
    if (fd < 0 || d != 10) {
        fail("F_DUPFD on the root");
    }
    syscall1(SYS_CLOSE, d);
    syscall1(SYS_CLOSE, fd);
    if (syscall2(SYS_MKDIR, (long)mnt, 0755) != 0) {
        fail("mkdir " MNT);
    }
    if (do_mount(src, mnt, 0, 0) != 0) {
        fail("a writable mount is placed");
    }
    row_renamed_dir();
    fill_volume();
    if (syscall2(SYS_UMOUNT2, (long)mnt, 0) != 0) {
        fail("the writable mount is taken away");
    }
    if (do_mount(src, mnt, MS_RDONLY | MS_REMOUNT, 0) != -EINVAL ||
        do_mount(src, mnt, MS_RDONLY | MS_BIND, 0) != -ENOTDIR ||
        do_mount(src, mnt, MS_RDONLY | MS_MOVE, 0) != -EINVAL) {
        fail("MS_REMOUNT of no mount and MS_MOVE are EINVAL, and MS_BIND of a device ENOTDIR");
    }
    if (do_mount(src, "/proc", MS_RDONLY, 0) != -EINVAL) {
        fail("a target on /proc is EINVAL");
    }
    if (do_mount(src, "/", MS_RDONLY, 0) != -EINVAL) {
        fail("the root as a target is EINVAL");
    }
    if (do_mount("/dev/null", mnt, MS_RDONLY, 0) != -EINVAL) {
        fail("a source that is not a partition window is EINVAL");
    }
    if (do_mount(src, mnt, MS_RDONLY, "nosuch=1") != -EINVAL) {
        fail("an unknown option is EINVAL");
    }
    if (do_mount(src, mnt, MS_RDONLY, 0) != 0) {
        fail("the read-only mount");
    }
    if (syscall5(SYS_MOUNT, (long)"none", (long)mnt, (long)"tmpfs", 0, 0) != 0 ||
        xstat(in_mnt("/hello.txt")) != -ENOENT) {
        fail("a second mount on the same target goes over the first");
    }
    if (syscall2(SYS_UMOUNT2, (long)mnt, 0) != 0 || xstat(in_mnt("/hello.txt")) != 0) {
        fail("an unmount takes the top mount and shows the one below");
    }
    if (do_mount(src, mnt2, MS_RDONLY, 0) != -ENOENT) {
        fail("a missing target is ENOENT");
    }
    if (syscall2(SYS_MKDIR, (long)mnt2, 0755) != 0) {
        fail("mkdir /mnt2");
    }
    if (do_mount(src, mnt2, MS_RDONLY, 0) != -EBUSY) {
        fail("a second mount over a claimed range is EBUSY");
    }
    if (do_mount(krffs_src, mnt2, MS_RDONLY, 0) != -EINVAL) {
        fail("the KRFFS partition is not a vfat volume");
    }
}

/**
 * The rows of the read-only mount: its file stats, opens and reads, writes
 * of every kind are EROFS (a symlink onto a name that exists is EEXIST
 * first), getdents64 refuses a buffer too small for one record with EINVAL
 * and still lists the directory after, statfs is vfat's, `..` climbs out,
 * a rename across is EXDEV, and the mount point is EBUSY. The refused
 * getdents64 must give the description back: a read that kept it would
 * leave every later read of the directory, through this descriptor or a
 * copy of it, waiting for a lock nothing releases.
 */
static void rows_reading(void)
{
    int fd, n;

    if (xstat(in_mnt("/hello.txt")) != 0) {
        fail("stat of a file on the mount");
    }
    fd = syscall3(SYS_OPEN, (long)in_mnt("/hello.txt"), O_RDONLY, 0);
    if (fd < 0) {
        fail("open on the mount");
    }
    n = syscall3(SYS_READ, fd, (long)buf, sizeof(buf));
    if (n != 27) {
        fail("the file's length");
    }
    buf[n] = 0;
    if (!contains((const char *)buf, "hello from the mount suite")) {
        fail("the file's bytes");
    }
    syscall1(SYS_CLOSE, fd);
    if (syscall3(SYS_OPEN, (long)in_mnt("/hello.txt"), O_WRONLY, 0) != -EROFS) {
        fail("a write-open on the read-only mount is EROFS");
    }
    if (syscall2(SYS_MKDIR, (long)in_mnt("/newdir"), 0755) != -EROFS) {
        fail("mkdir on the read-only mount is EROFS");
    }
    if (syscall2(SYS_SYMLINK, (long)"x", (long)in_mnt("/hello.txt")) != -EEXIST ||
        syscall2(SYS_SYMLINK, (long)"x", (long)in_mnt("/newlink")) != -EROFS) {
        fail("symlink on the read-only mount is EEXIST for a name that exists, else EROFS");
    }
    if (syscall1(SYS_UNLINK, (long)in_mnt("/hello.txt")) != -EROFS) {
        fail("unlink on the read-only mount is EROFS");
    }
    fd = syscall3(SYS_OPEN, (long)mnt, O_RDONLY, 0);
    if (fd < 0) {
        fail("open the mount root");
    }
    if (syscall3(SYS_GETDENTS64, fd, (long)buf, 1) != -EINVAL) {
        fail("getdents64 with a buffer too small for one record is EINVAL");
    }
    n = syscall3(SYS_GETDENTS64, fd, (long)buf, sizeof(buf));
    if (n <= 0 || !dirents_contain(buf, n, "hello.txt")) {
        fail("getdents64 lists the mount after the refused read");
    }
    {
        int f2 = syscall4(SYS_OPENAT, fd, (long)"hello.txt", O_RDONLY, 0);

        if (f2 < 0 || syscall3(SYS_READ, f2, (long)buf, 8) != 8) {
            fail("openat through the mount's directory descriptor");
        }
        syscall1(SYS_CLOSE, f2);
    }
    syscall1(SYS_CLOSE, fd);
    if (probe_statfs(in_mnt("/hello.txt"), buf) != 0 || *(unsigned *)buf != STATFS_MAGIC_VFAT) {
        fail("statfs64 on the mount is vfat's");
    }
    if (xstat(in_mnt("/nosuch")) != -ENOENT) {
        fail("a missing name on the mount is ENOENT");
    }
    if (xstat(in_mnt("/hello.txt/x")) != -ENOTDIR) {
        fail("a file used as a directory on the mount is ENOTDIR");
    }
    if (xstat(in_mnt("/../mnt/hello.txt")) != 0) {
        fail("dot-dot climbs out of the mount and back in");
    }
    if (xstat(above_mnt()) != 0) {
        fail("dot-dot at the mount root reaches the parent file system");
    }
    if (syscall2(SYS_RENAME, (long)in_mnt("/hello.txt"), (long)at_top("hello-moved")) != -EXDEV) {
        fail("rename across mounts is EXDEV");
    }
    if (syscall1(SYS_RMDIR, (long)mnt) != -EBUSY) {
        fail("rmdir of the mount point is EBUSY");
    }
    if (syscall1(SYS_UNLINK, (long)mnt) != -EBUSY) {
        fail("unlink of the mount point is EBUSY");
    }
    if (syscall2(SYS_RENAME, (long)mnt, (long)at_top("mnt3")) != -EBUSY) {
        fail("rename of the mount point is EBUSY");
    }
}

/** /proc/mounts lists the mount, and /proc/filesystems the types. */
static void rows_proc(void)
{
    char want[128];

    read_text("/proc/mounts");
    join3(want, (int)sizeof(want), src, " ", mnt_abs);
    join3(want + strlen(want), (int)(sizeof(want) - strlen(want)), " vfat ro", "", "");
    if (!contains((const char *)buf, want)) {
        fail("/proc/mounts names the mount");
    }
    read_text("/proc/filesystems");
    if (!contains((const char *)buf, "nodev\tproc") || !contains((const char *)buf, "\tvfat") ||
        !contains((const char *)buf, "\texfat")) {
        fail("/proc/filesystems names the types");
    }
}

/**
 * A working directory inside the mount keeps it busy: a child enters the
 * mount, reports its chdir through a pipe, and leaves the mount, by exiting,
 * when the parent closes the pipe `go` after its unmount was refused.
 */
static void row_working_directory(void)
{
    int pid, opened[2], go[2];
    char byte = 0;

    opened[0] = opened[1] = go[0] = go[1] = -1;
    if (syscall1(SYS_PIPE, (long)opened) != 0 || syscall1(SYS_PIPE, (long)go) != 0) {
        fail("the pipes for the chdir child");
    }
    pid = syscall1(SYS_FORK, 0);
    if (pid == 0) {
        syscall1(SYS_CLOSE, opened[0]);
        syscall1(SYS_CLOSE, go[1]);
        if (syscall1(SYS_CHDIR, (long)mnt) != 0) {
            sys_exit(9);
        }
        syscall3(SYS_WRITE, opened[1], (long)"c", 1);
        syscall3(SYS_READ, go[0], (long)&byte, 1);
        sys_exit(0);
    }
    syscall1(SYS_CLOSE, opened[1]);
    syscall1(SYS_CLOSE, go[0]);
    if (syscall3(SYS_READ, opened[0], (long)&byte, 1) != 1) {
        fail("the chdir child did not enter the mount");
    }
    syscall1(SYS_CLOSE, opened[0]);
    if (syscall2(SYS_UMOUNT2, (long)mnt, 0) != -EBUSY) {
        fail("umount2 with a working directory inside is EBUSY");
    }
    syscall1(SYS_CLOSE, go[1]);
    if (wait_status(pid) != 0) {
        fail("the chdir child");
    }
    if (syscall2(SYS_UMOUNT2, (long)mnt, 0) != 0) {
        fail("umount2 after the working directory left");
    }
}

/**
 * The exec child of `rows_lifetime`: open the file on the mount
 * close-on-exec, say so through the pipe `opened`, and execute `stdintest`,
 * whose standard input is the pipe `input` in a native run, which has no
 * console. Exits 7 when the open fails, 6 when the pipe cannot become
 * standard input, and 8 when the exec fails.
 */
static void exec_child(const int opened[2], const int input[2])
{
    char *av[2];

    av[0] = (char *)(probe_native() ? "./stdintest" : "/stdintest");
    av[1] = 0;
    if (syscall3(SYS_OPEN, (long)in_mnt("/hello.txt"), O_RDONLY | O_CLOEXEC, 0) < 0) {
        sys_exit(7);
    }
    if (probe_native() && syscall2(SYS_DUP2, input[0], 0) != 0) {
        sys_exit(6);
    }
    syscall3(SYS_WRITE, opened[1], (long)"o", 1);
    syscall1(SYS_CLOSE, opened[1]);
    syscall1(SYS_CLOSE, opened[0]);
    syscall3(SYS_EXECVE, (long)av[0], (long)av, (long)(av + 1));
    sys_exit(8);
}

/**
 * Every way a description can outlive its opener keeps the mount busy: a
 * descriptor and its duplicates, one a forked child inherited, and a working
 * directory inside the mount (`row_working_directory`). The child that
 * inherited a descriptor closes it only when the parent closes the pipe `go`,
 * after its unmount was refused.
 *
 * A close-on-exec descriptor is dropped by execve: the child holds the only
 * description and execs `stdintest`, which blocks on the console and stays
 * alive; the mount is busy until the exec and free after it, while the child
 * still runs, which is what shows execve did it. The parent waits for the
 * child's open through a pipe, since an unmount before the open would find
 * the mount free and the row would then judge the scheduler and not execve,
 * and then for the name `stdintest` in the child's stat file, which execve
 * sets after it has dropped the descriptor, before its one unmount.
 */
static void rows_lifetime(void)
{
    int fd, d1, d2, d3, dfd, pid, started;
    int opened[2], go[2], input[2];
    char byte = 0;

    fd = syscall3(SYS_OPEN, (long)in_mnt("/hello.txt"), O_RDONLY, 0);
    dfd = syscall3(SYS_OPEN, (long)mnt, O_RDONLY, 0);
    if (fd < 0 || dfd < 0) {
        fail("open for the lifetime rows");
    }
    if (syscall2(SYS_UMOUNT2, (long)mnt, 0) != -EBUSY) {
        fail("umount2 with a file open is EBUSY");
    }
    d1 = syscall1(SYS_DUP, fd);
    if (d1 < 0) {
        fail("dup");
    }
    d2 = syscall2(SYS_DUP2, fd, 14);
    if (d2 != 14) {
        fail("dup2");
    }
    d3 = syscall3(SYS_FCNTL, fd, F_DUPFD, 10);
    if (d3 < 10) {
        static char why[32] = "F_DUPFD rc=";
        int v = -d3;

        why[11] = (char)('0' + (v / 10) % 10);
        why[12] = (char)('0' + v % 10);
        why[13] = 0;
        fail(why);
    }
    syscall1(SYS_CLOSE, fd);
    syscall1(SYS_CLOSE, dfd);
    if (syscall2(SYS_UMOUNT2, (long)mnt, 0) != -EBUSY) {
        fail("umount2 with duplicates open is EBUSY");
    }
    syscall1(SYS_CLOSE, d1);
    syscall1(SYS_CLOSE, d2);
    if (syscall2(SYS_UMOUNT2, (long)mnt, 0) != -EBUSY) {
        fail("umount2 with the last duplicate open is EBUSY");
    }
    go[0] = -1; /* the kernel fills the pair through the cast address */
    go[1] = -1;
    if (syscall1(SYS_PIPE, (long)go) != 0) {
        fail("a pipe for the inheriting child");
    }
    pid = syscall1(SYS_FORK, 0);
    if (pid < 0) {
        fail("fork");
    }
    if (pid == 0) {
        syscall1(SYS_CLOSE, go[1]);
        syscall3(SYS_READ, go[0], (long)&byte, 1);
        syscall1(SYS_CLOSE, d3);
        sys_exit(0);
    }
    syscall1(SYS_CLOSE, go[0]);
    syscall1(SYS_CLOSE, d3);
    if (syscall2(SYS_UMOUNT2, (long)mnt, 0) != -EBUSY) {
        fail("umount2 with a child's inherited descriptor is EBUSY");
    }
    syscall1(SYS_CLOSE, go[1]);
    wait_status(pid);
    if (syscall2(SYS_UMOUNT2, (long)mnt, 0) != 0) {
        fail("umount2 once every description is closed");
    }
    if (xstat(in_mnt("/hello.txt")) != -ENOENT) {
        fail("the mount is gone after umount2");
    }
    if (do_mount(src, mnt, MS_RDONLY, 0) != 0) {
        fail("the mount again after umount2");
    }

    opened[0] = -1; /* the kernel fills the pair through the cast address */
    opened[1] = -1;
    input[0] = -1;
    input[1] = -1;
    if (syscall1(SYS_PIPE, (long)opened) != 0 ||
        (probe_native() && syscall1(SYS_PIPE, (long)input) != 0)) {
        fail("a pipe for the exec child");
    }
    pid = syscall1(SYS_FORK, 0);
    if (pid == 0) {
        exec_child(opened, input);
    }
    syscall1(SYS_CLOSE, opened[1]);
    if (syscall3(SYS_READ, opened[0], (long)&byte, 1) != 1) {
        fail("the exec child did not open the file");
    }
    syscall1(SYS_CLOSE, opened[0]);
    while ((started = runs_program(pid, "stdintest")) == 0) {
        sleep_ms(10);
    }
    if (started < 0) {
        fail("the exec child ended before it ran stdintest");
    }
    if (syscall2(SYS_UMOUNT2, (long)mnt, 0) != 0) {
        fail("umount2 after the close-on-exec descriptor was dropped by execve");
    }
    if (syscall2(SYS_KILL, pid, 9) != 0 || (wait_status(pid) & 0x7f) != 9) {
        fail("the exec child was not alive across the unmount");
    }
    if (probe_native()) {
        syscall1(SYS_CLOSE, input[0]);
        syscall1(SYS_CLOSE, input[1]);
    }
    if (do_mount(src, mnt, MS_RDONLY, 0) != 0) {
        fail("the mount once more");
    }

    row_working_directory();
}

/** A target too long for the mount's record (VFS_COVERED_PATH_MAX in
 * rach2/sys/fs/vfs.h, 256 bytes with the NUL) claims nothing: a path of
 * three 99-byte names below the second mount point, longer than 300 bytes
 * from the root, is ENAMETOOLONG, and the source then mounts elsewhere. No
 * one name may pass 255 bytes, so the path is three directories deep, and
 * they are made on a tmpfs, since KRFFS keeps a whole path in 256 bytes. */
static void row_long_target(void)
{
    static char target[64 + 310];
    int i, level, base;

    for (base = 0; mnt2[base] != '\0'; base++) {
        target[base] = mnt2[base];
    }
    for (level = 0; level < 3; level++) {
        target[base + level * 100] = '/';
        for (i = 1; i < 100; i++) {
            target[base + level * 100 + i] = 'n';
        }
    }
    if (syscall5(SYS_MOUNT, (long)"none", (long)mnt2, (long)"tmpfs", 0, 0) != 0) {
        fail("a tmpfs to hold the long target");
    }
    for (level = 1; level <= 3; level++) {
        target[base + level * 100] = 0;
        if (syscall2(SYS_MKDIR, (long)target, 0755) != 0) {
            fail("mkdir of the long target");
        }
        target[base + level * 100] = '/';
    }
    target[base + 300] = 0;
    if (do_mount(src, target, MS_RDONLY, 0) != -ENAMETOOLONG) {
        fail("a long target is ENAMETOOLONG");
    }
    if (syscall2(SYS_UMOUNT2, (long)mnt2, 0) != 0) {
        fail("the tmpfs of the long target is taken away");
    }
    if (do_mount(src, mnt2, MS_RDONLY, 0) != 0 || syscall2(SYS_UMOUNT2, (long)mnt2, 0) != 0) {
        fail("the source mounts after the refused long target");
    }
}

/**
 * Map the page of `path` that begins it readable, private or shared as
 * `flags` says, and ask mprotect to make it executable: the mprotect's
 * answer, 0 or a negated errno. `what` names the row a failed open or mmap
 * fails. Linux's mmap takes a file on a noexec mount's right to execute
 * away (VM_MAYEXEC), so the answer there is -EACCES.
 */
static long map_then_exec(const char *path, long flags, const char *what)
{
    long fd = syscall3(SYS_OPEN, (long)path, O_RDONLY, 0);
    long at, rc;

    if (fd < 0) {
        fail(what);
    }
    at = probe_mmap(0, 4096, PROT_READ, flags, fd, 0);
    syscall1(SYS_CLOSE, fd);
    if (at < 0 && at > -4096) {
        fail(what);
    }
    rc = syscall3(SYS_MPROTECT, at, 4096, PROT_READ | PROT_EXEC);
    syscall2(SYS_MUNMAP, at, 4096);
    return rc;
}

/** The rows of noexec and of a program run from the mount. The noexec and
 * executable mounts pass `umask=000`, which makes every file 0777, so that
 * only noexec refuses the program. A mapping of the program made readable
 * on the noexec mount may not be made executable by mprotect, shared or
 * private, and an executable mapping of it is EPERM; on the executable
 * mount mprotect makes it executable. */
static void rows_exec(void)
{
    char *av[2];
    long fd;
    int pid;

    if (do_mount(src, mnt, MS_RDONLY | MS_NOEXEC, "umask=000") != 0) {
        fail("the noexec mount");
    }
    av[0] = (char *)in_mnt("/hello");
    av[1] = 0;
    if (syscall3(SYS_EXECVE, (long)av[0], (long)av, (long)(av + 1)) != -EACCES) {
        fail("execve on a noexec mount is EACCES");
    }
    fd = syscall3(SYS_OPEN, (long)av[0], O_RDONLY, 0);
    if (fd < 0 || probe_mmap(0, 4096, PROT_READ | PROT_EXEC, MAP_PRIVATE, fd, 0) != -EPERM) {
        fail("an executable mapping of a file on a noexec mount is EPERM");
    }
    syscall1(SYS_CLOSE, fd);
    if (map_then_exec(av[0], MAP_PRIVATE, "a private mapping on the noexec mount") != -EACCES) {
        fail("mprotect execute of a private mapping on a noexec mount is EACCES");
    }
    if (map_then_exec(av[0], MAP_SHARED, "a shared mapping on the noexec mount") != -EACCES) {
        fail("mprotect execute of a shared mapping on a noexec mount is EACCES");
    }
    if (syscall2(SYS_UMOUNT2, (long)mnt, 0) != 0) {
        fail("umount2 after the noexec row");
    }
    if (do_mount(src, mnt, MS_RDONLY, "umask=000") != 0) {
        fail("the executable mount");
    }
    if (map_then_exec(av[0], MAP_PRIVATE, "a private mapping on the executable mount") != 0) {
        fail("mprotect execute of a mapping on an executable mount");
    }
    pid = syscall1(SYS_FORK, 0);
    if (pid == 0) {
        int xr = syscall3(SYS_EXECVE, (long)av[0], (long)av, (long)(av + 1));

        sys_exit(-xr);
    }
    {
        int st = wait_status(pid);

        if ((st & 0x7f) != 0) {
            fail("a program runs from the mount: a signal ended it");
        }
        if ((st & 0xff00) != 0) {
            static char why[40] = "a program runs from the mount: errno ";
            int v = (st >> 8) & 0xff;

            why[37] = (char)('0' + (v / 10) % 10);
            why[38] = (char)('0' + v % 10);
            why[39] = 0;
            fail(why);
        }
    }
    if (syscall2(SYS_UMOUNT2, (long)mnt, 0) != 0) {
        fail("umount2 after running from the mount");
    }
}

/** The rows of noexec on /dev/zero, which maps as anonymous memory. Through
 * a bind of /dev remounted noexec, an executable private mapping is EPERM
 * and mprotect refuses execute to a readable private or shared mapping with
 * EACCES, as Linux's mmap clears VM_MAYEXEC before /dev/zero's own mmap
 * runs; through /dev, which is executable, mprotect gives it to either. */
static void rows_zero_exec(void)
{
    long fd;

    if (syscall5(SYS_MOUNT, (long)"/dev", (long)mnt, 0, MS_BIND, 0) != 0 ||
        syscall5(SYS_MOUNT, 0, (long)mnt, 0, MS_REMOUNT | MS_BIND | MS_NOEXEC, 0) != 0) {
        fail("a bind of /dev remounted noexec");
    }
    fd = syscall3(SYS_OPEN, (long)in_mnt("/zero"), O_RDONLY, 0);
    if (fd < 0 || probe_mmap(0, 4096, PROT_READ | PROT_EXEC, MAP_PRIVATE, fd, 0) != -EPERM) {
        fail("an executable mapping of /dev/zero on a noexec mount is EPERM");
    }
    syscall1(SYS_CLOSE, fd);
    if (map_then_exec(in_mnt("/zero"), MAP_PRIVATE, "a private mapping of a noexec /dev/zero") !=
        -EACCES) {
        fail("mprotect execute of a private mapping of /dev/zero on a noexec mount is EACCES");
    }
    if (map_then_exec(in_mnt("/zero"), MAP_SHARED, "a shared mapping of a noexec /dev/zero") !=
        -EACCES) {
        fail("mprotect execute of a shared mapping of /dev/zero on a noexec mount is EACCES");
    }
    if (syscall2(SYS_UMOUNT2, (long)mnt, 0) != 0) {
        fail("umount2 of the bind of /dev");
    }
    if (map_then_exec("/dev/zero", MAP_PRIVATE, "a private mapping of /dev/zero") != 0 ||
        map_then_exec("/dev/zero", MAP_SHARED, "a shared mapping of /dev/zero") != 0) {
        fail("mprotect execute of a mapping of /dev/zero on an executable mount");
    }
}

/** The rows of umount2's flags and the permanent mounts, an unprivileged
 * caller, noexec and a program run from the mount (`rows_exec`) and on
 * /dev/zero (`rows_zero_exec`), and a target too long for the mount's
 * record, which claims nothing. MNT_DETACH
 * takes a mount with two files open from view, and their last close
 * unmounts it; the two keep its record apart from the one `rows_record`
 * looks for. */
static void rows_flags(void)
{
    int pid, fd, fd2;

    if (syscall2(SYS_UMOUNT2, (long)mnt, 0) != -EINVAL) {
        fail("umount2 of a directory that is not a mount point is EINVAL");
    }
    if (do_mount(src, mnt, MS_RDONLY, 0) != 0) {
        fail("the mount for the flag rows");
    }
    if (syscall2(SYS_UMOUNT2, (long)mnt, MNT_FORCE) != -EINVAL ||
        syscall2(SYS_UMOUNT2, (long)mnt, UMOUNT_NOFOLLOW) != -EINVAL) {
        fail("MNT_FORCE and UMOUNT_NOFOLLOW are EINVAL");
    }
    fd = syscall3(SYS_OPEN, (long)in_mnt("/hello.txt"), O_RDONLY, 0);
    fd2 = syscall3(SYS_OPEN, (long)in_mnt("/hello.txt"), O_RDONLY, 0);
    if (fd < 0 || fd2 < 0 || syscall2(SYS_UMOUNT2, (long)mnt, MNT_DETACH) != 0 ||
        xstat(in_mnt("/hello.txt")) != -ENOENT || syscall3(SYS_READ, fd, (long)buf, 1) != 1) {
        fail("MNT_DETACH takes a busy mount from view, and its open files still read");
    }
    syscall1(SYS_CLOSE, fd);
    syscall1(SYS_CLOSE, fd2);
    if (do_mount(src, mnt, MS_RDONLY, 0) != 0) {
        fail("the mount after the detached one, freed by its last close");
    }
    if (syscall2(SYS_UMOUNT2, (long)"/proc", 0) != -EBUSY ||
        syscall2(SYS_UMOUNT2, (long)"/dev", 0) != -EBUSY ||
        syscall2(SYS_UMOUNT2, (long)"/", 0) != -EBUSY) {
        fail("the permanent mounts are EBUSY");
    }
    pid = syscall1(SYS_FORK, 0);
    if (pid == 0) {
        if (syscall1(SYS_SETUID, probe_id(1000)) != 0) {
            sys_exit(3);
        }
        if (syscall2(SYS_UMOUNT2, (long)mnt, 0) != -EPERM) {
            sys_exit(4);
        }
        if (do_mount(src, mnt2, MS_RDONLY, 0) != -EPERM) {
            sys_exit(5);
        }
        sys_exit(0);
    }
    if (wait_status(pid) != 0) {
        fail("an unprivileged caller is refused with EPERM");
    }
    if (syscall2(SYS_UMOUNT2, (long)mnt, 0) != 0) {
        fail("umount2 before the noexec rows");
    }
    rows_exec();
    rows_zero_exec();
    row_long_target();
}

void probe_main(unsigned long *sp);

PROBE_SP_ENTRY;

/** `mountprobe claim`, on a disk whose entry 0 is the FAT volume: the boot
 * partition mounted read-only refuses the raw writer with EBUSY, which is
 * the bootctl conflict, and takes it again once the mount is gone; a
 * mount over the writer's range, by either of its names, is EBUSY. */
static void rows_claim(void)
{
    int fd;

    if (syscall2(SYS_MKDIR, (long)MNT, 0755) != 0) {
        fail("mkdir " MNT);
    }
    if (syscall5(SYS_MOUNT, (long)"/dev/bootpart", (long)MNT, (long)"vfat", MS_RDONLY, 0) != 0) {
        fail("the boot partition mounts read-only");
    }
    if (syscall3(SYS_OPEN, (long)"/dev/bootpart", O_WRONLY, 0) != -EBUSY) {
        fail("the raw writer on a mounted range is EBUSY");
    }
    fd = syscall3(SYS_OPEN, (long)"/dev/bootpart", O_RDONLY, 0);
    if (fd < 0) {
        fail("a reader of the mounted range is allowed");
    }
    syscall1(SYS_CLOSE, fd);
    if (syscall2(SYS_UMOUNT2, (long)MNT, 0) != 0) {
        fail("umount2 of the boot partition");
    }
    fd = syscall3(SYS_OPEN, (long)"/dev/bootpart", O_WRONLY, 0);
    if (fd < 0) {
        fail("the raw writer after the unmount");
    }
    if (syscall5(SYS_MOUNT, (long)"/dev/bootpart", (long)MNT, (long)"vfat", MS_RDONLY, 0) !=
        -EBUSY) {
        fail("a mount over the raw writer's range is EBUSY");
    }
    if (syscall5(SYS_MOUNT, (long)"/dev/disk0p1", (long)MNT, (long)"vfat", MS_RDONLY, 0) !=
        -EBUSY) {
        fail("the same range under its other name is EBUSY too");
    }
    syscall1(SYS_CLOSE, fd);
    syscall1(SYS_RMDIR, (long)MNT);
    syscall3(SYS_WRITE, 1, (long)"CLAIMPROBE OK\n", 14);
    sys_exit(0);
}

/** Whether the kernel's ring, read through /dev/bootlog to its end, has the
 * text `want`, which is shorter than 128 bytes; a native run's record comes
 * after more of the ring than one read takes. */
static int ring_has(const char *want)
{
    static char log[4096 + 128];
    int len = (int)strlen(want);
    int fd = syscall3(SYS_OPEN, (long)"/dev/bootlog", O_RDONLY, 0);
    int keep = 0, n, i, j;

    if (fd < 0) {
        fail("open /dev/bootlog");
    }
    while ((n = syscall3(SYS_READ, fd, (long)(log + keep), 4096)) > 0) {
        n += keep;
        for (i = 0; i + len <= n; i++) {
            for (j = 0; j < len && log[i + j] == want[j]; j++) {
            }
            if (j == len) {
                syscall1(SYS_CLOSE, fd);
                return 1;
            }
        }
        keep = n < len - 1 ? n : len - 1;
        for (i = 0; i < keep; i++) {
            log[i] = log[n - keep + i];
        }
    }
    syscall1(SYS_CLOSE, fd);
    return 0;
}

/** The unmount record counts descriptions, not descriptors: one open and
 * three duplicates, then a duplication that fails on a table filled to a
 * limit of sixteen and one refused past the limit, end in a record of one
 * open and one release, read back from the kernel log. */
static void rows_record(void)
{
    static char log[16384 + 1];
    static char want[128];
    unsigned rl[4];
    int fd, d, filler[16], nfill = 0, i, n;

    join3(want, (int)sizeof(want), "unmounted ", mnt_abs, ": 1 opens, 1 releases");
    if (do_mount(src, mnt, MS_RDONLY, 0) != 0) {
        fail("the mount for the record row");
    }
    fd = syscall3(SYS_OPEN, (long)in_mnt("/hello.txt"), O_RDONLY, 0);
    if (fd < 0) {
        fail("open for the record row");
    }
    if (syscall1(SYS_DUP, fd) < 0 || syscall2(SYS_DUP2, fd, 13) != 13 ||
        syscall3(SYS_FCNTL, fd, F_DUPFD, 3) < 3) {
        fail("the duplicates for the record row");
    }
    rl[0] = 16;
    rl[1] = 0;
    rl[2] = 1024;
    rl[3] = 0;
    if (syscall4(SYS_PRLIMIT64, 0, RLIMIT_NOFILE, (long)rl, 0) != 0) {
        fail("lowering the descriptor limit to sixteen");
    }
    while (nfill < 16 && (d = syscall3(SYS_OPEN, (long)"/dev/null", O_RDONLY, 0)) >= 0) {
        filler[nfill++] = d;
    }
    if (syscall1(SYS_DUP, fd) != -EMFILE) {
        fail("dup on a table at its limit is EMFILE");
    }
    if (syscall2(SYS_DUP2, fd, 1024) != -EBADF) {
        fail("dup2 past the limit is EBADF");
    }
    for (i = 0; i < nfill; i++) {
        syscall1(SYS_CLOSE, filler[i]);
    }
    for (i = 3; i < 16; i++) {
        syscall1(SYS_CLOSE, i);
    }
    if (syscall2(SYS_UMOUNT2, (long)mnt, 0) != 0) {
        fail("umount2 for the record row");
    }
    if (probe_native()) {
        if (!ring_has(want)) {
            fail("the unmount record does not say 1 opens, 1 releases");
        }
        return;
    }
    fd = syscall3(SYS_OPEN, (long)"/dev/bootlog", O_RDONLY, 0);
    if (fd < 0) {
        fail("open /dev/bootlog");
    }
    n = syscall3(SYS_READ, fd, (long)log, (int)sizeof(log) - 1);
    syscall1(SYS_CLOSE, fd);
    if (n <= 0) {
        fail("read /dev/bootlog");
    }
    log[n] = 0;
    for (i = 0; i + (int)strlen(want) <= n; i++) {
        int j = 0;

        while (want[j] && log[i + j] == want[j]) {
            j++;
        }
        if (!want[j]) {
            return;
        }
    }
    fail("the unmount record does not say 1 opens, 1 releases");
}

/** Whether the strings `a` and `b` are equal. */
static int same(const char *a, const char *b)
{
    while (*a && *a == *b) {
        a++;
        b++;
    }
    return *a == *b;
}

/**
 * The mount at the mount point as /proc/self/mountinfo lists it to the
 * caller's mount namespace: its id into `*id`, and 1 when its own options
 * begin with ro, 0 when with rw; -1 when no line, or more than one, names
 * the mount point.
 */
static int mnt_info(int *id)
{
    static char info[16384 + 1];
    char *line, *next, *f[6], *q;
    int fd = syscall3(SYS_OPEN, (long)"/proc/self/mountinfo", O_RDONLY, 0);
    int n = 0, r, k, lines = 0, ro = -1;

    if (fd < 0) {
        return -1;
    }
    while (n < (int)sizeof(info) - 1 &&
           (r = syscall3(SYS_READ, fd, (long)(info + n), (int)sizeof(info) - 1 - n)) > 0) {
        n += r;
    }
    syscall1(SYS_CLOSE, fd);
    info[n] = '\0';
    for (line = info; *line; line = next) {
        for (next = line; *next && *next != '\n'; next++) {
        }
        if (*next) {
            *next++ = '\0';
        }
        for (k = 0, q = line; k < 6 && *q; k++) {
            f[k] = q;
            while (*q && *q != ' ') {
                q++;
            }
            if (*q) {
                *q++ = '\0';
            }
        }
        if (k == 6 && same(f[4], mnt_abs)) {
            lines++;
            for (*id = 0, q = f[0]; *q >= '0' && *q <= '9'; q++) {
                *id = *id * 10 + (*q - '0');
            }
            ro = f[5][0] == 'r' && f[5][1] == 'o';
        }
    }
    return lines == 1 ? ro : -1;
}

/** The mount point remounted, as mount(2) with MS_REMOUNT and MS_BIND, with
 * `flags`: 0, or a negated errno. */
static int remount_mnt(unsigned flags)
{
    return syscall5(SYS_MOUNT, 0, (long)mnt, 0, MS_REMOUNT | MS_BIND | flags, 0);
}

/** A file `name` in the mount point created for writing and closed: 0, or a
 * negated errno. */
static int create_in_mnt(const char *name)
{
    int fd = syscall3(SYS_OPEN, (long)in_mnt(name), O_WRONLY | O_CREAT | O_TRUNC, 0644);

    if (fd < 0) {
        return fd;
    }
    syscall1(SYS_CLOSE, fd);
    return 0;
}

/** The reasons a child of `rows_namespaces` gives by its exit status. */
static const char *const ns_whys[] = {
    "",
    "unshare(CLONE_NEWNS) in the child",
    "the child's remount of its copy read-only, with the parent's writer on the parent's mount",
    "a create through the child's read-only copy is EROFS",
    "the child's mountinfo lists its copy read-only under an id of its own",
    "the child's handshake with the parent",
    "a remount of the child's copy with a writer of its own on it is EBUSY",
    "the child's descriptor of its read-only copy",
    "a file made through the child's copy",
    "the child still sees its copy after the parent's unmount",
    "an rmdir through the child's copy of the root of a directory a descriptor holds is EBUSY",
    "an rmdir through the child's copy of the root of its working directory is EBUSY",
    "a file made 0600 through the child's copy of the root is 0600",
    "a directory made through the child's copy in a setgid directory takes its group and setgid "
    "bit",
    "the child's chroot to its copy of the mount point",
    "the child's remount of its copy of /proc read-only and its descriptor of it",
    "the child's mount over a directory of its copy and its descriptor of the copy",
    "a process that joins a namespace by setns has the namespace's root, not a member's chroot",
    "the child's remount of its copy of /dev read-only and its descriptors of /dev/pts and "
    "/dev/null",
    "a name relative to a descriptor of an ended namespace's /dev/pts is looked up in that "
    "directory",
    "`..` from a descriptor of an ended namespace's /dev/pts is that namespace's read-only /dev",
    "a walker from a working directory on an ended namespace's copy of the root sleeps in its "
    "open of a FIFO",
    "an rmdir of a KRFFS directory only a walk's start keeps is EBUSY",
};

/** Fail with the reason of the wait status `st` of a child of the namespace
 * rows unless it exited 0. */
static void ns_status_check(int st)
{
    int code = (st >> 8) & 0xff;

    if ((st & 0x7f) != 0) {
        fail("a child of the namespace rows ended by a signal");
    }
    if (code != 0) {
        fail(code < (int)(sizeof(ns_whys) / sizeof(ns_whys[0])) ? ns_whys[code]
                                                                : "a child of the namespace rows");
    }
}

/** Reap the child `pid` of `rows_namespaces` and fail with its reason
 * unless it exited 0. */
static void ns_child_done(int pid)
{
    ns_status_check(wait_status(pid));
}

/**
 * Wait for the one byte that the child `pid`, which shares the caller's
 * descriptors (CLONE_FILES), writes on a pipe whose nonblocking read end is
 * `fd` once it is ready: 1 when the byte came, or 0 when the child ended
 * first, with its wait status in `*st`, reaped. A blocking read would never
 * end on such a child's exit, since the write end it would close is the
 * caller's too.
 */
static int child_ready(int pid, int fd, int *st)
{
    char byte;

    for (;;) {
        if (syscall3(SYS_READ, fd, (long)&byte, 1) == 1) {
            return 1;
        }
        if (syscall4(SYS_WAIT4, pid, (long)st, WNOHANG, 0) == pid) {
            return 0;
        }
        syscall0(SYS_SCHED_YIELD);
    }
}

/**
 * Read the `len` bytes at `buf` that the forked child `pid` of a namespace
 * row writes on the pipe `c2p` once it is ready, with the caller's write
 * end closed first, so that a child that ends before it writes gives end of
 * file; fail with the child's reason, or `why`, when they do not come. From
 * here the child is the row's (`row_child`), which `fail` kills and reaps.
 */
static void fork_ready(int pid, int *c2p, void *buf, int len, const char *why)
{
    int n, st;

    if (pid < 0) {
        fail(why);
    }
    syscall1(SYS_CLOSE, c2p[1]);
    c2p[1] = -1;
    n = syscall3(SYS_READ, c2p[0], (long)buf, len);
    if (n == 0) {
        ns_child_done(pid);
        fail(why);
    }
    if (n != len) {
        syscall2(SYS_KILL, pid, 9);
        syscall4(SYS_WAIT4, pid, (long)&st, 0, 0);
        fail(why);
    }
    row_child = pid;
}

/** Detach the mount at the mount point, the tmpfs of a namespace row, for
 * `fail`. */
static void mnt_detach_undo(void)
{
    syscall2(SYS_UMOUNT2, (long)mnt, MNT_DETACH);
}

/** Close the two pipes `a` and `b` of a namespace row. */
static void close_pipes(const int *a, const int *b)
{
    syscall1(SYS_CLOSE, a[0]);
    syscall1(SYS_CLOSE, a[1]);
    syscall1(SYS_CLOSE, b[0]);
    syscall1(SYS_CLOSE, b[1]);
}

/** The child of `ns_flags_row`, which says on `out` its copy's mountinfo id
 * and reads its go from `in`; its exit status, a reason of `ns_whys`. */
static int ns_flags_child(int out, int in)
{
    int w, id = 0;
    char byte;

    if (syscall1(SYS_UNSHARE, CLONE_NEWNS) != 0) {
        return 1;
    }
    if (remount_mnt(MS_RDONLY) != 0) {
        return 2;
    }
    if (create_in_mnt("/c") != -EROFS) {
        return 3;
    }
    if (mnt_info(&id) != 1) {
        return 4;
    }
    if (syscall3(SYS_WRITE, out, (long)&id, (int)sizeof(id)) != (int)sizeof(id) ||
        syscall3(SYS_READ, in, (long)&byte, 1) != 1) {
        return 5;
    }
    if (remount_mnt(0) != 0) {
        return 6;
    }
    w = syscall3(SYS_OPEN, (long)in_mnt("/w"), O_WRONLY, 0);
    return w >= 0 && remount_mnt(MS_RDONLY) == -EBUSY ? 0 : 6;
}

/**
 * The child remounts its copy read-only while the parent has a file open
 * for writing on its own mount, since the writers that hold off a remount
 * are those of the one mount; a create through the copy is EROFS; and the
 * child's mountinfo lists the copy read-only under an id other than the
 * parent's. Meanwhile the parent's mount stays writable, its mountinfo
 * says rw, and its own remount read-only is EBUSY for its writer, as is
 * the child's for a writer of the child's.
 */
static void ns_flags_row(void)
{
    int c2p[2], p2c[2], pid, wfd, id = 0, child_id = 0;
    char byte = 0;

    wfd = syscall3(SYS_OPEN, (long)in_mnt("/w"), O_WRONLY | O_CREAT | O_TRUNC, 0644);
    if (wfd < 0 || syscall1(SYS_PIPE, (long)c2p) != 0 || syscall1(SYS_PIPE, (long)p2c) != 0) {
        fail("a writer and the pipes of the namespace rows");
    }
    pid = syscall0(SYS_FORK);
    if (pid == 0) {
        sys_exit(ns_flags_child(c2p[1], p2c[0]));
    }
    fork_ready(pid, c2p, &child_id, (int)sizeof(child_id), "the child of the first namespace row");
    if (create_in_mnt("/p") != 0) {
        fail("the parent's mount stays writable while the child's copy is read-only");
    }
    if (mnt_info(&id) != 0 || id == child_id) {
        fail("the parent's mountinfo lists its mount rw under an id other than the child's");
    }
    if (remount_mnt(MS_RDONLY) != -EBUSY) {
        fail("the parent's remount read-only with its writer open is EBUSY");
    }
    if (syscall3(SYS_WRITE, p2c[1], (long)&byte, 1) != 1) {
        fail("the parent's handshake with the child");
    }
    row_child = 0;
    ns_child_done(pid);
    syscall1(SYS_CLOSE, wfd);
    close_pipes(c2p, p2c);
}

/**
 * A child that shares the parent's descriptors (CLONE_FILES) opens its
 * read-only copy and exits, which ends its namespace; the parent then
 * remounts its own mount writable, and an fchmod through the descriptor is
 * still EROFS, while a create through the parent's path succeeds. The copy
 * goes with the descriptor's close, and the parent's mount then unmounts.
 * The descriptor is 15, below the limit of sixteen the record rows set.
 */
static void ns_desc_row(void)
{
    int pid;

    if (remount_mnt(MS_RDONLY) != 0) {
        fail("the parent's mount remounted read-only for the descriptor row");
    }
    pid = syscall5(SYS_CLONE, CLONE_NEWNS | CLONE_FILES | SIGCHLD, 0, 0, 0, 0);
    if (pid == 0) {
        int d = syscall3(SYS_OPEN, (long)mnt, O_RDONLY, 0);

        sys_exit(d >= 0 && syscall2(SYS_DUP2, d, 15) == 15 ? 0 : 7);
    }
    if (pid < 0) {
        fail("clone with CLONE_NEWNS and CLONE_FILES");
    }
    ns_child_done(pid);
    if (remount_mnt(0) != 0) {
        fail("the parent's mount remounted writable once the child has ended");
    }
    if (syscall2(SYS_FCHMOD, 15, 0755) != -EROFS) {
        fail("an fchmod through the ended child's descriptor of its read-only copy is EROFS");
    }
    if (create_in_mnt("/d") != 0) {
        fail("a create through the parent's writable mount");
    }
    syscall1(SYS_CLOSE, 15);
    if (syscall2(SYS_UMOUNT2, (long)mnt, 0) != 0) {
        fail("the parent's mount unmounts once the copy's last descriptor closed");
    }
}

/** The child of `ns_unmount_row`, which says on `out` that it made its file
 * and reads its go from `in`; its exit status, a reason of `ns_whys`. */
static int ns_unmount_child(int out, int in)
{
    char byte = 0;

    if (syscall1(SYS_UNSHARE, CLONE_NEWNS) != 0) {
        return 1;
    }
    if (create_in_mnt("/k") != 0) {
        return 8;
    }
    if (syscall3(SYS_WRITE, out, (long)&byte, 1) != 1 ||
        syscall3(SYS_READ, in, (long)&byte, 1) != 1) {
        return 5;
    }
    return xstat(in_mnt("/k")) == 0 ? 0 : 9;
}

/**
 * The parent unmounts its mount while a child's namespace has a copy of
 * it: the unmount succeeds and the parent's view loses the file the child
 * made, which the child still sees.
 */
static void ns_unmount_row(void)
{
    int c2p[2], p2c[2], pid;
    char byte = 0;

    if (syscall5(SYS_MOUNT, (long)"none", (long)mnt, (long)"tmpfs", 0, 0) != 0 ||
        syscall1(SYS_PIPE, (long)c2p) != 0 || syscall1(SYS_PIPE, (long)p2c) != 0) {
        fail("a tmpfs and the pipes for the unmount row");
    }
    pid = syscall0(SYS_FORK);
    if (pid == 0) {
        sys_exit(ns_unmount_child(c2p[1], p2c[0]));
    }
    fork_ready(pid, c2p, &byte, 1, "the child of the unmount row");
    if (syscall2(SYS_UMOUNT2, (long)mnt, 0) != 0) {
        fail("the parent unmounts a mount a child's namespace has a copy of");
    }
    if (xstat(in_mnt("/k")) != -ENOENT) {
        fail("the parent's view loses the unmounted file system");
    }
    if (syscall3(SYS_WRITE, p2c[1], (long)&byte, 1) != 1) {
        fail("the parent's handshake in the unmount row");
    }
    row_child = 0;
    ns_child_done(pid);
    close_pipes(c2p, p2c);
}

/** The 32-bit word at `p`, little-endian, of a record a call filled. */
static unsigned word_at(const unsigned char *p)
{
    return (unsigned)p[0] | (unsigned)p[1] << 8 | (unsigned)p[2] << 16 | (unsigned)p[3] << 24;
}

/** stat(2) of `path`, its mode into `*mode` and its group into `*gid`: 0,
 * or a negated errno. */
static int mode_gid(const char *path, unsigned *mode, unsigned *gid)
{
    long st[KSTAT_SIZE / sizeof(long) + 1];
    int rc = syscall2(SYS_STAT, (long)path, (long)st);

    *mode = word_at((const unsigned char *)st + KSTAT_MODE);
    *gid = word_at((const unsigned char *)st + KSTAT_GID);
    return rc;
}

/** The f_flags fstatfs gives for the descriptor `fd`, or a negated errno. */
static long fd_statfs_flags(int fd)
{
    long st[KSTATFS_SIZE / sizeof(long) + 1];
    long rc;

#if defined __x86_64__
    rc = syscall2(SYS_FSTATFS, fd, (long)st);
#else
    rc = syscall3(SYS_FSTATFS64, fd, 84, (long)st);
#endif
    return rc != 0 ? rc : (long)word_at((const unsigned char *)st + KSTATFS_FLAGS);
}

/** The f_flags statfs gives for `path`, or a negated errno. */
static long path_statfs_flags(const char *path)
{
    long st[KSTATFS_SIZE / sizeof(long) + 1];
    long rc = probe_statfs(path, st);

    return rc != 0 ? rc : (long)word_at((const unsigned char *)st + KSTATFS_FLAGS);
}

/** The child of `copy_root_row`, in a namespace of its own; its exit
 * status, a reason of `ns_whys`. With `krffs`, the directory the rows make
 * in is on the KRFFS root, which refuses to remove a directory in use. */
static int copy_root_child(int krffs)
{
    unsigned mode = 0, gid = 0;
    int fd;

    if (syscall1(SYS_UNSHARE, CLONE_NEWNS) != 0) {
        return 1;
    }
    if (krffs && syscall1(SYS_RMDIR, (long)at_top("nsheld")) != -EBUSY) {
        return 10;
    }
    if (krffs && (syscall1(SYS_CHDIR, (long)at_top("nscwd")) != 0 ||
                  syscall1(SYS_RMDIR, (long)at_top("nscwd")) != -EBUSY)) {
        return 11;
    }
    syscall1(SYS_UMASK, 0);
    fd = syscall3(SYS_OPEN, (long)at_top("nsmode"), O_WRONLY | O_CREAT | O_TRUNC, 0600);
    if (fd < 0 || syscall1(SYS_CLOSE, fd) != 0 || mode_gid(at_top("nsmode"), &mode, &gid) != 0 ||
        (mode & 07777) != 0600) {
        return 12;
    }
    if (syscall2(SYS_MKDIR, (long)at_top("nssg/d"), 0755) != 0 ||
        mode_gid(at_top("nssg/d"), &mode, &gid) != 0 || gid != 7 || (mode & 07777) != 02755) {
        return 13;
    }
    return 0;
}

/**
 * A child that has made a namespace, and so a copy of the root mount,
 * makes and removes in the root's directories through the copy as through
 * the root: on the KRFFS root an rmdir of a directory a descriptor of the
 * parent's holds, or of the child's working directory, is EBUSY, and both
 * directories are still there for the parent to remove; a file made 0600
 * with no umask is 0600; and a directory made in a setgid directory takes
 * its group, 7, and its setgid bit.
 */
static void copy_root_row(void)
{
    long st[KSTATFS_SIZE / sizeof(long) + 1];
    int pid, held, krffs;

    krffs = probe_statfs(at_top("."), st) == 0 &&
            word_at((const unsigned char *)st) == STATFS_MAGIC_KRFFS;
    if (syscall2(SYS_MKDIR, (long)at_top("nsheld"), 0755) != 0 ||
        syscall2(SYS_MKDIR, (long)at_top("nscwd"), 0755) != 0 ||
        syscall2(SYS_MKDIR, (long)at_top("nssg"), 0775) != 0 ||
        syscall3(SYS_CHOWN, (long)at_top("nssg"), 0, 7) != 0 ||
        syscall2(SYS_CHMOD, (long)at_top("nssg"), 02775) != 0) {
        fail("the directories of the root's copy row");
    }
    held = syscall3(SYS_OPEN, (long)at_top("nsheld"), O_RDONLY, 0);
    if (held < 0) {
        fail("a descriptor of the directory the root's copy row holds");
    }
    pid = syscall0(SYS_FORK);
    if (pid == 0) {
        sys_exit(copy_root_child(krffs));
    }
    if (pid < 0) {
        fail("the child of the root's copy row");
    }
    ns_child_done(pid);
    syscall1(SYS_CLOSE, held);
    if (syscall1(SYS_RMDIR, (long)at_top("nsheld")) != 0 ||
        syscall1(SYS_RMDIR, (long)at_top("nscwd")) != 0 ||
        syscall1(SYS_UNLINK, (long)at_top("nsmode")) != 0 ||
        syscall1(SYS_RMDIR, (long)at_top("nssg/d")) != 0 ||
        syscall1(SYS_RMDIR, (long)at_top("nssg")) != 0) {
        fail("the directories and the file the child made are there to remove");
    }
}

/**
 * A child of its own namespace remounts its copy of /proc read-only, opens
 * /proc on descriptor 14, which it shares with the parent (CLONE_FILES), and
 * exits, ending the namespace. fstatfs through the descriptor says
 * ST_RDONLY, the copy's flags, while statfs of the parent's /proc does not.
 */
static void ns_statfs_row(void)
{
    long flags;
    int pid = syscall5(SYS_CLONE, CLONE_NEWNS | CLONE_FILES | SIGCHLD, 0, 0, 0, 0);

    if (pid == 0) {
        int d;

        if (syscall5(SYS_MOUNT, 0, (long)"/proc", 0, MS_REMOUNT | MS_BIND | MS_RDONLY, 0) != 0) {
            sys_exit(15);
        }
        d = syscall3(SYS_OPEN, (long)"/proc", O_RDONLY, 0);
        sys_exit(d >= 0 && syscall2(SYS_DUP2, d, 14) == 14 ? 0 : 15);
    }
    if (pid < 0) {
        fail("clone for the statfs row");
    }
    ns_child_done(pid);
    flags = fd_statfs_flags(14);
    syscall1(SYS_CLOSE, 14);
    if (flags < 0 || !(flags & ST_RDONLY)) {
        fail("fstatfs through a descriptor of an ended namespace's read-only /proc says ST_RDONLY");
    }
    flags = path_statfs_flags("/proc");
    if (flags < 0 || (flags & ST_RDONLY)) {
        fail("statfs of the parent's /proc does not say ST_RDONLY");
    }
}

/** The child of `ns_openat_row`, which says on `out` that its mount and its
 * descriptor are ready and reads its go from `in`; its exit status, a
 * reason of `ns_whys`. */
static int ns_openat_child(int out, int in)
{
    char byte = 0;
    int d;

    if (syscall5(SYS_MOUNT, (long)"none", (long)in_mnt("/s"), (long)"tmpfs", 0, 0) != 0 ||
        create_in_mnt("/s/f") != 0 || syscall2(SYS_SYMLINK, (long)"t", (long)in_mnt("/s/l")) != 0 ||
        syscall2(SYS_SYMLINK, (long)"s/u", (long)in_mnt("/l")) != 0) {
        return 16;
    }
    d = syscall3(SYS_OPEN, (long)mnt, O_RDONLY, 0);
    if (d < 0 || syscall2(SYS_DUP2, d, 14) != 14) {
        return 16;
    }
    if (syscall3(SYS_WRITE, out, (long)&byte, 1) != 1 ||
        syscall3(SYS_READ, in, (long)&byte, 1) != 1) {
        return 5;
    }
    return 0;
}

static int openat_pipes[4] = {-1, -1, -1, -1}; /* `ns_openat_row`'s c2p, p2c */

/** Undo what `ns_openat_row` made, for `fail`. */
static void openat_undo(void)
{
    int i;

    for (i = 0; i < 4; i++) {
        if (openat_pipes[i] >= 0) {
            syscall1(SYS_CLOSE, openat_pipes[i]);
        }
    }
    syscall1(SYS_CLOSE, 14);
    syscall1(SYS_UNLINK, (long)in_mnt("/s/t"));
    syscall1(SYS_UNLINK, (long)in_mnt("/s/u"));
    syscall1(SYS_UNLINK, (long)in_mnt("/l"));
    syscall1(SYS_RMDIR, (long)in_mnt("/s"));
    syscall2(SYS_UMOUNT2, (long)mnt, MNT_DETACH);
}

/** Make the parent's file `name` in the mount point hold one byte, for
 * `ns_openat_row`. */
static void openat_parent_file(const char *name)
{
    int fd = syscall3(SYS_OPEN, (long)in_mnt(name), O_WRONLY | O_CREAT | O_TRUNC, 0644);

    if (fd < 0 || syscall3(SYS_WRITE, fd, (long)"x", 1) != 1) {
        fail("the parent's files s/t and s/u for the openat row");
    }
    syscall1(SYS_CLOSE, fd);
}

/** Whether the parent's file `name` in the mount point still holds its
 * byte. */
static int openat_parent_whole(const char *name)
{
    char byte;
    int fd = syscall3(SYS_OPEN, (long)in_mnt(name), O_RDONLY, 0), n;

    if (fd < 0) {
        return 0;
    }
    n = syscall3(SYS_READ, fd, (long)&byte, 1);
    syscall1(SYS_CLOSE, fd);
    return n == 1;
}

/**
 * The open of the link l right below descriptor 14, the root of another
 * namespace's copy, with O_CREAT and O_TRUNC, by a process chrooted to the
 * mount point, so that the descriptor's path is its root and the link's
 * directory has a path of no components: 0 when the open succeeds.
 */
static int openat_root_link(void)
{
    int pid = syscall0(SYS_FORK), fd;

    if (pid == 0) {
        if (syscall1(SYS_CHROOT, (long)mnt) != 0 || syscall1(SYS_CHDIR, (long)"/") != 0) {
            sys_exit(1);
        }
        fd = syscall4(SYS_OPENAT, 14, (long)"l", O_WRONLY | O_CREAT | O_TRUNC, 0644);
        sys_exit(fd >= 0 ? 0 : 1);
    }
    return pid < 0 ? -1 : (wait_status(pid) & 0xffff);
}

/**
 * A child of its own namespace mounts a tmpfs over the directory s of its
 * copy of the mount point, makes s/f there, a symbolic link s/l to the
 * missing t, and a link l to s/u at the copy's root, which the parent's
 * mount shares, and puts a descriptor of its copy on descriptor 14, which
 * it shares with the parent. An openat of s/f through the descriptor
 * reaches the child's file, as Linux walks from the descriptor's own mount,
 * though the parent's s has no f; and an openat of s/l with O_CREAT and
 * O_TRUNC makes t in the child's tmpfs, the link's directory, and leaves
 * alone the parent's s/t, which the link's path names in the parent's tree.
 * So does an openat of l from a process chrooted to the mount point, whose
 * link's directory is the descriptor's own, with u. A failure kills and
 * reaps the child and removes what the row made, the tmpfs at the mount
 * point with it; a child that ends before its handshake fails the row
 * (`child_ready`).
 */
static void ns_openat_row(void)
{
    int *c2p = openat_pipes, *p2c = openat_pipes + 2, pid, fd, st;
    char byte = 0;

    row_undo = openat_undo;
    if (syscall2(SYS_MKDIR, (long)in_mnt("/s"), 0755) != 0 ||
        syscall2(SYS_PIPE2, (long)c2p, O_NONBLOCK) != 0 || syscall1(SYS_PIPE, (long)p2c) != 0) {
        fail("a directory and the pipes for the openat row");
    }
    openat_parent_file("/s/t");
    openat_parent_file("/s/u");
    pid = syscall5(SYS_CLONE, CLONE_NEWNS | CLONE_FILES | SIGCHLD, 0, 0, 0, 0);
    if (pid == 0) {
        sys_exit(ns_openat_child(c2p[1], p2c[0]));
    }
    if (pid < 0) {
        fail("the child of the openat row");
    }
    row_child = pid;
    if (!child_ready(pid, c2p[0], &st)) {
        row_child = 0;
        ns_status_check(st);
        fail("the child of the openat row ended before its handshake");
    }
    fd = syscall4(SYS_OPENAT, 14, (long)"s/f", O_RDONLY, 0);
    if (fd < 0) {
        fail("an openat through a descriptor of another namespace's copy reaches that copy's "
             "mounts");
    }
    syscall1(SYS_CLOSE, fd);
    if (xstat(in_mnt("/s/f")) != -ENOENT) {
        fail("the parent's own directory has no file the child made");
    }
    fd = syscall4(SYS_OPENAT, 14, (long)"s/l", O_WRONLY | O_CREAT | O_TRUNC, 0644);
    if (fd < 0) {
        fail("an openat with O_CREAT through another namespace's link to a missing file");
    }
    syscall1(SYS_CLOSE, fd);
    fd = syscall4(SYS_OPENAT, 14, (long)"s/t", O_RDONLY, 0);
    if (fd < 0) {
        fail("O_CREAT through another namespace's relative link makes its target in the link's "
             "directory");
    }
    syscall1(SYS_CLOSE, fd);
    if (!openat_parent_whole("/s/t")) {
        fail("O_CREAT|O_TRUNC through another namespace's link leaves the parent's file of the "
             "link's path whole");
    }
    if (openat_root_link() != 0) {
        fail("an openat with O_CREAT through a link right below another namespace's root");
    }
    fd = syscall4(SYS_OPENAT, 14, (long)"s/u", O_RDONLY, 0);
    if (fd < 0) {
        fail("O_CREAT through a link right below another namespace's root makes its target in "
             "that namespace's tree");
    }
    syscall1(SYS_CLOSE, fd);
    if (!openat_parent_whole("/s/u")) {
        fail("O_CREAT|O_TRUNC through a link right below another namespace's root leaves the "
             "caller's file of the link's path whole");
    }
    if (syscall1(SYS_UNLINK, (long)in_mnt("/s/t")) != 0 ||
        syscall1(SYS_UNLINK, (long)in_mnt("/s/u")) != 0 ||
        syscall1(SYS_UNLINK, (long)in_mnt("/l")) != 0) {
        fail("the parent's files of the openat row are there to remove");
    }
    syscall1(SYS_CLOSE, 14);
    if (syscall3(SYS_WRITE, p2c[1], (long)&byte, 1) != 1) {
        fail("the parent's handshake in the openat row");
    }
    row_child = 0;
    ns_child_done(pid);
    close_pipes(c2p, p2c);
    openat_pipes[0] = openat_pipes[1] = openat_pipes[2] = openat_pipes[3] = -1;
    row_undo = 0;
    if (syscall1(SYS_RMDIR, (long)in_mnt("/s")) != 0) {
        fail("the parent's directory s, bare once the child's namespace ended");
    }
}

/** The text of `n` into `out`, which has room for it. */
static char *decimal(char *out, int n)
{
    char digits[12];
    int i = 0, k = 0;

    do {
        digits[i++] = (char)('0' + n % 10);
        n /= 10;
    } while (n > 0);
    while (i > 0) {
        out[k++] = digits[--i];
    }
    out[k] = '\0';
    return out;
}

/** The child of `ns_setns_row` that makes a namespace and chroots to its
 * copy of the mount point, says so on `out` and reads its go from `in`;
 * its exit status, a reason of `ns_whys`. */
static int ns_chroot_child(int out, int in)
{
    char byte = 0;

    if (syscall1(SYS_UNSHARE, CLONE_NEWNS) != 0) {
        return 1;
    }
    if (syscall1(SYS_CHROOT, (long)mnt) != 0 || syscall1(SYS_CHDIR, (long)"/") != 0) {
        return 14;
    }
    if (syscall3(SYS_WRITE, out, (long)&byte, 1) != 1 ||
        syscall3(SYS_READ, in, (long)&byte, 1) != 1) {
        return 5;
    }
    return 0;
}

/** The child of `ns_setns_row` that joins the namespace of `pid` by setns;
 * its exit status, a reason of `ns_whys`. */
static int ns_join_child(int pid)
{
    char path[48], digits[12];
    int fd;

    join3(path, (int)sizeof(path), "/proc/", decimal(digits, pid), "/ns/mnt");
    fd = syscall3(SYS_OPEN, (long)path, O_RDONLY, 0);
    if (fd < 0 || syscall2(SYS_SETNS, fd, CLONE_NEWNS) != 0) {
        return 17;
    }
    return xstat(mnt_abs) == 0 ? 0 : 17;
}

/**
 * The one other process of a namespace has chrooted to the namespace's
 * copy of the mount point; a process that joins the namespace by setns is
 * given the namespace's own root, from which the mount point's absolute
 * path names a directory, as Linux gives `mnt_ns->root`.
 */
static void ns_setns_row(void)
{
    int c2p[2], p2c[2], pid, joiner;
    char byte = 0;

    if (syscall1(SYS_PIPE, (long)c2p) != 0 || syscall1(SYS_PIPE, (long)p2c) != 0) {
        fail("the pipes for the setns row");
    }
    pid = syscall0(SYS_FORK);
    if (pid == 0) {
        sys_exit(ns_chroot_child(c2p[1], p2c[0]));
    }
    fork_ready(pid, c2p, &byte, 1, "the chrooted child of the setns row");
    joiner = syscall0(SYS_FORK);
    if (joiner == 0) {
        sys_exit(ns_join_child(pid));
    }
    if (joiner < 0) {
        fail("the joining child of the setns row");
    }
    ns_child_done(joiner);
    if (syscall3(SYS_WRITE, p2c[1], (long)&byte, 1) != 1) {
        fail("the parent's handshake in the setns row");
    }
    row_child = 0;
    ns_child_done(pid);
    close_pipes(c2p, p2c);
}

/** The walks of `ns_dev_row` from descriptor 14, an ended namespace's
 * /dev/pts, its `..` first; 0, or a reason of `ns_whys`. */
static int ns_dev_walks(void)
{
    long st[KSTAT_SIZE / sizeof(long) + 1];
    long flags;
    int d, e, rc = 0;

    d = syscall4(SYS_OPENAT, 14, (long)"..", O_RDONLY | O_DIRECTORY, 0);
    flags = d < 0 ? d : fd_statfs_flags(d);
    if (flags < 0 || !(flags & ST_RDONLY) ||
        syscall4(SYS_FSTATAT, d, (long)"pts", (long)st, 0) != 0) {
        rc = 20;
    }
    syscall1(SYS_CLOSE, d);
    e = rc ? -1 : syscall4(SYS_OPENAT, 14, (long)"../null", O_RDONLY, 0);
    flags = e < 0 ? e : fd_statfs_flags(e);
    if (rc == 0 && (flags < 0 || !(flags & ST_RDONLY))) {
        rc = 20;
    }
    syscall1(SYS_CLOSE, e);
    e = rc ? -1 : syscall4(SYS_OPENAT, 14, (long)".", O_RDONLY | O_DIRECTORY, 0);
    if (rc == 0 && (syscall4(SYS_FSTATAT, 14, (long)"pts", (long)st, 0) != -ENOENT || e < 0 ||
                    syscall4(SYS_FSTATAT, e, (long)"pts", (long)st, 0) != -ENOENT)) {
        rc = 19;
    }
    syscall1(SYS_CLOSE, e);
    return rc;
}

/**
 * A child of its own namespace remounts its copy of /dev read-only, opens
 * /dev/pts on descriptor 14 and /dev/null on 13, which it shares with the
 * parent, and exits, ending the namespace. fstatfs through the /dev/null
 * descriptor says ST_RDONLY, the copy's flags. From the /dev/pts
 * descriptor, `..` and ../null reach the copy's read-only /dev, which has
 * pts, and `.` and a name are looked up in that directory, which has none.
 * A `..` that the type handed back to the walk where it took it again went
 * round for ever, where no signal reaches the call; the walk ends a
 * hand-back that uses up nothing with EIO (`walk_rest_back`), so the `..`
 * runs first, and fails the row instead of hanging it.
 */
static void ns_dev_row(void)
{
    long flags;
    int code, pid = syscall5(SYS_CLONE, CLONE_NEWNS | CLONE_FILES | SIGCHLD, 0, 0, 0, 0);

    if (pid == 0) {
        int pts, null;

        if (syscall5(SYS_MOUNT, 0, (long)"/dev", 0, MS_REMOUNT | MS_BIND | MS_RDONLY, 0) != 0) {
            sys_exit(18);
        }
        pts = syscall3(SYS_OPEN, (long)"/dev/pts", O_RDONLY | O_DIRECTORY, 0);
        null = syscall3(SYS_OPEN, (long)"/dev/null", O_RDONLY, 0);
        sys_exit(pts >= 0 && null >= 0 && syscall2(SYS_DUP2, pts, 14) == 14 &&
                         syscall2(SYS_DUP2, null, 13) == 13 && syscall1(SYS_CLOSE, pts) == 0 &&
                         syscall1(SYS_CLOSE, null) == 0
                     ? 0
                     : 18);
    }
    if (pid < 0) {
        fail("clone for the /dev row");
    }
    ns_child_done(pid);
    flags = fd_statfs_flags(13);
    syscall1(SYS_CLOSE, 13);
    if (flags < 0 || !(flags & ST_RDONLY)) {
        fail("fstatfs through a descriptor of /dev/null from an ended namespace's read-only /dev "
             "says ST_RDONLY");
    }
    code = ns_dev_walks();
    syscall1(SYS_CLOSE, 14);
    if (code != 0) {
        fail(ns_whys[code]);
    }
}

/** Whether /proc/<pid>/wchan names `want`, the function `pid` sleeps in. */
static int sleeps_in(int pid, const char *want)
{
    char path[48], digits[12], name[64];
    int fd, n;

    join3(path, (int)sizeof(path), "/proc/", decimal(digits, pid), "/wchan");
    fd = syscall3(SYS_OPEN, (long)path, O_RDONLY, 0);
    if (fd < 0) {
        return 0;
    }
    n = syscall3(SYS_READ, fd, (long)name, (int)sizeof(name) - 1);
    syscall1(SYS_CLOSE, fd);
    if (n <= 0) {
        return 0;
    }
    name[n] = '\0';
    if (name[n - 1] == '\n') {
        name[n - 1] = '\0';
    }
    return same(name, want);
}

/** The helper of `held_start_row`, whose working directory is the ended
 * namespace's nsfifo; its exit status, a reason of `ns_whys`. */
static int held_start_helper(void)
{
    int walker, tries, w, rc;

    walker = syscall5(SYS_CLONE, CLONE_FS | SIGCHLD, 0, 0, 0, 0);
    if (walker == 0) {
        int f = syscall3(SYS_OPEN, (long)"p", O_RDONLY, 0);

        sys_exit(f >= 0 ? 0 : 21);
    }
    if (walker < 0) {
        return 21;
    }
    for (tries = 0; tries < 100000 && !sleeps_in(walker, "wait_for_partner"); tries++) {
        syscall0(SYS_SCHED_YIELD);
    }
    if (tries == 100000 || syscall1(SYS_CHDIR, (long)"/") != 0) {
        syscall2(SYS_KILL, walker, 9);
        wait_status(walker);
        return 21;
    }
    rc = syscall1(SYS_RMDIR, (long)at_top("nsfifo")) == -EBUSY ? 0 : 22;
    w = syscall3(SYS_OPEN, (long)at_top("nsfifo/p"), O_WRONLY | O_NONBLOCK, 0);
    if (w < 0) {
        syscall2(SYS_KILL, walker, 9);
        wait_status(walker);
        return 21;
    }
    syscall1(SYS_CLOSE, w);
    return (wait_status(walker) & 0xffff) != 0 ? 21 : rc;
}

/** Undo what `held_start_row` made, for `fail`. */
static void held_start_undo(void)
{
    syscall1(SYS_CLOSE, 14);
    syscall1(SYS_UNLINK, (long)at_top("nsfifo/p"));
    syscall1(SYS_RMDIR, (long)at_top("nsfifo"));
}

/**
 * On the KRFFS root, which counts no holds on its directories, a directory
 * a walk starts from stays while the walk goes on: a child of its own
 * namespace puts a descriptor of its copy's nsfifo on descriptor 14, which
 * it shares with the parent, and exits. A helper changes its working
 * directory to it, and both close 14; a walker that shares the helper's
 * working directory (CLONE_FS) opens the FIFO p by its relative name and
 * sleeps for a writer; the helper changes directory to /, so only the
 * walk's path page keeps nsfifo, and an rmdir of nsfifo is EBUSY, before
 * the ENOTEMPTY its FIFO would give. A failure removes the FIFO and the
 * directory, and the helper kills and reaps a walker it cannot wake.
 */
static void held_start_row(void)
{
    int pid;

    row_undo = held_start_undo;
    if (syscall2(SYS_MKDIR, (long)at_top("nsfifo"), 0755) != 0 ||
        syscall3(SYS_MKNOD, (long)at_top("nsfifo/p"), S_IFIFO | 0644, 0) != 0) {
        fail("the directory and the FIFO of the held start row");
    }
    pid = syscall5(SYS_CLONE, CLONE_NEWNS | CLONE_FILES | SIGCHLD, 0, 0, 0, 0);
    if (pid == 0) {
        int d = syscall3(SYS_OPEN, (long)at_top("nsfifo"), O_RDONLY | O_DIRECTORY, 0);

        sys_exit(d >= 0 && syscall2(SYS_DUP2, d, 14) == 14 && syscall1(SYS_CLOSE, d) == 0 ? 0 : 21);
    }
    if (pid < 0) {
        fail("clone for the held start row");
    }
    ns_child_done(pid);
    pid = syscall0(SYS_FORK);
    if (pid == 0) {
        if (syscall1(SYS_FCHDIR, 14) != 0 || syscall1(SYS_CLOSE, 14) != 0) {
            sys_exit(21);
        }
        sys_exit(held_start_helper());
    }
    syscall1(SYS_CLOSE, 14);
    if (pid < 0) {
        fail("the helper of the held start row");
    }
    ns_child_done(pid);
    row_undo = 0;
    if (syscall1(SYS_UNLINK, (long)at_top("nsfifo/p")) != 0 ||
        syscall1(SYS_RMDIR, (long)at_top("nsfifo")) != 0) {
        fail("the FIFO and the directory of the held start row are there to remove");
    }
}

/**
 * The rows of a namespace's copies as the root, /proc and a descriptor of
 * another namespace see them (`copy_root_row`, `held_start_row` on the
 * KRFFS root, `ns_statfs_row`, `ns_dev_row`, `ns_openat_row`), and of setns
 * into a namespace whose member chrooted (`ns_setns_row`), the last two on
 * a tmpfs at the mount point.
 */
static void rows_copies(void)
{
    long st[KSTATFS_SIZE / sizeof(long) + 1];

    copy_root_row();
    if (probe_statfs(at_top("."), st) == 0 &&
        word_at((const unsigned char *)st) == STATFS_MAGIC_KRFFS) {
        held_start_row();
    }
    ns_statfs_row();
    ns_dev_row();
    if (syscall5(SYS_MOUNT, (long)"none", (long)mnt, (long)"tmpfs", 0, 0) != 0) {
        fail("a tmpfs for the openat and setns rows");
    }
    ns_openat_row();
    row_undo = mnt_detach_undo;
    ns_setns_row();
    if (syscall2(SYS_UMOUNT2, (long)mnt, 0) != 0) {
        fail("the tmpfs of the openat and setns rows unmounts");
    }
    row_undo = 0;
}

/** The mount point's absolute path followed by `rest`, in the next of four
 * buffers, for a row that leaves the working directory, from which a
 * native run's mount point is named. */
static const char *abs_mnt(const char *rest)
{
    static char paths[4][96];
    static int next;
    char *p = paths[next];

    next = (next + 1) % 4;
    return join3(p, (int)sizeof(paths[0]), mnt_abs, rest, "");
}

/** Make the file `path` holding the one byte `byte`, or fail `what`. */
static void put_byte(const char *path, char byte, const char *what)
{
    int fd = syscall3(SYS_OPEN, (long)path, O_WRONLY | O_CREAT | O_TRUNC, 0644);

    if (fd < 0 || syscall3(SYS_WRITE, fd, (long)&byte, 1) != 1) {
        fail(what);
    }
    syscall1(SYS_CLOSE, fd);
}

/** Fail `what` unless the file `name`, relative to the directory `at` or to
 * the working directory for AT_FDCWD, holds the byte `want` first. */
static void expect_byte(int at, const char *name, char want, const char *what)
{
    int fd = syscall4(SYS_OPENAT, at, (long)name, O_RDONLY, 0);
    char got = 0;

    if (fd < 0 || syscall3(SYS_READ, fd, (long)&got, 1) != 1 || got != want) {
        fail(what);
    }
    syscall1(SYS_CLOSE, fd);
}

/* The failures of `covered_row`, by its start and the cover's place. */
static const char *const covered_whys[] = {
    "a name relative to a descriptor of a directory a mount covers since is that directory's",
    "a name relative to a descriptor of a directory below a mount made since is that "
    "directory's",
    "a name relative to a working directory a mount covers since is that directory's",
    "a name relative to a working directory below a mount made since is that directory's",
};

static int covered_fd = -1;      /* `covered_row`'s descriptor of a/b, or -1 */
static int covered_home = -1;    /* its descriptor of the way back, or -1 */
static const char *covered_over; /* the cover's place while mounted, or 0 */

/** Undo what `covered_row` and `rows_covered` made, for `fail`: the cover,
 * the working directory, the descriptors, the files and the tmpfs. */
static void covered_undo(void)
{
    if (covered_over != 0) {
        syscall2(SYS_UMOUNT2, (long)abs_mnt(covered_over), MNT_DETACH);
    }
    if (covered_home >= 0) {
        syscall1(SYS_FCHDIR, covered_home);
        syscall1(SYS_CLOSE, covered_home);
    }
    if (covered_fd >= 0) {
        syscall1(SYS_CLOSE, covered_fd);
    }
    syscall1(SYS_UNLINK, (long)abs_mnt("/a/b/f"));
    syscall1(SYS_RMDIR, (long)abs_mnt("/a/b"));
    syscall1(SYS_RMDIR, (long)abs_mnt("/a"));
    syscall2(SYS_UMOUNT2, (long)abs_mnt(""), MNT_DETACH);
    covered_over = 0;
    covered_home = covered_fd = -1;
}

/**
 * A name relative to a directory that a mount made since covers, or a
 * directory above it: a/b on the tmpfs at the mount point holds f with "x",
 * and a descriptor of a/b is opened, or, with `cwd`, the working directory
 * is moved there. A tmpfs is then mounted over a/b, or over a when `above`
 * is set, with an a/b/f of its own that holds "y". The name f relative to
 * the descriptor or the working directory reads "x", as Linux walks from the
 * descriptor's or the working directory's own mount and directory
 * (fs/namei.c `path_init` in 6.12), while the path of a/b/f reads the
 * cover's "y". The row names the mount point by its absolute path, since it
 * leaves the working directory a native run names it from, and then comes
 * back by a descriptor of it. A failure undoes it all (`covered_undo`).
 */
static void covered_row(int cwd, int above)
{
    const char *why = covered_whys[cwd * 2 + above];
    const char *over = above ? "/a" : "/a/b";
    int at;

    if (syscall2(SYS_MKDIR, (long)abs_mnt("/a"), 0755) != 0 ||
        syscall2(SYS_MKDIR, (long)abs_mnt("/a/b"), 0755) != 0) {
        fail("the directories of the covered row");
    }
    put_byte(abs_mnt("/a/b/f"), 'x', "the file of the covered row");
    if (cwd) {
        covered_home = syscall3(SYS_OPEN, (long)".", O_RDONLY | O_DIRECTORY, 0);
        if (covered_home < 0 || syscall1(SYS_CHDIR, (long)abs_mnt("/a/b")) != 0) {
            fail("the working directory of the covered row");
        }
        at = AT_FDCWD;
    } else {
        covered_fd = syscall3(SYS_OPEN, (long)abs_mnt("/a/b"), O_RDONLY | O_DIRECTORY, 0);
        if (covered_fd < 0) {
            fail("the descriptor of the covered row");
        }
        at = covered_fd;
    }
    if (syscall5(SYS_MOUNT, (long)"none", (long)abs_mnt(over), (long)"tmpfs", 0, 0) != 0) {
        fail("the cover of the covered row");
    }
    covered_over = over;
    if (above && syscall2(SYS_MKDIR, (long)abs_mnt("/a/b"), 0755) != 0) {
        fail("the cover's directory of the covered row");
    }
    put_byte(abs_mnt("/a/b/f"), 'y', "the cover's file of the covered row");
    expect_byte(at, "f", 'x', why);
    expect_byte(AT_FDCWD, abs_mnt("/a/b/f"), 'y', "the path through a cover reaches the cover");
    if (syscall2(SYS_UMOUNT2, (long)abs_mnt(over), 0) != 0) {
        fail("the cover of the covered row unmounts");
    }
    covered_over = 0;
    if (cwd) {
        if (syscall1(SYS_FCHDIR, covered_home) != 0) {
            fail("the way back from the working directory of the covered row");
        }
        syscall1(SYS_CLOSE, covered_home);
        covered_home = -1;
    }
    if (covered_fd >= 0) {
        syscall1(SYS_CLOSE, covered_fd);
        covered_fd = -1;
    }
    if (syscall1(SYS_UNLINK, (long)abs_mnt("/a/b/f")) != 0 ||
        syscall1(SYS_RMDIR, (long)abs_mnt("/a/b")) != 0 ||
        syscall1(SYS_RMDIR, (long)abs_mnt("/a")) != 0) {
        fail("what the covered row made is there to remove");
    }
}

/**
 * The rows of a name relative to a directory that a mount made since covers,
 * over the directory or over one above it, from a descriptor and from the
 * working directory (`covered_row`), on a tmpfs at the mount point, which
 * a failure detaches with the rest (`covered_undo`).
 */
static void rows_covered(void)
{
    int cwd, above;

    if (syscall5(SYS_MOUNT, (long)"none", (long)mnt, (long)"tmpfs", 0, 0) != 0) {
        fail("a tmpfs for the covered rows");
    }
    row_undo = covered_undo;
    for (cwd = 0; cwd < 2; cwd++) {
        for (above = 0; above < 2; above++) {
            covered_row(cwd, above);
        }
    }
    if (syscall2(SYS_UMOUNT2, (long)mnt, 0) != 0) {
        fail("the tmpfs of the covered rows unmounts");
    }
    row_undo = 0;
}

/**
 * The rows of mount namespaces, on a tmpfs at the mount point, each with a
 * child that makes a namespace and so a copy of every mount, as Linux's
 * `copy_mnt_ns` makes one: a remount of each mount on its own with its own
 * writers (`ns_flags_row`), a descriptor that keeps an ended namespace's
 * read-only copy (`ns_desc_row`), and an unmount of a mount another
 * namespace has a copy of (`ns_unmount_row`). A failure detaches the
 * row's tmpfs.
 */
static void rows_namespaces(void)
{
    if (syscall5(SYS_MOUNT, (long)"none", (long)mnt, (long)"tmpfs", 0, 0) != 0) {
        fail("a tmpfs for the namespace rows");
    }
    row_undo = mnt_detach_undo;
    ns_flags_row();
    ns_desc_row();
    ns_unmount_row();
    row_undo = 0;
}

/* The walk gate of debugfs (debugfs.5), which stops a chosen thread at a
 * walk event until a write lets it go. */
#define WALK_GATE "/sys/kernel/debug/vfs/walk_gate"

/** Write `text` to the walk gate, or fail `what`. */
static void gate_write(const char *text, const char *what)
{
    int fd = syscall3(SYS_OPEN, (long)WALK_GATE, O_WRONLY, 0);
    int n = (int)strlen(text);

    if (fd < 0 || syscall3(SYS_WRITE, fd, (long)text, n) != n) {
        syscall1(SYS_CLOSE, fd);
        fail(what);
    }
    syscall1(SYS_CLOSE, fd);
}

/** The walk gate's line into `line`, of `cap` bytes, or fail. */
static void gate_read(char *line, int cap)
{
    int fd = syscall3(SYS_OPEN, (long)WALK_GATE, O_RDONLY, 0);
    int n = fd < 0 ? fd : syscall3(SYS_READ, fd, (long)line, cap - 1);

    syscall1(SYS_CLOSE, fd);
    if (n <= 0) {
        fail("the walk gate reads");
    }
    line[n] = '\0';
}

/** Arm the walk gate to stop the thread `pid` at `event`, from zero. */
static void gate_arm(int pid, const char *event)
{
    char text[48], head[24], digits[12];

    join3(head, (int)sizeof(head), "arm ", decimal(digits, pid), " ");
    gate_write(join3(text, (int)sizeof(text), head, event, ""), "the walk gate is armed");
}

/**
 * Wait until the child `pid` sleeps at the walk gate, by the gate's own
 * `waiting 1`; fail `why` when the child ends first, which it is reaped
 * for.
 */
static void gate_wait(int pid, const char *why)
{
    char line[160];
    int st;

    for (;;) {
        gate_read(line, (int)sizeof(line));
        if (contains(line, " waiting 1 ")) {
            return;
        }
        if (syscall4(SYS_WAIT4, pid, (long)&st, WNOHANG, 0) == pid) {
            row_child = 0;
            fail(why);
        }
        syscall0(SYS_SCHED_YIELD);
    }
}

static int walks_home = -1;     /* the way back from a walk row's working directory, or -1 */
static int walks_fd = -1;       /* a walk row's descriptor, or -1 */
static int walks_pipe[2];       /* the gated row's go to its child */
static const char *walks_cover; /* a cover's place while mounted, or 0 */
static char walks_over[96];     /* the gated row's cover's place */
static int walks_links;         /* the links `links_row` made */

/** Remove the files and links the walk rows make in the tmpfs at the mount
 * point. */
static void walks_unlink(void)
{
    char path[112], digits[12];
    int i;

    for (i = 1; i <= walks_links; i++) {
        syscall1(SYS_UNLINK,
                 (long)join3(path, (int)sizeof(path), abs_mnt("/k"), decimal(digits, i), ""));
    }
    walks_links = 0;
    syscall1(SYS_UNLINK, (long)abs_mnt("/m40"));
    syscall1(SYS_UNLINK, (long)abs_mnt("/m41"));
    syscall1(SYS_UNLINK, (long)abs_mnt("/f"));
}

/** Undo what the walk rows made, for `fail`: the gate, a cover, the working
 * directory, the descriptors, the pipe, the files and the tmpfs. */
static void walks_undo(void)
{
    int fd = syscall3(SYS_OPEN, (long)WALK_GATE, O_WRONLY, 0);

    if (fd >= 0) {
        syscall3(SYS_WRITE, fd, (long)"off", 3);
        syscall1(SYS_CLOSE, fd);
    }
    if (walks_cover != 0) {
        syscall2(SYS_UMOUNT2, (long)walks_cover, MNT_DETACH);
    }
    walks_unlink();
    if (walks_home >= 0) {
        syscall1(SYS_FCHDIR, walks_home);
        syscall1(SYS_CLOSE, walks_home);
    }
    syscall1(SYS_CLOSE, walks_fd);
    syscall1(SYS_CLOSE, walks_pipe[0]);
    syscall1(SYS_CLOSE, walks_pipe[1]);
    syscall1(SYS_UNLINK, (long)abs_mnt("/marker"));
    syscall1(SYS_UNLINK, (long)abs_mnt("/a/b/f"));
    syscall1(SYS_RMDIR, (long)abs_mnt("/a/b"));
    syscall1(SYS_RMDIR, (long)abs_mnt("/a"));
    syscall2(SYS_UMOUNT2, (long)abs_mnt(""), MNT_DETACH);
    walks_cover = 0;
    walks_home = walks_fd = walks_pipe[0] = walks_pipe[1] = -1;
}

/** Move the working directory to `path`, keeping the way back, or fail
 * `what`. */
static void walks_enter(const char *path, const char *what)
{
    walks_home = syscall3(SYS_OPEN, (long)".", O_RDONLY | O_DIRECTORY, 0);
    if (walks_home < 0 || syscall1(SYS_CHDIR, (long)path) != 0) {
        fail(what);
    }
}

/** Come back from where `walks_enter` went, or fail. */
static void walks_leave(void)
{
    if (syscall1(SYS_FCHDIR, walks_home) != 0) {
        fail("the way back from a walk row's working directory");
    }
    syscall1(SYS_CLOSE, walks_home);
    walks_home = -1;
}

/**
 * A name relative to /dev/pts once a bind covers it: the working directory
 * is /dev/pts, and a descriptor is open on it, when the tmpfs at the mount
 * point, which holds `marker`, is bound over /dev/pts. The name `marker`
 * relative to either is looked up in the devpts directory, which has none,
 * as Linux walks from the held directory (fs/namei.c `path_init` in 6.12),
 * while /dev/pts/marker reaches the bind's.
 */
static void synth_start_row(void)
{
    char st[KSTAT_SIZE];

    put_byte(abs_mnt("/marker"), 'm', "the marker of the /dev/pts row");
    walks_fd = syscall3(SYS_OPEN, (long)"/dev/pts", O_RDONLY | O_DIRECTORY, 0);
    if (walks_fd < 0) {
        fail("a descriptor of /dev/pts");
    }
    walks_enter("/dev/pts", "the working directory /dev/pts");
    if (syscall5(SYS_MOUNT, (long)abs_mnt(""), (long)"/dev/pts", 0, MS_BIND, 0) != 0) {
        fail("a bind over /dev/pts");
    }
    walks_cover = "/dev/pts";
    if (xstat("marker") != -ENOENT) {
        fail("a name relative to a working directory /dev/pts that a bind covers since is "
             "devpts's");
    }
    if (syscall4(SYS_FSTATAT, walks_fd, (long)"marker", (long)st, 0) != -ENOENT) {
        fail("a name relative to a descriptor of /dev/pts that a bind covers since is devpts's");
    }
    if (xstat("/dev/pts/marker") != 0) {
        fail("the path /dev/pts/marker reaches the bind over /dev/pts");
    }
    if (syscall2(SYS_UMOUNT2, (long)"/dev/pts", 0) != 0) {
        fail("the bind over /dev/pts unmounts");
    }
    walks_cover = 0;
    walks_leave();
    syscall1(SYS_CLOSE, walks_fd);
    walks_fd = -1;
    if (syscall1(SYS_UNLINK, (long)abs_mnt("/marker")) != 0) {
        fail("the marker of the /dev/pts row is there to remove");
    }
}

/** The child of `gated_walk_row`: on its go from `in`, make or truncate f
 * in the working directory and write "c" in it; 0, or 1 to 3 for the step
 * that failed. */
static int gated_walk_child(int in)
{
    char byte;
    int fd;

    if (syscall3(SYS_READ, in, (long)&byte, 1) != 1) {
        return 1;
    }
    fd = syscall3(SYS_OPEN, (long)"f", O_WRONLY | O_CREAT | O_TRUNC, 0644);
    if (fd < 0) {
        return 2;
    }
    return syscall3(SYS_WRITE, fd, (long)"c", 1) == 1 ? 0 : 3;
}

/**
 * An open with O_CREAT|O_TRUNC that a mount covers the start of while its
 * walk is under way: a/b on the tmpfs at the mount point is the working
 * directory of a child, which opens f there. The walk gate stops the
 * child's walk under the big lock where it has taken its start, and the
 * parent, from the gate's own count, mounts a tmpfs over a/b, or over a when
 * `above` is set, with an a/b/f holding "y", and lets the walk go on. The
 * child makes its f in the directory it started from, and the cover's f
 * still holds "y" once the child is done, as Linux's walk goes on from the
 * directory it holds.
 */
static void gated_walk_row(int above)
{
    const char *over =
        join3(walks_over, (int)sizeof(walks_over), abs_mnt(above ? "/a" : "/a/b"), "", "");
    char byte = 0;
    int pid;

    if (syscall2(SYS_MKDIR, (long)abs_mnt("/a"), 0755) != 0 ||
        syscall2(SYS_MKDIR, (long)abs_mnt("/a/b"), 0755) != 0 ||
        syscall1(SYS_PIPE, (long)walks_pipe) != 0) {
        fail("the directories and the pipe of the gated walk row");
    }
    walks_enter(abs_mnt("/a/b"), "the working directory of the gated walk row");
    pid = syscall5(SYS_CLONE, SIGCHLD, 0, 0, 0, 0);
    if (pid == 0) {
        syscall1(SYS_CLOSE, walks_pipe[1]);
        sys_exit(gated_walk_child(walks_pipe[0]));
    }
    if (pid < 0) {
        fail("the child of the gated walk row");
    }
    row_child = pid;
    gate_arm(pid, "full");
    if (syscall3(SYS_WRITE, walks_pipe[1], (long)&byte, 1) != 1) {
        fail("the go of the gated walk row");
    }
    gate_wait(pid, "the child of the gated walk row ended before its walk stopped at the gate");
    if (syscall5(SYS_MOUNT, (long)"none", (long)over, (long)"tmpfs", 0, 0) != 0) {
        fail("the cover of the gated walk row");
    }
    walks_cover = over;
    if (above && syscall2(SYS_MKDIR, (long)abs_mnt("/a/b"), 0755) != 0) {
        fail("the cover's directory of the gated walk row");
    }
    put_byte(abs_mnt("/a/b/f"), 'y', "the cover's file of the gated walk row");
    gate_write("release", "the walk gate lets the child go");
    row_child = 0;
    if (wait_status(pid) != 0) {
        fail("the child of the gated walk row makes its file");
    }
    gate_write("off", "the walk gate stops counting");
    expect_byte(AT_FDCWD, abs_mnt("/a/b/f"), 'y',
                "an O_CREAT|O_TRUNC walk that a cover came over leaves the cover's file whole");
    expect_byte(AT_FDCWD, "f", 'c',
                "an O_CREAT|O_TRUNC walk that a cover came over makes its file where it started");
    if (syscall2(SYS_UMOUNT2, (long)over, 0) != 0) {
        fail("the cover of the gated walk row unmounts");
    }
    walks_cover = 0;
    walks_leave();
    syscall1(SYS_CLOSE, walks_pipe[0]);
    syscall1(SYS_CLOSE, walks_pipe[1]);
    walks_pipe[0] = walks_pipe[1] = -1;
    if (syscall1(SYS_UNLINK, (long)abs_mnt("/a/b/f")) != 0 ||
        syscall1(SYS_RMDIR, (long)abs_mnt("/a/b")) != 0 ||
        syscall1(SYS_RMDIR, (long)abs_mnt("/a")) != 0) {
        fail("what the gated walk row made is there to remove");
    }
}

/**
 * The bound of links, Linux's 40 (fs/namei.c MAXSYMLINKS in 6.12): in the
 * tmpfs at the mount point, kN links to kN+1 for N from 1 to 40 and k41 to
 * f. k2 reaches f through 40 links and k1 is ELOOP; m40, a link to
 * /proc/self/cwd/k5, reaches f through m40, /proc/self, the jump through
 * cwd and 37 more, and m41, to /proc/self/cwd/k4, is ELOOP, since a jump
 * through a link of /proc counts as any link does.
 */
static void links_row(void)
{
    char name[8], target[8], digits[12];
    int i;

    walks_enter(abs_mnt(""), "the working directory of the link row");
    put_byte("f", 'l', "the file of the link row");
    for (i = 1; i <= 41; i++) {
        join3(name, (int)sizeof(name), "k", decimal(digits, i), "");
        join3(target, (int)sizeof(target), i < 41 ? "k" : "f", i < 41 ? decimal(digits, i + 1) : "",
              "");
        if (syscall2(SYS_SYMLINK, (long)target, (long)name) != 0) {
            fail("the links of the link row");
        }
        walks_links = i;
    }
    if (syscall2(SYS_SYMLINK, (long)"/proc/self/cwd/k5", (long)"m40") != 0 ||
        syscall2(SYS_SYMLINK, (long)"/proc/self/cwd/k4", (long)"m41") != 0) {
        fail("the links through /proc of the link row");
    }
    expect_byte(AT_FDCWD, "k2", 'l', "a walk follows 40 links");
    if (syscall3(SYS_OPEN, (long)"k1", O_RDONLY, 0) != -ELOOP) {
        fail("a walk refuses the 41st link with ELOOP");
    }
    expect_byte(AT_FDCWD, "m40", 'l', "a walk follows 40 links, a jump through /proc among them");
    if (syscall3(SYS_OPEN, (long)"m41", O_RDONLY, 0) != -ELOOP) {
        fail("a jump through a link of /proc counts toward the 40 links");
    }
    walks_unlink();
    walks_leave();
}

/**
 * The rows of where a walk starts and how far it goes, on a tmpfs at the
 * mount point: a held /dev/pts that a bind covers (`synth_start_row`), an
 * O_CREAT|O_TRUNC walk that a cover comes over while the walk gate holds it
 * (`gated_walk_row`), and the bound of 40 links (`links_row`). A failure
 * undoes them (`walks_undo`).
 */
static void rows_walks(void)
{
    walks_pipe[0] = walks_pipe[1] = -1;
    if (syscall5(SYS_MOUNT, (long)"none", (long)mnt, (long)"tmpfs", 0, 0) != 0) {
        fail("a tmpfs for the walk rows");
    }
    row_undo = walks_undo;
    synth_start_row();
    gated_walk_row(0);
    gated_walk_row(1);
    links_row();
    if (syscall2(SYS_UMOUNT2, (long)mnt, 0) != 0) {
        fail("the tmpfs of the walk rows unmounts");
    }
    row_undo = 0;
}

/*
 * The `reloc` rows: a file of a mounted KRFFS volume moves while a bind of
 * it waits, the source named by a descriptor and by its path, and the bind
 * must judge and attach the file where it is now. The suite gives the rows
 * an empty KRFFS volume on /dev/disk0p1, whose writes QEMU throttles to 50
 * a second, and on /dev/disk2p1, on the other IDE channel, a FAT32 volume
 * whose reads it throttles to one a second, with a target three
 * directories deep for each try of the two path rows, /dN/a/b/c/t for the
 * file target's tries (N 1 to 3) and /dN/a/b/c/td for the directory
 * target's (N 4 to 6), each directory in a cluster of its own that 64 KiB
 * of padding keeps apart from the next, so that no try finds its target's
 * directories in the cache.
 *
 * In each row a writer grows /rk/f from 4 KiB of 'A' to 64 KiB of 'B', which
 * moves it past /rk/g after it, and a mkdir of /rk/d then takes its old
 * offset first fit, so that a bind judging the file's old node finds a
 * directory there. By descriptor, a syncer's fsync of a dirty file takes the
 * volume's namespace lock across a throttled write, so that the writer and
 * the mkdir queue on the lock; the binder copies the descriptor's node while
 * they still sleep there and queues behind them. By path, the binder takes
 * its hold on the file under the lock, lets the lock go and sleeps reading
 * the uncached directories of its target on the FAT volume while the writer
 * and the mkdir run.
 *
 * The answer is judged first, since a correct kernel gives it in any order:
 * a file target shows the moved file, and a directory target is refused
 * with ENOTDIR. The order is then read from the kernel's bind trace
 * (/sys/kernel/debug/vfs/bind_trace), armed for the try: by descriptor the
 * bind copied the descriptor's node at the file's old place before the
 * move, and took its hold after it, under the lock the move held; by path
 * it took its hold on the old node before the move; and in both its last
 * judgement of its source came after the move. A try whose trace shows
 * another order says so and runs again, and the row fails when three tries
 * did not get it. Once the order is got, the bind must have judged, and by
 * descriptor held, the node the file moved to. The yields only make the
 * order likely, and the bind's wait is printed for the record.
 */

#define O_RDWR       2
#define O_PATH       010000000
#define SIGKILL      9
#define TRACE_EVENTS 64
#define TRACE_PATH   "/sys/kernel/debug/vfs/bind_trace"

/* One event of the kernel's bind trace, as its line gives it. */
struct trace_event {
    long seq;
    char kind;          /* 'c' copy, 'h' hold, 'j' judge or 'm' move */
    unsigned long pid;  /* the process that recorded it */
    unsigned long a, b; /* the node, or a move's from and to */
};

static struct trace_event events[TRACE_EVENTS];
static int nevents;
static unsigned long trace_lost; /* events past the kernel's ring */
static char chunk[65536];

/* One try of a row: the file's node before the move (`was`), after it
 * (`now`) and the directory's (`made`); the bind's wait in microseconds and
 * its answer; and what the trace says, the sequence numbers of the bind's
 * first copy of its descriptor's node, its first hold, its last judgement
 * and the file's move, -1 for none, and the nodes each had. */
struct reloc_try {
    unsigned long was, now, made;
    long waited, rc;
    long copy, hold, judge, move;
    unsigned long copied, held, judged, moved;
};

/** The inode number or the size of `path` from stat(2), as `at`, KSTAT_INO
 * or KSTAT_SIZE_AT, says; a KRFFS node's inode number is its offset plus
 * one, the node the bind trace gives. Fails the row when stat does. */
static unsigned long stat_field(const char *path, int at)
{
    static char st[KSTAT_SIZE];

    if (syscall2(SYS_STAT, (long)path, (long)st) != 0) {
        fail(path);
    }
    return *(unsigned long *)(st + at);
}

/** Write `word` to the kernel's bind trace: 0, or the negative errno. */
static long trace_write(const char *word)
{
    long fd = syscall3(SYS_OPEN, (long)TRACE_PATH, O_WRONLY, 0), rc;

    if (fd < 0) {
        return fd;
    }
    rc = syscall3(SYS_WRITE, fd, (long)word, (long)strlen(word));
    syscall1(SYS_CLOSE, fd);
    return rc < 0 ? rc : 0;
}

/** Read the decimal at `p` into `*v`; the character after it, or 0 when
 * `p` holds no digit. */
static const char *trace_number(const char *p, unsigned long *v)
{
    const char *start = p;

    for (*v = 0; *p >= '0' && *p <= '9'; p++) {
        *v = *v * 10 + (unsigned long)(*p - '0');
    }
    return p == start ? 0 : p;
}

/** Read one line `SEQ KIND PID A B` of the trace at `p` into `e`; the next
 * line, or 0 when the line is not one. */
static const char *trace_line(const char *p, struct trace_event *e)
{
    unsigned long seq;

    p = trace_number(p, &seq);
    if (!p || *p++ != ' ') {
        return 0;
    }
    e->seq = (long)seq;
    e->kind = p[0] == 'm' && p[1] == 'o' ? 'm' : p[0];
    while (*p >= 'a' && *p <= 'z') {
        p++;
    }
    if (*p++ != ' ' || !(p = trace_number(p, &e->pid)) || *p++ != ' ' ||
        !(p = trace_number(p, &e->a)) || *p++ != ' ' || !(p = trace_number(p, &e->b)) ||
        *p++ != '\n') {
        return 0;
    }
    return p;
}

/** Read the kernel's bind trace into `events` and `trace_lost`: 0, or -1
 * when it cannot be read or a line of it is not the trace's. */
static int trace_read(void)
{
    static char text[8192];
    long fd = syscall3(SYS_OPEN, (long)TRACE_PATH, O_RDONLY, 0), n, got = 0;
    const char *p = text;

    if (fd < 0) {
        return -1;
    }
    do {
        n = syscall3(SYS_READ, fd, (long)text + got, (long)sizeof(text) - 1 - got);
        got += n > 0 ? n : 0;
    } while (n > 0 && got < (long)sizeof(text) - 1);
    syscall1(SYS_CLOSE, fd);
    text[got] = 0;
    if (n < 0) {
        return -1;
    }
    for (nevents = 0; nevents < TRACE_EVENTS && *p >= '0' && *p <= '9'; nevents++) {
        p = trace_line(p, &events[nevents]);
        if (!p) {
            return -1;
        }
    }
    if (p[0] != 'l' || p[1] != 'o' || p[2] != 's' || p[3] != 't' || p[4] != ' ' ||
        !(p = trace_number(p + 5, &trace_lost)) || *p != '\n') {
        return -1;
    }
    return 0;
}

/** Fill the trace's part of `o` for the bind of process `binder` of the
 * file whose node was `o->was`. */
static void reloc_order_read(struct reloc_try *o, unsigned long binder)
{
    unsigned long was = o->was;
    const struct trace_event *e;
    int i;

    o->copy = o->hold = o->judge = o->move = -1;
    o->copied = o->held = o->judged = o->moved = 0;
    for (i = 0; i < nevents; i++) {
        e = &events[i];
        if (e->kind == 'm' && e->a == was && o->move < 0) {
            o->move = e->seq;
            o->moved = e->b;
        } else if (e->pid != binder) {
            continue;
        } else if (e->kind == 'c' && o->copy < 0) {
            o->copy = e->seq;
            o->copied = e->a;
        } else if (e->kind == 'h' && o->hold < 0) {
            o->hold = e->seq;
            o->held = e->a;
        } else if (e->kind == 'j') {
            o->judge = e->seq;
            o->judged = e->a;
        }
    }
}

/**
 * Why the try `o` did not get its row's order, or 0 when it did: the file
 * moved and the directory took its old node, and the trace shows, by
 * descriptor (`by_fd`), the copy before the move and the hold after it, by
 * path, the hold on the old node before the move, and in both the last
 * judgement after the move.
 */
static const char *reloc_order_missed(int by_fd, const struct reloc_try *o)
{
    unsigned long was = o->was;

    if (o->move < 0 || o->moved != o->now || o->now == was) {
        return "the kernel recorded no move of the file to where it is";
    }
    if (o->made != was) {
        return "the directory did not take the file's old node";
    }
    if (o->hold < 0 || o->judge < 0) {
        return "the kernel recorded no hold or no judgement by the bind";
    }
    if (by_fd && (o->copy < 0 || o->copied != was || o->copy > o->move)) {
        return "the bind copied the descriptor's node after the move";
    }
    if (by_fd && o->hold < o->move) {
        return "the bind took its hold before the move";
    }
    if (!by_fd && (o->held != was || o->hold > o->move)) {
        return "the bind took its hold after the move";
    }
    if (o->judge < o->move) {
        return "the bind's last judgement of its source came before the move";
    }
    return 0;
}

/** Make `path` hold `length` bytes of `c`. */
static void fill_file(const char *path, char c, long length)
{
    long fd = syscall3(SYS_OPEN, (long)path, O_CREAT | O_WRONLY | O_TRUNC, 0600);
    long i;

    if (fd < 0) {
        fail("create a relocation file");
    }
    for (i = 0; i < length; i++) {
        chunk[i] = c;
    }
    if (syscall3(SYS_WRITE, fd, (long)chunk, length) != length) {
        fail("write a relocation file");
    }
    syscall1(SYS_CLOSE, fd);
}

/** Give the processor up `times` times. */
static void yield(int times)
{
    while (times-- > 0) {
        syscall0(SYS_SCHED_YIELD);
    }
}

/**
 * Fork a child for one of the row's calls: after `wait` yields it grows the
 * file open on `fd` to 64 KiB of 'B' (`what` 'g'), makes /rk/d ('m'), or
 * fsyncs `fd` ('s'), and exits 0 when the call did what it should. Returns
 * the child's PID, or -1 when the fork failed.
 */
static int reloc_child(char what, long fd, int wait)
{
    int pid = syscall0(SYS_FORK);
    long i, rc;

    if (pid != 0) {
        return pid < 0 ? -1 : pid;
    }
    yield(wait);
    if (what == 'g') {
        for (i = 0; i < (long)sizeof(chunk); i++) {
            chunk[i] = 'B';
        }
        rc = syscall3(SYS_WRITE, fd, (long)chunk, sizeof(chunk)) == (long)sizeof(chunk) ? 0 : -1;
    } else if (what == 'm') {
        rc = syscall2(SYS_MKDIR, (long)"/rk/d", 0755);
    } else {
        rc = syscall1(SYS_FSYNC, fd);
    }
    sys_exit(rc != 0);
}

/** Wait for the child `pid`: its status, or -1 when waitpid fails. */
static int reloc_wait(int pid)
{
    int status = 0;

    return syscall3(SYS_WAITPID, pid, (long)&status, 0) == pid ? status : -1;
}

/** Kill and reap each child of `pids`, 0 for none, so that none outlives a
 * failed start. */
static void reloc_stop(const int pids[3])
{
    int i;

    for (i = 0; i < 3; i++) {
        if (pids[i] > 0) {
            syscall2(SYS_KILL, pids[i], SIGKILL);
            reloc_wait(pids[i]);
        }
    }
}

/**
 * Start the children of one row and leave the binder ready: by descriptor
 * (`by_fd`) the source is /proc/self/fd/9, an O_PATH descriptor of /rk/f,
 * and the syncer, the writer and the mkdir are started in that order, the
 * last two to queue on the volume's lock; by path the writer and the mkdir
 * wait in yields for the binder to sleep on the FAT volume. `pids` gets the
 * three children, 0 for none. Returns 0, or what failed, with every child
 * it started stopped and reaped.
 */
static const char *reloc_start(int by_fd, int pids[3])
{
    long wr = syscall3(SYS_OPEN, (long)"/rk/f", O_WRONLY, 0), opath, big;

    pids[0] = pids[1] = pids[2] = 0;
    if (wr < 0) {
        return "open the file to grow";
    }
    if (by_fd) {
        opath = syscall3(SYS_OPEN, (long)"/rk/f", O_PATH, 0);
        if (opath < 0 || syscall2(SYS_DUP2, opath, 9) != 9) {
            return "open the bind's source O_PATH as descriptor 9";
        }
        fill_file("/rk/big", 'Z', 4096);
        big = syscall3(SYS_OPEN, (long)"/rk/big", O_RDWR, 0);
        if (big < 0 || syscall3(SYS_WRITE, big, (long)chunk, 4096) != 4096) {
            return "dirty the file the syncer writes back";
        }
        pids[0] = reloc_child('s', big, 0);
        yield(4);
    }
    if (pids[0] >= 0) {
        pids[1] = reloc_child('g', wr, by_fd ? 0 : 40);
    }
    if (pids[0] >= 0 && pids[1] >= 0) {
        pids[2] = reloc_child('m', 0, by_fd ? 10 : 80);
    }
    if (pids[0] < 0 || pids[1] < 0 || pids[2] < 0) {
        reloc_stop(pids);
        return "fork for a relocation row";
    }
    if (by_fd) {
        yield(100);
    }
    return 0;
}

/** Fail unless the bind of the moved file on `target` shows its 64 KiB of
 * 'B', and take the bind away. */
static void reloc_bound_file(const char *target)
{
    long fd, n, i, got = 0;

    if (stat_field(target, KSTAT_SIZE_AT) != sizeof(chunk)) {
        fail("the bind does not show the moved file's length");
    }
    fd = syscall3(SYS_OPEN, (long)target, O_RDONLY, 0);
    if (fd < 0) {
        fail("open the moved file through its bind");
    }
    while ((n = syscall3(SYS_READ, fd, (long)buf, sizeof(buf))) > 0) {
        for (i = 0; i < n; i++) {
            if (buf[i] != 'B') {
                fail("the bind shows bytes other than the moved file's");
            }
        }
        got += n;
    }
    syscall1(SYS_CLOSE, fd);
    if (n < 0 || got != (long)sizeof(chunk)) {
        fail("read the moved file's 64 KiB through its bind");
    }
    if (syscall2(SYS_UMOUNT2, (long)target, 0) != 0) {
        fail("unmount the bind of the moved file");
    }
}

/** The target of try `try` of the row by descriptor (`by_fd`) or by path,
 * onto a file or, with `dir`, a directory, into `out` of 32 bytes. */
static void reloc_target(int by_fd, int dir, int try, char *out)
{
    const char *t =
        by_fd ? (dir ? "/rk-d" : "/rk-t") : (dir ? "/fat/dN/a/b/c/td" : "/fat/dN/a/b/c/t");
    int i;

    for (i = 0; t[i]; i++) {
        out[i] = t[i] == 'N' ? (char)('1' + dir * 3 + try) : t[i];
    }
    out[i] = 0;
}

/** Microseconds of CLOCK_MONOTONIC, for the record of a bind's wait. */
static long now_us(void)
{
    struct {
        long sec, nsec;
    } ts;

    syscall2(SYS_CLOCK_GETTIME, 1, (long)&ts);
    return ts.sec * 1000000 + ts.nsec / 1000;
}

/** Append `s` to `line`, of 256 bytes, at `*len`. */
static void append_text(char *line, int *len, const char *s)
{
    while (*s && *len < 250) {
        line[(*len)++] = *s++;
    }
    line[*len] = 0;
}

/** Append `s` and then the decimal `v` to `line`, of 256 bytes, at
 * `*len`. */
static void append(char *line, int *len, const char *s, long v)
{
    char digits[24];
    int i = 0;
    unsigned long u = v < 0 ? (unsigned long)-v : (unsigned long)v;

    append_text(line, len, s);
    if (v < 0) {
        line[(*len)++] = '-';
    }
    do {
        digits[i++] = (char)('0' + u % 10);
        u /= 10;
    } while (u > 0);
    while (i > 0 && *len < 250) {
        line[(*len)++] = digits[--i];
    }
    line[*len] = 0;
}

/** Print that the try did not get its order, for `why`, and exit 2, which
 * runs the row again. */
static void reloc_missed(const char *why)
{
    char line[256];
    int len = 0;

    append_text(line, &len, "reloc: order not got: ");
    append_text(line, &len, why);
    puts(line);
    sys_exit(2);
}

/** Print the record of the try `o` of the row by descriptor (`by_fd`) or
 * by path onto a file or, with `dir`, a directory. */
static void reloc_record(int by_fd, int dir, const struct reloc_try *o)
{
    char line[256];
    int len = 0;

    append(line, &len, by_fd ? "reloc: fd source: " : "reloc: path source: ", (long)o->was);
    append(line, &len, " moved to ", (long)o->now);
    append(line, &len, ", directory at ", (long)o->made);
    if (by_fd) {
        append(line, &len, "; copy ", o->copy);
        append(line, &len, " at ", (long)o->copied);
    }
    append(line, &len, by_fd ? ", hold " : "; hold ", o->hold);
    append(line, &len, " at ", (long)o->held);
    append(line, &len, ", move ", o->move);
    append(line, &len, ", judge ", o->judge);
    append(line, &len, " at ", (long)o->judged);
    append(line, &len, "; waited ", o->waited / 1000);
    append(line, &len, dir ? " ms, directory target: " : " ms, file target: ", o->rc);
    puts(line);
}

/**
 * One try of a row, in a child of its own: bind /rk/f, by descriptor or by
 * path, onto a file or, with `dir`, a directory, while the file moves, and
 * judge the bind's answer, then the order the trace shows, then the nodes
 * the bind judged and held. A trace that lost events fails the row, since
 * the order it lacks cannot be judged. Exits 2 through `reloc_missed` only
 * when a whole trace shows that the order was not got. Every child it
 * starts is reaped before it fails.
 */
static void reloc_bind(int by_fd, int dir, int try)
{
    const char *source = by_fd ? "/proc/self/fd/9" : "/rk/f", *why;
    unsigned long binder;
    int pids[3], bad = 0, read, i;
    struct reloc_try o;
    char target[32];

    reloc_target(by_fd, dir, try, target);
    fill_file("/rk/f", 'A', 4096);
    fill_file("/rk/g", 'G', 4096);
    o.was = stat_field("/rk/f", KSTAT_INO);
    binder = (unsigned long)syscall0(SYS_GETPID);
    if (trace_write("arm") != 0) {
        fail("arm the kernel's bind trace");
    }
    why = reloc_start(by_fd, pids);
    if (why) {
        fail(why);
    }
    o.waited = now_us();
    o.rc = syscall5(SYS_MOUNT, (long)source, (long)target, 0, MS_BIND, 0);
    o.waited = now_us() - o.waited;
    for (i = 0; i < 3; i++) {
        bad |= pids[i] > 0 && reloc_wait(pids[i]) != 0;
    }
    read = trace_read();
    trace_write("off");
    if (bad) {
        fail("the syncer, the writer or the mkdir of a relocation row failed");
    }
    if (read != 0) {
        fail("read the kernel's bind trace");
    }
    if (trace_lost != 0) {
        fail("the kernel's bind trace lost events");
    }
    o.now = stat_field("/rk/f", KSTAT_INO);
    o.made = stat_field("/rk/d", KSTAT_INO);
    reloc_order_read(&o, binder);
    reloc_record(by_fd, dir, &o);
    if (dir ? o.rc != -ENOTDIR : o.rc != 0) {
        fail(dir ? "a bind of the moved file onto a directory is not ENOTDIR"
                 : "a bind of the moved file onto a file failed");
    }
    if (!dir) {
        reloc_bound_file(target);
    }
    why = reloc_order_missed(by_fd, &o);
    if (why) {
        reloc_missed(why);
    }
    if (o.judged != o.now || (by_fd && o.held != o.now)) {
        fail("the bind judged or held its source at the node the file had left");
    }
}

/** Count the removal answer `rc` as a failure, printed with `what`, unless
 * it is 0 or `none`, the answer when there was nothing to remove. */
static int reloc_gone(const char *what, long rc, long none)
{
    char line[256];
    int len = 0;

    if (rc == 0 || rc == none) {
        return 0;
    }
    append_text(line, &len, "MOUNTPROBE FAIL: ");
    append(line, &len, what, rc);
    puts(line);
    return 1;
}

/** Remove what a try left: the bind on `target`, the row's files and the
 * directory made at the file's old node. Returns the removals that
 * failed. */
static int reloc_tidy(const char *target)
{
    int bad = 0;

    bad += reloc_gone("detach the bind of a relocation row: ",
                      syscall2(SYS_UMOUNT2, (long)target, MNT_DETACH), -EINVAL);
    bad += reloc_gone("remove /rk/f: ", syscall1(SYS_UNLINK, (long)"/rk/f"), -ENOENT);
    bad += reloc_gone("remove /rk/g: ", syscall1(SYS_UNLINK, (long)"/rk/g"), -ENOENT);
    bad += reloc_gone("remove /rk/big: ", syscall1(SYS_UNLINK, (long)"/rk/big"), -ENOENT);
    bad += reloc_gone("remove /rk/d: ", syscall1(SYS_RMDIR, (long)"/rk/d"), -ENOENT);
    return bad;
}

/** Run `reloc_bind` in a child, so that a failed row still lets the next
 * run, then remove what it left. A try that did not get the row's order
 * runs again, on a target of its own, three tries in all. Returns 1 for a
 * failed row. */
static int reloc_round(int by_fd, int dir)
{
    char target[32];
    int pid, status, tries, bad;

    for (tries = 0; tries < 3; tries++) {
        pid = syscall0(SYS_FORK);
        if (pid < 0) {
            puts("MOUNTPROBE FAIL: fork for a relocation row");
            return 1;
        }
        if (pid == 0) {
            reloc_bind(by_fd, dir, tries);
            sys_exit(0);
        }
        status = reloc_wait(pid);
        reloc_target(by_fd, dir, tries, target);
        bad = reloc_tidy(target);
        if (bad || status != 2 << 8) {
            return status != 0 || bad;
        }
    }
    puts("MOUNTPROBE FAIL: no try of a relocation row got its order");
    return 1;
}

/** Take away every mount and mount point of the `reloc` rows, and disarm
 * the bind trace, each tried whatever the others answer. Returns the
 * removals that failed, each printed. */
static int reloc_cleanup(void)
{
    int bad = 0;

    bad += reloc_gone("disarm the kernel's bind trace: ", trace_write("off"), -ENOENT);
    bad += reloc_gone("unmount /fat: ", syscall2(SYS_UMOUNT2, (long)"/fat", 0), -EINVAL);
    bad += reloc_gone("unmount /rk: ", syscall2(SYS_UMOUNT2, (long)"/rk", 0), -EINVAL);
    bad += reloc_gone("remove /rk-t: ", syscall1(SYS_UNLINK, (long)"/rk-t"), -ENOENT);
    bad += reloc_gone("remove /rk-d: ", syscall1(SYS_RMDIR, (long)"/rk-d"), -ENOENT);
    bad += reloc_gone("remove /fat: ", syscall1(SYS_RMDIR, (long)"/fat"), -ENOENT);
    bad += reloc_gone("remove /rk: ", syscall1(SYS_RMDIR, (long)"/rk"), -ENOENT);
    return bad;
}

/** `mountprobe reloc`: make the mount points, mount the two volumes, run
 * the four rows, by descriptor and by path onto a file and onto a
 * directory, take every mount and mount point away whatever failed, and
 * print MOUNTPROBE OK when every step and row passed. Does not return. */
static void rows_reloc(void)
{
    const char *why = 0;
    long fd;
    int failed = 0;

    if (syscall2(SYS_MKDIR, (long)"/rk", 0755) != 0 ||
        syscall2(SYS_MKDIR, (long)"/fat", 0755) != 0 ||
        syscall2(SYS_MKDIR, (long)"/rk-d", 0755) != 0) {
        why = "make the relocation rows' mount points";
    }
    fd = why ? -1 : syscall3(SYS_OPEN, (long)"/rk-t", O_CREAT | O_WRONLY, 0600);
    if (!why && fd < 0) {
        why = "make the relocation rows' file target";
    }
    if (fd >= 0) {
        syscall1(SYS_CLOSE, fd);
    }
    if (!why && syscall5(SYS_MOUNT, (long)"/dev/disk0p1", (long)"/rk", (long)"krffs", 0, 0) != 0) {
        why = "mount the relocation volume";
    }
    if (!why && do_mount("/dev/disk2p1", "/fat", 0, 0) != 0) {
        why = "mount the FAT volume";
    }
    if (!why) {
        failed = reloc_round(1, 0) + reloc_round(1, 1) + reloc_round(0, 0) + reloc_round(0, 1);
    }
    if (reloc_cleanup() != 0 && !why) {
        why = "take the relocation rows' mounts and mount points away";
    }
    if (why) {
        fail(why);
    }
    if (failed) {
        fail("a bind of a KRFFS file that moved while the bind waited");
    }
    puts("MOUNTPROBE OK");
    sys_exit(0);
}

/** Run the `claim` rows when the first positional argument begins with c,
 * and the `reloc` rows when it begins with r; otherwise run every other
 * group of rows in order and remove the mount points. */
void probe_main(unsigned long *sp)
{
    const char *mode;

    probe_args((int)sp[0], (char **)(sp + 1));
    mode = probe_word(1);
    if (mode != 0 && mode[0] == 'c') {
        claim_mode = 1;
        rows_claim();
    }
    if (mode != 0 && mode[0] == 'r') {
        rows_reloc();
    }
    if (probe_native()) {
        native_paths();
    }
    rows_placing();
    rows_reading();
    rows_proc();
    rows_lifetime();
    rows_flags();
    rows_record();
    rows_namespaces();
    rows_copies();
    rows_covered();
    rows_walks();
    syscall1(SYS_RMDIR, (long)mnt2);
    syscall1(SYS_RMDIR, (long)mnt);
    puts("MOUNTPROBE OK");
    sys_exit(0);
}
