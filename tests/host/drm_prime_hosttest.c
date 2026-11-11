/* PRIME sharing, cache identity and lifetimes over the real GEM and dma-buf
 * code, and vgem's fences on a buffer another device imports; frames,
 * descriptor tables, device regions and the commit worker are host models.
 * host-deps: rach2/sys/lib/kstring.c rach2/sys/lib/kctypes.c
 * host-deps: tests/host/compat/spinlock_host.c tests/host/compat/atomic_host.c
 */
#include "drm/drm_gem.c"
#include "drm/drm_prime.c"
#include "dmabuf/fence.c"
#include "dmabuf/dmabuf.c"
#include "dmabuf/sync_file.c"
#include "dmabuf/udmabuf.c"
#include "drm/vgem.c"

/** No interrupts on the host: the fences' masking is a no-op. */
uint32_t kirq_save(void)
{
    return 0;
}

/** The pair of kirq_save. */
void kirq_restore(uint32_t flags)
{
    (void)flags;
}

extern int printf(const char *format, ...);
extern void *calloc(unsigned long n, unsigned long size);
extern void free(void *p);

struct percpu cpu_table[NR_CPUS];
static process_t self;
static fd_table_t table;
static fd_entry_t slots[64];
static struct open_file descriptions[64];
static int failures, wakes, wait_mode;
static int64_t now_ns;
static struct dma_fence *signal_on_wait;

#define CHECK(c, s)                                  \
    do {                                             \
        if (!(c)) {                                  \
            printf("drm-prime-test: fail: %s\n", s); \
            failures++;                              \
        }                                            \
    } while (0)

/** Fixed monotonic clock. */
void rtc_monotonic(int64_t *sec, int64_t *ns)
{
    *sec = now_ns / 1000000000ll;
    *ns = now_ns % 1000000000ll;
}
/** Fixed tick count. */
uint64_t rtc_ticks_now(void)
{
    return (uint64_t)(now_ns / 1000000);
}
/** Host arithmetic for the kernel's division helper. */
uint64_t tick_div(uint64_t n, uint32_t d, uint32_t *rem)
{
    if (rem)
        *rem = (uint32_t)(n % d);
    return n / d;
}
/** Initialize a model queue. */
void wait_queue_init(wait_queue_t *wq)
{
    k_memset(wq, 0, sizeof(*wq));
}
/** Record a wake without scheduling. */
void wq_wake(wait_queue_t *wq, uint32_t kinds)
{
    (void)wq;
    (void)kinds;
    wakes++;
}
/** Wake every modeled kind. */
void wq_wake_all(wait_queue_t *wq)
{
    wq_wake(wq, WQ_ALL);
}
/** Advance to an armed timeout or complete a producer during the sleep. */
int wq_wait_event(wait_queue_t *wq, wq_cond_fn cond, void *arg)
{
    (void)wq;
    if (wait_mode == 1)
        return -EINTR;
    if (signal_on_wait)
        dma_fence_signal(signal_on_wait);
    else
        now_ns = (int64_t)current_proc->wakeup_tick * 1000000;
    CHECK(cond(arg), "wait condition after scheduled event");
    return 0;
}
/** Uninterruptible waits ignore the model's interrupt. */
int wq_wait_event_nointr(wait_queue_t *wq, wq_cond_fn cond, void *arg)
{
    int saved = wait_mode, rc;
    wait_mode = 0;
    rc = wq_wait_event(wq, cond, arg);
    wait_mode = saved;
    return rc;
}
/** Copy from a real test address, refusing the model's invalid page. */
int copy_from_user(void *dst, const void __user *src, size_t n)
{
    if ((uintptr_t)src < 4096)
        return -EFAULT;
    k_memcpy(dst, src, n);
    return 0;
}
/** Copy to a real test address. */
int copy_to_user(void __user *dst, const void *src, size_t n)
{
    if ((uintptr_t)dst < 4096)
        return -EFAULT;
    k_memcpy(dst, src, n);
    return 0;
}
/** Allocate a descriptor in the model's bounded table. */
int fd_alloc(fd_table_t *t)
{
    int i;
    for (i = 0; i < 64; i++)
        if (t->entries[i].type == FD_TYPE_NONE) {
            k_memset(&descriptions[i], 0, sizeof(descriptions[i]));
            t->entries[i].ofd = &descriptions[i];
            return i;
        }
    return -EMFILE;
}
/** Find a live model descriptor. */
fd_entry_t *fd_get(fd_table_t *t, int fd)
{
    return fd >= 0 && fd < 64 && t->entries[fd].type != FD_TYPE_NONE ? &t->entries[fd] : NULL;
}
/** Release the object attached to a model descriptor. */
int fd_free(fd_table_t *t, int fd)
{
    fd_entry_t *e = fd_get(t, fd);
    if (!e)
        return -EBADF;
    if (e->ops && e->ops->close)
        e->ops->close(e);
    k_memset(e, 0, sizeof(*e));
    return 0;
}

