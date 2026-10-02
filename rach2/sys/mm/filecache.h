/*
 * filecache.h: the pages of ext2 files that private mappings read, kept in
 * free frames between mappings (mm/Readme.md).
 */

#ifndef _FILECACHE_H_
#define _FILECACHE_H_

#include "../lib/kint.h"

struct vfs_mount;
struct fd_entry;

/** Copy page `pgoff` of the file `e` is open on into the frame `phys` when
 * the cache has it as the file is now: 1 then, and 0 when it has not. */
int filecache_read(const struct fd_entry *e, uint32_t pgoff, uintptr_t phys);

/** The count of the changes the cache has been told of, taken before a page
 * is read for `filecache_store`. */
uint32_t filecache_epoch(void);

/** Keep a copy of the frame `phys`, page `pgoff` of the file `e` is open on
 * as it was just read, while free memory allows and no change has been told
 * of since `filecache_epoch` gave `epoch`, which the read might have missed
 * part of. */
void filecache_store(const struct fd_entry *e, uint32_t pgoff, uintptr_t phys, uint32_t epoch);

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
