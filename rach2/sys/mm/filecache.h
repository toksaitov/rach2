/*
 * filecache.h: the pages of ext2 files that private mappings and exec read,
 * kept in free frames between them (mm/Readme.md).
 */

#ifndef _FILECACHE_H_
#define _FILECACHE_H_

#include "../lib/kint.h"

struct vfs_mount;
struct fd_entry;

/** Copy page `pgoff` of the file `e` is open on into the frame `phys` when
 * the cache has it as the file is now: 1 then, and 0 when it has not. */
int filecache_read(const struct fd_entry *e, uint32_t pgoff, uintptr_t phys);

/** The same for page `pgoff` of the file `node` of `mnt`, into the kernel
 * buffer `buf` of a page, as exec reads a program. */
int filecache_read_node(struct vfs_mount *mnt, void *node, uint32_t pgoff, void *buf);

/** The count of the changes recorded so far (`filecache_forget`,
 * `filecache_forget_mount`), taken before a page is read for
 * `filecache_store`. */
uint32_t filecache_epoch(void);

/** Keep a copy of the frame `phys`, page `pgoff` of the file `e` is open on
 * as it was just read, while a quarter of the frames are free and no change
 * has been recorded since `filecache_epoch` gave `epoch`, since the read
 * might have seen part of one. Only an ext2 regular file's page is kept. */
void filecache_store(const struct fd_entry *e, uint32_t pgoff, uintptr_t phys, uint32_t epoch);

/** The same for page `pgoff` of the file `node` of `mnt`, from the kernel
 * buffer `buf` of a page. */
void filecache_store_node(struct vfs_mount *mnt, void *node, uint32_t pgoff, const void *buf,
                          uint32_t epoch);

/** Drop every page of the file `node` of `mnt`, which a write, a truncation
 * or an allocation in it has just changed. */
void filecache_forget(struct vfs_mount *mnt, void *node);

/** Drop every page of the files of `mnt`, at its unmount. */
void filecache_forget_mount(const struct vfs_mount *mnt);

/** The frames the cache holds, which `mem_reclaim` can have back. */
uint32_t filecache_pages(void);

/** Give back up to `want` frames, the least recently used first; the
 * number given back. */
uint32_t filecache_shrink(uint32_t want);

#endif /* _FILECACHE_H_ */
