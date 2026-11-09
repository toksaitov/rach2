/*
 * ipc32.h: the System V IPC and POSIX message queue records of an i386
 * program, in words of 32 bits or fewer, as Linux 6.12's i386 and compat
 * types lay them out (rach2/ia32.md lists each, and
 * tests/host/ia32_ipc_hosttest.c checks their sizes and offsets).
 */

#ifndef _IPC32_H_
#define _IPC32_H_

#include "../lib/kint.h"

/*
 * The old records, which a control command without IPC_64 reads and writes
 * through ipc(2), on i686 and from an i386 program on x86-64: 16-bit ids,
 * mode and pids, 32-bit times. Linux's ipc_perm, semid_ds, shmid_ds,
 * msqid_ds and shminfo on i386, and its compat_ forms of them.
 */
struct ipc32_perm {
    int32_t key;
    uint16_t uid, gid, cuid, cgid;
    uint16_t mode;
    uint16_t seq;
};

struct ipc32_semid_ds {
    struct ipc32_perm perm;
    int32_t otime, ctime;
    uint32_t base, pending, pending_last, undo; /* kernel pointers, 0 */
    uint16_t nsems;
    uint16_t pad;
};

struct ipc32_shmid_ds {
    struct ipc32_perm perm;
    int32_t segsz;
    int32_t atime, dtime, ctime;
    uint16_t cpid, lpid;
    uint16_t nattch, unused;
    uint32_t unused2, unused3;
};

/* `cbytes`, `qnum` and `qbytes` are 16 bits; `lcbytes` and `lqbytes` are
 * the whole byte counts, which i386's own kernel fills and its IPC_SET
 * reads when `qbytes` is 0, and which Linux's compat call leaves 0. */
struct ipc32_msqid_ds {
    struct ipc32_perm perm;
    uint32_t first, last; /* kernel pointers, 0 */
    int32_t stime, rtime, ctime;
    uint32_t lcbytes, lqbytes;
    uint16_t cbytes, qnum, qbytes;
    uint16_t lspid, lrpid;
    uint16_t pad;
};

struct ipc32_shminfo {
    int32_t shmmax, shmmin, shmmni, shmseg, shmall;
};

/*
 * The IPC_64 records of an i386 program on x86-64, Linux's compat_ipc64_perm
 * and the compat_*64_ds: each time's high word beside its low one, the
 * counts in 32 bits. On i686 they are the native records (arch/i686/uabi.h).
 */
struct ipc32_perm64 {
    int32_t key;
    uint32_t uid, gid, cuid, cgid;
    uint16_t mode, pad1;
    uint16_t seq, pad2;
    uint32_t unused1, unused2;
};

struct ipc32_semid64_ds {
    struct ipc32_perm64 perm;
    uint32_t otime, otime_high, ctime, ctime_high;
    uint32_t nsems;
    uint32_t unused3, unused4;
};

struct ipc32_shmid64_ds {
    struct ipc32_perm64 perm;
    uint32_t segsz;
    uint32_t atime, atime_high, dtime, dtime_high, ctime, ctime_high;
    int32_t cpid, lpid;
    uint32_t nattch;
    uint32_t unused4, unused5;
};

struct ipc32_msqid64_ds {
    struct ipc32_perm64 perm;
    uint32_t stime, stime_high, rtime, rtime_high, ctime, ctime_high;
    uint32_t cbytes, qnum, qbytes;
    int32_t lspid, lrpid;
    uint32_t unused4, unused5;
};

/* IPC_INFO's shminfo64 and SHM_INFO's shm_info, compat_shminfo64 and
 * compat_shm_info. */
struct ipc32_shminfo64 {
    uint32_t shmmax, shmmin, shmmni, shmseg, shmall;
    uint32_t unused[4];
};

struct ipc32_shm_info {
    int32_t used_ids;
    uint32_t shm_tot, shm_rss, shm_swp, swap_attempts, swap_successes;
};

/* The head of msgsnd's and msgrcv's message, compat_msgbuf, whose text
 * follows the 32-bit type, and ipc(2)'s MSGRCV pair of version 0,
 * compat_ipc_kludge. */
struct ipc32_msgbuf {
    int32_t mtype;
};

struct ipc32_kludge {
    uint32_t msgp;
    int32_t msgtyp;
};

/* compat_mq_attr, of mq_open and mq_getsetattr. */
struct ipc32_mq_attr {
    int32_t flags, maxmsg, msgsize, curmsgs;
    int32_t reserved[4];
};

#endif /* _IPC32_H_ */
