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

/* One object as /proc/sysvipc reports it. `a` to `c` are the counts and
 * pids of its kind: a segment's size, creator and last operator, a set's
 * semaphores, a queue's bytes, messages, last sender and last receiver;
 * `d` a segment's attachments and `e` a queue's last receiver; a size past
 * 32 bits, which a segment on x86-64 may have, is 0xFFFFFFFF. The times
 * are seconds since the epoch, 0 for never. */
struct sysv_row {
    int32_t key;
    int32_t id;
    uint32_t mode;
    uint32_t uid, gid, cuid, cgid;
    uint32_t a, b, c, d, e;
    int64_t t0, t1, t2;
};

/** ipc: the one call that reaches every object through
 * `call`'s low 16 bits, as the C library calls it on i386. */
long sys_ipc(struct isr_params *params);

/** The direct calls, numbers 393 to 402 and semtimedop_time64's 420. */
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

/** Apply and drop the semaphore adjustments of `tg`, a process whose last
 * thread has ended, as Linux does at exit for SEM_UNDO. */
void sysv_exit(struct tgroup *tg);

/** Fill `row` with the object in place `i` of `kind`'s table; 0 when the
 * place is empty, 1 when it has one, -1 past the table's end. */
int sysv_row_at(int kind, int i, struct sysv_row *row);

#endif /* _SYSV_H_ */
