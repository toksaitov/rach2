/*
 * The files mmap maps. A mapping of a file keeps the file open as a
 * descriptor would, with a copy of the descriptor that holds its open file
 * description, for as long as a region maps it, so a program may close the
 * descriptor after mmap as it may on Linux. A page of a private mapping is
 * borrowed from tmpfs or ext2 until its first write, or read from another
 * file system when first touched; a page of a shared mapping is
 * the page of the file in memory, or of the shared anonymous memory, itself
 * (mm/paging.c).
 */
#ifndef _FILEMAP_H_
#define _FILEMAP_H_

#include "../lib/kint.h"
#include "../lib/kuser.h"
#include "../fs/fd.h"
#include "vm.h"

struct shmem;

/**
 * A mapped file for the open descriptor `e`, for a private mapping, with one
 * reference, which the caller puts once a region has taken its own; NULL
 * when the heap has no room.
 */
struct vm_file *filemap_new(const fd_entry_t *e);

/** A private mapping through `e`, borrowing `sh` for tmpfs, or with `disk`
 * the ext2 file's pages, until its first store makes a copy. Other file
 * systems use `filemap_new`.
 * Returns one reference, or NULL when the heap has no room. */
struct vm_file *filemap_private(const fd_entry_t *e, struct shmem *sh, int disk);

/**
 * The same for a shared mapping of the file in memory whose pages are `sh`
 * (a tmpfs file, `tmpfs_shmem`): the mapping maps the file's own pages.
 */
struct vm_file *filemap_shared(const fd_entry_t *e, struct shmem *sh);

/** The pages in memory `f` maps when it is a shared mapping of a tmpfs
 * file, as `filemap_shared` made it; NULL for any other mapped file. */
struct shmem *filemap_shmem(const struct vm_file *f);

/** The open file description the mapped file `f` keeps, whose mount and
 * inode number name the file; NULL for shared anonymous memory and for a
 * region object that is not a mapped file. */
const struct open_file *filemap_ofd(const struct vm_file *f);

/** The inode number of shared anonymous memory `f` on the mount
 * `vfs_memfd_mount` has (`filemap_anon_shared`); 0 for any other mapped
 * file and for a region object that is not one. */
uint64_t filemap_anon_ino(const struct vm_file *f);

/** The descriptor entry the mapped file `f` keeps, whose /proc link target
 * names the file; NULL for shared anonymous memory and for a region object
 * that is not a mapped file. It lives as long as a reference to `f`. */
const fd_entry_t *filemap_entry(const struct vm_file *f);

/** Whether the pages `f` maps are memory's own, with no file on a disk
 * behind them: a tmpfs file's, a memfd's, shared anonymous memory's. Linux
 * counts such pages in a process's RssShmem, and a disk file's in RssFile. */
int filemap_in_memory(const struct vm_file *f);

/**
 * The same for a shared mapping of a file on a disk: the mapping maps the
 * file's pages in memory, which every shared mapping of the file maps, read
 * from the file at a page's first touch and written back to it by
 * `filemap_sync_file`, by `msync`, by sync(2), by the flusher, and when the
 * last mapping through a descriptor open for writing goes. A page keeps a
 * dirty bit from a store's first fault until a write of it succeeds with no
 * such mapping left, and the pages go once the file has no mapping and no
 * dirty page: until then the last such mapping's descriptor stays the
 * pages' own, and every later write-back writes through it. A device that
 * has gone, a mount no namespace sees and an unmount drop what cannot be
 * written (`filemap_unmount`). NULL when the heap has no room.
 */
struct vm_file *filemap_disk_shared(const fd_entry_t *e);

/** Whether `f` is a shared mapping of a file on a disk, as
 * `filemap_disk_shared` made it, whose reference keeps the file's pages in
 * memory and the copy of its descriptor; 0 for NULL and any other file.
 * A short-term write pin and an i915 userptr pin hold it until the device
 * has finished (mm/pin.c, drm/i915/i915_gem_userptr.c). */
int filemap_disk_pages(const struct vm_file *f);

/** Whether the file `ino` of `mnt` has resident pages for any private or
 * shared mapping, or dirty pages its last mapping left, which descriptor
 * writes must update and fsync writes (`filemap_sync_file`). With or
 * without the big lock: asked under the file's lock (`vfs_node_lock`), a
 * no stays true until it is let go, since a page joins a file's pages only
 * at a fault, which takes that lock. */
int filemap_disk_mapped(const struct vfs_mount *mnt, uint64_t ino);

/** Whether the pages in memory of the disk file `ino` of `mnt` may hold
 * stores the file lacks: whether a shared mapping through a descriptor open
 * for writing has mapped it. A file that only private mappings or read-only
 * shared ones map has pages equal to the file, so a read that finds none
 * reads the file, and one that finds some takes the big lock to read the
 * pages (`filemap_disk_mapped`). Without the big lock. */
int filemap_disk_stores(const struct vfs_mount *mnt, uint64_t ino);

/** Whether any shared mapping holds this disk file's pages, or dirty
 * pages a shared mapping left keep them. Private mappings alone do not
 * prevent a loop device attaching the file. */
int filemap_disk_shared_mapped(const struct vfs_mount *mnt, uint64_t ino);

/**
 * Whether stores may reach the pages of `node`, a file of `mnt`, without a
 * write of the file: on tmpfs, a shared mapping through a descriptor open
 * for writing, which maps the file's own pages, or a seal against shrinking,
 * which /dev/udmabuf and the virtio shared-memory transport require before
 * they hand the pages to a device or a mapping and keep no record against
 * the file. 0 on any other type. A loop device does not attach such a file.
 * Under the big lock, as mmap makes its backing.
 */
