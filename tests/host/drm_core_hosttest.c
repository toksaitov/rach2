/**
 * drm_core_hosttest.c: the DRM core, rach2/sys/drm/drm.c, drm_auth.c and
 * drm_gem.c, and kcmp(2), rach2/sys/proc/kcmp.c, with i686's `cpu.h`;
 * drm_core64_hosttest.c runs the same rows with x86-64's.
 *
 * host-deps: rach2/sys/lib/kstring.c rach2/sys/lib/kctypes.c
 * host-deps: tests/host/compat/kheap_host.c tests/host/compat/spinlock_host.c
 * host-deps: tests/host/compat/atomic_host.c
 * host-deps: tests/host/compat/fd_flat.c
 * host-deps: tests/host/compat/mutex_none.c
 *
 * The four files are included whole. A fake driver binds two devices: one
 * with a card and a render node, GEM, dumb buffers of the shmem helper
 * mapped write-combining, and an ioctl of its own; and one with a card
 * alone and no display. The devfs registry is a table of the nodes
 * registered, which a row opens as devfs does, through the node's `open`
 * on a descriptor of its own with an open file description. A user pointer
 * is a pointer of the test's, and one below 4096 faults. The frames of the
 * shmem helper are pages of the host's, so a row writes through the address
 * a mapping's fault would map, and a device region is a record of what
 * `vm_device_map` was given, whose `page` and `put` a row calls as a fault
 * and an unmap would. Every read is nonblocking, so none sleeps.
 *
 * The rows: registration of the nodes with their numbers, modes and bus
 * names; VERSION, GET_UNIQUE, GET_CAP, SET_CLIENT_CAP, SET_VERSION,
 * GET_CLIENT and GET_STATS; the dispatcher's refusals by type, number,
 * flags and node and its copies by direction; the master role between two
 * openers, a root caller and a close, with magic and authentication, and
 * the listener's view of each change; dumb buffers created, mapped, written
 * and freed, with their handles, flink names and mmap offsets; a handle
 * table's growth, its lowest free id and its walk; a handle reserved while
 * its object's `open` hook runs; references held by the walk over a
 * device's objects, and an object whose last put is under way passed over
 * by it and by mmap; events queued, polled, read and left by a closed
 * file; the locks each row runs under, including a big lock inherited from
 * a file-system device node, and the mode lock, by
 * its flags and the device's; unplug; and kcmp of a dup, a fork's copy and
 * two opens of a node, and its other kinds. Each row that ends with the
 * core's locks free shows that no call left one held or took one twice,
 * which the host's spinlock traps.
 *
 * Returns 0 when every row passes, 1 otherwise.
 */

#include "drm/drm.c"
#include "drm/drm_auth.c"
#include "drm/drm_gem.c"
#include "proc/kcmp.c"

extern int printf(const char *format, ...);
extern void *calloc(unsigned long count, unsigned long size);
extern void *aligned_alloc(unsigned long align, unsigned long size);
extern void free(void *p);
extern int memcmp(const void *a, const void *b, unsigned long n);
extern void *memcpy(void *dst, const void *src, unsigned long n);
extern int strcmp(const char *a, const char *b);
extern unsigned long kheap_host_largest;

static int fails;

#define CHECK(what, cond)                              \
    do {                                               \
        if (!(cond)) {                                 \
            printf("drm-core-test: fail: %s\n", what); \
            fails++;                                   \
        }                                              \
    } while (0)

struct percpu cpu_table[NR_CPUS];

/* ---- the kernel around the core ---- */

/** The boot log: nothing to keep. */
void klog(int level, const char *subsystem, const char *format, ...)
{
    (void)level;
    (void)subsystem;
    (void)format;
}

/** No interrupts to mask on the host, for the spinlock's model. */
uint32_t kirq_save(void)
{
    return 0;
}

void kirq_restore(uint32_t flags)
{
    (void)flags;
}

/** No probes run here. */
int32_t udmabuf_start(void)
{
    return 0;
}

int32_t virtio_gpu_probe(void)
{
    return -ENODEV;
}

/** The commit worker, which no row here needs. */
int32_t drm_atomic_worker_start(void)
{
    return 0;
}

/** The commit lock, which the dispatcher takes for a KMS row and the
 * unplug for its mark: counted, so that a row sees it held and let go. */
static int mode_lock_depth;

/** The big lock, counted the same way (proc/smp.h). */
static int bkl_depth, bkl_enters;

void bkl_enter(void)
{
    bkl_enters++;
    bkl_depth++;
}

void bkl_exit(void)
{
    bkl_depth--;
}

int bkl_held(void)
{
    return bkl_depth > 0;
}

/** Drop the whole inherited depth, as the dispatcher does for a row. */
int bkl_drop(void)
{
    int depth = bkl_depth;

    bkl_depth = 0;
    return depth;
}

/** Restore the caller's depth after the row, with no lock left by it. */
void bkl_take(int depth)
{
    if (depth) {
        CHECK("a row left no big lock before restoring its caller", bkl_depth == 0);
        bkl_depth = depth;
    }
}

int32_t drm_modeset_lock(struct drm_device *dev)
{
    (void)dev;
    mode_lock_depth++;
    return 0;
}

void drm_modeset_lock_nointr(struct drm_device *dev)
{
    (void)dev;
    mode_lock_depth++;
}

void drm_modeset_unlock(struct drm_device *dev)
{
    (void)dev;
    mode_lock_depth--;
}

/** No region is shared with a VM here. */
int32_t vgpu_shm_start(void)
{
    return 0;
}

int32_t vkms_probe(void)
{
    return -ENODEV;
}

int32_t vgem_probe(void)
{
    return -ENODEV;
}

void arch_drm_probe(int phase)
{
    (void)phase;
}

static int client_registers, client_unregisters;

void drm_client_dev_register(struct drm_device *dev)
{
    (void)dev;
    client_registers++;
}

void drm_client_dev_unregister(struct drm_device *dev)
{
    (void)dev;
    client_unregisters++;
}

/** The /sys entries, which drm_sysfs.c's own rows in procfs_hosttest.c
 * check; here a device always gets them. */
int32_t drm_sysfs_register(struct drm_device *dev)
{
    (void)dev;
    return 0;
}

void drm_sysfs_unregister(struct drm_device *dev)
{
    (void)dev;
}

/** The debugfs files, which a boot checks; here a device has none. */
void drm_debugfs_init(void)
{}

void drm_debugfs_register(struct drm_device *dev)
{
    (void)dev;
}

void drm_debugfs_unregister(struct drm_device *dev)
{
    (void)dev;
}

/* The registry: the nodes registered now. */
#define NODES 8
static struct devfs_node *nodes[NODES];

int32_t devfs_node_register(struct devfs_node *node)
{
    int i;

    for (i = 0; i < NODES; i++) {
        if (nodes[i] && strcmp(nodes[i]->name, node->name) == 0) {
            return -EEXIST;
        }
    }
    for (i = 0; i < NODES; i++) {
        if (!nodes[i]) {
            nodes[i] = node;
            return 0;
        }
    }
    return -ENOSPC;
}

void devfs_node_unregister(struct devfs_node *node)
{
    int i;

    for (i = 0; i < NODES; i++) {
        if (nodes[i] == node) {
            nodes[i] = NULL;
        }
    }
}

/** The node registered as `name`, or NULL. */
static struct devfs_node *node_named(const char *name)
{
    int i;

    for (i = 0; i < NODES; i++) {
        if (nodes[i] && strcmp(nodes[i]->name, name) == 0) {
            return nodes[i];
        }
    }
    return NULL;
}

static const void *zapped_obj;
static int zaps;

void paging_zap_device(const void *obj, uint32_t pgoff, uint32_t count)
{
    (void)pgoff;
    (void)count;
    zapped_obj = obj;
    zaps++;
}

void wait_queue_init(wait_queue_t *wq)
{
    (void)wq;
}

static int wakes;

void wq_wake(wait_queue_t *wq, uint32_t kinds)
{
    (void)wq;
    (void)kinds;
    wakes++;
}

void wq_wake_all(wait_queue_t *wq)
{
    wq_wake(wq, WQ_ALL);
}

/** The fence model counts event completion; the fence test drives callbacks. */
int32_t dma_fence_signal(struct dma_fence *f)
{
    if (f->signaled)
        return -EINVAL;
    f->signaled = 1;
    return 0;
}

/** Release a modeled fence after its last holder. */
void dma_fence_put(struct dma_fence *f)
{
    if (f && --f->refs == 0)
        kfree(f);
}

/** No PRIME objects are made by the core rows. */
void dma_buf_put(struct dma_buf *buf)
{
    CHECK("no PRIME reference in core model", buf == NULL);
}

/** No PRIME objects are made by the core rows: a table's cache is NULL. */
struct dma_buf *dma_buf_ref(struct dma_buf *buf)
{
    CHECK("no PRIME reference in core model", buf == NULL);
    return buf;
}

/** Make a modeled reservation empty. */
void dma_resv_init(struct dma_resv *r)
{
    k_memset(r, 0, sizeof(*r));
}

/** Release modeled reservation slots at the object's last reference. */
void dma_resv_fini(struct dma_resv *r)
{
    uint32_t i;
    dma_fence_put(r->write);
    for (i = 0; i < r->nreads; i++)
        dma_fence_put(r->reads[i]);
    kfree(r->reads);
    k_memset(r, 0, sizeof(*r));
}

/** A wait would sleep: no row reaches one. */
int wq_wait_event(wait_queue_t *wq, wq_cond_fn cond, void *arg)
{
    (void)wq;
    (void)cond;
    (void)arg;
    CHECK("no read sleeps", 0);
    return -EINTR;
}

/* Run once by the next copy_to_user before it copies: a second reader
 * whose read runs while the first's copy sleeps. */
static void (*copy_hook)(void);
/* A row that expects both copies without an inherited big lock. */
static int copies_unlocked, unlocked_copies, copy_out_fault;

int copy_to_user(void __user *dst, const void *src, size_t n)
{
    void (*hook)(void) = copy_hook;

    copy_hook = NULL;
    if (copies_unlocked) {
        CHECK("copy out without the inherited big lock", bkl_depth == 0);
        unlocked_copies++;
    }
    if (hook) {
        hook();
    }
    if (copy_out_fault || (uintptr_t)dst < 4096) {
        return -1;
    }
    memcpy((void *)dst, src, n);
    return 0;
}

int copy_from_user(void *dst, const void __user *src, size_t n)
{
    if (copies_unlocked) {
        CHECK("copy in without the inherited big lock", bkl_depth == 0);
        unlocked_copies++;
    }
    if ((uintptr_t)src < 4096) {
        return -1;
    }
    memcpy(dst, (const void *)src, n);
    return 0;
}

