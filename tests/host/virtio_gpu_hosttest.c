/*
 * virtio_gpu_hosttest.c: the virtio GPU driver, rach2/sys/drm/virtio_gpu.c,
 * against a model of QEMU's virtio-gpu behind the virtio core's calls: its
 * configuration, two queues that answer at once, on the next reap or never,
 * and the 2D commands over resources whose backing is host memory;
 * virtio_gpu64_hosttest.c runs the same rows with x86-64's structures.
 *
 * host-deps: rach2/sys/lib/kstring.c rach2/sys/lib/kctypes.c
 *
 * The rows: a bind with EDID, which reads the EDID and then the displays,
 * fences each command with the next id, and lists Linux's 26 modes of
 * QEMU's EDID with the EDID property, one CRTC, a primary and a cursor
 * plane and no IN_FORMATS; a bind without EDID, whose preferred mode is the
 * display's size among the DMT modes; a dumb buffer whose 2D resource is
 * backed by one entry for adjacent pages; SETCRTC, which sets the scanout,
 * copies the whole buffer to the host and flushes it; DIRTYFB, which copies
 * and flushes the clip alone, from its offset in the buffer; scattered
 * pages, whose entries go on past the command's page into extra pages; a
 * queue too small for the commands, which holds the rest until answers
 * make room, in order; an error answer, which fails the fence with EIO; a
 * device that never answers, whose waiter gives up after five seconds and
 * whose late answer the reaper frees; the cursor, set with its hotspot and
 * then moved; a display change, served at the next open, which rereads the
 * EDID and the displays, clears the event and sends one hotplug; the
 * driver's ioctls; a buffer's last reference, which unrefs its resource
 * and frees its pages once the host answers; virtio-vga, which removes the
 * firmware's framebuffer from its memory BARs; a device with no scanout,
 * which has no KMS; and an unplug, which fails the commands still out with
 * ENODEV and leaves nothing allocated once the last reference goes.
 *
 * Returns 0 when every row passes, 1 otherwise.
 */

#include "drm/drm_mode.c"
#include "drm/drm_atomic.c"
#include "drm/drm_legacy.c"
#include "drm/drm_edid.c"
#include "drm/drm_vblank.c"
#include "dmabuf/fence.c"
#include "drm/virtio_gpu.c"
#include "drm/virtio_gpu_fence.c"
#include "drm/virtio_gpu_3d.c"
#include "drm/virtio_gpu_submit.c"
#include "drm/virtio_gpu_blob.c"

extern int printf(const char *format, ...);
extern void *calloc(unsigned long n, unsigned long size);
extern void free(void *p);
extern int strcmp(const char *a, const char *b);

struct percpu cpu_table[NR_CPUS];
static process_t self;
static int failures;
#ifdef VGPU_3D_TEST
static void model_wait_step(void);
#endif

#define CHECK(c, s)                                     \
    do {                                                \
        if (!(c)) {                                     \
            printf("virtio-gpu-test: fail: %s\n", (s)); \
            failures++;                                 \
        }                                               \
    } while (0)

/* QEMU 10.2.1's EDID at its default 1280x800, the base block and a CEA
 * extension, as tests/host/drm_edid_hosttest.c has it. */
static const uint8_t qemu_edid[256] = {
    0x00, 0xff, 0xff, 0xff, 0xff, 0xff, 0xff, 0x00, 0x49, 0x14, 0x34, 0x12, 0x00, 0x00, 0x00, 0x00,
    0x2a, 0x18, 0x01, 0x04, 0xa5, 0x20, 0x14, 0x78, 0x06, 0xee, 0x91, 0xa3, 0x54, 0x4c, 0x99, 0x26,
    0x0f, 0x50, 0x54, 0x21, 0x08, 0x00, 0xe1, 0xc0, 0xd1, 0xc0, 0xd1, 0x00, 0xa9, 0x40, 0xb3, 0x00,
    0x95, 0x00, 0x81, 0x80, 0x81, 0x40, 0xea, 0x29, 0x00, 0xc0, 0x51, 0x20, 0x1c, 0x30, 0x40, 0x26,
    0x44, 0x40, 0x45, 0xcb, 0x10, 0x00, 0x00, 0x18, 0x00, 0x00, 0x00, 0xf7, 0x00, 0x0a, 0x00, 0x40,
    0x82, 0x00, 0x28, 0x20, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0xfd, 0x00, 0x32,
    0x7d, 0x1e, 0xa0, 0xff, 0x01, 0x0a, 0x20, 0x20, 0x20, 0x20, 0x20, 0x20, 0x00, 0x00, 0x00, 0xfc,
    0x00, 0x51, 0x45, 0x4d, 0x55, 0x20, 0x4d, 0x6f, 0x6e, 0x69, 0x74, 0x6f, 0x72, 0x0a, 0x01, 0x3a,
    0x02, 0x03, 0x0b, 0x00, 0x46, 0x7d, 0x65, 0x60, 0x59, 0x1f, 0x61, 0x00, 0x00, 0x00, 0x10, 0x00,
    0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00,
    0x10, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00,
    0x00, 0x00, 0x10, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00,
    0x00, 0x00, 0x00, 0x00, 0x10, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00,
    0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x10, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00,
    0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x10, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00,
    0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x2f,
};

/* ------------------------------------------------------------------------
 * The kernel around the driver
 */

static int allocations, fail_after = -1;

/** Zeroed host memory, counted; the `fail_after`-th next call fails. */
void *kmalloc(size_t size)
{
    void *p;

    if (fail_after == 0) {
        return NULL;
    }
    if (fail_after > 0) {
        fail_after--;
    }
    p = calloc(1, size ? size : 1);
    if (p) {
        allocations++;
    }
    return p;
}

/** Zeroed memory over the same count. */
void *kzalloc(size_t size)
{
    return kmalloc(size);
}

/** Free and uncount; NULL is allowed. */
void kfree(void *p)
{
    if (p) {
        allocations--;
    }
    free(p);
}

/** Copy from a test address; one below 4096 faults. */
int copy_from_user(void *dst, const void __user *src, size_t n)
{
    if ((uintptr_t)src < 4096) {
        return -EFAULT;
    }
    k_memcpy(dst, (const void *)src, n);
    return 0;
}

/** Copy to a test address; one below 4096 faults. */
int copy_to_user(void __user *dst, const void *src, size_t n)
{
    if ((uintptr_t)dst < 4096) {
        return -EFAULT;
    }
    k_memcpy((void *)dst, src, n);
    return 0;
}

/** Copy a string from a test address, at most `n` bytes; one below 4096
 * faults. The length, or -ENAMETOOLONG with no NUL among the `n`. */
int strncpy_from_user(char *dst, const char __user *src, uint32_t n)
{
    uint32_t i;

    if ((uintptr_t)src < 4096) {
        return -EFAULT;
    }
    for (i = 0; i < n; i++) {
        dst[i] = ((const char *)src)[i];
        if (!dst[i]) {
            return (int)i;
        }
    }
    return -ENAMETOOLONG;
}

#define UPTR(p) ((uint64_t)(uintptr_t)(p))

static char last_log[256];
static int warnings;

/** Keep the record's subsystem and format, and count warnings and worse. */
void klog(int level, const char *subsystem, const char *format, ...)
{
    uint32_t n = 0, i;

    for (i = 0; subsystem[i] && n < sizeof(last_log) - 2; i++) {
        last_log[n++] = subsystem[i];
    }
    last_log[n++] = ':';
    for (i = 0; format[i] && n < sizeof(last_log) - 1; i++) {
        last_log[n++] = format[i];
    }
    last_log[n] = '\0';
    if (level <= KLOG_WARN) {
        warnings++;
    }
}

static int64_t now_ns = 5000000000ll;
static uint32_t udelays;

/** The model's monotonic clock. */
void rtc_monotonic(int64_t *sec, int64_t *nsec)
{
    *sec = now_ns / 1000000000ll;
    *nsec = now_ns % 1000000000ll;
}

/** Ticks of a millisecond. */
uint64_t rtc_ticks_now(void)
{
    return (uint64_t)(now_ns / 1000000);
}

/** Host arithmetic for the kernel's division helper. */
uint64_t tick_div(uint64_t n, uint32_t d, uint32_t *rem)
{
    if (rem) {
        *rem = (uint32_t)(n % d);
    }
    return n / d;
}

/** A busy wait: the clock advances and nothing else runs. */
void k_udelay(uint32_t us)
{
    udelays++;
    now_ns += (int64_t)us * 1000;
}

/** The scheduler is off, as at boot, so a waiter polls. */
int sched_is_enabled(void)
{
    return 0;
}

static int irq_depth;

/** Count a lock taken with interrupts off. */
uint32_t spin_lock_irqsave(struct spinlock *lock)
{
    (void)lock;
    irq_depth++;
    return 0;
}

/** Count it given back. */
void spin_unlock_irqrestore(struct spinlock *lock, uint32_t flags)
{
    (void)lock;
    (void)flags;
    CHECK(irq_depth > 0, "a lock is given back no more than it is taken");
    irq_depth--;
}

/** Initialize a queue. */
void wait_queue_init(wait_queue_t *wq)
{
    k_memset(wq, 0, sizeof(*wq));
}

/** A wake, which nothing waits for. */
void wq_wake_all(wait_queue_t *wq)
{
    (void)wq;
}

/** A wait whose condition must already hold, since nothing else runs. */
int wq_wait_event(wait_queue_t *wq, wq_cond_fn cond, void *arg)
{
    (void)wq;
#ifdef VGPU_3D_TEST
    model_wait_step();
#endif
    CHECK(cond(arg), "a wait's condition holds in one thread");
    return 0;
}