static int allocations, fail_after = -1;

/** Allocate zeroed model storage, optionally failing a chosen allocation. */
void *kmalloc(size_t size)
{
    void *p;
    if (fail_after == 0)
        return NULL;
    if (fail_after > 0)
        fail_after--;
    p = calloc(1, size);
    if (p)
        allocations++;
    return p;
}

/** Kernel zeroed allocation over the same fault counter. */
void *kzalloc(size_t size)
{
    return kmalloc(size);
}

/** Drop one tracked allocation. */
void kfree(void *p)
{
    if (p)
        allocations--;
    free(p);
}

/** Hold a modeled device. */
void drm_dev_get(struct drm_device *dev)
{
    dev->refs++;
}

/** Drop a modeled device; its record belongs to the test. */
void drm_dev_put(struct drm_device *dev)
{
    dev->refs--;
}

struct model_frame {
    uintptr_t phys;
    unsigned refs;
};
static struct model_frame frames[64];
static int frames_live, pins;

/** Release one page hold, freeing its bytes when the final holder goes. */
uint32_t frame_unref(uintptr_t phys)
{
    unsigned i;
    for (i = 0; i < 64; i++) {
        if (frames[i].phys == phys) {
            unsigned left = --frames[i].refs;
            if (!left) {
                free((void *)phys);
                frames[i].phys = 0;
                frames_live--;
            }
            return left;
        }
    }
    CHECK(0, "release known frame");
    return 0;
}

/** Find a resident page in the model's single leaf. */
uintptr_t shmem_resident(const struct shmem *sh, uint32_t page)
{
    return sh->leaves && page < 64 ? sh->leaves[0][page] : 0;
}

/** Allocate and zero the requested pages, retaining their shmem holds. */
int shmem_allocate(struct shmem *sh, uint64_t off, uint64_t len)
{
    unsigned i;
    uint32_t page;
    if (!sh->leaves) {
        sh->leaves = kzalloc(sizeof(*sh->leaves));
        if (!sh->leaves)
            return -ENOMEM;
        sh->leaves[0] = kzalloc(64 * sizeof(uintptr_t));
        if (!sh->leaves[0]) {
            kfree(sh->leaves);
            sh->leaves = NULL;
            return -ENOMEM;
        }
    }
    for (page = (uint32_t)(off >> 12); page < (off + len) >> 12; page++) {
        if (page >= 64)
            return -ENOMEM;
        if (shmem_resident(sh, page))
            continue;
        for (i = 0; i < 64 && frames[i].refs; i++)
            ;
        if (i == 64)
            return -ENOMEM;
        frames[i].phys = (uintptr_t)calloc(1, PAGE_SIZE);
        frames[i].refs = 1;
        frames_live++;
        sh->leaves[0][page] = frames[i].phys;
    }
    return 0;
}

/** Pin a resident or newly allocated page for udmabuf. */
uintptr_t shmem_map_page(struct shmem *sh, uint32_t page)
{
    uintptr_t phys;
    unsigned i;
    if (shmem_allocate(sh, (uint64_t)page << 12, PAGE_SIZE))
        return 0;
    phys = shmem_resident(sh, page);
    for (i = 0; i < 64; i++) {
        if (frames[i].phys == phys) {
            frames[i].refs++;
            pins++;
            return phys;
        }
    }
    return 0;
}

/** Release shmem's pages; an udmabuf pin can keep their bytes alive. */
void shmem_free(struct shmem *sh)
{
    unsigned i;
    if (!sh->leaves)
        return;
    for (i = 0; i < 64; i++) {
        if (sh->leaves[0][i])
            frame_unref(sh->leaves[0][i]);
    }
    kfree(sh->leaves[0]);
    kfree(sh->leaves);
    sh->leaves = NULL;
}

