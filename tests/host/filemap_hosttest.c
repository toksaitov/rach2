/**
 * Host test of the mapped files of mm/filemap.c: shared anonymous memory,
 * `filemap_anon_shared`, against a mapping of an open file, `filemap_new`.
 *
 * host-deps: rach2/sys/lib/kstring.c rach2/sys/lib/kctypes.c
 * host-deps: tests/host/compat/kheap_host.c tests/host/compat/kirq_none.c
 * host-deps: tests/host/compat/spinlock_host.c
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
 * the file's descriptor back.
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

/* ---- what no row reaches ---- */

/** Not reached. */
uintptr_t alloc_user_frame(void)
{
    return 0;
}

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
void frame_ref(uintptr_t phys)
{
    (void)phys;
}

/** Not reached. */
void free_frame(uintptr_t phys)
{
    (void)phys;
}

/** Not reached. */
int32_t fs_read_at(fd_entry_t *e, void *buf, uint32_t count, uint64_t off)
{
    (void)e;
    (void)buf;
    (void)count;
    (void)off;
    return -EIO;
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
void *kmap_temp(uintptr_t phys)
{
    (void)phys;
    return NULL;
}

/** Not reached. */
void kunmap_temp(void *va)
{
    (void)va;
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
uintptr_t shmem_adopt(struct shmem *sh, uint32_t pgoff, uintptr_t phys)
{
    (void)sh;
    (void)pgoff;
    (void)phys;
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
uintptr_t shmem_resident(const struct shmem *sh, uint32_t pgoff)
{
    (void)sh;
    (void)pgoff;
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
int vfs_getattr(struct vfs_mount *mnt, void *node, struct vfs_stat *st)
{
    (void)mnt;
    (void)node;
    (void)st;
    return -EIO;
}

/** Not reached. */
const struct vfs_mount *vfs_mount_base(const struct vfs_mount *m)
{
    return m;
}

/** Not reached. */
void vfs_node_lock(const struct vfs_ref *ref)
{
    (void)ref;
}

/** Not reached. */
void vfs_node_unlock(const struct vfs_ref *ref)
{
    (void)ref;
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

int main(void)
{
    anon_shared_rows();
    if (fails) {
        printf("filemap-hosttest: %d failed\n", fails);
        return 1;
    }
    printf("filemap-hosttest: ok\n");
    return 0;
}
