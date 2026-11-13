/**
 * Host test of the mapped files of mm/filemap.c: shared anonymous memory,
 * `filemap_anon_shared`, against a mapping of an open file, `filemap_new`.
 *
 * host-deps: rach2/sys/lib/kstring.c rach2/sys/lib/kctypes.c
 * host-deps: tests/host/compat/kheap_host.c tests/host/compat/kirq_none.c
 * host-deps: tests/host/compat/spinlock_host.c tests/host/compat/bkl_held.c
 * host-cflags: -Wno-pointer-to-int-cast -Wno-int-to-pointer-cast
 *
 * filemap.c is included whole, so that a row reads the list of mappings.
 * The pages, the descriptors and the file systems are models; tmpfs gives
 * inode numbers from 41 on.
 *
 * The rows: shared anonymous memory is named "/dev/zero (deleted)", keeps
 * no descriptor, so that /proc/PID/maps finds no entry and no description
 * to name it by, has its own pages in memory and an inode number of the
 * mount of memfds, a new one for each; a mapping of a file has its
 * descriptor's copy and no such number; and the release of each takes it
 * off the list of mappings, frees shared anonymous memory's pages and gives
 * the file's descriptor back. A disk file's pages may hold stores only once
 * a shared mapping through a descriptor open for writing maps it: private
 * mappings and a shared one open only for reading leave it clear, a
 * writable one marks that file and not another of its bucket, and the
 * release of the last mapping of that file clears the mark.
 *
 * A mapping of a file of a type that reads without the big lock lets its
 * faults run without it, and one of another type, or of a file in memory,
 * does not. A fault without the big lock finds a disk file's page in memory
 * under the file's lock alone, and reads a page that is not there under the
 * file's lock, then adds it to the file's pages under the big lock as well,
 * which it gives back. A hit in the page cache supplies the page without
 * reading the file or storing the cached bytes again. A delayed write mark
 * after truncation refuses both an absent page and its replacement, so
 * writeback leaves the regrown clean hole unallocated.
 *
 * Returns 0 when every row passes, 1 otherwise.
 */

#include "mm/filemap.c"

extern int printf(const char *format, ...);

static int fails;

/** Count a failure and print `row` when `ok` is 0. */
static void check(int ok, const char *row)
{
    if (!ok) {
        printf("filemap-hosttest: fail: %s\n", row);
        fails++;
    }
}

struct percpu cpu_table[NR_CPUS];

/* The calls the release makes, which the rows count. */
static int shmem_frees, backing_releases, backing_holds;

/* The last inode number of the mount of memfds. */
static uint64_t memfd_ino = 40;
static struct vfs_mount memfd_mount;

/** The next inode number of the mount of memfds, which only it may take. */
uint64_t tmpfs_new_ino(struct vfs_mount *mnt)
{
    check(mnt == &memfd_mount, "an inode number is taken on the mount of memfds");
    return ++memfd_ino;
}

/** The mount of memfds. */
struct vfs_mount *vfs_memfd_mount(void)
{
    return &memfd_mount;
}

/** Count the release of shared anonymous memory's pages. */
void shmem_free(struct shmem *sh)
{
    (void)sh;
    shmem_frees++;
}

/** Count the release of shared anonymous memory's pages that gives way. */
void shmem_free_yielding(struct shmem *sh)
{
    (void)sh;
    shmem_frees++;
}

/** Count the release of a mapped file's descriptor. */
void fd_release_backing(fd_entry_t *e)
{
    fd_ofd_unref(e->ofd);
    backing_releases++;
}

/** No mount is locked for a release. */
struct vfs_mount *fd_release_lock(const fd_entry_t *e)
{
    (void)e;
    return NULL;
}

/** No mapped file is a node of the KRFFS root. */
krffs_node_t *fd_root_node(const fd_entry_t *e)
{
    (void)e;
    return NULL;
}

/** Count a description reference, including sync's independent hold. */
void fd_ofd_ref(struct open_file *ofd)
{
    if (ofd) {
        backing_holds++;
    }
}

/** Drop a description reference without closing a mapping's descriptor. */
void fd_ofd_unref(struct open_file *ofd)
{
    if (ofd) {
        check(backing_holds > 0, "a description reference is owned before its put");
        backing_holds--;
    }
}

/* ---- a disk file's page, as the fault rows see it ---- */

/* The frame the allocator gives, the page that frame holds, the frame in
 * the file's pages (0 for none), and the depth of the file's lock. */
#define MODEL_FRAME ((uintptr_t)0x5000u)
static uint8_t model_bytes[PAGE_SIZE];
static uintptr_t model_page;
static int file_locked;
static uint64_t model_size = 2 * PAGE_SIZE;
static int disk_writes, disk_error;
static uint8_t disk_bytes[PAGE_SIZE];
static struct vm_file *model_dead;
static uint64_t model_dead_gen;
/* A producer in the gap after every reap, armed by the bounded-sync row. */
static void (*model_reap_gap)(void);
static int model_reap_rounds;
/* A mapping whose last put lands just after the next reap has emptied the
 * list, as another processor's unmap may drop it then. */