struct model_region {
    struct vm_file vf;
    const struct vm_device_ops *ops;
    void *obj;
    uint32_t cache;
};

/** Record a shared device mapping and its ownership, or refuse it. */
int vm_device_map(vma_t *vma, const struct vm_device_ops *ops, void *obj, uint32_t cache)
{
    struct model_region *r;
    if (vma->type != VM_SHARED)
        return -EINVAL;
    r = kzalloc(sizeof(*r));
    if (!r)
        return -ENOMEM;
    r->ops = ops;
    r->obj = obj;
    r->cache = cache;
    vma->file = &r->vf;
    return 0;
}

/** Release a mapped region's object once at final unmap. */
static void unmap(vma_t *vma)
{
    struct model_region *r = (struct model_region *)vma->file;
    r->ops->put(r->obj);
    kfree(r);
    vma->file = NULL;
}

/** Fault a page through the recorded production mapping operations. */
static uintptr_t mapped_page(vma_t *vma, uint32_t page)
{
    struct model_region *r = (struct model_region *)vma->file;
    uintptr_t phys = 0;
    if (r->ops->page(r->obj, page, &phys))
        return 0;
    return phys;
}

static struct shmem memfd_pages;
const struct vfs_type vfs_tmpfs_type = {0};
static struct vfs_mount memfd_mount;

/** A model memfd's shmem backing. */
struct shmem *tmpfs_shmem(void *node)
{
    (void)node;
    return &memfd_pages;
}

/** The model memfd is sealed against shrink, as udmabuf requires. */
uint32_t tmpfs_seals(void *node)
{
    (void)node;
    return TMPFS_SEAL_SHRINK;
}

/** Node publication is outside these rows. */
int32_t devfs_node_register(struct devfs_node *node)
{
    (void)node;
    return 0;
}

/* vgem's registration, which the rows do not run. */
int kcmdline_has(const char *flag)
{
    (void)flag;
    return 0;
}
/** No log on the host. */
void klog(int level, const char *subsystem, const char *fmt, ...)
{
    (void)level;
    (void)subsystem;
    (void)fmt;
}
/** Unused: vgem_probe returns before it. */
void drm_dev_init(struct drm_device *dev, const struct drm_driver *drv)
{
    dev->driver = drv;
}
/** Unused: vgem_probe returns before it. */
int32_t drm_dev_register(struct drm_device *dev)
{
    (void)dev;
    return -ENODEV;
}

/* The commit worker's queue of delayed works, which `expire` runs. */
static struct drm_work *works[8];
static uint32_t last_due_ms;

/** Queue `work` as the commit worker does, holding its device. */
void drm_dev_work_later_ms(struct drm_device *dev, struct drm_work *work, uint32_t ms)
{
    int i;

    last_due_ms = ms;
    if (work->queued)
        return;
    for (i = 0; i < 8 && works[i]; i++) {
    }
    CHECK(i < 8, "work queue has room");
    if (i == 8)
        return;
    drm_dev_get(dev);
    work->dev = dev;
    work->queued = 1;
    works[i] = work;
}

/** Take `work` off the queue, dropping its device. */
void drm_dev_work_cancel(struct drm_work *work)
{
    int i;

    if (!work->queued)
        return;
    for (i = 0; i < 8; i++) {
        if (works[i] == work)
            works[i] = NULL;
    }
    work->queued = 0;
    drm_dev_put(work->dev);
}

/** Run every queued work, as the worker does once their time has come. */
static void expire(void)
{
    struct drm_work *w;
    int i;

    for (i = 0; i < 8; i++) {
        if ((w = works[i]) != NULL) {
            works[i] = NULL;
            w->queued = 0;
            w->fn(w);
            drm_dev_put(w->dev);
        }
    }
}

static const struct drm_driver driver = {.ident = DRM_IDENT_VKMS,
                                         .dumb_create = drm_gem_shmem_dumb_create};
static struct drm_device exporter, importer;
static struct drm_file a, b, c;

/** Create a two-page shmem object, returning its handle and borrowed object. */
static struct drm_gem_object *new_object(uint32_t *handle)
{
    struct drm_gem_shmem_object *s = drm_gem_shmem_create(&exporter, 8192, 1);
    CHECK(s != NULL, "create source");
    CHECK(!drm_gem_handle_create(&a, &s->base, handle), "create source handle");
    drm_gem_object_put(&s->base);
    return &s->base;
}

