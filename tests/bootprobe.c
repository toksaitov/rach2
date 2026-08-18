/**
 * Exercises /proc/cmdline and the /dev/bootpart boot-partition window.
 *
 * Two boots share this one probe, told apart by the command line the harness
 * passes. With `rach2.bootprobe=fixture` the disk carries a FAT-typed
 * partition 1 of known bytes (sector k holds byte j = (k * 7 + j) & 0xff), and
 * the probe proves the device end to end: the 0600 mode from stat and fstat
 * both, seeks resolved against the partition size, a read of known bytes at a
 * seek, a written sector read back, exactly 0 at the end, -ENOSPC past it,
 * -EINVAL unaligned, and -EACCES for an unprivileged open. Without the flag
 * the disk has only the KRFFS partition and the device must be absent from
 * stat and open alike. /proc/cmdline is proved at each of its seams: content,
 * the /proc listing, stat/fstat identity, and the fd's readlink origin.
 *
 * Prints "BOOTPROBE OK" or "BOOTPROBE ABSENT OK".
 *
 * Freestanding; direct int 0x80.
 */

typedef unsigned int size_t;

#define SYS_EXIT       1
#define SYS_READ       3
#define SYS_WRITE      4
#define SYS_OPEN       5
#define SYS_CLOSE      6
#define SYS_LSEEK      19
#define SYS_READLINK   85
#define SYS_STAT64     195
#define SYS_FSTAT64    197
#define SYS_SETUID32   213
#define SYS_GETDENTS64 220

#define O_RDONLY 0
#define O_WRONLY 1
#define O_RDWR   2

#define SEEK_SET 0
#define SEEK_END 2

#define ENOENT 2
#define EPERM  1
#define EACCES 13
#define EINVAL 22
#define ENOSPC 28

/* i386 struct stat64: st_mode at offset 16, the 64-bit st_ino (the field musl
 * reads) at 88 (96 bytes total). */
#define ST_MODE  16
#define ST_INO64 88

#define PART_SECTORS 1024
#define SECTOR       512

static int syscall1(int n, int a)
{
    int r;
    __asm__ __volatile__("int $0x80" : "=a"(r) : "0"(n), "b"(a) : "memory");
    return r;
}

static int syscall3(int n, int a, int b, int c)
{
    int r;
    __asm__ __volatile__("int $0x80" : "=a"(r) : "0"(n), "b"(a), "c"(b), "d"(c) : "memory");
    return r;
}

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
    syscall3(SYS_WRITE, 1, (int)(size_t)s, (int)strlen(s));
    syscall3(SYS_WRITE, 1, (int)(size_t)"\n", 1);
}

static int fails = 0;

static void check(int cond, const char *msg)
{
    if (!cond) {
        fails++;
        syscall3(SYS_WRITE, 1, (int)(size_t)"  FAIL: ", 8);
        puts(msg);
    }
}

/** Whether `hay` (NUL-terminated) contains `needle`. */
static int contains(const char *hay, const char *needle)
{
    size_t i, j;

    for (i = 0; hay[i]; i++) {
        for (j = 0; needle[j] && hay[i + j] == needle[j]; j++) {
        }
        if (!needle[j]) {
            return 1;
        }
    }
    return needle[0] == 0;
}

static char cmdline[300];
static char statbuf[96];
static char fstatbuf[96];
static char sector[SECTOR];
static char written[SECTOR];

