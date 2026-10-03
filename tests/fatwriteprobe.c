/**
 * fatwriteprobe: FAT32 written from a program, read back after a reboot.
 *
 * `fatwriteprobe write` mounts /dev/disk0p2 on /fw for writing and puts on
 * it a 64 KiB file of a pattern, a long-named file, a directory with a
 * file inside, a renamed file, a removed file, and a file that is fsynced;
 * then it unmounts cleanly. `fatwriteprobe readback`, on the next boot,
 * mounts the volume and requires every byte and name; the host reads the
 * same volume with mtools and the check. `fatwriteprobe crash` rewrites
 * two hundred files until the machine is killed, and says so before it
 * starts, which is where the suite kills it; `fatwriteprobe repair` mounts
 * what that left, which runs the check, and counts what survived.
 *
 * Prints "FATWRITE OK (mode)" or "FATWRITE FAIL: reason". Freestanding;
 * direct system calls (probe.h).
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
#define MNT_DETACH      2

#define EPERM        1
#define ENOENT       2
#define EACCES       13
#define EMFILE       24
#define EBUSY        16
#define EXDEV        18
#define ENOTDIR      20
#define EINVAL       22
#define EROFS        30
#define ENAMETOOLONG 36

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

static void fail(const char *why) __attribute__((noreturn));
static void fail(const char *why)
{
    syscall3(SYS_WRITE, 1, (long)"FATWRITE FAIL: ", 15);
    puts(why);
    sys_exit(1);
}

/* A failure with its number, for the boot's log. */
static void fail_n(const char *why, int n) __attribute__((noreturn));
static void fail_n(const char *why, int n)
{
    char digits[16];
    int i = 15, neg = n < 0;
    unsigned v = neg ? (unsigned)(-n) : (unsigned)n;

    digits[i] = 0;
    do {
        digits[--i] = (char)('0' + v % 10);
        v /= 10;
    } while (v && i > 1);
    if (neg) {
        digits[--i] = '-';
    }
    syscall3(SYS_WRITE, 1, (long)"FATWRITE FAIL: ", 15);
    syscall3(SYS_WRITE, 1, (long)why, (int)strlen(why));
    syscall3(SYS_WRITE, 1, (long)" (", 2);
    syscall3(SYS_WRITE, 1, (long)(digits + i), (int)strlen(digits + i));
    puts(")");
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
    char st[KSTAT_SIZE];

    st[0] = 0;
    return syscall2(SYS_STAT, (long)path, (long)st);
}

/* The type mounted: vfat unless the second argument names another, which
 * is how the exfat suite runs the same modes on its volume. */
static const char *fs_type = "vfat";

static int do_mount(const char *src, const char *target, unsigned flags, const char *data)
{
    return syscall5(SYS_MOUNT, (long)src, (long)target, (long)fs_type, (int)flags, (long)data);
}

