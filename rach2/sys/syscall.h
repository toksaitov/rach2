#ifndef _SYSCALL_H_
#define _SYSCALL_H_

#include "irq.h"

/**
 * Register the syscall handler and populate the dispatch table.
 */
void init_syscall(void);

/** Restore text mode if `pid` owns the framebuffer. */
void fb_release_if_owner(uint32_t pid);

/* Whether any open descriptor holds this KRFFS node. The debug shell asks
   before removing or renaming, since it goes below the syscall layer that owns the
   rule that a held node is orphaned, not freed. */
struct krffs_node;
int fs_node_is_held(const struct krffs_node *node);
void fs_node_moved(const struct krffs_node *from, struct krffs_node *to);

/** The host and domain names of the caller's UTS namespace, which uname
 * gives as its node name and domain name, for /proc/sys/kernel/hostname and
 * domainname. */
const char *uts_hostname(void);
const char *uts_domainname(void);

/** Whether the caller may set them: privileged over its UTS namespace. */
int uts_privileged(void);

/* The fixed fields of uname(2), which /proc/version repeats. The system is
   named Linux, whose interface the kernel follows, as config.guess, CMake and
   scripts match the name exactly; the release is the version of the Linux
   headers the GNU toolchain was built with, and Rach2 names itself in the
   release's suffix and in the version. */
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
