/**
 * mount(2) and umount2(2) on a FAT32 partition, and the lifetime of a mount.
 *
 * The suite's disk has a FAT32 volume on /dev/disk0p2 holding `hello.txt`
 * and a copy of the `hello` program. The rows: a writable mount is placed
 * and taken away, then a read-only one is placed; the files on it stat, open,
 * read and list, and its statfs is vfat's; a program on a `noexec` mount is
 * refused with EACCES though its mode allows it; a mount is busy while any
 * description on it lives, in this process or a forked one, through `dup`,
 * `dup2` and `F_DUPFD`, and a close-on-exec descriptor is dropped by
 * `execve`; a working directory inside it is busy too; the refused flags
 * and targets answer EINVAL, a long target ENAMETOOLONG, a rename across
 * EXDEV, and the mount point refuses `rmdir`, `unlink` and `rename` with
 * EBUSY; an unprivileged caller gets EPERM; `..` climbs out of the mount
 * physically; and /proc/mounts and /proc/filesystems say what is mounted.
 *
 * Prints "MOUNTPROBE OK" or "MOUNTPROBE FAIL: reason". Freestanding; int
 * 0x80.
 */

typedef unsigned int size_t;

#define SYS_EXIT       1
#define SYS_FORK       2
#define SYS_READ       3
#define SYS_WRITE      4
#define SYS_OPEN       5
#define SYS_CLOSE      6
#define SYS_WAITPID    7
#define SYS_UNLINK     10
#define SYS_EXECVE     11
#define SYS_CHDIR      12
#define SYS_MOUNT      21
#define SYS_SETUID     213
#define SYS_RENAME     38
#define SYS_MKDIR      39
#define SYS_RMDIR      40
#define SYS_DUP        41
#define SYS_UMOUNT2    52
#define SYS_FCNTL      221
#define SYS_DUP2       63
#define SYS_STAT64     195
#define SYS_GETDENTS64 220
#define SYS_STATFS64   268
#define SYS_OPENAT     295
#define SYS_NANOSLEEP  162
#define SYS_KILL       37

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

#define EPERM        1
#define ENOENT       2
#define EACCES       13
#define EMFILE       24
#define EBADF        9
#define EBUSY        16
#define EXDEV        18
#define ENOTDIR      20
#define EINVAL       22
#define EROFS        30
#define ENAMETOOLONG 36

#define STATFS_MAGIC_VFAT 0x4d44

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

static int sys5(int n, int a, int b, int c, int d, int e)
{
    int r;
    __asm__ __volatile__("int $0x80"
                         : "=a"(r)
                         : "0"(n), "b"(a), "c"(b), "d"(c), "S"(d), "D"(e)
                         : "memory");
    return r;
}

static void sys_exit(int status) __attribute__((noreturn));
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

static int claim_mode;

