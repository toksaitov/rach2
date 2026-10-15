#ifndef _SYSCALL_H_
#define _SYSCALL_H_

/* What the rest of the kernel calls in the system call layer. */

#include "irq.h"

/** Fill the dispatch table through `arch_syscall_init`, which routes the
 * architecture's system call entry to the dispatcher, and read
 * `rach2.trace=enosys` from the command line. */
void init_syscall(void);

/** Give up the framebuffer and restore text mode if the thread group `pid`
 * owns it, as the end of the process does. */
void fb_release_if_owner(uint32_t pid);

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
 * one that has it: set_thread_area, i386's (arch/i686/tls.c). */
long sys_set_thread_area(isr_params_t *params);

#endif
