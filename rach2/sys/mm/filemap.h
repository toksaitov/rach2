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

/** Check mmap's access mode and execute permission for the descriptor `e`,
 * as Linux's do_mmap checks them before the file's own mmap runs: -EBADF
 * for an O_PATH descriptor, -EACCES for one not open for reading or for a
 * shared writer through one not open for writing, -EPERM for execute asked
 * of a file `filemap_noexec` names, and zero otherwise. */
int32_t filemap_access(const fd_entry_t *e, int shared, uint32_t prot);

/** Check mmap's access mode, execute permission (`filemap_access`) and
 * mapping operation. Return zero or the error to report before creating a
 * mapping: -ENODEV for a descriptor with neither its own mmap nor a file. */
int32_t filemap_check(const fd_entry_t *e, int shared, uint32_t prot);

/** Whether the file `e` is open on was reached through a noexec mount or
 * lies on a file system that forbids execution, so that a mapping of it may
 * never execute, as Linux's path_noexec: 1 or 0. The mount is the file's
 * (`vmnt`), that of the device node a device was opened through
 * (`dev_mnt`), or that of a synthetic node such as /dev/zero opened by
 * its path (`synth_mnt`), as Linux judges the open's f_path. */
int filemap_noexec(const fd_entry_t *e);

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
 * dirty bit from a store's fault until a write-back, which first takes
 * write access from every entry that maps the page, so that the next store
 * faults and sets the bit again, and each write-back writes only the pages
 * stored since their last write; a failed write keeps the bit, and so does
 * a write of a page a device holds. Every write-back writes through a
 * handle of the pages' own, opened on the file system that holds the file
 * at the first such mapping, which holds no mount, so that a bind the
 * mapping was made through may go while pages are dirty. The pages and the
 * handle go once the file has no mapping and no dirty page. A device that
 * has gone, and the unmount of the file system itself, drop what cannot be
 * written (`filemap_unmount`). NULL when the heap has no room or the handle
 * cannot be opened.
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

/** After a write of the file `ino` on `mnt` that the file system accepted,
 * copy the `len` bytes at `off` from the kernel buffer `buf` the write was
 * made from into the file's pages in memory, clean or dirty, setting no
 * dirty bit, since the file has the bytes: every byte a write puts in the
 * file is in the pages before the file's lock goes, so that no write-back
 * puts older bytes over it. The caller holds the file's lock and the big
 * lock, and made the write from `buf` under the same hold of the file's
 * lock. Does not sleep or allocate. */
void filemap_disk_written(const struct vfs_mount *mnt, uint64_t ino, uint64_t off, const void *buf,
                          uint32_t len);

/**
 * Mark page `pgoff` of the file `f` maps dirty, as the holder of a write
 * pin of its frame `phys` lets the pin go, before the frame's unpin, as
 * Linux's unpin_user_pages_dirty_lock dirties each page a device may have
 * written (mm/gup.c:285-325): a write-back that began while the device
 * still stored into the frame writes the page again on its next pass. Does
 * nothing unless `f` is a shared mapping of a disk file through a
 * description open for writing and `phys` is still the file's page there.
 * The caller holds `f` and the big lock; this does not sleep. A write pin
 * through `pin_user` needs no call, since a long-term write pin refuses
 * such a mapping.
 */
void filemap_pin_dirty(struct vm_file *f, uint32_t pgoff, uintptr_t phys);

/*
 * The system calls on a file's descriptors call the four functions below,
 * with `filemap_disk_written` above, so that the file and its resident
 * mapped pages agree.
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

/** After a truncation of the file `ino` of `mnt` to `size`, free the pages
 * in memory past the new end, which are first taken out of every mapping
 * (`shmem_truncate`), and clear their dirty bits. The caller holds the
 * file's lock and the big lock, on which the release of a device's write
 * pin relies to find the page still the file's (`filemap_pin_dirty`). */
void filemap_file_truncated(const struct vfs_mount *mnt, uint64_t ino, uint64_t size);

/** For fsync of the file `ofd` is open on, write the file's dirty pages
 * back, those of mappings whose release is still queued among them; 0, or
 * the errno of this write-back's first failure, which the caller returns, as
 * Linux's fsync returns its own write-back's error. A page whose write fails
 * stays dirty for the next write-back, and the failure is recorded in the
 * file's own error sequence and for syncfs (`vfs_ino_err_set`), which the
 * caller's check then moves `ofd`'s cursor past (`vfs_ino_err_check`), so
 * that each other description of the file reports it once and the fsync of
 * another file of the file system does not. Under the big lock; this may
 * sleep. */
int filemap_sync_file(const struct open_file *ofd);

/** For sync, the flusher and the shutdown, write back the dirty pages of
 * every file a shared mapping maps or left dirty pages of. A failure is not
 * answered here but recorded in the file's own error sequence and for
 * syncfs (`vfs_ino_err_set`), and the page stays dirty for a later
 * write-back. Under the big lock; this may sleep. */
void filemap_sync_all(void);

/** For a remount of `mnt`, a base mount, read-only, once new writers are
 * refused (`fd_mount_readonly_begin`): write back the dirty pages of every
 * file of it, as Linux's remount syncs the file system first
 * (fs/ext2/super.c:1334). 0, or the errno of the first failure, with the
 * pages that failed still dirty. The caller keeps the mount from going.
 * Under the big lock; this may sleep. */
int filemap_sync_mount(struct vfs_mount *mnt);

/** For the put of the node `node`, inode `ino` of the base mount `mnt`, of a
 * file with no name left, that leaves only the two holds of its record's
 * writer (`wr_handle`): drop the record's dirty pages and let the writer go,
 * as Linux's evict drops an unlinked inode's pages (truncate_inode_pages_final,
 * fs/inode.c:724), since nothing can read them any more and the writer would
 * keep the file's blocks until the unmount. Nothing is done while a mapping
 * of the file lives or a pass writes it. Under the big lock, before the
 * caller drops its hold; this may sleep. */
void filemap_evict_unlinked(struct vfs_mount *mnt, uint64_t ino, void *node);

/** For the unmount of `mnt`, a mount that is no bind and that nothing
 * holds, once its operations have drained and before its file system is
 * written: write the pages in memory of its files, drop what cannot be
 * written, and let the files' handles go, so that no record outlives the
 * mount. No later step of the unmount can fail. Under the big lock; this
 * may sleep. */
void filemap_unmount(struct vfs_mount *mnt);

/** msync of the mapping `f` of a region: its file's pages written back
 * when it maps a disk file's, and with `flush`, for MS_SYNC, after a
 * write-back that succeeds, the file system's fsync through `f`'s own
 * description, as Linux's msync syncs the region's file (mm/msync.c:87); 0
 * or the first failure. A failed page write is the answer and is recorded
 * as `filemap_sync_file` records it; with `flush`, `f`'s description's
 * cursor into the file's error sequence moves past it, and a failure
 * recorded there since that description last looked (`vfs_ino_err_check`)
 * is the answer when nothing else failed. Under the big lock; this may
 * sleep. */
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