/* ---- the shmem helper's frames: pages of the host's ---- */

#define MODEL_PAGES 64
static int frames_live;

uintptr_t shmem_resident(const struct shmem *sh, uint32_t pgoff)
{
    if (!sh->leaves || pgoff >= MODEL_PAGES) {
        return 0;
    }
    return sh->leaves[0][pgoff];
}

int shmem_allocate(struct shmem *sh, uint64_t off, uint64_t len)
{
    uint64_t p;

    if (!sh->leaves) {
        sh->leaves = (uintptr_t **)calloc(1, sizeof(uintptr_t *));
        sh->leaves[0] = (uintptr_t *)calloc(MODEL_PAGES, sizeof(uintptr_t));
        sh->nleaves = 1;
    }
    for (p = off / PAGE_SIZE; p < (off + len + PAGE_SIZE - 1) / PAGE_SIZE; p++) {
        if (p >= MODEL_PAGES) {
            return -ENOSPC;
        }
        if (!sh->leaves[0][p]) {
            void *page = aligned_alloc(PAGE_SIZE, PAGE_SIZE);

            k_memset(page, 0, PAGE_SIZE);
            sh->leaves[0][p] = (uintptr_t)page;
            sh->held++;
            frames_live++;
        }
    }
    return 0;
}

void shmem_free(struct shmem *sh)
{
    uint32_t p;

    if (!sh->leaves) {
        return;
    }
    for (p = 0; p < MODEL_PAGES; p++) {
        if (sh->leaves[0][p]) {
            free((void *)sh->leaves[0][p]);
            frames_live--;
        }
    }
    free(sh->leaves[0]);
    free(sh->leaves);
    sh->leaves = NULL;
    sh->held = 0;
    sh->size = 0;
}

/* ---- device regions: what vm_device_map was given ---- */

struct region {
    struct vm_file vf;
    const struct vm_device_ops *ops;
    void *obj;
    uint32_t cache;
};

int vm_device_map(vma_t *vma, const struct vm_device_ops *ops, void *obj, uint32_t cache)
{
    struct region *r;

    if (vma->type != VM_SHARED) {
        return -EINVAL;
    }
    r = (struct region *)calloc(1, sizeof(*r));
    r->ops = ops;
    r->obj = obj;
    r->cache = cache;
    vma->file = &r->vf;
    vma->prot |= VM_PROT_IO | (cache & (VM_PROT_WC | VM_PROT_UC));
    return 0;
}

/** The unmap of the last region of `vma`: the object's reference goes. */
static void region_unmap(vma_t *vma)
{
    struct region *r = (struct region *)vma->file;

    r->ops->put(r->obj);
    free(r);
    vma->file = NULL;
}

/* ---- the process side of kcmp ---- */

#define PROCS 4
static process_t *procs[PROCS];

process_t *proc_named(uint32_t pid)
{
    int i;

    for (i = 0; i < PROCS; i++) {
        if (procs[i] && procs[i]->pid == pid && pid != 0) {
            return procs[i];
        }
    }
    return NULL;
}

fd_entry_t *fd_get(fd_table_t *table, int fd)
{
    if (!table || fd < 0 || fd >= table->cap || table->chunks[0][fd].type == FD_TYPE_NONE) {
        return NULL;
    }
    return &table->chunks[0][fd];
}

/* The epoll set of the model: descriptor 7 of a table, whose one
 * registration is of descriptor `epoll_tfd` on the file `epoll_file`. */
static int epoll_tfd;
static const struct open_file *epoll_file;

int32_t epoll_target(const fd_entry_t *e, int tfd, uint32_t toff, const struct open_file **ofd)
{
    if (e->type != FD_TYPE_ANON) {
        return -EINVAL;
    }
    if (tfd != epoll_tfd || toff != 0) {
        return -ENOENT;
    }
    *ofd = epoll_file;
    return 0;
}

static const void *undo_owner;

const void *sysv_sem_undo_list(const struct tgroup *tg)
{
    return tg == undo_owner ? tg : NULL;
}

void krandom_bytes(void *buf, uint32_t n)
{
    uint32_t i;

    for (i = 0; i < n; i++) {
        ((uint8_t *)buf)[i] = (uint8_t)(i * 37u + 11u);
    }
}

/* ---- the handlers of the other files' tasks ---- */

#define FOREIGN(name)                                                       \
    int32_t name(struct drm_device *dev, void *data, struct drm_file *file) \
    {                                                                       \
        (void)dev;                                                          \
        (void)data;                                                         \
        (void)file;                                                         \
        return -EOPNOTSUPP;                                                 \
    }
FOREIGN(drm_prime_handle_to_fd_ioctl)
FOREIGN(drm_prime_fd_to_handle_ioctl)
static int core_mode_big, core_mode_held;

/** Record the core KMS row's locks; this model has no mode objects. */
int32_t drm_mode_getresources(struct drm_device *dev, void *data, struct drm_file *file)
{
    (void)dev;
    (void)data;
    (void)file;
    core_mode_big = bkl_depth;
    core_mode_held = mode_lock_depth;
    return -EOPNOTSUPP;
}
FOREIGN(drm_mode_getplane_res)
FOREIGN(drm_mode_getcrtc)
FOREIGN(drm_mode_getplane)
FOREIGN(drm_mode_getencoder)
FOREIGN(drm_mode_getconnector)
FOREIGN(drm_mode_gamma_get_ioctl)
FOREIGN(drm_mode_getproperty_ioctl)
FOREIGN(drm_mode_getblob_ioctl)
FOREIGN(drm_mode_createblob_ioctl)
FOREIGN(drm_mode_destroyblob_ioctl)
FOREIGN(drm_mode_obj_get_properties_ioctl)
FOREIGN(drm_mode_getfb)
FOREIGN(drm_mode_getfb2_ioctl)
FOREIGN(drm_mode_addfb_ioctl)
FOREIGN(drm_mode_addfb2_ioctl)
FOREIGN(drm_mode_rmfb_ioctl)
FOREIGN(drm_mode_closefb_ioctl)
FOREIGN(drm_mode_create_lease_ioctl)
FOREIGN(drm_mode_list_lessees_ioctl)
FOREIGN(drm_mode_get_lease_ioctl)
FOREIGN(drm_mode_revoke_lease_ioctl)
FOREIGN(drm_mode_setcrtc)
FOREIGN(drm_mode_setplane)
FOREIGN(drm_mode_cursor_ioctl)
FOREIGN(drm_mode_cursor2_ioctl)
FOREIGN(drm_mode_gamma_set_ioctl)
FOREIGN(drm_connector_property_set_ioctl)
FOREIGN(drm_mode_obj_set_property_ioctl)
FOREIGN(drm_mode_page_flip_ioctl)
FOREIGN(drm_mode_dirtyfb_ioctl)
FOREIGN(drm_mode_atomic_ioctl)
FOREIGN(drm_wait_vblank_ioctl)
FOREIGN(drm_crtc_get_sequence_ioctl)
FOREIGN(drm_crtc_queue_sequence_ioctl)
FOREIGN(drm_syncobj_create_ioctl)
FOREIGN(drm_syncobj_destroy_ioctl)
FOREIGN(drm_syncobj_handle_to_fd_ioctl)
FOREIGN(drm_syncobj_fd_to_handle_ioctl)
FOREIGN(drm_syncobj_transfer_ioctl)
/** A syncobj wait's model records the lock its dispatcher passed it. */
int32_t drm_syncobj_wait_ioctl(struct drm_device *dev, void *data, struct drm_file *file)
{
    (void)dev;
    (void)data;
    (void)file;
    CHECK("SYNCOBJ_WAIT runs without the big lock", bkl_depth == 0);
    return -EOPNOTSUPP;
}
FOREIGN(drm_syncobj_timeline_wait_ioctl)
FOREIGN(drm_syncobj_eventfd_ioctl)
FOREIGN(drm_syncobj_reset_ioctl)
FOREIGN(drm_syncobj_signal_ioctl)
FOREIGN(drm_syncobj_timeline_signal_ioctl)
FOREIGN(drm_syncobj_query_ioctl)

/** drm_vblank.c's turning off of an unplugged device's CRTCs: the model's
 * devices have none. */
void drm_vblank_unplug(struct drm_device *dev)
{
    (void)dev;
}

/** drm_mode.c's release of a file's framebuffers and blobs: the model's
 * files make none. */
void drm_mode_file_release(struct drm_file *file)
{
    (void)file;
}

/** drm_syncobj.c's release of a file's sync objects: the model's files make
 * none. */
void drm_syncobj_release(struct drm_file *file)
{
    (void)file;
}

/* ---- the fake driver ---- */

struct fake_echo {
    uint32_t value;
};

#define FAKE_IOCTL_ECHO DRM_IOWR(DRM_COMMAND_BASE + 0, struct fake_echo)

/** The driver's own ioctl: the value plus one. */
static int32_t fake_echo(struct drm_device *dev, void *data, struct drm_file *file)
{
    (void)dev;
    (void)file;
    ((struct fake_echo *)data)->value++;
    return 0;
}

/* What a row's handler found held: the big lock's and the mode lock's
 * depths. */
struct fake_locks {
    uint32_t big, mode;
};

#define FAKE_IOCTL_LOCKED   DRM_IOR(DRM_COMMAND_BASE + 1, struct fake_locks)
#define FAKE_IOCTL_UNLOCKED DRM_IOWR(DRM_COMMAND_BASE + 2, struct fake_locks)
#define FAKE_IOCTL_KMS      DRM_IOR(DRM_COMMAND_BASE + 3, struct fake_locks)
#define FAKE_IOCTL_READ     DRM_IOR(DRM_COMMAND_BASE + 4, struct fake_locks)

/** A row that reports the locks it runs under. */
static int32_t fake_locks(struct drm_device *dev, void *data, struct drm_file *file)
{
    struct fake_locks *l = (struct fake_locks *)data;

    (void)dev;
    (void)file;
    l->big = (uint32_t)bkl_depth;
    l->mode = (uint32_t)mode_lock_depth;
    return 0;
}

/* The flags of the core's kinds of row: a locked one, an unlocked render
 * row, ATOMIC's, and a KMS read such as GETCRTC. */
static const struct drm_ioctl_desc fake_ioctls[] = {
    {FAKE_IOCTL_ECHO, DRM_AUTH | DRM_RENDER_ALLOW, fake_echo, "FAKE_ECHO"},
    {FAKE_IOCTL_LOCKED, DRM_RENDER_ALLOW, fake_locks, "FAKE_LOCKED"},
    {FAKE_IOCTL_UNLOCKED, DRM_RENDER_ALLOW | DRM_UNLOCKED, fake_locks, "FAKE_UNLOCKED"},
    {FAKE_IOCTL_KMS, DRM_MASTER | DRM_UNLOCKED | DRM_KMS | DRM_MODESET_LOCK, fake_locks,
     "FAKE_KMS"},
    {FAKE_IOCTL_READ, DRM_MODESET_LOCK, fake_locks, "FAKE_READ"},
};