/** Export one handle with a chosen descriptor mode. */
static int export_fd(struct drm_file *file, uint32_t handle, uint32_t flags)
{
    struct drm_prime_handle args = {0, 0, -1};
    args.handle = handle;
    args.flags = flags;
    CHECK(!drm_prime_handle_to_fd_ioctl(file->dev, &args, file), "export handle");
    return args.fd;
}

/** Import one descriptor, checking Linux's ignored import flags. */
static uint32_t import_fd(struct drm_file *file, int fd)
{
    struct drm_prime_handle args = {0, 0xffffffffu, -1};
    args.fd = fd;
    CHECK(!drm_prime_fd_to_handle_ioctl(file->dev, &args, file), "import fd");
    return args.handle;
}

/** Initialize one page of shared mapping at a chosen offset. */
static void mapping(vma_t *vma, uint32_t offset)
{
    k_memset(vma, 0, sizeof(*vma));
    vma->type = VM_SHARED;
    vma->start = PAGE_SIZE;
    vma->end = 2 * PAGE_SIZE;
    vma->pgoff = offset;
}

/** Check handle reuse, self-import and a second device's shared pages. */
static void sharing(void)
{
    uint32_t handle, other, cross;
    int fd, again, reexport;
    struct drm_gem_object *obj = new_object(&handle), *imported;
    struct dma_buf *buf;
    struct dma_fence *f = kzalloc(sizeof(*f));
    vma_t vma, direct;
    uintptr_t phys;

    fd = export_fd(&a, handle, DRM_CLOEXEC | DRM_RDWR);
    again = export_fd(&a, handle, 0);
    buf = dma_buf_get(fd);
    CHECK(buf == obj->dma_buf && slots[again].priv == buf, "exports share identity");
    CHECK((slots[fd].flags & FD_FLAG_CLOEXEC) && !(slots[again].flags & FD_FLAG_CLOEXEC),
          "close-on-exec belongs to the descriptor");
    CHECK(slots[again].flags & FD_FLAG_WRITE, "reexport preserves first export access");
    CHECK(import_fd(&a, fd) == handle, "self-import returns original handle");
    other = import_fd(&b, fd);
    CHECK(drm_handle_find(&b.objects, other) == obj, "same device returns original object");
    CHECK(import_fd(&b, again) == other, "repeated import returns same handle");
    cross = import_fd(&c, fd);
    imported = drm_handle_find(&c.objects, cross);
    CHECK(imported != obj && imported->import == buf, "other device imports buffer");
    CHECK(imported->resv == obj->resv, "both objects share implicit fences");
    dma_fence_init(f, NULL, dma_fence_context_alloc(1), 1);
    CHECK(!dma_resv_add_fence(imported->resv, f, 1) && obj->resv->write == f,
          "imported producer reaches source reservation");
    dma_fence_signal(f);
    dma_fence_put(f);
    reexport = export_fd(&c, cross, DRM_RDWR);
    CHECK(slots[reexport].priv == buf, "import reexports original dma-buf");
    {
        struct drm_mode_map_dumb m;
        k_memset(&m, 0, sizeof(m));
        m.handle = cross;
        CHECK(drm_mode_mmap_dumb_ioctl(&importer, &m, &c) == -EINVAL,
              "MAP_DUMB refuses imported objects");
    }
    CHECK(!drm_gem_create_mmap_offset(imported), "driver can map imported object");
    mapping(&vma, (uint32_t)(imported->mmap_offset >> PAGE_SHIFT));
    CHECK(!drm_gem_mmap(&c, &vma), "map import through GEM");
    mapping(&direct, 0);
    CHECK(!slots[fd].ops->mmap(&slots[fd], &direct), "map dma-buf descriptor");
    phys = mapped_page(&vma, 0);
    CHECK(phys && phys == mapped_page(&direct, 0), "both mappings share physical page");
    CHECK(((struct model_region *)vma.file)->cache == VM_PROT_WC,
          "import retains export cache type");
    *(uint32_t *)phys = 0xaabbccddu;
    drm_gem_release(&a);
    CHECK(import_fd(&b, fd) == other, "export survives exporter file exit");
    fd_free(&table, fd);
    fd_free(&table, again);
    fd_free(&table, reexport);
    drm_gem_release(&b);
    drm_gem_release(&c);
    dma_buf_put(buf);
    CHECK(mapped_page(&vma, 0) == phys && *(uint32_t *)mapped_page(&direct, 0) == 0xaabbccddu,
          "mappings survive all descriptors and handles");
    unmap(&direct);
    CHECK(exporter.objects && importer.objects, "import mapping keeps both devices alive");
    exporter.unplugged = 1;
    CHECK(!mapped_page(&vma, 0), "source device loss refuses later import faults");
    exporter.unplugged = 0;
    unmap(&vma);
    CHECK(!exporter.objects && !importer.objects && !frames_live,
          "last mapping frees imported and exported storage");
}

