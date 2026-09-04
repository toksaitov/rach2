/**
 * krffsprobe: a mounted KRFFS volume, from a program.
 *
 * `krffsprobe read` mounts /dev/disk0p1 read-only, the volume
 * tests/system/suites/krffsmount.sh builds, on /k read-only and asks it what
 * a mounted volume owes: every byte of a file whose contents follow a rule,
 * the names in the root and in a subdirectory, a stat of each kind, a read
 * past the end, a name that is not there, and a write refused because the
 * mount is read-only. Then it unmounts.
 *
 * `krffsprobe write` mounts it for writing and does what a file system is
 * for: creates, writes, reads back, grows, truncates, renames, removes,
 * makes and removes a directory, changes a mode, syncs and unmounts.
 * `krffsprobe readback` is the next boot, which must find every one of those
 * where the write boot left it. `krffsprobe crash` writes and never unmounts,
 * for a machine the harness kills, and `krffsprobe writers` runs two writers
 * on one mount.
 *
 * `krffsprobe refuse` mounts the same device expecting the kernel to refuse
 * it: the suite gives it a volume broken on purpose, and the row is the
 * errno plus the kernel's own line on the console.
 *
 * Prints "KRFFSPROBE OK (mode)" or "KRFFSPROBE FAIL: reason". Freestanding;
 * int 0x80.
 */

typedef unsigned int size_t;

#define SYS_EXIT       1
#define SYS_READ       3
#define SYS_WRITE      4
#define SYS_OPEN       5
#define SYS_CLOSE      6
#define SYS_MOUNT      21
#define SYS_MKDIR      39
#define SYS_UMOUNT2    52
#define SYS_FORK       2
#define SYS_UNLINK     10
#define SYS_CHMOD      15
#define SYS_LSEEK      19
#define SYS_RENAME     38
#define SYS_RMDIR      40
#define SYS_FSYNC      118
#define SYS_FTRUNCATE  93
#define SYS_WAITPID    7
#define SYS_STAT64     195
#define SYS_GETDENTS64 220

#define O_RDONLY 0
#define O_WRONLY 1
#define O_RDWR   2
#define O_CREAT  0100
#define O_TRUNC  01000

#define MS_RDONLY 1

#define ENOENT 2
#define EINVAL 22
#define EROFS  30

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

static void fail(const char *why) __attribute__((noreturn));
static void fail(const char *why)
{
    sys3(SYS_WRITE, 1, (int)"KRFFSPROBE FAIL: ", 17);
    puts(why);
    sys_exit(1);
}

static void fail_with(const char *why, int n) __attribute__((noreturn));
static void fail_with(const char *why, int n)
{
    char digits[12];
    int i = 11;
    unsigned v = n < 0 ? (unsigned)-n : (unsigned)n;

    digits[i] = '\0';
    do {
        digits[--i] = (char)('0' + v % 10);
        v /= 10;
    } while (v);
    if (n < 0) {
        digits[--i] = '-';
    }
    sys3(SYS_WRITE, 1, (int)"KRFFSPROBE FAIL: ", 17);
    sys3(SYS_WRITE, 1, (int)why, (int)strlen(why));
    sys3(SYS_WRITE, 1, (int)" (got ", 6);
    sys3(SYS_WRITE, 1, (int)(digits + i), (int)strlen(digits + i));
    puts(")");
    sys_exit(1);
}

static int streq(const char *a, const char *b)
{
    size_t i;

    for (i = 0; a[i] && a[i] == b[i]; i++) {
    }
    return a[i] == b[i];
}

/* What the suite wrote into /pattern.bin: the byte at `i` is a rule, so a
   volume that reads back wrong says where. */
static unsigned char pattern_byte(unsigned i)
{
    return (unsigned char)((i * 31u + 7u) & 0xFFu);
}

#define PATTERN_BYTES 65536u

