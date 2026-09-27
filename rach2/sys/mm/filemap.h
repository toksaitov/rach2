#ifndef _FILEMAP_H_
#define _FILEMAP_H_

#include "../lib/kint.h"
#include "../fs/fd.h"
#include "vm.h"

/*
 * The files mmap maps. A mapping of a file keeps the file open as a
 * descriptor would, with a copy of the descriptor that holds its open file
 * description, for as long as a region maps it, so a program may close the
 * descriptor after mmap as it may on Linux. A page of a private mapping is
 * read from the file when it is first touched; a page of a shared mapping is
 * the page of the file in memory, or of the shared anonymous memory, itself
 * (mm/paging.c).
 */

struct shmem;

/**
 * A mapped file for the open descriptor `e`, for a private mapping, with one
 * reference, which the caller puts once a region has taken its own; NULL
 * when the heap has no room.
 */
struct vm_file *filemap_new(const fd_entry_t *e);

/**
 * The same for a shared mapping of the file in memory whose pages are `sh`
 * (a tmpfs file, `tmpfs_shmem`): the mapping maps the file's own pages.
 */
struct vm_file *filemap_shared(const fd_entry_t *e, struct shmem *sh);

/**
 * Shared anonymous memory, for MAP_SHARED|MAP_ANONYMOUS: pages of its own
 * that every region mapping it, in this process and the children it forks,
 * maps; freed with the last region.
 */
struct vm_file *filemap_anon_shared(void);

/** Whether a mapping holds a KRFFS file with the node `node`, as a
 * descriptor in a table does: the node the file is at now, which a
 * relocation moves in the mapping's open file description. */
int filemap_holds(const void *node);

/*
 * The file system's part, in syscall_fs.c: read `count` bytes of the file
 * `e` is open on from `off` into the kernel buffer `buf`, without moving the
 * descriptor's offset. Returns the bytes read, fewer at the end of the file,
 * or -errno.
 */
int32_t fs_read_at(fd_entry_t *e, void *buf, uint32_t count, uint64_t off);

#endif /* _FILEMAP_H_ */