/** Check cache removal, handle reuse and the mode of a read-only export. */
static void caches(void)
{
    uint32_t handle, imported, replacement;
    int fd, back;
    struct drm_gem_object *obj = new_object(&handle), *different;
    struct dma_buf *buf;
    fd = export_fd(&a, handle, 0);
    CHECK(!(slots[fd].flags & FD_FLAG_WRITE), "first export without RDWR is read-only");
    buf = dma_buf_get(fd);
    imported = import_fd(&c, fd);
    CHECK(!drm_gem_handle_delete(&c, imported), "close imported handle");
    different = new_object(&replacement);
    CHECK(different != obj, "second source object is distinct");
    CHECK(!drm_gem_handle_create(&c, different, &replacement) && replacement == imported,
          "closed handle number can be reused");
    imported = import_fd(&c, fd);
    CHECK(imported != replacement && drm_handle_find(&c.objects, imported) != different,
          "cache never returns reused unrelated handle");
    drm_gem_release(&a);
    back = export_fd(&c, imported, DRM_RDWR);
    CHECK(slots[back].priv == buf, "export after original handle closed keeps identity");
    fd_free(&table, back);
    fd_free(&table, fd);
    dma_buf_put(buf);
    drm_gem_release(&c);
    CHECK(!exporter.objects && !importer.objects, "cache references released at close");
}

/** Import a real udmabuf made from modeled sealed memfd pages. */
static void udmabuf_import(void)
{
    struct udmabuf_create_item item = {0, 0, 0, 8192};
    struct drm_gem_object *obj;
    uint32_t handle;
    int memfd = fd_alloc(&table), fd, back;
    vma_t vma;
    uintptr_t phys;
    slots[memfd].type = FD_TYPE_FILE;
    memfd_mount.type = &vfs_tmpfs_type;
    descriptions[memfd].vmnt = &memfd_mount;
    memfd_pages.size = 8192;
    item.memfd = (uint32_t)memfd;
    fd = udma_create(UDMABUF_FLAGS_CLOEXEC, 1, &item);
    CHECK(fd >= 0 && pins == 2, "udmabuf pins both memfd pages");
    handle = import_fd(&c, fd);
    CHECK(import_fd(&c, fd) == handle, "udmabuf import cache");
    obj = drm_handle_find(&c.objects, handle);
    CHECK(obj->resv == obj->import->resv, "udmabuf reservation shared");
    CHECK(!drm_gem_create_mmap_offset(obj), "udmabuf GEM offset");
    mapping(&vma, (uint32_t)(obj->mmap_offset >> PAGE_SHIFT));
    CHECK(!drm_gem_mmap(&c, &vma), "map udmabuf import");
    phys = mapped_page(&vma, 1);
    CHECK(phys == shmem_resident(&memfd_pages, 1), "import reads original memfd pages");
    back = export_fd(&c, handle, DRM_RDWR);
    CHECK(slots[back].priv == slots[fd].priv, "udmabuf reexport identity");
    fd_free(&table, memfd);
    shmem_free(&memfd_pages);
    fd_free(&table, fd);
    fd_free(&table, back);
    drm_gem_release(&c);
    CHECK(frames_live == 2 && mapped_page(&vma, 1) == phys,
          "pins outlive memfd, dma-buf fds and handles");
    unmap(&vma);
    CHECK(!frames_live, "last udmabuf import mapping frees pinned frames");
}