/* stat64's fields, at the offsets the kernel writes them. */
#define ST_MODE_OFF 16
#define ST_SIZE_OFF 44

static unsigned stat_mode(const char *path, unsigned char *st)
{
    if (sys2(SYS_STAT64, (int)path, (int)st) != 0) {
        fail("stat");
    }
    return *(unsigned *)(st + ST_MODE_OFF);
}

/**
 * `krffs` is among the types `/proc/filesystems` names.
 *
 * A type mount(2) accepts and does not list is one nobody can find without
 * reading the kernel.
 */
static void listed_rows(void)
{
    char buf[512];
    int fd = sys3(SYS_OPEN, (int)"/proc/filesystems", O_RDONLY, 0);
    int n;
    int i;

    if (fd < 0) {
        fail_with("opening /proc/filesystems", fd);
    }
    n = sys3(SYS_READ, fd, (int)buf, (int)sizeof(buf) - 1);
    sys1(SYS_CLOSE, fd);
    if (n <= 0) {
        fail_with("reading /proc/filesystems", n);
    }
    buf[n] = '\0';
    for (i = 0; i + 5 <= n; i++) {
        if (buf[i] == 'k' && buf[i + 1] == 'r' && buf[i + 2] == 'f' && buf[i + 3] == 'f' &&
            buf[i + 4] == 's') {
            return;
        }
    }
    fail("/proc/filesystems does not name krffs");
}

static void read_rows(void)
{
    unsigned char st[144];
    char buf[4096];
    unsigned at = 0;
    unsigned mode;
    int fd;
    int n;

    fd = sys3(SYS_OPEN, (int)"/k/pattern.bin", O_RDONLY, 0);
    if (fd < 0) {
        fail_with("opening /k/pattern.bin", fd);
    }
    while (at < PATTERN_BYTES) {
        int i;

        n = sys3(SYS_READ, fd, (int)buf, (int)sizeof(buf));
        if (n <= 0) {
            fail_with("reading /k/pattern.bin", n);
        }
        for (i = 0; i < n; i++) {
            if ((unsigned char)buf[i] != pattern_byte(at + (unsigned)i)) {
                fail_with("a byte of /k/pattern.bin", (int)(at + (unsigned)i));
            }
        }
        at += (unsigned)n;
    }
    if (at != PATTERN_BYTES) {
        fail_with("the length of /k/pattern.bin", (int)at);
    }
    n = sys3(SYS_READ, fd, (int)buf, (int)sizeof(buf));
    if (n != 0) {
        fail_with("a read past the end", n);
    }
    sys1(SYS_CLOSE, fd);

    fd = sys3(SYS_OPEN, (int)"/k/sub/inner.txt", O_RDONLY, 0);
    if (fd < 0) {
        fail_with("opening a file in a subdirectory", fd);
    }
    n = sys3(SYS_READ, fd, (int)buf, (int)sizeof(buf));
    if (n != 6 || buf[0] != 'i' || buf[5] != '\n') {
        fail_with("the bytes of /k/sub/inner.txt", n);
    }
    sys1(SYS_CLOSE, fd);

    if (sys3(SYS_OPEN, (int)"/k/nothing", O_RDONLY, 0) != -ENOENT) {
        fail("a name that is not there");
    }

    mode = stat_mode("/k", st);
    if ((mode & 0170000u) != 0040000u) {
        fail_with("the root of the mount is not a directory", (int)mode);
    }
    mode = stat_mode("/k/sub", st);
    if ((mode & 0170000u) != 0040000u) {
        fail_with("/k/sub is not a directory", (int)mode);
    }
    mode = stat_mode("/k/pattern.bin", st);
    if ((mode & 0170000u) != 0100000u) {
        fail_with("/k/pattern.bin is not a regular file", (int)mode);
    }
    if (*(unsigned *)(st + ST_SIZE_OFF) != PATTERN_BYTES) {
        fail_with("the size stat reports", (int)*(unsigned *)(st + ST_SIZE_OFF));
    }

    /* The names, through getdents. */
    {
        int saw_pattern = 0;
        int saw_sub = 0;
        int off = 0;

        fd = sys3(SYS_OPEN, (int)"/k", O_RDONLY, 0);
        if (fd < 0) {
            fail_with("opening the mount as a directory", fd);
        }
        n = sys3(SYS_GETDENTS64, fd, (int)buf, (int)sizeof(buf));
        if (n <= 0) {
            fail_with("getdents on the mount", n);
        }
        while (off + 19 < n) {
            unsigned short reclen = *(unsigned short *)(buf + off + 16);
            const char *name = buf + off + 19;

            if (streq(name, "pattern.bin")) {
                saw_pattern = 1;
            }
            if (streq(name, "sub")) {
                saw_sub = 1;
            }
            if (reclen == 0) {
                break;
            }
            off += reclen;
        }
        sys1(SYS_CLOSE, fd);
        if (!saw_pattern || !saw_sub) {
            fail("the listing of the mount");
        }
    }

    (void)0;
}

