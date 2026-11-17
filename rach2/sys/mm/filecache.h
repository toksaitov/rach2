/*
 * filecache.h: the pages of ext2 files that read(2), private mappings and
 * exec read, kept in free frames between them (mm/Readme.md).
 */

#ifndef _FILECACHE_H_
#define _FILECACHE_H_

#include "../lib/kint.h"
#include "../lib/kuser.h"

struct vfs_mount;
struct fd_entry;
struct open_file;

/** Make the tables that find a file's pages, sized for the frames the machine
 * has. The boot calls it once, after `init_paging`; when the heap cannot give
 * them, it logs a warning and the cache keeps no page. */
void init_filecache(void);

/** Copy page `pgoff` of the file `e` is open on into the frame `phys` when
 * the cache has it as the file is now: 1 then, and 0 when it has not. */
int filecache_read(const struct fd_entry *e, uint32_t pgoff, uintptr_t phys);

/** The same for page `pgoff` of the file `node` of `mnt`, into the kernel
 * buffer `buf` of a page, as exec reads a program. */
int filecache_read_node(struct vfs_mount *mnt, void *node, uint32_t pgoff, void *buf);

/**
 * Lend the frame of each page `first + i`, for `i` below `count`, of the
 * file `e` is open on that the cache has as the file is now and whose
 * `phys[i]` is 0, in `phys[i]` with a reference for the caller, leaving the
 * other entries as they are. The frame stays the cache's too, so its holder
 * changes it only as a write or a truncation of the file changes the file,
 * which then drops the pages it changed from the cache (`filecache_changed`).
 * One look at the file serves the whole range. Returns the frames lent.
 */
uint32_t filecache_lend(const struct fd_entry *e, uint32_t first, uint32_t count, uintptr_t *phys);

/** The count of the changes recorded so far of the file `node` of `mnt`
 * and of the few files that share its counter (`filecache_forget`,
 * `filecache_forget_mount`), taken before a page is read for
 * `filecache_store`. */
uint32_t filecache_epoch(struct vfs_mount *mnt, void *node);

/**
 * Read up to `count` bytes at `offset` of the file `ofd` is open on into the
 * user buffer `buf` through the cache, as read(2) does: each page the cache
 * has as the file is now is copied from it, and a page it lacks is read
 * with the window the description's readahead gives (`ofd->ra`), each page
 * into a new frame, which joins the cache locked until the disk answers;
 * the reader submits each window's reads itself without waiting for them,
 * reaching a marked page submits the next window so, and reaching a locked
 * page waits until its read ends. Returns 0, with nothing done, for a
 * file the cache does not keep, which the caller reads itself; 1 with `*got`
 * the bytes read, 0 at or past the end of the file, -EFAULT for a
 * buffer that takes no byte, or the file system's error. Before each page
 * after the first a nonzero `cut(ofd)`, when `cut` is not NULL, ends the
 * read with the pages before it, since the caller must read that page some
 * other way. The caller holds the description's lock, and may sleep.
 */
int filecache_file_read(struct open_file *ofd, char __user *buf, uint32_t count, uint64_t offset,
                        int32_t *got, int (*cut)(const struct open_file *ofd));

/**
 * Submit, in the caller's own context, the reads of the pages of the `len`
 * bytes at `offset` of the file `ofd` is open on, all of them for a `len`
 * of 0, into pages put in the cache locked until their reads end, and
 * return without waiting for the disk, as Linux's POSIX_FADV_WILLNEED and
 * readahead(2) start
 * their reads (`force_page_cache_ra`, mm/readahead.c:327-353): at most what
 * one request to the device moves or the description's window, whichever
 * is larger, cut at the file's end. A file the cache does not keep is left
 * alone. The caller may sleep.
 */
void filecache_willneed(struct open_file *ofd, uint64_t offset, uint64_t len);

/**
 * Drop the pages the cache has of the whole pages of the `len` bytes at
 * `offset` of the file `ofd` is open on, and of a last partial page that
 * ends the file, as Linux's POSIX_FADV_DONTNEED drops clean pages
 * (mm/fadvise.c:113-167); every page here is clean, since a write drops
 * its file's pages.
 */
void filecache_dontneed(struct open_file *ofd, uint64_t offset, uint64_t len);

/** Keep a copy of the frame `phys`, page `pgoff` of the file `e` is open on
 * as it was just read, unless a change of the file has been recorded since
 * `filecache_epoch` gave `epoch`, since the read might have seen part of
 * one. Only an ext2 regular file's page is kept. A new frame is taken while
 * a quarter of the frames are free and the records fit in an eighth of the
 * heap; past that the least recently used page's frame is used. */
void filecache_store(const struct fd_entry *e, uint32_t pgoff, uintptr_t phys, uint32_t epoch);

/** Keep the frame `phys` itself, with a reference of the cache's, as page
 * `pgoff` of the file `e` is open on as it was just read, as
 * `filecache_store` keeps a copy; its holder changes it only as
 * `filecache_lend` allows. */
void filecache_keep(const struct fd_entry *e, uint32_t pgoff, uintptr_t phys, uint32_t epoch);

/** Whether the cache keeps pages of the file `e` is open on: 1 for an ext2
 * regular file, 0 for any other. */
int filecache_keeps(const struct fd_entry *e);

/** The same for page `pgoff` of the file `node` of `mnt`, from the kernel
 * buffer `buf` of a page. */
void filecache_store_node(struct vfs_mount *mnt, void *node, uint32_t pgoff, const void *buf,
                          uint32_t epoch);

/** Drop every page of the file `node` of `mnt`, which an allocation or a
 * change of its size has just changed. May reschedule between batches. */
void filecache_forget(struct vfs_mount *mnt, void *node);

/* What a file was before a write or a truncation (`filecache_before`). */
struct filecache_was {
    uint64_t size;
    int64_t mtime;
    uint32_t generation;
    int known; /* 0: not a file whose pages the cache keeps */
};

/** Note in `*was` what the file `node` of `mnt` is now, before a write or a
 * truncation changes it (`filecache_changed`). */
void filecache_before(struct vfs_mount *mnt, void *node, struct filecache_was *was);

/**
 * Drop the pages of the file `node` of `mnt` that hold bytes `from` up to
 * `to`, `to` excluded, which a write or a truncation has just changed, and
 * keep its others, as Linux's page cache keeps the folios a write does not
 * touch. The pages kept were read from the file as `*was`, from
 * `filecache_before`, has it: when the cache's record of the file says
 * otherwise, the file changed some other way too, and every page goes,
 * with a chance to reschedule between batches.
 */
void filecache_changed(struct vfs_mount *mnt, void *node, const struct filecache_was *was,
                       uint64_t from, uint64_t to);

/** Drop every page of the file numbered `ino` on `mnt`, as
 * `filecache_forget` does, without asking the file system. May reschedule
 * between batches. */
void filecache_forget_ino(const struct vfs_mount *mnt, uint64_t ino);

/** Drop every page of the files of `mnt` after the VFS sets VFS_UNMOUNTING
 * and drains its users. May reschedule between batches. */
void filecache_forget_mount(const struct vfs_mount *mnt);

/** The frames the cache holds, which `mem_reclaim` can have back. */
uint32_t filecache_pages(void);

/** Drop pages, the least recently used first, until `want` frames have gone
 * back to the pool or the cache is empty. Returns the frames freed, which
 * leaves out a frame a reader still holds. */
uint32_t filecache_shrink(uint32_t want);

#endif /* _FILECACHE_H_ */