static struct vm_file *model_late_put;
/* Whether the leaf of the file's pages that a page goes in exists. */
static int model_leaf;
/* The lookups of the file's pages and those made under the file's lock;
 * the additions, those made under both the file's lock and the big lock,
 * and those under the file's lock alone; the references taken; and the
 * reads of the file. */
static int lookups, lookups_locked, adopts, adopts_locked, adopts_file, refs, reads;
/* Whether the page cache has the page, and its reads and stores. */
static int model_cached, cache_reads, cache_stores;
/* The big lock's model (`compat/bkl_held.c`): whether a row stands for a
 * caller without it, and the code's takes since the row cleared them. */
extern int bkl_host_free;
extern int bkl_host_takes;

/** MODEL_FRAME. */
uintptr_t alloc_user_frame(void)
{
    return MODEL_FRAME;
}

/** Take the file's lock. */
void vfs_node_lock(const struct vfs_ref *ref)
{
    (void)ref;
    file_locked++;
}

/** Give the file's lock back. */
void vfs_node_unlock(const struct vfs_ref *ref)
{
    (void)ref;
    file_locked--;
}

/** The current file size, which a truncation row changes. */
int vfs_getattr(struct vfs_mount *mnt, void *node, struct vfs_stat *st)
{
    (void)mnt;
    (void)node;
    k_memset(st, 0, sizeof(*st));
    st->size = model_size;
    return 0;
}

/** The frame in the file's pages, counted with the locks it was found under. */
uintptr_t shmem_resident(const struct shmem *sh, uint32_t pgoff)
{
    (void)sh;
    (void)pgoff;
    lookups++;
    lookups_locked += file_locked > 0;
    return model_page;
}

/** Whether the page's leaf exists: `model_leaf`. */
int shmem_has_leaf(const struct shmem *sh, uint32_t pgoff)
{
    (void)sh;
    (void)pgoff;
    return model_leaf;
}

/** Add `phys` to the file's pages, counted with the locks held. */
uintptr_t shmem_adopt(struct shmem *sh, uint32_t pgoff, uintptr_t phys)
{
    (void)sh;
    (void)pgoff;
    adopts++;
    adopts_locked += file_locked > 0 && bkl_held();
    adopts_file += file_locked > 0 && !bkl_held();
    model_page = phys;
    return phys;
}

/** Count a reference. */
void frame_ref(uintptr_t phys)
{
    (void)phys;
    refs++;
}

/** The page MODEL_FRAME holds. */
void *kmap_temp(uintptr_t phys)
{
    return phys == MODEL_FRAME ? model_bytes : NULL;
}

/** Nothing. */
void kunmap_temp(void *va)
{
    (void)va;
}

/** Read a page of the file: `count` bytes of 0x5a. */
int32_t fs_read_at(fd_entry_t *e, void *buf, uint32_t count, uint64_t off)
{
    (void)e;
    (void)off;
    reads++;
    k_memset(buf, 0x5a, count);
    return (int32_t)count;
}

/** The page cache's stable epoch. */
uint32_t filecache_epoch(struct vfs_mount *mnt, void *node)
{
    (void)mnt;
    (void)node;
    return 0;
}

/** Count a cache read; copy bytes of 0xa5 on a hit, or return 0 on a miss. */
int filecache_read(const struct fd_entry *e, uint32_t pgoff, uintptr_t phys)
{
    (void)e;
    (void)pgoff;
    cache_reads++;
    if (!model_cached) {
        return 0;
    }
    check(phys == MODEL_FRAME && file_locked > 0,
          "the cache fills the frame under the file's lock");
    k_memset(model_bytes, 0xa5, sizeof(model_bytes));
    return 1;
}

/** Count a cache look; on a hit lend MODEL_FRAME, which then holds bytes of
 * 0xa5, for each page asked for that is 0 in `phys`. */
uint32_t filecache_lend(const struct fd_entry *e, uint32_t first, uint32_t count, uintptr_t *phys)
{
    uint32_t i, lent = 0;

    (void)e;
    (void)first;
    cache_reads++;
    if (!model_cached) {
        return 0;
    }
    check(file_locked > 0, "the cache lends its frame under the file's lock");
    k_memset(model_bytes, 0xa5, sizeof(model_bytes));
    for (i = 0; i < count; i++) {
        if (phys[i] == 0) {
            phys[i] = MODEL_FRAME;
            lent++;
        }
    }
    return lent;
}