/** The same, uninterruptible. */
int wq_wait_event_nointr(wait_queue_t *wq, wq_cond_fn cond, void *arg)
{
    return wq_wait_event(wq, cond, arg);
}

/** Reserve `e` on `file`, as drm.c does. */
int32_t drm_event_reserve(struct drm_file *file, struct drm_pending_event *e)
{
    if (file->event_space < e->length) {
        return -ENOMEM;
    }
    file->event_space -= e->length;
    e->file = file;
    return 0;
}

/* Events sent and taken back, and the type of the last sent. */
static uint32_t events_sent, events_cancelled, last_event_type;

/** Give back a reserved event's space, if it has a file, and free it. */
static void event_free(struct drm_pending_event *e)
{
    if (e->file) {
        e->file->event_space += e->length;
    }
    kfree(e);
}

/** Signal an event's fence, count it, and free it, as if read. */
void drm_send_event(struct drm_pending_event *e)
{
    struct dma_fence *fence = e->fence;

    e->fence = NULL;
    if (fence) {
        dma_fence_signal(fence);
        dma_fence_put(fence);
    }
    events_sent++;
    last_event_type = e->event.base.type;
    event_free(e);
}

/** Take back a reserved event. */
void drm_event_cancel_free(struct drm_pending_event *e)
{
    dma_fence_put(e->fence);
    events_cancelled++;
    event_free(e);
}

#ifndef VGPU_3D_TEST
/** No sync files. */
struct dma_fence *sync_file_get_fence(int32_t fd)
{
    (void)fd;
    return NULL;
}

/** No descriptor table: a sync file cannot be made. */
int32_t sync_file_create(struct dma_fence *fence, const char *name)
{
    (void)fence;
    (void)name;
    return -EMFILE;
}

#endif

/** Nothing to free. */
int fd_free(fd_table_t *table, int fd)
{
    (void)table;
    (void)fd;
    return 0;
}

/* ---- the device's record and its objects, as drm.c and drm_gem.c ---- */

static int hotplug_events, registered, released, unplugged_calls;

/** Fill a device for `driver`, as drm.c does. */
void drm_dev_init(struct drm_device *d, const struct drm_driver *driver)
{
    k_memset(d, 0, sizeof(*d));
    d->driver = driver;
    d->features = driver->ident.features;
    d->index = -1;
    d->refs = 1;
}

/** Register as card 0. */
int32_t drm_dev_register(struct drm_device *d)
{
    d->index = 0;
    d->registered = 1;
    registered++;
    return 0;
}

/** Take a reference. */
void drm_dev_get(struct drm_device *d)
{
    d->refs++;
}

/** Drop one; the last calls the driver's release. */
void drm_dev_put(struct drm_device *d)
{
    if (d && --d->refs == 0) {
        released++;
        d->driver->release(d);
    }
}

/** Count a hotplug uevent. */
void drm_kms_hotplug_event(struct drm_device *d)
{
    (void)d;
    hotplug_events++;
}

/** No connector has a sysfs file here. */
void drm_sysfs_connector_hotplug_event(struct drm_connector *connector)
{
    (void)connector;
}

/** No in-kernel client listens here. */
void drm_client_dev_hotplug(struct drm_device *dev)
{
    (void)dev;
}

/** One thread: interrupts need no masking. */
uint32_t kirq_save(void)
{
    return 0;
}

/** Nothing was masked. */
void kirq_restore(uint32_t flags)
{
    (void)flags;
}

/** An object of `dev`, as drm_gem.c makes one. */
void drm_gem_object_init(struct drm_device *d, struct drm_gem_object *obj, uint64_t size,
                         const struct drm_gem_object_funcs *funcs)
{
    k_memset(obj, 0, sizeof(*obj));
    obj->refs = 1;
    obj->dev = d;
    obj->size = size;
    obj->funcs = funcs;
    obj->resv = &obj->resv_own;
    drm_dev_get(d);
}

/** Take a reference. */
void drm_gem_object_get(struct drm_gem_object *obj)
{
    if (obj) {
        obj->refs++;
    }
}

static void resv_drop(struct dma_resv *resv);

/** Drop one; the last frees it through its hooks and drops its device. */
void drm_gem_object_put(struct drm_gem_object *obj)
{
    struct drm_device *d;

    if (!obj || --obj->refs > 0) {
        return;
    }
    d = obj->dev;
    resv_drop(&obj->resv_own);
    obj->funcs->free(obj);
    drm_dev_put(d);
}

/* Handles, each also on its file's list of `struct drm_handle`, where
 * PRIME keeps its export. `prime_drop` drops a deleted handle's export, in
 * the rows that have them. */
#define MAX_HANDLES 32
static struct {
    struct drm_file *file;
    uint32_t id;
    struct drm_gem_object *obj;
    struct drm_handle node;
} handles[MAX_HANDLES];
static void (*prime_drop)(struct dma_buf *buf);

/** The lowest free handle of `file` from 1, holding `obj`. */
int32_t drm_gem_handle_create(struct drm_file *file, struct drm_gem_object *obj, uint32_t *handle)
{
    uint32_t id;
    int i, slot = -1, taken;

    for (id = 1;; id++) {
        taken = 0;
        for (i = 0; i < MAX_HANDLES; i++) {
            taken |= handles[i].file == file && handles[i].id == id;
        }
        if (!taken) {
            break;
        }
    }
    for (i = 0; i < MAX_HANDLES && slot < 0; i++) {
        if (!handles[i].file) {
            slot = i;
        }
    }
    if (slot < 0) {
        return -ENOMEM;
    }
    handles[slot].file = file;
    handles[slot].id = id;
    handles[slot].obj = obj;
    k_memset(&handles[slot].node, 0, sizeof(handles[slot].node));
    handles[slot].node.id = id;
    handles[slot].node.object = obj;
    handles[slot].node.next = file->objects;
    file->objects = &handles[slot].node;
    obj->refs++;
    *handle = id;
    return 0;
}

/** The object of `handle`, held, or NULL. */
struct drm_gem_object *drm_gem_object_lookup(struct drm_file *file, uint32_t handle)
{
    int i;

    for (i = 0; i < MAX_HANDLES; i++) {
        if (handles[i].file == file && handles[i].id == handle) {
            handles[i].obj->refs++;
            return handles[i].obj;
        }
    }
    return NULL;
}

/** Drop `handle`: 0 or -EINVAL. */
int32_t drm_gem_handle_delete(struct drm_file *file, uint32_t handle)
{
    int i;

    for (i = 0; i < MAX_HANDLES; i++) {
        if (handles[i].file == file && handles[i].id == handle) {
            struct drm_handle **p;

            for (p = &file->objects; *p && *p != &handles[i].node; p = &(*p)->next) {
            }
            if (*p) {
                *p = handles[i].node.next;
            }
            if (handles[i].node.prime && prime_drop) {
                prime_drop(handles[i].node.prime);
            }
            handles[i].file = NULL;
            drm_gem_object_put(handles[i].obj);
            return 0;
        }
    }
    return -EINVAL;
}

/** Give `obj` an offset in the mmap space, once. */
int32_t drm_gem_create_mmap_offset(struct drm_gem_object *obj)
{
    static uint64_t next = 0x10000000ull;

    if (!obj->mmap_offset) {
        obj->mmap_offset = next;
        next += obj->size;
    }
    return 0;
}

/*
 * Frames: a pool of host pages, whose addresses are the "physical" ones
 * the device model reads. `scatter` leaves a page free between two given,
 * so that no two of an object's pages are adjacent.
 */
#define POOL_PAGES 4096
static uint8_t pool[POOL_PAGES * PAGE_SIZE] __attribute__((aligned(4096)));
static uint8_t pool_used[POOL_PAGES];
static int scatter, pages_held;

/** A free page of the pool, zeroed, or 0. */
static uintptr_t page_get(void)
{
    static uint32_t at;
    uint32_t i, k;

    for (k = 0; k < POOL_PAGES; k++) {
        i = (at + k) % POOL_PAGES;
        if (!pool_used[i]) {
            pool_used[i] = 1;
            at = i + (scatter ? 2 : 1);
            pages_held++;
            k_memset(pool + i * PAGE_SIZE, 0, PAGE_SIZE);
            return (uintptr_t)(pool + i * PAGE_SIZE);
        }
    }
    return 0;
}

/** Give back the page at `p`. */
static void page_put(uintptr_t p)
{
    pool_used[(p - (uintptr_t)pool) / PAGE_SIZE] = 0;
    pages_held--;
}

/* An object's pages, by its shmem record: leaves[0] is an array of frames
 * in the model. */
uintptr_t drm_gem_shmem_page(struct drm_gem_shmem_object *obj, uint32_t pgoff)
{
    struct shmem *sh = &obj->pages;
    uint32_t n = (uint32_t)(obj->base.size >> PAGE_SHIFT);

    if (pgoff >= n) {
        return 0;
    }
    if (!sh->leaves) {
        sh->leaves = (uintptr_t **)kzalloc(sizeof(uintptr_t *));
        sh->leaves[0] = (uintptr_t *)kzalloc(n * sizeof(uintptr_t));
        sh->nleaves = 1;
        sh->npages = n;
    }
    if (!sh->leaves[0][pgoff]) {
        sh->leaves[0][pgoff] = page_get();
        sh->held++;
    }
    return sh->leaves[0][pgoff];
}