static int opens, postcloses, unplugs, releases;

/** The driver's `open`: counted. */
static int32_t fake_open(struct drm_device *dev, struct drm_file *file)
{
    (void)dev;
    (void)file;
    opens++;
    return 0;
}

/** The driver's `postclose`: counted. */
static void fake_postclose(struct drm_device *dev, struct drm_file *file)
{
    (void)dev;
    (void)file;
    postcloses++;
}

/** The driver's `unplug`: counted. */
static void fake_unplug(struct drm_device *dev)
{
    (void)dev;
    unplugs++;
}

/** The driver's `release`: counted; the record is static. */
static void fake_release(struct drm_device *dev)
{
    (void)dev;
    releases++;
}

/** Dumb buffers of the shmem helper, mapped write-combining. */
static int32_t fake_dumb_create(struct drm_file *file, struct drm_device *dev,
                                struct drm_mode_create_dumb *args)
{
    int32_t rc = drm_gem_shmem_dumb_create(file, dev, args);
    struct drm_gem_object *obj;

    if (rc == 0) {
        obj = drm_gem_object_lookup(file, args->handle);
        ((struct drm_gem_shmem_object *)obj)->map_wc = 1;
        drm_gem_object_put(obj);
    }
    return rc;
}

static const struct drm_driver fake_driver = {
    {"fakedrm", "fake driver of the host test", "20261109", 1, 2, 3,
     DRIVER_GEM | DRIVER_MODESET | DRIVER_ATOMIC | DRIVER_RENDER | DRIVER_SYNCOBJ, DRM_BUS_PCI},
    fake_open,
    fake_postclose,
    fake_dumb_create,
    NULL,
    fake_unplug,
    fake_ioctls,
    (int)(sizeof(fake_ioctls) / sizeof(fake_ioctls[0])),
    fake_release};

static const struct drm_driver plain_driver = {
    {"plaindrm", "a driver without display", "20261109", 2, 0, 0, DRIVER_GEM, DRM_BUS_PLATFORM},
    NULL,
    NULL,
    NULL,
    NULL,
    NULL,
    NULL,
    0,
    NULL};

static struct drm_device fake_dev, plain_dev;
static pci_device_t fake_pci;

/* ---- the processes and their descriptors ---- */

static process_t root_proc, user_a, user_b, child_proc;
static struct tgroup tg_root, tg_a, tg_b, tg_child;
static struct mm mm_root, mm_a, mm_b;

/** Run as `p`. */
static void run_as(process_t *p)
{
    cpu_table[0].current = p;
}

/** Fill `p` as a process of `pid` and effective and real `uid`. */
static void proc_fill(process_t *p, struct tgroup *tg, struct mm *mm, uint32_t pid, uint32_t uid)
{
    p->pid = pid;
    p->tgid = pid;
    p->tg = tg;
    p->mm = mm;
    mm->dumpable = 1;
    p->ruid = p->euid = p->suid = uid;
    p->rgid = p->egid = p->sgid = uid;
    k_strncpy(p->comm, "test", sizeof(p->comm));
}

/** Open the node `name` on `e` as `p` with the open flags `flags`, as devfs
 * opens a registered node: the descriptor's operations, a new description,
 * then the node's `open`. */
static int32_t open_node(const char *name, process_t *p, int flags, fd_entry_t *e)
{
    struct devfs_node *node = node_named(name);
    int32_t rc;

    k_memset(e, 0, sizeof(*e));
    if (!node) {
        return -ENOENT;
    }
    run_as(p);
    e->type = FD_TYPE_DEVICE;
    e->ops = node->ops;
    e->flags = FD_FLAG_READ | FD_FLAG_WRITE;
    e->ofd = (struct open_file *)calloc(1, sizeof(struct open_file));
    e->ofd->refcount = 1;
    e->ofd->status_flags = OFD_NONBLOCK;
    rc = node->open(node, e, flags);
    if (rc != 0) {
        free(e->ofd);
        e->ofd = NULL;
    }
    return rc;
}

/** Close the descriptor `e`, releasing the file with the description's
 * last descriptor. */
static void close_node(fd_entry_t *e)
{
    if (!e->ofd) {
        return;
    }
    if (--e->ofd->refcount == 0) {
        e->ops->close(e);
        free(e->ofd);
    }
    e->ofd = NULL;
    e->type = FD_TYPE_NONE;
}

/** A copy of the descriptor `e`, as dup and fork make. */
static void dup_node(const fd_entry_t *e, fd_entry_t *copy)
{
    *copy = *e;
    copy->ofd->refcount++;
}

/** The ioctl `cmd` of `e` with `arg`, as `p`. */
static int32_t ioc(fd_entry_t *e, process_t *p, uint32_t cmd, void *arg)
{
    run_as(p);
    return e->ops->ioctl(e, cmd, (uintptr_t)arg);
}

/* ---- the master's listener ---- */

static int master_changes;
static struct drm_file *master_seen;

/** The listener: counts the changes and keeps the last master. */
static void master_listen(struct drm_device *dev, struct drm_file *master)
{
    (void)dev;
    master_changes++;
    master_seen = master;
}

static struct drm_master_notifier listener = {master_listen, NULL};

/* ---- rows ---- */

/** The devices register their nodes under the lowest indexes, with
 * Linux's numbers and udev's modes, and their bus names. */
static void test_register(void)
{
    struct devfs_node *card, *render;

    drm_master_notifier_register(&listener);
    drm_dev_init(&fake_dev, &fake_driver);
    fake_pci.bus = 0;
    fake_pci.device = 2;
    fake_pci.function = 0;
    fake_dev.pci = &fake_pci;
    CHECK("init gives one reference", fake_dev.refs == 1 && fake_dev.index == -1);
    CHECK("register the fake device", drm_dev_register(&fake_dev) == 0);
    drm_dev_init(&plain_dev, &plain_driver);
    CHECK("register the plain device", drm_dev_register(&plain_dev) == 0);
    card = node_named("dri/card0");
    render = node_named("dri/renderD128");
    CHECK("card0 registered", card && card->major == 226 && card->minor == 0);
    CHECK("renderD128 registered", render && render->major == 226 && render->minor == 128);
    CHECK("card mode 0660, render 0666",
          card && render && card->mode == 0660 && render->mode == 0666);
    CHECK("card1 has no render node", node_named("dri/card1") && !node_named("dri/renderD129"));
    CHECK("indexes in order", fake_dev.index == 0 && plain_dev.index == 1);
    CHECK("found by index", drm_dev_at(0) == &fake_dev && drm_dev_at(1) == &plain_dev &&
                                !drm_dev_at(2) && !drm_dev_at(-1));
    CHECK("PCI bus name", strcmp(fake_dev.unique, "pci:0000:00:02.0") == 0);
    CHECK("platform bus name", strcmp(plain_dev.unique, "plaindrm") == 0);
    CHECK("the console told twice", client_registers == 2);
}

/** VERSION, GET_UNIQUE, GET_CLIENT, GET_STATS and SET_VERSION. */
static void test_identity(void)
{
    fd_entry_t card, render, second;
    struct drm_version v;
    struct drm_unique u;
    struct drm_client c;
    struct drm_stats st;
    struct drm_set_version sv;
    char name[4], date[16], desc[64], uniq[32];

    CHECK("open card0", open_node("dri/card0", &user_a, 0, &card) == 0);
    CHECK("open renderD128", open_node("dri/renderD128", &user_a, 0, &render) == 0);
    CHECK("the driver saw both opens", opens == 2);

    k_memset(&v, 0, sizeof(v));
    v.name_len = sizeof(name);
    v.name = name;
    v.date_len = sizeof(date);
    v.date = date;
    v.desc_len = sizeof(desc);
    v.desc = desc;
    CHECK("VERSION on the render node", ioc(&render, &user_a, DRM_IOCTL_VERSION, &v) == 0);
    CHECK("VERSION numbers",
          v.version_major == 1 && v.version_minor == 2 && v.version_patchlevel == 3);
    CHECK("VERSION name cut, length whole", v.name_len == 7 && memcmp(name, "fake", 4) == 0);
    CHECK("VERSION date", v.date_len == 8 && memcmp(date, "20261109", 8) == 0);
    v.name = (char *)16;
    v.name_len = 7;
    CHECK("VERSION to a bad buffer faults", ioc(&card, &user_a, DRM_IOCTL_VERSION, &v) == -EFAULT);

    u.unique_len = sizeof(uniq);
    u.unique = uniq;
    CHECK("GET_UNIQUE empty before SET_VERSION",
          ioc(&card, &user_a, DRM_IOCTL_GET_UNIQUE, &u) == 0 && u.unique_len == 0);
    CHECK("GET_UNIQUE refused on the render node",
          ioc(&render, &user_a, DRM_IOCTL_GET_UNIQUE, &u) == -EACCES);

    k_memset(&c, 0, sizeof(c));
    CHECK("GET_CLIENT 0", ioc(&card, &user_a, DRM_IOCTL_GET_CLIENT, &c) == 0 && c.auth == 1 &&
                              c.pid == 200 && c.uid == 65534);
    c.idx = 1;
    CHECK("GET_CLIENT 1", ioc(&card, &user_a, DRM_IOCTL_GET_CLIENT, &c) == -EINVAL);

    k_memset(&st, 0xff, sizeof(st));
    CHECK("GET_STATS, past the stack's 128 bytes",
          sizeof(st) <= 128 || (ioc(&card, &user_a, DRM_IOCTL_GET_STATS, &st) == 0 &&
                                st.count == 0 && st.data[14].value == 0));

    sv.drm_di_major = 1;
    sv.drm_di_minor = 4;
    sv.drm_dd_major = -1;
    sv.drm_dd_minor = 0;
    CHECK("SET_VERSION by the master", ioc(&card, &user_a, DRM_IOCTL_SET_VERSION, &sv) == 0 &&
                                           sv.drm_dd_major == 1 && sv.drm_dd_minor == 2);
    sv.drm_di_minor = 5;
    CHECK("SET_VERSION past the interface",
          ioc(&card, &user_a, DRM_IOCTL_SET_VERSION, &sv) == -EINVAL && sv.drm_di_minor == 4);
    u.unique_len = 4;
    CHECK("GET_UNIQUE short after SET_VERSION",
          ioc(&card, &user_a, DRM_IOCTL_GET_UNIQUE, &u) == 0 && u.unique_len == 16);
    u.unique_len = sizeof(uniq);
    k_memset(uniq, 0, sizeof(uniq));
    CHECK("GET_UNIQUE whole", ioc(&card, &user_a, DRM_IOCTL_GET_UNIQUE, &u) == 0 &&
                                  u.unique_len == 16 && strcmp(uniq, "pci:0000:00:02.0") == 0);
    CHECK("open card0 again", open_node("dri/card0", &user_a, 0, &second) == 0);
    u.unique_len = sizeof(uniq);
    CHECK("GET_UNIQUE of a file opened under that master",
          ioc(&second, &user_a, DRM_IOCTL_GET_UNIQUE, &u) == 0 && u.unique_len == 16);
    close_node(&second);

    close_node(&render);
    close_node(&card);
    CHECK("the driver saw each close", postcloses == 3);
    CHECK("no master after the close", fake_dev.master == NULL && fake_dev.open_count == 0);
}

