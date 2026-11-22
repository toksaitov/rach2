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
 * say what is mounted; and the kernel's record of an unmount counts
 * descriptions, not descriptors. `mountprobe claim` runs the boot
 * partition's rows alone (`rows_claim`), and `mountprobe reloc` the rows of
 * a bind whose KRFFS source moves while it waits (`rows_reloc`), on disks of
 * their own.
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

#define O_CREAT   0100
#define O_RDONLY  0
#define O_WRONLY  1
#define O_TRUNC   01000
#define O_CLOEXEC 02000000
#define F_DUPFD   0

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

#define STATFS_MAGIC_VFAT 0x4d44

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

static int claim_mode; /* the `claim` mode, whose lines say CLAIMPROBE */

/** Print the mode's FAIL line with `why`, and exit 1. */
static void fail(const char *why) __attribute__((noreturn));
static void fail(const char *why)
{
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

/*
 * The `reloc` rows: a file of a mounted KRFFS volume moves while a bind of
 * it waits, the source named by a descriptor and by its path, and the bind
 * must judge and attach the file where it is now. The suite gives the rows
 * an empty KRFFS volume on /dev/disk0p1, whose writes QEMU throttles to 50
 * a second, and on /dev/disk2p1, on the other IDE channel, a FAT32 volume
 * whose reads it throttles to one a second, with the targets /d1/a/b/c/t and
 * /d2/a/b/c/td three directories deep, each directory in a cluster of its
 * own that 64 KiB of padding keeps apart from the next.
 *
 * In each row a writer grows /rk/f from 4 KiB of 'A' to 64 KiB of 'B', which
 * moves it past /rk/g after it, and a mkdir of /rk/d then takes its old
 * offset first fit, so that a bind holding the file's old node would find a
 * directory there. By descriptor, a syncer's fsync of a dirty file takes the
 * volume's namespace lock across a throttled write, so that the writer and
 * the mkdir queue on the lock; the binder copies the descriptor's node while
 * they still sleep there and queues behind them, and the lock serves its
 * waiters in order. By path, the binder takes its hold on the file under the
 * lock, lets the lock go and sleeps reading the uncached directories of its
 * target on the FAT volume while the writer and the mkdir run.
 *
 * A row judges the order it got from what the kernel says. In both the file
 * has not moved when the bind is called, and has moved, with the directory
 * at its old offset, once it returns. By descriptor the writer and the mkdir
 * are inside their calls when the bind is called, which each marks in a page
 * the processes share as it makes the call, and neither has returned; and
 * the bind sleeps (a voluntary switch in /proc/self/status) and so waits on
 * the lock behind them. By path the bind reads disk2 (/proc/diskstats), and
 * the reads of it that have ended when the write and the mkdir return are
 * fewer than when the bind returns: the bind was still looking its target up
 * once the file had moved, and it took its hold before its first read. A try
 * that did not get its order says so and runs again, and the row fails when
 * three tries did not get it. The yields only make the order likely; the
 * bind's wait and the waiters' states in /proc/PID/stat are printed for the
 * record.
 */

#define O_RDWR        2
#define O_PATH        010000000
#define PROT_WRITE    2
#define MAP_ANONYMOUS 0x20

/* Where the writer and the mkdir are in their calls, in a page the row's
 * processes share. */
struct reloc_seen {
    int writing; /* 1 once the writer is about to make its write */
    int making;  /* 1 once the mkdir is about to be made */
    long grown;  /* the reads of disk2 that had ended when the write, which
                    moves /rk/f, returned, or -1 before */
    long made;   /* the same when the mkdir of /rk/d returned */
};

static struct reloc_seen *seen;
static char chunk[65536];

/** The inode number or the size of `path` from stat(2), as `at`, KSTAT_INO
 * or KSTAT_SIZE_AT, says; a KRFFS node's inode number is its offset plus
 * one. Fails the row when stat does. */
static unsigned long stat_field(const char *path, int at)
{
    static char st[KSTAT_SIZE];

    if (syscall2(SYS_STAT, (long)path, (long)st) != 0) {
        fail(path);
    }
    return *(unsigned long *)(st + at);
}

/** The number after `key` in the text of `path`, or -1 when the file cannot
 * be read or has no such key. */
static long proc_number(const char *path, const char *key)
{
    static char text[8192];
    long fd = syscall3(SYS_OPEN, (long)path, O_RDONLY, 0), n, v;
    const char *p;
    int i = 0;

    if (fd < 0) {
        return -1;
    }
    n = syscall3(SYS_READ, fd, (long)text, (int)sizeof(text) - 1);
    syscall1(SYS_CLOSE, fd);
    text[n > 0 ? n : 0] = 0;
    for (p = text; *p; p++) {
        for (i = 0; key[i] && p[i] == key[i]; i++) {
        }
        if (!key[i]) {
            break;
        }
    }
    if (!*p) {
        return -1;
    }
    for (p += i; *p == ' ' || *p == '\t'; p++) {
    }
    for (v = 0; *p >= '0' && *p <= '9'; p++) {
        v = v * 10 + (*p - '0');
    }
    return v;
}

/** The voluntary context switches of this process. */
static long voluntary_switches(void)
{
    return proc_number("/proc/self/status", "\nvoluntary_ctxt_switches:");
}

/** The reads of disk2, the FAT volume's disk, that have ended. */
static long fat_reads(void)
{
    return proc_number("/proc/diskstats", " disk2 ");
}

/** The state of process `pid` in its /proc/PID/stat, or '?' when it cannot
 * be read. */
static char proc_state(int pid)
{
    char path[32] = "/proc/", digits[12], line[256] = "";
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
        return '?';
    }
    n = syscall3(SYS_READ, fd, (long)line, (int)sizeof(line) - 1);
    syscall1(SYS_CLOSE, fd);
    for (i = n - 1; i >= 0 && line[i] != ')'; i--) {
    }
    return i >= 0 && i + 2 < n ? line[i + 2] : '?';
}