/** Count a page stored after a file read. */
void filecache_store(const struct fd_entry *e, uint32_t pgoff, uintptr_t phys, uint32_t epoch)
{
    (void)e;
    (void)pgoff;
    (void)phys;
    (void)epoch;
    cache_stores++;
}

/** Count a frame kept after a file read, as a store. */
void filecache_keep(const struct fd_entry *e, uint32_t pgoff, uintptr_t phys, uint32_t epoch)
{
    (void)e;
    (void)pgoff;
    (void)phys;
    (void)epoch;
    cache_stores++;
}

/** The model keeps no page, so no fault reads around. */
int filecache_keeps(const struct fd_entry *e)
{
    (void)e;
    return 0;
}

/** Nothing is cached to forget. */
void filecache_forget_ino(const struct vfs_mount *mnt, uint64_t ino)
{
    (void)mnt;
    (void)ino;
}

/* ---- what no row reaches ---- */

/** Not reached. */
void arch_irq_enable(void)
{}

/** Not reached. */
int kirq_enabled(void)
{
    return 0;
}

/** Not reached. */
uint32_t block_flags(block_device_t *device)
{
    (void)device;
    return 0;
}

/** Not reached. */
int copy_to_user(void __user *dst, const void *src, size_t n)
{
    (void)dst;
    (void)src;
    (void)n;
    return -EFAULT;
}

/** Not reached. */
void free_frame(uintptr_t phys)
{
    (void)phys;
}

/** Count a writeback of the model file. */
int32_t fs_write_at(fd_entry_t *e, const void *buf, uint32_t count, uint64_t off)
{
    (void)e;
    disk_writes++;
    if (disk_error) {
        return disk_error;
    }
    check(off + count <= PAGE_SIZE, "writeback stays within the model file");
    if (off + count <= PAGE_SIZE) {
        k_memcpy(disk_bytes + (uint32_t)off, buf, count);
    }
    return (int32_t)count;
}

/** Not reached. */
void klog(int level, const char *subsystem, const char *format, ...)
{
    (void)level;
    (void)subsystem;
    (void)format;
}

/** Not reached. */
int loop_backs(struct vfs_mount *mnt, void *node)
{
    (void)mnt;
    (void)node;
    return 0;
}

/* tmpfs's type, which no mount of the model has. */
const struct vfs_type vfs_tmpfs_type = {"tmpfs", 0, 0, 0, 0};

/** Not reached. */
struct shmem *tmpfs_shmem(void *node)
{
    (void)node;
    return NULL;
}

/** Not reached. */
uint32_t tmpfs_seals(void *node)
{
    (void)node;
    return 0;
}

/** Not reached. */
void paging_zap_where(int (*match)(const struct vma *r, void *arg, uintptr_t *lo, uintptr_t *hi),
                      void *arg)
{
    (void)match;
    (void)arg;
}

/** Not reached. */
int proc_in_syscall(void)
{
    return 0;
}

/** Not reached. */
uintptr_t shmem_map_page(struct shmem *sh, uint32_t pgoff)
{
    (void)sh;
    (void)pgoff;
    return 0;
}

/** Not reached. */
int shmem_quota_full(const struct shmem *sh)
{
    (void)sh;
    return 0;
}

/** A truncation removes the model page before returning. */
int shmem_truncate(struct shmem *sh, uint64_t size)
{
    (void)sh;
    model_size = size;
    model_page = 0;
    return 0;
}

/** Not reached. */
int vfs_fsync(struct vfs_mount *mnt, void *handle, uint32_t *seen)
{
    (void)mnt;
    (void)handle;
    (void)seen;
    return 0;
}

/** Not reached: no row runs an msync with MS_SYNC. */
int vfs_fsync_error(struct vfs_mount *mnt, uint32_t *seen)
{
    (void)mnt;
    (void)seen;
    return 0;
}

/** A failed write-back's record for the descriptions' syncs, which no row
 * reads: the rows read the error kept for the file's fsync. */
void vfs_wb_err_set(struct vfs_mount *mnt, int rc)
{
    (void)mnt;
    (void)rc;
}

/** The clock, which no row reads. */
uint64_t tick_count(void)
{
    return 0;
}

/** The age of written stores, which no row reads. */
void vfs_writeback_aged(const struct vfs_mount *mnt, uint64_t born)
{
    (void)mnt;
    (void)born;
}

/** Not reached. */
const struct vfs_mount *vfs_mount_base(const struct vfs_mount *m)
{
    return m;
}

/** Not reached. */
void vfs_ns_unlock(struct vfs_mount *mnt)
{
    (void)mnt;
}

/** Hold a mapped file or a failed release's own reference. */
void vm_file_get(struct vm_file *f)
{
    f->refs++;
}

/** A zero-reference mapping is queued for release and cannot be taken. */
int vm_file_tryget(struct vm_file *f)
{
    if (!f->refs) {
        return 0;
    }
    f->refs++;
    return 1;
}

