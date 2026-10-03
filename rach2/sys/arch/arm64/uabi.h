#ifndef _ARM64_UABI_H_
#define _ARM64_UABI_H_

#include "../../lib/kint.h"

/*
 * The user structures whose layout no portable declaration gives, for
 * AArch64. Linux gives AArch64 the generic layouts, which differ from
 * x86-64's in struct stat; these are x86-64's, which an ARM64 port replaces
 * with its own. Until then they let the shared code compile.
 */

/* struct stat, of stat, lstat, fstat and newfstatat. */
struct user_stat {
    unsigned long st_dev;
    unsigned long st_ino;
    unsigned long st_nlink;
    unsigned int st_mode;
    unsigned int st_uid;
    unsigned int st_gid;
    unsigned int __pad0;
    unsigned long st_rdev;
    long st_size;
    long st_blksize;
    long st_blocks;
    unsigned long st_atime;
    unsigned long st_atime_nsec;
    unsigned long st_mtime;
    unsigned long st_mtime_nsec;
    unsigned long st_ctime;
    unsigned long st_ctime_nsec;
    long __unused[3];
};

#define STAT_SET_INO(st, v) ((st)->st_ino = (v))

/* struct statfs, of statfs and fstatfs. */
struct user_statfs {
    long f_type;
    long f_bsize;
    long f_blocks;
    long f_bfree;
    long f_bavail;
    long f_files;
    long f_ffree;
    uint32_t f_fsid[2];
    long f_namelen;
    long f_frsize;
    long f_flags;
    long f_spare[4];
};

/* The System V IPC records of IPC_STAT, whose times are whole longs. */
struct shmid64_ds {
    struct ipc64_perm perm;
    unsigned long segsz;
    long atime, dtime, ctime;
    int32_t cpid, lpid;
    unsigned long nattch;
    unsigned long unused4, unused5;
};

struct semid64_ds {
    struct ipc64_perm perm;
    long otime, ctime;
    unsigned long nsems;
    unsigned long unused3, unused4;
};

struct msqid64_ds {
    struct ipc64_perm perm;
    long stime, rtime, ctime;
    unsigned long cbytes, qnum, qbytes;
    int32_t lspid, lrpid;
    unsigned long unused4, unused5;
};

#define IPC_TIME_SET(ds, f, t) ((ds)->f = (long)(t))

/* The bytes of a siginfo_t through the fields of a child's report, si_signo,
 * si_errno, si_code, si_pid, si_uid and si_status, which waitid writes. */
#define SIGINFO_CHLD_BYTES 28

#endif /* _ARM64_UABI_H_ */
