/**
 * mount(2) and umount2(2) on a FAT32 partition, and the lifetime of a mount.
 *
 * The suite's disk has a FAT32 volume on /dev/disk0p2 holding `hello.txt`
 * and a copy of the `hello` program. The rows: a writable mount is placed
 * and taken away, then a read-only one is placed; the files on it stat, open,
 * read and list, its statfs is vfat's, and a symlink onto a name that exists
 * there is EEXIST before the mount's EROFS; a program on a `noexec` mount is
 * refused with EACCES though its mode allows it; a mount is busy while any
 * description on it lives, in this process or a forked one, through `dup`,
 * `dup2` and `F_DUPFD`, and a close-on-exec descriptor is dropped by
 * `execve`; a working directory inside it is busy too; the refused flags
 * and targets answer EINVAL, a bind of a device ENOTDIR, a long target
 * ENAMETOOLONG, a rename across
 * EXDEV, and the mount point refuses `rmdir`, `unlink` and `rename` with
 * EBUSY; an unprivileged caller gets EPERM; `..` climbs out of the mount
 * physically; and /proc/mounts and /proc/filesystems say what is mounted.
 *
 * Prints "MOUNTPROBE OK" or "MOUNTPROBE FAIL: reason". Freestanding; int
 * 0x80.
 */

#include "probe.h"

#define O_CREAT   0100
#define O_RDONLY  0
#define O_WRONLY  1
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

static void sys_exit(int status) __attribute__((noreturn));
static void sys_exit(int status)
{
    syscall1(SYS_EXIT, status);
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
    syscall3(SYS_WRITE, 1, (long)s, (int)strlen(s));
    syscall3(SYS_WRITE, 1, (long)"\n", 1);
}

static int claim_mode;

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

/* Whether the getdents64 records in buf[0..n) name `name`: the records are
   binary, so they are walked by d_reclen rather than read as text. */
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

static int xstat(const char *path)
{
    char st[104];

    st[0] = 0;
    return syscall2(SYS_STAT, (long)path, (long)st);
}

static int do_mount(const char *src, const char *target, unsigned flags, const char *data)
{
    return syscall5(SYS_MOUNT, (long)src, (long)target, (long)"vfat", (int)flags, (long)data);
}

static void sleep_ms(int ms)
{
    int ts[2];

    ts[0] = ms / 1000;
    ts[1] = (ms % 1000) * 1000000;
    syscall2(SYS_NANOSLEEP, (long)ts, 0);
}

static int wait_status(int pid)
{
    int status = 0;

    if (syscall3(SYS_WAITPID, pid, (long)&status, 0) != pid) {
        fail("waitpid");
    }
    return status;
}

static unsigned char buf[4096];

#define SRC "/dev/disk0p2"
#define MNT "/mnt"

/* Read a whole /proc file into buf as text. */
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

