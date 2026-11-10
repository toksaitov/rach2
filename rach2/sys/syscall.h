#ifndef _SYSCALL_H_
#define _SYSCALL_H_

/* What the rest of the kernel calls in the system call layer. */

#include "irq.h"

/** Fill the dispatch table through `arch_syscall_init`, which routes the
 * architecture's system call entry to the dispatcher, and read
 * `rach2.trace=enosys` from the command line. */
void init_syscall(void);

/**
 * Whether the current thread is in a system call it made by i386's
 * convention, `int $0x80` on x86-64, as Linux's `in_compat_syscall`: a
 * handler shared by both tables asks it where the two ABIs' structures
 * differ. Always 0 on a kernel with one ABI.
 */
int in_compat_syscall(void);

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