static void sleep_ms(int ms)
{
    long ts[2];

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

#define FW      "/fw"
#define SRC     "/dev/disk0p2"
#define O_CREAT 0100
#define O_TRUNC 01000

static unsigned char pattern[65536];
static unsigned char back[65536];

static void fill_pattern(void)
{
    unsigned i;

    for (i = 0; i < sizeof(pattern); i++) {
        pattern[i] = (unsigned char)((i * 31 + 7) & 0xFF);
    }
}

static int write_file(const char *path, const void *data, int n)
{
    int fd = syscall3(SYS_OPEN, (long)path, O_WRONLY | O_CREAT | O_TRUNC, 0644);
    int rc;

    if (fd < 0) {
        return fd;
    }
    rc = syscall3(SYS_WRITE, fd, (long)data, n);
    syscall1(SYS_CLOSE, fd);
    return rc == n ? 0 : (rc < 0 ? rc : -1);
}

static int read_file(const char *path, void *dst, int cap)
{
    int fd = syscall3(SYS_OPEN, (long)path, O_RDONLY, 0);
    int total = 0, n;

    if (fd < 0) {
        return fd;
    }
    while ((n = syscall3(SYS_READ, fd, (long)dst + total, cap - total)) > 0) {
        total += n;
    }
    syscall1(SYS_CLOSE, fd);
    return n < 0 ? n : total;
}

static void mode_write(void)
{
    int fd;

    fill_pattern();
    syscall2(SYS_MKDIR, (long)FW, 0755);
    if (do_mount(SRC, FW, 0, 0) != 0) {
        fail("the writable mount");
    }
    if (write_file(FW "/pattern.bin", pattern, sizeof(pattern)) != 0) {
        fail("the pattern file");
    }
    if (write_file(FW "/Long Name File.txt", "long", 4) != 0) {
        fail("the long-named file");
    }
    if (syscall2(SYS_MKDIR, (long)FW "/Sub Dir", 0755) != 0 ||
        write_file(FW "/Sub Dir/inner.txt", "inner", 5) != 0) {
        fail("the directory and its file");
    }
    if (write_file(FW "/tmp.txt", "moved", 5) != 0 ||
        syscall2(SYS_RENAME, (long)FW "/tmp.txt", (long)FW "/renamed.txt") != 0) {
        fail("the renamed file");
    }
    if (write_file(FW "/gone.txt", "gone", 4) != 0 ||
        syscall1(SYS_UNLINK, (long)FW "/gone.txt") != 0) {
        fail("the removed file");
    }
    fd = syscall3(SYS_OPEN, (long)FW "/synced.txt", O_WRONLY | O_CREAT | O_TRUNC, 0644);
    if (fd < 0 || syscall3(SYS_WRITE, fd, (long)"synced", 6) != 6 || syscall1(SYS_FSYNC, fd) != 0) {
        fail("the fsynced file");
    }
    syscall1(SYS_CLOSE, fd);
    if (syscall2(SYS_UMOUNT2, (long)FW, 0) != 0) {
        fail("the clean unmount");
    }
    puts("FATWRITE OK (wrote)");
}

static void mode_readback(void)
{
    int n, i;

    fill_pattern();
    syscall2(SYS_MKDIR, (long)FW, 0755);
    if (do_mount(SRC, FW, MS_RDONLY, 0) != 0) {
        fail("the read-only mount for the read back");
    }
    n = read_file(FW "/pattern.bin", back, sizeof(back));
    if (n != (int)sizeof(pattern)) {
        fail("the pattern file's size after the reboot");
    }
    for (i = 0; i < n; i++) {
        if (back[i] != pattern[i]) {
            fail("the pattern file's bytes after the reboot");
        }
    }
    if (read_file(FW "/Long Name File.txt", back, sizeof(back)) != 4 || back[0] != 'l') {
        fail("the long-named file after the reboot");
    }
    if (read_file(FW "/long name file.txt", back, sizeof(back)) != 4) {
        fail("the long name with its case folded");
    }
    if (fs_type[0] == 'v' && read_file(FW "/LONGNA~1.TXT", back, sizeof(back)) != 4) {
        fail("the alias"); /* FAT32's short alias; exFAT has none */
    }
    if (read_file(FW "/Sub Dir/inner.txt", back, sizeof(back)) != 5) {
        fail("the file in the directory");
    }
    if (read_file(FW "/renamed.txt", back, sizeof(back)) != 5 || back[0] != 'm') {
        fail("the renamed file");
    }
    if (syscall3(SYS_OPEN, (long)FW "/tmp.txt", O_RDONLY, 0) != -ENOENT ||
        syscall3(SYS_OPEN, (long)FW "/gone.txt", O_RDONLY, 0) != -ENOENT) {
        fail("the old and the removed names are gone");
    }
    if (read_file(FW "/synced.txt", back, sizeof(back)) != 6) {
        fail("the fsynced file");
    }
    if (syscall2(SYS_UMOUNT2, (long)FW, 0) != 0) {
        fail("umount2 after the read back");
    }
    puts("FATWRITE OK (read back)");
}

static void mode_crash(void)
{
    int i;
    static char name[32];

    fill_pattern();
    syscall2(SYS_MKDIR, (long)FW, 0755);
    if (do_mount(SRC, FW, 0, 0) != 0) {
        fail("the writable mount for the crash");
    }
    puts("FATWRITE WRITING");
    /* Rewritten until the machine is killed, so the kill lands inside a
     * write sequence however long the harness waits. */
    for (i = 0;; i = (i + 1) % 200) {
        name[0] = '/';
        name[1] = 'f';
        name[2] = 'w';
        name[3] = '/';
        name[4] = 'f';
        name[5] = (char)('0' + i / 100);
        name[6] = (char)('0' + (i / 10) % 10);
        name[7] = (char)('0' + i % 10);
        name[8] = 0;
        {
            int rc = write_file(name, pattern, 1024);

            if (rc != 0) {
                fail_n("a file of the crash sequence", rc);
            }
        }
    }
}

static void mode_repair(void)
{
    int fd, n, count = 0;
    static unsigned char dents[4096];

    syscall2(SYS_MKDIR, (long)FW, 0755);
    if (do_mount(SRC, FW, 0, 0) != 0) {
        fail("the mount after the crash");
    }
    fd = syscall3(SYS_OPEN, (long)FW, O_RDONLY, 0);
    if (fd < 0) {
        fail("open the volume's root");
    }
    while ((n = syscall3(SYS_GETDENTS64, fd, (long)dents, (int)sizeof(dents))) > 0) {
        int off = 0;

        while (off + 19 < n) {
            unsigned short reclen;
            const char *dn = (const char *)dents + off + 19;

            reclen = (unsigned short)(dents[off + 16] | (dents[off + 17] << 8));
            if (dn[0] == 'f' && dn[1] >= '0' && dn[1] <= '9') {
                count++;
            }
            if (reclen == 0) {
                break;
            }
            off += reclen;
        }
    }
    syscall1(SYS_CLOSE, fd);
    if (count < 1) {
        fail("nothing of the crash sequence survived");
    }
    if (syscall2(SYS_UMOUNT2, (long)FW, 0) != 0) {
        fail("umount2 after the repair");
    }
    puts("FATWRITE OK (repaired)");
}

void probe_main(unsigned long *sp);

PROBE_SP_ENTRY;

void probe_main(unsigned long *sp)
{
    int argc = (int)sp[0];
    char **argv = (char **)(sp + 1);
    char mode = argc >= 2 ? argv[1][0] : 'w';

    if (argc >= 3) {
        fs_type = argv[2];
    }
    if (mode == 'r' && argv[1][1] == 'e' && argv[1][2] == 'p') {
        mode_repair();
    } else if (mode == 'r') {
        mode_readback();
    } else if (mode == 'c') {
        mode_crash();
    } else {
        mode_write();
    }
    sys_exit(0);
}
