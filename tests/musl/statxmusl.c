/**
 * statx, which GNU libc and musl call first for every stat.
 *
 * Each row asks the raw statx and the raw fstatat64 (newfstatat on x86-64)
 * about one object and
 * requires them to agree: the type and permission bits, the inode, the link
 * count, the owner, the size, the blocks, the three times, and the device
 * numbers, which statx splits into major and minor numbers that makedev
 * joins again. The objects are a regular file, a directory, a FIFO,
 * /dev/null (major 1, minor 3), /proc/self followed and not, a pipe named by
 * its descriptor with AT_EMPTY_PATH, and the working directory named by
 * AT_FDCWD and an empty path. The mask reports the basic set. Then the
 * refusals: a flag statx does not take, both sync flags at once, the
 * reserved mask bit, a missing file, and an empty path without
 * AT_EMPTY_PATH.
 *
 * Prints "STATXMUSL OK" on success and "STATXMUSL FAIL (detail)" otherwise.
 */

#define _GNU_SOURCE
#include <errno.h>
#include <fcntl.h>
#include <stdint.h>
#include <stdio.h>
#include <string.h>
#include <sys/stat.h>
#include <sys/syscall.h>
#include <sys/sysmacros.h>
#include <unistd.h>

#define FILE_PATH "/tmp/statxmusl.f"
#define FIFO_PATH "/tmp/statxmusl.p"

/*
 * The struct the kernel's fstatat fills, whose layouts stat(2) gives: i386's
 * 96-byte stat64, through fstatat64, and x86-64's 144-byte stat, through
 * newfstatat. A time's seconds are as wide as the struct keeps them.
 */
#if defined __x86_64__
struct kstat {
    uint64_t k_dev;
    uint64_t k_ino;
    uint64_t k_nlink;
    uint32_t k_mode, k_uid, k_gid, pad0;
    uint64_t k_rdev;
    int64_t k_size;
    uint64_t k_blksize;
    uint64_t k_blocks;
    uint64_t k_atime, k_atime_nsec;
    uint64_t k_mtime, k_mtime_nsec;
    uint64_t k_ctime, k_ctime_nsec;
    int64_t unused[3];
};

typedef char kstat_is_144_bytes[sizeof(struct kstat) == 144 ? 1 : -1];
typedef int64_t ksec_t;
#define SYS_FSTATAT SYS_newfstatat
#else
struct kstat {
    uint64_t k_dev;
    uint8_t pad0[4];
    uint32_t old_ino;
    uint32_t k_mode, k_nlink, k_uid, k_gid;
    uint64_t k_rdev;
    uint8_t pad3[4];
    int64_t k_size;
    uint32_t k_blksize;
    uint64_t k_blocks;
    uint32_t k_atime, k_atime_nsec;
    uint32_t k_mtime, k_mtime_nsec;
    uint32_t k_ctime, k_ctime_nsec;
    uint64_t k_ino;
} __attribute__((packed));

typedef char kstat_is_96_bytes[sizeof(struct kstat) == 96 ? 1 : -1];
typedef int32_t ksec_t;
#define SYS_FSTATAT SYS_fstatat64
#endif

/* The statx flags a caller may pass that change nothing here. */
#define AT_STATX_FORCE_SYNC 0x2000
#define AT_STATX_DONT_SYNC  0x4000

/** Report one failed check and return 1, the probe's failure status. */
static int fail(const char *what)
{
    printf("STATXMUSL FAIL (%s: errno %d)\n", what, errno);
    return 1;
}

/** The raw statx of `path` from `dirfd` with `flags`, into `sx`. */
static long raw_statx(int dirfd, const char *path, int flags, unsigned mask, struct statx *sx)
{
    memset(sx, 0xA5, sizeof(*sx));
    return syscall(SYS_statx, dirfd, path, flags, mask, sx);
}

/** Whether statx and fstatat64 agree on every field the two share. */
static int same(const struct statx *sx, const struct kstat *k)
{
    return (sx->stx_mask & STATX_BASIC_STATS) == STATX_BASIC_STATS &&
           sx->stx_mode == (uint16_t)k->k_mode && sx->stx_ino == k->k_ino &&
           sx->stx_nlink == k->k_nlink && sx->stx_uid == k->k_uid && sx->stx_gid == k->k_gid &&
           (int64_t)sx->stx_size == k->k_size && sx->stx_blocks == k->k_blocks &&
           sx->stx_blksize == k->k_blksize &&
           makedev(sx->stx_dev_major, sx->stx_dev_minor) == k->k_dev &&
           makedev(sx->stx_rdev_major, sx->stx_rdev_minor) == k->k_rdev &&
           sx->stx_atime.tv_sec == (ksec_t)k->k_atime && sx->stx_atime.tv_nsec == k->k_atime_nsec &&
           sx->stx_mtime.tv_sec == (ksec_t)k->k_mtime && sx->stx_mtime.tv_nsec == k->k_mtime_nsec &&
           sx->stx_ctime.tv_sec == (ksec_t)k->k_ctime && sx->stx_ctime.tv_nsec == k->k_ctime_nsec;
}