/** The inode number of descriptor `fd`, from fstat(2), which reads a KRFFS
 * node without its namespace lock. */
static unsigned long fd_ino(int fd)
{
    static char st[KSTAT_SIZE];

    if (syscall2(SYS_FSTAT, fd, (long)st) != 0) {
        fail("fstat of the bind's source descriptor");
    }
    return *(unsigned long *)(st + KSTAT_INO);
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
 * fsyncs `fd` ('s'), records in `seen` the reads of disk2 that have ended
 * once the write or the mkdir has returned, and exits 0 when the call did
 * what it should. Returns the child's PID.
 */
static int reloc_child(char what, long fd, int wait)
{
    int pid = syscall0(SYS_FORK);
    long i, rc;

    if (pid < 0) {
        fail("fork for a relocation row");
    }
    if (pid > 0) {
        return pid;
    }
    yield(wait);
    if (what == 'g') {
        for (i = 0; i < (long)sizeof(chunk); i++) {
            chunk[i] = 'B';
        }
        seen->writing = 1;
        rc = syscall3(SYS_WRITE, fd, (long)chunk, sizeof(chunk)) == (long)sizeof(chunk) ? 0 : -1;
        seen->grown = fat_reads();
    } else if (what == 'm') {
        seen->making = 1;
        rc = syscall2(SYS_MKDIR, (long)"/rk/d", 0755);
        seen->made = fat_reads();
    } else {
        rc = syscall1(SYS_FSYNC, fd);
    }
    sys_exit(rc != 0);
}

/** Reap `pid`, when there is one, and fail with `what` unless it exited 0. */
static void reloc_reap(int pid, const char *what)
{
    if (pid > 0 && wait_status(pid) != 0) {
        fail(what);
    }
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

/**
 * Start the waiters of one row and leave the binder ready: by descriptor
 * (`by_fd`) the source is /proc/self/fd/9, an O_PATH descriptor of /rk/f,
 * and the syncer, the writer and the mkdir are started in that order, the
 * last two to queue on the volume's lock; by path the writer and the mkdir
 * wait in yields for the binder to sleep on the FAT volume. `pids` gets the
 * three children, 0 for none.
 */
static void reloc_start(int by_fd, int pids[3])
{
    long wr = syscall3(SYS_OPEN, (long)"/rk/f", O_WRONLY, 0), opath, big;

    if (wr < 0) {
        fail("open the file to grow");
    }
    pids[0] = 0;
    if (!by_fd) {
        pids[1] = reloc_child('g', wr, 40);
        pids[2] = reloc_child('m', 0, 80);
        return;
    }
    opath = syscall3(SYS_OPEN, (long)"/rk/f", O_PATH, 0);
    if (opath < 0 || syscall2(SYS_DUP2, opath, 9) != 9) {
        fail("open the bind's source O_PATH as descriptor 9");
    }
    fill_file("/rk/big", 'Z', 4096);
    big = syscall3(SYS_OPEN, (long)"/rk/big", O_RDWR, 0);
    if (big < 0 || syscall3(SYS_WRITE, big, (long)chunk, 4096) != 4096) {
        fail("dirty the file the syncer writes back");
    }
    pids[0] = reloc_child('s', big, 0);
    yield(4);
    pids[1] = reloc_child('g', wr, 0);
    pids[2] = reloc_child('m', 0, 10);
    yield(100);
}

/** Print that the row did not get its order, for `why`, and exit 2, which
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

/**
 * Exit through `reloc_missed` unless the row got its order. `before` is
 * nonzero when, as the bind was called, the file had not moved and, by
 * descriptor, the writer and the mkdir were inside their calls; `slept`
 * counts the bind's voluntary switches by descriptor and its reads of disk2
 * by path; `reads` is disk2's ended reads when the bind returned.
 */
static void reloc_order_held(int by_fd, int before, long slept, long reads)
{
    if (!before) {
        reloc_missed(by_fd ? "the file moved, or the writer or the mkdir was not inside its call, "
                             "before the bind"
                           : "the file moved before the bind");
    }
    if (slept < 1) {
        reloc_missed(by_fd ? "the bind did not wait for the volume's lock"
                           : "the bind did not read the FAT volume");
    }
    if (!by_fd &&
        (seen->grown < 0 || seen->grown >= reads || seen->made < 0 || seen->made >= reads)) {
        reloc_missed("the file moved after the bind's last read of the FAT volume");
    }
}

/** Fail unless the bind of the moved file on `target` shows its 64 KiB of
 * 'B', and take the bind away. */
static void reloc_bound_file(const char *target)
{
    long fd, i;

    if (stat_field(target, KSTAT_SIZE_AT) != sizeof(chunk)) {
        fail("the bind does not show the moved file's length");
    }
    fd = syscall3(SYS_OPEN, (long)target, O_RDONLY, 0);
    if (fd < 0 || syscall3(SYS_READ, fd, (long)buf, 8) != 8) {
        fail("read the moved file through its bind");
    }
    syscall1(SYS_CLOSE, fd);
    for (i = 0; i < 8; i++) {
        if (buf[i] != 'B') {
            fail("the bind shows the file's old bytes");
        }
    }
    if (syscall2(SYS_UMOUNT2, (long)target, 0) != 0) {
        fail("unmount the bind of the moved file");
    }
}

/**
 * One row, in a child of its own: bind /rk/f, by descriptor or by path,
 * onto a file or, with `dir`, a directory, while the file moves, and judge
 * the order and the bind. A file target must show the moved file; a
 * directory target must be refused with ENOTDIR.
 */
static void reloc_bind(int by_fd, int dir)
{
    static const char *const targets[] = {"/fat/d1/a/b/c/t", "/fat/d2/a/b/c/td", "/rk-t", "/rk-d"};
    const char *target = targets[by_fd * 2 + dir];
    const char *source = by_fd ? "/proc/self/fd/9" : "/rk/f";
    unsigned long was, held = 0;
    long slept, waited, reads, rc;
    int pids[3], before, len = 0;
    char line[256], states[4] = "---";

    fill_file("/rk/f", 'A', 4096);
    fill_file("/rk/g", 'G', 4096);
    was = stat_field("/rk/f", KSTAT_INO);
    reloc_start(by_fd, pids);
    if (by_fd) {
        states[0] = proc_state(pids[0]);
        states[1] = proc_state(pids[1]);
        states[2] = proc_state(pids[2]);
        held = fd_ino(9);
    }
    before = by_fd
                 ? held == was && seen->writing && seen->making && seen->grown < 0 && seen->made < 0
                 : stat_field("/rk/f", KSTAT_INO) == was;
    slept = by_fd ? voluntary_switches() : fat_reads();
    waited = now_us();
    rc = syscall5(SYS_MOUNT, (long)source, (long)target, 0, MS_BIND, 0);
    waited = now_us() - waited;
    reads = fat_reads();
    slept = (by_fd ? voluntary_switches() : reads) - slept;
    reloc_reap(pids[0], "the syncer's fsync");
    reloc_reap(pids[1], "the writer that moves the file");
    reloc_reap(pids[2], "the mkdir at the old offset");
    append(line, &len, by_fd ? "reloc: fd source: " : "reloc: path source: ", (long)was - 1);
    if (by_fd) {
        append(line, &len, ", descriptor at ", (long)held - 1);
        append_text(line, &len, ", waiters ");
        append_text(line, &len, states);
    }
    append(line, &len, " moved to ", (long)stat_field("/rk/f", KSTAT_INO) - 1);
    append(line, &len, ", directory made at ", (long)stat_field("/rk/d", KSTAT_INO) - 1);
    append(line, &len, by_fd ? ", switches " : ", fat reads ", slept);
    append(line, &len, ", bind waited ", waited / 1000);
    append(line, &len, dir ? " ms, directory target: " : " ms, file target: ", rc);
    puts(line);
    reloc_order_held(by_fd, before, slept, reads);
    if (stat_field("/rk/f", KSTAT_INO) == was || stat_field("/rk/d", KSTAT_INO) != was) {
        fail("the file did not move, or the directory did not take its old offset");
    }
    if (dir ? rc != -ENOTDIR : rc != 0) {
        fail(dir ? "a bind of the moved file onto a directory is not ENOTDIR"
                 : "a bind of the moved file onto a file failed");
    }
    if (!dir) {
        reloc_bound_file(target);
    }
}

/** Run `reloc_bind` in a child, so that a failed row still lets the next
 * run, then remove what it left: a bind on its target, its files and the
 * directory made at the file's old offset. A try that did not get the
 * row's order runs again, three tries in all. Returns 1 for a failed row. */
static int reloc_round(int by_fd, int dir)
{
    static const char *const targets[] = {"/fat/d1/a/b/c/t", "/fat/d2/a/b/c/td", "/rk-t", "/rk-d"};
    int pid, status = 0, tries;

    for (tries = 0; tries < 3; tries++) {
        seen->writing = seen->making = 0;
        seen->grown = seen->made = -1;
        pid = syscall0(SYS_FORK);
        if (pid < 0) {
            fail("fork for a relocation row");
        }
        if (pid == 0) {
            reloc_bind(by_fd, dir);
            sys_exit(0);
        }
        status = wait_status(pid);
        syscall2(SYS_UMOUNT2, (long)targets[by_fd * 2 + dir], MNT_DETACH);
        syscall1(SYS_UNLINK, (long)"/rk/f");
        syscall1(SYS_UNLINK, (long)"/rk/g");
        syscall1(SYS_UNLINK, (long)"/rk/big");
        syscall1(SYS_RMDIR, (long)"/rk/d");
        if (status != 2 << 8) {
            return status != 0;
        }
    }
    puts("MOUNTPROBE FAIL: no try of a relocation row got its order");
    return 1;
}

/** `mountprobe reloc`: mount the two volumes, run the four rows, by
 * descriptor and by path onto a file and onto a directory, take the mounts
 * and the mount points away, and print MOUNTPROBE OK when every row passed.
 * Does not return. */
static void rows_reloc(void)
{
    long at = probe_mmap(0, 4096, PROT_READ | PROT_WRITE, MAP_SHARED | MAP_ANONYMOUS, -1, 0);
    int failed;

    if (at < 0 && at > -4096) {
        fail("map the page the rows share");
    }
    seen = (struct reloc_seen *)at;
    if (syscall2(SYS_MKDIR, (long)"/rk", 0755) != 0 ||
        syscall2(SYS_MKDIR, (long)"/fat", 0755) != 0 ||
        syscall2(SYS_MKDIR, (long)"/rk-d", 0755) != 0) {
        fail("make the relocation rows' mount points");
    }
    fill_file("/rk-t", 'T', 1);
    if (syscall5(SYS_MOUNT, (long)"/dev/disk0p1", (long)"/rk", (long)"krffs", 0, 0) != 0 ||
        do_mount("/dev/disk2p1", "/fat", 0, 0) != 0) {
        fail("mount the relocation volume and the FAT volume");
    }
    failed = reloc_round(1, 0) + reloc_round(1, 1) + reloc_round(0, 0) + reloc_round(0, 1);
    if (syscall2(SYS_UMOUNT2, (long)"/fat", 0) != 0 || syscall2(SYS_UMOUNT2, (long)"/rk", 0) != 0) {
        fail("unmount the relocation volume and the FAT volume");
    }
    syscall1(SYS_UNLINK, (long)"/rk-t");
    syscall1(SYS_RMDIR, (long)"/rk-d");
    syscall1(SYS_RMDIR, (long)"/fat");
    syscall1(SYS_RMDIR, (long)"/rk");
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
    syscall1(SYS_RMDIR, (long)mnt2);
    syscall1(SYS_RMDIR, (long)mnt);
    puts("MOUNTPROBE OK");
    sys_exit(0);
}