/** A model object's page at `pgoff`, 0 for one not given yet. */
uintptr_t shmem_resident(const struct shmem *sh, uint32_t pgoff)
{
    return sh->leaves && pgoff < sh->npages ? sh->leaves[0][pgoff] : 0;
}

/** Free a model object's pages. */
void shmem_free(struct shmem *sh)
{
    uint32_t i;

    if (!sh->leaves) {
        return;
    }
    for (i = 0; i < sh->npages; i++) {
        if (sh->leaves[0][i]) {
            page_put(sh->leaves[0][i]);
        }
    }
    kfree(sh->leaves[0]);
    kfree(sh->leaves);
    sh->leaves = NULL;
    sh->held = 0;
}

static uint32_t maps_cache;

/** A device mapping: its cache type kept; the reference is the region's. */
int vm_device_map(vma_t *vma, const struct vm_device_ops *ops, void *obj, uint32_t cache)
{
    (void)vma;
    maps_cache = cache;
    ops->put(obj);
    return 0;
}

/* ---- reservations: each object's fences, kept here ---- */

#define MAX_RESV 16
static struct {
    struct dma_resv *resv;
    struct dma_fence *fence;
} resvs[MAX_RESV];

/** Keep `fence` on `resv`, held. */
int32_t dma_resv_add_fence(struct dma_resv *resv, struct dma_fence *fence, int write)
{
    int i;

    (void)write;
    for (i = 0; i < MAX_RESV; i++) {
        if (!resvs[i].resv) {
            resvs[i].resv = resv;
            resvs[i].fence = dma_fence_get(fence);
            return 0;
        }
    }
    return -ENOMEM;
}

#ifndef VGPU_3D_TEST
/** The 2D rows never create sync objects. */
struct drm_syncobj *drm_syncobj_find(struct drm_file *file, uint32_t h)
{
    (void)file;
    (void)h;
    return NULL;
}

void drm_syncobj_put(struct drm_syncobj *s)
{
    (void)s;
}
void drm_syncobj_replace_fence(struct drm_syncobj *s, struct dma_fence *f)
{
    (void)s;
    (void)f;
}
void drm_syncobj_add_point(struct drm_syncobj *s, struct dma_fence_chain *c, struct dma_fence *f,
                           uint64_t p)
{
    (void)s;
    (void)c;
    (void)f;
    (void)p;
}
int32_t drm_syncobj_find_fence(struct drm_file *file, uint32_t h, uint64_t p, uint32_t flags,
                               struct dma_fence **f)
{
    (void)file;
    (void)h;
    (void)p;
    (void)flags;
    (void)f;
    return -ENOENT;
}
#endif

/** The core's path of a driver's file in the device's debugfs directory,
 * which the model names DEV for every device. */
void drm_debugfs_path(const struct drm_device *dev, const char *name, char *path, uint32_t cap)
{
    (void)dev;
    k_strncpy(path, "dri/DEV/", cap);
    k_strncpy(path + 8, name, cap - 8u);
    path[cap - 1u] = '\0';
}

/** Register the reset trigger, keeping its operations in the 3D model. */
#ifndef VGPU_3D_TEST
int32_t debugfs_create_file(const char *path, uint32_t mode, const struct file_ops *ops,
                            debugfs_open_fn open, void *data)
{
    (void)path;
    (void)mode;
    (void)ops;
    (void)open;
    (void)data;
    return 0;
}
void debugfs_remove(const char *path)
{
    (void)path;
}
void drm_dev_unplug(struct drm_device *d)
{
    if (!d->unplugged) {
        d->unplugged = 1;
        vgpu_unplug(d);
        drm_dev_put(d);
    }
}
#endif

static const struct dma_fence_ops model_fence_ops = {fence_driver, fence_timeline, NULL, NULL};

/** The first unsignalled fence of `resv`, held, or a signalled one. */
struct dma_fence *dma_resv_snapshot(struct dma_resv *resv, int write)
{
    struct dma_fence *f;
    int i;

    (void)write;
    for (i = 0; i < MAX_RESV; i++) {
        if (resvs[i].resv == resv && !dma_fence_is_signaled(resvs[i].fence)) {
            return dma_fence_get(resvs[i].fence);
        }
    }
    f = (struct dma_fence *)kzalloc(sizeof(*f));
    dma_fence_init(f, &model_fence_ops, 0, 0);
    dma_fence_signal(f);
    return f;
}

/** Drop every fence `resv` holds. */
static void resv_drop(struct dma_resv *resv)
{
    int i;

    for (i = 0; i < MAX_RESV; i++) {
        if (resvs[i].resv == resv) {
            dma_fence_put(resvs[i].fence);
            resvs[i].resv = NULL;
        }
    }
}

/* ---- PCI: one function, its class chosen by the row ---- */

static pci_device_t pci_fn;
static int pci_present;
static uint64_t removed_base[6], removed_size[6];
static int nremoved;

/** A memory BAR is one whose low bit is clear. */
bool pci_bar_is_memory(uint32_t bar)
{
    return !(bar & 1);
}

/** A 64-bit memory BAR has type 2. */
bool pci_bar_is_64bit(uint32_t bar)
{
    return (bar & 7) == 4;
}

/** BAR `i`'s address, the model's. */
uint64_t pci_bar_address64(const pci_device_t *d, int i)
{
    return (uint64_t)(d->bar[i] & ~0xfu) |
           (pci_bar_is_64bit(d->bar[i]) ? (uint64_t)d->bar[i + 1] << 32 : 0);
}

/** BAR `i`'s size: 16 MiB for BAR 0, 16 KiB for the others. */
uint64_t pci_bar_size64(const pci_device_t *d, int i)
{
    return d->bar[i] ? (i == 0 ? 16u << 20 : 16u << 10) : 0;
}

/** Record a removal of the firmware's framebuffer. */
int32_t drm_aperture_remove_conflicting(uint64_t base, uint64_t size, const char *name)
{
    CHECK(!strcmp(name, "virtiodrmfb"), "the removal is Linux's virtiodrmfb");
    if (nremoved < 6) {
        removed_base[nremoved] = base;
        removed_size[nremoved++] = size;
    }
    return 0;
}

/* ------------------------------------------------------------------------
 * The device: QEMU's virtio-gpu
 */

/* How a queue answers: at its kick, at the next reap, or not at all. */
enum { ANSWER_KICK, ANSWER_REAP, ANSWER_NEVER };

#define MAX_FLIGHT 128
#define MAX_RES    64
#define MAX_LOG    512

struct model_queue {
    struct virtqueue vq;
    uint32_t room; /* chains it takes at once */
    struct {
        void *token;
        uint64_t resp;
        uint32_t resp_len, len;
        int answered;
    } flight[MAX_FLIGHT];
    uint32_t nflight;
};

struct model_res {
    uint32_t id, format, width, height;
    uint8_t *pixels;
    uint32_t nents;
    struct virtio_gpu_mem_entry *ents;
};

static struct {
    struct virtio_dev dev;
    uint64_t offered;
    int inited, ready, torn_down;
    struct virtio_gpu_config {
        uint32_t events_read, events_clear, num_scanouts, num_capsets;
    } config;
    struct model_queue q[2];
    int answer;
    uint32_t fail_type; /* answer this command with ERR_INVALID_RESOURCE_ID */
    struct {
        uint32_t w, h, enabled;
    } scanout[4];
    struct {
        uint32_t res;
        struct virtio_gpu_rect r;
    } shown[4];
    struct model_res res[MAX_RES];
    /* What came, in order: each command's type, and its fence id or 0. */
    uint32_t log[MAX_LOG], fence[MAX_LOG], nlog;
    struct virtio_gpu_rect last_transfer, last_flush;
    uint64_t last_offset;
    uint32_t transfers, flushes;
    struct virtio_gpu_update_cursor cursor;
    uint32_t cursor_cmd;
    uint64_t hostmem_len; /* shared memory region 1, 0 for none */
    uint32_t windows;     /* kernel windows onto it now mapped */
    pte_t window_cache;   /* the last window's cache bits */
} gpu;

/* Host-visible memory: shared memory region 1 at 14 TiB, as QEMU places
 * it, whose first HOSTMEM_BACKING bytes are host memory here, which the
 * model's MAP_BLOB and the kernel's windows reach. */
#define HOSTMEM_ADDR    ((uint64_t)14 << 40)
#define HOSTMEM_BACKING (1u << 20)
static uint8_t hostmem[HOSTMEM_BACKING];

int virtio_shm_region(struct virtio_dev *d, uint8_t id, uint64_t *addr, uint64_t *len)
{
    (void)d;
    if (id != VIRTIO_GPU_SHM_ID_HOST_VISIBLE || !gpu.hostmem_len) {
        return -ENOENT;
    }
    *addr = HOSTMEM_ADDR;
    *len = gpu.hostmem_len;
    return 0;
}

/** A window onto a piece of host-visible memory, which must lie within
 * the modeled bytes. */
uintptr_t mmio_map(uint64_t phys, size_t size, pte_t cache)
{
    CHECK(phys >= HOSTMEM_ADDR && phys - HOSTMEM_ADDR + size <= HOSTMEM_BACKING,
          "a kernel window maps a piece of host-visible memory, never the whole");
    if (phys < HOSTMEM_ADDR || phys - HOSTMEM_ADDR + size > HOSTMEM_BACKING) {
        return 0;
    }
    gpu.windows++;
    gpu.window_cache = cache;
    return (uintptr_t)&hostmem[phys - HOSTMEM_ADDR];
}