/**
 * Ask both calls about `path` from `dirfd` with `flags`, require them to
 * agree, and leave statx's answer in `sx`. Returns 0 or the failure status.
 */
static int agree(int dirfd, const char *path, int flags, struct statx *sx, const char *what)
{
    struct kstat k;

    if (raw_statx(dirfd, path, flags, STATX_BASIC_STATS, sx) != 0) {
        return fail(what);
    }
    memset(&k, 0x5A, sizeof(k));
    if (syscall(SYS_FSTATAT, dirfd, path, &k,
                flags & ~(AT_STATX_FORCE_SYNC | AT_STATX_DONT_SYNC)) != 0 ||
        !same(sx, &k)) {
        return fail(what);
    }
    return 0;
}

/** Whether the raw statx of `path` with `flags` and `mask` fails with `err`. */
static int refused(const char *path, int flags, unsigned mask, int err)
{
    struct statx sx;

    return raw_statx(AT_FDCWD, path, flags, mask, &sx) == -1 && errno == err;
}

static int kinds(void)
{
    struct statx sx, tmp;
    int fds[2];
    int fd;

    fd = open(FILE_PATH, O_WRONLY | O_CREAT | O_TRUNC, 0640);
    if (fd < 0 || write(fd, "stats", 5) != 5 || close(fd) != 0) {
        return fail("the file");
    }
    if (agree(AT_FDCWD, FILE_PATH, 0, &sx, "a regular file") || !S_ISREG(sx.stx_mode) ||
        sx.stx_size != 5 || (sx.stx_mode & 07777) != 0640) {
        return fail("a regular file's attributes");
    }
    if (agree(AT_FDCWD, "/tmp", 0, &tmp, "a directory") || !S_ISDIR(tmp.stx_mode)) {
        return fail("a directory's type");
    }
    unlink(FIFO_PATH);
    if (mkfifo(FIFO_PATH, 0600) != 0 || agree(AT_FDCWD, FIFO_PATH, 0, &sx, "a FIFO") ||
        !S_ISFIFO(sx.stx_mode)) {
        return fail("a FIFO's type");
    }
    if (agree(AT_FDCWD, "/dev/null", 0, &sx, "/dev/null") || !S_ISCHR(sx.stx_mode) ||
        sx.stx_rdev_major != 1 || sx.stx_rdev_minor != 3) {
        return fail("/dev/null's numbers");
    }
    if (agree(AT_FDCWD, "/proc/self", AT_SYMLINK_NOFOLLOW, &sx, "/proc/self unfollowed") ||
        !S_ISLNK(sx.stx_mode) || agree(AT_FDCWD, "/proc/self", 0, &sx, "/proc/self") ||
        !S_ISDIR(sx.stx_mode)) {
        return fail("/proc/self followed and not");
    }
    if (pipe(fds) != 0 || agree(fds[0], "", AT_EMPTY_PATH, &sx, "a pipe") ||
        !S_ISFIFO(sx.stx_mode)) {
        return fail("a pipe by its descriptor");
    }
    close(fds[0]);
    close(fds[1]);
    if (chdir("/tmp") != 0 || agree(AT_FDCWD, "", AT_EMPTY_PATH, &sx, "the working directory") ||
        sx.stx_ino != tmp.stx_ino) {
        return fail("the working directory by an empty path");
    }
    if (agree(AT_FDCWD, "statxmusl.f", AT_STATX_DONT_SYNC, &sx, "a sync flag") ||
        sx.stx_size != 5) {
        return fail("a relative path with a sync flag");
    }
    return 0;
}

int main(void)
{
    if (kinds()) {
        return 1;
    }
    if (!refused(FILE_PATH, 0x1, STATX_BASIC_STATS, EINVAL) ||
        !refused(FILE_PATH, AT_STATX_FORCE_SYNC | AT_STATX_DONT_SYNC, STATX_BASIC_STATS, EINVAL) ||
        !refused(FILE_PATH, 0, 0x80000000u, EINVAL)) {
        return fail("a flag or mask statx does not take");
    }
    if (!refused("/tmp/statxmusl.none", 0, STATX_BASIC_STATS, ENOENT) ||
        !refused("", 0, STATX_BASIC_STATS, ENOENT)) {
        return fail("a missing file or an empty path");
    }
    unlink(FILE_PATH);
    unlink(FIFO_PATH);
    printf("STATXMUSL OK\n");
    return 0;
}