/** GET_CAP and SET_CLIENT_CAP, on a device with display and one without. */
static void test_caps(void)
{
    fd_entry_t card, render, plain;
    struct drm_get_cap cap;
    struct drm_set_client_cap cc;
    struct drm_file *f;

    open_node("dri/card0", &user_a, 0, &card);
    open_node("dri/renderD128", &user_a, 0, &render);
    open_node("dri/card1", &user_a, 0, &plain);
    f = (struct drm_file *)card.priv;

    cap.capability = DRM_CAP_DUMB_BUFFER;
    CHECK("DUMB_BUFFER", ioc(&card, &user_a, DRM_IOCTL_GET_CAP, &cap) == 0 && cap.value == 1);
    cap.capability = DRM_CAP_PRIME;
    CHECK("PRIME on the render node",
          ioc(&render, &user_a, DRM_IOCTL_GET_CAP, &cap) == 0 && cap.value == 3);
    cap.capability = DRM_CAP_SYNCOBJ;
    CHECK("SYNCOBJ", ioc(&card, &user_a, DRM_IOCTL_GET_CAP, &cap) == 0 && cap.value == 1);
    cap.capability = DRM_CAP_SYNCOBJ_TIMELINE;
    CHECK("SYNCOBJ_TIMELINE", ioc(&card, &user_a, DRM_IOCTL_GET_CAP, &cap) == 0 && cap.value == 0);
    cap.capability = DRM_CAP_CURSOR_WIDTH;
    CHECK("CURSOR_WIDTH", ioc(&card, &user_a, DRM_IOCTL_GET_CAP, &cap) == 0 && cap.value == 64);
    cap.capability = 0x99;
    CHECK("an unknown cap", ioc(&card, &user_a, DRM_IOCTL_GET_CAP, &cap) == -EINVAL);
    cap.capability = DRM_CAP_TIMESTAMP_MONOTONIC;
    CHECK("TIMESTAMP_MONOTONIC without display",
          ioc(&plain, &user_a, DRM_IOCTL_GET_CAP, &cap) == 0 && cap.value == 1);
    cap.capability = DRM_CAP_DUMB_BUFFER;
    CHECK("DUMB_BUFFER without display",
          ioc(&plain, &user_a, DRM_IOCTL_GET_CAP, &cap) == -EOPNOTSUPP && cap.value == 0);

    cc.capability = DRM_CLIENT_CAP_WRITEBACK_CONNECTORS;
    cc.value = 1;
    CHECK("WRITEBACK before ATOMIC", ioc(&card, &user_a, DRM_IOCTL_SET_CLIENT_CAP, &cc) == -EINVAL);
    cc.capability = DRM_CLIENT_CAP_ATOMIC;
    cc.value = 3;
    CHECK("ATOMIC 3", ioc(&card, &user_a, DRM_IOCTL_SET_CLIENT_CAP, &cc) == -EINVAL);
    cc.value = 1;
    CHECK("ATOMIC", ioc(&card, &user_a, DRM_IOCTL_SET_CLIENT_CAP, &cc) == 0 && f->atomic == 1 &&
                        f->universal_planes == 1 && f->aspect_ratio == 1);
    cc.capability = DRM_CLIENT_CAP_CURSOR_PLANE_HOTSPOT;
    CHECK("HOTSPOT without the feature",
          ioc(&card, &user_a, DRM_IOCTL_SET_CLIENT_CAP, &cc) == -EOPNOTSUPP);
    cc.capability = DRM_CLIENT_CAP_UNIVERSAL_PLANES;
    CHECK("SET_CLIENT_CAP refused on the render node",
          ioc(&render, &user_a, DRM_IOCTL_SET_CLIENT_CAP, &cc) == -EACCES);
    CHECK("SET_CLIENT_CAP without display",
          ioc(&plain, &user_a, DRM_IOCTL_SET_CLIENT_CAP, &cc) == -EOPNOTSUPP);
    k_strncpy(user_a.comm, "Xorg", sizeof(user_a.comm));
    cc.capability = DRM_CLIENT_CAP_ATOMIC;
    CHECK("ATOMIC refused to the modesetting DDX",
          ioc(&card, &user_a, DRM_IOCTL_SET_CLIENT_CAP, &cc) == -EOPNOTSUPP);
    k_strncpy(user_a.comm, "test", sizeof(user_a.comm));

    close_node(&plain);
    close_node(&render);
    close_node(&card);
}

/** The dispatcher's refusals and copies. */
static void test_dispatch(void)
{
    fd_entry_t card, render, other, excl;
    struct fake_echo echo;
    struct drm_auth auth;
    struct drm_unique u;

    open_node("dri/card0", &user_a, 0, &card);
    open_node("dri/renderD128", &user_b, 0, &render);
    open_node("dri/card0", &user_b, 0, &other);

    CHECK("O_EXCL refused", open_node("dri/card0", &user_a, DRM_O_EXCL, &excl) == -EBUSY);
    echo.value = 41;
    CHECK("another type",
          ioc(&card, &user_a, UAPI_IOWR('v', 0, struct fake_echo), &echo) == -ENOTTY);
    CHECK("a core number no row names",
          ioc(&card, &user_a, DRM_IOWR(0x04, struct fake_echo), &echo) == -EINVAL);
    CHECK("a core number past the table",
          ioc(&card, &user_a, DRM_IOWR(0xE0, struct fake_echo), &echo) == -EINVAL);
    CHECK("a driver number past its rows",
          ioc(&card, &user_a, DRM_IOWR(DRM_COMMAND_BASE + 5, struct fake_echo), &echo) == -EINVAL);
    CHECK("the driver's ioctl",
          ioc(&card, &user_a, FAKE_IOCTL_ECHO, &echo) == 0 && echo.value == 42);
    CHECK("DRM_AUTH: a render node's client needs none",
          ioc(&render, &user_b, FAKE_IOCTL_ECHO, &echo) == 0 && echo.value == 43);
    CHECK("DRM_AUTH: an unauthenticated card client is refused",
          ioc(&other, &user_b, FAKE_IOCTL_ECHO, &echo) == -EACCES && echo.value == 43);
    CHECK("a write-only number reads nothing back",
          ioc(&card, &user_a, DRM_IOW(DRM_COMMAND_BASE + 0, struct fake_echo), &echo) == 0 &&
              echo.value == 43);
    CHECK("a read-only number copies nothing in",
          ioc(&card, &user_a, DRM_IOR(DRM_COMMAND_BASE + 0, struct fake_echo), &echo) == 0 &&
              echo.value == 1);
    CHECK("a bad argument faults", ioc(&card, &user_a, FAKE_IOCTL_ECHO, (void *)8) == -EFAULT);
    CHECK("GET_MAGIC refused on the render node",
          ioc(&render, &user_b, DRM_IOCTL_GET_MAGIC, &auth) == -EACCES);
    CHECK("DRM_MASTER refuses a client",
          ioc(&other, &user_b, DRM_IOCTL_AUTH_MAGIC, &auth) == -EACCES);
    u.unique_len = 0;
    u.unique = NULL;
    CHECK("DRM_ROOT_ONLY refuses the master without privilege",
          ioc(&card, &user_a, DRM_IOCTL_SET_UNIQUE, &u) == -EACCES);
    CHECK("SET_UNIQUE by a privileged master",
          ioc(&card, &root_proc, DRM_IOCTL_SET_UNIQUE, &u) == -EINVAL);
    CHECK("BLOCK by a privileged master", ioc(&card, &root_proc, DRM_IOCTL_BLOCK, &u) == 0);

    close_node(&other);
    close_node(&render);
    close_node(&card);
}

/**
 * The locks a row runs under: the big lock unless the row is DRM_UNLOCKED,
 * and a KMS row's DRM_UNLOCKED only on a device with DRIVER_KMS_UNLOCKED;
 * the master rule's check under the big lock, let go before the handler;
 * the mode lock across a DRM_MODESET_LOCK row; and on an unplugged device
 * the row refused with both locks let go.
 */
static void test_locks(void)
{
    fd_entry_t card, render, other;
    struct fake_locks l;

    open_node("dri/card0", &user_a, 0, &card);
    open_node("dri/renderD128", &user_b, 0, &render);
    open_node("dri/card0", &user_b, 0, &other);
    bkl_enters = 0;
    CHECK("locks: a locked row runs under the big lock alone",
          ioc(&render, &user_b, FAKE_IOCTL_LOCKED, &l) == 0 && l.big == 1 && l.mode == 0 &&
              bkl_enters == 1);
    bkl_enters = 0;
    CHECK("locks: an unlocked render row runs under neither",
          ioc(&render, &user_b, FAKE_IOCTL_UNLOCKED, &l) == 0 && l.big == 0 && l.mode == 0 &&
              bkl_enters == 0);
    {
        struct drm_syncobj_wait wait;

        k_memset(&wait, 0, sizeof(wait));
        bkl_enters = 0;
        CHECK("locks: a syncobj wait enters without a big-lock take",
              ioc(&render, &user_b, DRM_IOCTL_SYNCOBJ_WAIT, &wait) == -EOPNOTSUPP &&
                  bkl_enters == 0);
    }
    CHECK("locks: a KMS read runs under the big lock and the mode lock",
          ioc(&card, &user_a, FAKE_IOCTL_READ, &l) == 0 && l.big == 1 && l.mode == 1);
    CHECK("locks: ATOMIC's row keeps the big lock on a driver that does not opt out",
          ioc(&card, &user_a, FAKE_IOCTL_KMS, &l) == 0 && l.big == 1 && l.mode == 1);
    {
        struct drm_mode_card_res res;

        k_memset(&res, 0, sizeof(res));
        CHECK("locks: core KMS keeps the big lock without driver opt-in",
              ioc(&card, &user_a, DRM_IOCTL_MODE_GETRESOURCES, &res) == -EOPNOTSUPP &&
                  core_mode_big == 1 && core_mode_held == 1);
        fake_dev.features |= DRIVER_KMS_UNLOCKED;
        CHECK("locks: core KMS uses only the mode mutex with driver opt-in",
              ioc(&card, &user_a, DRM_IOCTL_MODE_GETRESOURCES, &res) == -EOPNOTSUPP &&
                  core_mode_big == 0 && core_mode_held == 1);
    }
    CHECK("locks: ATOMIC's row runs under the mode lock alone with DRIVER_KMS_UNLOCKED",
          ioc(&card, &user_a, FAKE_IOCTL_KMS, &l) == 0 && l.big == 0 && l.mode == 1);
    CHECK("locks: the master rule refuses a client before the mode lock",
          ioc(&other, &user_b, FAKE_IOCTL_KMS, &l) == -EACCES);
    fake_dev.features &= ~DRIVER_KMS_UNLOCKED;
    fake_dev.unplugged = 1;
    CHECK("locks: an unplugged device refuses a KMS row",
          ioc(&card, &user_a, FAKE_IOCTL_READ, &l) == -ENODEV);
    fake_dev.unplugged = 0;
    CHECK("locks: every row let both locks go", bkl_depth == 0 && mode_lock_depth == 0);
    close_node(&other);
    close_node(&render);
    close_node(&card);
}