/**
 * `/proc/mounts` calls the volume read-only.
 *
 * A type with no write among its operations refuses a write whatever the
 * flags say, so this line is the only place a caller sees what it got. The
 * row that matters is the mount that did not ask for read-only: the kernel
 * gives it anyway, and says so here.
 */
static void mounts_say_ro(void)
{
    {
        char mounts[1024];
        int fd = sys3(SYS_OPEN, (int)"/proc/mounts", O_RDONLY, 0);
        int n;
        int i;
        int saw = 0;

        if (fd < 0) {
            fail_with("opening /proc/mounts", fd);
        }
        n = sys3(SYS_READ, fd, (int)mounts, (int)sizeof(mounts) - 1);
        sys1(SYS_CLOSE, fd);
        if (n <= 0) {
            fail_with("reading /proc/mounts", n);
        }
        mounts[n] = '\0';
        for (i = 0; i + 11 <= n; i++) {
            if (mounts[i] == 'k' && mounts[i + 1] == 'r' && mounts[i + 2] == 'f' &&
                mounts[i + 3] == 'f' && mounts[i + 4] == 's' && mounts[i + 5] == ' ' &&
                mounts[i + 6] == 'r' && mounts[i + 7] == 'o') {
                saw = 1;
            }
        }
        if (!saw) {
            fail("/proc/mounts does not call the volume read-only");
        }
    }
}

/* A read-only mount refuses every way of changing it. */
static void refusal_rows(void)
{
    int rc = sys3(SYS_OPEN, (int)"/k/pattern.bin", O_WRONLY, 0);

    if (rc != -EROFS) {
        fail_with("opening a file for writing on a read-only mount", rc);
    }
    rc = sys3(SYS_OPEN, (int)"/k/made", O_WRONLY | O_CREAT, 0644);
    if (rc != -EROFS) {
        fail_with("creating a file on a read-only mount", rc);
    }
}

/* The bytes a write boot leaves, and the boot after it reads. */
static void write_pattern_from(int fd, unsigned start, unsigned bytes)
{
    char buf[512];
    unsigned at = start;

    bytes += start;
    while (at < bytes) {
        unsigned i;
        unsigned take = bytes - at > sizeof(buf) ? (unsigned)sizeof(buf) : bytes - at;
        int n;

        for (i = 0; i < take; i++) {
            buf[i] = (char)pattern_byte(at + i);
        }
        n = sys3(SYS_WRITE, fd, (int)buf, (int)take);
        if (n != (int)take) {
            fail_with("writing the pattern", n);
        }
        at += take;
    }
}

/* The pattern from its start, which is what a fresh file gets. */
static void write_pattern(int fd, unsigned bytes)
{
    write_pattern_from(fd, 0, bytes);
}