void mmio_unmap(uintptr_t va, size_t size)
{
    (void)size;
    CHECK(va >= (uintptr_t)hostmem && va < (uintptr_t)hostmem + HOSTMEM_BACKING && gpu.windows,
          "an unmapped window was mapped");
    gpu.windows--;
}

/** The request bytes of a chain, its readable segments gathered, into
 * `buf` of `cap`: their length. */
static uint32_t gather(const struct virtio_sg *sg, unsigned out, uint8_t *buf, uint32_t cap)
{
    uint32_t n = 0, i;

    for (i = 0; i < out; i++) {
        CHECK(n + sg[i].len <= cap, "a request fits the model's buffer");
        k_memcpy(buf + n, (const void *)(uintptr_t)sg[i].addr, sg[i].len);
        n += sg[i].len;
    }
    return n;
}

/** The resource `id`, or NULL. */
static struct model_res *res_find(uint32_t id)
{
    int i;

    for (i = 0; i < MAX_RES; i++) {
        if (id && gpu.res[i].id == id) {
            return &gpu.res[i];
        }
    }
    return NULL;
}

/** Copy `len` bytes at `off` of a resource's backing into `dst`. */
static void backing_read(const struct model_res *r, uint64_t off, uint8_t *dst, uint32_t len)
{
    uint32_t i, k;
    uint64_t at = 0;

    while (len) {
        for (i = 0, at = 0; i < r->nents && at + r->ents[i].length <= off; i++) {
            at += r->ents[i].length;
        }
        CHECK(i < r->nents, "a transfer lies within the backing");
        if (i >= r->nents) {
            return;
        }
        k = (uint32_t)(r->ents[i].length - (off - at));
        k = k < len ? k : len;
        k_memcpy(dst, (const uint8_t *)(uintptr_t)r->ents[i].addr + (off - at), k);
        dst += k;
        off += k;
        len -= k;
    }
}

/** GET_DISPLAY_INFO into `resp`: the answer's length. */
static uint32_t exec_display_info(uint8_t *resp, uint32_t *ok)
{
    struct virtio_gpu_resp_display_info *d = (struct virtio_gpu_resp_display_info *)resp;
    uint32_t i;

    k_memset(d, 0, sizeof(*d));
    for (i = 0; i < gpu.config.num_scanouts; i++) {
        d->pmodes[i].r.width = gpu.scanout[i].w;
        d->pmodes[i].r.height = gpu.scanout[i].h;
        d->pmodes[i].enabled = gpu.scanout[i].enabled;
    }
    *ok = VIRTIO_GPU_RESP_OK_DISPLAY_INFO;
    return sizeof(*d);
}

/** GET_EDID into `resp`, QEMU's EDID for every scanout: its length. */
static uint32_t exec_edid(uint8_t *resp, uint32_t *ok)
{
    struct virtio_gpu_resp_edid *e = (struct virtio_gpu_resp_edid *)resp;

    k_memset(e, 0, sizeof(*e));
    e->size = sizeof(qemu_edid);
    k_memcpy(e->edid, qemu_edid, sizeof(qemu_edid));
    *ok = VIRTIO_GPU_RESP_OK_EDID;
    return sizeof(*e);
}

/** RESOURCE_CREATE_2D: the answer's type. */
static uint32_t exec_create(const struct virtio_gpu_resource_create_2d *c)
{
    uint32_t i;

    if (!c->resource_id || res_find(c->resource_id)) {
        return VIRTIO_GPU_RESP_ERR_INVALID_RESOURCE_ID;
    }
    for (i = 0; i < MAX_RES && gpu.res[i].id; i++) {
    }
    CHECK(i < MAX_RES, "the model holds the resources");
    gpu.res[i].id = c->resource_id;
    gpu.res[i].format = c->format;
    gpu.res[i].width = c->width;
    gpu.res[i].height = c->height;
    gpu.res[i].pixels = (uint8_t *)calloc((unsigned long)c->width * c->height, 4);
    return VIRTIO_GPU_RESP_OK_NODATA;
}

/** RESOURCE_UNREF: the answer's type. */
static uint32_t exec_unref(const struct virtio_gpu_resource_unref *u)
{
    struct model_res *r = res_find(u->resource_id);

    if (!r) {
        return VIRTIO_GPU_RESP_ERR_INVALID_RESOURCE_ID;
    }
    free(r->pixels);
    free(r->ents);
    k_memset(r, 0, sizeof(*r));
    return VIRTIO_GPU_RESP_OK_NODATA;
}

/** RESOURCE_ATTACH_BACKING of `n` bytes with its entries: the answer's
 * type. */
static uint32_t exec_attach(const struct virtio_gpu_resource_attach_backing *a, uint32_t n)
{
    struct model_res *r = res_find(a->resource_id);

    CHECK(n == sizeof(*a) + a->nr_entries * sizeof(struct virtio_gpu_mem_entry),
          "ATTACH_BACKING carries exactly its entries");
    if (!r || r->ents) {
        return VIRTIO_GPU_RESP_ERR_INVALID_RESOURCE_ID;
    }
    r->nents = a->nr_entries;
    r->ents = (struct virtio_gpu_mem_entry *)calloc(a->nr_entries, sizeof(*r->ents));
    k_memcpy(r->ents, a + 1, a->nr_entries * sizeof(*r->ents));
    return VIRTIO_GPU_RESP_OK_NODATA;
}

/** TRANSFER_TO_HOST_2D, as QEMU copies it: each line of the rectangle from
 * the offset on by the resource's stride. The answer's type. */
static uint32_t exec_transfer(const struct virtio_gpu_transfer_to_host_2d *t)
{
    struct model_res *r = res_find(t->resource_id);
    uint32_t y;

    if (!r || !r->ents) {
        return VIRTIO_GPU_RESP_ERR_INVALID_RESOURCE_ID;
    }
    for (y = 0; y < t->r.height; y++) {
        backing_read(r, t->offset + (uint64_t)r->width * 4 * y,
                     r->pixels + ((uint64_t)(t->r.y + y) * r->width + t->r.x) * 4, t->r.width * 4);
    }
    gpu.last_transfer = t->r;
    gpu.last_offset = t->offset;
    gpu.transfers++;
    return VIRTIO_GPU_RESP_OK_NODATA;
}

/** SET_SCANOUT: the answer's type. */
static uint32_t exec_scanout(const struct virtio_gpu_set_scanout *s)
{
    if (s->scanout_id >= gpu.config.num_scanouts || (s->resource_id && !res_find(s->resource_id))) {
        return VIRTIO_GPU_RESP_ERR_INVALID_SCANOUT_ID;
    }
    gpu.shown[s->scanout_id].res = s->resource_id;
    gpu.shown[s->scanout_id].r = s->r;
    return VIRTIO_GPU_RESP_OK_NODATA;
}

#ifdef VGPU_3D_TEST
#include "vgpu3d_model.h"
#endif

/** Carry out the request `req` of `n` bytes, logging its type and fence
 * id, and write the answer into `resp`: its length. */
static uint32_t execute(const uint8_t *req, uint32_t n, uint8_t *resp)
{
    const struct virtio_gpu_ctrl_hdr *h = (const struct virtio_gpu_ctrl_hdr *)req;
    struct virtio_gpu_ctrl_hdr *rh = (struct virtio_gpu_ctrl_hdr *)resp;
    uint32_t type = h->type, ok = VIRTIO_GPU_RESP_OK_NODATA, len = sizeof(*rh);

    if (gpu.nlog < MAX_LOG) {
        gpu.log[gpu.nlog] = type;
        gpu.fence[gpu.nlog++] = (h->flags & VIRTIO_GPU_FLAG_FENCE) ? (uint32_t)h->fence_id : 0;
    }
    k_memset(resp, 0, sizeof(*rh));
    switch (type) {
    case VIRTIO_GPU_CMD_GET_DISPLAY_INFO:
        len = exec_display_info(resp, &ok);
        break;
    case VIRTIO_GPU_CMD_GET_EDID:
        len = exec_edid(resp, &ok);
        break;
    case VIRTIO_GPU_CMD_RESOURCE_CREATE_2D:
        ok = exec_create((const struct virtio_gpu_resource_create_2d *)(const void *)req);
        break;
    case VIRTIO_GPU_CMD_RESOURCE_UNREF:
        ok = exec_unref((const struct virtio_gpu_resource_unref *)(const void *)req);
        break;
    case VIRTIO_GPU_CMD_RESOURCE_ATTACH_BACKING:
        ok = exec_attach((const struct virtio_gpu_resource_attach_backing *)(const void *)req, n);
        break;
    case VIRTIO_GPU_CMD_TRANSFER_TO_HOST_2D:
        ok = exec_transfer((const struct virtio_gpu_transfer_to_host_2d *)(const void *)req);
        break;
    case VIRTIO_GPU_CMD_SET_SCANOUT:
        ok = exec_scanout((const struct virtio_gpu_set_scanout *)(const void *)req);
        break;
    case VIRTIO_GPU_CMD_RESOURCE_FLUSH:
        gpu.last_flush = ((const struct virtio_gpu_resource_flush *)(const void *)req)->r;
        gpu.flushes++;
        break;
    case VIRTIO_GPU_CMD_UPDATE_CURSOR:
    case VIRTIO_GPU_CMD_MOVE_CURSOR:
        gpu.cursor = *(const struct virtio_gpu_update_cursor *)(const void *)req;
        gpu.cursor_cmd = type;
        break;
    default:
#ifdef VGPU_3D_TEST
        len = execute_3d(req, n, resp, &ok);
#else
        ok = VIRTIO_GPU_RESP_ERR_UNSPEC;
#endif
        break;
    }
    if (type == gpu.fail_type) {
        ok = VIRTIO_GPU_RESP_ERR_INVALID_RESOURCE_ID;
        len = sizeof(*rh);
    }
    rh->type = ok;
    rh->flags = h->flags;
    rh->fence_id = h->fence_id;
    return len;
}

