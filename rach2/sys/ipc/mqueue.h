/*
 * mqueue.h: POSIX message queues and the mqueue file system, as Linux gives
 * them (ipc/Readme.md).
 */

#ifndef _MQUEUE_H_
#define _MQUEUE_H_

#include "../lib/kint.h"

struct isr_params;

/* The handlers of mq_open, mq_unlink, mq_timedsend, mq_timedreceive,
 * mq_notify and mq_getsetattr, which arch_syscall_init enters in the table
 * on both architectures, and of i686's mq_timedsend_time64 and
 * mq_timedreceive_time64. The timespec mq_timedsend and mq_timedreceive
 * take is two longs, 32-bit on i686 and 64-bit on x86-64. */
long sys_mq_open(struct isr_params *params);
long sys_mq_unlink(struct isr_params *params);
long sys_mq_timedsend(struct isr_params *params);
long sys_mq_timedreceive(struct isr_params *params);
long sys_mq_notify(struct isr_params *params);
long sys_mq_getsetattr(struct isr_params *params);
long sys_mq_timedsend_time64(struct isr_params *params);
long sys_mq_timedreceive_time64(struct isr_params *params);

struct ipcns;
/** The end of the IPC namespace `ns`, from `ipc_ns_gone`: its queues lose
 * their names, as mq_unlink takes one, and each lives on while a
 * descriptor, a waiter or an open of its file has it. */
void mq_ns_gone(const struct ipcns *ns);

struct vfs_type;
/** The type of the mqueue file system, which lists the queues of the IPC
 * namespace that mounted it as the files of one directory. */
extern const struct vfs_type vfs_mqueue_type;

#endif /* _MQUEUE_H_ */