static void fail(const char *why) __attribute__((noreturn));
static void fail(const char *why)
{
    if (claim_mode) {
        sys3(SYS_WRITE, 1, (int)"CLAIMPROBE FAIL: ", 17);
    } else {
        sys3(SYS_WRITE, 1, (int)"MOUNTPROBE FAIL: ", 17);
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
    return sys2(SYS_STAT64, (int)path, (int)st);
}

static int do_mount(const char *src, const char *target, unsigned flags, const char *data)
{
    return sys5(SYS_MOUNT, (int)src, (int)target, (int)"vfat", (int)flags, (int)data);
}

static void sleep_ms(int ms)
{
    int ts[2];

    ts[0] = ms / 1000;
    ts[1] = (ms % 1000) * 1000000;
    sys2(SYS_NANOSLEEP, (int)ts, 0);
}

static int wait_status(int pid)
{
    int status = 0;

    if (sys3(SYS_WAITPID, pid, (int)&status, 0) != pid) {
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
    int fd = sys3(SYS_OPEN, (int)path, O_RDONLY, 0);
    int n;

    if (fd < 0) {
        fail(path);
    }
    n = sys3(SYS_READ, fd, (int)buf, sizeof(buf) - 1);
    if (n < 0) {
        fail("reading a /proc file");
    }
    buf[n] = 0;
    sys1(SYS_CLOSE, fd);
}

static void rows_placing(void)
{
    int fd, d;

    /* F_DUPFD on the root, before any mount, so a failure later is the
       mount's and not the table's. */
    fd = sys3(SYS_OPEN, (int)"/hello", O_RDONLY, 0);
    d = sys3(SYS_FCNTL, fd, F_DUPFD, 10);
    if (fd < 0 || d != 10) {
        fail("F_DUPFD on the root");
    }
    sys1(SYS_CLOSE, d);
    sys1(SYS_CLOSE, fd);
    if (sys2(SYS_MKDIR, (int)MNT, 0755) != 0) {
        fail("mkdir " MNT);
    }
    if (do_mount(SRC, MNT, 0, 0) != 0) {
        fail("a writable mount is placed");
    }
    /* A directory descriptor on the mount follows a rename of its
       directory: the type rebuilds the path from the volume. */
    {
        int rfd, xfd;

        if (sys2(SYS_MKDIR, (int)MNT "/rdir", 0755) != 0) {
            fail("mkdir on the writable mount");
        }
        rfd = sys3(SYS_OPEN, (int)MNT "/rdir", O_RDONLY, 0);
        if (rfd < 0 || sys2(SYS_RENAME, (int)MNT "/rdir", (int)MNT "/rdir2") != 0) {
            fail("open and rename a directory on the mount");
        }
        xfd = sys4(SYS_OPENAT, rfd, (int)"x", O_WRONLY | O_CREAT, 0644);
        if (xfd < 0 || xstat(MNT "/rdir2/x") != 0) {
            fail("a directory descriptor follows its directory's rename");
        }
        sys1(SYS_CLOSE, xfd);
        sys1(SYS_CLOSE, rfd);
        if (sys1(SYS_UNLINK, (int)MNT "/rdir2/x") != 0) {
            fail("unlink on the writable mount");
        }
        if (sys1(SYS_RMDIR, (int)MNT "/rdir2") != 0) {
            fail("unlink and rmdir on the writable mount");
        }
    }
    if (sys2(SYS_UMOUNT2, (int)MNT, 0) != 0) {
        fail("the writable mount is taken away");
    }
    if (do_mount(SRC, MNT, MS_RDONLY | MS_REMOUNT, 0) != -EINVAL ||
        do_mount(SRC, MNT, MS_RDONLY | MS_BIND, 0) != -EINVAL ||
        do_mount(SRC, MNT, MS_RDONLY | MS_MOVE, 0) != -EINVAL) {
        fail("MS_REMOUNT, MS_BIND and MS_MOVE are EINVAL");
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
    if (sys2(SYS_MKDIR, (int)"/mnt2", 0755) != 0) {
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
    fd = sys3(SYS_OPEN, (int)MNT "/hello.txt", O_RDONLY, 0);
    if (fd < 0) {
        fail("open on the mount");
    }
    n = sys3(SYS_READ, fd, (int)buf, sizeof(buf));
    if (n != 27) {
        fail("the file's length");
    }
    buf[n] = 0;
    if (!contains((const char *)buf, "hello from the mount suite")) {
        fail("the file's bytes");
    }
    sys1(SYS_CLOSE, fd);
    if (sys3(SYS_OPEN, (int)MNT "/hello.txt", O_WRONLY, 0) != -EROFS) {
        fail("a write-open on the read-only mount is EROFS");
    }
    if (sys2(SYS_MKDIR, (int)MNT "/newdir", 0755) != -EROFS) {
        fail("mkdir on the read-only mount is EROFS");
    }
    if (sys1(SYS_UNLINK, (int)MNT "/hello.txt") != -EROFS) {
        fail("unlink on the read-only mount is EROFS");
    }
    fd = sys3(SYS_OPEN, (int)MNT, O_RDONLY, 0);
    if (fd < 0) {
        fail("open the mount root");
    }
    n = sys3(SYS_GETDENTS64, fd, (int)buf, sizeof(buf));
    if (n <= 0 || !dirents_contain(buf, n, "hello.txt")) {
        fail("getdents64 lists the mount");
    }
    {
        int f2 = sys4(SYS_OPENAT, fd, (int)"hello.txt", O_RDONLY, 0);

        if (f2 < 0 || sys3(SYS_READ, f2, (int)buf, 8) != 8) {
            fail("openat through the mount's directory descriptor");
        }
        sys1(SYS_CLOSE, f2);
    }
    sys1(SYS_CLOSE, fd);
    if (sys3(SYS_STATFS64, (int)MNT "/hello.txt", 84, (int)buf) != 0 ||
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
    if (sys2(SYS_RENAME, (int)MNT "/hello.txt", (int)"/hello-moved") != -EXDEV) {
        fail("rename across mounts is EXDEV");
    }
    if (sys1(SYS_RMDIR, (int)MNT) != -EBUSY) {
        fail("rmdir of the mount point is EBUSY");
    }
    if (sys1(SYS_UNLINK, (int)MNT) != -EBUSY) {
        fail("unlink of the mount point is EBUSY");
    }
    if (sys2(SYS_RENAME, (int)MNT, (int)"/mnt3") != -EBUSY) {
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

    fd = sys3(SYS_OPEN, (int)MNT "/hello.txt", O_RDONLY, 0);
    dfd = sys3(SYS_OPEN, (int)MNT, O_RDONLY, 0);
    if (fd < 0 || dfd < 0) {
        fail("open for the lifetime rows");
    }
    if (sys2(SYS_UMOUNT2, (int)MNT, 0) != -EBUSY) {
        fail("umount2 with a file open is EBUSY");
    }
    d1 = sys1(SYS_DUP, fd);
    if (d1 < 0) {
        fail("dup");
    }
    d2 = sys2(SYS_DUP2, fd, 14);
    if (d2 != 14) {
        fail("dup2");
    }
    d3 = sys3(SYS_FCNTL, fd, F_DUPFD, 10);
    if (d3 < 10) {
        static char why[32] = "F_DUPFD rc=";
        int v = -d3;

        why[11] = (char)('0' + (v / 10) % 10);
        why[12] = (char)('0' + v % 10);
        why[13] = 0;
        fail(why);
    }
    sys1(SYS_CLOSE, fd);
    sys1(SYS_CLOSE, dfd);
    if (sys2(SYS_UMOUNT2, (int)MNT, 0) != -EBUSY) {
        fail("umount2 with duplicates open is EBUSY");
    }
    sys1(SYS_CLOSE, d1);
    sys1(SYS_CLOSE, d2);
    if (sys2(SYS_UMOUNT2, (int)MNT, 0) != -EBUSY) {
        fail("umount2 with the last duplicate open is EBUSY");
    }
    /* A forked child inherits the description; it must close it in its own
       process before the mount is free. */
    pid = sys1(SYS_FORK, 0);
    if (pid < 0) {
        fail("fork");
    }
    if (pid == 0) {
        sleep_ms(300);
        sys1(SYS_CLOSE, d3);
        sys_exit(0);
    }
    sys1(SYS_CLOSE, d3);
    if (sys2(SYS_UMOUNT2, (int)MNT, 0) != -EBUSY) {
        fail("umount2 with a child's inherited descriptor is EBUSY");
    }
    wait_status(pid);
    if (sys2(SYS_UMOUNT2, (int)MNT, 0) != 0) {
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
       while the child still runs, which is what shows execve did it. */
    pid = sys1(SYS_FORK, 0);
    if (pid == 0) {
        char *av[2];

        av[0] = (char *)"/stdintest";
        av[1] = 0;
        fd = sys3(SYS_OPEN, (int)MNT "/hello.txt", O_RDONLY | O_CLOEXEC, 0);
        if (fd < 0) {
            sys_exit(7);
        }
        sys3(SYS_EXECVE, (int)"/stdintest", (int)av, (int)av + 4);
        sys_exit(8);
    }
    {
        int tries;

        for (tries = 0; tries < 300; tries++) {
            if (sys2(SYS_UMOUNT2, (int)MNT, 0) == 0) {
                break;
            }
            sleep_ms(10);
        }
        if (tries == 300) {
            fail("umount2 after the close-on-exec descriptor was dropped by execve");
        }
        if (sys2(SYS_KILL, pid, 9) != 0 || (wait_status(pid) & 0x7f) != 9) {
            fail("the exec child was not alive across the unmount");
        }
    }
    if (do_mount(SRC, MNT, MS_RDONLY, 0) != 0) {
        fail("the mount once more");
    }

    /* A working directory inside the mount holds it. */
    pid = sys1(SYS_FORK, 0);
    if (pid == 0) {
        if (sys1(SYS_CHDIR, (int)MNT) != 0) {
            sys_exit(9);
        }
        sleep_ms(300);
        sys_exit(0);
    }
    sleep_ms(100);
    if (sys2(SYS_UMOUNT2, (int)MNT, 0) != -EBUSY) {
        fail("umount2 with a working directory inside is EBUSY");
    }
    if (wait_status(pid) != 0) {
        fail("the chdir child");
    }
    if (sys2(SYS_UMOUNT2, (int)MNT, 0) != 0) {
        fail("umount2 after the working directory left");
    }
}

static void rows_flags(void)
{
    char *av[2];
    int pid;

    if (sys2(SYS_UMOUNT2, (int)MNT, 0) != -EINVAL) {
        fail("umount2 of a directory that is not a mount point is EINVAL");
    }
    if (do_mount(SRC, MNT, MS_RDONLY, 0) != 0) {
        fail("the mount for the flag rows");
    }
    if (sys2(SYS_UMOUNT2, (int)MNT, MNT_DETACH) != -EINVAL ||
        sys2(SYS_UMOUNT2, (int)MNT, MNT_FORCE) != -EINVAL ||
        sys2(SYS_UMOUNT2, (int)MNT, UMOUNT_NOFOLLOW) != -EINVAL) {
        fail("MNT_DETACH, MNT_FORCE and UMOUNT_NOFOLLOW are EINVAL");
    }
    if (sys2(SYS_UMOUNT2, (int)"/proc", 0) != -EBUSY ||
        sys2(SYS_UMOUNT2, (int)"/dev", 0) != -EBUSY || sys2(SYS_UMOUNT2, (int)"/", 0) != -EBUSY) {
        fail("the permanent mounts are EBUSY");
    }
    /* Unprivileged: mount and umount2 are root's. */
    pid = sys1(SYS_FORK, 0);
    if (pid == 0) {
        if (sys1(SYS_SETUID, 1000) != 0) {
            sys_exit(3);
        }
        if (sys2(SYS_UMOUNT2, (int)MNT, 0) != -EPERM) {
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
    if (sys2(SYS_UMOUNT2, (int)MNT, 0) != 0) {
        fail("umount2 before the noexec rows");
    }

    /* A noexec mount refuses a program its mode would allow: umask=000
       gives every file 0777. */
    if (do_mount(SRC, MNT, MS_RDONLY | MS_NOEXEC, "umask=000") != 0) {
        fail("the noexec mount");
    }
    av[0] = (char *)MNT "/hello";
    av[1] = 0;
    if (sys3(SYS_EXECVE, (int)MNT "/hello", (int)av, (int)av + 4) != -EACCES) {
        fail("execve on a noexec mount is EACCES");
    }
    if (sys2(SYS_UMOUNT2, (int)MNT, 0) != 0) {
        fail("umount2 after the noexec row");
    }
    /* And without noexec the same program runs from the mount. */
    if (do_mount(SRC, MNT, MS_RDONLY, "umask=000") != 0) {
        fail("the executable mount");
    }
    pid = sys1(SYS_FORK, 0);
    if (pid == 0) {
        int xr = sys3(SYS_EXECVE, (int)MNT "/hello", (int)av, (int)av + 4);

        sys_exit(-xr);
    }
    {
        int st = wait_status(pid);

        if ((st & 0xff00) != 0) {
            static char why[40] = "a program runs from the mount: errno ";
            int v = (st >> 8) & 0xff;

            why[37] = (char)('0' + (v / 10) % 10);
            why[38] = (char)('0' + v % 10);
            why[39] = 0;
            fail(why);
        }
    }
    if (sys2(SYS_UMOUNT2, (int)MNT, 0) != 0) {
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
        if (sys2(SYS_MKDIR, (int)longname, 0755) != 0) {
            fail("mkdir of the long target");
        }
        if (do_mount(SRC, longname, MS_RDONLY, 0) != -ENAMETOOLONG) {
            fail("a long target is ENAMETOOLONG");
        }
        sys1(SYS_RMDIR, (int)longname);
        /* The source is still free to mount: the refusal claimed nothing. */
        if (do_mount(SRC, "/mnt2", MS_RDONLY, 0) != 0 || sys2(SYS_UMOUNT2, (int)"/mnt2", 0) != 0) {
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

    if (sys2(SYS_MKDIR, (int)MNT, 0755) != 0) {
        fail("mkdir " MNT);
    }
    if (sys5(SYS_MOUNT, (int)"/dev/bootpart", (int)MNT, (int)"vfat", MS_RDONLY, 0) != 0) {
        fail("the boot partition mounts read-only");
    }
    if (sys3(SYS_OPEN, (int)"/dev/bootpart", O_WRONLY, 0) != -EBUSY) {
        fail("the raw writer on a mounted range is EBUSY");
    }
    fd = sys3(SYS_OPEN, (int)"/dev/bootpart", O_RDONLY, 0);
    if (fd < 0) {
        fail("a reader of the mounted range is allowed");
    }
    sys1(SYS_CLOSE, fd);
    if (sys2(SYS_UMOUNT2, (int)MNT, 0) != 0) {
        fail("umount2 of the boot partition");
    }
    fd = sys3(SYS_OPEN, (int)"/dev/bootpart", O_WRONLY, 0);
    if (fd < 0) {
        fail("the raw writer after the unmount");
    }
    if (sys5(SYS_MOUNT, (int)"/dev/bootpart", (int)MNT, (int)"vfat", MS_RDONLY, 0) != -EBUSY) {
        fail("a mount over the raw writer's range is EBUSY");
    }
    if (sys5(SYS_MOUNT, (int)"/dev/disk0p1", (int)MNT, (int)"vfat", MS_RDONLY, 0) != -EBUSY) {
        fail("the same range under its other name is EBUSY too");
    }
    sys1(SYS_CLOSE, fd);
    sys1(SYS_RMDIR, (int)MNT);
    sys3(SYS_WRITE, 1, (int)"CLAIMPROBE OK\n", 14);
    sys_exit(0);
}

/* The unmount record counts descriptions, not descriptors: one open and
   three duplicates, then a duplication refused past the limit, end in a
   record of one open and one release, read back from the kernel log. */
static void rows_record(void)
{
    static char log[16384 + 1];
    int fd, i, n;
    static const char want[] = "unmounted " MNT ": 1 opens, 1 releases";

    if (do_mount(SRC, MNT, MS_RDONLY, 0) != 0) {
        fail("the mount for the record row");
    }
    fd = sys3(SYS_OPEN, (int)MNT "/hello.txt", O_RDONLY, 0);
    if (fd < 0) {
        fail("open for the record row");
    }
    if (sys1(SYS_DUP, fd) < 0 || sys2(SYS_DUP2, fd, 13) != 13 ||
        sys3(SYS_FCNTL, fd, F_DUPFD, 3) < 3) {
        fail("the duplicates for the record row");
    }
    if (sys2(SYS_DUP2, fd, 1024) != -EBADF) {
        fail("dup2 past the limit is EBADF");
    }
    for (i = 3; i < 16; i++) {
        sys1(SYS_CLOSE, i);
    }
    if (sys2(SYS_UMOUNT2, (int)MNT, 0) != 0) {
        fail("umount2 for the record row");
    }
    fd = sys3(SYS_OPEN, (int)"/dev/bootlog", O_RDONLY, 0);
    if (fd < 0) {
        fail("open /dev/bootlog");
    }
    n = sys3(SYS_READ, fd, (int)log, (int)sizeof(log) - 1);
    sys1(SYS_CLOSE, fd);
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
    sys1(SYS_RMDIR, (int)"/mnt2");
    sys1(SYS_RMDIR, (int)MNT);
    puts("MOUNTPROBE OK");
    sys_exit(0);
}
