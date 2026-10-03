#ifndef _ARM_UABI_H_
#define _ARM_UABI_H_

#include "../../lib/kint.h"

/*
 * The user structures whose layout no portable declaration gives, for 32-bit
 * ARM: the declarations of Linux's ARM headers, which match i386's field for
 * field, the compiler aligning each 64-bit field as the EABI wants. An ARM
 * port checks them against its headers; until then they let the shared code
 * compile.
 */

/* struct stat64, of stat64, lstat64, fstat64 and fstatat64: the inode number
 * twice, its low word at byte 12 for programs older than the full one. */
struct user_stat {
    unsigned long long st_dev;
    unsigned char __pad0[4];
    unsigned long __st_ino;
    unsigned int st_mode;
    unsigned int st_nlink;
    unsigned long st_uid;
    unsigned long st_gid;
    unsigned long long st_rdev;
    unsigned char __pad3[4];
    long long st_size;
    unsigned long st_blksize;
    unsigned long long st_blocks;
    unsigned long st_atime;
    unsigned long st_atime_nsec;
    unsigned long st_mtime;
    unsigned long st_mtime_nsec;
    unsigned long st_ctime;
    unsigned long st_ctime_nsec;
    unsigned long long st_ino;
};

#define STAT_SET_INO(st, v) ((st)->st_ino = (v), (st)->__st_ino = (unsigned long)(v))

/* The old struct stat of stat (106), lstat (107) and fstat (108), which
 * i386 has beside stat64 and x86-64 does not. */
#define ARCH_WANT_OLD_STAT 1

struct linux_stat {
    unsigned long st_dev;
    unsigned long st_ino;
    unsigned short st_mode;
    unsigned short st_nlink;
    unsigned short st_uid;
    unsigned short st_gid;
    unsigned long st_rdev;
    unsigned long st_size;
    unsigned long st_blksize;
    unsigned long st_blocks;
    unsigned long st_atime;
    unsigned long st_atime_nsec;
    unsigned long st_mtime;
    unsigned long st_mtime_nsec;
    unsigned long st_ctime;
    unsigned long st_ctime_nsec;
    unsigned long __unused4;
    unsigned long __unused5;
};

/* struct statfs64, of statfs64 and fstatfs64. */
struct user_statfs {
    uint32_t f_type;
    uint32_t f_bsize;
    uint64_t f_blocks;
    uint64_t f_bfree;
    uint64_t f_bavail;
    uint64_t f_files;
    uint64_t f_ffree;
    uint32_t f_fsid[2];
    uint32_t f_namelen;
    uint32_t f_frsize;
    uint32_t f_flags;
    uint32_t f_spare[4];
};

/* The System V IPC records of IPC_STAT, whose times keep their high words
 * in fields of their own. */
struct shmid64_ds {
    struct ipc64_perm perm;
    unsigned long segsz;
    unsigned long atime, atime_high, dtime, dtime_high, ctime, ctime_high;
    int32_t cpid, lpid;
    unsigned long nattch;
    unsigned long unused4, unused5;
};

struct semid64_ds {
    struct ipc64_perm perm;
    unsigned long otime, otime_high, ctime, ctime_high;
    unsigned long nsems;
    unsigned long unused3, unused4;
};

struct msqid64_ds {
    struct ipc64_perm perm;
    unsigned long stime, stime_high, rtime, rtime_high, ctime, ctime_high;
    unsigned long cbytes, qnum, qbytes;
    int32_t lspid, lrpid;
    unsigned long unused4, unused5;
};

#define IPC_TIME_SET(ds, f, t)               \
    ((ds)->f = (unsigned long)(uint32_t)(t), \
     (ds)->f##_high = (unsigned long)(uint32_t)((uint64_t)(t) >> 32))

/* The bytes of a siginfo_t through the fields of a child's report, si_signo,
 * si_errno, si_code, si_pid, si_uid and si_status, which waitid writes. */
#define SIGINFO_CHLD_BYTES 24

#endif /* _ARM_UABI_H_ */
