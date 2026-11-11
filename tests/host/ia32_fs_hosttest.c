/**
 * ia32_fs_hosttest.c: the i386 file and time structures of
 * rach2/sys/arch/x86_64/ia32_abi.h, which the x86-64 kernel's compat calls
 * read and write, against Linux 6.12's: each structure's size and the offset
 * of each member Linux's compat code fills, and the joining of a 64-bit
 * argument that i386 splits over two registers or words.
 *
 * The rows hold Linux's numbers for i386. They were read from Linux 6.12's
 * headers, arch/x86/include/uapi/asm/stat.h, include/uapi/asm-generic/
 * statfs.h and fcntl.h, include/uapi/linux/uio.h and time_types.h, and from
 * the compat types of include/linux/compat.h, fs/readdir.c and
 * include/linux/time32.h with compat_ulong_t a 32-bit word, compiled by clang
 * for i386-linux-gnu with offsetof and sizeof. Every member of ia32_abi.h is
 * a word of 32 bits or fewer, so the host lays each structure out as i386
 * does, whatever its own ABI.
 */
#include "arch/x86_64/ia32_abi.h"

#include <stddef.h>

extern int printf(const char *format, ...);

/* A structure's size or a member's offset, and Linux's. */
struct row {
    const char *what;
    unsigned long found;
    unsigned long linux;
};

#define SIZE(t, n)   {#t, sizeof(struct t), n}
#define OFF(t, f, n) {#t "." #f, offsetof(struct t, f), n}

static const struct row rows[] = {
    /* compat_stat */
    SIZE(ia32_stat, 64),
    OFF(ia32_stat, st_ino, 4),
    OFF(ia32_stat, st_mode, 8),
    OFF(ia32_stat, st_uid, 12),
    OFF(ia32_stat, st_rdev, 16),
    OFF(ia32_stat, st_size, 20),
    OFF(ia32_stat, st_blksize, 24),
    OFF(ia32_stat, st_blocks, 28),
    OFF(ia32_stat, st_atime, 32),
    OFF(ia32_stat, st_mtime_nsec, 44),
    OFF(ia32_stat, st_ctime, 48),
    /* __old_kernel_stat */
    SIZE(ia32_old_stat, 32),
    OFF(ia32_old_stat, st_rdev, 12),
    OFF(ia32_old_stat, st_size, 16),
    OFF(ia32_old_stat, st_atime, 20),
    OFF(ia32_old_stat, st_ctime, 28),
    /* struct stat64 */
    SIZE(ia32_stat64, 96),
    OFF(ia32_stat64, st_ino32, 12),
    OFF(ia32_stat64, st_mode, 16),
    OFF(ia32_stat64, st_nlink, 20),
    OFF(ia32_stat64, st_uid, 24),
    OFF(ia32_stat64, st_rdev_lo, 32),
    OFF(ia32_stat64, st_size_lo, 44),
    OFF(ia32_stat64, st_blksize, 52),
    OFF(ia32_stat64, st_blocks_lo, 56),
    OFF(ia32_stat64, st_atime, 64),
    OFF(ia32_stat64, st_mtime, 72),
    OFF(ia32_stat64, st_ctime_nsec, 84),
    OFF(ia32_stat64, st_ino_lo, 88),
    /* compat_statfs */
    SIZE(ia32_statfs, 64),
    OFF(ia32_statfs, f_bsize, 4),
    OFF(ia32_statfs, f_blocks, 8),
    OFF(ia32_statfs, f_files, 20),
    OFF(ia32_statfs, f_fsid, 28),
    OFF(ia32_statfs, f_namelen, 36),
    OFF(ia32_statfs, f_frsize, 40),
    OFF(ia32_statfs, f_flags, 44),
    OFF(ia32_statfs, f_spare, 48),
    /* compat_statfs64 */
    SIZE(ia32_statfs64, 84),
    OFF(ia32_statfs64, f_bsize, 4),
    OFF(ia32_statfs64, f_blocks_lo, 8),
    OFF(ia32_statfs64, f_bfree_lo, 16),
    OFF(ia32_statfs64, f_bavail_lo, 24),
    OFF(ia32_statfs64, f_files_lo, 32),
    OFF(ia32_statfs64, f_ffree_lo, 40),
    OFF(ia32_statfs64, f_fsid, 48),
    OFF(ia32_statfs64, f_namelen, 56),
    OFF(ia32_statfs64, f_frsize, 60),
    OFF(ia32_statfs64, f_flags, 64),
    OFF(ia32_statfs64, f_spare, 68),
    /* compat_ustat */
    SIZE(ia32_ustat, 20),
    OFF(ia32_ustat, f_tinode, 4),
    OFF(ia32_ustat, f_fname, 8),
    OFF(ia32_ustat, f_fpack, 14),
    /* compat_flock and compat_flock64 */
    SIZE(ia32_flock, 16),
    OFF(ia32_flock, l_whence, 2),
    OFF(ia32_flock, l_start, 4),
    OFF(ia32_flock, l_len, 8),
    OFF(ia32_flock, l_pid, 12),
    SIZE(ia32_flock64, 24),
    OFF(ia32_flock64, l_whence, 2),
    OFF(ia32_flock64, l_start_lo, 4),
    OFF(ia32_flock64, l_len_lo, 12),
    OFF(ia32_flock64, l_pid, 20),
    /* The time structures of both ABIs */
    SIZE(ia32_timespec, 8),
    OFF(ia32_timespec, tv_nsec, 4),
    SIZE(ia32_timeval, 8),
    OFF(ia32_timeval, tv_usec, 4),
    SIZE(ia32_itimerspec, 16),
    OFF(ia32_itimerspec, it_value, 8),
    SIZE(ia32_itimerval, 16),
    OFF(ia32_itimerval, it_value, 8),
    SIZE(ia32_utimbuf, 8),
    OFF(ia32_utimbuf, modtime, 4),
    SIZE(ia32_timespec64, 16),
    OFF(ia32_timespec64, tv_nsec_lo, 8),
    /* compat_sigevent */
    SIZE(ia32_sigevent, 64),
    OFF(ia32_sigevent, sigev_signo, 4),
    OFF(ia32_sigevent, sigev_notify, 8),
    OFF(ia32_sigevent, sigev_tid, 12),
    /* compat_sel_arg_struct and pselect6's sixth argument */
    SIZE(ia32_sel_arg, 20),
    OFF(ia32_sel_arg, tvp, 16),
    SIZE(ia32_sigset_arg, 8),
    OFF(ia32_sigset_arg, ss_len, 4),
    /* compat_linux_dirent and compat_old_linux_dirent: the name */
    {"ia32 dirent name", IA32_DIRENT_NAME, 10},
    /* compat_iovec, which iov_fetch reads as two 32-bit words */
    {"ia32 iovec", 2 * sizeof(uint32_t), 8},
    {"ia32 iovec.iov_len", sizeof(uint32_t), 4},
};