/**
 * A traced entry or a kernel caller can enter with a big lock.
 * An unlocked row gives that whole depth back across copies and the handler,
 * including failures, and restores it before returning to the caller.
 * `drm-core inherited` runs these rows alone after the device setup.
 */
static void test_inherited(void)
{
    fd_entry_t card, render, other;
    struct fake_locks l;
    int depth;

    open_node("dri/card0", &user_a, 0, &card);
    open_node("dri/renderD128", &user_b, 0, &render);
    open_node("dri/card0", &user_b, 0, &other);
    k_memset(&l, 0, sizeof(l));
    for (depth = 1; depth <= 2; depth++) {
        bkl_depth = depth;
        copies_unlocked = 1;
        unlocked_copies = 0;
        CHECK("inherited: render row and both copies without the big lock",
              ioc(&render, &user_b, FAKE_IOCTL_UNLOCKED, &l) == 0 && l.big == 0 && l.mode == 0 &&
                  unlocked_copies == 2 && bkl_depth == depth);
        CHECK("inherited: copy-in failure restores the whole depth",
              ioc(&render, &user_b, FAKE_IOCTL_UNLOCKED, (void *)16) == -EFAULT &&
                  bkl_depth == depth);
        copy_out_fault = 1;
        CHECK("inherited: copy-out failure restores the whole depth",
              ioc(&render, &user_b, FAKE_IOCTL_UNLOCKED, &l) == -EFAULT && bkl_depth == depth);
        copy_out_fault = 0;
        kheap_host_largest = DRM_IOCTL_STACK;
        CHECK(
            "inherited: allocation failure restores the whole depth",
            ioc(&render, &user_b,
                UAPI_IOC(UAPI_IOC_READ, DRM_IOCTL_BASE, DRM_COMMAND_BASE + 2, DRM_IOCTL_STACK + 1),
                &l) == -ENOMEM &&
                bkl_depth == depth);
        kheap_host_largest = 0;
        copies_unlocked = 0;
        CHECK("inherited: a locked row keeps the caller's depth",
              ioc(&render, &user_b, FAKE_IOCTL_LOCKED, &l) == 0 && l.big == (uint32_t)depth + 1 &&
                  bkl_depth == depth);
        fake_dev.features |= DRIVER_KMS_UNLOCKED;
        CHECK("inherited: KMS keeps only the mode lock",
              ioc(&card, &user_a, FAKE_IOCTL_KMS, &l) == 0 && l.big == 0 && l.mode == 1 &&
                  bkl_depth == depth && mode_lock_depth == 0);
        CHECK("inherited: permission failure restores the whole depth",
              ioc(&other, &user_b, FAKE_IOCTL_KMS, &l) == -EACCES && bkl_depth == depth &&
                  mode_lock_depth == 0);
        fake_dev.features &= ~DRIVER_KMS_UNLOCKED;
        CHECK("inherited: KMS without opt-in keeps the big lock",
              ioc(&card, &user_a, FAKE_IOCTL_KMS, &l) == 0 && l.big == (uint32_t)depth + 1 &&
                  l.mode == 1 && bkl_depth == depth);
        bkl_depth = 0;
    }
    close_node(&other);
    close_node(&render);
    close_node(&card);
}

/** The master role between two openers, root, a passed descriptor and a
 * close, with magic and authentication. */
static void test_master(void)
{
    fd_entry_t a, b, b2, plain;
    struct drm_file *fa, *fb;
    struct drm_auth auth, auth2;
    struct fake_echo echo;

    master_changes = 0;
    open_node("dri/card0", &user_a, 0, &a);
    fa = (struct drm_file *)a.priv;
    CHECK("the first opener is master",
          fake_dev.master == fa && fa->authenticated && fa->is_master && fa->was_master);
    CHECK("the listener saw it", master_changes == 1 && master_seen == fa);
    open_node("dri/card0", &user_b, 0, &b);
    fb = (struct drm_file *)b.priv;
    CHECK("the second opener is not", fake_dev.master == fa && !fb->authenticated &&
                                          !fb->is_master && fb->master_id == fa->master_id);
    open_node("dri/card1", &user_b, 0, &plain);
    CHECK("each device has its own master", plain_dev.master == plain.priv);
    close_node(&plain);

    CHECK("GET_MAGIC", ioc(&b, &user_b, DRM_IOCTL_GET_MAGIC, &auth) == 0 && auth.magic == 1);
    CHECK("GET_MAGIC again",
          ioc(&b, &user_b, DRM_IOCTL_GET_MAGIC, &auth2) == 0 && auth2.magic == 1);
    CHECK("the master's magic is the next",
          ioc(&a, &user_a, DRM_IOCTL_GET_MAGIC, &auth2) == 0 && auth2.magic == 2);
    auth2.magic = 9;
    CHECK("AUTH_MAGIC of no magic", ioc(&a, &user_a, DRM_IOCTL_AUTH_MAGIC, &auth2) == -EINVAL);
    CHECK("AUTH_MAGIC", ioc(&a, &user_a, DRM_IOCTL_AUTH_MAGIC, &auth) == 0 && fb->authenticated);
    CHECK("a magic is spent", ioc(&a, &user_a, DRM_IOCTL_AUTH_MAGIC, &auth) == -EINVAL);
    echo.value = 0;
    CHECK("the authenticated client passes DRM_AUTH",
          ioc(&b, &user_b, FAKE_IOCTL_ECHO, &echo) == 0 && echo.value == 1);

    CHECK("SET_MASTER by the master", ioc(&a, &user_a, DRM_IOCTL_SET_MASTER, NULL) == 0);
    CHECK("SET_MASTER by a client never master",
          ioc(&b, &user_b, DRM_IOCTL_SET_MASTER, NULL) == -EACCES);
    CHECK("DROP_MASTER by a client never master",
          ioc(&b, &user_b, DRM_IOCTL_DROP_MASTER, NULL) == -EACCES);
    CHECK("DROP_MASTER by root of a client",
          ioc(&b, &root_proc, DRM_IOCTL_DROP_MASTER, NULL) == -EINVAL);
    master_changes = 0;
    CHECK("DROP_MASTER", ioc(&a, &user_a, DRM_IOCTL_DROP_MASTER, NULL) == 0 && !fake_dev.master);
    CHECK("the listener saw the drop", master_changes == 1 && master_seen == NULL);
    CHECK("DROP_MASTER again", ioc(&a, &user_a, DRM_IOCTL_DROP_MASTER, NULL) == -EINVAL);
    CHECK("AUTH_MAGIC needs the master", ioc(&a, &user_a, DRM_IOCTL_AUTH_MAGIC, &auth) == -EACCES);

    /* A descriptor passed to root, as a seat manager holds it. */
    dup_node(&b, &b2);
    CHECK("SET_MASTER by root through a passed descriptor",
          ioc(&b2, &root_proc, DRM_IOCTL_SET_MASTER, NULL) == 0 && fake_dev.master == fb);
    CHECK("a new master record",
          fb->is_master && fb->master_id != fa->master_id && master_seen == fb);
    CHECK("SET_MASTER while another is master",
          ioc(&a, &user_a, DRM_IOCTL_SET_MASTER, NULL) == -EBUSY);
    close_node(&b2);
    CHECK("one descriptor's close keeps the file", fake_dev.master == fb);
    master_changes = 0;
    close_node(&b);
    CHECK("the master's close drops the role",
          !fake_dev.master && master_changes == 1 && master_seen == NULL);
    CHECK("a former master's process takes it back",
          ioc(&a, &user_a, DRM_IOCTL_SET_MASTER, NULL) == 0 && fake_dev.master == fa);
    ioc(&a, &user_a, DRM_IOCTL_DROP_MASTER, NULL);
    CHECK("a former master's descriptor in another process is refused",
          ioc(&a, &user_b, DRM_IOCTL_SET_MASTER, NULL) == -EACCES);
    close_node(&a);
}

/* The descriptors and the first mapping the dumb buffer rows share. */
static fd_entry_t da, db, dr;
static vma_t dvma;
static uint64_t doff;

/** The region record of the dumb rows' mapping. */
#define DREG ((struct region *)dvma.file)

/** Dumb buffers created by an authenticated client, and refused. */
static void test_dumb_create(void)
{
    struct drm_mode_create_dumb cd;
    struct drm_gem_object *obj;
    struct drm_auth auth;

    open_node("dri/card0", &user_a, 0, &da);
    open_node("dri/card0", &user_b, 0, &db);
    open_node("dri/renderD128", &user_b, 0, &dr);
    ioc(&db, &user_b, DRM_IOCTL_GET_MAGIC, &auth);
    ioc(&da, &user_a, DRM_IOCTL_AUTH_MAGIC, &auth);

    k_memset(&cd, 0, sizeof(cd));
    cd.width = 0;
    cd.height = 32;
    cd.bpp = 32;
    cd.handle = 7;
    cd.pitch = 7;
    CHECK("CREATE_DUMB of no width",
          ioc(&da, &user_a, DRM_IOCTL_MODE_CREATE_DUMB, &cd) == -EINVAL && cd.handle == 0 &&
              cd.pitch == 0 && cd.size == 0);
    cd.width = 0x10000;
    cd.height = 0x10000;
    CHECK("CREATE_DUMB past 32 bits",
          ioc(&da, &user_a, DRM_IOCTL_MODE_CREATE_DUMB, &cd) == -EINVAL);
    cd.width = 64;
    cd.height = 32;
    CHECK("CREATE_DUMB", ioc(&da, &user_a, DRM_IOCTL_MODE_CREATE_DUMB, &cd) == 0 &&
                             cd.handle == 1 && cd.pitch == 256 && cd.size == 8192);
    CHECK("CREATE_DUMB refused on the render node",
          ioc(&dr, &user_b, DRM_IOCTL_MODE_CREATE_DUMB, &cd) == -EACCES);
    obj = drm_gem_object_lookup((struct drm_file *)da.priv, 1);
    CHECK("the object, held by its handle", obj && obj->refs == 2 && obj->handle_count == 1);
    drm_gem_object_put(obj);
}