/** Queue the last put for a release with no regions' lock held. */
void vm_file_put(struct vm_file *f)
{
    check(f->refs > 0, "the mapping has a reference to put");
    if (--f->refs == 0) {
        f->dead_gen = ++model_dead_gen;
        f->dead = model_dead;
        model_dead = f;
    }
}

/** Snapshot the model's zero publications. */
uint64_t vm_file_reap_start(void)
{
    return model_dead_gen;
}

/** Reap only zero publications through `gen`, then let the producer run. */
void vm_file_reap_until(uint64_t gen)
{
    struct vm_file **link;

    for (;;) {
        struct vm_file *f;

        for (link = &model_dead; *link && (*link)->dead_gen > gen; link = &(*link)->dead) {
        }
        f = *link;
        if (!f) {
            break;
        }
        *link = f->dead;
        f->dead = NULL;
        f->release(f);
    }
    if (model_late_put) {
        struct vm_file *f = model_late_put;

        model_late_put = NULL;
        vm_file_put(f);
    }
    if (model_reap_gap) {
        model_reap_rounds++;
        model_reap_gap();
    }
}

/** Run queued releases, including those queued by another release. */
void vm_file_reap(void)
{
    vm_file_reap_until(~(uint64_t)0);
}

/** Not reached. */
void wait_queue_init(wait_queue_t *wq)
{
    (void)wq;
}

/** Not reached. */
int wq_wait_event_nointr(wait_queue_t *wq, wq_cond_fn cond, void *arg)
{
    (void)wq;
    check(cond(arg), "sync has no unfinished release inside its boundary");
    return 0;
}

/** Not reached. */
void wq_wake_all(wait_queue_t *wq)
{
    (void)wq;
}

/* ---- the rows ---- */

/** Whether the string `s` is `want`. */
static int same(const char *s, const char *want)
{
    while (s && *s && *s == *want) {
        s++;
        want++;
    }
    return s && *s == *want;
}

/**
 * Shared anonymous memory against a mapping of an open file: its name,
 * which Linux's shmem_zero_setup gives its file (mm/shmem.c:5383-5389); no
 * entry and no description, so /proc/PID/maps names it by its path alone;
 * its own pages in memory; and an inode number of the mount of memfds, a new
 * one for each, where a mapped file has none. Each release takes it off the
 * list of mappings and gives back what it holds.
 */
static void anon_shared_rows(void)
{
    struct vm_file *a, *b, *file;
    struct open_file ofd;
    fd_entry_t e;

    k_memset(&ofd, 0, sizeof(ofd));
    k_memset(&e, 0, sizeof(e));
    e.type = FD_TYPE_FILE;
    e.ofd = &ofd;
    a = filemap_anon_shared();
    b = filemap_anon_shared();
    file = filemap_new(&e);
    check(a && b && file, "each mapping is made");
    if (!a || !b || !file) {
        return;
    }
    check(same(a->path, "/dev/zero (deleted)") && same(b->path, "/dev/zero (deleted)"),
          "shared anonymous memory is named /dev/zero (deleted)");
    check(filemap_entry(a) == NULL && filemap_ofd(a) == NULL,
          "and keeps no entry and no description");
    check(filemap_shmem(a) == &((struct filemap *)a)->own && filemap_in_memory(a),
          "and its pages are its own, in memory");
    check(filemap_anon_ino(a) == 41u && filemap_anon_ino(b) == 42u,
          "and each has a new inode number of the mount of memfds");
    check(file->path == NULL && filemap_entry(file) != NULL && filemap_entry(file)->ofd == &ofd &&
              filemap_ofd(file) == &ofd && filemap_anon_ino(file) == 0u,
          "a mapped file keeps its descriptor's copy and no such number");
    check(maps == (struct filemap *)file && maps->next == (struct filemap *)b &&
              maps->next->next == (struct filemap *)a && maps->next->next->next == NULL,
          "each is on the list of mappings");
    b->release(b);
    check(shmem_frees == 1 && backing_releases == 0 && maps == (struct filemap *)file &&
              maps->next == (struct filemap *)a,
          "a release of shared anonymous memory frees its pages and leaves the list");
    file->release(file);
    check(shmem_frees == 1 && backing_releases == 1 && maps == (struct filemap *)a,
          "a release of a mapped file gives its descriptor back");
    a->release(a);
    check(shmem_frees == 2 && maps == NULL, "and the last release empties the list");
}

/**
 * What a read of a disk file without the big lock judges by
 * (`filemap_disk_stores`): whether a shared mapping through a descriptor
 * open for writing has mapped that file, whose pages may then hold stores
 * the file lacks. Private mappings leave it clear, as Linux's filemap_read
 * reads the file's own folios with no lock (mm/filemap.c:2614-2720), and so
 * does a shared mapping through a descriptor open only for reading, which
 * can never be made writable; a writable one sets it for its file alone,
 * not for 7, which shares a bucket of the 256 with 263; and the release of
 * the shared file's last mapping clears it.
 */
