/**
 * statxmusl: check that statx agrees with fstatat on every kind of file.
 *
 * On i686 both GNU libc and musl call statx first for every stat, since its
 * times are 64 bits wide. Each row asks the raw statx and the raw fstatat64
 * (newfstatat on x86-64) about one object and requires them to agree: the
 * type and permission bits, the inode, the link count, the owner, the size,
 * the blocks, the three times, and the device numbers, which statx splits
 * into major and minor numbers that makedev joins again. The objects are a
 * regular file, a directory, a FIFO, /dev/null (major 1, minor 3), /proc/self
 * followed and not, a pipe named by its descriptor with AT_EMPTY_PATH, and
 * the working directory named by AT_FDCWD and an empty path, and on x86-64 a
 * file whose times are 2^31 seconds, which statx reports whole. The mask
 * reports the basic set. A pipe and sockets have the attributes Linux's
 * pipefs and sockfs give them: a pipe mode 0600, its maker's ids and the
 * time it was made, a socket mode 0777 and the epoch, and fchmod, fchown
 * and futimens change them for both ends. Then the mount id: a file in
 * /tmp, a /proc file, /dev/null and the root each report STATX_MNT_ID, by
 * path and by an O_PATH descriptor, with the id their /proc/self/fdinfo/N
 * gives and that /proc/self/mountinfo gives the mount the path is on. Then
 * the handles:
 * name_to_handle_at of the file with no room answers EOVERFLOW with the
 * size and the mount id, and with room the twelve bytes of the inode;
 * a /proc file has a handle only with AT_HANDLE_FID; an unknown flag is
 * EINVAL; and open_by_handle_at, for root, opens the file again by its
 * handle and answers ESTALE for a handle of another type, and for any
 * other user EPERM. Then the errors: a
 * flag statx does not take, both sync flags at once, the reserved mask bit,
 * a missing file, and an empty path without AT_EMPTY_PATH.
 *
 * Prints `STATXMUSL OK` and exits 0 on success, and prints
 * `STATXMUSL FAIL (detail)` and exits 1 otherwise. etude's report
 * (probe_report.h), for the case ETUDE_CASE names, has an assertion for
 * each row in order: `file`, `wide-time` on x86-64 alone, `directory`,
 * `fifo`, `dev-null`, `proc-self`, `pipe`, `pipe-attrs`, `cwd`, `sync-flag`,
 * `mount-id`, `handle`, `refused-flags` and `refused-paths`; the branch of
 * the group `arch`, `x86-64` or `i686`, says whether `wide-time` ran. The
 * first failure fails its assertion with the row and errno and leaves the
 * rest not run.
 */

#define _GNU_SOURCE
#include <errno.h>
#include <fcntl.h>
#include <stdint.h>
#include <stdio.h>
#include <string.h>
#include <sys/socket.h>
#include <sys/stat.h>
#include <sys/syscall.h>
#include <sys/sysmacros.h>
#include <unistd.h>

#include "../probe_report.h"

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

/* The mask bit of the mount id, and the id's place in struct statx, the
 * first of the words musl's header leaves as padding. */
#define STATX_MNT_ID_BIT 0x1000u
#define STX_MNT_ID(sx)   ((sx)->__pad1[0])

/* name_to_handle_at's flag for a handle that only identifies the file, and
 * its struct file_handle with room for the largest handle. */
#define AT_HANDLE_FID_BIT 0x200
struct handle {
    unsigned int handle_bytes;
    int handle_type;
    unsigned char f_handle[128];
};