/** The model queue of `vq`. */
static struct model_queue *mq(struct virtqueue *vq)
{
    return (struct model_queue *)(void *)vq;
}

/** Answer the chains of `q` the device has carried out, oldest first,
 * through the driver's `done`: how many. */
static int deliver(struct model_queue *q)
{
    int n = 0;

    while (q->nflight && q->flight[0].answered) {
        void *token = q->flight[0].token;
        uint32_t len = q->flight[0].len;

        q->nflight--;
        k_memmove(&q->flight[0], &q->flight[1], q->nflight * sizeof(q->flight[0]));
        q->vq.done(&q->vq, token, len);
        n++;
    }
    return n;
}

int virtqueue_add(struct virtqueue *vq, const struct virtio_sg *sg, unsigned out, unsigned in,
                  void *token)
{
    static uint8_t req[PAGE_SIZE * (VIRTIO_GPU_CMD_EXTRA + 1)];
    struct model_queue *q = mq(vq);
    uint32_t n;
#ifdef VGPU_3D_TEST
    uint32_t i, room = 0, off = 0;
    uint8_t *response;
#endif

    CHECK(in >= 1 && out >= 1, "a command has its request and answer");
    if (gpu.torn_down) {
        return -EIO;
    }
    if (q->nflight >= q->room) {
        return -ENOSPC;
    }
    n = gather(sg, out, req, sizeof(req));
    q->flight[q->nflight].token = token;
    q->flight[q->nflight].resp = sg[out].addr;
    q->flight[q->nflight].resp_len = sg[out].len;
#ifdef VGPU_3D_TEST
    for (i = 0; i < in; i++) {
        room += sg[out + i].len;
    }
    response = calloc(1, room);
    q->flight[q->nflight].len = execute(req, n, response);
    CHECK(q->flight[q->nflight].len <= room, "the answer fits its segments");
    for (i = 0; i < in; i++) {
        k_memcpy((void *)(uintptr_t)sg[out + i].addr, response + off, sg[out + i].len);
        off += sg[out + i].len;
    }
    free(response);
#else
    q->flight[q->nflight].len = execute(req, n, (uint8_t *)(uintptr_t)sg[out].addr);
    CHECK(q->flight[q->nflight].len <= sg[out].len, "the answer fits its segment");
#endif
    q->flight[q->nflight].answered = gpu.answer != ANSWER_NEVER;
    q->nflight++;
    return 0;
}

void virtqueue_kick(struct virtqueue *vq)
{
    if (gpu.answer == ANSWER_KICK) {
        (void)deliver(mq(vq));
    }
}

int virtqueue_reap(struct virtqueue *vq)
{
    return deliver(mq(vq));
}

int virtqueue_chain_fits(const struct virtqueue *vq, const struct virtio_sg *sg, unsigned out,
                         unsigned in)
{
    (void)vq;
    (void)sg;
    return out + in <= VIRTIO_GPU_CMD_SGS + 64u;
}

int virtio_backing_fill(struct virtio_dev *d, struct shmem *sh, uint64_t bytes)
{
    (void)d;
    (void)sh;
    (void)bytes;
    return 0;
}

void virtio_backing_release(struct virtio_dev *d, struct shmem *sh)
{
    (void)d;
    (void)sh;
}

int virtio_dma_addr(struct virtio_dev *d, uintptr_t phys, uint64_t *addr)
{
    (void)d;
    *addr = (uint64_t)phys;
    return 0;
}

void virtio_put(struct virtio_dev *d)
{
    (void)d;
}

int32_t virtio_init(struct virtio_dev *d, uint64_t wanted, uint16_t nqueues)
{
    CHECK(nqueues == 2, "virtio-gpu has two queues");
    d->features = gpu.offered & (wanted | VIRTIO_BIT(VIRTIO_F_INDIRECT_DESC));
    d->nqueues = nqueues;
    gpu.inited = 1;
    gpu.torn_down = 0;
    return 0;
}

int virtio_has(const struct virtio_dev *d, unsigned bit)
{
    return (d->features & VIRTIO_BIT(bit)) != 0;
}

int virtio_queue_setup(struct virtio_dev *d, uint16_t index, virtqueue_done_t done, void *priv,
                       struct virtqueue **out)
{
    struct model_queue *q = &gpu.q[index];

    q->vq.dev = d;
    q->vq.index = index;
    q->vq.size = index == 0 ? 64 : 16;
    q->vq.done = done;
    q->vq.priv = priv;
    q->nflight = 0;
    *out = &q->vq;
    return 0;
}

int virtio_ready(struct virtio_dev *d)
{
    (void)d;
    gpu.ready = 1;
    return 0;
}

void virtio_teardown(struct virtio_dev *d)
{
    (void)d;
    gpu.torn_down = 1;
    gpu.q[0].nflight = gpu.q[1].nflight = 0;
}

void virtio_config_read(struct virtio_dev *d, uint32_t off, void *buf, uint32_t len)
{
    (void)d;
    k_memcpy(buf, (const uint8_t *)&gpu.config + off, len);
}

void virtio_config_write(struct virtio_dev *d, uint32_t off, const void *buf, uint32_t len)
{
    uint32_t v = 0;

    (void)d;
    k_memcpy(&v, buf, len);
    CHECK(off == VIRTIO_GPU_CONFIG_EVENTS_CLEAR, "the driver writes only events_clear");
    gpu.config.events_clear = v;
    gpu.config.events_read &= ~v;
}

void *virtio_dma_alloc(struct virtio_dev *d, uint32_t bytes, uint64_t *addr)
{
    void *p;

    (void)d;
    CHECK(bytes <= PAGE_SIZE, "a DMA allocation is at most a page");
    p = kzalloc(bytes);
    *addr = p ? (uint64_t)(uintptr_t)p : 0;
    return p;
}

void virtio_dma_free(struct virtio_dev *d, void *p, uint32_t bytes)
{
    (void)d;
    (void)bytes;
    kfree(p);
}

int virtio_pci_find(uint16_t id, int nth, struct virtio_dev **out)
{
    CHECK(id == VIRTIO_ID_GPU, "the probe looks for a GPU");
    if (nth > 0) {
        return -ENODEV;
    }
    gpu.dev.id = id;
    gpu.dev.name = "virtio0";
    *out = &gpu.dev;
    return 0;
}

const pci_device_t *virtio_pci_function(const struct virtio_dev *d)
{
    (void)d;
    return pci_present ? &pci_fn : NULL;
}

/* ------------------------------------------------------------------------
 * The rows
 */

static struct virtio_gpu_device *vg;
static struct drm_device *dev;
static struct drm_file file;

/** Reset the model: one scanout of 1280x800, EDID offered, answers at the
 * kick. */
static void model_reset(void)
{
    int i;

    for (i = 0; i < MAX_RES; i++) {
        free(gpu.res[i].pixels);
        free(gpu.res[i].ents);
    }
    k_memset(&gpu, 0, sizeof(gpu));
    gpu.offered = VIRTIO_BIT(VIRTIO_GPU_F_EDID) | VIRTIO_BIT(VIRTIO_F_INDIRECT_DESC);
    gpu.config.num_scanouts = 1;
    gpu.scanout[0].w = 1280;
    gpu.scanout[0].h = 800;
    gpu.scanout[0].enabled = 1;
    gpu.q[0].room = 64;
    gpu.q[1].room = 16;
    gpu.answer = ANSWER_KICK;
    pci_present = 0;
    nremoved = 0;
    hotplug_events = 0;
}

/** Bind the model's device and open a file as its master: the probe's
 * result. */
static int32_t bind(void)
{
    int32_t rc = virtio_gpu_probe();

    vg = vgpu_devices[0];
    dev = vg ? &vg->drm : NULL;
    if (dev) {
        k_memset(&file, 0, sizeof(file));
        file.dev = dev;
        file.authenticated = 1;
        file.event_space = 4096;
        dev->master = &file;
        file.is_master = 1;
    }
    return rc;
}

/** Drop the file's handles and the bind's reference, which frees the
 * device, as an unplug followed by the last close does. */
static void unbind(void)
{
    int i;

    if (dev && drm_core_check_feature(dev, DRIVER_MODESET)) {
        drm_mode_file_release(&file);
    }
    for (i = 0; i < MAX_HANDLES; i++) {
        if (handles[i].file == &file) {
            (void)drm_gem_handle_delete(&file, handles[i].id);
        }
    }
    if (dev) {
        dev->unplugged = 1;
        vgpu_unplug(dev);
        drm_dev_put(dev);
    }
    vg = NULL;
    dev = NULL;
}

/** The modes of the device's connector, `n` of them into `out`, and the
 * property count into `*nprops`. */
