#ifndef _X86_64_IA32_ABI_H_
#define _X86_64_IA32_ABI_H_

/*
 * The i386 layouts of the file and time structures an i386 program passes
 * the x86-64 kernel, as Linux 6.12's compat types have them (rach2/ia32.md
 * lists each, and tests/host/ia32_fs_hosttest.c checks their sizes and
 * offsets). i386 aligns a 64-bit integer to 4 bytes, which no x86-64
 * compiler does; each 64-bit member is therefore a pair of 32-bit words, low
 * word first, which IA32_GET64 and IA32_PUT64 join and split, so that each
 * structure has the same bytes on every compiler.
 */

#include "../../lib/kint.h"

/* The 64-bit value i386 passes in the registers or words `lo` and `hi`. */
#define IA32_JOIN(lo, hi) ((uint64_t)(uint32_t)(lo) | ((uint64_t)(uint32_t)(hi) << 32))

/* The 64-bit member `f` of an i386 structure, kept as `f`_lo and `f`_hi. */
#define IA32_GET64(s, f) IA32_JOIN((s)->f##_lo, (s)->f##_hi)
#define IA32_PUT64(s, f, v) \
    ((s)->f##_lo = (uint32_t)(uint64_t)(v), (s)->f##_hi = (uint32_t)((uint64_t)(v) >> 32))

/* old_timespec32 and old_timeval32: 32-bit seconds and nanoseconds or
 * microseconds. */
struct ia32_timespec {
    int32_t tv_sec;
    int32_t tv_nsec;
};

struct ia32_timeval {
    int32_t tv_sec;
    int32_t tv_usec;
};

/* old_itimerspec32 and old_itimerval32: the interval, then the value. */
struct ia32_itimerspec {
    struct ia32_timespec it_interval;
    struct ia32_timespec it_value;
};

struct ia32_itimerval {
    struct ia32_timeval it_interval;
    struct ia32_timeval it_value;
};

/* old_utimbuf32, of utime. */
struct ia32_utimbuf {
    int32_t actime;
    int32_t modtime;
};

/* __kernel_timespec, of the _time64 calls: 64-bit seconds and nanoseconds,
 * the high word of the nanoseconds padding that Linux ignores for an i386
 * caller (get_timespec64), since a C library may leave it unset. */
struct ia32_timespec64 {
    uint32_t tv_sec_lo, tv_sec_hi;
    uint32_t tv_nsec_lo, tv_nsec_hi;
};

/* compat_sigevent, of timer_create: a 4-byte sigval, then the signal, the
 * notification and the thread of SIGEV_THREAD_ID, in a union of 64 bytes. */
struct ia32_sigevent {
    uint32_t sigev_value;
    int32_t sigev_signo;
    int32_t sigev_notify;
    int32_t sigev_tid;
    uint32_t sigev_pad[12];
};

/* struct stat of i386's stat, lstat and fstat (106 to 108), compat_stat:
 * 16-bit mode, link count and ids, and a 32-bit size. */
struct ia32_stat {
    uint32_t st_dev;
    uint32_t st_ino;
    uint16_t st_mode;
    uint16_t st_nlink;
    uint16_t st_uid;
    uint16_t st_gid;
    uint32_t st_rdev;
    uint32_t st_size;
    uint32_t st_blksize;
    uint32_t st_blocks;
    uint32_t st_atime;
    uint32_t st_atime_nsec;
    uint32_t st_mtime;
    uint32_t st_mtime_nsec;
    uint32_t st_ctime;
    uint32_t st_ctime_nsec;
    uint32_t unused4;
    uint32_t unused5;
};

/* __old_kernel_stat of oldstat, oldlstat and oldfstat (18, 84, 28): 16-bit
 * device and inode numbers and ids, and the times in whole seconds. */
struct ia32_old_stat {
    uint16_t st_dev;
    uint16_t st_ino;
    uint16_t st_mode;
    uint16_t st_nlink;
    uint16_t st_uid;
    uint16_t st_gid;
    uint16_t st_rdev;
    uint16_t pad;
    uint32_t st_size;
    uint32_t st_atime;
    uint32_t st_mtime;
    uint32_t st_ctime;
};

/* i386's struct stat64, of stat64, lstat64, fstat64 and fstatat64: the
 * inode number twice, its low word at byte 12 for programs older than the
 * whole one at the end. */
struct ia32_stat64 {
    uint32_t st_dev_lo, st_dev_hi;
    uint32_t pad0;
    uint32_t st_ino32;
    uint32_t st_mode;
    uint32_t st_nlink;
    uint32_t st_uid;
    uint32_t st_gid;
    uint32_t st_rdev_lo, st_rdev_hi;
    uint32_t pad3;
    uint32_t st_size_lo, st_size_hi;
    uint32_t st_blksize;
    uint32_t st_blocks_lo, st_blocks_hi;
    uint32_t st_atime;
    uint32_t st_atime_nsec;
    uint32_t st_mtime;
    uint32_t st_mtime_nsec;
    uint32_t st_ctime;
    uint32_t st_ctime_nsec;
    uint32_t st_ino_lo, st_ino_hi;
};

/* compat_statfs, of statfs and fstatfs: every count in 32 bits. */
struct ia32_statfs {
    uint32_t f_type;
    uint32_t f_bsize;
    uint32_t f_blocks;
    uint32_t f_bfree;
    uint32_t f_bavail;
    uint32_t f_files;
    uint32_t f_ffree;
    uint32_t f_fsid[2];
    uint32_t f_namelen;
    uint32_t f_frsize;
    uint32_t f_flags;
    uint32_t f_spare[4];
};

/* compat_statfs64, of statfs64 and fstatfs64, packed to 4 bytes: 84. */
struct ia32_statfs64 {
    uint32_t f_type;
    uint32_t f_bsize;
    uint32_t f_blocks_lo, f_blocks_hi;
    uint32_t f_bfree_lo, f_bfree_hi;
    uint32_t f_bavail_lo, f_bavail_hi;
    uint32_t f_files_lo, f_files_hi;
    uint32_t f_ffree_lo, f_ffree_hi;
    uint32_t f_fsid[2];
    uint32_t f_namelen;
    uint32_t f_frsize;
    uint32_t f_flags;
    uint32_t f_spare[4];
};

/* compat_flock, of fcntl's F_GETLK, F_SETLK and F_SETLKW: 32-bit offsets. */
struct ia32_flock {
    int16_t l_type;
    int16_t l_whence;
    int32_t l_start;
    int32_t l_len;
    int32_t l_pid;
};

/* compat_flock64, of the 64-bit and open file description commands, packed
 * to 4 bytes: 24. */
struct ia32_flock64 {
    int16_t l_type;
    int16_t l_whence;
    uint32_t l_start_lo, l_start_hi;
    uint32_t l_len_lo, l_len_hi;
    int32_t l_pid;
};

/* The head of compat_linux_dirent, of getdents, whose name follows at byte
 * 10 and whose d_type is the record's last byte, and of
 * compat_old_linux_dirent, of readdir, whose name follows its length. */
#define IA32_DIRENT_NAME 10u

/* sel_arg_struct, of the old select (82): the five arguments. */
struct ia32_sel_arg {
    uint32_t n;
    uint32_t inp;
    uint32_t outp;
    uint32_t exp;
    uint32_t tvp;
};

/* The sixth argument of pselect6: the mask's address and its size. */
struct ia32_sigset_arg {
    uint32_t ss;
    uint32_t ss_len;
};

#endif /* _X86_64_IA32_ABI_H_ */
