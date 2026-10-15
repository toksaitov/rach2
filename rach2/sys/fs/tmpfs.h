#ifndef _TMPFS_H_
#define _TMPFS_H_

/*
 * tmpfs: a file system whose files live in memory and go when it is
 * unmounted, as Linux's tmpfs. It has every kind of node: regular files,
 * directories, symbolic links, hard links, FIFOs, sockets and device nodes.
 * A regular file's bytes are a struct shmem (mm/shmem.h), which a shared
 * mapping maps page for page. /dev/shm is one, and the files of
 * memfd_create and the segments of System V shared memory are on another,
 * which no path reaches.
 */

#include "vfs.h"

extern const struct vfs_type vfs_tmpfs_type;

struct shmem;

/** The bytes of the regular file `node` of a tmpfs mount, for a shared
 * mapping of it; NULL for any other node. */
struct shmem *tmpfs_shmem(void *node);

/* The seals of a tmpfs file, with the values of fcntl's F_SEAL_ bits, in
 * order: no seal added, no smaller size, no larger size, no write, no write
 * from now on, which here is the same as no write, and no change of the
 * execute bits. A file made in a directory starts with TMPFS_SEAL_SEAL
 * alone, and a memfd with the seals `tmpfs_memfd` is given. */
#define TMPFS_SEAL_SEAL         0x01u
#define TMPFS_SEAL_SHRINK       0x02u
#define TMPFS_SEAL_GROW         0x04u
#define TMPFS_SEAL_WRITE        0x08u
#define TMPFS_SEAL_FUTURE_WRITE 0x10u
#define TMPFS_SEAL_EXEC         0x20u
#define TMPFS_SEAL_ALL          0x3Fu

/**
 * A regular file of `mnt` that no directory names, for memfd_create and for
 * a System V shared memory segment: `name` is kept for /proc/PID/fd, `mode`
 * has its permission bits and `seals` its first seals. The node comes back
 * in `*node` with one hold, and goes with its last: it has no name that
 * would keep it. 0, or -ENOMEM.
 */
int tmpfs_memfd(struct vfs_mount *mnt, const char *name, uint32_t mode, uint32_t seals,
                void **node);

/** The seals of the tmpfs file `node`. They do not reach a shared mapping:
 * mmap(2) maps a file with TMPFS_SEAL_WRITE or TMPFS_SEAL_FUTURE_WRITE
 * writable and shared, and a mapping made before the seal goes on writing,
 * where Linux fails the mapping with EPERM and the seal with EBUSY. */
uint32_t tmpfs_seals(void *node);

/**
 * Add `seals` to the tmpfs file `node`, as F_ADD_SEALS does: 0, -EINVAL for
 * a bit that is no seal, or -EPERM when the file has TMPFS_SEAL_SEAL. A
 * file with execute bits that is sealed against their change is sealed
 * against a change of size and against writing too, as on Linux.
 */
int tmpfs_add_seals(void *node, uint32_t seals);

/** The name `tmpfs_memfd` was given for the file `node`, or NULL for a file
 * made in a directory. */
const char *tmpfs_label(void *node);

#endif /* _TMPFS_H_ */