static uint32_t get_modes(struct drm_mode_modeinfo *out, uint32_t n, uint32_t *nprops)
{
    struct drm_mode_get_connector c;
    uint32_t props[16];
    uint64_t values[16];

    k_memset(&c, 0, sizeof(c));
    c.connector_id = vg->outputs[0].connector.base.id;
    CHECK(drm_mode_getconnector(dev, &c, &file) == 0, "GETCONNECTOR probes");
    c.modes_ptr = UPTR(out);
    c.count_modes = c.count_modes < n ? c.count_modes : n;
    c.props_ptr = UPTR(props);
    c.prop_values_ptr = UPTR(values);
    c.count_props = c.count_props < 16 ? c.count_props : 16;
    c.count_encoders = 0;
    CHECK(drm_mode_getconnector(dev, &c, &file) == 0, "GETCONNECTOR gives the modes");
    if (nprops) {
        *nprops = c.count_props;
    }
    return c.count_modes;
}

/** Whether the device saw `type` as command `i` of its log. */
static int logged(uint32_t i, uint32_t type)
{
    return i < gpu.nlog && gpu.log[i] == type;
}

/** The fence ids of the log are 1, 2, 3 ... for the control queue, 0 for
 * the cursor queue and for UNMAP_BLOB, which goes unfenced. */
static int fences_in_order(void)
{
    uint32_t i, next = 1;

    for (i = 0; i < gpu.nlog; i++) {
        if (gpu.log[i] == VIRTIO_GPU_CMD_UPDATE_CURSOR ||
            gpu.log[i] == VIRTIO_GPU_CMD_MOVE_CURSOR ||
            gpu.log[i] == VIRTIO_GPU_CMD_RESOURCE_UNMAP_BLOB) {
            if (gpu.fence[i]) {
                return 0;
            }
        } else if (gpu.fence[i] != next++) {
            return 0;
        }
    }
    return 1;
}

/** Bind with EDID: the commands, the identity and the display. */
static void row_bind(void)
{
    struct drm_mode_modeinfo modes[64];
    uint32_t n, props = 0, i;
    int found = 0;

    model_reset();
    CHECK(bind() == 1, "the probe binds the model's device");
    CHECK(vg && vg->has_edid && vg->num_scanouts == 1 && gpu.ready, "EDID taken, one scanout");
    CHECK(logged(0, VIRTIO_GPU_CMD_GET_EDID) && logged(1, VIRTIO_GPU_CMD_GET_DISPLAY_INFO) &&
              gpu.nlog == 2,
          "the bind reads the EDID and then the displays");
    CHECK(fences_in_order(), "each command carries the next fence id");
    CHECK(registered == 1 && dev->mode_config.num_crtc == 1 && dev->mode_config.num_plane == 2 &&
              dev->mode_config.num_connector == 1 && dev->mode_config.num_encoder == 1,
          "one CRTC, two planes, a connector and an encoder");
    CHECK(vg->outputs[0].primary.type == DRM_PLANE_TYPE_PRIMARY &&
              vg->outputs[0].cursor.type == DRM_PLANE_TYPE_CURSOR,
          "a primary and a cursor plane");
    for (i = 0; i < vg->outputs[0].primary.base.count_props; i++) {
        found |= vg->outputs[0].primary.base.props[i] == dev->mode_config.modifiers_property;
    }
    CHECK(dev->mode_config.fb_modifiers_not_supported && !found, "no modifiers and no IN_FORMATS");
    CHECK(vg->outputs[0].connector.connector_type == DRM_MODE_CONNECTOR_VIRTUAL,
          "a virtual connector");
    n = get_modes(modes, 64, &props);
    CHECK(n == 26, "Linux's 26 modes of QEMU's EDID");
    CHECK(n && modes[0].hdisplay == 1280 && modes[0].vdisplay == 800 &&
              (modes[0].type & DRM_MODE_TYPE_PREFERRED),
          "1280x800 first and preferred");
    CHECK(props > 0 && vg->outputs[0].connector.edid, "the EDID property is set");
    CHECK(dev->features & DRIVER_RENDER, "a render node");
}

/** Bind without EDID: the DMT modes and the display's size preferred. */
static void row_no_edid(void)
{
    struct drm_mode_modeinfo modes[128];
    uint32_t n, i, preferred = 0;

    unbind();
    model_reset();
    gpu.offered = VIRTIO_BIT(VIRTIO_F_INDIRECT_DESC);
    gpu.scanout[0].w = 1000;
    gpu.scanout[0].h = 700;
    CHECK(bind() == 1 && !vg->has_edid, "a bind without EDID");
    CHECK(logged(0, VIRTIO_GPU_CMD_GET_DISPLAY_INFO) && gpu.nlog == 1,
          "the bind reads the displays alone");
    n = get_modes(modes, 128, NULL);
    for (i = 0; i < n; i++) {
        if (modes[i].type & DRM_MODE_TYPE_PREFERRED) {
            preferred++;
            CHECK(modes[i].hdisplay == 1000 && modes[i].vdisplay == 700,
                  "the preferred mode is the display's size");
        }
    }
    CHECK(n > 20 && preferred == 1, "the DMT modes and one preferred");
    CHECK(!vg->outputs[0].connector.edid, "no EDID blob");
}

/** A dumb buffer of `w` by `h`, its handle into `*handle`. */
static struct virtio_gpu_object *dumb(uint32_t w, uint32_t h, uint32_t *handle)
{
    struct drm_mode_create_dumb c;

    k_memset(&c, 0, sizeof(c));
    c.width = w;
    c.height = h;
    c.bpp = 32;
    if (vgpu_dumb_create(&file, dev, &c) != 0) {
        return NULL;
    }
    CHECK(c.pitch == w * 4 &&
              c.size == ((uint64_t)w * 4 * h + PAGE_SIZE - 1) / PAGE_SIZE * PAGE_SIZE,
          "the dumb buffer's pitch and size");
    *handle = c.handle;
    return vgpu_bo(drm_gem_object_lookup(&file, c.handle));
}

/** A framebuffer of the dumb buffer `handle`. */
static uint32_t addfb(uint32_t handle, uint32_t w, uint32_t h)
{
    struct drm_mode_fb_cmd f;

    k_memset(&f, 0, sizeof(f));
    f.width = w;
    f.height = h;
    f.pitch = w * 4;
    f.bpp = 32;
    f.depth = 24;
    f.handle = handle;
    CHECK(drm_mode_addfb_ioctl(dev, &f, &file) == 0, "ADDFB of a dumb buffer");
    return f.fb_id;
}

/** Write `v` into pixel `x`, `y` of `bo`, whose pitch is `w` pixels. */
static void poke(struct virtio_gpu_object *bo, uint32_t w, uint32_t x, uint32_t y, uint32_t v)
{
    uint64_t off = ((uint64_t)y * w + x) * 4;
    uint8_t *p = (uint8_t *)drm_gem_shmem_page(&bo->shmem, (uint32_t)(off >> PAGE_SHIFT));

    k_memcpy(p + (off & (PAGE_SIZE - 1)), &v, 4);
}

/** Pixel `x`, `y` of the host's copy of resource `id`. */
static uint32_t host_pixel(uint32_t id, uint32_t x, uint32_t y)
{
    struct model_res *r = res_find(id);
    uint32_t v = 0;

    if (r) {
        k_memcpy(&v, r->pixels + ((uint64_t)y * r->width + x) * 4, 4);
    }
    return v;
}

/** Whether a rectangle is `x`, `y`, `w`, `h`. */
static int rect_is(const struct virtio_gpu_rect *r, uint32_t x, uint32_t y, uint32_t w, uint32_t h)
{
    return r->x == x && r->y == y && r->width == w && r->height == h;
}

static struct virtio_gpu_object *shown_bo;
static uint32_t shown_handle, shown_fb;