/** MAP_DUMB and mmap: refusals, write-combining, faults and writes. */
static void test_dumb_map(void)
{
    struct drm_mode_map_dumb md;
    struct drm_gem_object *obj;
    uintptr_t phys0, phys1;
    vma_t inner;

    md.handle = 1;
    md.pad = 0;
    CHECK("MAP_DUMB", ioc(&da, &user_a, DRM_IOCTL_MODE_MAP_DUMB, &md) == 0 &&
                          md.offset == (uint64_t)DRM_FILE_PAGE_OFFSET_START << PAGE_SHIFT);
    doff = md.offset;
    md.handle = 5;
    CHECK("MAP_DUMB of no handle", ioc(&da, &user_a, DRM_IOCTL_MODE_MAP_DUMB, &md) == -ENOENT);

    k_memset(&dvma, 0, sizeof(dvma));
    dvma.end = 3 * PAGE_SIZE;
    dvma.type = VM_SHARED;
    dvma.prot = VM_PROT_READ | VM_PROT_WRITE;
    dvma.pgoff = (uint32_t)(doff >> PAGE_SHIFT);
    CHECK("mmap past the object", da.ops->mmap(&da, &dvma) == -EINVAL);
    dvma.end = 2 * PAGE_SIZE;
    dvma.pgoff = (uint32_t)(doff >> PAGE_SHIFT) + 1;
    CHECK("mmap from inside the object past its end", da.ops->mmap(&da, &dvma) == -EINVAL);
    dvma.pgoff = (uint32_t)(doff >> PAGE_SHIFT);
    CHECK("mmap by a file without a handle", db.ops->mmap(&db, &dvma) == -EACCES);
    dvma.type = VM_FILE;
    CHECK("a private mapping",
          da.ops->mmap(&da, &dvma) == -EINVAL && dvma.pgoff == (uint32_t)(doff >> PAGE_SHIFT));
    dvma.type = VM_SHARED;
    CHECK("mmap", da.ops->mmap(&da, &dvma) == 0 && dvma.pgoff == 0 && dvma.file);
    CHECK("write-combining", DREG->cache == VM_PROT_WC && (dvma.prot & (VM_PROT_IO | VM_PROT_WC)) ==
                                                              (VM_PROT_IO | VM_PROT_WC));
    obj = drm_gem_object_lookup((struct drm_file *)da.priv, 1);
    CHECK("the mapping holds the object", obj->refs == 3);
    drm_gem_object_put(obj);
    phys0 = phys1 = 0;
    CHECK("the first page faults in", DREG->ops->page(DREG->obj, 0, &phys0) == 0 && phys0);
    CHECK("the second page faults in",
          DREG->ops->page(DREG->obj, 1, &phys1) == 0 && phys1 && phys1 != phys0);
    CHECK("no page past the end", DREG->ops->page(DREG->obj, 2, &phys1) == -1);
    CHECK("a new page reads zeros", ((uint8_t *)phys0)[100] == 0);
    memcpy((void *)(phys0 + 256), "pixels", 6);
    obj = drm_gem_object_lookup((struct drm_file *)da.priv, 1);
    CHECK("the driver reads what the mapping wrote",
          memcmp((void *)(drm_gem_shmem_page((struct drm_gem_shmem_object *)obj, 0) + 256),
                 "pixels", 6) == 0);

    /* Linux's drm_vma_offset_lookup_locked: a page inside the object. */
    k_memset(&inner, 0, sizeof(inner));
    inner.end = PAGE_SIZE;
    inner.type = VM_SHARED;
    inner.prot = VM_PROT_READ;
    inner.pgoff = (uint32_t)(doff >> PAGE_SHIFT) + 1;
    phys0 = 0;
    CHECK("mmap from inside the object",
          da.ops->mmap(&da, &inner) == 0 && inner.pgoff == 1 &&
              ((struct region *)inner.file)
                      ->ops->page(((struct region *)inner.file)->obj, inner.pgoff, &phys0) == 0 &&
              phys0 == drm_gem_shmem_page((struct drm_gem_shmem_object *)obj, 1));
    region_unmap(&inner);
    drm_gem_object_put(obj);
}

/** A second buffer, flink names, GEM_OPEN, GEM_CLOSE and the lowest free
 * handle and offset. */
static void test_dumb_names(void)
{
    struct drm_mode_create_dumb cd;
    struct drm_mode_map_dumb md;
    struct drm_mode_destroy_dumb dd;
    struct drm_gem_close gc;
    struct drm_gem_flink fl;
    struct drm_gem_open go;

    k_memset(&cd, 0, sizeof(cd));
    cd.bpp = 32;
    cd.width = 16;
    cd.height = 16;
    CHECK("a second buffer", ioc(&da, &user_a, DRM_IOCTL_MODE_CREATE_DUMB, &cd) == 0 &&
                                 cd.handle == 2 && cd.size == 4096);
    md.handle = 2;
    CHECK("its offset follows the first",
          ioc(&da, &user_a, DRM_IOCTL_MODE_MAP_DUMB, &md) == 0 && md.offset == doff + 8192);

    fl.handle = 2;
    CHECK("FLINK", ioc(&da, &user_a, DRM_IOCTL_GEM_FLINK, &fl) == 0 && fl.name == 1);
    CHECK("FLINK refused on the render node",
          ioc(&dr, &user_b, DRM_IOCTL_GEM_FLINK, &fl) == -EACCES);
    fl.handle = 1;
    CHECK("FLINK of another", ioc(&da, &user_a, DRM_IOCTL_GEM_FLINK, &fl) == 0 && fl.name == 2);
    fl.handle = 9;
    CHECK("FLINK of no handle", ioc(&da, &user_a, DRM_IOCTL_GEM_FLINK, &fl) == -ENOENT);
    go.name = 1;
    CHECK("GEM_OPEN by another client",
          ioc(&db, &user_b, DRM_IOCTL_GEM_OPEN, &go) == 0 && go.handle == 1 && go.size == 4096);
    md.handle = 1;
    CHECK("the other client maps it through its own handle",
          ioc(&db, &user_b, DRM_IOCTL_MODE_MAP_DUMB, &md) == 0 && md.offset == doff + 8192);
    gc.handle = 1;
    gc.pad = 1;
    CHECK("GEM_CLOSE with a pad, which Linux does not read",
          ioc(&db, &user_b, DRM_IOCTL_GEM_CLOSE, &gc) == 0);
    gc.pad = 0;
    CHECK("GEM_CLOSE again", ioc(&db, &user_b, DRM_IOCTL_GEM_CLOSE, &gc) == -EINVAL);
    dd.handle = 2;
    CHECK("DESTROY_DUMB", ioc(&da, &user_a, DRM_IOCTL_MODE_DESTROY_DUMB, &dd) == 0);
    go.name = 1;
    CHECK("the name goes with the last handle",
          ioc(&db, &user_b, DRM_IOCTL_GEM_OPEN, &go) == -ENOENT);

    cd.width = 16;
    CHECK("the lowest free handle",
          ioc(&da, &user_a, DRM_IOCTL_MODE_CREATE_DUMB, &cd) == 0 && cd.handle == 2);
    md.handle = 2;
    CHECK("the freed offset is given again",
          ioc(&da, &user_a, DRM_IOCTL_MODE_MAP_DUMB, &md) == 0 && md.offset == doff + 8192);
}

/** The mapped buffer outlives its handle; the files' closes free the rest. */
static void test_dumb_free(void)
{
    struct drm_mode_destroy_dumb dd;
    int live;

    live = frames_live;
    dd.handle = 1;
    CHECK("DESTROY_DUMB of the mapped buffer",
          ioc(&da, &user_a, DRM_IOCTL_MODE_DESTROY_DUMB, &dd) == 0);
    CHECK("the mapping keeps its frames", frames_live == live);
    region_unmap(&dvma);
    CHECK("the unmap frees them", frames_live == live - 2);

    close_node(&dr);
    close_node(&db);
    close_node(&da);
    CHECK("every object freed with its file",
          fake_dev.objects == NULL && fake_dev.mmap_objects == NULL && frames_live == 0);
    CHECK("each object held the device once", fake_dev.refs == 1);
}

/** Whether every lock of the core is free, as it is between calls: the
 * fake device's, and `f`'s tables' when `f` is given. The host's spinlock
 * traps on a lock taken twice, so a row that ends here also shows that no
 * call nested one. */
static int locks_free(const struct drm_file *f)
{
    return !fake_dev.object_lock.locked && !fake_dev.event_lock.locked &&
           (!f || (!f->objects.lock.locked && !f->syncobjs.lock.locked));
}

/** A handle table past its first 64 slots: the lowest free id, two growths
 * with the lock let go, finds, removes and a walk, each call leaving the
 * table's lock free. */
static void test_handle_table(void)
{
    static struct drm_handles t;
    static int objs[130];
    uint32_t i, id, n;
    int ok = 1;

    for (i = 1; i <= 129; i++) {
        ok &= drm_handle_insert(&t, &objs[i], &id) == 0 && id == i && !t.lock.locked;
    }
    CHECK("129 handles in order, past two growths", ok && t.cap == 256);
    CHECK("a find", drm_handle_find(&t, 100) == &objs[100] && !drm_handle_find(&t, 130) &&
                        !drm_handle_find(&t, 0) && !t.lock.locked);
    CHECK("a remove", drm_handle_remove(&t, 64) == &objs[64] && !drm_handle_find(&t, 64) &&
                          !drm_handle_remove(&t, 64) && !t.lock.locked);
    CHECK("the id removed is the lowest free", drm_handle_insert(&t, &objs[0], &id) == 0 &&
                                                   id == 64 && drm_handle_find(&t, 64) == &objs[0]);
    for (n = 0, i = drm_handle_next(&t, 1); i; i = drm_handle_next(&t, i + 1)) {
        n++;
    }
    CHECK("the walk sees every handle", n == 129 && !t.lock.locked);
    for (i = drm_handle_next(&t, 1); i; i = drm_handle_next(&t, i + 1)) {
        drm_handle_remove(&t, i);
    }
    CHECK("each removed", drm_handle_next(&t, 1) == 0 && !t.lock.locked);
    drm_handles_fini(&t);
}