static void disk_stores_rows(void)
{
    struct vm_file *a, *b, *shared, *reader;
    struct open_file ofd7, ofd263;
    struct vfs_mount disk;
    fd_entry_t e7, e263, e263_read;

    k_memset(&disk, 0, sizeof(disk));
    k_memset(&ofd7, 0, sizeof(ofd7));
    k_memset(&ofd263, 0, sizeof(ofd263));
    k_memset(&e7, 0, sizeof(e7));
    k_memset(&e263, 0, sizeof(e263));
    ofd7.vmnt = &disk;
    ofd7.ino = 7;
    ofd263.vmnt = &disk;
    ofd263.ino = 263;
    e7.type = FD_TYPE_FILE;
    e7.ofd = &ofd7;
    e263.type = FD_TYPE_FILE;
    e263.flags = FD_FLAG_READ | FD_FLAG_WRITE;
    e263.ofd = &ofd263;
    e263_read = e263;
    e263_read.flags = FD_FLAG_READ;
    shmem_frees = 0;
    a = filemap_private(&e7, NULL, 1);
    b = filemap_private(&e7, NULL, 1);
    check(a && b && filemap_shmem(a) == filemap_shmem(b) && !filemap_disk_pages(a),
          "two private mappings of a disk file share its pages and are not shared");
    check(!filemap_disk_stores(&disk, 7) && !filemap_disk_stores(&disk, 263),
          "and leave the file with no stores");
    reader = filemap_disk_shared(&e263_read);
    check(reader && filemap_disk_pages(reader) && !filemap_disk_stores(&disk, 263),
          "a shared mapping through a descriptor open only for reading leaves no stores");
    shared = filemap_disk_shared(&e263);
    check(shared && filemap_disk_pages(shared), "a shared mapping of a disk file is made");
    check(filemap_disk_stores(&disk, 263) && !filemap_disk_stores(&disk, 7) &&
              !filemap_disk_stores(&disk, 8),
          "and through a descriptor open for writing marks its file, and that file alone, as "
          "one that may hold stores");
    if (shared) {
        shared->release(shared);
    }
    check(filemap_disk_stores(&disk, 263) && shmem_frees == 0,
          "the mark stays while a mapping of the file is left");
    if (reader) {
        reader->release(reader);
    }
    check(!filemap_disk_stores(&disk, 7) && !filemap_disk_stores(&disk, 263) && shmem_frees == 1,
          "the release of its last mapping frees its pages and clears the mark");
    if (a) {
        a->release(a);
    }
    if (b) {
        b->release(b);
    }
    check(shmem_frees == 2 && maps == NULL && dshare_count == 0,
          "the private mappings' releases free their file's pages");
}

/** After two cache misses, give a private mapping's page the cache's frame
 * under the file's lock alone, without another file read or cache store. */
static void cached_fault_row(struct vm_file *f)
{
    uintptr_t phys = 0;

    model_page = 0;
    model_leaf = 1;
    model_cached = 1;
    k_memset(model_bytes, 0, sizeof(model_bytes));
    check(f->share(f, 1, &phys) == 0 && phys == MODEL_FRAME && reads == 2 && cache_reads == 3 &&
              cache_stores == 2 && model_bytes[0] == 0xa5 && model_bytes[PAGE_SIZE - 1] == 0xa5,
          "a cache hit supplies the page without a file read or another cache store");
    check(model_page == MODEL_FRAME && adopts == 3 && adopts_file == 2 && bkl_host_takes == 0 &&
              !bkl_held() && file_locked == 0,
          "and adds it to the existing leaf under the file's lock alone");
    model_cached = 0;
}

/** A delayed mark cannot dirty a removed frame or a regrown hole's page. */
static void late_mark_rows(struct vm_file *f)
{
    struct filemap *m = (struct filemap *)f;
    struct dshare *ds = m->ds;
    uintptr_t old = MODEL_FRAME;

    model_page = old;
    check(f->mkwrite(f, 1, old) == 0 && dshare_stored(ds, 1),
          "a resident frame is marked before stores may reach it");
    dshare_unmark_from(ds, 0);
    model_size = PAGE_SIZE;
    check(f->mkwrite(f, 1, old) == -EAGAIN && !dshare_stored(ds, 1),
          "a frame still resident past the current EOF cannot be marked");
    model_size = 2 * PAGE_SIZE;
    filemap_file_truncated(ds->mnt, ds->ino, PAGE_SIZE);
    check(!model_page && !dshare_stored(ds, 1),
          "truncation removes the page and its writeback mark");
    check(f->mkwrite(f, 1, old) == -EAGAIN && !dshare_stored(ds, 1),
          "a delayed fault cannot restore the removed frame's mark");
    model_size = 2 * PAGE_SIZE;
    model_page = old + PAGE_SIZE;
    check(f->mkwrite(f, 1, old) == -EAGAIN && !dshare_stored(ds, 1),
          "a delayed mark also refuses a replacement at the same file offset");
    disk_writes = 0;
    check(dshare_write_page(ds, m, 1) == 0 && disk_writes == 0,
          "writeback leaves a regrown clean hole unallocated");
    check(f->mkwrite(f, 1, model_page) == 0 && dshare_stored(ds, 1),
          "a new fault may mark the replacement frame itself");
    dshare_unmark_from(ds, 0);
    model_page = 0;
}