/** A dumb buffer, SETCRTC and DIRTYFB. */
static void row_scanout(void)
{
    struct drm_mode_modeinfo modes[64];
    struct drm_mode_crtc set;
    struct drm_mode_fb_dirty_cmd d;
    struct drm_clip_rect clip;
    struct drm_mode_fb_cmd2 f2;
    struct model_res *r;
    uint32_t id, before;

    unbind();
    model_reset();
    CHECK(bind() == 1, "a bind for the scanout");
    (void)get_modes(modes, 64, NULL);
    shown_bo = dumb(1280, 800, &shown_handle);
    CHECK(shown_bo != NULL, "a dumb buffer of 1280x800");
    if (!shown_bo) {
        return;
    }
    id = shown_bo->res_id;
    r = res_find(id);
    CHECK(id == 1 && r && r->format == VIRTIO_GPU_FORMAT_B8G8R8X8_UNORM && r->width == 1280 &&
              r->height == 800,
          "resource 1, XRGB8888 of 1280x800");
    CHECK(r && r->nents == 1 && r->ents[0].length == 1280u * 800 * 4,
          "adjacent pages are one entry");
    poke(shown_bo, 1280, 0, 0, 0x11223344u);
    poke(shown_bo, 1280, 1279, 799, 0x55667788u);
    shown_fb = addfb(shown_handle, 1280, 800);
    k_memset(&set, 0, sizeof(set));
    set.crtc_id = vg->outputs[0].crtc.base.id;
    set.fb_id = shown_fb;
    set.set_connectors_ptr = UPTR(&vg->outputs[0].connector.base.id);
    set.count_connectors = 1;
    set.mode = modes[0];
    set.mode_valid = 1;
    before = gpu.nlog;
    CHECK(drm_mode_setcrtc(dev, &set, &file) == 0, "SETCRTC of 1280x800");
    CHECK(gpu.shown[0].res == id && rect_is(&gpu.shown[0].r, 0, 0, 1280, 800),
          "the scanout shows the buffer");
    CHECK(rect_is(&gpu.last_transfer, 0, 0, 1280, 800) && gpu.last_offset == 0 &&
              rect_is(&gpu.last_flush, 0, 0, 1280, 800),
          "the whole buffer is copied and flushed");
    CHECK(host_pixel(id, 0, 0) == 0x11223344u && host_pixel(id, 1279, 799) == 0x55667788u,
          "the host has the buffer's pixels");
    CHECK(logged(before, VIRTIO_GPU_CMD_TRANSFER_TO_HOST_2D) &&
              logged(before + 1, VIRTIO_GPU_CMD_SET_SCANOUT) &&
              logged(before + 2, VIRTIO_GPU_CMD_RESOURCE_FLUSH),
          "transfer, scanout and flush in Linux's order");

    poke(shown_bo, 1280, 100, 50, 0xAABBCCDDu);
    poke(shown_bo, 1280, 0, 0, 0x99999999u);
    clip.x1 = 100;
    clip.y1 = 50;
    clip.x2 = 140;
    clip.y2 = 60;
    k_memset(&d, 0, sizeof(d));
    d.fb_id = shown_fb;
    d.num_clips = 1;
    d.clips_ptr = UPTR(&clip);
    before = gpu.nlog;
    CHECK(drm_mode_dirtyfb_ioctl(dev, &d, &file) == 0, "DIRTYFB of a clip");
    CHECK(rect_is(&gpu.last_transfer, 100, 50, 40, 10) &&
              gpu.last_offset == 100 * 4 + 50 * 1280 * 4 &&
              rect_is(&gpu.last_flush, 100, 50, 40, 10),
          "the clip alone is copied, from its offset, and flushed");
    CHECK(host_pixel(id, 100, 50) == 0xAABBCCDDu && host_pixel(id, 0, 0) == 0x11223344u,
          "only the clip's pixels reach the host");
    CHECK(gpu.nlog == before + 2, "no SET_SCANOUT for damage alone");
    CHECK(fences_in_order(), "the fences stay in order");

    k_memset(&f2, 0, sizeof(f2));
    f2.fb_id = shown_fb;
    CHECK(drm_mode_getfb2_ioctl(dev, &f2, &file) == 0 && f2.flags == 0 && f2.modifier[0] == 0,
          "GETFB2 gives no modifier");
    k_memset(&f2, 0, sizeof(f2));
    f2.width = 64;
    f2.height = 64;
    f2.pixel_format = DRM_FORMAT_XRGB8888;
    f2.flags = DRM_MODE_FB_MODIFIERS;
    f2.handles[0] = shown_handle;
    f2.pitches[0] = 1280 * 4;
    CHECK(drm_mode_addfb2_ioctl(dev, &f2, &file) == -EINVAL, "ADDFB2 with a modifier is EINVAL");
}

/** Scattered pages: one entry each, past the command's page. */
static void row_scatter(void)
{
    struct virtio_gpu_object *bo;
    struct model_res *r;
    uint32_t h, i;
    int adjacent = 0;

    scatter = 1;
    bo = dumb(1024, 200, &h); /* 200 pages, 74 past the inline 126 */
    scatter = 0;
    CHECK(bo != NULL, "a scattered buffer");
    if (!bo) {
        return;
    }
    r = res_find(bo->res_id);
    CHECK(r && r->nents == 200, "a scattered buffer has an entry for each page");
    for (i = 0; r && i + 1 < r->nents; i++) {
        adjacent |= r->ents[i].addr + PAGE_SIZE == r->ents[i + 1].addr;
    }
    CHECK(r && !adjacent && r->ents[199].addr == drm_gem_shmem_page(&bo->shmem, 199),
          "the entries are the pages in order");
    drm_gem_object_put(&bo->shmem.base);
}

/** A queue with room for two: the rest wait and go in order. */
static void row_pending(void)
{
    struct drm_virtgpu_3d_transfer t;
    uint32_t before = gpu.nlog, i;

    gpu.q[0].room = 2;
    gpu.answer = ANSWER_REAP;
    k_memset(&t, 0, sizeof(t));
    t.bo_handle = shown_handle;
    t.box.w = 8;
    t.box.h = 8;
    for (i = 0; i < 5; i++) {
        t.box.x = i * 8;
        CHECK(vgpu_transfer_to_host_ioctl(dev, &t, &file) == 0, "TRANSFER_TO_HOST");
    }
    CHECK(gpu.nlog == before + 2 && vg->pending[0] != NULL, "two go in and three wait");
    while (virtqueue_reap(vg->ctrlq) > 0) {
    }
    CHECK(gpu.nlog == before + 5 && vg->pending[0] == NULL, "answers let the rest in");
    CHECK(rect_is(&gpu.last_transfer, 32, 0, 8, 8), "in the order sent");
    CHECK(fences_in_order(), "the fence ids follow the queue's order");
    vgpu_reap(vg);
    gpu.q[0].room = 64;
    gpu.answer = ANSWER_KICK;
}

/** An error answer: the fence fails with EIO, the reaper logs it, and a
 * waiter gets EIO. */
static void row_error(void)
{
    struct virtio_gpu_cmd *cmd;
    struct dma_fence *f;

    gpu.fail_type = VIRTIO_GPU_CMD_TRANSFER_TO_HOST_2D;
    gpu.answer = ANSWER_REAP;
    cmd = vgpu_transfer_cmd(vg, shown_bo->res_id, 0, rect_of(0, 0, 4, 4));
    vgpu_cmd_send(cmd, NULL, NULL);
    f = dma_fence_get(cmd->fence);
    warnings = 0;
    vgpu_reap(vg);
    CHECK(dma_fence_is_signaled(f) && f->error == -EIO, "the fence fails with EIO");
    CHECK(warnings == 1, "the reaper logs the error");
    dma_fence_put(f);
    gpu.answer = ANSWER_KICK;
    cmd = vgpu_transfer_cmd(vg, shown_bo->res_id, 0, rect_of(0, 0, 4, 4));
    CHECK(vgpu_cmd_send_wait(cmd, NULL, 0) == -EIO, "a waiter gets EIO");
    gpu.fail_type = 0;
}

/** No answer: the waiter gives up at five seconds; the late answer is the
 * reaper's. */
static void row_timeout(void)
{
    struct virtio_gpu_cmd *cmd;
    int live = allocations;
    int64_t start = now_ns;

    gpu.answer = ANSWER_NEVER;
    cmd = vgpu_transfer_cmd(vg, shown_bo->res_id, 0, rect_of(0, 0, 4, 4));
    CHECK(vgpu_cmd_send_wait(cmd, NULL, 0) == -ETIMEDOUT, "no answer in five seconds");
    CHECK(now_ns - start >= 5000000000ll && now_ns - start < 5100000000ll,
          "the waiter gave up at five seconds");
    gpu.q[0].flight[0].answered = 1;
    gpu.answer = ANSWER_KICK;
    vgpu_reap(vg);
    CHECK(allocations == live, "the late answer frees the command");
}

/** The cursor: UPDATE_CURSOR with its hotspot, then MOVE_CURSOR. */
static void row_cursor(void)
{
    struct virtio_gpu_object *bo;
    struct drm_mode_cursor2 c;
    uint32_t h;

    bo = dumb(64, 64, &h);
    CHECK(bo != NULL, "a cursor buffer");
    if (!bo) {
        return;
    }
    k_memset(&c, 0, sizeof(c));
    c.flags = DRM_MODE_CURSOR_BO | DRM_MODE_CURSOR_MOVE;
    c.crtc_id = vg->outputs[0].crtc.base.id;
    c.width = 64;
    c.height = 64;
    c.handle = h;
    c.x = 10;
    c.y = 20;
    c.hot_x = 3;
    c.hot_y = 4;
    CHECK(drm_mode_cursor2_ioctl(dev, &c, &file) == 0, "CURSOR2 sets the cursor");
    CHECK(gpu.cursor_cmd == VIRTIO_GPU_CMD_UPDATE_CURSOR && gpu.cursor.resource_id == bo->res_id &&
              gpu.cursor.hot_x == 3 && gpu.cursor.hot_y == 4 && gpu.cursor.pos.x == 10 &&
              gpu.cursor.pos.y == 20,
          "UPDATE_CURSOR names the resource, its hotspot and place");
    c.flags = DRM_MODE_CURSOR_MOVE;
    c.x = 30;
    CHECK(drm_mode_cursor2_ioctl(dev, &c, &file) == 0, "CURSOR2 moves the cursor");
    CHECK(gpu.cursor_cmd == VIRTIO_GPU_CMD_MOVE_CURSOR && gpu.cursor.pos.x == 30,
          "MOVE_CURSOR for a move alone");
    CHECK(fences_in_order(), "the cursor queue has no fences");
    drm_gem_object_put(&bo->shmem.base);
}

/** A display change: served at the next open, one hotplug. */
static void row_hotplug(void)
{
    struct drm_mode_modeinfo modes[128];
    uint32_t before, n;

    unbind();
    model_reset();
    gpu.offered = VIRTIO_BIT(VIRTIO_F_INDIRECT_DESC);
    CHECK(bind() == 1, "a bind for the change");
    gpu.scanout[0].w = 1600;
    gpu.scanout[0].h = 900;
    gpu.config.events_read = VIRTIO_GPU_EVENT_DISPLAY;
    gpu.dev.config_changed(&gpu.dev);
    CHECK(vg->display_pending && hotplug_events == 0, "the interrupt only marks the change");
    before = gpu.nlog;
    CHECK(vgpu_open(dev, &file) == 0, "an open serves the change");
    CHECK(logged(before, VIRTIO_GPU_CMD_GET_DISPLAY_INFO) && hotplug_events == 1 &&
              !vg->display_pending,
          "the displays are read again and one hotplug sent");
    CHECK(gpu.config.events_clear == VIRTIO_GPU_EVENT_DISPLAY && gpu.config.events_read == 0,
          "the event is cleared");
    n = get_modes(modes, 128, NULL);
    CHECK(n && (modes[0].type & DRM_MODE_TYPE_PREFERRED) && modes[0].hdisplay == 1600 &&
              modes[0].vdisplay == 900,
          "the new size is preferred");
    gpu.config.events_read = 0;
    gpu.dev.config_changed(&gpu.dev);
    CHECK(vgpu_open(dev, &file) == 0 && hotplug_events == 1,
          "no hotplug without the display event");
}