static int32_t hook_rc;
static int hook_runs, hook_ok, hook_frees;

/** An object's `open` hook, which sees the handle being made reserved: no
 * lookup, find or close reaches it, the object already counts it, and no
 * lock is held while the hook runs, since a driver's hook may sleep. */
static int32_t hook_open(struct drm_gem_object *obj, struct drm_file *file)
{
    hook_runs++;
    hook_ok = !drm_handle_find(&file->objects, 1) && !drm_gem_object_lookup(file, 1) &&
              drm_gem_handle_delete(file, 1) == -EINVAL && obj->handle_count == 1 &&
              locks_free(file);
    return hook_rc;
}

/** The free of the hook's object. */
static void hook_free(struct drm_gem_object *obj)
{
    hook_frees++;
    kfree(obj);
}

static const struct drm_gem_object_funcs hook_funcs = {hook_free, NULL, NULL, NULL,
                                                       NULL,      NULL, NULL, hook_open};

/** A handle's id is reserved while the object's `open` hook runs and
 * published after it, as Linux's drm_gem_handle_create_tail; a failed hook
 * leaves no handle, count or reference. */
static void test_handle_open(void)
{
    fd_entry_t e;
    struct drm_file *f;
    struct drm_gem_object *obj = (struct drm_gem_object *)kzalloc(sizeof(*obj));
    uint32_t h = 0;

    open_node("dri/card0", &user_a, 0, &e);
    f = (struct drm_file *)e.priv;
    drm_gem_object_init(&fake_dev, obj, PAGE_SIZE, &hook_funcs);
    hook_rc = -EIO;
    CHECK("a failed open hook leaves no handle",
          drm_gem_handle_create(f, obj, &h) == -EIO && hook_runs == 1 && hook_ok &&
              !drm_handle_next(&f->objects, 1) && obj->handle_count == 0 && !obj->handles &&
              obj->refs == 1 && locks_free(f));
    hook_rc = 0;
    CHECK("the handle is published after the hook",
          drm_gem_handle_create(f, obj, &h) == 0 && h == 1 && hook_runs == 2 && hook_ok &&
              drm_handle_find(&f->objects, 1) == obj && obj->refs == 2 && locks_free(f));
    drm_gem_object_put(obj);
    close_node(&e);
    CHECK("the close frees it", hook_frees == 1 && !fake_dev.objects && locks_free(NULL));
}

/** References: the walk holds each object it visits and drops it at the
 * next; an object whose last put has counted down but not yet taken it
 * off the device's list is passed over by the walk and refused by mmap, as
 * Linux's kref_get_unless_zero in drm_gem_mmap. */
static void test_object_refs(void)
{
    fd_entry_t e;
    struct drm_file *f;
    struct drm_mode_create_dumb cd;
    struct drm_mode_map_dumb md;
    struct drm_gem_object *o1, *o2, *o, *last = NULL;
    vma_t vma;
    int n, held = 1;

    open_node("dri/card0", &user_a, 0, &e);
    f = (struct drm_file *)e.priv;
    k_memset(&cd, 0, sizeof(cd));
    cd.bpp = 32;
    cd.width = 16;
    cd.height = 16;
    ioc(&e, &user_a, DRM_IOCTL_MODE_CREATE_DUMB, &cd);
    ioc(&e, &user_a, DRM_IOCTL_MODE_CREATE_DUMB, &cd);
    md.handle = 2;
    md.pad = 0;
    CHECK("two buffers, the second mapped",
          cd.handle == 2 && ioc(&e, &user_a, DRM_IOCTL_MODE_MAP_DUMB, &md) == 0);
    o1 = drm_gem_object_lookup(f, 1);
    o2 = drm_gem_object_lookup(f, 2);
    for (n = 0, o = drm_gem_object_next(&fake_dev, NULL); o;
         o = drm_gem_object_next(&fake_dev, o)) {
        n++;
        held &= o->refs == 3 && locks_free(f);
    }
    CHECK("the walk holds what it visits, and only that",
          n == 2 && held && o1->refs == 2 && o2->refs == 2 && locks_free(f));

    o2->refs = 0;
    for (n = 0, o = drm_gem_object_next(&fake_dev, NULL); o;
         o = drm_gem_object_next(&fake_dev, o)) {
        n++;
        last = o;
    }
    CHECK("an object at 0 is passed over", n == 1 && last == o1 && o1->refs == 2);
    k_memset(&vma, 0, sizeof(vma));
    vma.end = PAGE_SIZE;
    vma.type = VM_SHARED;
    vma.prot = VM_PROT_READ | VM_PROT_WRITE;
    vma.pgoff = (uint32_t)(md.offset >> PAGE_SHIFT);
    CHECK("mmap refuses it", e.ops->mmap(&e, &vma) == -EINVAL && !vma.file && locks_free(f));
    o2->refs = 2;
    CHECK("and maps it held", e.ops->mmap(&e, &vma) == 0 && o2->refs == 3 && locks_free(f));
    region_unmap(&vma);
    CHECK("the unmap drops the region's", o2->refs == 2);
    drm_gem_object_put(o1);
    drm_gem_object_put(o2);
    close_node(&e);
    CHECK("the close frees both", !fake_dev.objects && !fake_dev.mmap_objects && locks_free(NULL));
}

/** Events: queued, polled, read whole, and left by a released file. */
static void test_events(void)
{
    fd_entry_t a;
    struct drm_file *f;
    struct drm_pending_event *e1, *e2, *e3;
    struct wait_queue *wq;
    char buf[64];

    open_node("dri/card0", &user_a, 0, &a);
    f = (struct drm_file *)a.priv;
    CHECK("no event to poll", a.ops->poll(&a, POLLIN, &wq) == 0 && wq == &f->event_wait);
    CHECK("no event to read", a.ops->read(&a, buf, sizeof(buf)) == -EAGAIN);

    e1 = (struct drm_pending_event *)kzalloc(sizeof(*e1));
    e1->length = sizeof(struct drm_event_vblank);
    e1->event.base.type = DRM_EVENT_FLIP_COMPLETE;
    e1->event.base.length = e1->length;
    e1->event.vbl.sequence = 7;
    CHECK("reserve", drm_event_reserve(f, e1) == 0 && f->event_space == 4096 - 32 &&
                         e1->dev == &fake_dev && locks_free(f));
    e2 = (struct drm_pending_event *)kzalloc(sizeof(*e2));
    e2->length = 5000;
    CHECK("reserve past the space", drm_event_reserve(f, e2) == -ENOMEM);
    kfree(e2);
    wakes = 0;
    drm_send_event(e1);
    CHECK("the send wakes the reader", wakes == 1);
    CHECK("poll sees the event", a.ops->poll(&a, POLLIN, &wq) == POLLIN);
    CHECK("a buffer too short reads nothing", a.ops->read(&a, buf, 16) == 0);
    CHECK("read", a.ops->read(&a, buf, sizeof(buf)) == 32 &&
                      ((struct drm_event_vblank *)buf)->sequence == 7 && f->event_space == 4096);
    e2 = (struct drm_pending_event *)kzalloc(sizeof(*e2));
    e2->length = sizeof(struct drm_event_vblank);
    drm_event_reserve(f, e2);
    drm_event_cancel_free(e2);
    CHECK("a cancel gives the space back", f->event_space == 4096 && !f->pending);

    e3 = (struct drm_pending_event *)kzalloc(sizeof(*e3));
    e3->length = sizeof(struct drm_event_vblank);
    drm_event_reserve(f, e3);
    e2 = (struct drm_pending_event *)kzalloc(sizeof(*e2));
    e2->length = sizeof(struct drm_event_vblank);
    drm_event_reserve(f, e2);
    drm_send_event(e2);
    close_node(&a);
    CHECK("the release leaves a reserved event unattached, on its device's lock",
          e3->file == NULL && e3->dev == &fake_dev && locks_free(NULL));
    drm_send_event(e3); /* frees it; the sanitizer finds a leak or a fault */
}

static fd_entry_t *race_entry;
static int32_t race_got;
static char race_buf[64];

/** The second reader of test_read_race, on the same file. */
static void race_read(void)
{
    race_got = race_entry->ops->read(race_entry, race_buf, sizeof(race_buf));
}

/** A queued event with its space reserved and the sequence `seq`. */
static struct drm_pending_event *flip_event(struct drm_file *f, uint32_t seq)
{
    struct drm_pending_event *e = (struct drm_pending_event *)kzalloc(sizeof(*e));

    e->length = sizeof(struct drm_event_vblank);
    e->event.base.type = DRM_EVENT_FLIP_COMPLETE;
    e->event.base.length = e->length;
    e->event.vbl.sequence = seq;
    drm_event_reserve(f, e);
    drm_send_event(e);
    return e;
}

/** Two readers of one file: each event goes to one of them whole, and the
 * one a reader copies is off the queue, so the other cannot free it during
 * the copy; a faulting copy puts the event back with its space. */
static void test_read_race(void)
{
    fd_entry_t a;
    struct drm_file *f;
    char buf[64];

    open_node("dri/card0", &user_a, 0, &a);
    f = (struct drm_file *)a.priv;
    flip_event(f, 1);
    flip_event(f, 2);
    race_entry = &a;
    race_got = 0;
    copy_hook = race_read;
    CHECK("the first reader keeps the event it took",
          a.ops->read(&a, buf, 32) == 32 && ((struct drm_event_vblank *)buf)->sequence == 1);
    CHECK("the second reader gets the next",
          race_got == 32 && ((struct drm_event_vblank *)race_buf)->sequence == 2);
    CHECK("both spaces are back", f->event_space == 4096 && !f->events && !f->events_tail);
    flip_event(f, 3);
    CHECK("a bad buffer faults", a.ops->read(&a, (char *)8, 32) == -EFAULT);
    CHECK("and the event stays queued with its space",
          f->events && f->events == f->events_tail && f->event_space == 4096 - 32);
    CHECK("for the next read", a.ops->read(&a, buf, sizeof(buf)) == 32 &&
                                   ((struct drm_event_vblank *)buf)->sequence == 3);
    close_node(&a);
}

/** Unplug: nodes gone, ioctls and reads ENODEV, poll hung up, mappings
 * zapped and refused, the record kept until its last file closes. */
