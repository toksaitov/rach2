/*
 * sysv.h: System V interprocess communication, shared memory segments,
 * semaphore sets and message queues, as Linux gives them (ipc/Readme.md).
 */

#ifndef _SYSV_H_
#define _SYSV_H_

#include "../lib/kint.h"

struct isr_params;
struct tgroup;

/* The three kinds of object, in the order /proc/sysvipc lists its files. */
#define SYSV_SHM 0
#define SYSV_SEM 1
#define SYSV_MSG 2

/* One object as /proc/sysvipc reports it, with the ids and pids as the
 * reader sees them. `a` to `c` are a segment's size, creator and last
 * operator, a set's count of semaphores (`a` alone), and a queue's bytes,
 * messages and last sender; `d` is a segment's attachments and `e` a
 * queue's last receiver. A size past 32 bits, which a segment on x86-64 may
 * have, is 0xFFFFFFFF. `t0` to `t2` are a segment's attach, detach and
 * change times, a set's operation and change times, and a queue's send,
 * receive and change times, in seconds since the epoch, 0 for never. */
struct sysv_row {
    int32_t key;
    int32_t id;
    uint32_t mode;
    uint32_t uid, gid, cuid, cgid;
    uint32_t a, b, c, d, e;
    int64_t t0, t1, t2;
};

/* The handlers of the System V calls, which arch_syscall_init enters in the
 * table. i686 registers every one; x86-64 has no ipc, and makes its semop a
 * semtimedop with no time limit and its semtimedop sys_semtimedop_time64,
 * since a timespec there is 64-bit (arch/x86_64/systab.c). */
long sys_ipc(struct isr_params *params);
long sys_semget(struct isr_params *params);
long sys_semctl(struct isr_params *params);
long sys_shmget(struct isr_params *params);
long sys_shmctl(struct isr_params *params);
long sys_shmat(struct isr_params *params);
long sys_shmdt(struct isr_params *params);
long sys_msgget(struct isr_params *params);
long sys_msgsnd(struct isr_params *params);
long sys_msgrcv(struct isr_params *params);
long sys_msgctl(struct isr_params *params);
long sys_semtimedop_time64(struct isr_params *params);

/**
 * Apply and drop the SEM_UNDO adjustments of `tg`, whose last thread is
 * ending, as Linux does at exit: each value moves by its adjustment, kept
 * within 0 and SEMVMX, and the set's waiters are woken. Called by that
 * thread, in whose IPC namespace the sets are found; the adjustments of a
 * set that is gone are dropped.
 */
void sysv_exit(struct tgroup *tg);

/**
 * Fill `row` with the object in place `i` of `kind`'s table, as the
 * caller's IPC namespace sees it; 0 when the place is empty or has an
 * object of another namespace, 1 when it has one of the caller's, -1 past
 * the table's end. A call for place 0 of the segments first frees each
 * removed segment that has no attachment left.
 */
int sysv_row_at(int kind, int i, struct sysv_row *row);

#endif /* _SYSV_H_ */