/** Fail each allocation in export and import, then retry on the same files. */
static void failures_and_flags(void)
{
    struct drm_prime_handle args = {123, 4, -1};
    struct drm_gem_object *obj;
    uint32_t handle;
    int fd, i, rc, filled[64], count;
    CHECK(drm_prime_handle_to_fd_ioctl(&exporter, &args, &a) == -EINVAL,
          "export refuses unknown flags");
    args.flags = 0;
    CHECK(drm_prime_handle_to_fd_ioctl(&exporter, &args, &a) == -ENOENT,
          "export refuses unknown handle");
    CHECK(drm_prime_fd_to_handle_ioctl(&exporter, &args, &a) == -EBADF,
          "import refuses absent descriptor");
    fd = fd_alloc(&table);
    slots[fd].type = FD_TYPE_FILE;
    args.fd = fd;
    CHECK(drm_prime_fd_to_handle_ioctl(&exporter, &args, &a) == -EINVAL,
          "import refuses ordinary file");
    fd_free(&table, fd);
    obj = new_object(&handle);
    args.handle = handle;
    fail_after = 0;
    CHECK(drm_prime_handle_to_fd_ioctl(&exporter, &args, &a) == -ENOMEM && !obj->dma_buf,
          "export allocation failure retains source");
    fail_after = -1;
    fd = export_fd(&a, handle, DRM_RDWR);
    args.fd = fd;
    slots[fd].flags |= FD_FLAG_PATH;
    CHECK(drm_prime_fd_to_handle_ioctl(&importer, &args, &c) == -EINVAL,
          "import refuses O_PATH descriptors");
    slots[fd].flags &= ~FD_FLAG_PATH;
    for (count = 0; count < 64; count++) {
        filled[count] = fd_alloc(&table);
        if (filled[count] < 0)
            break;
        slots[filled[count]].type = FD_TYPE_FILE;
    }
    CHECK(drm_prime_handle_to_fd_ioctl(&exporter, &args, &a) == -EMFILE,
          "descriptor exhaustion preserves cached buffer");
    for (i = 0; i < count; i++)
        fd_free(&table, filled[i]);
    for (i = 0; i < 2; i++) {
        fail_after = i;
        rc = drm_prime_fd_to_handle_ioctl(&importer, &args, &c);
        fail_after = -1;
        CHECK(rc == -ENOMEM && !drm_handle_next(&c.objects, 1) && !importer.objects,
              "import allocation failure unwinds all references");
    }
    CHECK(!drm_prime_fd_to_handle_ioctl(&importer, &args, &c), "retry failed import");
    drm_gem_release(&c);
    drm_gem_release(&a);
    fd_free(&table, fd);
    CHECK(!exporter.objects && !importer.objects, "failure paths leave no objects");
}

static int hook_fd, hook_entered;
static uint32_t raced_handle;

/** Simulate another import completing while the first hook sleeps. */
static struct drm_gem_object *racing_import(struct drm_device *dev, struct dma_buf *buf,
                                            int32_t *err)
{
    struct drm_gem_shmem_object *s;
    if (!hook_entered) {
        hook_entered = 1;
        raced_handle = import_fd(&c, hook_fd);
    }
    s = drm_gem_shmem_create(dev, buf->size, 0);
    if (!s) {
        *err = -ENOMEM;
        return NULL;
    }
    return &s->base;
}

/** A sleeping driver import must recheck the file's cache before publishing. */
static int unmappable_freed;

/** The free of an object without `mmap`. */
static void unmappable_free(struct drm_gem_object *obj)
{
    (void)obj;
    unmappable_freed++;
}

static const struct drm_gem_object_funcs unmappable_funcs = {
    unmappable_free, NULL, NULL, NULL, NULL, NULL, NULL, NULL};

/** MAP_DUMB of an object without `mmap`, as i915's adopted firmware
 * framebuffer is: ENODEV, as Linux's i915 refuses a stolen object. */
static void unmappable(void)
{
    static struct drm_gem_object obj;
    struct drm_mode_map_dumb m;
    uint32_t h = 0;

    drm_gem_object_init(&importer, &obj, PAGE_SIZE, &unmappable_funcs);
    CHECK(drm_gem_handle_create(&c, &obj, &h) == 0, "a handle on an object without mmap");
    drm_gem_object_put(&obj);
    k_memset(&m, 0, sizeof(m));
    m.handle = h;
    CHECK(drm_mode_mmap_dumb_ioctl(&importer, &m, &c) == -ENODEV && m.offset == 0 &&
              obj.mmap_offset == 0,
          "MAP_DUMB refuses an object without mmap with ENODEV");
    CHECK(drm_gem_handle_delete(&c, h) == 0 && unmappable_freed == 1,
          "the object freed with its handle");
}

