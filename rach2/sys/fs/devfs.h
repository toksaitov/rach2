#ifndef _DEVFS_H_
#define _DEVFS_H_

#include "../lib/kint.h"

/*
 * devfs: a synthetic /dev. The device namespace is virtual (not KRFFS nodes): a
 * small registry maps a leaf name ("null", "zero", ...) to a character device
 * backed by a file_ops vtable and a {major,minor}. open() of a /dev path binds the
 * device's ops onto the descriptor (the ops dispatch then drives
 * read/write/close), exactly as pipes do; stat/getdents synthesize from the
 * registry. Two names ("console" and "tty") resolve to the kernel console and so
 * open as FD_TYPE_CONSOLE, reusing the existing terminal path.
 */

struct fd_entry;

/*
 * The synthetic device the namespace lives on. KRFFS files report st_dev 0, so a
 * distinct nonzero id keeps a device's identity from ever colliding with a real
 * file's; this matters because ttyname compares (st_dev, st_ino).
 */
#define DEVFS_FSID 5u

/** Open a /dev leaf by name; the new fd, or a negative errno. */
int devfs_open(const char *name, int flags);

/** Synthesize stat fields for /dev or a device leaf; 0 ok, -1 if no such leaf. */
int devfs_stat(const char *name, uint32_t *mode, uint32_t *rdev, uint32_t *ino);

/*
 * The encoded rdev (major<<8 | minor) and the stable inode of an open
 * FD_TYPE_DEVICE descriptor, for fstat64. Both return 0 if e is not a device.
 */
uint32_t devfs_rdev(struct fd_entry *e);
uint32_t devfs_fd_ino(struct fd_entry *e);

/* The leaf name ("null", ...) of an open device fd, for /proc/<pid>/fd readlink.
 * Returns "?" if e is not a devfs device. */
const char *devfs_fd_name(struct fd_entry *e);

/*
 * Whether lseek on this device returns offset 0 (the mem devices null/zero/full)
 * rather than -ESPIPE. Returns 1 for a seek-to-zero device, 0 otherwise.
 */
int devfs_lseek_zero(struct fd_entry *e);

/*
 * Number of devfs entries, and the leaf name of entry i (for getdents on /dev).
 * Returns NULL past the end.
 */
int devfs_count(void);
const char *devfs_name(int i);

#endif /* _DEVFS_H_ */
