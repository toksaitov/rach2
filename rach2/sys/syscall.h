#ifndef _SYSCALL_H_
#define _SYSCALL_H_

/* What the rest of the kernel calls in the system call layer. */

#include "irq.h"

/** Fill the dispatch table through `arch_syscall_init`, which routes the
 * architecture's system call entry to the dispatcher, and read
 * `rach2.trace=enosys` and `rach2.cycleaccount` (`sched_cycle_account`)
 * from the command line. */
void init_syscall(void);

/**
 * Whether the current thread is in a system call it made by i386's
 * convention, `int $0x80` on x86-64, as Linux's `in_compat_syscall`: a
 * handler shared by both tables asks it where the two ABIs' structures
 * differ. Always 0 on a kernel with one ABI.
 */
int in_compat_syscall(void);

/* The ABIs of `syscall_unlock`: the kernel's own table, and on x86-64 the
 * i386 table of a call SC_COMPAT marks. */
#define SYSCALL_ABI_NATIVE 0
#define SYSCALL_ABI_IA32   1

/**
 * Run the calls of `abi` that dispatch to `handler` without the big lock,
 * as Linux runs every call: the handler takes the locks of what it touches.
 * Every number the table gives `handler` is marked, so the call is made
 * once the table is filled: in `init_syscall`, or at the end of the
 * architecture's fill (`arch_syscall_init`, `ia32_syscall_init`). A
 * handler on no number, or an ABI this kernel does not have, marks nothing.
 */
void syscall_unlock(int abi, syscall_fn_t handler);

/**
 * Whether the system call in `params` takes the big lock at its entry:
 * nonzero unless its handler is marked (`syscall_unlock`) and the thread
 * has nothing at the entry that needs the lock, a tracer's stop, a seccomp
 * filter, descriptor holds left from before, or USB sticks to publish
 * (`usbdisk_pending`). A thread with none of these runs the call the
 * number names, so the judgment is that of the handler the call runs after
 * the tracer's entry stop. Interrupts are off.
 */
int syscall_entry_locked(const isr_params_t *params);

/**
 * Deliver what the loopback backlog holds, at a return to user mode whose
 * thread put a datagram on it (WORK_LOOP), under the big lock: the
 * return's own, or the lock waited for here, as Linux's sender runs the
 * softirq it raised: the drain runs over what the call put on the backlog
 * before the program runs again, unless its budgets end it first
 * (`ip_loop_drain_from`), which leaves the rest marked. The wait answers
 * other processors' requests (`bkl_acquire`). Interrupts are off.
 */
void syscall_loop_drain(void);

/**
 * ioctl(2) of the request `request` with the argument `arg` on the
 * descriptor `fd`, as `sys_ioctl` answers it from a frame. The compat
 * ioctl calls it with a translated argument, a kernel buffer the thread's
 * user-copy window covers (`user_window_open`). Returns what the request
 * returns, or -errno.
 */
long ioctl_fd(int fd, uint32_t request, uintptr_t arg);

struct krffs_node;

/**
 * Whether anything holds the KRFFS root node `node`: a descriptor, an end of a
 * FIFO, a descriptor in flight, a file mapping or the address space of a
 * program (`fd_node_holders`).
 *
 * The debug shell removes and renames through the file system directly, below
 * the system call layer, which keeps the rule that a held node is orphaned and
 * not freed, so it asks here and keeps no copy of the rule.
 */
int fs_node_is_held(const struct krffs_node *node);

/**
 * Move every reference to the KRFFS root node `from` to `to`, where the file
 * now is; nothing when `from` is NULL or is `to`. The debug shell writes
 * through the file system directly, and a write that grows a file past its
 * extent relocates it. The sweep over descriptors, descriptions, working
 * directories and roots, mounts, loop devices and locks is the VFS's
 * (`vfs_repoint`), the one place that knows every kind of holder.
 */
void fs_node_moved(const struct krffs_node *from, struct krffs_node *to);

/** The mount id and the inode number fstat gives descriptor `fd` of the
 * process `pid`, for its /proc/PID/fdinfo/N: 0, -ENOENT for a descriptor
 * that is not open, or the error of a mounted type's getattr, which may
 * sleep. The id is 0 for an object on no mount of the table: a pipe, a
 * socket or an anonymous descriptor. */
int32_t fd_stat_ids(int pid, int fd, uint32_t *mnt_id, uint64_t *ino);

struct fd_entry;
struct fd_table;

/**
 * A copy of descriptor `fd` of `table`, another process's or the caller's
 * own, into `*out`, taken under the table's lock with what a descriptor in
 * flight holds: a reference to its description and to a socket. The owner
 * may then close the descriptor, with or without the big lock, and what the
 * copy names stays until `fd_peek_drop`, as Linux's /proc holds the file
 * `fget_task` finds (`proc_fd_link`, fs/proc/fd.c:182) or `get_file` takes
 * under `file_lock` (`seq_show`, fs/proc/fd.c:39-46). 0, or -EBADF for a
 * descriptor that is not open.
 */
int fd_peek_hold(struct fd_table *table, int fd, struct fd_entry *out);

/** Give back what `fd_peek_hold` took with the copy `copy`, as the close of
 * a descriptor in flight does: the last holder of the description releases
 * its objects and ends it, as Linux's `fput` does. May sleep. */
void fd_peek_drop(struct fd_entry *copy);

/** The host and domain names of the caller's UTS namespace, which uname
 * gives as its node name and domain name, for /proc/sys/kernel/hostname and
 * domainname. */
const char *uts_hostname(void);
const char *uts_domainname(void);

/** Whether the caller may set them: privileged over its UTS namespace. */
int uts_privileged(void);

/* The fixed fields of uname(2), which /proc/version repeats. The system is
 * named Linux, whose interface the kernel follows, as config.guess, CMake and
 * scripts match the name exactly; the release is the version of the Linux
 * headers the GNU toolchain was built with, and Rach2 names itself in the
 * release's suffix and in the version. */
#define UTS_SYSNAME "Linux"
#define UTS_RELEASE "6.18.0-rach2"
#define UTS_VERSION "#1 Rach2 1.0.0"

/** Set the host name, or the domain name, to the `len` bytes of `name`,
 * at most 64, which need not end in a NUL; the caller has checked the
 * privilege. */
void uts_set_hostname(const char *name, uint32_t len);
void uts_set_domainname(const char *name, uint32_t len);

/* The calls only some architectures have, each defined in the directory of
 * one that has it: set_thread_area, i386's (arch/i686/tls.c, and on x86-64
 * arch/x86_64/tls.c for the compat table, with get_thread_area). */
long sys_set_thread_area(isr_params_t *params);
long sys_get_thread_area(isr_params_t *params);

#endif
