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

struct fd_entry;
struct vfs_statfs;
/** Fill `st` with the statfs of the mqueue file system and return 1 when
 * `e` is the descriptor of a queue, as mq_open makes it; return 0 for any
 * other descriptor. */
int mq_fd_statfs(const struct fd_entry *e, struct vfs_statfs *st);

struct vfs_ref;
/** Fill `r` with the queue's file and a mount it is on, and return 1, when
 * `e` is the descriptor of a queue: the first mount of the mqueue type that
 * lists the queue's IPC namespace, or the first of any namespace when none
 * does, since a queue's inode number is unique over every namespace. Return
 * 0 for any other descriptor, or when no mqueue file system is mounted. The
 * node is held by the descriptor, not by `r`. */
int mq_fd_ref(const struct fd_entry *e, struct vfs_ref *r);

/** The name of the queue the descriptor `e` is open on, without its leading
 * slash, with `*unlinked` set to 1 once mq_unlink has taken it and to 0
 * before; NULL for any other descriptor. */
const char *mq_fd_name(const struct fd_entry *e, int *unlinked);

struct vfs_type;
/** The type of the mqueue file system, which lists the queues of the IPC
 * namespace that mounted it as the files of one directory. */
extern const struct vfs_type vfs_mqueue_type;

#endif /* _MQUEUE_H_ */
