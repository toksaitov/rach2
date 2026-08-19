#ifndef _PROCFS_H_
#define _PROCFS_H_

#include "../lib/kint.h"

struct vpath;
struct fd_entry;

/*
 * procfs: the synthetic /proc, generated per query from the process table.
 * Enough of it for what musl and busybox read: /proc/<pid>/stat and cmdline,
 * /proc/<pid>/fd/<n> magic symlinks (ttyname reads /proc/self/fd/N), and PID
 * directory enumeration (busybox ps). Nothing is stored on disk.
 */

/* st_dev for /proc nodes, distinct from KRFFS (0) and devfs (DEVFS_FSID). */
#define PROCFS_FSID 6u

/** Resolve a vpath pid (SYNTH_PID_SELF is the caller) to a live pid, or -1. */
int procfs_resolve_pid(int pid);

/** Open a /proc/<pid>/{stat,cmdline} file; returns the fd or -errno. */
int procfs_open(const struct vpath *vp, int flags);

/** Render the /proc/<pid>/fd/<fd> symlink target into buf (NOT NUL-terminated);
 *  returns the byte length or -errno. */
int procfs_readlink_fd(int pid, int fd, char *buf, int cap);

/** PID-directory enumeration: the nth live pid (n >= 0), or -1 past the end. */
int procfs_nth_pid(int n);

/* Stable synthetic inodes for stat. */
uint32_t procfs_ino_dir(void);
uint32_t procfs_ino_cmdline(void);

/** The inode of /proc/net and of /proc/net/dev. */
uint32_t procfs_ino_netdir(void);
uint32_t procfs_ino_netdev(void);
uint32_t procfs_ino_disk(void);
uint32_t procfs_fd_ino(struct fd_entry *e);
uint32_t procfs_ino_pid(int pid);

#endif /* _PROCFS_H_ */