/** The faults of `unlocked_rows` through `f`, a private mapping, and
 * `shared`, a shared one, of a file whose reads run without the big lock. */
static void fault_rows(struct vm_file *f, struct vm_file *shared)
{
    uintptr_t phys = 0;

    bkl_host_free = 1;
    bkl_host_takes = 0;
    check(f->share(f, 1, &phys) == 1 && phys == MODEL_FRAME && reads == 1 && model_bytes[0] == 0x5a,
          "a fault without the big lock reads a page that is not in memory");
    check(adopts == 1 && adopts_locked == 1 && bkl_host_takes == 1 && !bkl_held() &&
              file_locked == 0,
          "and adds it, its leaf not yet made, to the file's pages under the file's lock and "
          "the big lock, both given back");
    model_page = 0;
    model_leaf = 1;
    bkl_host_takes = 0;
    check(f->share(f, 0, &phys) == 1 && phys == MODEL_FRAME && reads == 2,
          "a fault reads another page that is not in memory");
    check(adopts == 2 && adopts_file == 1 && bkl_host_takes == 0 && file_locked == 0,
          "and adds it to a leaf that exists under the file's lock alone");
    model_leaf = 0;
    bkl_host_takes = 0;
    lookups = 0;
    lookups_locked = 0;
    check(shared->share(shared, 1, &phys) == 0 && phys == MODEL_FRAME && refs == 1 && reads == 2,
          "a page in memory is lent with a reference and no read");
    check(lookups == 1 && lookups_locked == 1 && bkl_host_takes == 0 && file_locked == 0,
          "under the file's lock alone");
    cached_fault_row(f);
    late_mark_rows(shared);
    bkl_host_free = 0;
}

/**
 * Which mappings' faults run without the big lock (`unlocked`): a regular
 * file's of a type that reads without it, as ext2's do, private or shared,
 * and no other type's, nor a file in memory's; and how such a fault reaches
 * a disk file's pages (`filemap_disk_page`). A page not in memory is read
 * under the file's lock and joins the file's pages under the big lock too
 * when its leaf must be made, since readers under the big lock alone walk
 * the table's arrays, and under the file's lock alone when the leaf exists,
 * as Linux's __filemap_add_folio takes only the mapping's own lock
 * (mm/filemap.c:842-950); a page in memory is lent under the file's lock
 * alone, as Linux's filemap_fault finds and locks a folio with no global
 * lock (mm/filemap.c:3326-3370).
 */
static void unlocked_rows(void)
{
    static struct vfs_type reads_free, reads_locked;
    static struct shmem sh;
    struct vfs_mount disk, other;
    struct open_file ofd, ofd_other;
    fd_entry_t e, e_other;
    struct vm_file *f, *shared, *g, *mem;

    k_memset(&disk, 0, sizeof(disk));
    k_memset(&other, 0, sizeof(other));
    k_memset(&ofd, 0, sizeof(ofd));
    k_memset(&ofd_other, 0, sizeof(ofd_other));
    k_memset(&e, 0, sizeof(e));
    k_memset(&e_other, 0, sizeof(e_other));
    reads_free.reads_unlocked = 1;
    disk.type = &reads_free;
    other.type = &reads_locked;
    ofd.vmnt = &disk;
    ofd.ino = 9;
    ofd_other.vmnt = &other;
    e.type = FD_TYPE_FILE;
    e.ofd = &ofd;
    e_other.type = FD_TYPE_FILE;
    e_other.ofd = &ofd_other;
    f = filemap_private(&e, NULL, 1);
    shared = filemap_disk_shared(&e);
    g = filemap_new(&e_other);
    mem = filemap_shared(&e, &sh);
    check(f && shared && g && mem, "each mapping is made");
    if (!f || !shared || !g || !mem) {
        return;
    }
    check(f->unlocked && shared->unlocked,
          "a private and a shared mapping of a type that reads without the big lock run "
          "their faults without it");
    check(!g->unlocked, "a mapping of another type runs its faults under it");
    check(!mem->unlocked, "and so does a mapping of a file in memory");

    fault_rows(f, shared);

    mem->release(mem);
    g->release(g);
    shared->release(shared);
    f->release(f);
    check(maps == NULL && dshare_count == 0, "the releases leave no mapping");
}