void _start(void)
{
    int fd, rc, n, fixture;
    unsigned int mode, ino_stat, ino_fstat;

    /* /proc/cmdline: content, then the descriptor's readlink origin. */
    fd = syscall3(SYS_OPEN, (int)(size_t)"/proc/cmdline", O_RDONLY, 0);
    check(fd >= 0, "open /proc/cmdline");
    n = syscall3(SYS_READ, fd, (int)(size_t)cmdline, (int)sizeof(cmdline) - 1);
    check(n > 0, "read /proc/cmdline");
    if (n > 0) {
        cmdline[n] = '\0';
    }
    /* The RAM-root boot deliberately does not ask for a disk root, so the token
       it is checked for is the one its own mode is named by. */
    if (contains(cmdline, "rach2.bootprobe=ramroot")) {
        check(!contains(cmdline, "rach2.fsdisk"), "the RAM-root boot asks for no disk root");
    } else {
        check(contains(cmdline, "rach2.fsdisk"), "cmdline carries rach2.fsdisk");
    }
    check(n > 0 && cmdline[n - 1] == '\n', "cmdline ends with a newline");
    {
        char link[32] = "";
        char path[24] = "/proc/self/fd/";
        int d = fd, len = 0;
        char digits[8];
        int k = 0;

        do {
            digits[k++] = (char)('0' + d % 10);
            d /= 10;
        } while (d > 0);
        while (path[len]) {
            len++;
        }
        while (k > 0) {
            path[len++] = digits[--k];
        }
        path[len] = '\0';
        rc = syscall3(SYS_READLINK, (int)(size_t)path, (int)(size_t)link, (int)sizeof(link) - 1);
        check(rc > 0, "readlink the cmdline fd");
        if (rc > 0) {
            link[rc] = '\0';
        }
        check(rc > 0 && contains(link, "/proc/cmdline"), "the fd's origin is /proc/cmdline");
    }

    /* stat and fstat agree on the file's identity. */
    rc = syscall3(SYS_STAT64, (int)(size_t)"/proc/cmdline", (int)(size_t)statbuf, 0);
    check(rc == 0, "stat64 /proc/cmdline");
    rc = syscall3(SYS_FSTAT64, fd, (int)(size_t)fstatbuf, 0);
    check(rc == 0, "fstat64 the cmdline fd");
    mode = *(unsigned int *)(statbuf + ST_MODE);
    check(mode == 0100444, "stat mode is a read-only regular file");
    ino_stat = *(unsigned int *)(statbuf + ST_INO64);
    ino_fstat = *(unsigned int *)(fstatbuf + ST_INO64);
    check(ino_stat == 0x101, "stat inode is the cmdline inode");
    check(mode == *(unsigned int *)(fstatbuf + ST_MODE) && ino_fstat == 0x101,
          "fstat agrees on mode and the cmdline inode");
    syscall1(SYS_CLOSE, fd);

    /* The /proc listing names cmdline. */
    fd = syscall3(SYS_OPEN, (int)(size_t)"/proc", O_RDONLY, 0);
    check(fd >= 0, "open /proc as a directory");
    if (fd >= 0) {
        char dbuf[512];
        int saw = 0, off = 0;

        n = syscall3(SYS_GETDENTS64, fd, (int)(size_t)dbuf, (int)sizeof(dbuf));
        while (off + 19 < n) {
            unsigned short reclen = *(unsigned short *)(dbuf + off + 16);
            const char *nm = dbuf + off + 19;

            if (contains(nm, "cmdline")) {
                saw = 1;
            }
            if (reclen == 0) {
                break;
            }
            off += reclen;
        }
        check(saw, "getdents /proc lists cmdline");
        syscall1(SYS_CLOSE, fd);
    }

    fixture = contains(cmdline, "rach2.bootprobe=fixture");

    /* A machine that mounted a RAM image and no disk file system still has a
       disk, and milestone 25 gives it both level-one devices: the boot
       partition it may have to repair, and the gap holding the boot loader.
       Before discovery ran ahead of the root choice, /dev/bootpart appeared
       only at the end of a successful mount, so this boot had neither. */
    if (contains(cmdline, "rach2.bootprobe=ramroot")) {
        rc = syscall3(SYS_STAT64, (int)(size_t)"/dev/bootpart", (int)(size_t)statbuf, 0);
        check(rc == 0, "a RAM-root boot still has /dev/bootpart");
        check(*(unsigned int *)(statbuf + ST_MODE) == 020600, "and it is a 0600 device");
        rc = syscall3(SYS_STAT64, (int)(size_t)"/dev/grubcore", (int)(size_t)statbuf, 0);
        check(rc == 0, "a RAM-root boot has /dev/grubcore");
        check(*(unsigned int *)(statbuf + ST_MODE) == 020600, "and it is a 0600 device");

        fd = syscall3(SYS_OPEN, (int)(size_t)"/dev/grubcore", O_RDWR, 0);
        check(fd >= 0, "root opens /dev/grubcore read-write");
        rc = syscall3(SYS_LSEEK, fd, 0, SEEK_END);
        check(rc == 2047 * SECTOR, "grubcore spans LBA 1 through 2047 and no more");

        rc = syscall3(SYS_LSEEK, fd, 0, SEEK_SET);
        check(rc == 0, "seek to the gap's first sector");
        n = syscall3(SYS_READ, fd, (int)(size_t)sector, SECTOR);
        check(n == SECTOR, "the gap's first sector reads whole");

        /* The window ends at LBA 2047 and the checks are on the whole request,
           so a write starting past the end writes nothing rather than part. */
        rc = syscall3(SYS_LSEEK, fd, 2047 * SECTOR, SEEK_SET);
        check(rc == 2047 * SECTOR, "seek to the gap's end");
        n = syscall3(SYS_READ, fd, (int)(size_t)sector, SECTOR);
        check(n == 0, "a read at the end is exactly 0");
        n = syscall3(SYS_WRITE, fd, (int)(size_t)sector, SECTOR);
        check(n == -ENOSPC, "a write at the end is -ENOSPC");
        rc = syscall3(SYS_LSEEK, fd, 2046 * SECTOR, SEEK_SET);
        check(rc == 2046 * SECTOR, "seek to the last sector");
        n = syscall3(SYS_WRITE, fd, (int)(size_t)sector, 2 * SECTOR);
        check(n == -ENOSPC, "a write crossing the end writes nothing");

        rc = syscall3(SYS_LSEEK, fd, 1, SEEK_SET);
        check(rc == 1, "an unaligned seek is allowed");
        n = syscall3(SYS_READ, fd, (int)(size_t)sector, SECTOR);
        check(n == -EINVAL, "an unaligned read is -EINVAL");
        n = syscall3(SYS_WRITE, fd, (int)(size_t)sector, SECTOR);
        check(n == -EINVAL, "an unaligned write is -EINVAL");
        rc = syscall3(SYS_LSEEK, fd, 0, SEEK_SET);
        check(rc == 0, "seek back to the start");
        n = syscall3(SYS_WRITE, fd, (int)(size_t)sector, SECTOR - 1);
        check(n == -EINVAL, "a partial-sector count is -EINVAL");

        /* The mode stops a non-root caller at open and nothing else: the
           credential check passes uid 0 whatever the mode says. So the write
           handler asks again, and this descriptor, opened while privileged and
           carried across a privilege drop, must stop working. */
        rc = syscall1(SYS_SETUID32, 1000);
        check(rc == 0, "the probe drops to an unprivileged uid");
        rc = syscall3(SYS_LSEEK, fd, 0, SEEK_SET);
        check(rc == 0, "the descriptor still seeks after the drop");
        n = syscall3(SYS_READ, fd, (int)(size_t)sector, SECTOR);
        check(n == SECTOR, "and still reads, which is not the privilege in question");
        n = syscall3(SYS_WRITE, fd, (int)(size_t)sector, SECTOR);
        check(n == -EPERM, "but a write through it is -EPERM after the drop");
        syscall1(SYS_CLOSE, fd);

        rc = syscall3(SYS_OPEN, (int)(size_t)"/dev/grubcore", O_RDONLY, 0);
        check(rc == -EACCES, "and an unprivileged open of grubcore is -EACCES");

        if (fails == 0) {
            puts("BOOTPROBE RAMROOT OK");
            sys_exit(0);
        }
        puts("BOOTPROBE FAILED");
        sys_exit(1);
    }

    if (!fixture) {
        /* A single-partition disk: the device must not exist anywhere. */
        rc = syscall3(SYS_STAT64, (int)(size_t)"/dev/bootpart", (int)(size_t)statbuf, 0);
        check(rc == -ENOENT, "stat64 /dev/bootpart is -ENOENT");
        rc = syscall3(SYS_OPEN, (int)(size_t)"/dev/bootpart", O_RDONLY, 0);
        check(rc == -ENOENT, "open /dev/bootpart is -ENOENT");
        if (fails == 0) {
            puts("BOOTPROBE ABSENT OK");
            sys_exit(0);
        }
        puts("BOOTPROBE FAILED");
        sys_exit(1);
    }

    /* The fixture: partition 1 is FAT-typed known bytes. */
    rc = syscall3(SYS_STAT64, (int)(size_t)"/dev/bootpart", (int)(size_t)statbuf, 0);
    check(rc == 0, "stat64 /dev/bootpart");
    mode = *(unsigned int *)(statbuf + ST_MODE);
    check(mode == 020600, "bootpart is a 0600 character device");

    fd = syscall3(SYS_OPEN, (int)(size_t)"/dev/bootpart", O_RDWR, 0);
    check(fd >= 0, "root opens /dev/bootpart read-write");
    rc = syscall3(SYS_FSTAT64, fd, (int)(size_t)fstatbuf, 0);
    check(rc == 0 && *(unsigned int *)(fstatbuf + ST_MODE) == 020600,
          "fstat mode matches the path stat");

    /* Known bytes at a seek: sector 5 of the pattern. */
    rc = syscall3(SYS_LSEEK, fd, 5 * SECTOR, SEEK_SET);
    check(rc == 5 * SECTOR, "seek to sector 5");
    n = syscall3(SYS_READ, fd, (int)(size_t)sector, SECTOR);
    check(n == SECTOR, "read sector 5 whole");
    {
        int j, ok = 1;

        for (j = 0; j < SECTOR; j++) {
            if ((unsigned char)sector[j] != (unsigned char)((5 * 7 + j) & 0xff)) {
                ok = 0;
                break;
            }
        }
        check(ok, "sector 5 holds the pattern");
    }

    /* The end: SEEK_END lands on the partition size, a read there is exactly
       0, a read past it is exactly 0, and a write there is -ENOSPC. */
    rc = syscall3(SYS_LSEEK, fd, 0, SEEK_END);
    check(rc == PART_SECTORS * SECTOR, "SEEK_END is the partition size");
    n = syscall3(SYS_READ, fd, (int)(size_t)sector, SECTOR);
    check(n == 0, "a read at the end returns exactly 0");
    rc = syscall3(SYS_LSEEK, fd, SECTOR, SEEK_END);
    check(rc == (PART_SECTORS + 1) * SECTOR, "a seek past the end is allowed");
    n = syscall3(SYS_READ, fd, (int)(size_t)sector, SECTOR);
    check(n == 0, "a read past the end returns exactly 0");
    n = syscall3(SYS_WRITE, fd, (int)(size_t)sector, SECTOR);
    check(n == -ENOSPC, "a write past the end is -ENOSPC");

    /* Alignment: partial sectors are refused in both directions. */
    rc = syscall3(SYS_LSEEK, fd, 0, SEEK_SET);
    check(rc == 0, "seek back to 0");
    n = syscall3(SYS_READ, fd, (int)(size_t)sector, 100);
    check(n == -EINVAL, "an unaligned read count is -EINVAL");
    rc = syscall3(SYS_LSEEK, fd, 100, SEEK_SET);
    check(rc == 100, "a seek needs no alignment");
    n = syscall3(SYS_READ, fd, (int)(size_t)sector, SECTOR);
    check(n == -EINVAL, "an unaligned offset is -EINVAL");

    /* A write comes back on a re-read. */
    {
        int j;

        for (j = 0; j < SECTOR; j++) {
            written[j] = (char)(0xA5 ^ (j & 0xff));
        }
    }
    rc = syscall3(SYS_LSEEK, fd, 7 * SECTOR, SEEK_SET);
    check(rc == 7 * SECTOR, "seek to sector 7");
    n = syscall3(SYS_WRITE, fd, (int)(size_t)written, SECTOR);
    check(n == SECTOR, "write sector 7 whole");
    rc = syscall3(SYS_LSEEK, fd, 7 * SECTOR, SEEK_SET);
    check(rc == 7 * SECTOR, "seek back to sector 7");
    n = syscall3(SYS_READ, fd, (int)(size_t)sector, SECTOR);
    check(n == SECTOR, "read sector 7 back");
    {
        int j, ok = 1;

        for (j = 0; j < SECTOR; j++) {
            if (sector[j] != written[j]) {
                ok = 0;
                break;
            }
        }
        check(ok, "the written sector reads back");
    }
    syscall1(SYS_CLOSE, fd);

    /* Unprivileged, the device refuses at open with -EACCES. */
    rc = syscall1(SYS_SETUID32, 1000);
    check(rc == 0, "drop to uid 1000");
    rc = syscall3(SYS_OPEN, (int)(size_t)"/dev/bootpart", O_RDONLY, 0);
    check(rc == -EACCES, "an unprivileged open is -EACCES");
    rc = syscall3(SYS_OPEN, (int)(size_t)"/dev/bootpart", O_RDWR, 0);
    check(rc == -EACCES, "an unprivileged read-write open is -EACCES");

    if (fails == 0) {
        puts("BOOTPROBE OK");
        sys_exit(0);
    }
    puts("BOOTPROBE FAILED");
    sys_exit(1);
}
