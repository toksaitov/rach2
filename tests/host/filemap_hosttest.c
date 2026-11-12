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
 * The pages, the descriptors and the file systems are stubs that no row
 * reaches but the release's; tmpfs gives inode numbers from 41 on.
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
 * which it gives back.
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
static int shmem_frees, backing_releases;

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
    (void)e;
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

/** Take a reference to a description, which no row counts. */
void fd_ofd_ref(struct open_file *ofd)
{
    (void)ofd;
}

/* ---- a disk file's page, as the fault rows see it ---- */

/* The frame the allocator gives, the page that frame holds, the frame in
 * the file's pages (0 for none), and the depth of the file's lock. */
#define MODEL_FRAME ((uintptr_t)0x5000u)
static uint8_t model_bytes[PAGE_SIZE];
static uintptr_t model_page;
static int file_locked;
/* The lookups of the file's pages and those made under the file's lock;
 * the additions and those made under both the file's lock and the big
 * lock; the references taken; and the reads of the file. */
static int lookups, lookups_locked, adopts, adopts_locked, refs, reads;
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

/** The file is two pages long. */
int vfs_getattr(struct vfs_mount *mnt, void *node, struct vfs_stat *st)
{
    (void)mnt;
    (void)node;
    k_memset(st, 0, sizeof(*st));
    st->size = 2 * PAGE_SIZE;
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

/** Add `phys` to the file's pages, counted with the locks held. */
uintptr_t shmem_adopt(struct shmem *sh, uint32_t pgoff, uintptr_t phys)
{
    (void)sh;
    (void)pgoff;
    adopts++;
    adopts_locked += file_locked > 0 && bkl_held();
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
uint32_t filecache_epoch(struct vfs_mount *mnt, void *node)
{
    (void)mnt;
    (void)node;
    return 0;
}

/** Not reached. */
int filecache_read(const struct fd_entry *e, uint32_t pgoff, uintptr_t phys)
{
    (void)e;
    (void)pgoff;
    (void)phys;
    return -EIO;
}

/** Not reached. */
void filecache_store(const struct fd_entry *e, uint32_t pgoff, uintptr_t phys, uint32_t epoch)
{
    (void)e;
    (void)pgoff;
    (void)phys;
    (void)epoch;
}

/** Not reached. */
void free_frame(uintptr_t phys)
{
    (void)phys;
}

/** Not reached. */
int32_t fs_write_at(fd_entry_t *e, const void *buf, uint32_t count, uint64_t off)
{
    (void)e;
    (void)buf;
    (void)count;
    (void)off;
    return -EIO;
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

/** Not reached. */
int shmem_truncate(struct shmem *sh, uint64_t size)
{
    (void)sh;
    (void)size;
    return 0;
}

/** Not reached. */
int vfs_fsync(struct vfs_mount *mnt, void *handle)
{
    (void)mnt;
    (void)handle;
    return 0;
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

/** Not reached. */
void vm_file_get(struct vm_file *f)
{
    (void)f;
}

/** Not reached. */
void vm_file_put(struct vm_file *f)
{
    (void)f;
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
    (void)cond;
    (void)arg;
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

/**
 * Which mappings' faults run without the big lock (`unlocked`): a regular
 * file's of a type that reads without it, as ext2's do, private or shared,
 * and no other type's, nor a file in memory's; and how such a fault reaches
 * a disk file's pages (`filemap_disk_page`). A page not in memory is read
 * under the file's lock and joins the file's pages under the big lock too,
 * which readers under it alone read; a page in memory is lent under the
 * file's lock alone, as Linux's filemap_fault finds and locks a folio with
 * no global lock (mm/filemap.c:3326-3370).
 */
static void unlocked_rows(void)
{
    static struct vfs_type reads_free, reads_locked;
    static struct shmem sh;
    struct vfs_mount disk, other;
    struct open_file ofd, ofd_other;
    fd_entry_t e, e_other;
    struct vm_file *f, *shared, *g, *mem;
    uintptr_t phys = 0;

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

    bkl_host_free = 1;
    bkl_host_takes = 0;
    check(f->share(f, 1, &phys) == 1 && phys == MODEL_FRAME && reads == 1 && model_bytes[0] == 0x5a,
          "a fault without the big lock reads a page that is not in memory");
    check(adopts == 1 && adopts_locked == 1 && bkl_host_takes == 1 && !bkl_held() &&
              file_locked == 0,
          "and adds it to the file's pages under the file's lock and the big lock, "
          "both given back");
    bkl_host_takes = 0;
    lookups = 0;
    lookups_locked = 0;
    check(shared->share(shared, 1, &phys) == 0 && phys == MODEL_FRAME && refs == 1 && reads == 1,
          "a page in memory is lent with a reference and no read");
    check(lookups == 1 && lookups_locked == 1 && bkl_host_takes == 0 && file_locked == 0,
          "under the file's lock alone");
    bkl_host_free = 0;

    mem->release(mem);
    g->release(g);
    shared->release(shared);
    f->release(f);
    check(maps == NULL && dshare_count == 0, "the releases leave no mapping");
}

int main(void)
{
    anon_shared_rows();
    disk_stores_rows();
    unlocked_rows();
    if (fails) {
        printf("filemap-hosttest: %d failed\n", fails);
        return 1;
    }
    printf("filemap-hosttest: ok\n");
    return 0;
}