static void read_pattern(const char *path, unsigned bytes)
{
    char buf[512];
    unsigned at = 0;
    int fd = sys3(SYS_OPEN, (int)path, O_RDONLY, 0);
    int n;

    if (fd < 0) {
        fail_with("opening what was written", fd);
    }
    while (at < bytes) {
        int i;

        n = sys3(SYS_READ, fd, (int)buf, (int)sizeof(buf));
        if (n <= 0) {
            fail_with("reading what was written", n);
        }
        for (i = 0; i < n; i++) {
            if ((unsigned char)buf[i] != pattern_byte(at + (unsigned)i)) {
                fail_with("a byte of what was written", (int)(at + (unsigned)i));
            }
        }
        at += (unsigned)n;
    }
    n = sys3(SYS_READ, fd, (int)buf, (int)sizeof(buf));
    if (n != 0) {
        fail_with("the length of what was written", n);
    }
    sys1(SYS_CLOSE, fd);
}

/* What a write boot leaves behind, which the boot after it checks. */
static void write_rows(void)
{
    unsigned char st[144];
    int fd;
    int rc;

    /* The rows run twice in one suite: once on a volume nobody has written
       and once on the volume a killed machine left, so they start by taking
       away what they are about to make. What is not there does not matter. */
    sys1(SYS_UNLINK, (int)"/k/made/inside");
    sys1(SYS_RMDIR, (int)"/k/made");
    sys1(SYS_UNLINK, (int)"/k/after.txt");
    sys1(SYS_UNLINK, (int)"/k/before.txt");
    sys1(SYS_UNLINK, (int)"/k/gone.txt");
    sys1(SYS_UNLINK, (int)"/k/grown.bin");
    sys1(SYS_RMDIR, (int)"/k/empty");

    fd = sys3(SYS_OPEN, (int)"/k/written.bin", O_WRONLY | O_CREAT | O_TRUNC, 0644);
    if (fd < 0) {
        fail_with("creating a file on the volume", fd);
    }
    write_pattern(fd, 20000);
    if (sys1(SYS_FSYNC, fd) != 0) {
        fail("fsync");
    }
    sys1(SYS_CLOSE, fd);
    read_pattern("/k/written.bin", 20000);

    /* A grow past what the node holds, then a truncate back. */
    fd = sys3(SYS_OPEN, (int)"/k/grown.bin", O_WRONLY | O_CREAT, 0644);
    if (fd < 0) {
        fail_with("creating the file to grow", fd);
    }
    write_pattern(fd, 100);
    write_pattern_from(fd, 100, 7900);
    sys1(SYS_CLOSE, fd);
    fd = sys3(SYS_OPEN, (int)"/k/grown.bin", O_WRONLY, 0);
    if (fd < 0) {
        fail_with("opening the file to truncate", fd);
    }
    rc = sys2(SYS_FTRUNCATE, fd, 4000);
    if (rc != 0) {
        fail_with("truncating", rc);
    }
    sys1(SYS_CLOSE, fd);
    if (sys2(SYS_STAT64, (int)"/k/grown.bin", (int)st) != 0 ||
        *(unsigned *)(st + ST_SIZE_OFF) != 4000) {
        fail("the size after a truncate");
    }

    /* A directory, a file in it, a rename, and a removal. */
    if (sys2(SYS_MKDIR, (int)"/k/made", 0755) != 0) {
        fail("mkdir on the volume");
    }
    fd = sys3(SYS_OPEN, (int)"/k/made/inside", O_WRONLY | O_CREAT, 0644);
    if (fd < 0) {
        fail_with("creating a file in a new directory", fd);
    }
    if (sys3(SYS_WRITE, fd, (int)"inside\n", 7) != 7) {
        fail("writing in a new directory");
    }
    sys1(SYS_CLOSE, fd);
    fd = sys3(SYS_OPEN, (int)"/k/gone.txt", O_WRONLY | O_CREAT, 0644);
    if (fd < 0) {
        fail_with("creating the file to remove", fd);
    }
    sys1(SYS_CLOSE, fd);
    if (sys1(SYS_UNLINK, (int)"/k/gone.txt") != 0) {
        fail("unlink on the volume");
    }
    fd = sys3(SYS_OPEN, (int)"/k/before.txt", O_WRONLY | O_CREAT, 0644);
    if (fd < 0) {
        fail_with("creating the file to rename", fd);
    }
    if (sys3(SYS_WRITE, fd, (int)"renamed\n", 8) != 8) {
        fail("writing the file to rename");
    }
    sys1(SYS_CLOSE, fd);
    if (sys2(SYS_RENAME, (int)"/k/before.txt", (int)"/k/after.txt") != 0) {
        fail("rename on the volume");
    }
    if (sys2(SYS_MKDIR, (int)"/k/empty", 0755) != 0) {
        fail("the directory to remove");
    }
    if (sys1(SYS_RMDIR, (int)"/k/empty") != 0) {
        fail("rmdir on the volume");
    }
    if (sys2(SYS_CHMOD, (int)"/k/written.bin", 0600) != 0) {
        fail("chmod on the volume");
    }
    rc = (int)stat_mode("/k/written.bin", st);
    if ((rc & 07777) != 0600) {
        fail_with("the mode after a chmod", rc & 07777);
    }
}

