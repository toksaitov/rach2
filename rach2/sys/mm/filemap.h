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

/** The pages in memory `f` maps when it is a shared mapping of a tmpfs
 * file, as `filemap_shared` made it; NULL for any other mapped file. */
struct shmem *filemap_shmem(const struct vm_file *f);

/**
 * The same for a shared mapping of a file on a disk: the mapping maps the
 * file's pages in memory, which every shared mapping of the file maps, read
 * from the file at a page's first touch and written back to it by
 * `filemap_sync_file`, by `msync`, and when a mapping through a descriptor
 * open for writing goes. The pages go with the file's last mapping.
 */
struct vm_file *filemap_disk_shared(const fd_entry_t *e);

/** Whether the file `ino` of `mnt` has its pages in memory for a shared
 * mapping, which a new shared mapping of it then maps too. */
int filemap_disk_mapped(const struct vfs_mount *mnt, uint64_t ino);

/*
 * What the calls on a file's descriptors tell the shared mappings of a disk
 * file, so that the file and its pages in memory agree: each does nothing
 * when no such mapping exists. A read writes the pages it covers back first
 * (`filemap_file_reading`), a write puts its bytes into the pages it covers
 * (`filemap_file_written`), a truncation frees the pages past the new end,
 * which it first takes out of every mapping (`filemap_file_truncated`), and
 * fsync writes the file's pages back (`filemap_sync_file`); sync writes back
 * every file's (`filemap_sync_all`). Each returns 0 or the first failed
 * write's error, where it has anything to return.
 */
int filemap_file_reading(const struct vfs_mount *mnt, uint64_t ino, uint64_t off, uint32_t len);
void filemap_file_written(fd_entry_t *e, uint64_t off, uint32_t len);
void filemap_file_truncated(const struct vfs_mount *mnt, uint64_t ino, uint64_t size);
int filemap_sync_file(const struct vfs_mount *mnt, uint64_t ino);
void filemap_sync_all(void);

/** msync of the mapping `f` of a region: its file's pages written back
 * when it maps a disk file's, and with `flush`, for MS_SYNC, the file
 * system's fsync of the file after them; 0 or the first failure. */
int filemap_msync(struct vm_file *f, int flush);

/**
 * Shared anonymous memory, for MAP_SHARED|MAP_ANONYMOUS: pages of its own
 * that every region mapping it, in this process and the children it forks,
 * maps; freed with the last region.
 */
struct vm_file *filemap_anon_shared(void);

/**
 * Take the pages of `sh` from its page `first` on out of every region that
 * maps them, in every address space, for a truncation that is about to free
 * them: a later access to one faults and maps the page the memory has then,
 * as it would after a first touch.
 */
void filemap_shmem_cut(struct shmem *sh, uint32_t first);

/** Whether a mapping holds a KRFFS file with the node `node`, as a
 * descriptor in a table does: the node the file is at now, which a
 * relocation moves in the mapping's open file description. */
int filemap_holds(const void *node);

/*
 * The file system's part, in syscall_fs.c: read `count` bytes of the file
 * `e` is open on from `off` into the kernel buffer `buf`, without moving the
 * descriptor's offset. Returns the bytes read, fewer at the end of the file,
 * or -errno. `fs_write_at` writes them from the kernel buffer the same way.
 */
int32_t fs_read_at(fd_entry_t *e, void *buf, uint32_t count, uint64_t off);
int32_t fs_write_at(fd_entry_t *e, const void *buf, uint32_t count, uint64_t off);

#endif /* _FILEMAP_H_ */