static void rows_placing(void)
{
    int fd, d;

    /* F_DUPFD on the root, before any mount, so a failure later is the
       mount's and not the table's. */
    fd = syscall3(SYS_OPEN, (long)"/hello", O_RDONLY, 0);
    d = syscall3(SYS_FCNTL, fd, F_DUPFD, 10);
    if (fd < 0 || d != 10) {
        fail("F_DUPFD on the root");
    }
    syscall1(SYS_CLOSE, d);
    syscall1(SYS_CLOSE, fd);
    if (syscall2(SYS_MKDIR, (long)MNT, 0755) != 0) {
        fail("mkdir " MNT);
    }
    if (do_mount(SRC, MNT, 0, 0) != 0) {
        fail("a writable mount is placed");
    }
    /* A directory descriptor on the mount follows a rename of its
       directory: the type rebuilds the path from the volume. */
    {
        int rfd, xfd;

        if (syscall2(SYS_MKDIR, (long)MNT "/rdir", 0755) != 0) {
            fail("mkdir on the writable mount");
        }
        rfd = syscall3(SYS_OPEN, (long)MNT "/rdir", O_RDONLY, 0);
        if (rfd < 0 || syscall2(SYS_RENAME, (long)MNT "/rdir", (long)MNT "/rdir2") != 0) {
            fail("open and rename a directory on the mount");
        }
        xfd = syscall4(SYS_OPENAT, rfd, (long)"x", O_WRONLY | O_CREAT, 0644);
        if (xfd < 0 || xstat(MNT "/rdir2/x") != 0) {
            fail("a directory descriptor follows its directory's rename");
        }
        syscall1(SYS_CLOSE, xfd);
        syscall1(SYS_CLOSE, rfd);
        if (syscall1(SYS_UNLINK, (long)MNT "/rdir2/x") != 0) {
            fail("unlink on the writable mount");
        }
        if (syscall1(SYS_RMDIR, (long)MNT "/rdir2") != 0) {
            fail("unlink and rmdir on the writable mount");
        }
    }
    if (syscall2(SYS_UMOUNT2, (long)MNT, 0) != 0) {
        fail("the writable mount is taken away");
    }
    if (do_mount(SRC, MNT, MS_RDONLY | MS_REMOUNT, 0) != -EINVAL ||
        do_mount(SRC, MNT, MS_RDONLY | MS_BIND, 0) != -ENOTDIR ||
        do_mount(SRC, MNT, MS_RDONLY | MS_MOVE, 0) != -EINVAL) {
        fail("MS_REMOUNT of no mount and MS_MOVE are EINVAL, and MS_BIND of a device ENOTDIR");
    }
    if (do_mount(SRC, "/proc", MS_RDONLY, 0) != -EINVAL) {
        fail("a target on /proc is EINVAL");
    }
    if (do_mount(SRC, "/", MS_RDONLY, 0) != -EINVAL) {
        fail("the root as a target is EINVAL");
    }
    if (do_mount("/dev/null", MNT, MS_RDONLY, 0) != -EINVAL) {
        fail("a source that is not a partition window is EINVAL");
    }
    if (do_mount(SRC, MNT, MS_RDONLY, "nosuch=1") != -EINVAL) {
        fail("an unknown option is EINVAL");
    }
    if (do_mount(SRC, MNT, MS_RDONLY, 0) != 0) {
        fail("the read-only mount");
    }
    /* Another source, so the EBUSY can only be the target's. */
    if (do_mount("/dev/disk0p1", MNT, MS_RDONLY, 0) != -EBUSY) {
        fail("a second mount on the same target is EBUSY");
    }
    if (do_mount(SRC, "/mnt2", MS_RDONLY, 0) != -ENOENT) {
        fail("a missing target is ENOENT");
    }
    if (syscall2(SYS_MKDIR, (long)"/mnt2", 0755) != 0) {
        fail("mkdir /mnt2");
    }
    if (do_mount(SRC, "/mnt2", MS_RDONLY, 0) != -EBUSY) {
        fail("a second mount over a claimed range is EBUSY");
    }
    if (do_mount("/dev/disk0p1", "/mnt2", MS_RDONLY, 0) != -EINVAL) {
        fail("the KRFFS partition is not a vfat volume");
    }
}

static void rows_reading(void)
{
    int fd, n;

    if (xstat(MNT "/hello.txt") != 0) {
        fail("stat of a file on the mount");
    }
    fd = syscall3(SYS_OPEN, (long)MNT "/hello.txt", O_RDONLY, 0);
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
    if (syscall3(SYS_OPEN, (long)MNT "/hello.txt", O_WRONLY, 0) != -EROFS) {
        fail("a write-open on the read-only mount is EROFS");
    }
    if (syscall2(SYS_MKDIR, (long)MNT "/newdir", 0755) != -EROFS) {
        fail("mkdir on the read-only mount is EROFS");
    }
    if (syscall2(SYS_SYMLINK, (long)"x", (long)MNT "/hello.txt") != -EEXIST ||
        syscall2(SYS_SYMLINK, (long)"x", (long)MNT "/newlink") != -EROFS) {
        fail("symlink on the read-only mount is EEXIST for a name that exists, else EROFS");
    }
    if (syscall1(SYS_UNLINK, (long)MNT "/hello.txt") != -EROFS) {
        fail("unlink on the read-only mount is EROFS");
    }
    fd = syscall3(SYS_OPEN, (long)MNT, O_RDONLY, 0);
    if (fd < 0) {
        fail("open the mount root");
    }
    /* A buffer too small for one record answers EINVAL and gives the
       description back: a read that kept it would leave every later read of
       this file, through this descriptor or a copy of it, waiting for a lock
       nothing releases. */
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
    if (syscall3(SYS_STATFS64, (long)MNT "/hello.txt", 84, (long)buf) != 0 ||
        *(unsigned *)buf != STATFS_MAGIC_VFAT) {
        fail("statfs64 on the mount is vfat's");
    }
    if (xstat(MNT "/nosuch") != -ENOENT) {
        fail("a missing name on the mount is ENOENT");
    }
    if (xstat(MNT "/hello.txt/x") != -ENOTDIR) {
        fail("a file used as a directory on the mount is ENOTDIR");
    }
    if (xstat(MNT "/../mnt/hello.txt") != 0) {
        fail("dot-dot climbs out of the mount and back in");
    }
    if (xstat(MNT "/../etc") != 0) {
        fail("dot-dot at the mount root reaches the parent file system");
    }
    if (syscall2(SYS_RENAME, (long)MNT "/hello.txt", (long)"/hello-moved") != -EXDEV) {
        fail("rename across mounts is EXDEV");
    }
    if (syscall1(SYS_RMDIR, (long)MNT) != -EBUSY) {
        fail("rmdir of the mount point is EBUSY");
    }
    if (syscall1(SYS_UNLINK, (long)MNT) != -EBUSY) {
        fail("unlink of the mount point is EBUSY");
    }
    if (syscall2(SYS_RENAME, (long)MNT, (long)"/mnt3") != -EBUSY) {
        fail("rename of the mount point is EBUSY");
    }
}