int filemap_memory_shared_writable(const struct vfs_mount *mnt, void *node);

/** After a disk write, copy the `len` bytes at `off` from kernel buffer
 * `buf` into the resident clean pages of the file `ino` on `mnt`. The
 * caller holds the file's lock. Does not sleep or allocate. */
void filemap_disk_written(const struct vfs_mount *mnt, uint64_t ino, uint64_t off, const void *buf,
                          uint32_t len);

/*
 * The system calls on a file's descriptors call the five functions below,
 * so that the file and its resident mapped pages agree.
 * Each does nothing when the file has no resident-page owner, and each but
 * `filemap_file_truncated` may sleep.
 */

/**
 * Copy up to `count` bytes at `off` of the file `ino` of `mnt` to the user
 * buffer `dst` from the page in memory that holds `off`, as Linux's
 * filemap_read copies the page cache's folio, so that a read sees the
 * stores of shared mappings without writing them back. The copy stays
 * within that page; the caller has cut `count` at the file's end. Returns
 * the bytes copied; 0 when the page is not in memory, and the caller then
 * reads the file; or -EFAULT when the buffer cannot be written. A reference
 * on the page is held across the copy, which may sleep; the page is mapped
 * (`kmap_cpu`) only to copy each piece of at most 512 bytes into a buffer
 * on the stack, so no mapping is held across the user copy, whose fault may
 * wait for a file's lock.
 */
int32_t filemap_read_resident(const struct vfs_mount *mnt, uint64_t ino, uint64_t off,
                              char __user *dst, uint32_t count);

/** After a write of `len` bytes at `off` through `e`, put the bytes, read
 * back from the file, into the pages in memory the write covers, so that
 * every mapping sees them. */
void filemap_file_written(fd_entry_t *e, uint64_t off, uint32_t len);

/** After a truncation of the file `ino` of `mnt` to `size`, free the pages
 * in memory past the new end, which are first taken out of every mapping
 * (`shmem_truncate`). */
void filemap_file_truncated(const struct vfs_mount *mnt, uint64_t ino, uint64_t size);

/** For fsync of the file `ofd` is open on, write the file's dirty pages
 * back, those of mappings whose release is still queued among them; 0, or
 * the failure of this write-back. A failure, this one's or an earlier one's,
 * a failed last unmap's among them, is recorded once, only in the file
 * system's write-back errors, which the caller's fsync reports through the
 * description's cursor (`vfs_fsync`). A page whose write fails stays dirty
 * for the next write-back. Under the big lock. */
int filemap_sync_file(const struct open_file *ofd);

/** For sync and the flusher, write back the dirty pages of every file a
 * shared mapping maps or left dirty pages of; a failure is not answered here
 * but recorded once for each description's fsync and syncfs
 * (`vfs_wb_err_set`). */
void filemap_sync_all(void);

/** For the unmount of `mnt`, once its operations have drained: when the
 * only references left to it are those of descriptions only a file's
 * pages in memory hold, write those pages, drop what cannot be written, and
 * let the descriptions go, so that the unmount can proceed. Otherwise do
 * nothing. Under the big lock; this may sleep. */
void filemap_unmount(struct vfs_mount *mnt);

/** msync of the mapping `f` of a region: its file's pages written back
 * when it maps a disk file's, and with `flush`, for MS_SYNC, the file
 * system's fsync after them through `f`'s own description, whose cursor
 * into the write-back errors moves past what it answers, as Linux's msync
 * syncs the region's file (mm/msync.c:87); 0 or the first failure. A failed
 * page write is recorded for every description's sync and, being answered,
 * moves `f`'s description's cursor past it, with or without `flush`. */
int filemap_msync(struct vm_file *f, int flush);

/**
 * Shared anonymous memory, for MAP_SHARED|MAP_ANONYMOUS: pages of its own
 * that every region mapping it, in this process and the children it forks,
 * maps; freed with the last region. Its path is "/dev/zero (deleted)", the
 * name Linux's /proc/PID/maps gives it, or none when the heap has no room
 * for the name, and its device and inode those of a file of the mount
 * `vfs_memfd_mount` has, with an inode number of its own (`tmpfs_new_ino`),
 * as Linux's are of the shmem file shmem_zero_setup makes on shm_mnt. NULL
 * when the heap has no room. The caller holds the big lock.
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

/**
 * Read `count` bytes of the file `e` is open on from `off` into the kernel
 * buffer `buf`, without moving the descriptor's offset, for a file mapping.
 * Returns the bytes read, fewer at the end of the file, or a negative errno:
 * -ENODEV for a descriptor that is not open on a file. The mount's namespace
 * lock is taken where the type needs it, and not the description's lock,
 * since a read(2) into a mapping of the file being read faults here while
 * that lock is held. May sleep. The file system's part, in syscall_fs.c.
 */
int32_t fs_read_at(fd_entry_t *e, void *buf, uint32_t count, uint64_t off);

/** Write `count` bytes from the kernel buffer `buf` to the file `e` is open
 * on at `off`, as `fs_read_at` reads them, for the writeback of a shared
 * file mapping: the bytes written, or a negative errno. In syscall_fs.c. */
int32_t fs_write_at(fd_entry_t *e, const void *buf, uint32_t count, uint64_t off);

#endif /* _FILEMAP_H_ */