static void test_unplug(void)
{
    fd_entry_t a, r;
    struct drm_mode_create_dumb cd;
    struct drm_mode_map_dumb md;
    struct drm_get_cap cap;
    struct wait_queue *wq;
    struct region *reg;
    vma_t vma;
    uintptr_t phys;
    char buf[32];
    struct drm_gem_object *shared;
    struct drm_gem_shmem_object *imported;
    struct dma_buf export;

    open_node("dri/card0", &user_a, 0, &a);
    open_node("dri/renderD128", &user_a, 0, &r);
    k_memset(&cd, 0, sizeof(cd));
    cd.width = 32;
    cd.height = 32;
    cd.bpp = 32;
    ioc(&a, &user_a, DRM_IOCTL_MODE_CREATE_DUMB, &cd);
    md.handle = cd.handle;
    ioc(&a, &user_a, DRM_IOCTL_MODE_MAP_DUMB, &md);
    k_memset(&vma, 0, sizeof(vma));
    vma.end = PAGE_SIZE;
    vma.type = VM_SHARED;
    vma.pgoff = (uint32_t)(md.offset >> PAGE_SHIFT);
    CHECK("map before the unplug", a.ops->mmap(&a, &vma) == 0);
    reg = (struct region *)vma.file;
    shared = reg->obj;
    k_memset(&export, 0, sizeof(export));
    shared->dma_buf = &export;
    imported = drm_gem_shmem_create(&plain_dev, PAGE_SIZE, 0);
    imported->base.import = &export;

    zaps = 0;
    unplugs = 0;
    client_unregisters = 0;
    drm_dev_unplug(&fake_dev);
    CHECK("the nodes leave", !node_named("dri/card0") && !node_named("dri/renderD128"));
    CHECK("the driver and the console are told", unplugs == 1 && client_unregisters == 1);
    CHECK("the source and importing mappings are zapped",
          zaps == 2 && zapped_obj == &imported->base);
    imported->base.import = NULL;
    shared->dma_buf = NULL;
    drm_gem_object_put(&imported->base);
    CHECK("a fault after it raises SIGBUS", reg->ops->page(reg->obj, 0, &phys) == -1);
    CHECK("not found by index", drm_dev_at(0) == NULL);
    cap.capability = DRM_CAP_DUMB_BUFFER;
    CHECK("an ioctl answers ENODEV", ioc(&a, &user_a, DRM_IOCTL_GET_CAP, &cap) == -ENODEV);
    CHECK("poll reports nothing", a.ops->poll(&a, POLLIN, &wq) == 0);
    CHECK("read finds no event", a.ops->read(&a, buf, sizeof(buf)) == -EAGAIN);
    vma.file = NULL;
    vma.pgoff = (uint32_t)(md.offset >> PAGE_SHIFT);
    CHECK("mmap answers ENODEV", a.ops->mmap(&a, &vma) == -ENODEV);
    vma.file = &reg->vf;
    CHECK("the record stays while files are open", releases == 0 && devices[0] == &fake_dev);
    close_node(&r);
    close_node(&a);
    CHECK("the object stays with its mapping", releases == 0);
    region_unmap(&vma);
    CHECK("the last holder releases the record", releases == 1 && devices[0] == NULL);
    CHECK("its frames are freed", frames_live == 0);
    k_memset(&cd, 0, sizeof(cd));
    drm_dev_init(&fake_dev, &fake_driver);
    CHECK("a new device takes the freed index",
          drm_dev_register(&fake_dev) == 0 && fake_dev.index == 0 && node_named("dri/card0"));
    CHECK("its bus name is the driver's", strcmp(fake_dev.unique, "fakedrm") == 0);
}

/** Set the five arguments of a call in `p` as the architecture's entry
 * passes them. */
static void set_args(isr_params_t *p, uint32_t a, uint32_t b, int type, uintptr_t i1, uintptr_t i2)
{
#ifdef RACH2_HOST_X86_64
    p->rdi = a;
    p->rsi = b;
    p->rdx = (uint64_t)type;
    p->r10 = i1;
    p->r8 = i2;
#else
    p->ebx = a;
    p->ecx = b;
    p->edx = (uint32_t)type;
    p->esi = (uint32_t)i1;
    p->edi = (uint32_t)i2;
#endif
}

/** kcmp: a dup, a fork's copy and two opens of a node, and the other
 * kinds. With i686's registers, a slot of KCMP_EPOLL_TFD whose address does
 * not fit in 32 bits cannot be passed, and those rows run only with
 * x86-64's. */
static void test_kcmp(void)
{
    fd_entry_t parent_fds[8], child_fds[8];
    fd_table_t parent_table, child_table;
    struct kcmp_epoll_slot slot;
    isr_params_t p;
    long first, second;
    int i;

    k_memset(parent_fds, 0, sizeof(parent_fds));
    k_memset(child_fds, 0, sizeof(child_fds));
    open_node("dri/card0", &user_a, 0, &parent_fds[3]);
    dup_node(&parent_fds[3], &parent_fds[4]);
    open_node("dri/card0", &user_a, 0, &parent_fds[5]);
    dup_node(&parent_fds[3], &child_fds[3]); /* the fork's copy */
    parent_fds[7].type = FD_TYPE_ANON;       /* an epoll set */
    child_fds[7].type = FD_TYPE_ANON;
    parent_table.chunks[0] = parent_fds;
    parent_table.cap = 8;
    child_table.chunks[0] = child_fds;
    child_table.cap = 8;
    user_a.fds = &parent_table;
    child_proc.fds = &child_table;
    procs[0] = &root_proc;
    procs[1] = &user_a;
    procs[2] = &child_proc;
    procs[3] = &user_b;
    run_as(&user_a);

#define KCMP(a, b, type, i1, i2) \
    (k_memset(&p, 0, sizeof(p)), set_args(&p, a, b, type, i1, i2), sys_kcmp(&p))

    CHECK("a dup is the same file", KCMP(200, 200, KCMP_FILE, 3, 4) == 0);
    CHECK("a fork's copy is the same file", KCMP(200, 201, KCMP_FILE, 3, 3) == 0);
    first = KCMP(200, 200, KCMP_FILE, 3, 5);
    second = KCMP(200, 200, KCMP_FILE, 5, 3);
    CHECK("two opens are two files, in an order",
          (first == 1 && second == 2) || (first == 2 && second == 1));
    CHECK("the order holds", KCMP(200, 201, KCMP_FILE, 5, 3) == second);
    CHECK("a descriptor not open", KCMP(200, 200, KCMP_FILE, 3, 6) == -EBADF);
    CHECK("a descriptor past the table", KCMP(200, 200, KCMP_FILE, 3, 0x80000000u) == -EBADF);
    CHECK("no such process", KCMP(200, 999, KCMP_FILE, 3, 3) == -ESRCH);
    CHECK("a process of root", KCMP(200, 100, KCMP_VM, 0, 0) == -EPERM);
    run_as(&root_proc);
    CHECK("root may read both", KCMP(200, 100, KCMP_VM, 0, 0) > 0);
    run_as(&user_a);
    CHECK("an unknown kind", KCMP(200, 201, 8, 0, 0) == -EINVAL);
    CHECK("the fork's child has its own address space", KCMP(200, 201, KCMP_VM, 0, 0) > 0);
    CHECK("and its own table", KCMP(200, 201, KCMP_FILES, 0, 0) > 0);
    CHECK("one process is the same as itself", KCMP(200, 200, KCMP_FILES, 0, 0) == 0);
    CHECK("no I/O context is the same", KCMP(200, 201, KCMP_IO, 0, 0) == 0);
    user_a.ioprio = 4;
    CHECK("an I/O context against none", KCMP(200, 201, KCMP_IO, 0, 0) > 0);
    user_a.ioprio = 0;
    CHECK("no adjustments are the same", KCMP(200, 201, KCMP_SYSVSEM, 0, 0) == 0);
    undo_owner = &tg_a;
    CHECK("adjustments against none", KCMP(200, 201, KCMP_SYSVSEM, 0, 0) > 0);
    undo_owner = NULL;
    CHECK("dispositions of two processes", KCMP(200, 201, KCMP_SIGHAND, 0, 0) > 0);

    epoll_tfd = 3;
    epoll_file = parent_fds[3].ofd;
#ifndef RACH2_HOST_X86_64
    if ((uint64_t)(uintptr_t)&slot != (uint32_t)(uintptr_t)&slot) {
        goto done;
    }
#endif
    slot.efd = 7;
    slot.tfd = 3;
    slot.toff = 0;
    CHECK("an epoll target is the file", KCMP(200, 201, KCMP_EPOLL_TFD, 4, (uintptr_t)&slot) == 0);
    CHECK("an epoll target against another file",
          KCMP(200, 201, KCMP_EPOLL_TFD, 5, (uintptr_t)&slot) == second);
    slot.toff = 1;
    CHECK("no such registration", KCMP(200, 201, KCMP_EPOLL_TFD, 4, (uintptr_t)&slot) == -ENOENT);
    slot.efd = 3;
    CHECK("a set that is not one", KCMP(200, 201, KCMP_EPOLL_TFD, 4, (uintptr_t)&slot) == -EINVAL);
    slot.efd = 6;
    CHECK("a set not open", KCMP(200, 201, KCMP_EPOLL_TFD, 4, (uintptr_t)&slot) == -EBADF);
done:
    CHECK("a slot that faults", KCMP(200, 201, KCMP_EPOLL_TFD, 4, 16) == -EFAULT);
#undef KCMP

    for (i = 0; i < 7; i++) {
        close_node(&child_fds[i]);
        close_node(&parent_fds[i]);
    }
    user_a.fds = NULL;
    child_proc.fds = NULL;
}

int main(int argc, char **argv)
{
    proc_fill(&root_proc, &tg_root, &mm_root, 100, 0);
    proc_fill(&user_a, &tg_a, &mm_a, 200, 1000);
    proc_fill(&child_proc, &tg_child, &mm_b, 201, 1000);
    proc_fill(&user_b, &tg_b, &mm_b, 300, 1000);
    user_a.sigacts = tg_a.action;
    child_proc.sigacts = tg_child.action;
    test_register();
    test_inherited();
    if (argc == 2 && strcmp(argv[1], "inherited") == 0) {
        if (fails) {
            printf("drm-core-test: inherited: %d failed\n", fails);
            return 1;
        }
        printf("drm-core-test: inherited: ok\n");
        return 0;
    }
    test_identity();
    test_caps();
    test_dispatch();
    test_locks();
    test_master();
    test_dumb_create();
    test_dumb_map();
    test_dumb_names();
    test_dumb_free();
    test_handle_table();
    test_handle_open();
    test_object_refs();
    test_events();
    test_read_race();
    test_unplug();
    test_kcmp();
    CHECK("the end: the big lock and the mode lock let go", bkl_depth == 0 && mode_lock_depth == 0);
    if (fails) {
        printf("drm-core-test: %d failed\n", fails);
        return 1;
    }
    printf("drm-core-test: ok\n");
    return 0;
}