/** An import whose driver hook sleeps while another import of the same
 * buffer publishes its handle: the first gets that handle, and its own
 * object is freed with its reference on the buffer. */
static void concurrent_import(void)
{
    struct drm_driver custom = driver;
    uint32_t source, handle;
    new_object(&source);
    custom.gem_prime_import = racing_import;
    importer.driver = &custom;
    hook_fd = export_fd(&a, source, DRM_RDWR);
    handle = import_fd(&c, hook_fd);
    CHECK(handle == raced_handle && drm_handle_next(&c.objects, 1) == handle &&
              !drm_handle_next(&c.objects, handle + 1),
          "concurrent import returns the handle already published");
    CHECK(importer.objects && !importer.objects->dev_next,
          "discarded concurrent object releases its buffer reference");
    drm_gem_release(&a);
    drm_gem_release(&c);
    fd_free(&table, hook_fd);
    importer.driver = &driver;
}

/** Attach a vgem fence to `handle`, returning the ioctl's result. */
static int32_t vgem_attach(struct drm_file *file, uint32_t handle, uint32_t flags, uint32_t *id)
{
    struct drm_vgem_fence_attach arg;
    int32_t rc;

    k_memset(&arg, 0, sizeof(arg));
    arg.handle = handle;
    arg.flags = flags;
    rc = vgem_fence_attach_ioctl(file->dev, &arg, file);
    if (id)
        *id = arg.out_fence;
    return rc;
}

/** Signal vgem fence `id`, returning the ioctl's result. */
static int32_t vgem_signal(struct drm_file *file, uint32_t id, uint32_t flags)
{
    struct drm_vgem_fence_signal arg;

    arg.fence = id;
    arg.flags = flags;
    return vgem_fence_signal_ioctl(file->dev, &arg, file);
}

/** Whether the reservation of `obj` has work pending for a writer
 * (`write`) or a reader. */
static int resv_busy(struct drm_gem_object *obj, int write)
{
    struct dma_fence *f = dma_resv_snapshot(obj->resv, write);
    int busy = f && !dma_fence_is_signaled(f);

    dma_fence_put(f);
    return busy;
}

/**
 * vgem as IGT's cork uses it: a write-combined dumb buffer, exported and
 * imported by another device, held busy by a vgem fence until it is
 * signalled, and Linux's answers for every misuse, the timeout and the
 * close of a file with fences pending.
 */
