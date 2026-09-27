#ifndef _TMPFS_H_
#define _TMPFS_H_

#include "vfs.h"

/*
 * tmpfs: a file system whose files live in memory and go when it is
 * unmounted, as Linux's tmpfs. It has every kind of node: regular files,
 * directories, symbolic links, hard links, FIFOs, sockets and device nodes.
 * A regular file's bytes are a struct shmem (mm/shmem.h), which a shared
 * mapping maps page for page. /dev/shm is one, and memfd_create makes its
 * files on one of its own that no path reaches.
 */

extern const struct vfs_type vfs_tmpfs_type;

struct shmem;

/** The bytes of the regular file `node` of a tmpfs mount, for a shared
 * mapping of it; NULL for any other node. */
struct shmem *tmpfs_shmem(void *node);

#endif /* _TMPFS_H_ */
