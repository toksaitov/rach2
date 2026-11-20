/*
 * mqueue.h: POSIX message queues and the mqueue file system, as Linux gives
 * them (ipc/Readme.md).
 */

#ifndef _MQUEUE_H_
#define _MQUEUE_H_

#include "../fs/fd.h"

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
/** Allocate a private mount with a sticky root of mode 01777 belonging to
 * the namespace creator. Return 0 or -ENOMEM. */
int mq_root_made(struct ipcns *ns);

/** Release the namespace's mount. Visible mounts and open descriptions
 * retain the queues they still refer to. */
void mq_ns_gone(const struct ipcns *ns);

/** File operations shared by mq_open and ordinary opens of queue inodes. */
extern const struct file_ops mq_ops;
/** Bind a queue inode to its reserved descriptor. Access mode 3 allows
 * neither message transfer. The opener supplies the close-on-exec bit. */
static __inline__ void mq_bind(fd_entry_t *e, unsigned acc, unsigned cloexec)
{
    e->type = FD_TYPE_FILE;
    e->ops = &mq_ops;
    e->flags = cloexec | fd_open_rights(acc);
}

/** Remove the caller's notification when any queue descriptor closes.
 * The caller holds the big lock and the description. */
void mq_flush(const struct fd_entry *e);

struct vfs_statfs;
/** Fill `st` with the statfs of the mqueue file system and return 1 when
 * `e` is the descriptor of a queue, as mq_open makes it; return 0 for any
 * other descriptor. */
int mq_fd_statfs(const struct fd_entry *e, struct vfs_statfs *st);

struct vfs_ref;
/** Borrow the inode and opening mount of a queue descriptor if r is set.
 * Return 1 for a queue and 0 for another kind of descriptor. */
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
