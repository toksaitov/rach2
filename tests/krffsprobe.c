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
 * direct system calls (probe.h).
 */

#include "probe.h"

#define O_RDONLY 0
#define O_WRONLY 1
#define O_RDWR   2
#define O_CREAT  0100
#define O_TRUNC  01000

#define MS_RDONLY 1

#define ENOENT 2
#define ENOMEM 12
#define EINVAL 22
#define EBUSY  16
#define EROFS  30

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

static void fail(const char *why) __attribute__((noreturn));
static void fail(const char *why)
{
    syscall3(SYS_WRITE, 1, (long)"KRFFSPROBE FAIL: ", 17);
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
    syscall3(SYS_WRITE, 1, (long)"KRFFSPROBE FAIL: ", 17);
    syscall3(SYS_WRITE, 1, (long)why, (int)strlen(why));
    syscall3(SYS_WRITE, 1, (long)" (got ", 6);
    syscall3(SYS_WRITE, 1, (long)(digits + i), (int)strlen(digits + i));
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
    if (syscall2(SYS_STAT, (long)path, (long)st) != 0) {
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
    int fd = syscall3(SYS_OPEN, (long)"/proc/filesystems", O_RDONLY, 0);
    int n;
    int i;

    if (fd < 0) {
        fail_with("opening /proc/filesystems", fd);
    }
    n = syscall3(SYS_READ, fd, (long)buf, (int)sizeof(buf) - 1);
    syscall1(SYS_CLOSE, fd);
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

    fd = syscall3(SYS_OPEN, (long)"/k/pattern.bin", O_RDONLY, 0);
    if (fd < 0) {
        fail_with("opening /k/pattern.bin", fd);
    }
    while (at < PATTERN_BYTES) {
        int i;

        n = syscall3(SYS_READ, fd, (long)buf, (int)sizeof(buf));
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
    n = syscall3(SYS_READ, fd, (long)buf, (int)sizeof(buf));
    if (n != 0) {
        fail_with("a read past the end", n);
    }
    syscall1(SYS_CLOSE, fd);

    fd = syscall3(SYS_OPEN, (long)"/k/sub/inner.txt", O_RDONLY, 0);
    if (fd < 0) {
        fail_with("opening a file in a subdirectory", fd);
    }
    n = syscall3(SYS_READ, fd, (long)buf, (int)sizeof(buf));
    if (n != 6 || buf[0] != 'i' || buf[5] != '\n') {
        fail_with("the bytes of /k/sub/inner.txt", n);
    }
    syscall1(SYS_CLOSE, fd);

    if (syscall3(SYS_OPEN, (long)"/k/nothing", O_RDONLY, 0) != -ENOENT) {
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

        fd = syscall3(SYS_OPEN, (long)"/k", O_RDONLY, 0);
        if (fd < 0) {
            fail_with("opening the mount as a directory", fd);
        }
        n = syscall3(SYS_GETDENTS64, fd, (long)buf, (int)sizeof(buf));
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
        syscall1(SYS_CLOSE, fd);
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
        int fd = syscall3(SYS_OPEN, (long)"/proc/mounts", O_RDONLY, 0);
        int n;
        int i;
        int saw = 0;

        if (fd < 0) {
            fail_with("opening /proc/mounts", fd);
        }
        n = syscall3(SYS_READ, fd, (long)mounts, (int)sizeof(mounts) - 1);
        syscall1(SYS_CLOSE, fd);
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
    int rc = syscall3(SYS_OPEN, (long)"/k/pattern.bin", O_WRONLY, 0);

    if (rc != -EROFS) {
        fail_with("opening a file for writing on a read-only mount", rc);
    }
    rc = syscall3(SYS_OPEN, (long)"/k/made", O_WRONLY | O_CREAT, 0644);
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
        n = syscall3(SYS_WRITE, fd, (long)buf, (int)take);
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
    int fd = syscall3(SYS_OPEN, (long)path, O_RDONLY, 0);
    int n;

    if (fd < 0) {
        fail_with("opening what was written", fd);
    }
    while (at < bytes) {
        int i;

        n = syscall3(SYS_READ, fd, (long)buf, (int)sizeof(buf));
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
    n = syscall3(SYS_READ, fd, (long)buf, (int)sizeof(buf));
    if (n != 0) {
        fail_with("the length of what was written", n);
    }
    syscall1(SYS_CLOSE, fd);
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
    syscall1(SYS_UNLINK, (long)"/k/made/inside");
    syscall1(SYS_RMDIR, (long)"/k/made");
    syscall1(SYS_UNLINK, (long)"/k/after.txt");
    syscall1(SYS_UNLINK, (long)"/k/before.txt");
    syscall1(SYS_UNLINK, (long)"/k/gone.txt");
    syscall1(SYS_UNLINK, (long)"/k/grown.bin");
    syscall1(SYS_UNLINK, (long)"/k/held.bin");
    syscall1(SYS_UNLINK, (long)"/k/moved.bin");
    syscall1(SYS_UNLINK, (long)"/k/blocker.bin");
    syscall1(SYS_RMDIR, (long)"/k/empty");
    syscall1(SYS_UNLINK, (long)"/k/secret.txt");
    syscall1(SYS_RMDIR, (long)"/k/private");
    syscall1(SYS_UNLINK, (long)"/k/hold/keep.txt");
    syscall1(SYS_UNLINK, (long)"/k/hold/gone.bin");
    syscall1(SYS_RMDIR, (long)"/k/hold");
    syscall1(SYS_RMDIR, (long)"/k/busydir");
    syscall1(SYS_UNLINK, (long)"/k/over.txt");
    syscall1(SYS_UNLINK, (long)"/k/spare.txt");
    syscall1(SYS_UNLINK, (long)"/k/cut.bin");
    syscall1(SYS_UNLINK, (long)"/k/cutblock.bin");
    syscall1(SYS_UNLINK, (long)"/k/over.bin");

    fd = syscall3(SYS_OPEN, (long)"/k/written.bin", O_WRONLY | O_CREAT | O_TRUNC, 0644);
    if (fd < 0) {
        fail_with("creating a file on the volume", fd);
    }
    write_pattern(fd, 20000);
    if (syscall1(SYS_FSYNC, fd) != 0) {
        fail("fsync");
    }
    syscall1(SYS_CLOSE, fd);
    read_pattern("/k/written.bin", 20000);

    /* A grow past what the node holds, then a truncate back. */
    fd = syscall3(SYS_OPEN, (long)"/k/grown.bin", O_WRONLY | O_CREAT, 0644);
    if (fd < 0) {
        fail_with("creating the file to grow", fd);
    }
    write_pattern(fd, 100);
    write_pattern_from(fd, 100, 7900);
    syscall1(SYS_CLOSE, fd);
    fd = syscall3(SYS_OPEN, (long)"/k/grown.bin", O_WRONLY, 0);
    if (fd < 0) {
        fail_with("opening the file to truncate", fd);
    }
    rc = syscall2(SYS_FTRUNCATE, fd, 4000);
    if (rc != 0) {
        fail_with("truncating", rc);
    }
    syscall1(SYS_CLOSE, fd);
    if (syscall2(SYS_STAT, (long)"/k/grown.bin", (long)st) != 0 ||
        *(unsigned *)(st + ST_SIZE_OFF) != 4000) {
        fail("the size after a truncate");
    }

    /* A directory, a file in it, a rename, and a removal. */
    if (syscall2(SYS_MKDIR, (long)"/k/made", 0755) != 0) {
        fail("mkdir on the volume");
    }
    fd = syscall3(SYS_OPEN, (long)"/k/made/inside", O_WRONLY | O_CREAT, 0644);
    if (fd < 0) {
        fail_with("creating a file in a new directory", fd);
    }
    if (syscall3(SYS_WRITE, fd, (long)"inside\n", 7) != 7) {
        fail("writing in a new directory");
    }
    syscall1(SYS_CLOSE, fd);
    fd = syscall3(SYS_OPEN, (long)"/k/gone.txt", O_WRONLY | O_CREAT, 0644);
    if (fd < 0) {
        fail_with("creating the file to remove", fd);
    }
    syscall1(SYS_CLOSE, fd);
    if (syscall1(SYS_UNLINK, (long)"/k/gone.txt") != 0) {
        fail("unlink on the volume");
    }
    fd = syscall3(SYS_OPEN, (long)"/k/before.txt", O_WRONLY | O_CREAT, 0644);
    if (fd < 0) {
        fail_with("creating the file to rename", fd);
    }
    if (syscall3(SYS_WRITE, fd, (long)"renamed\n", 8) != 8) {
        fail("writing the file to rename");
    }
    syscall1(SYS_CLOSE, fd);
    if (syscall2(SYS_RENAME, (long)"/k/before.txt", (long)"/k/after.txt") != 0) {
        fail("rename on the volume");
    }
    if (syscall2(SYS_MKDIR, (long)"/k/empty", 0755) != 0) {
        fail("the directory to remove");
    }
    if (syscall1(SYS_RMDIR, (long)"/k/empty") != 0) {
        fail("rmdir on the volume");
    }
    /* A name taken away while the file is open: the reader keeps the file,
       the name is gone at once, and the space comes back at the close. */
    fd = syscall3(SYS_OPEN, (long)"/k/held.bin", O_WRONLY | O_CREAT | O_TRUNC, 0644);
    if (fd < 0) {
        fail_with("creating the file to hold open", fd);
    }
    write_pattern(fd, 200);
    syscall1(SYS_CLOSE, fd);
    fd = syscall3(SYS_OPEN, (long)"/k/held.bin", O_RDONLY, 0);
    if (fd < 0) {
        fail_with("opening the file to hold", fd);
    }
    if (syscall1(SYS_UNLINK, (long)"/k/held.bin") != 0) {
        fail("unlinking a file that is open");
    }
    if (syscall3(SYS_OPEN, (long)"/k/held.bin", O_RDONLY, 0) != -ENOENT) {
        fail("the name of an unlinked file is still there");
    }
    {
        char held[200];
        int n = syscall3(SYS_READ, fd, (long)held, (int)sizeof(held));
        int i;

        if (n != 200) {
            fail_with("reading a file whose name was taken away", n);
        }
        for (i = 0; i < n; i++) {
            if ((unsigned char)held[i] != pattern_byte((unsigned)i)) {
                fail_with("a byte of the file whose name was taken away", i);
            }
        }
    }
    syscall1(SYS_CLOSE, fd);

    /* A directory somebody has open is not taken away under them, and a
       rename over an open file leaves that file for its holder the way an
       unlink does. */
    {
        int dirfd;

        if (syscall2(SYS_MKDIR, (long)"/k/busydir", 0755) != 0) {
            fail("the directory to hold open");
        }
        dirfd = syscall3(SYS_OPEN, (long)"/k/busydir", O_RDONLY, 0);
        if (dirfd < 0) {
            fail_with("opening a directory of the volume", dirfd);
        }
        if (syscall1(SYS_RMDIR, (long)"/k/busydir") != -EBUSY) {
            fail("a directory somebody had open was removed");
        }
        syscall1(SYS_CLOSE, dirfd);
        if (syscall1(SYS_RMDIR, (long)"/k/busydir") != 0) {
            fail("removing the directory after its holder let go");
        }
    }
    fd = syscall3(SYS_OPEN, (long)"/k/over.txt", O_WRONLY | O_CREAT | O_TRUNC, 0644);
    if (fd < 0) {
        fail_with("creating the file to be renamed over", fd);
    }
    if (syscall3(SYS_WRITE, fd, (long)"under\n", 6) != 6) {
        fail("writing the file to be renamed over");
    }
    syscall1(SYS_CLOSE, fd);
    fd = syscall3(SYS_OPEN, (long)"/k/over.txt", O_RDONLY, 0);
    if (fd < 0) {
        fail_with("holding the file to be renamed over", fd);
    }
    {
        int spare = syscall3(SYS_OPEN, (long)"/k/spare.txt", O_WRONLY | O_CREAT | O_TRUNC, 0644);
        char kept[8];
        int n;

        if (spare < 0) {
            fail_with("the file to rename over it", spare);
        }
        if (syscall3(SYS_WRITE, spare, (long)"spare\n", 6) != 6) {
            fail("writing the file to rename over it");
        }
        syscall1(SYS_CLOSE, spare);
        if (syscall2(SYS_RENAME, (long)"/k/spare.txt", (long)"/k/over.txt") != 0) {
            fail("renaming over a file somebody has open");
        }
        n = syscall3(SYS_READ, fd, (long)kept, (int)sizeof(kept));
        if (n != 6 || kept[0] != 'u') {
            fail_with("the bytes of a file renamed over while it was open", n);
        }
        syscall1(SYS_CLOSE, fd);
        fd = syscall3(SYS_OPEN, (long)"/k/over.txt", O_RDONLY, 0);
        if (fd < 0) {
            fail_with("the file that took the name", fd);
        }
        n = syscall3(SYS_READ, fd, (long)kept, (int)sizeof(kept));
        if (n != 6 || kept[0] != 's') {
            fail_with("the bytes of the file that took the name", n);
        }
        syscall1(SYS_CLOSE, fd);
    }

    /* A truncation of a file whose name is gone. It has no path, and the one
       its parent and empty name spell reaches the directory it was in; the
       truncation must reach the file itself and leave that directory and
       everything else in it exactly as they were. */
    if (syscall2(SYS_MKDIR, (long)"/k/hold", 0755) != 0) {
        fail("the directory for the truncation of a nameless file");
    }
    fd = syscall3(SYS_OPEN, (long)"/k/hold/keep.txt", O_WRONLY | O_CREAT | O_TRUNC, 0644);
    if (fd < 0) {
        fail_with("the file that must survive", fd);
    }
    if (syscall3(SYS_WRITE, fd, (long)"keep\n", 5) != 5) {
        fail("writing the file that must survive");
    }
    syscall1(SYS_CLOSE, fd);
    fd = syscall3(SYS_OPEN, (long)"/k/hold/gone.bin", O_RDWR | O_CREAT | O_TRUNC, 0644);
    if (fd < 0) {
        fail_with("the file to truncate after its name goes", fd);
    }
    write_pattern(fd, 300);
    if (syscall1(SYS_UNLINK, (long)"/k/hold/gone.bin") != 0) {
        fail("unlinking the file to truncate");
    }
    syscall3(SYS_FTRUNCATE, fd, 0, 0);
    syscall1(SYS_CLOSE, fd);
    if (syscall2(SYS_STAT, (long)"/k/hold", (long)st) != 0) {
        fail("the directory of a nameless file that was truncated");
    }
    {
        char keep[8];
        int n;

        fd = syscall3(SYS_OPEN, (long)"/k/hold/keep.txt", O_RDONLY, 0);
        if (fd < 0) {
            fail_with("the file beside the nameless one that was truncated", fd);
        }
        n = syscall3(SYS_READ, fd, (long)keep, (int)sizeof(keep));
        if (n != 5 || keep[0] != 'k') {
            fail_with("the bytes beside the nameless file that was truncated", n);
        }
        syscall1(SYS_CLOSE, fd);
    }

    /* A descriptor open across a grow that moves the file: the write path
       repoints it, and the same descriptor reads what it wrote. */
    fd = syscall3(SYS_OPEN, (long)"/k/moved.bin", O_RDWR | O_CREAT | O_TRUNC, 0644);
    if (fd < 0) {
        fail_with("creating the file to move", fd);
    }
    write_pattern(fd, 64);
    {
        int blocker =
            syscall3(SYS_OPEN, (long)"/k/blocker.bin", O_WRONLY | O_CREAT | O_TRUNC, 0644);

        if (blocker < 0) {
            fail_with("creating the file that boxes it in", blocker);
        }
        write_pattern(blocker, 64);
        syscall1(SYS_CLOSE, blocker);
    }
    write_pattern_from(fd, 64, 6000);
    if (syscall3(SYS_LSEEK, fd, 0, 0) != 0) {
        fail("seeking back through a descriptor whose file moved");
    }
    {
        char moved[512];
        int n = syscall3(SYS_READ, fd, (long)moved, (int)sizeof(moved));
        int i;

        if (n != (int)sizeof(moved)) {
            fail_with("reading through a descriptor whose file moved", n);
        }
        for (i = 0; i < n; i++) {
            if ((unsigned char)moved[i] != pattern_byte((unsigned)i)) {
                fail_with("a byte read through a descriptor whose file moved", i);
            }
        }
    }
    syscall1(SYS_CLOSE, fd);

    /* A grow through ftruncate, which is the other way a file moves, with two
       descriptors on it: both must follow the file, the bytes it had must
       still be there, and the length it gained must read as zeroes. */
    {
        int one = syscall3(SYS_OPEN, (long)"/k/cut.bin", O_RDWR | O_CREAT | O_TRUNC, 0644);
        int two;
        int blocker;
        char got[512];
        int n;
        int i;

        if (one < 0) {
            fail_with("creating the file to grow by truncation", one);
        }
        write_pattern(one, 256);
        two = syscall3(SYS_OPEN, (long)"/k/cut.bin", O_RDONLY, 0);
        if (two < 0) {
            fail_with("opening it a second time", two);
        }
        blocker = syscall3(SYS_OPEN, (long)"/k/cutblock.bin", O_WRONLY | O_CREAT | O_TRUNC, 0644);
        if (blocker < 0) {
            fail_with("creating the file that boxes the truncation in", blocker);
        }
        write_pattern(blocker, 64);
        syscall1(SYS_CLOSE, blocker);
        if (syscall3(SYS_FTRUNCATE, one, 8192, 0) != 0) {
            fail("growing a boxed-in file through ftruncate");
        }
        if (syscall2(SYS_STAT, (long)"/k/cut.bin", (long)st) != 0) {
            fail("stat after a grow through ftruncate");
        }
        if (*(unsigned *)(st + ST_SIZE_OFF) != 8192) {
            fail_with("the size after a grow through ftruncate",
                      (int)*(unsigned *)(st + ST_SIZE_OFF));
        }
        n = syscall3(SYS_READ, two, (long)got, (int)sizeof(got));
        if (n != (int)sizeof(got)) {
            fail_with("reading through the second descriptor after the move", n);
        }
        for (i = 0; i < 256; i++) {
            if ((unsigned char)got[i] != pattern_byte((unsigned)i)) {
                fail_with("a byte the file had before it was moved", i);
            }
        }
        for (i = 256; i < n; i++) {
            if (got[i] != 0) {
                fail_with("a byte of the length a truncation added", i);
            }
        }
        if (syscall3(SYS_LSEEK, one, 0, 0) != 0) {
            fail("seeking the writing descriptor after the move");
        }
        if (syscall3(SYS_WRITE, one, (long)"after\n", 6) != 6) {
            fail("writing through the descriptor whose file was moved");
        }
        syscall1(SYS_CLOSE, one);
        syscall1(SYS_CLOSE, two);
    }

    /* An overwrite of bytes that are already there, which a write that
       reported what it never wrote would leave unchanged. */
    {
        int over = syscall3(SYS_OPEN, (long)"/k/over.bin", O_RDWR | O_CREAT | O_TRUNC, 0644);
        char got[16];
        int n;

        got[0] = 0; /* the read below fills it, which cppcheck cannot see */
        if (over < 0) {
            fail_with("creating the file to overwrite", over);
        }
        if (syscall3(SYS_WRITE, over, (long)"aaaaaaaaaaaaaaaa", 16) != 16) {
            fail("writing the file to overwrite");
        }
        if (syscall3(SYS_LSEEK, over, 4, 0) != 4) {
            fail("seeking into the file to overwrite");
        }
        if (syscall3(SYS_WRITE, over, (long)"bbbb", 4) != 4) {
            fail("overwriting inside a file");
        }
        if (syscall3(SYS_LSEEK, over, 0, 0) != 0) {
            fail("seeking back over the overwrite");
        }
        n = syscall3(SYS_READ, over, (long)got, 16);
        if (n != 16 || got[3] != 'a' || got[4] != 'b' || got[7] != 'b' || got[8] != 'a') {
            fail_with("the bytes an overwrite inside a file left", n);
        }
        syscall1(SYS_CLOSE, over);
    }

    if (syscall2(SYS_CHMOD, (long)"/k/written.bin", 0600) != 0) {
        fail("chmod on the volume");
    }
    rc = (int)stat_mode("/k/written.bin", st);
    if ((rc & 07777) != 0600) {
        fail_with("the mode after a chmod", rc & 07777);
    }

    /* The mode a creation asks for is the mode it gets: the format stamps a
       new node 0644 and the volume must put the caller's own bits on it. */
    fd = syscall3(SYS_OPEN, (long)"/k/secret.txt", O_WRONLY | O_CREAT | O_TRUNC, 0600);
    if (fd < 0) {
        fail_with("creating a file with a mode of its own", fd);
    }
    syscall1(SYS_CLOSE, fd);
    rc = (int)stat_mode("/k/secret.txt", st);
    if ((rc & 07777) != 0600) {
        fail_with("the mode of a file created 0600", rc & 07777);
    }
    if (syscall2(SYS_MKDIR, (long)"/k/private", 0700) != 0) {
        fail("making a directory with a mode of its own");
    }
    rc = (int)stat_mode("/k/private", st);
    if ((rc & 07777) != 0700) {
        fail_with("the mode of a directory made 0700", rc & 07777);
    }
}

/* What the boot after a write boot must find. */
static void readback_rows(void)
{
    unsigned char st[144];
    char buf[32];
    int fd;

    read_pattern("/k/written.bin", 20000);
    if (syscall2(SYS_STAT, (long)"/k/grown.bin", (long)st) != 0 ||
        *(unsigned *)(st + ST_SIZE_OFF) != 4000) {
        fail("the truncated file's size after a reboot");
    }
    if ((stat_mode("/k/written.bin", st) & 07777) != 0600) {
        fail("the mode after a reboot");
    }
    if ((stat_mode("/k/secret.txt", st) & 07777) != 0600) {
        fail("the created file's own mode after a reboot");
    }
    if ((stat_mode("/k/private", st) & 07777) != 0700) {
        fail("the made directory's own mode after a reboot");
    }
    {
        int fd2 = syscall3(SYS_OPEN, (long)"/k/over.bin", O_RDONLY, 0);
        char got[16];
        int n;

        got[0] = 0; /* as above: filled by the read, not by this */
        if (fd2 < 0) {
            fail_with("the overwritten file after a reboot", fd2);
        }
        n = syscall3(SYS_READ, fd2, (long)got, 16);
        if (n != 16 || got[3] != 'a' || got[4] != 'b' || got[7] != 'b' || got[8] != 'a') {
            fail_with("the bytes of the overwritten file after a reboot", n);
        }
        syscall1(SYS_CLOSE, fd2);
    }
    if (syscall2(SYS_STAT, (long)"/k/cut.bin", (long)st) != 0 ||
        *(unsigned *)(st + ST_SIZE_OFF) != 8192) {
        fail("the size of the file grown by truncation after a reboot");
    }
    fd = syscall3(SYS_OPEN, (long)"/k/made/inside", O_RDONLY, 0);
    if (fd < 0) {
        fail_with("the file in the new directory after a reboot", fd);
    }
    if (syscall3(SYS_READ, fd, (long)buf, (int)sizeof(buf)) != 7) {
        fail("the bytes in the new directory after a reboot");
    }
    syscall1(SYS_CLOSE, fd);
    fd = syscall3(SYS_OPEN, (long)"/k/after.txt", O_RDONLY, 0);
    if (fd < 0) {
        fail_with("the renamed file after a reboot", fd);
    }
    syscall1(SYS_CLOSE, fd);
    if (syscall3(SYS_OPEN, (long)"/k/before.txt", O_RDONLY, 0) != -ENOENT) {
        fail("the old name is still there after a reboot");
    }
    if (syscall3(SYS_OPEN, (long)"/k/gone.txt", O_RDONLY, 0) != -ENOENT) {
        fail("the removed file is still there after a reboot");
    }
    if (syscall3(SYS_OPEN, (long)"/k/empty", O_RDONLY, 0) != -ENOENT) {
        fail("the removed directory is still there after a reboot");
    }
}

__asm__(".global _start\n"
        "_start:\n"
        "   mov %esp, %eax\n"
        "   push %eax\n"
        "   call probe_main\n");

/*
 * The pull row writes until the stick is not there any more, then once
 * more; `KRFFSPROBE HOLDING` is said after the first write, so the pull the
 * suite makes on the word lands on a process writing.
 */
void probe_main(unsigned int *sp)
{
    int argc = (int)sp[0];
    char **argv = (char **)(sp + 1);
    const char *mode = argc > 1 ? argv[1] : "";
    int rc;

    syscall2(SYS_MKDIR, (long)"/k", 0755);

    if (streq(mode, "refuse")) {
        rc = syscall5(SYS_MOUNT, (long)"/dev/disk0p1", (long)"/k", (long)"krffs", MS_RDONLY, 0);
        if (rc == 0) {
            fail("the kernel mounted a volume it should have refused");
        }
        if (rc != -EINVAL) {
            fail_with("the errno of a refused mount", rc);
        }
        puts("KRFFSPROBE OK (refuse)");
        sys_exit(0);
    }

    /* A write the device fails takes the mount read-only, and the unmount
       that follows writes nothing: the volume owes the next mount a check,
       and the flag that says so must still be set when the host reads it. */
    if (streq(mode, "failed")) {
        int fd;
        int i;
        int refused = 0;

        rc = syscall5(SYS_MOUNT, (long)"/dev/disk0p1", (long)"/k", (long)"krffs", 0, 0);
        if (rc != 0) {
            fail_with("mounting the volume the device will fail", rc);
        }
        for (i = 0; i < 64 && !refused; i++) {
            fd = syscall3(SYS_OPEN, (long)"/k/failing.bin", O_WRONLY | O_CREAT | O_TRUNC, 0644);
            if (fd < 0) {
                refused = 1;
                break;
            }
            if (syscall3(SYS_WRITE, fd, (long)"failing\n", 8) != 8) {
                refused = 1;
            }
            if (syscall1(SYS_FSYNC, fd) != 0) {
                refused = 1;
            }
            syscall1(SYS_CLOSE, fd);
        }
        if (!refused) {
            fail("the device never failed a write");
        }
        rc = syscall2(SYS_UMOUNT2, (long)"/k", 0);
        if (rc != 0) {
            fail_with("unmounting a volume the device took read-only", rc);
        }
        puts("KRFFSPROBE OK (failed)");
        sys_exit(0);
    }

    /* Three volumes at once, past the two the kernel once had room for.
       Each is a volume of its own: what one mount writes is in that volume
       and in no other. */
    if (streq(mode, "three")) {
        int fd;
        char got[16];

        syscall2(SYS_MKDIR, (long)"/k2", 0755);
        syscall2(SYS_MKDIR, (long)"/k3", 0755);
        rc = syscall5(SYS_MOUNT, (long)"/dev/disk0p1", (long)"/k", (long)"krffs", 0, 0);
        if (rc != 0) {
            fail_with("mounting the first volume", rc);
        }
        rc = syscall5(SYS_MOUNT, (long)"/dev/disk0p2", (long)"/k2", (long)"krffs", 0, 0);
        if (rc != 0) {
            fail_with("mounting the second volume", rc);
        }
        rc = syscall5(SYS_MOUNT, (long)"/dev/disk0p3", (long)"/k3", (long)"krffs", 0, 0);
        if (rc != 0) {
            fail_with("mounting the third volume", rc);
        }
        fd = syscall3(SYS_OPEN, (long)"/k3/third.txt", O_WRONLY | O_CREAT | O_TRUNC, 0644);
        if (fd < 0 || syscall3(SYS_WRITE, fd, (long)"third\n", 6) != 6) {
            fail("writing the third volume");
        }
        syscall1(SYS_CLOSE, fd);
        if (syscall3(SYS_OPEN, (long)"/k/third.txt", O_RDONLY, 0) != -ENOENT ||
            syscall3(SYS_OPEN, (long)"/k2/third.txt", O_RDONLY, 0) != -ENOENT) {
            fail("a file of the third volume is in another");
        }
        fd = syscall3(SYS_OPEN, (long)"/k/first.txt", O_WRONLY | O_CREAT | O_TRUNC, 0644);
        if (fd < 0 || syscall3(SYS_WRITE, fd, (long)"first\n", 6) != 6) {
            fail("writing the first volume");
        }
        syscall1(SYS_CLOSE, fd);
        fd = syscall3(SYS_OPEN, (long)"/k2/second.txt", O_WRONLY | O_CREAT | O_TRUNC, 0644);
        if (fd < 0 || syscall3(SYS_WRITE, fd, (long)"second\n", 7) != 7) {
            fail("writing the second volume");
        }
        syscall1(SYS_CLOSE, fd);
        if (syscall3(SYS_OPEN, (long)"/k2/first.txt", O_RDONLY, 0) != -ENOENT) {
            fail("a file of one volume is in the other");
        }
        if (syscall3(SYS_OPEN, (long)"/k/second.txt", O_RDONLY, 0) != -ENOENT) {
            fail("a file of the other volume is in the one");
        }
        got[0] = 0;
        fd = syscall3(SYS_OPEN, (long)"/k2/second.txt", O_RDONLY, 0);
        if (fd < 0 || syscall3(SYS_READ, fd, (long)got, 7) != 7 || got[0] != 's') {
            fail("reading back the second volume");
        }
        syscall1(SYS_CLOSE, fd);
        if (syscall2(SYS_UMOUNT2, (long)"/k", 0) != 0) {
            fail("unmounting the first volume");
        }
        if (syscall2(SYS_UMOUNT2, (long)"/k3", 0) != 0) {
            fail("unmounting the third volume");
        }
        if (syscall2(SYS_UMOUNT2, (long)"/k2", 0) != 0) {
            fail("unmounting the second volume");
        }
        puts("KRFFSPROBE OK (three)");
        sys_exit(0);
    }

    /* A stick pulled from under a mounted volume. What the machine owes then
       is an error from what was in flight, no writes to a device that is not
       there, and an unmount that takes the mount down rather than failing its
       sync for ever. */
    if (streq(mode, "pull")) {
        int tries = 0;
        int writes = 0;

        rc = syscall5(SYS_MOUNT, (long)"/dev/disk0p1", (long)"/k", (long)"krffs", 0, 0);
        if (rc != 0) {
            fail_with("mounting the stick's volume", rc);
        }
        {
            int fd = syscall3(SYS_OPEN, (long)"/k/hello.txt", O_RDONLY, 0);
            char one[8];

            one[0] = 0;
            if (fd < 0 || syscall3(SYS_READ, fd, (long)one, (int)sizeof(one)) <= 0) {
                fail("reading the stick's volume before the pull");
            }
            syscall1(SYS_CLOSE, fd);
        }
        for (;;) {
            int fd = syscall3(SYS_OPEN, (long)"/k/pulled.bin", O_WRONLY | O_CREAT | O_TRUNC, 0644);
            int wrote;

            if (fd < 0) {
                break;
            }
            wrote = syscall3(SYS_WRITE, fd, (long)"pull\n", 5);
            syscall1(SYS_FSYNC, fd);
            if (++writes == 1) {
                puts("KRFFSPROBE HOLDING");
            }
            syscall1(SYS_CLOSE, fd);
            if (wrote != 5) {
                break;
            }
            if (++tries > 100000) {
                fail("the stick was never pulled");
            }
        }
        /* The mount stays while the stick is plugged back in: the driver
           finds a second device with the first still held, and a machine
           that gave the new one the old one's slot would be handing this
           mount a pointer into it. */
        puts("KRFFSPROBE GONE");
        for (tries = 0; tries < 2000000; tries++) {
            int fd = syscall3(SYS_OPEN, (long)"/dev/disk0p1", O_RDONLY, 0);

            if (fd >= 0) {
                char one[512];
                int got = syscall3(SYS_READ, fd, (long)one, (int)sizeof(one));

                syscall1(SYS_CLOSE, fd);
                if (got == (int)sizeof(one)) {
                    break; /* a stick answers there again */
                }
            }
        }
        {
            /* And the mount over the device that went answers as it did:
               nothing of the new stick reaches it. */
            int fd = syscall3(SYS_OPEN, (long)"/k/hello.txt", O_RDONLY, 0);
            char one[8];

            one[0] = 0;
            if (fd >= 0) {
                if (syscall3(SYS_READ, fd, (long)one, (int)sizeof(one)) > 0) {
                    fail("a mount whose stick went read the stick that replaced it");
                }
                syscall1(SYS_CLOSE, fd);
            }
        }
        syscall1(SYS_SYNC, 0);
        rc = syscall2(SYS_UMOUNT2, (long)"/k", 0);
        if (rc != 0) {
            fail_with("unmounting a volume whose stick has gone", rc);
        }
        puts("KRFFSPROBE OK (pull)");
        sys_exit(0);
    }

    if (streq(mode, "write") || streq(mode, "crash") || streq(mode, "writers")) {
        rc = syscall5(SYS_MOUNT, (long)"/dev/disk0p1", (long)"/k", (long)"krffs", 0, 0);
        if (rc != 0) {
            fail_with("mounting the volume for writing", rc);
        }
        if (streq(mode, "crash")) {
            int fd = syscall3(SYS_OPEN, (long)"/k/killed.bin", O_WRONLY | O_CREAT, 0644);

            if (fd < 0) {
                fail_with("creating the file to be killed", fd);
            }
            /* The same bytes over and over: the machine is killed somewhere
               in here, and a volume that filled up would say nothing about
               what a stop leaves behind. The token comes after the first
               round, so what the machine is killed in the middle of is a
               write and not a mount. */
            for (;;) {
                syscall3(SYS_LSEEK, fd, 0, 0);
                write_pattern(fd, 4096);
                syscall1(SYS_FSYNC, fd);
                puts("KRFFSPROBE WRITING");
            }
        }
        if (streq(mode, "writers")) {
            int child = syscall1(SYS_FORK, 0);
            const char *name = child == 0 ? "/k/child.bin" : "/k/parent.bin";
            int fd = syscall3(SYS_OPEN, (long)name, O_WRONLY | O_CREAT | O_TRUNC, 0644);
            int i;

            if (fd < 0) {
                fail_with("a writer's file", fd);
            }
            for (i = 0; i < 8; i++) {
                write_pattern_from(fd, (unsigned)i * 2000, 2000);
            }
            syscall1(SYS_FSYNC, fd);
            syscall1(SYS_CLOSE, fd);
            if (child == 0) {
                sys_exit(0);
            }
            {
                int status = 0;

                if (syscall3(SYS_WAITPID, child, (long)&status, 0) != child) {
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
        rc = syscall2(SYS_UMOUNT2, (long)"/k", 0);
        if (rc != 0) {
            fail_with("unmounting after writing", rc);
        }
        syscall3(SYS_WRITE, 1, (long)"KRFFSPROBE OK (", 15);
        syscall3(SYS_WRITE, 1, (long)mode, (int)strlen(mode));
        puts(")");
        sys_exit(0);
    }

    if (streq(mode, "readback")) {
        rc = syscall5(SYS_MOUNT, (long)"/dev/disk0p1", (long)"/k", (long)"krffs", MS_RDONLY, 0);
        if (rc != 0) {
            fail_with("mounting the volume to read it back", rc);
        }
        readback_rows();
        rc = syscall2(SYS_UMOUNT2, (long)"/k", 0);
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
    rc = syscall5(SYS_MOUNT, (long)"/dev/disk0p1", (long)"/k", (long)"krffs", MS_RDONLY, 0);
    if (rc != 0) {
        fail_with("mounting the volume", rc);
    }
    read_rows();
    mounts_say_ro();
    refusal_rows();
    rc = syscall2(SYS_UMOUNT2, (long)"/k", 0);
    if (rc != 0) {
        fail_with("unmounting the volume", rc);
    }

    if (syscall3(SYS_OPEN, (long)"/k/pattern.bin", O_RDONLY, 0) >= 0) {
        fail("a file of the volume is still there after the unmount");
    }
    puts("KRFFSPROBE OK (read)");
    sys_exit(0);
}