/** Report one failed check and return 1, the probe's failure status. */
static int fail(const char *what)
{
    char why[96];

    snprintf(why, sizeof(why), "%s: errno %d", what, errno);
    printf("STATXMUSL FAIL (%s)\n", why);
    report_stop(why);
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

/**
 * Write a regular file and check that both calls agree on it and report its
 * type, size and mode, and on x86-64 a time of 2^31 seconds. Returns 0, or 1
 * once the failure is reported.
 */
static int regular_file(void)
{
    struct statx sx;
    int fd;

    fd = open(FILE_PATH, O_WRONLY | O_CREAT | O_TRUNC, 0640);
    if (fd < 0 || write(fd, "stats", 5) != 5 || close(fd) != 0) {
        return fail("the file");
    }
    if (agree(AT_FDCWD, FILE_PATH, 0, &sx, "a regular file") || !S_ISREG(sx.stx_mode) ||
        sx.stx_size != 5 || (sx.stx_mode & 07777) != 0640) {
        return fail("a regular file's attributes");
    }
    report_held();
#if defined __x86_64__
    {
        struct timespec past[2] = {{(time_t)1 << 31, 0}, {(time_t)1 << 31, 0}};

        if (utimensat(AT_FDCWD, FILE_PATH, past, 0) != 0 ||
            agree(AT_FDCWD, FILE_PATH, 0, &sx, "a file of times past 2^31") ||
            sx.stx_mtime.tv_sec != (int64_t)1 << 31) {
            return fail("statx of a time past 2^31 seconds");
        }
    }
    report_held();
#endif
    return 0;
}

/**
 * Check the attributes of sockets as Linux's sockfs gives them: an AF_INET
 * and an AF_UNIX socket are S_IFSOCK and 0777, the caller's, with times of
 * 0, and take fchmod. Returns 0, or 1 after printing the failure.
 */
static int socket_attrs(void)
{
    struct stat st;
    int pair[2], s;

    s = socket(AF_INET, SOCK_DGRAM, 0);
    if (s < 0 || fstat(s, &st) != 0 || st.st_mode != (S_IFSOCK | 0777) || st.st_mtime != 0 ||
        st.st_uid != geteuid() || fchmod(s, 0700) != 0 || fstat(s, &st) != 0 ||
        st.st_mode != (S_IFSOCK | 0700)) {
        return fail("an AF_INET socket's mode, owner and time");
    }
    close(s);
    if (socketpair(AF_UNIX, SOCK_STREAM, 0, pair) != 0 || fstat(pair[0], &st) != 0 ||
        st.st_mode != (S_IFSOCK | 0777) || fchmod(pair[0], 0700) != 0 || fstat(pair[0], &st) != 0 ||
        st.st_mode != (S_IFSOCK | 0700)) {
        return fail("an AF_UNIX socket's mode");
    }
    close(pair[0]);
    close(pair[1]);
    return 0;
}

/**
 * Check the attributes of a pipe as Linux's pipefs gives them: a new pipe
 * is S_IFIFO and 0600, owned by the caller's ids and stamped with the time
 * it was made; fchmod of one end shows at the other, and for root fchown
 * and futimens set what the other end's fstat gives; then the sockets'
 * (`socket_attrs`). Returns 0, or 1 after printing the failure.
 */
static int pipe_attrs(void)
{
    struct timespec times[2] = {{5, 0}, {6, 0}};
    struct stat st;
    int fds[2];

    if (pipe(fds) != 0 || fstat(fds[0], &st) != 0 || st.st_mode != (S_IFIFO | 0600) ||
        st.st_uid != geteuid() || st.st_gid != getegid() || st.st_mtime == 0) {
        return fail("a new pipe's mode, owner and time");
    }
    if (fchmod(fds[0], 0640) != 0 || fstat(fds[1], &st) != 0 || st.st_mode != (S_IFIFO | 0640)) {
        return fail("fchmod of a pipe");
    }
    if (geteuid() == 0 && (fchown(fds[0], 1000, 1001) != 0 || futimens(fds[0], times) != 0 ||
                           fstat(fds[1], &st) != 0 || st.st_uid != 1000 || st.st_gid != 1001 ||
                           st.st_atime != 5 || st.st_mtime != 6)) {
        return fail("fchown and futimens of a pipe");
    }
    close(fds[0]);
    close(fds[1]);
    return socket_attrs();
}

/**
 * Check each kind of file with both calls: a regular file, a directory, a
 * FIFO, a device, a symbolic link followed and not, a pipe and the working
 * directory by an empty path, and a relative path with a sync flag. Leaves
 * the working directory at /tmp. Returns 0, or 1 once the failure is
 * reported.
 */
static int kinds(void)
{
    struct statx sx, tmp;
    int fds[2];

    if (regular_file() != 0) {
        return 1;
    }
    if (agree(AT_FDCWD, "/tmp", 0, &tmp, "a directory") || !S_ISDIR(tmp.stx_mode)) {
        return fail("a directory's type");
    }
    report_held();
    unlink(FIFO_PATH);
    if (mkfifo(FIFO_PATH, 0600) != 0 || agree(AT_FDCWD, FIFO_PATH, 0, &sx, "a FIFO") ||
        !S_ISFIFO(sx.stx_mode)) {
        return fail("a FIFO's type");
    }
    report_held();
    if (agree(AT_FDCWD, "/dev/null", 0, &sx, "/dev/null") || !S_ISCHR(sx.stx_mode) ||
        sx.stx_rdev_major != 1 || sx.stx_rdev_minor != 3) {
        return fail("/dev/null's numbers");
    }
    report_held();
    if (agree(AT_FDCWD, "/proc/self", AT_SYMLINK_NOFOLLOW, &sx, "/proc/self unfollowed") ||
        !S_ISLNK(sx.stx_mode) || agree(AT_FDCWD, "/proc/self", 0, &sx, "/proc/self") ||
        !S_ISDIR(sx.stx_mode)) {
        return fail("/proc/self followed and not");
    }
    report_held();
    if (pipe(fds) != 0 || agree(fds[0], "", AT_EMPTY_PATH, &sx, "a pipe") ||
        !S_ISFIFO(sx.stx_mode)) {
        return fail("a pipe by its descriptor");
    }
    report_held();
    close(fds[0]);
    close(fds[1]);
    if (pipe_attrs() != 0) {
        return 1;
    }
    report_held();
    if (chdir("/tmp") != 0 || agree(AT_FDCWD, "", AT_EMPTY_PATH, &sx, "the working directory") ||
        sx.stx_ino != tmp.stx_ino) {
        return fail("the working directory by an empty path");
    }
    report_held();
    if (agree(AT_FDCWD, "statxmusl.f", AT_STATX_DONT_SYNC, &sx, "a sync flag") ||
        sx.stx_size != 5) {
        return fail("a relative path with a sync flag");
    }
    report_held();
    return 0;
}

/** The id /proc/self/mountinfo gives the mount `path` is on: that of the
 * longest mount point that is a prefix of it, the last of equal ones, which
 * is mounted over the others; 0 for none. */
static unsigned long long mountinfo_id(const char *path)
{
    FILE *f = fopen("/proc/self/mountinfo", "r");
    char line[512], point[256];
    unsigned long long id, best = 0;
    size_t n, best_len = 0;

    if (f == NULL) {
        return 0;
    }
    while (fgets(line, sizeof(line), f) != NULL) {
        if (sscanf(line, "%llu %*u %*s %*s %255s", &id, point) != 2) {
            continue;
        }
        n = strlen(point);
        if (strncmp(path, point, n) == 0 &&
            (point[n - 1] == '/' || path[n] == '/' || path[n] == '\0') && n >= best_len) {
            best = id;
            best_len = n;
        }
    }
    fclose(f);
    return best;
}

/** The mnt_id line of /proc/self/fdinfo/`fd`, or 0 for none. */
static unsigned long long fdinfo_id(int fd)
{
    char path[64], line[128];
    unsigned long long id = 0;
    FILE *f;

    snprintf(path, sizeof(path), "/proc/self/fdinfo/%d", fd);
    f = fopen(path, "r");
    if (f == NULL) {
        return 0;
    }
    while (fgets(line, sizeof(line), f) != NULL) {
        if (sscanf(line, "mnt_id: %llu", &id) == 1) {
            break;
        }
    }
    fclose(f);
    return id;
}

/** Check the mount id of a file of /tmp, of /proc, of /dev and of the root
 * against fdinfo and mountinfo. Returns 0, or 1 once the failure is
 * reported. */
static int mount_ids(void)
{
    static const char *const paths[] = {FILE_PATH, "/proc/self/status", "/dev/null", "/"};
    struct statx by_path, by_fd;
    unsigned i;

    for (i = 0; i < sizeof(paths) / sizeof(paths[0]); i++) {
        unsigned long long want = mountinfo_id(paths[i]);
        int fd = open(paths[i], O_PATH);

        if (fd < 0 || raw_statx(AT_FDCWD, paths[i], 0, STATX_MNT_ID_BIT, &by_path) != 0 ||
            raw_statx(fd, "", AT_EMPTY_PATH, STATX_MNT_ID_BIT, &by_fd) != 0) {
            return fail(paths[i]);
        }
        printf("statxmusl: %s: statx %llu, by descriptor %llu, fdinfo %llu, mountinfo %llu\n",
               paths[i], (unsigned long long)STX_MNT_ID(&by_path),
               (unsigned long long)STX_MNT_ID(&by_fd), fdinfo_id(fd), want);
        if (!(by_path.stx_mask & STATX_MNT_ID_BIT) || !(by_fd.stx_mask & STATX_MNT_ID_BIT) ||
            want == 0 || STX_MNT_ID(&by_path) != want || STX_MNT_ID(&by_fd) != want ||
            fdinfo_id(fd) != want) {
            close(fd);
            errno = 0;
            return fail(paths[i]);
        }
        close(fd);
    }
    report_held();
    return 0;
}

/** The raw name_to_handle_at of `path` with `flags` and `room` bytes of
 * handle, into `h` and `*mnt`. */
static long to_handle(const char *path, int flags, unsigned room, struct handle *h, int *mnt)
{
    memset(h, 0xA5, sizeof(*h));
    h->handle_bytes = room;
    *mnt = -1;
    return syscall(SYS_name_to_handle_at, AT_FDCWD, path, h, mnt, flags);
}

/** Check name_to_handle_at on FILE_PATH, whose stat is `st`, leaving its
 * handle in `h`, and on a /proc file. Returns 0, or 1 once the failure is
 * reported. */
static int handle_names(const struct stat *st, struct handle *h)
{
    unsigned long long want = mountinfo_id(FILE_PATH);
    struct handle other;
    int mnt;

    if (to_handle(FILE_PATH, 0, 0, h, &mnt) != -1 || errno != EOVERFLOW || h->handle_bytes != 12 ||
        (unsigned long long)mnt != want) {
        return fail("a handle with no room");
    }
    if (to_handle(FILE_PATH, 0, 128, h, &mnt) != 0 || h->handle_bytes != 12 ||
        h->handle_type != 0x51 || (unsigned long long)mnt != want ||
        memcmp(h->f_handle, &st->st_ino, sizeof(st->st_ino)) != 0) {
        return fail("a file's handle");
    }
    if (to_handle("/proc/self/status", 0, 128, &other, &mnt) != -1 || errno != EOPNOTSUPP ||
        to_handle("/proc/self/status", AT_HANDLE_FID_BIT, 128, &other, &mnt) != 0 ||
        (unsigned long long)mnt != mountinfo_id("/proc/self/status")) {
        return fail("a /proc file's handle");
    }
    if (to_handle(FILE_PATH, 0x8, 128, &other, &mnt) != -1 || errno != EINVAL) {
        return fail("an unknown flag");
    }
    return 0;
}

/** Check name_to_handle_at and open_by_handle_at on FILE_PATH and a /proc
 * file. Returns 0, or 1 once the failure is reported. */
static int handles(void)
{
    struct handle h, other;
    struct stat st, again;
    char text[8];
    int fd;

    if (stat(FILE_PATH, &st) != 0) {
        return fail("the file's stat");
    }
    if (handle_names(&st, &h) != 0) {
        return 1;
    }
    fd = (int)syscall(SYS_open_by_handle_at, AT_FDCWD, &h, O_RDONLY);
    if (geteuid() != 0) {
        if (fd != -1 || errno != EPERM) {
            return fail("open_by_handle_at of an unprivileged caller");
        }
        report_held();
        return 0;
    }
    if (fd < 0 || fstat(fd, &again) != 0 || again.st_ino != st.st_ino ||
        read(fd, text, sizeof(text)) != 5 || memcmp(text, "stats", 5) != 0) {
        return fail("a file opened by its handle");
    }
    close(fd);
    other = h;
    other.handle_type = 1;
    if (syscall(SYS_open_by_handle_at, AT_FDCWD, &other, O_RDONLY) != -1 || errno != ESTALE) {
        return fail("a handle of another type");
    }
    report_held();
    return 0;
}

/** Begin the report, with the branch of the architecture, and plan its
 * assertions. */
static void plan(void)
{
    static const char *const ids[] = {"file",          "wide-time",    "directory", "fifo",
                                      "dev-null",      "proc-self",    "pipe",      "pipe-attrs",
                                      "cwd",           "sync-flag",    "mount-id",  "handle",
                                      "refused-flags", "refused-paths"};
    unsigned i;

    report_begin(report_case(environ, "guest.default.statxmusl"));
#if defined __x86_64__
    report_branch("arch", "x86-64");
#else
    report_branch("arch", "i686");
#endif
    for (i = 0; i < sizeof(ids) / sizeof(ids[0]); i++) {
#if !defined __x86_64__
        if (strcmp(ids[i], "wide-time") == 0) {
            continue;
        }
#endif
        report_plan(ids[i]);
    }
}

int main(void)
{
    plan();
    if (kinds() || mount_ids() || handles()) {
        return 1;
    }
    if (!refused(FILE_PATH, 0x1, STATX_BASIC_STATS, EINVAL) ||
        !refused(FILE_PATH, AT_STATX_FORCE_SYNC | AT_STATX_DONT_SYNC, STATX_BASIC_STATS, EINVAL) ||
        !refused(FILE_PATH, 0, 0x80000000u, EINVAL)) {
        return fail("a flag or mask statx does not take");
    }
    report_held();
    if (!refused("/tmp/statxmusl.none", 0, STATX_BASIC_STATS, ENOENT) ||
        !refused("", 0, STATX_BASIC_STATS, ENOENT)) {
        return fail("a missing file or an empty path");
    }
    report_held();
    report_end();
    unlink(FILE_PATH);
    unlink(FIFO_PATH);
    printf("STATXMUSL OK\n");
    return 0;
}