/** Make one dirty shared page for release, retry and EOF-tail rows. */
static struct vm_file *writeback_file(struct vfs_mount *mnt, struct open_file *ofd)
{
    fd_entry_t e;
    struct vm_file *f;
    struct filemap *m;

    k_memset(&e, 0, sizeof(e));
    e.type = FD_TYPE_FILE;
    e.flags = FD_FLAG_READ | FD_FLAG_WRITE;
    e.ofd = ofd;
    ofd->vmnt = mnt;
    ofd->ino = 93;
    f = filemap_disk_shared(&e);
    check(f != NULL, "a shared writeback mapping is made");
    if (!f) {
        return NULL;
    }
    m = (struct filemap *)f;
    m->ds->sh.npages = 1;
    model_page = MODEL_FRAME;
    model_size = PAGE_SIZE;
    k_memset(model_bytes, 0x65, sizeof(model_bytes));
    k_memset(disk_bytes, 0, sizeof(disk_bytes));
    disk_writes = disk_error = 0;
    check(f->mkwrite(f, 0, model_page) == 0, "the page is marked for a shared store");
    return f;
}

/** Failed unmaps keep stores; fsync reports the error after a good retry. */
static void writeback_retry_rows(void)
{
    struct vfs_mount mnt;
    struct open_file ofd;
    struct vm_file *f;
    int freed, released;

    k_memset(&mnt, 0, sizeof(mnt));
    k_memset(&ofd, 0, sizeof(ofd));
    f = writeback_file(&mnt, &ofd);
    if (!f) {
        return;
    }
    freed = shmem_frees;
    released = backing_releases;
    disk_error = -EIO;
    vm_file_put(f);
    vm_file_reap();
    check(f->refs == 1 && ((struct filemap *)f)->retained == 1 && shmem_frees == freed &&
              backing_releases == released && dshare_count == 1,
          "a failed final write keeps its page, mark, file and mapping for retry");
    disk_error = 0;
    check(filemap_sync_file(&ofd) == -EIO && disk_bytes[0] == 0x65 &&
              disk_bytes[PAGE_SIZE - 1] == 0x65,
          "fsync retries the stored bytes and reports the earlier release error");
    vm_file_reap();
    check(!maps && dshare_count == 0 && shmem_frees == freed + 1 &&
              backing_releases == released + 1 && filemap_sync_file(&ofd) == 0,
          "the successful retry frees the keeper and reports its error once");
}

/** Final release allocates no buffer and fsync drains deferred writers. */
static void writeback_release_rows(void)
{
    struct vfs_mount mnt;
    struct open_file ofd;
    struct vm_file *f;
    extern unsigned long kheap_host_largest;

    k_memset(&mnt, 0, sizeof(mnt));
    k_memset(&ofd, 0, sizeof(ofd));
    f = writeback_file(&mnt, &ofd);
    if (!f) {
        return;
    }
    kheap_host_largest = 1;
    vm_file_put(f);
    vm_file_reap();
    kheap_host_largest = 0;
    check(!maps && disk_writes > 0 && disk_bytes[PAGE_SIZE - 1] == 0x65,
          "final unmap needs no heap allocation to write its dirty page");

    f = writeback_file(&mnt, &ofd);
    if (!f) {
        return;
    }
    vm_file_put(f);
    check(filemap_sync_file(&ofd) == 0 && disk_writes > 0 && disk_bytes[0] == 0x65,
          "fsync reaps a zero-reference writer before claiming all stores reached the file");
    check(!maps && !model_dead && dshare_count == 0,
          "the queued writer and the sync hold are both released");
}

/** A last put after sync's reap cannot hide the mapping's stored bytes. */
static void writeback_late_put_rows(void)
{
    struct vfs_mount mnt;
    struct open_file ofd;
    struct vm_file *f;

    k_memset(&mnt, 0, sizeof(mnt));
    k_memset(&ofd, 0, sizeof(ofd));
    f = writeback_file(&mnt, &ofd);
    if (!f) {
        return;
    }
    model_late_put = f;
    check(filemap_sync_file(&ofd) == 0 && disk_writes > 0 && disk_bytes[0] == 0x65 &&
              disk_bytes[PAGE_SIZE - 1] == 0x65,
          "a writer whose last put lands between the reap and the choice is written back");
    check(!model_late_put && model_dead == f && f->refs == 0,
          "the later last put stays queued beyond sync's boundary");
    vm_file_reap();
    check(!model_dead && !maps && dshare_count == 0,
          "a later reap frees the writer after its stores reached the file");
}

/* The description the recurring producer maps. */
static struct open_file *producer_ofd;