/** Check that the 64-bit value of the words `lo` and `hi` is `want`. */
static int join(uint32_t lo, uint32_t hi, uint64_t want)
{
    uint64_t v = IA32_JOIN(lo, hi);

    if (v == want)
        return 0;
    printf("ia32_fs: join %08x %08x: %016llx, want %016llx\n", lo, hi, (unsigned long long)v,
           (unsigned long long)want);
    return 1;
}

int main(void)
{
    struct ia32_flock64 fl;
    struct ia32_stat64 st;
    size_t i;
    int bad = 0;

    for (i = 0; i < sizeof(rows) / sizeof(rows[0]); i++) {
        if (rows[i].found != rows[i].linux) {
            printf("ia32_fs: %s: %lu, Linux %lu\n", rows[i].what, rows[i].found, rows[i].linux);
            bad++;
        }
    }

    /* The low word comes first and neither word extends its sign: pread64's
     * ESI and EDI, ftruncate64's ECX and EDX, fadvise64_64's two pairs. */
    bad += join(0x89abcdefu, 0x01234567u, 0x0123456789abcdefull);
    bad += join(0xffffffffu, 0, 0xffffffffull);
    bad += join(0, 0x80000000u, 0x8000000000000000ull);
    bad += join(0xffffffffu, 0xffffffffu, ~0ull);
    if ((int64_t)IA32_JOIN(0xffffffffu, 0xffffffffu) != -1) {
        printf("ia32_fs: join of -1 is not -1 as a signed offset\n");
        bad++;
    }

    /* A packed 64-bit member is split and joined again whole. */
    IA32_PUT64(&fl, l_start, 0x0000000500000007ull);
    IA32_PUT64(&fl, l_len, (uint64_t)-2);
    if (fl.l_start_lo != 7 || fl.l_start_hi != 5 || IA32_GET64(&fl, l_start) != 0x500000007ull ||
        (int64_t)IA32_GET64(&fl, l_len) != -2) {
        printf("ia32_fs: flock64's split members do not round-trip\n");
        bad++;
    }
    IA32_PUT64(&st, st_ino, 0x123456789ull);
    if (st.st_ino_lo != 0x23456789u || st.st_ino_hi != 1) {
        printf("ia32_fs: stat64's inode number is not split low word first\n");
        bad++;
    }

    if (bad) {
        printf("ia32_fs: result: %d faults\n", bad);
        return 1;
    }
    printf("ia32_fs: result: %lu layout rows and the split arguments match Linux 6.12\n",
           (unsigned long)(sizeof(rows) / sizeof(rows[0])));
    return 0;
}