/* What the boot after a write boot must find. */
static void readback_rows(void)
{
    unsigned char st[144];
    char buf[32];
    int fd;

    read_pattern("/k/written.bin", 20000);
    if (sys2(SYS_STAT64, (int)"/k/grown.bin", (int)st) != 0 ||
        *(unsigned *)(st + ST_SIZE_OFF) != 4000) {
        fail("the truncated file's size after a reboot");
    }
    if ((stat_mode("/k/written.bin", st) & 07777) != 0600) {
        fail("the mode after a reboot");
    }
    fd = sys3(SYS_OPEN, (int)"/k/made/inside", O_RDONLY, 0);
    if (fd < 0) {
        fail_with("the file in the new directory after a reboot", fd);
    }
    if (sys3(SYS_READ, fd, (int)buf, (int)sizeof(buf)) != 7) {
        fail("the bytes in the new directory after a reboot");
    }
    sys1(SYS_CLOSE, fd);
    fd = sys3(SYS_OPEN, (int)"/k/after.txt", O_RDONLY, 0);
    if (fd < 0) {
        fail_with("the renamed file after a reboot", fd);
    }
    sys1(SYS_CLOSE, fd);
    if (sys3(SYS_OPEN, (int)"/k/before.txt", O_RDONLY, 0) != -ENOENT) {
        fail("the old name is still there after a reboot");
    }
    if (sys3(SYS_OPEN, (int)"/k/gone.txt", O_RDONLY, 0) != -ENOENT) {
        fail("the removed file is still there after a reboot");
    }
    if (sys3(SYS_OPEN, (int)"/k/empty", O_RDONLY, 0) != -ENOENT) {
        fail("the removed directory is still there after a reboot");
    }
}

__asm__(".global _start\n"
        "_start:\n"
        "   mov %esp, %eax\n"
        "   push %eax\n"
        "   call probe_main\n");