/** The driver's ioctls without 3D. */
static void row_ioctls(void)
{
    struct drm_virtgpu_getparam gp;
    struct drm_virtgpu_resource_create rc;
    struct drm_virtgpu_resource_info ri;
    struct drm_virtgpu_map mp;
    struct drm_virtgpu_3d_transfer t;
    struct drm_virtgpu_3d_wait w;
    int value = -1;

    k_memset(&gp, 0, sizeof(gp));
    gp.param = VIRTGPU_PARAM_CAPSET_QUERY_FIX;
    gp.value = UPTR(&value);
    CHECK(vgpu_getparam_ioctl(dev, &gp, &file) == 0 && value == 1, "CAPSET_QUERY_FIX is 1");
    gp.param = VIRTGPU_PARAM_3D_FEATURES;
    CHECK(vgpu_getparam_ioctl(dev, &gp, &file) == 0 && value == 0, "3D_FEATURES is 0");
    gp.param = 99;
    CHECK(vgpu_getparam_ioctl(dev, &gp, &file) == -EINVAL, "an unknown parameter is EINVAL");
    gp.param = VIRTGPU_PARAM_3D_FEATURES;
    gp.value = 8;
    CHECK(vgpu_getparam_ioctl(dev, &gp, &file) == -EFAULT, "a bad pointer is EFAULT");

    k_memset(&rc, 0, sizeof(rc));
    rc.target = 2;
    rc.format = VIRTIO_GPU_FORMAT_B8G8R8A8_UNORM;
    rc.width = 16;
    rc.height = 16;
    rc.size = 16 * 16 * 4;
    rc.depth = 1;
    rc.array_size = 1;
    CHECK(vgpu_resource_create_ioctl(dev, &rc, &file) == 0 && rc.res_handle && rc.bo_handle,
          "RESOURCE_CREATE of a 2D texture");
    CHECK(res_find(rc.res_handle) && res_find(rc.res_handle)->format == rc.format,
          "the host has it in its format");
    rc.target = 3;
    CHECK(vgpu_resource_create_ioctl(dev, &rc, &file) == -EINVAL, "a 3D target is EINVAL");
    k_memset(&ri, 0, sizeof(ri));
    ri.bo_handle = rc.bo_handle;
    CHECK(vgpu_resource_info_ioctl(dev, &ri, &file) == 0 && ri.res_handle == rc.res_handle &&
              ri.size == PAGE_SIZE,
          "RESOURCE_INFO");
    k_memset(&mp, 0, sizeof(mp));
    mp.handle = rc.bo_handle;
    CHECK(vgpu_map_ioctl(dev, &mp, &file) == 0 && mp.offset, "MAP gives an offset");
    mp.handle = 77;
    CHECK(vgpu_map_ioctl(dev, &mp, &file) == -ENOENT, "MAP of no handle is ENOENT");

    gpu.answer = ANSWER_NEVER;
    k_memset(&t, 0, sizeof(t));
    t.bo_handle = rc.bo_handle;
    t.box.w = 16;
    t.box.h = 16;
    CHECK(vgpu_transfer_to_host_ioctl(dev, &t, &file) == 0, "TRANSFER_TO_HOST");
    k_memset(&w, 0, sizeof(w));
    w.handle = rc.bo_handle;
    w.flags = VIRTGPU_WAIT_NOWAIT;
    CHECK(vgpu_wait_ioctl(dev, &w, &file) == -EBUSY, "WAIT NOWAIT while the transfer is out");
    gpu.q[0].flight[gpu.q[0].nflight - 1].answered = 1;
    CHECK(vgpu_wait_ioctl(dev, &w, &file) == 0, "WAIT once it is answered, which its reap takes");
    w.flags = 0;
    CHECK(vgpu_wait_ioctl(dev, &w, &file) == 0, "a waiting WAIT on an idle buffer");
    gpu.answer = ANSWER_KICK;
    vgpu_reap(vg);
    CHECK(vgpu_execbuffer_ioctl(dev, NULL, &file) == -ENOSYS &&
              vgpu_get_caps_ioctl(dev, NULL, &file) == -ENOSYS &&
              vgpu_transfer_from_host_ioctl(dev, NULL, &file) == -ENOSYS,
          "the 3D calls are ENOSYS");
    CHECK(vgpu_blob_ioctl(dev, NULL, &file) == -EINVAL, "blobs and contexts are EINVAL");
}

/** A buffer's last reference: UNREF, and its pages once answered. */
static void row_free(void)
{
    struct virtio_gpu_object *bo;
    uint32_t h, id, held = (uint32_t)pages_held;

    gpu.answer = ANSWER_REAP;
    bo = dumb(64, 64, &h);
    CHECK(bo != NULL, "a buffer to free");
    if (!bo) {
        return;
    }
    id = bo->res_id;
    drm_gem_object_put(&bo->shmem.base);
    CHECK(drm_gem_handle_delete(&file, h) == 0, "the handle is closed");
    CHECK(gpu.log[gpu.nlog - 1] == VIRTIO_GPU_CMD_RESOURCE_UNREF && res_find(id) == NULL,
          "the last reference unrefs the resource");
    CHECK((uint32_t)pages_held > held, "the pages wait for the answer");
    (void)virtqueue_reap(vg->ctrlq);
    vgpu_reap(vg);
    CHECK((uint32_t)pages_held == held, "the answer frees the pages");
    bo = dumb(64, 64, &h);
    CHECK(bo && bo->res_id == id, "the lowest free id is used again");
    if (bo) {
        drm_gem_object_put(&bo->shmem.base);
    }
    gpu.answer = ANSWER_KICK;
}

/** virtio-vga: the firmware's framebuffer is removed from each memory BAR. */
static void row_vga(void)
{
    unbind();
    model_reset();
    pci_present = 1;
    k_memset(&pci_fn, 0, sizeof(pci_fn));
    pci_fn.class_code = 0x03;
    pci_fn.subclass = 0x00;
    pci_fn.bar[0] = 0xF0000008u; /* prefetchable memory */
    pci_fn.bar[1] = 0xC001u;     /* I/O */
    pci_fn.bar[2] = 0xF1000000u;
    pci_fn.bar[4] = 0xFE000004u; /* 64-bit */
    pci_fn.bar[5] = 0x1u;
    CHECK(bind() == 1 && vg->vga, "a virtio-vga binds");
    CHECK(nremoved == 3 && removed_base[0] == 0xF0000000u && removed_size[0] == 16u << 20 &&
              removed_base[1] == 0xF1000000u && removed_base[2] == 0x1FE000000ull,
          "each memory BAR's range is removed");
    unbind();
    model_reset();
    pci_present = 1;
    pci_fn.class_code = 0x03;
    pci_fn.subclass = 0x80;
    CHECK(bind() == 1 && !vg->vga && nremoved == 0, "a virtio-gpu-pci removes nothing");
}

/** No scanout: no KMS, a render node still. */
static void row_no_scanout(void)
{
    unbind();
    model_reset();
    gpu.config.num_scanouts = 0;
    CHECK(bind() == 1, "a device with no scanout binds");
    CHECK(!(dev->features & (DRIVER_MODESET | DRIVER_ATOMIC)) && (dev->features & DRIVER_RENDER) &&
              gpu.nlog == 0,
          "no KMS, a render node, and no display read");
}

/** Unplug with a command out: its fence fails with ENODEV, and the last
 * reference leaves nothing. */
static void row_unplug(void)
{
    struct virtio_gpu_cmd *cmd;
    struct dma_fence *f;

    unbind();
    model_reset();
    CHECK(bind() == 1, "a bind to unplug");
    gpu.answer = ANSWER_NEVER;
    cmd = vgpu_transfer_cmd(vg, 0, 0, rect_of(0, 0, 1, 1));
    vgpu_cmd_send(cmd, NULL, NULL);
    f = dma_fence_get(cmd->fence);
    unbind();
    CHECK(dma_fence_is_signaled(f) && f->error == -ENODEV, "an unanswered fence fails with ENODEV");
    dma_fence_put(f);
    CHECK(released > 0, "the last reference releases the device");
}

int main(void)
{
    cpu_table[0].current = &self;
    row_bind();
    row_no_edid();
    row_scanout();
    row_scatter();
    row_pending();
    row_error();
    row_timeout();
    row_cursor();
    row_ioctls();
    row_free();
    if (shown_bo) {
        drm_gem_object_put(&shown_bo->shmem.base);
        shown_bo = NULL;
    }
    row_hotplug();
    row_vga();
    row_no_scanout();
    row_unplug();
    CHECK(allocations == 0, "nothing is left allocated");
    CHECK(pages_held == 0, "no page is left held");
    CHECK(irq_depth == 0, "every lock is given back");
    if (failures) {
        printf("virtio-gpu-test: %d failures\n", failures);
        return 1;
    }
    printf("virtio-gpu-test: ok\n");
    return 0;
}
