#ifndef _PROCFS_H_
#define _PROCFS_H_

#include "../lib/kint.h"

struct vpath;

/*
 * procfs -- the synthetic /proc, generated per query from the process table.
 * Enough of it for what musl and busybox read: /proc/<pid>/stat and cmdline,
 * /proc/<pid>/fd/<n> magic symlinks (ttyname reads /proc/self/fd/N), and PID
 * directory enumeration (busybox ps). Nothing is stored on disk.
 */

/* st_dev for /proc nodes, distinct from KRFFS (0) and devfs (DEVFS_FSID). */
#define PROCFS_FSID 6u

/* Resolve a vpath pid (SYNTH_PID_SELF -> the caller) to a live pid, or -1. */
int procfs_resolve_pid(int pid);

/*
 * Open a /proc/<pid>/{stat,cmdline} file: snapshots the text into a pooled buffer
 * and binds it to a descriptor (read serves the snapshot). Returns the fd or
 * -errno. vp carries the (already-classified) pid and which file.
 */
int procfs_open(const struct vpath *vp, int flags);

/* The readlink target of /proc/<pid>/fd/<fd> into buf (NOT NUL-terminated);
 * returns the byte length or -errno. */
int procfs_readlink_fd(int pid, int fd, char *buf, int cap);

/* PID-directory enumeration: the nth live pid (n >= 0), or -1 past the end. */
int procfs_nth_pid(int n);

/* Stable synthetic inodes for stat. */
uint32_t procfs_ino_dir(void);
uint32_t procfs_ino_pid(int pid);

#endif /* _PROCFS_H_ */