void probe_main(unsigned int *sp)
{
    int argc = (int)sp[0];
    char **argv = (char **)(sp + 1);
    const char *mode = argc > 1 ? argv[1] : "";
    int rc;

    sys2(SYS_MKDIR, (int)"/k", 0755);

    if (streq(mode, "refuse")) {
        rc = sys5(SYS_MOUNT, (int)"/dev/disk0p1", (int)"/k", (int)"krffs", MS_RDONLY, 0);
        if (rc == 0) {
            fail("the kernel mounted a volume it should have refused");
        }
        if (rc != -EINVAL) {
            fail_with("the errno of a refused mount", rc);
        }
        puts("KRFFSPROBE OK (refuse)");
        sys_exit(0);
    }

    if (streq(mode, "write") || streq(mode, "crash") || streq(mode, "writers")) {
        rc = sys5(SYS_MOUNT, (int)"/dev/disk0p1", (int)"/k", (int)"krffs", 0, 0);
        if (rc != 0) {
            fail_with("mounting the volume for writing", rc);
        }
        if (streq(mode, "crash")) {
            int fd = sys3(SYS_OPEN, (int)"/k/killed.bin", O_WRONLY | O_CREAT, 0644);

            if (fd < 0) {
                fail_with("creating the file to be killed", fd);
            }
            puts("KRFFSPROBE WRITING");
            /* The same bytes over and over: the machine is killed somewhere
               in here, and a volume that filled up would say nothing about
               what a stop leaves behind. */
            for (;;) {
                sys3(SYS_LSEEK, fd, 0, 0);
                write_pattern(fd, 4096);
                sys1(SYS_FSYNC, fd);
            }
        }
        if (streq(mode, "writers")) {
            int child = sys1(SYS_FORK, 0);
            const char *name = child == 0 ? "/k/child.bin" : "/k/parent.bin";
            int fd = sys3(SYS_OPEN, (int)name, O_WRONLY | O_CREAT | O_TRUNC, 0644);
            int i;

            if (fd < 0) {
                fail_with("a writer's file", fd);
            }
            for (i = 0; i < 8; i++) {
                write_pattern_from(fd, (unsigned)i * 2000, 2000);
            }
            sys1(SYS_FSYNC, fd);
            sys1(SYS_CLOSE, fd);
            if (child == 0) {
                sys_exit(0);
            }
            {
                int status = 0;

                if (sys3(SYS_WAITPID, child, (int)&status, 0) != child) {
                    fail("waiting for the other writer");
                }
                if (status != 0) {
                    fail_with("the other writer's status", status);
                }
            }
            read_pattern("/k/parent.bin", 16000);
            read_pattern("/k/child.bin", 16000);
        } else {
            write_rows();
        }
        rc = sys2(SYS_UMOUNT2, (int)"/k", 0);
        if (rc != 0) {
            fail_with("unmounting after writing", rc);
        }
        sys3(SYS_WRITE, 1, (int)"KRFFSPROBE OK (", 15);
        sys3(SYS_WRITE, 1, (int)mode, (int)strlen(mode));
        puts(")");
        sys_exit(0);
    }

    if (streq(mode, "readback")) {
        rc = sys5(SYS_MOUNT, (int)"/dev/disk0p1", (int)"/k", (int)"krffs", MS_RDONLY, 0);
        if (rc != 0) {
            fail_with("mounting the volume to read it back", rc);
        }
        readback_rows();
        rc = sys2(SYS_UMOUNT2, (int)"/k", 0);
        if (rc != 0) {
            fail_with("unmounting after reading back", rc);
        }
        puts("KRFFSPROBE OK (readback)");
        sys_exit(0);
    }

    if (!streq(mode, "read")) {
        fail("unknown mode");
    }

    listed_rows();
    rc = sys5(SYS_MOUNT, (int)"/dev/disk0p1", (int)"/k", (int)"krffs", MS_RDONLY, 0);
    if (rc != 0) {
        fail_with("mounting the volume", rc);
    }
    read_rows();
    mounts_say_ro();
    refusal_rows();
    rc = sys2(SYS_UMOUNT2, (int)"/k", 0);
    if (rc != 0) {
        fail_with("unmounting the volume", rc);
    }

    if (sys3(SYS_OPEN, (int)"/k/pattern.bin", O_RDONLY, 0) >= 0) {
        fail("a file of the volume is still there after the unmount");
    }
    puts("KRFFSPROBE OK (read)");
    sys_exit(0);
}