static void vgem_fences(void)
{
    static const struct drm_driver vgem = {DRM_IDENT_VGEM,   vgem_open, vgem_postclose,
                                           vgem_dumb_create, NULL,      NULL,
                                           vgem_ioctls,      2,         NULL};
    struct drm_device vdev;
    struct drm_file v;
    struct drm_mode_create_dumb dumb;
    struct drm_gem_object *obj, *imported;
    uint32_t handle, cross, id, id2, id3;
    int fd;

    k_memset(&vdev, 0, sizeof(vdev));
    k_memset(&v, 0, sizeof(v));
    vdev.driver = &vgem;
    vdev.features = DRIVER_GEM | DRIVER_RENDER;
    vdev.refs = 1;
    v.dev = &vdev;
    CHECK(!vgem_open(&vdev, &v), "vgem open");
    CHECK(vgem_signal(&v, 0, 0) == -ENOENT, "fence 0 is ENOENT, IGT's vgem_has_fences");
    CHECK(vgem_signal(&v, 1, 0) == -ENOENT, "an id never given is ENOENT");
    k_memset(&dumb, 0, sizeof(dumb));
    dumb.width = dumb.height = 1;
    dumb.bpp = 32;
    CHECK(!vgem_dumb_create(&v, &vdev, &dumb) && dumb.size == PAGE_SIZE && dumb.pitch == 4,
          "dumb buffer of one pixel takes a page");
    handle = dumb.handle;
    obj = drm_handle_find(&v.objects, handle);
    CHECK(obj && obj->map_wc, "vgem maps its buffers write-combined");

    CHECK(vgem_attach(&v, handle, 2, NULL) == -EINVAL, "unknown flag is EINVAL");
    {
        struct drm_vgem_fence_attach arg;
        k_memset(&arg, 0, sizeof(arg));
        arg.handle = handle;
        arg.pad = 1;
        CHECK(vgem_fence_attach_ioctl(&vdev, &arg, &v) == -EINVAL, "pad is EINVAL");
    }
    CHECK(vgem_attach(&v, handle + 100, 0, NULL) == -ENOENT, "no such handle is ENOENT");
    CHECK(!works[0] && !works[1], "a refused attach leaves no timer");

    fd = export_fd(&v, handle, DRM_RDWR);
    cross = import_fd(&c, fd);
    imported = drm_handle_find(&c.objects, cross);
    CHECK(imported && imported->resv == obj->resv, "importer shares vgem's reservation");

    CHECK(!vgem_attach(&v, handle, VGEM_FENCE_WRITE, &id) && id == 1, "write fence is id 1");
    CHECK(last_due_ms == VGEM_FENCE_TIMEOUT_MS && works[0] && works[0]->queued,
          "fence expires in ten seconds");
    CHECK(resv_busy(imported, 0) && resv_busy(imported, 1),
          "importer's readers and writers wait for the vgem writer");
    CHECK(vgem_attach(&v, handle, 0, NULL) == -EBUSY, "reader behind a writer is EBUSY");
    CHECK(vgem_attach(&v, handle, VGEM_FENCE_WRITE, NULL) == -EBUSY,
          "writer behind a writer is EBUSY");
    CHECK(vgem_signal(&v, 1, 1) == -EINVAL, "signal flags are EINVAL");
    CHECK(!vgem_signal(&v, 1, 0), "signal the writer");
    CHECK(!works[0] && !works[1], "signal stops the timer");
    CHECK(!resv_busy(imported, 1), "importer is idle once the writer signals");
    CHECK(vgem_signal(&v, 1, 0) == -ENOENT, "a consumed id is ENOENT");

    CHECK(!vgem_attach(&v, handle, 0, &id2) && id2 == 2, "reader is id 2, ids not reused");
    CHECK(!vgem_attach(&v, handle, 0, &id3) && id3 == 3, "readers share a buffer");
    CHECK(!resv_busy(imported, 0) && resv_busy(imported, 1), "readers hold back writers alone");
    CHECK(vgem_attach(&v, handle, VGEM_FENCE_WRITE, NULL) == -EBUSY,
          "writer behind readers is EBUSY");
    CHECK(!vgem_signal(&v, id2, 0), "signal one reader");
    expire();
    CHECK(!resv_busy(imported, 1), "the other reader signals by itself at its timeout");
    CHECK(vgem_signal(&v, id3, 0) == -ETIMEDOUT, "signal after the timeout is ETIMEDOUT");

    CHECK(!vgem_attach(&v, handle, VGEM_FENCE_WRITE, &id) && id == 4, "write fence is id 4");
    CHECK(resv_busy(imported, 0), "importer waits for the new writer");
    fd_free(&table, fd);
    drm_gem_release(&c);
    drm_gem_release(&v);
    vgem_postclose(&vdev, &v);
    CHECK(!works[0] && !works[1], "close stops the pending fence's timer");
    CHECK(!v.driver_priv && vdev.refs == 1, "close frees the table and the device's holds");
    CHECK(!vdev.objects, "the buffer goes with its last handle and import");
}

/** Run only the PRIME rows with each kernel ABI's structures. */
int main(void)
{
    table.entries = slots;
    table.cap = 64;
    self.fds = &table;
    cpu_table[0].current = &self;
    exporter.driver = &driver;
    exporter.features = DRIVER_GEM;
    exporter.refs = 1;
    importer.driver = &driver;
    importer.features = DRIVER_GEM;
    importer.refs = 1;
    a.dev = &exporter;
    b.dev = &exporter;
    c.dev = &importer;
    sharing();
    caches();
    udmabuf_import();
    failures_and_flags();
    concurrent_import();
    unmappable();
    vgem_fences();
    CHECK(exporter.refs == 1 && importer.refs == 1, "device references return to baseline");
    CHECK(!allocations && !frames_live, "all allocations and frames released");
    printf("drm-prime-test: %s\n", failures ? "failed" : "ok");
    return failures != 0;
}