/** Map, store and drop the last reference in every reap-to-write gap. */
static void writeback_produce(void)
{
    fd_entry_t e;
    struct vm_file *f;

    k_memset(&e, 0, sizeof(e));
    e.type = FD_TYPE_FILE;
    e.flags = FD_FLAG_READ | FD_FLAG_WRITE;
    e.ofd = producer_ofd;
    f = filemap_disk_shared(&e);
    check(f != NULL, "the recurring producer makes a writable mapping");
    if (f) {
        check(f->mkwrite(f, 0, model_page) == 0, "the producer marks its store");
        model_bytes[0] = 0x66;
        vm_file_put(f);
    }
}

/** A producer after every reap cannot make sync chase its later releases. */
static void writeback_bounded_rows(void)
{
    struct vfs_mount mnt;
    struct open_file ofd;
    struct vm_file *f;
    int holds = backing_holds;

    k_memset(&mnt, 0, sizeof(mnt));
    k_memset(&ofd, 0, sizeof(ofd));
    f = writeback_file(&mnt, &ofd);
    if (!f) {
        return;
    }
    vm_file_put(f);
    producer_ofd = &ofd;
    model_reap_rounds = 0;
    model_reap_gap = writeback_produce;
    check(filemap_sync_file(&ofd) == 0 && model_reap_rounds == 1 && disk_bytes[0] == 0x66 &&
              disk_bytes[PAGE_SIZE - 1] == 0x65,
          "fsync writes through its own description and returns after one bounded reap");
    check(model_dead && !model_dead->refs && backing_holds == holds + 1,
          "the later producer's mapping waits for another reap, with no leaked sync hold");
    model_reap_gap = NULL;
    producer_ofd = NULL;
    vm_file_reap();
    check(!model_dead && !maps && !dshare_count && backing_holds == holds,
          "cleanup frees the later mapping, its pages and its description");
}

/** A retry by sync(2) alone frees what a failed release kept, not its error. */
static void writeback_sync_retry_rows(void)
{
    struct vfs_mount mnt;
    struct open_file ofd;
    struct vm_file *f;
    struct dshare *ds;
    int released;

    k_memset(&mnt, 0, sizeof(mnt));
    k_memset(&ofd, 0, sizeof(ofd));
    f = writeback_file(&mnt, &ofd);
    if (!f) {
        return;
    }
    released = backing_releases;
    disk_error = -EIO;
    vm_file_put(f);
    vm_file_reap();
    check(((struct filemap *)f)->retained == 1 && maps == (struct filemap *)f,
          "the failed final write keeps its mapping");
    disk_error = 0;
    filemap_sync_all();
    vm_file_reap();
    ds = dshare_of(&mnt, ofd.ino);
    check(!maps && !model_dead && backing_releases == released + 1 && disk_bytes[0] == 0x65 &&
              dshare_count == 1 && ds && ds->users == 0 && ds->error == -EIO,
          "a successful sync(2) frees the kept pages, descriptor and mapping, and keeps the error");
    filemap_sync_all();
    check(dshare_count == 1, "sync(2) leaves a record holding only an error alone");
    check(filemap_sync_file(&ofd) == -EIO && dshare_count == 0,
          "the next fsync reports the error and frees the record");
    check(filemap_sync_file(&ofd) == 0, "and reports it once");
}

/** Stores beyond EOF are zeroed before a later growth can expose them. */
static void writeback_tail_rows(void)
{
    struct vfs_mount mnt;
    struct open_file ofd;
    struct vm_file *f;

    k_memset(&mnt, 0, sizeof(mnt));
    k_memset(&ofd, 0, sizeof(ofd));
    f = writeback_file(&mnt, &ofd);
    if (!f) {
        return;
    }
    model_size = 17;
    check(filemap_sync_file(&ofd) == 0 && model_bytes[16] == 0x65 && model_bytes[17] == 0 &&
              model_bytes[PAGE_SIZE - 1] == 0,
          "writeback zeros stores past EOF before a later growth can make them file data");
    model_size = PAGE_SIZE;
    check(filemap_sync_file(&ofd) == 0 && disk_bytes[16] == 0x65 && disk_bytes[17] == 0 &&
              disk_bytes[PAGE_SIZE - 1] == 0,
          "regrowth writes only zeros from the old page's tail");
    vm_file_put(f);
    vm_file_reap();
    check(!maps && dshare_count == 0 && !model_dead, "writeback rows leave no retained file");
}

int main(void)
{
    anon_shared_rows();
    disk_stores_rows();
    unlocked_rows();
    writeback_retry_rows();
    writeback_release_rows();
    writeback_late_put_rows();
    writeback_bounded_rows();
    writeback_sync_retry_rows();
    writeback_tail_rows();
    if (fails) {
        printf("filemap-hosttest: %d failed\n", fails);
        return 1;
    }
    printf("filemap-hosttest: ok\n");
    return 0;
}