static void rows_proc(void)
{
    read_text("/proc/mounts");
    if (!contains((const char *)buf, SRC " " MNT " vfat ro")) {
        fail("/proc/mounts names the mount");
    }
    read_text("/proc/filesystems");
    if (!contains((const char *)buf, "nodev\tproc") || !contains((const char *)buf, "\tvfat") ||
        !contains((const char *)buf, "\texfat")) {
        fail("/proc/filesystems names the types");
    }
}

/* Every way a description can outlive its opener keeps the mount busy. */
static void rows_lifetime(void)
{
    int fd, d1, d2, d3, dfd, pid;
    int opened[2];
    char byte = 0;

    fd = syscall3(SYS_OPEN, (long)MNT "/hello.txt", O_RDONLY, 0);
    dfd = syscall3(SYS_OPEN, (long)MNT, O_RDONLY, 0);
    if (fd < 0 || dfd < 0) {
        fail("open for the lifetime rows");
    }
    if (syscall2(SYS_UMOUNT2, (long)MNT, 0) != -EBUSY) {
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
    if (syscall2(SYS_UMOUNT2, (long)MNT, 0) != -EBUSY) {
        fail("umount2 with duplicates open is EBUSY");
    }
    syscall1(SYS_CLOSE, d1);
    syscall1(SYS_CLOSE, d2);
    if (syscall2(SYS_UMOUNT2, (long)MNT, 0) != -EBUSY) {
        fail("umount2 with the last duplicate open is EBUSY");
    }
    /* A forked child inherits the description; it must close it in its own
       process before the mount is free. */
    pid = syscall1(SYS_FORK, 0);
    if (pid < 0) {
        fail("fork");
    }
    if (pid == 0) {
        sleep_ms(300);
        syscall1(SYS_CLOSE, d3);
        sys_exit(0);
    }
    syscall1(SYS_CLOSE, d3);
    if (syscall2(SYS_UMOUNT2, (long)MNT, 0) != -EBUSY) {
        fail("umount2 with a child's inherited descriptor is EBUSY");
    }
    wait_status(pid);
    if (syscall2(SYS_UMOUNT2, (long)MNT, 0) != 0) {
        fail("umount2 once every description is closed");
    }
    if (xstat(MNT "/hello.txt") != -ENOENT) {
        fail("the mount is gone after umount2");
    }
    if (do_mount(SRC, MNT, MS_RDONLY, 0) != 0) {
        fail("the mount again after umount2");
    }

    /* A close-on-exec descriptor is dropped by execve: the child holds the
       only description and execs `stdintest`, which blocks on the console
       and stays alive; the mount is busy until the exec and free after it,
       while the child still runs, which is what shows execve did it. The
       parent waits for the child's open through a pipe before it tries the
       unmount, since a first try before the open would find the mount free
       and the row would then judge the scheduler and not execve. */
    opened[0] = -1; /* the kernel fills the pair through the cast address */
    opened[1] = -1;
    if (syscall1(SYS_PIPE, (long)opened) != 0) {
        fail("a pipe for the exec child");
    }
    pid = syscall1(SYS_FORK, 0);
    if (pid == 0) {
        char *av[2];

        av[0] = (char *)"/stdintest";
        av[1] = 0;
        fd = syscall3(SYS_OPEN, (long)MNT "/hello.txt", O_RDONLY | O_CLOEXEC, 0);
        if (fd < 0) {
            sys_exit(7);
        }
        syscall3(SYS_WRITE, opened[1], (long)"o", 1);
        syscall1(SYS_CLOSE, opened[1]);
        syscall1(SYS_CLOSE, opened[0]);
        syscall3(SYS_EXECVE, (long)"/stdintest", (long)av, (long)av + 4);
        sys_exit(8);
    }
    syscall1(SYS_CLOSE, opened[1]);
    if (syscall3(SYS_READ, opened[0], (long)&byte, 1) != 1) {
        fail("the exec child did not open the file");
    }
    syscall1(SYS_CLOSE, opened[0]);
    {
        int tries;

        for (tries = 0; tries < 300; tries++) {
            if (syscall2(SYS_UMOUNT2, (long)MNT, 0) == 0) {
                break;
            }
            sleep_ms(10);
        }
        if (tries == 300) {
            fail("umount2 after the close-on-exec descriptor was dropped by execve");
        }
        if (syscall2(SYS_KILL, pid, 9) != 0 || (wait_status(pid) & 0x7f) != 9) {
            fail("the exec child was not alive across the unmount");
        }
    }
    if (do_mount(SRC, MNT, MS_RDONLY, 0) != 0) {
        fail("the mount once more");
    }

    /* A working directory inside the mount holds it. */
    pid = syscall1(SYS_FORK, 0);
    if (pid == 0) {
        if (syscall1(SYS_CHDIR, (long)MNT) != 0) {
            sys_exit(9);
        }
        sleep_ms(300);
        sys_exit(0);
    }
    sleep_ms(100);
    if (syscall2(SYS_UMOUNT2, (long)MNT, 0) != -EBUSY) {
        fail("umount2 with a working directory inside is EBUSY");
    }
    if (wait_status(pid) != 0) {
        fail("the chdir child");
    }
    if (syscall2(SYS_UMOUNT2, (long)MNT, 0) != 0) {
        fail("umount2 after the working directory left");
    }
}

static void rows_flags(void)
{
    char *av[2];
    int pid;

    if (syscall2(SYS_UMOUNT2, (long)MNT, 0) != -EINVAL) {
        fail("umount2 of a directory that is not a mount point is EINVAL");
    }
    if (do_mount(SRC, MNT, MS_RDONLY, 0) != 0) {
        fail("the mount for the flag rows");
    }
    if (syscall2(SYS_UMOUNT2, (long)MNT, MNT_DETACH) != -EINVAL ||
        syscall2(SYS_UMOUNT2, (long)MNT, MNT_FORCE) != -EINVAL ||
        syscall2(SYS_UMOUNT2, (long)MNT, UMOUNT_NOFOLLOW) != -EINVAL) {
        fail("MNT_DETACH, MNT_FORCE and UMOUNT_NOFOLLOW are EINVAL");
    }
    if (syscall2(SYS_UMOUNT2, (long)"/proc", 0) != -EBUSY ||
        syscall2(SYS_UMOUNT2, (long)"/dev", 0) != -EBUSY ||
        syscall2(SYS_UMOUNT2, (long)"/", 0) != -EBUSY) {
        fail("the permanent mounts are EBUSY");
    }
    /* Unprivileged: mount and umount2 are root's. */
    pid = syscall1(SYS_FORK, 0);
    if (pid == 0) {
        if (syscall1(SYS_SETUID, 1000) != 0) {
            sys_exit(3);
        }
        if (syscall2(SYS_UMOUNT2, (long)MNT, 0) != -EPERM) {
            sys_exit(4);
        }
        if (do_mount(SRC, "/mnt2", MS_RDONLY, 0) != -EPERM) {
            sys_exit(5);
        }
        sys_exit(0);
    }
    if (wait_status(pid) != 0) {
        fail("an unprivileged caller is refused with EPERM");
    }
    if (syscall2(SYS_UMOUNT2, (long)MNT, 0) != 0) {
        fail("umount2 before the noexec rows");
    }

    /* A noexec mount refuses a program its mode would allow: umask=000
       gives every file 0777. */
    if (do_mount(SRC, MNT, MS_RDONLY | MS_NOEXEC, "umask=000") != 0) {
        fail("the noexec mount");
    }
    av[0] = (char *)MNT "/hello";
    av[1] = 0;
    if (syscall3(SYS_EXECVE, (long)MNT "/hello", (long)av, (long)av + 4) != -EACCES) {
        fail("execve on a noexec mount is EACCES");
    }
    if (syscall2(SYS_UMOUNT2, (long)MNT, 0) != 0) {
        fail("umount2 after the noexec row");
    }
    /* And without noexec the same program runs from the mount. */
    if (do_mount(SRC, MNT, MS_RDONLY, "umask=000") != 0) {
        fail("the executable mount");
    }
    pid = syscall1(SYS_FORK, 0);
    if (pid == 0) {
        int xr = syscall3(SYS_EXECVE, (long)MNT "/hello", (long)av, (long)av + 4);

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
    if (syscall2(SYS_UMOUNT2, (long)MNT, 0) != 0) {
        fail("umount2 after running from the mount");
    }

    /* A target whose path does not fit the record is refused before
       anything is claimed. */
    {
        static char longname[80];
        int i;

        longname[0] = '/';
        for (i = 1; i < 70; i++) {
            longname[i] = 'n';
        }
        longname[70] = 0;
        if (syscall2(SYS_MKDIR, (long)longname, 0755) != 0) {
            fail("mkdir of the long target");
        }
        if (do_mount(SRC, longname, MS_RDONLY, 0) != -ENAMETOOLONG) {
            fail("a long target is ENAMETOOLONG");
        }
        syscall1(SYS_RMDIR, (long)longname);
        /* The source is still free to mount: the refusal claimed nothing. */
        if (do_mount(SRC, "/mnt2", MS_RDONLY, 0) != 0 ||
            syscall2(SYS_UMOUNT2, (long)"/mnt2", 0) != 0) {
            fail("the source mounts after the refused long target");
        }
    }
}

void probe_main(unsigned int *sp);

__asm__(".global _start\n"
        "_start:\n"
        "   mov %esp, %eax\n"
        "   push %eax\n"
        "   call probe_main\n");

/* `mountprobe claim`, on a disk whose entry 0 is the FAT volume: the boot
   partition mounted read-only refuses the raw writer with EBUSY, which is
   the bootctl conflict, and takes it again once the mount is gone; a
   mount over the writer's range, by either of its names, is EBUSY. */
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

/* The unmount record counts descriptions, not descriptors: one open and
   three duplicates, then a duplication that fails on a table filled to a
   limit of sixteen and one refused past the limit, end in a record of one
   open and one release, read back from the kernel log. */
static void rows_record(void)
{
    static char log[16384 + 1];
    unsigned rl[4];
    int fd, d, filler[16], nfill = 0, i, n;
    static const char want[] = "unmounted " MNT ": 1 opens, 1 releases";

    if (do_mount(SRC, MNT, MS_RDONLY, 0) != 0) {
        fail("the mount for the record row");
    }
    fd = syscall3(SYS_OPEN, (long)MNT "/hello.txt", O_RDONLY, 0);
    if (fd < 0) {
        fail("open for the record row");
    }
    if (syscall1(SYS_DUP, fd) < 0 || syscall2(SYS_DUP2, fd, 13) != 13 ||
        syscall3(SYS_FCNTL, fd, F_DUPFD, 3) < 3) {
        fail("the duplicates for the record row");
    }
    /* The descriptor limit lowered to sixteen, the table filled to it, and a
       duplication that must fail there without taking a reference. */
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
    if (syscall2(SYS_UMOUNT2, (long)MNT, 0) != 0) {
        fail("umount2 for the record row");
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
    for (i = 0; i + (int)sizeof(want) - 1 <= n; i++) {
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

void probe_main(unsigned int *sp)
{
    int argc = (int)sp[0];
    char **argv = (char **)(sp + 1);

    if (argc >= 2 && argv[1][0] == 'c') {
        claim_mode = 1;
        rows_claim();
    }
    rows_placing();
    rows_reading();
    rows_proc();
    rows_lifetime();
    rows_flags();
    rows_record();
    syscall1(SYS_RMDIR, (long)"/mnt2");
    syscall1(SYS_RMDIR, (long)MNT);
    puts("MOUNTPROBE OK");
    sys_exit(0);
}
