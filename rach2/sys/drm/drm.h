/*
 * drm.h: the kernel's display and rendering devices, Linux's Direct
 * Rendering Manager: the contracts of devices, drivers, open files, buffer
 * objects, fences, display state, device loss and the firmware
 * framebuffer's handoff, and every ioctl handler the dispatcher names.
 */
#ifndef _DRM_H_
#define _DRM_H_

#include "../arch/arch.h"
#include "../lib/kint.h"
#include "../lib/spinlock.h"
#include "../fs/fd.h"
#include "../mm/shmem.h"
#include "../mm/vm.h"
#include "../pci.h"
#include "../proc/mutex.h"
#include "../proc/wait_queue.h"
#include "uapi.h"

struct sysfs_pci_ident;

/*
 * rach2/sys/drm/Readme.md tells the design; this header is its contract.
 * The rules every file of sys/drm/ and sys/dmabuf/ keeps:
 *
 * Locking. A row runs under the big kernel lock unless it is marked to run
 * without it (DRM_UNLOCKED, below); so does an interrupt handler unless it
 * is registered ISR_UNLOCKED, the i915's and virtio-gpu's are. The big
 * lock goes at every sleep, so a function that may sleep says so: the
 * waits on fences and vblanks, a blocking atomic commit, the copies to and
 * from user memory and a driver's commands to its device. Before sleeping,
 * a caller takes a reference on every object it uses after the sleep; an
 * object it found by ID or handle is found again or held, never assumed.
 *
 * A device's `mode_lock`, the commit lock, a mutex its holder may take again
 * (`drm_modeset_lock`), keeps the mode state: the mode objects and their
 * table, the framebuffers' and blobs' references, the committed states,
 * the connectors' probes, modes and EDIDs, and the leases, as Linux's
 * mode_config.mutex and modeset locks do. Each KMS row holds it
 * (DRM_MODESET_LOCK), as do the commit worker, the console client, the
 * probes of hotplug, sysfs and debugfs, a file's release of its
 * framebuffers and blobs, and the unplug's mark. A commit takes it before
 * it reads the state it replaces and keeps it until the new state is the
 * device's. ATOMIC and PAGE_FLIP hold it without the big lock on a device
 * whose driver declares DRIVER_KMS_UNLOCKED (DRM_KMS), whose display hooks
 * then take only their own locks, and so does the commit worker for such a
 * device's tails (`drm_atomic_commit_work`). WAIT_VBLANK holds none of it: the
 * vblanks are under the two locks below, as Linux's drm_wait_vblank_ioctl
 * takes no modeset lock (drm_vblank.c:1733-1873). User memory is copied
 * under it, so it comes before an address space's `mmap_lock`, as Linux's
 * modeset locks do; nothing takes it under `mmap_lock`.
 *
 * An interrupt handler takes no sleeping lock and may only signal fences,
 * count vblanks and queue events. The order, outermost first, is a
 * device's `mode_lock`, the locks of a driver's buffers (the i915 GEM lock),
 * then the spinlocks below and those a driver keeps for its own hardware.
 *
 * Spinlocks, as Linux's, keep the structures that DRM_UNLOCKED ioctls and
 * other processors reach whole without the big lock. Each is taken with
 * interrupts off, for a region that neither sleeps nor allocates, and no
 * two of the first three nest:
 *   * a handle table's `lock` (`struct drm_handles`) over its slots, its
 *     PRIME hash and each handle's PRIME cache, Linux's `table_lock` and
 *     `syncobj_table_lock` (include/drm/drm_file.h:309-315);
 *   * a device's `object_lock` over its list of buffer objects, their flink
 *     names, each object's handles and handle count, its weak `dma_buf`,
 *     and the device's mmap space, Linux's `object_name_lock` and the
 *     offset manager's `vm_lock`;
 *   * a device's `event_lock` over each file's queued and reserved events
 *     and its event space, Linux's `event_lock` (drm_device.h:276), and
 *     over each CRTC's queued vblank events and pending flip and a
 *     driver's per-pipe events; a fence's lock may be taken under it, and
 *     a device's `vbl_lock` after it, over each CRTC's vblank holds,
 *     counts, times and the driver's vblank hooks, Linux's `vbl_lock`
 *     (drm_device.h:242), then the driver's interrupt locks;
 *   * a CRTC's CRC `lock` (`struct drm_crtc_crc`) over its entries,
 *     alone or after `event_lock`, Linux's `crc->lock`;
 *   * the commit worker's `work_lock` over its queues of commits, device
 *     work and hotplug events, under which no fence is called and no
 *     sleeper woken (drm_atomic.c);
 *   * a syncobj's `lock` over its fence and its lists of waits and
 *     eventfds, Linux's (drm_syncobj.c:266-396), under which a fence's lock
 *     and the chain lock of dmabuf/fence.c may be taken, as Linux's are.
 * A buffer object's `refs` change atomically, as Linux's kref: a walk that
 * meets an object under `object_lock` takes a reference only while the
 * count is not 0 (`drm_gem_object_next`, as Linux's drm_gem.c:1177), since
 * the last put takes the object off the device's lists under that lock
 * before it frees it. A device's, a syncobj's, a fence's and a dma-buf's
 * `refs` change atomically too, and a lookup by handle takes its reference
 * under the table's lock (`drm_handle_find_get`).
 *
 * References. Each object counts its holders in `refs`, and the last put
 * frees it:
 *   * A device is held by its driver while bound, by each open file of
 *     its nodes, and by each of its buffer objects. Unplugging drops the
 *     driver's; the record outlives every open file.
 *   * An open file (`struct drm_file`) belongs to one open file
 *     description, so `dup`, `fork` and descriptor passing share it, and
 *     it is released when the description's last descriptor closes, by an
 *     explicit close, by `exec`'s close-on-exec or by the exit of the last
 *     process that had it. The release drops its master role, its
 *     handles, its syncobj handles, the framebuffers it made (each removed
 *     from the display first, as RMFB does), its pending events and its
 *     leases.
 *   * A buffer object is held by each handle any file has to it, by each
 *     framebuffer that scans it out, by its dma-buf while one is exported,
 *     by each mapping of it into a process, which 76-2a's region holds and
 *     which `fork` copies and `munmap`, `exec` and exit drop, and by its
 *     driver while the device reads or writes it.
 *   * A framebuffer is held by its ID while its maker has not removed it,
 *     and by each plane state that shows it.
 *   * A fence is held by each waiter, each callback's owner, each
 *     reservation slot, sync file and syncobj point that names it, and by
 *     its timeline until it signals.
 *
 * Device loss. A device that goes, a virtio device reset, a GPU that
 * hangs past recovery or a firmware framebuffer whose aperture a native
 * driver takes, is unplugged (`drm_dev_unplug`): its nodes leave /dev at
 * once, every later ioctl and mmap of its open files fails with ENODEV,
 * every mapping of its buffers is revoked so that a later access raises
 * SIGBUS (76-2a), and the driver signals every fence it still owes with
 * the error -ENODEV. read and poll do not change, as on Linux: they give
 * the events already queued, and after them read waits and poll reports
 * nothing, so that a compositor whose loop polls the node does not spin
 * on a device that will never be readable. The records stay until their
 * last holder lets go. virtio-gpu's objects of guest memory are the
 * exception to the SIGBUS: a revoked mapping of one, its own or an
 * importer's through its dma-buf, takes its pages again at the next touch,
 * as it does on Linux, where a program outlives the reset of its GPU
 * (77-5).
 *
 * Discovery. A program finds a device by its identity: the driver's name
 * and the bus that DRM_IOCTL_VERSION, /sys/class/drm and the PCI ids give,
 * never by minor number. Card N is /dev/dri/cardN at major 226, minor N,
 * and its render node /dev/dri/renderD(128 + N), minor 128 + N; N is the
 * order of registration, which differs between boots and machines.
 */

/* ------------------------------------------------------------------------
 * Drivers and their identity
 */

/* The features of a driver, Linux 6.12's DRIVER_* bits. */
#define DRIVER_GEM              (1u << 0)
#define DRIVER_MODESET          (1u << 1)
#define DRIVER_RENDER           (1u << 3)
#define DRIVER_ATOMIC           (1u << 4)
#define DRIVER_SYNCOBJ          (1u << 5)
#define DRIVER_SYNCOBJ_TIMELINE (1u << 6)
#define DRIVER_COMPUTE_ACCEL    (1u << 7)
#define DRIVER_GEM_GPUVA        (1u << 8)
#define DRIVER_CURSOR_HOTSPOT   (1u << 9)
/* Rach2's: the driver's display hooks take only their own locks and the
 * device's `mode_lock`, so the KMS rows marked DRM_UNLOCKED run without the
 * big lock on its device (DRM_KMS). Linux has no big lock to leave. */
#define DRIVER_KMS_UNLOCKED     (1u << 31)

/* The bus of a device, by libdrm's DRM_BUS_* numbers. virtio-gpu is on the
 * virtio bus under its PCI function, which libdrm follows to the PCI
 * device, so it is DRM_BUS_PCI here. */
#define DRM_BUS_PCI      0
#define DRM_BUS_PLATFORM 2

/*
 * What a driver says of itself, by which Mesa's loader, GBM, libdrm and
 * IGT choose it: DRM_IOCTL_VERSION's name, description, date and version,
 * the features DRM_IOCTL_GET_CAP and the dispatcher read, and the bus.
 */
struct drm_ident {
    const char *name;
    const char *desc;
    const char *date;
    int major, minor, patchlevel;
    uint32_t features; /* DRIVER_* */
    int bus;           /* DRM_BUS_* */
};

/* The six drivers' identities as Linux 6.12 reports them (i915_driver.c
 * and i915_driver.h, virtgpu_drv.c and virtgpu_drv.h, tiny/bochs.c,
 * tiny/simpledrm.c, vkms/vkms_drv.c and vgem/vgem_drv.c). fbdrm reports
 * itself as simpledrm, the driver Linux gives the firmware's framebuffer. */
#define DRM_IDENT_I915                                                              \
    {"i915",                                                                        \
     "Intel Graphics",                                                              \
     "20230929",                                                                    \
     1,                                                                             \
     6,                                                                             \
     0,                                                                             \
     DRIVER_GEM | DRIVER_RENDER | DRIVER_MODESET | DRIVER_ATOMIC | DRIVER_SYNCOBJ | \
         DRIVER_SYNCOBJ_TIMELINE | DRIVER_KMS_UNLOCKED,                             \
     DRM_BUS_PCI}
#define DRM_IDENT_VIRTIO_GPU                                                        \
    {"virtio_gpu",                                                                  \
     "virtio GPU",                                                                  \
     "0",                                                                           \
     0,                                                                             \
     1,                                                                             \
     0,                                                                             \
     DRIVER_MODESET | DRIVER_GEM | DRIVER_RENDER | DRIVER_ATOMIC | DRIVER_SYNCOBJ | \
         DRIVER_SYNCOBJ_TIMELINE | DRIVER_CURSOR_HOTSPOT,                           \
     DRM_BUS_PCI}
#define DRM_IDENT_BOCHS                                                          \
    {"bochs-drm", "bochs dispi vga interface (qemu stdvga)",   "20130925", 1, 0, \
     0,           DRIVER_GEM | DRIVER_MODESET | DRIVER_ATOMIC, DRM_BUS_PCI}
#define DRM_IDENT_SIMPLEDRM                                                                    \
    {"simpledrm", "DRM driver for simple-framebuffer platform devices", "20200625",      1, 0, \
     0,           DRIVER_ATOMIC | DRIVER_GEM | DRIVER_MODESET,          DRM_BUS_PLATFORM}
#define DRM_IDENT_VKMS                            \
    {"vkms",                                      \
     "Virtual Kernel Mode Setting",               \
     "20180514",                                  \
     1,                                           \
     0,                                           \
     0,                                           \
     DRIVER_MODESET | DRIVER_ATOMIC | DRIVER_GEM, \
     DRM_BUS_PLATFORM}
#define DRM_IDENT_VGEM                                          \
    {"vgem", "Virtual GEM provider",     "20120112",      1, 0, \
     0,      DRIVER_GEM | DRIVER_RENDER, DRM_BUS_PLATFORM}

/* The device numbers of the nodes and the most devices at once. */
#define DRM_MAJOR             226
#define DRM_MINOR_RENDER_BASE 128
#define DRM_MAX_DEVICES       64

struct drm_device;
struct drm_file;
struct drm_gem_object;
struct drm_framebuffer;
struct drm_atomic_state;
struct drm_crtc;
struct drm_plane;
struct drm_connector;
struct drm_mode_config;
struct dma_buf;
struct dma_fence;

/* An ioctl handler: `data` is the kernel's copy of the argument, which the
 * dispatcher copies in before and out after by the direction and size the
 * number gives. Returns 0 or -errno; it may sleep. */
typedef int32_t (*drm_ioctl_t)(struct drm_device *dev, void *data, struct drm_file *file);

/* Who may call an ioctl, Linux's flags: an authenticated client, the
 * master, root, and a render node's client. DRM_UNLOCKED runs the
 * handler without the big lock, under the locks its row names, and the
 * dispatcher takes the big lock around every other handler. Linux 6.12
 * runs every row unlocked (drm_ioctl.c:728-746) and has dropped the flag,
 * which once marked such a row against drm_global_mutex; Rach2 keeps it,
 * at its old value, while its rows leave the big lock one at a time.
 *
 * Rach2's own two: DRM_MODESET_LOCK has the dispatcher hold the device's
 * `mode_lock` across the handler, for a row that reads or changes the mode
 * objects, their table or their committed states, as Linux's handlers take
 * mode_config.mutex and the modeset locks themselves; DRM_KMS marks a row
 * that reaches the driver's display hooks, whose DRM_UNLOCKED holds only on
 * a device with DRIVER_KMS_UNLOCKED. */
#define DRM_AUTH         (1u << 0)
#define DRM_MASTER       (1u << 1)
#define DRM_ROOT_ONLY    (1u << 2)
#define DRM_UNLOCKED     (1u << 4)
#define DRM_RENDER_ALLOW (1u << 5)
#define DRM_MODESET_LOCK (1u << 8)
#define DRM_KMS          (1u << 9)

/* An ioctl of the core's table or a driver's own (numbers DRM_COMMAND_BASE
 * to DRM_COMMAND_END - 1). `cmd` is the full number, whose size is the most
 * the handler reads and writes. */
struct drm_ioctl_desc {
    uint32_t cmd;
    uint32_t flags;
    drm_ioctl_t func;
    const char *name;
};

/*
 * A driver. Every hook may be NULL where the features leave it unused.
 * `open` and `postclose` see each file of the device opened and released;
 * `dumb_create` makes a dumb buffer for MODE_CREATE_DUMB, sets its handle,
 * pitch and size, and may sleep; `gem_prime_import` makes an object of a
 * dma-buf from another device or from udmabuf, or returns NULL with
 * `*err` set. It borrows the buffer; the core attaches its reference and
 * reservation to the returned object. NULL selects the shmem import helper.
 * `unplug` stops the hardware once the core has unplugged the
 * device; `release` frees the device's record once its last reference is
 * gone, NULL for a record that is never freed. The display hooks are on
 * each mode object (below) and the commit hooks on the mode configuration.
 */
struct drm_driver {
    struct drm_ident ident;
    int32_t (*open)(struct drm_device *dev, struct drm_file *file);
    void (*postclose)(struct drm_device *dev, struct drm_file *file);
    int32_t (*dumb_create)(struct drm_file *file, struct drm_device *dev,
                           struct drm_mode_create_dumb *args);
    struct drm_gem_object *(*gem_prime_import)(struct drm_device *dev, struct dma_buf *buf,
                                               int32_t *err);
    void (*unplug)(struct drm_device *dev);
    const struct drm_ioctl_desc *ioctls; /* the driver's own, or NULL */
    int num_ioctls;
    void (*release)(struct drm_device *dev); /* frees the record after the last put */
};

/* ------------------------------------------------------------------------
 * Fences: dma_fence, in sys/dmabuf/fence.c (76-3a)
 */

/*
 * A fence's driver and timeline names, for SYNC_IOC_FILE_INFO and debugfs,
 * and its release, which frees the record when the last reference goes;
 * NULL frees it with kfree. The rest may be NULL, as Linux's:
 * `enable_signaling` is called once, the first time anyone waits or adds a
 * callback, for a fence that signals only on request, with interrupts
 * masked, so it must not sleep; it returns 0 when the fence has signalled
 * meanwhile. `signaled` answers a query without
 * enabling anything, and returns 1 when the fence may be signalled now.
 * `wait` replaces the default sleep of `dma_fence_wait_until`, after
 * signalling is enabled, with the same deadline and results.
 * `use_64bit_seqno` says the producer's seqnos never wrap and order as
 * 64-bit numbers; when it is 0, as for a fence with no ops, they order by
 * their low 32 bits, which wrap, as Linux's member of that name.
 */
struct dma_fence_ops {
    const char *(*get_driver_name)(struct dma_fence *fence);
    const char *(*get_timeline_name)(struct dma_fence *fence);
    int (*enable_signaling)(struct dma_fence *fence);
    void (*release)(struct dma_fence *fence);
    int (*signaled)(struct dma_fence *fence);
    int32_t (*wait)(struct dma_fence *fence, int intr, int64_t deadline_ns);
    int use_64bit_seqno;
};

/* A callback run once when its fence signals, in the signaller's context,
 * which may be an interrupt: it may only signal, wake and queue. */
struct dma_fence_cb {
    struct dma_fence_cb *next;
    void (*func)(struct dma_fence *fence, struct dma_fence_cb *cb);
};

/*
 * A point on a timeline that some work will reach: it signals once, with
 * an error if the work failed, and never goes back. `context` names the
 * timeline (`dma_fence_context_alloc`) and `seqno` orders fences of one
 * timeline. `timestamp_ns` is CLOCK_MONOTONIC at the signal.
 */
struct dma_fence {
    const struct dma_fence_ops *ops;
    uint64_t context;
    uint64_t seqno;
    volatile uint32_t refs; /* changed atomically */
    /* Over `signaled`, `listened`, `cbs`, `running` and `deadline_ns`, as
     * Linux's fence lock; never held while a callback or
     * `enable_signaling` runs. */
    struct spinlock lock;
    volatile int signaled;
    int32_t error; /* 0, or the -errno the work failed with */
    int64_t timestamp_ns;
    int64_t deadline_ns; /* earliest requested completion hint, INT64_MAX if none */
    struct dma_fence_cb *cbs;
    /* The callback a signal runs, taken off `cbs`, and the processor
     * record of the signaller, or NULL between callbacks. */
    struct dma_fence_cb *running;
    const void *running_cpu;
    wait_queue_t wq;
    /* A callback, a sleeper or a poller has asked for the signal, and
     * `enable_signaling` has run, as Linux's
     * DMA_FENCE_FLAG_ENABLE_SIGNAL_BIT is set by
     * __dma_fence_enable_signaling from dma_fence_add_callback,
     * dma_fence_wait_timeout and dma_fence_enable_sw_signaling; it stays
     * set when they leave. A query sets nothing. */
    int listened;
};

/** The first of `num` new timeline contexts. Any context. */
uint64_t dma_fence_context_alloc(uint32_t num);
/** Make `fence` an unsignalled point `seqno` of `context`, with one
 * reference, the caller's. */
void dma_fence_init(struct dma_fence *fence, const struct dma_fence_ops *ops, uint64_t context,
                    uint64_t seqno);
/** Whether seqno `f1` comes after `f2` on a timeline whose producer has
 * `ops`, which may be NULL, as Linux's __dma_fence_is_later: a 64-bit
 * comparison for a producer with `use_64bit_seqno`, else the signed
 * difference of the low 32 bits, so that a 32-bit timeline orders across
 * its wrap. Any context. */
int dma_fence_seqno_is_later(uint64_t f1, uint64_t f2, const struct dma_fence_ops *ops);
/** Whether `f1` comes after `f2`, two fences of one context, by the
 * ordering of `f1`'s producer, as Linux's dma_fence_is_later; 0 for
 * fences of different contexts. Any context. */
int dma_fence_is_later(struct dma_fence *f1, struct dma_fence *f2);
/** Whether `f1` is `f2` or comes after it, as Linux's
 * dma_fence_is_later_or_same: another fence with the same seqno is not.
 * Any context. */
int dma_fence_is_later_or_same(struct dma_fence *f1, struct dma_fence *f2);
/** Take a reference; NULL is allowed and returned. */
struct dma_fence *dma_fence_get(struct dma_fence *fence);
/** Drop a reference, releasing the fence with the last; NULL is allowed. */
void dma_fence_put(struct dma_fence *fence);
/** Record the error a fence will signal with, before it signals. */
void dma_fence_set_error(struct dma_fence *fence, int32_t error);
/** Signal: stamp the time, run the callbacks, wake the waiters. Returns 0,
 * or -EINVAL when it had signalled. Any context, an interrupt among them. */
int32_t dma_fence_signal(struct dma_fence *fence);
/** Whether the fence has signalled, asking its `signaled` if need be and
 * signalling it when that says so; enables nothing, as Linux's. */
int dma_fence_is_signaled(struct dma_fence *fence);
/** Ask for the signal, as Linux's dma_fence_enable_sw_signaling: mark the
 * fence `listened` and run its `enable_signaling` the first time. */
void dma_fence_enable_sw_signaling(struct dma_fence *fence);
/** Run `func` when `fence` signals: 0, with the fence marked `listened`,
 * or -ENOENT, with nothing added, when it has signalled already. */
int32_t dma_fence_add_callback(struct dma_fence *fence, struct dma_fence_cb *cb,
                               void (*func)(struct dma_fence *, struct dma_fence_cb *));
/** Take a callback back: 1 when it was pending, 0 when it ran. The fence
 * stays `listened`. */
int dma_fence_remove_callback(struct dma_fence *fence, struct dma_fence_cb *cb);
/**
 * Wait for the fence until the CLOCK_MONOTONIC time `deadline_ns`, or for
 * ever when it is negative; sleeps. Returns 0 once signalled (the fence's
 * error is read from it), -ETIME at the deadline, or -EINTR for a
 * signal when `intr` is set. A fence not signalled before the deadline is
 * enabled (`dma_fence_enable_sw_signaling`), as Linux's
 * dma_fence_wait_timeout enables it, and then waited for by its `wait`, or
 * else by `dma_fence_default_wait`.
 */
int32_t dma_fence_wait_until(struct dma_fence *fence, int intr, int64_t deadline_ns);
/** The sleep of `dma_fence_wait_until` without the fence's `wait`, Linux's
 * dma_fence_default_wait: enables signalling and sleeps on the fence's
 * queue; for a driver's `wait` that sleeps on its own fence. */
int32_t dma_fence_default_wait(struct dma_fence *fence, int intr, int64_t deadline_ns);

/*
 * The implicit fences of a buffer, Linux's dma_resv: outstanding writes
 * and reads. A new write joins the write fence; a new read is added.
 * Each slot holds a reference. Kept by a buffer object, or by its dma-buf
 * when it is imported, so that every device and the CPU see one set.
 */
struct dma_resv {
    /* Over the rest, as Linux's reservation's ww_mutex: a sleeping lock,
     * taken after the driver's object locks (dmabuf/Readme.md). */
    struct mutex lock;
    struct dma_fence *write; /* all pending writers, or NULL */
    struct dma_fence **reads;
    uint32_t nreads, cap;
};

/* ------------------------------------------------------------------------
 * Sharing: dma-buf, sync files and udmabuf, in sys/dmabuf/ (76-3a)
 */

/** Whether the descriptor `e` is a dma-buf. Any context. */
int is_dma_buf_file(const fd_entry_t *e);
/** The fence a sync file descriptor `fd` of the caller holds, with a
 * reference taken, or NULL when `fd` is no sync file. */
struct dma_fence *sync_file_get_fence(int32_t fd);
/** Register /dev/udmabuf through the devfs registry; 0 or -errno. Called
 * once at boot, from `drm_start`. */
int32_t udmabuf_start(void);
/** Give the descriptor `e`, just opened on debugfs's sync/sw_sync, a new
 * software timeline: 0 or -errno. */
int32_t sw_sync_open(fd_entry_t *e);
/** Write debugfs's sync/info into `cap` bytes at `buf`, NUL-terminated and
 * cut to fit, as Linux 6.12 writes it: each live software timeline by its
 * opener's command and value, with its points not yet reached, then the
 * `fences:` header, with nothing under it, as on Linux. Returns the length
 * of the whole text; a NULL `buf` measures it. Under the big kernel lock. */
uint32_t sw_sync_info(char *buf, uint32_t cap);

/* ------------------------------------------------------------------------
 * Buffer objects: GEM, in drm_gem.c (76-4a)
 */

/*
 * What a driver does for its buffer objects: `free` releases the memory
 * when the last reference goes; `mmap` sets up the region `vma` for a
 * mapping of the object's pages from its page `vma->pgoff`, as
 * file_ops.mmap does for a file (rach2/sys/fs/file.h), and on success the
 * region takes over the caller's reference to the object: a device region
 * (`vm_device_map`) whose object is the `struct drm_gem_object` itself, so
 * that an unplug's `paging_zap_device` finds its pages. `pin` and `unpin`
 * keep the pages where the device reads them for scanout. NULL `mmap`
 * refuses mapping with ENODEV; NULL `free` frees the record with kfree.
 * `page` returns a borrowed physical frame, or 0 on failure, for PRIME
 * imports by other devices; NULL refuses those imports. It decides alone
 * whether it serves once its device is unplugged, but a frame it gave stays
 * the object's while the object's dma-buf exists, unplugged or not, and is
 * never purged meanwhile, since an importer such as i915 keeps it in its
 * own GTT and page tables, and holds the dma-buf while it does, as Linux's
 * page references and attachment pins keep it. `map_wc` records
 * the cache type that the exporter and its importers use. `vmap` returns
 * the kernel's address of the object's bytes, one contiguous range, for
 * the console's buffer (drm_client.c): valid while the object is scanned
 * out, or NULL when the kernel cannot reach it so; NULL `vmap` is the same.
 * `cpu_access` is the exporter's part of a dma-buf's DMA_BUF_IOCTL_SYNC,
 * with its DMA_BUF_SYNC_ flags, before the start waits for the fences, so
 * that it may wait its own way: 0 or -errno, which ends the call without
 * the wait; NULL does nothing. It may sleep. `open` sees each handle a file
 * gets to the object, by creation, GEM_OPEN or PRIME import, as Linux's
 * drm_gem_object_funcs.open: 0, or -errno, which takes the handle back;
 * NULL does nothing. It may sleep.
 */
struct drm_gem_object_funcs {
    void (*free)(struct drm_gem_object *obj);
    int32_t (*mmap)(struct drm_gem_object *obj, vma_t *vma);
    int32_t (*pin)(struct drm_gem_object *obj);
    void (*unpin)(struct drm_gem_object *obj);
    uintptr_t (*page)(struct drm_gem_object *obj, uint32_t pgoff);
    volatile uint8_t *(*vmap)(struct drm_gem_object *obj);
    int32_t (*cpu_access)(struct drm_gem_object *obj, uint64_t flags);
    int32_t (*open)(struct drm_gem_object *obj, struct drm_file *file);
};

/*
 * A buffer object. `size` is a whole number of pages. `name` is its flink
 * name, 0 for none, which the object keeps while any file has a handle to
 * it. `mmap_offset` is where it lies in the mmap space of its device's
 * files, in bytes, a whole number of pages from DRM_FILE_PAGE_OFFSET_START,
 * 0 until MAP_DUMB or a driver asks (`drm_gem_create_mmap_offset`).
 * `dma_buf` is the dma-buf exported of it, kept so that a second export
 * and a self-import find it again; the dma-buf holds a reference on the
 * object, not the other way. `import` is the dma-buf it was made from,
 * held. `resv` is its implicit fences, or its import's. `no_export` is set
 * by a driver whose object PRIME may not export, which then answers
 * EINVAL, as Linux's i915 refuses its userptr objects.
 */
struct drm_gem_object {
    volatile uint32_t refs; /* changed atomically (drm_gem.c) */
    struct drm_device *dev;
    uint64_t size;
    uint32_t name;
    uint64_t mmap_offset;
    struct dma_buf *dma_buf;
    struct dma_buf *import;
    struct dma_resv *resv;
    struct dma_resv resv_own;
    const struct drm_gem_object_funcs *funcs;
    int map_wc;                 /* PRIME mappings use write-combining */
    uint32_t handle_count;      /* handles any file has to it, under `object_lock` */
    struct drm_handle *handles; /* those handles, through `obj_next`, under `object_lock` */
    uint8_t no_export;
    struct drm_gem_object *dev_next;  /* the device's objects, under `object_lock` */
    struct drm_gem_object *mmap_next; /* those with an offset, by offset, under `object_lock` */
};

/* The mmap space of a device's buffer objects, Linux's on each ABI: from
 * 4 GiB for 256 times that on x86-64, and from 16 MiB for 16 times that on
 * i686, in pages. */
#define DRM_FILE_PAGE_OFFSET_START                                        \
    (sizeof(uintptr_t) == 8 ? (uint32_t)((0xFFFFFFFFu >> PAGE_SHIFT) + 1) \
                            : (uint32_t)((0xFFFFFFu >> PAGE_SHIFT) + 1))
#define DRM_FILE_PAGE_OFFSET_SIZE                                           \
    (sizeof(uintptr_t) == 8 ? (uint32_t)((0xFFFFFFFFu >> PAGE_SHIFT) * 256) \
                            : (uint32_t)((0xFFFFFFu >> PAGE_SHIFT) * 16))

/** Make `obj` an object of `size` bytes, a whole number of pages, of `dev`
 * with the hooks `funcs`: one reference, the caller's, a reference on the
 * device, and its own reservation. */
void drm_gem_object_init(struct drm_device *dev, struct drm_gem_object *obj, uint64_t size,
                         const struct drm_gem_object_funcs *funcs);
/** Take a reference to `obj`, which the caller holds one to; NULL is
 * allowed. Any context. */
void drm_gem_object_get(struct drm_gem_object *obj);
/** Take a reference to `obj` unless its last has gone, as Linux's
 * kref_get_unless_zero: 1 when it took one, 0 when the object's free is
 * under way. For a caller that reaches the object through a pointer that
 * holds none, such as a binding's, or through its owner's list, as
 * drm_gem_lru_scan does (drm_gem.c:1519). Any context. */
int drm_gem_object_get_unless_zero(struct drm_gem_object *obj);
/** Drop a reference; the last takes the object off its device's lists and
 * frees it through its `free`, and drops its device. NULL is allowed. */
void drm_gem_object_put(struct drm_gem_object *obj);
/** The object of `dev` after `prev`, or its first when `prev` is NULL,
 * with a reference taken, or NULL past the last; drops the caller's
 * reference to `prev`. Objects whose last reference is going are passed
 * over. A walk that may sleep is `for (o = drm_gem_object_next(dev, NULL);
 * o; o = drm_gem_object_next(dev, o))`; one that leaves early puts `o`. */
struct drm_gem_object *drm_gem_object_next(struct drm_device *dev, struct drm_gem_object *prev);
/** Give `file` a new handle to `obj`, the lowest free from 1, in `*handle`,
 * with a reference of its own, and run the object's `open` hook: 0, or
 * -ENOMEM, or the hook's -errno, which leaves no handle. */
int32_t drm_gem_handle_create(struct drm_file *file, struct drm_gem_object *obj, uint32_t *handle);
/** The object of `file`'s `handle`, with a reference taken, or NULL. */
struct drm_gem_object *drm_gem_object_lookup(struct drm_file *file, uint32_t handle);
/** Drop `file`'s `handle`: 0, or -EINVAL for no such handle. */
int32_t drm_gem_handle_delete(struct drm_file *file, uint32_t handle);
/** Drop every handle of `file`, as its release does. */
void drm_gem_release(struct drm_file *file);
/** Give `obj` its place in its device's mmap space, once: 0, or -ENOSPC
 * when the space has no room for it. */
int32_t drm_gem_create_mmap_offset(struct drm_gem_object *obj);
/**
 * mmap of a card or render node, as Linux's drm_gem_mmap: the object whose
 * pages in the mmap space hold the region's page offset, which the file has
 * a handle to, mapped by its `mmap` with `pgoff` the page within it, with a
 * reference the region holds; or the object of a driver's node there,
 * mapped by the node's `mmap`. -ENODEV for a device unplugged or an object
 * without `mmap`, -EINVAL for no object at that offset or a range past its
 * end, -EACCES for an object the file has no handle to, or the hook's
 * error.
 */
int32_t drm_gem_mmap(struct drm_file *file, vma_t *vma);

/*
 * A further place of an object in its device's mmap space, which a driver
 * gives for another way of mapping it, as i915 gives one for each kind of
 * mapping MMAP_OFFSET names (Linux's drm_vma_offset_node beside the
 * object's own). It spans the object's size from `offset`, in bytes, a
 * whole number of pages, 0 until `drm_mmap_node_add` places it. An mmap at
 * that offset by a file with a handle to `obj` calls `mmap`, with the
 * node and the region, as `drm_gem_object_funcs.mmap` is called: on
 * success the region takes over a reference to `obj` that the core took.
 * The driver keeps the node, and takes it out with `drm_mmap_node_remove`
 * before `obj` is freed; the node holds no reference.
 */
struct drm_mmap_node {
    struct drm_gem_object *obj;
    uint64_t offset;
    int32_t (*mmap)(struct drm_mmap_node *node, vma_t *vma);
    struct drm_mmap_node *next; /* the device's nodes, by offset */
};

/** Place `node`, whose `obj` and `mmap` are set, in the mmap space of its
 * object's device, first-fit beside the objects' own offsets: 0, or
 * -ENOSPC when the space has no room. A placed node is left as it is. */
int32_t drm_mmap_node_add(struct drm_mmap_node *node);
/** Take `node` out of its device's mmap space; nothing when unplaced. */
void drm_mmap_node_remove(struct drm_mmap_node *node);

/*
 * A buffer object of pages from the frame allocator, Linux's GEM shmem
 * helper, which drivers without memory of their own use for dumb buffers
 * and imports: its pages are given as they are first touched, zeroed, and
 * mapped cached, or write-combining when `map_wc` is set, as Linux maps
 * them. Its `free` and `mmap` are the helper's.
 */
struct drm_gem_shmem_object {
    struct drm_gem_object base;
    struct shmem pages;
    int map_wc;
};

/** A new shmem object of `size` bytes, rounded up to pages, with one
 * reference, the caller's; NULL with no memory or for a size of 0 or past
 * SHMEM_MAX_SIZE. */
struct drm_gem_shmem_object *drm_gem_shmem_create(struct drm_device *dev, uint64_t size,
                                                  int map_wc);
/** The frame of page `pgoff` of `obj`, given and zeroed if it had none, or
 * 0 when none can be had or the page is past the object; no reference is
 * taken. For a driver that reads or writes the buffer itself. */
uintptr_t drm_gem_shmem_page(struct drm_gem_shmem_object *obj, uint32_t pgoff);
/** A driver's `dumb_create` of shmem objects, Linux's
 * drm_gem_shmem_dumb_create: the pitch of `width` pixels of `bpp` bits,
 * rounded up to bytes, and the size of `height` rows in whole pages, mapped
 * cached. 0, -EINVAL for a size past the helper's reach, or -ENOMEM. */
int32_t drm_gem_shmem_dumb_create(struct drm_file *file, struct drm_device *dev,
                                  struct drm_mode_create_dumb *args);
/** The same, the objects mapped write-combined, for vgem, whose importers
 * have no call that would bracket a processor's access. */
int32_t drm_gem_shmem_dumb_create_wc(struct drm_file *file, struct drm_device *dev,
                                     struct drm_mode_create_dumb *args);

/* ------------------------------------------------------------------------
 * Open files
 */

/* The node a file is open on. */
#define DRM_MINOR_PRIMARY 0
#define DRM_MINOR_RENDER  2

/* An event queued for a file's reader, the bytes of one uapi event. Before
 * it is sent, `next` links it on its CRTC's vblank list while it waits
 * there for the count `sequence`; after, on its file's queue. `flip_crtc`
 * names the CRTC whose pending flip its sending completes, for a commit's
 * event (drm_vblank.c). A commit's event may have no file, when it serves
 * an out-fence or the flip's tracking alone. */
struct drm_pending_event {
    struct drm_pending_event *next;
    struct drm_pending_event *pending_next; /* the file's reserved events */
    struct drm_file *file;                  /* NULL once the file is released unsent */
    struct drm_device *dev;                 /* whose `event_lock` keeps it once reserved */
    struct dma_fence *fence;                /* the sender signals it as it sends, or NULL */
    uint64_t sequence;                      /* the vblank count it waits for */
    struct drm_crtc *flip_crtc;             /* the CRTC whose flip it completes, or NULL */
    uint32_t length;
    union {
        struct drm_event base;
        struct drm_event_vblank vbl;
        struct drm_event_crtc_sequence seq;
    } event;
};

/*
 * A handle of a file's table: to a buffer object or to a syncobj. A buffer
 * object's handle is also on the object's list of handles, with the file
 * that holds it, which is what mmap asks, as Linux's vm_files of the
 * object's drm_vma_offset_node.
 */
struct drm_handle {
    uint32_t id;
    void *object;
    struct dma_buf *prime;         /* held export/import cache, NULL for other handles */
    struct drm_handle *prime_next; /* the table's PRIME bucket */
    struct drm_file *file;         /* the holder, for a buffer object's handle */
    struct drm_handle *obj_next;   /* the object's handles */
};

/* A file's table of handles, indexed by id as Linux's idr is, so that a
 * lookup costs one load whatever the handles a client holds. Its handles
 * with a PRIME cache are also hashed by their dma-buf, as Linux's
 * prime_fpriv->dmabufs, so that an import finds its handle without a walk
 * over the table. Every call below takes `lock` itself and allocates and
 * frees outside it. */
struct drm_handles {
    struct spinlock lock;      /* the members below and each handle's `prime` */
    struct drm_handle **slot;  /* slot[id], NULL where the id is free; slot[0] unused */
    uint32_t cap;              /* the slots allocated, 0 for none */
    uint32_t free;             /* no id below it is free */
    struct drm_handle **prime; /* buckets of the PRIME caches, NULL before the first */
    uint32_t prime_buckets;    /* a power of two, 0 before the first */
    uint32_t prime_count;      /* handles with a PRIME cache */
};

/** Add `object` to the table `t` under the lowest free id from 1, as
 * Linux's idr gives handles, into `*id`: 0, or -ENOMEM. */
int32_t drm_handle_insert(struct drm_handles *t, void *object, uint32_t *id);
/** The object of `id` in `t`, or NULL; no reference is taken, so the
 * caller keeps the object alive by other means. */
void *drm_handle_find(struct drm_handles *t, uint32_t id);
/** The object of `id` in `t`, or NULL, with a reference that `get` takes
 * under the table's lock, so that no removal frees it first. */
void *drm_handle_find_get(struct drm_handles *t, uint32_t id, void (*get)(void *object));
/** Take `id` out of `t`, drop its PRIME cache reference and return its
 * object, or NULL for none. */
void *drm_handle_remove(struct drm_handles *t, uint32_t id);
/** Make `buf` the PRIME cache of `t`'s handle `id`, with the caller's
 * reference, as Linux's drm_prime_add_buf_handle: 0; -ENOENT when `id`
 * names no handle or one to another object than `object`; -EEXIST when the
 * handle has a cache already, which stays; or -ENOMEM. On an error the
 * reference stays with the caller. */
int32_t drm_handle_set_prime(struct drm_handles *t, uint32_t id, const void *object,
                             struct dma_buf *buf);
/** The PRIME cache of `t`'s handle `id`, with a reference for the caller,
 * or NULL when it has none or `id` names no handle. */
struct dma_buf *drm_handle_prime_get(struct drm_handles *t, uint32_t id);
/** The lowest id of `t` whose PRIME cache is `buf`, or 0 for none, as
 * Linux's drm_prime_lookup_buf_handle. */
uint32_t drm_handle_prime_find(struct drm_handles *t, const struct dma_buf *buf);
/** The lowest id from `id` that is in use in `t`, or 0 when none is: a
 * walk over the table is `for (i = drm_handle_next(t, 1); i;
 * i = drm_handle_next(t, i + 1))`. */
uint32_t drm_handle_next(struct drm_handles *t, uint32_t id);
/** Free the slots and PRIME buckets of `t`, whose handles have all been
 * removed. */
void drm_handles_fini(struct drm_handles *t);

/*
 * An open of a card or render node, one per open file description. The
 * client caps are what SET_CLIENT_CAP set. `event_space` is how many bytes
 * of events may still queue, Linux's 4096 at open. The events, the
 * reserved list and `event_space` are under the device's `event_lock`.
 *
 * The master role follows Linux's drm_master records, each named here by
 * an id from the device's `next_master_id`. A file of the card has the id
 * of the master it was opened under, or a new one of its own when it
 * became master with none (`is_master`, Linux's meaning: the file made the
 * record). The device's `master` is the file that holds the role now
 * (`drm_is_current_master`). `was_master` says it held it once, which lets
 * it take the role back without privilege while its process is the same.
 * GET_MAGIC gives a magic in the namespace of the file's master id, and
 * AUTH_MAGIC by the master spends it (`magic_spent`) as it authenticates
 * the file. SET_VERSION 1.1 or later by the master gives its record the
 * bus name, Linux's master->unique, which `set_busid` marks on each file
 * of the record; GET_UNIQUE answers it, or nothing before. A render
 * node's file has no master id, 0.
 */
struct drm_file {
    struct drm_device *dev;
    int minor_type; /* DRM_MINOR_PRIMARY or DRM_MINOR_RENDER */
    uint32_t pid;   /* the last thread group to use it, Linux's update */
    uint32_t uid;
    drm_magic_t magic; /* 0 until GET_MAGIC */
    int magic_spent;
    int authenticated;
    uint32_t master_id;
    int is_master;
    int was_master;
    int set_busid; /* its master record has the bus name: SET_VERSION 1.1 or later */
    int console;   /* the console's own file (drm_client.c), never a node's */
    int universal_planes, atomic, aspect_ratio, writeback_connectors, stereo_allowed,
        supports_virtualized_cursor_plane;
    struct drm_handles objects;  /* buffer object handles */
    struct drm_handles syncobjs; /* syncobj handles */
    struct drm_framebuffer *fbs; /* framebuffers it made, through `file_next` */
    struct drm_pending_event *events, *events_tail;
    struct drm_pending_event *pending; /* reserved, not yet sent */
    uint32_t event_space;
    wait_queue_t event_wait;
    void *driver_priv;
    struct drm_file *dev_next; /* the device's open files */
};

/** Whether `file` holds its device's master role now. */
#define drm_is_current_master(file) ((file)->dev->master == (file))

/*
 * Events, Linux's drm_event_reserve, drm_send_event and
 * drm_event_cancel_free. A sender allocates `e` with kzalloc, sets its
 * event and `length`, and reserves it, which counts its length against the
 * file's space and keeps it on the file's list of reserved events. Sending
 * queues it for the reader, signals its fence and wakes the reader; the
 * file frees it once read. A file released first leaves its reserved
 * events with `file` NULL, which a send then frees unread.
 */

/** Reserve `e` on `file`: 0, or -ENOMEM when the file's space has not
 * `e->length` bytes, as Linux. */
int32_t drm_event_reserve(struct drm_file *file, struct drm_pending_event *e);
/** Send `e`, reserved; it is the file's, or freed. Any context. */
void drm_send_event(struct drm_pending_event *e);
/** Send `e` as `drm_send_event` does, with its device's `event_lock`
 * held, under which its fence is signalled, as Linux's
 * drm_send_event_locked; an event never reserved is freed. */
void drm_send_event_locked(struct drm_pending_event *e);
/** Take back `e`, reserved and not sent, give back its space and free
 * it. */
void drm_event_cancel_free(struct drm_pending_event *e);

/** Release what `file` holds of its device's display, as its release does:
 * each framebuffer it made, taken off the planes that show it first, as
 * RMFB does, and each blob it made. May sleep in the commit. */
void drm_mode_file_release(struct drm_file *file);

/**
 * The DRM ioctl dispatcher, Linux's drm_ioctl: the handler of `cmd` from
 * the core's table, or for numbers DRM_COMMAND_BASE to DRM_COMMAND_END - 1
 * the driver's, called with the kernel's copy of the argument at `arg` if
 * the caller may call it. ENODEV on a device unplugged, ENOTTY for a
 * number of another type, EINVAL for a number no row names, EACCES for a
 * caller the row's flags refuse, EFAULT, ENOMEM, or the handler's answer.
 * The argument is copied back by the direction the number and the row
 * share, whatever the handler answered. Called without the big lock, since
 * a node's operations are FILE_OPS_UNLOCKED: the copies run without it,
 * and the handler of a row without DRM_UNLOCKED, with the permission
 * check, under it. May sleep.
 */
int32_t drm_ioctl(struct drm_file *file, uint32_t cmd, uintptr_t arg);

/** Whether a held descriptor is an open DRM card or render node. */
int drm_is_file(const fd_entry_t *entry);

/* ------------------------------------------------------------------------
 * Display state: mode objects and their atomic state (76-5a, 76-5b)
 *
 * A device's display is Linux's KMS: CRTCs, planes, encoders and
 * connectors, which its driver makes once (drm_mode.c's init calls), and
 * framebuffers and blobs, which programs and the core make and free. Each
 * is a mode object with an ID from 1, the lowest free, as Linux's idr gives
 * them, and a list of properties. The state a program sets lives in each
 * CRTC's, plane's and connector's `state`, which a commit replaces whole:
 * it duplicates the states it changes into a `struct drm_atomic_state`,
 * checks them, and swaps them in (drm_atomic.c). Every state a commit can
 * change is taken under the device's commit lock (`drm_modeset_lock`).
 */

/* A mode object: its ID, its DRM_MODE_OBJECT_* type, and its properties
 * with the values an atomic state does not hold itself (the immutable ones,
 * and every one of a driver without DRIVER_ATOMIC). */
#define DRM_OBJECT_MAX_PROPERTY 64
struct drm_property;
struct drm_mode_object {
    uint32_t id;
    uint32_t type;
    uint32_t refs; /* framebuffers and blobs; 0 for the fixed objects */
    uint32_t count_props;
    struct drm_property *props[DRM_OBJECT_MAX_PROPERTY];
    uint64_t prop_values[DRM_OBJECT_MAX_PROPERTY];
};

/* A rectangle from (x1, y1) to (x2, y2), exclusive; in pixels, or in 16.16
 * fixed point for a plane's source. */
struct drm_rect {
    int32_t x1, y1, x2, y2;
};

/* One value of an enum or bitmask property and its name; for a bitmask the
 * value is the bit's number. */
struct drm_prop_enum_list {
    int type;
    const char *name;
};

/*
 * A property: its name, its DRM_MODE_PROP_* flags and its values: a range's
 * two bounds, an object property's object type, or an enum's values and a
 * bitmask's bit numbers with a name for each. Made by the core's and the
 * drivers' init and freed with the device's mode configuration.
 */
struct drm_property {
    struct drm_mode_object base;
    struct drm_device *dev;
    char name[DRM_PROP_NAME_LEN];
    uint32_t flags;
    uint32_t num_values;
    uint64_t *values;
    const char **enum_names; /* one per value for enums and bitmasks, else NULL */
    struct drm_property *next;
};

/* A blob: bytes a property names by ID, made by the kernel or by
 * CREATEPROPBLOB, and freed by its last holder. While its maker holds it,
 * `owner` names the file, which DESTROYPROPBLOB and close let go of. */
struct drm_property_blob {
    struct drm_mode_object base;
    struct drm_device *dev;
    struct drm_file *owner; /* its maker while it holds it, NULL for the kernel */
    uint32_t length;
    void *data;
    struct drm_property_blob *next;
};

/* A pixel format as Linux's drm_format_info: its depth for ADDFB and
 * GETFB, its planes, the bytes of a pixel in each, and the subsampling of
 * every plane but the first. */
struct drm_format_info {
    uint32_t format;
    uint8_t depth;
    uint8_t num_planes;
    uint8_t cpp[3];
    uint8_t hsub, vsub;
    uint8_t has_alpha;
};

/** The format `format`'s description, or NULL for one the core does not
 * know. */
const struct drm_format_info *drm_format_info(uint32_t format);
/** ADDFB's format for `bpp` and `depth`, Linux's drm_mode_legacy_fb_format,
 * or DRM_FORMAT_INVALID. */
uint32_t drm_mode_legacy_fb_format(uint32_t bpp, uint32_t depth);

/* A framebuffer: up to four planes of buffer objects in a fourcc format.
 * Held by its ID while its maker has it (`owner`), by each plane state that
 * shows it and by each lookup; the last put frees it. */
struct drm_framebuffer {
    struct drm_mode_object base;
    struct drm_device *dev;
    struct drm_file *owner; /* its maker, until RMFB, CLOSEFB or close */
    char comm[16];          /* its maker's program name, for debugfs */
    const struct drm_format_info *info;
    uint32_t format;   /* DRM_FORMAT_* */
    uint64_t modifier; /* DRM_FORMAT_MOD_LINEAR unless ADDFB2 gave one */
    uint32_t flags;    /* DRM_MODE_FB_* */
    uint32_t width, height;
    uint32_t pitches[4], offsets[4];
    struct drm_gem_object *obj[4];     /* held; the same object may repeat */
    struct drm_framebuffer *next;      /* the device's list */
    struct drm_framebuffer *file_next; /* the owner's list, newest first */
};

#define DRM_PLANE_TYPE_OVERLAY 0
#define DRM_PLANE_TYPE_PRIMARY 1
#define DRM_PLANE_TYPE_CURSOR  2

/*
 * A plane's state: the framebuffer it shows (held), where on its CRTC in
 * pixels, and which part of the framebuffer in 16.16 fixed point; the
 * fence to wait for before scanning out (IN_FENCE_FD, or else the commit
 * takes the framebuffer's pending writes; held), and the damage since the
 * last commit (FB_DAMAGE_CLIPS, held). `src`, `dst` and `visible`
 * are set by `drm_atomic_helper_check_plane_state` for the driver: the
 * source and the destination clipped to the CRTC's mode.
 */
struct drm_plane_state {
    struct drm_plane *plane;
    struct drm_crtc *crtc;
    struct drm_framebuffer *fb;
    int32_t crtc_x, crtc_y;
    uint32_t crtc_w, crtc_h;
    uint32_t src_x, src_y, src_w, src_h;
    uint32_t rotation; /* DRM_MODE_ROTATE_* | DRM_MODE_REFLECT_* */
    uint32_t zpos;
    uint16_t alpha; /* 0xffff opaque */
    int32_t hotspot_x, hotspot_y;
    struct dma_fence *fence;
    struct drm_property_blob *fb_damage_clips;
    struct drm_rect src, dst;
    int visible;
};

/*
 * A CRTC's state: whether it is on, its mode (with the blob MODE_ID names,
 * held), the planes and connectors it feeds, the colour blobs, and the
 * event to send and the out-fence to signal when the commit that made this
 * state reaches the screen. The `_changed` marks are the check's, for the
 * commit: a CRTC needs a modeset (`drm_atomic_crtc_needs_modeset`) when its
 * mode, its enable, its active or its connectors change.
 */
struct drm_crtc_state {
    struct drm_crtc *crtc;
    int enable;
    int active;
    struct drm_mode_modeinfo mode;
    struct drm_property_blob *mode_blob;
    uint32_t plane_mask;     /* bit N: the plane of index N */
    uint32_t connector_mask; /* bit N: the connector of index N */
    struct drm_property_blob *degamma_lut, *ctm, *gamma_lut;
    int vrr_enabled;
    int mode_changed, active_changed, connectors_changed, planes_changed;
    int color_mgmt_changed;
    int async_flip;
    struct drm_pending_event *event;
    int32_t __user *out_fence_ptr;
};

/* A connector's state: the CRTC it is routed to and the encoder that drives
 * it there, which the check chooses, and the values of the properties a
 * driver may attach: "scaling mode" in `scaling_mode`, DRM_MODE_SCALE_*,
 * and "max bpc" in `max_requested_bpc`. */
struct drm_connector_state {
    struct drm_connector *connector;
    struct drm_crtc *crtc;
    struct drm_encoder *best_encoder;
    uint32_t link_status; /* DRM_MODE_LINK_STATUS_* */
    uint32_t content_protection;
    uint32_t scaling_mode;
    uint32_t max_requested_bpc;
};

/*
 * A plane's hooks. `atomic_check` refuses a state the hardware cannot show
 * with -EINVAL or -ERANGE; `atomic_update` makes the new state the
 * hardware's and `atomic_disable` turns the plane off, between the CRTC's
 * `atomic_begin` and `atomic_flush`. They may sleep in a blocking commit.
 */
struct drm_plane_funcs {
    int32_t (*atomic_check)(struct drm_plane *plane, struct drm_atomic_state *state);
    void (*atomic_update)(struct drm_plane *plane, struct drm_atomic_state *state);
    void (*atomic_disable)(struct drm_plane *plane, struct drm_atomic_state *state);
};

/*
 * A plane: a layer a CRTC composes. `formats` and `modifiers` are the
 * driver's, kept for the plane's life; NULL `modifiers` is
 * DRM_FORMAT_MOD_LINEAR alone. The optional properties are NULL until
 * their create call attaches them.
 */
struct drm_plane {
    struct drm_mode_object base;
    struct drm_device *dev;
    uint32_t index;
    int type; /* DRM_PLANE_TYPE_* */
    uint32_t possible_crtcs;
    const uint32_t *formats;
    uint32_t format_count;
    const uint64_t *modifiers;
    uint32_t modifier_count;
    const struct drm_plane_funcs *funcs;
    struct drm_plane_state *state;
    struct drm_property *rotation_property, *zpos_property, *alpha_property;
    struct drm_property *hotspot_x_property, *hotspot_y_property;
};

/*
 * A CRTC's hooks. The atomic ones run in a commit as Linux's helpers run
 * them; `mode_valid` answers 0 for a mode the CRTC can drive, checked when
 * a commit changes the mode. `enable_vblank` and `disable_vblank` start and
 * stop the CRTC's vblank interrupt or timer, whose handler calls
 * `drm_crtc_handle_vblank` once a frame; a CRTC with `enable_vblank` has
 * vblanks (drm_vblank.c), and one without sends a commit's events as the
 * commit ends. `enable_vblank` answers 0 or -errno; `disable_vblank` may be
 * called from inside `drm_crtc_handle_vblank`, which turns the interrupt
 * off from there once nobody has wanted vblanks for the off-delay. The CRC
 * hooks serve debugfs's crc/control and crc/data (76-18b):
 * `set_crc_source` starts the source `name`, or stops it for NULL, after
 * which the driver adds one entry a frame with `drm_crtc_add_crc_entry`;
 * `verify_crc_source` answers 0 when `name` is one and sets how many values
 * each frame's entry has, without starting it, or -EINVAL; and
 * `get_crc_sources` lists them. A CRTC without `set_crc_source` and
 * `verify_crc_source` has no CRCs. `get_vblank_counter`, when given, reads
 * the hardware's 32-bit frame counter, which advances at each vblank
 * whether or not the interrupt is on, so that the count takes in the
 * vblanks that came while it was off, as Linux's hook of that name does.
 * `get_vblank_timestamp`, when given, sets `*time_ns` to the
 * CLOCK_MONOTONIC time at which the latest vertical blank ends, from where
 * the CRTC scans, and answers 1, or 0 when it cannot say, as Linux's hook
 * of that name; the core then stamps a vblank with the clock when its
 * handler runs.
 */
struct drm_crtc_funcs {
    int32_t (*atomic_check)(struct drm_crtc *crtc, struct drm_atomic_state *state);
    void (*atomic_enable)(struct drm_crtc *crtc, struct drm_atomic_state *state);
    void (*atomic_disable)(struct drm_crtc *crtc, struct drm_atomic_state *state);
    void (*atomic_begin)(struct drm_crtc *crtc, struct drm_atomic_state *state);
    void (*atomic_flush)(struct drm_crtc *crtc, struct drm_atomic_state *state);
    int32_t (*mode_valid)(struct drm_crtc *crtc, const struct drm_mode_modeinfo *mode);
    int32_t (*enable_vblank)(struct drm_crtc *crtc);
    void (*disable_vblank)(struct drm_crtc *crtc);
    int32_t (*set_crc_source)(struct drm_crtc *crtc, const char *name);
    int32_t (*verify_crc_source)(struct drm_crtc *crtc, const char *name, uint32_t *values_cnt);
    const char *const *(*get_crc_sources)(struct drm_crtc *crtc, uint32_t *count);
    uint32_t (*get_vblank_counter)(struct drm_crtc *crtc);
    int (*get_vblank_timestamp)(struct drm_crtc *crtc, int64_t *time_ns, int in_vblank_irq);
};

/*
 * A CRTC's vblanks, Linux's drm_vblank_crtc: the count of vblanks and the
 * CLOCK_MONOTONIC time of the last, 0 until one comes after the CRTC is turned
 * on unless `get_vblank_timestamp` gives it. `refs` counts the holders that
 * keep the interrupt on: each waiter, each queued event, and while the CRTC is
 * off (`inmodeset`) one that keeps it off, since a hold taken then would never
 * see a vblank. `put_ns` is when the last holder let go, from which the
 * off-delay runs. `events` are the events waiting for a count, in the order
 * queued. `wait` wakes at each vblank, when vblanks stop, and when a commit's
 * flip on the CRTC completes. `last` is the hardware's frame counter when the
 * count last took it in, for a CRTC with `get_vblank_counter`. `backwards`
 * counts the times stored before the latest one stored earlier,
 * `stamp_max_ns`, which debugfs's `crtc-N/vblank_backwards` reads.
 * `framedur_ns` is a frame of the CRTC's mode, Linux's field of that name,
 * which `drm_crtc_vblank_on` sets from the mode and which is 0 for a mode
 * with no clock.
 */
struct drm_vblank {
    uint32_t seq; /* odd while `count` and `time_ns` change, Linux's seqlock */
    uint64_t count;
    uint32_t last;
    int64_t time_ns;
    int64_t stamp_max_ns;
    int64_t framedur_ns;
    uint32_t backwards;
    uint32_t refs;
    int enabled;   /* the driver's interrupt is on */
    int inmodeset; /* the CRTC is off, and `refs` holds one for it */
    int64_t put_ns;
    struct drm_pending_event *events, *events_tail;
    wait_queue_t wait;
};

/* One frame's CRC entry, which debugfs's crc/data reads, and the entries
 * a CRTC keeps while crc/data is open, a power of two. */
#define DRM_MAX_CRC_NR     10
#define DRM_CRC_ENTRIES_NR 128
struct drm_crtc_crc_entry {
    int has_frame_counter;
    uint32_t frame;
    uint32_t crcs[DRM_MAX_CRC_NR];
};

/*
 * A CRTC's CRCs, Linux's drm_crtc_crc. `source` is what crc/control last
 * chose, "auto" at first. While crc/data is open (`opened`) the driver
 * fills `entries` from `head` and the reader takes them from `tail`; an
 * entry that finds them full is dropped and sets `overflow`. `values` is
 * how many values each entry has.
 */
struct drm_crtc_crc {
    char source[32];
    int opened, overflow;
    struct drm_crtc_crc_entry *entries; /* DRM_CRC_ENTRIES_NR while open, else NULL */
    uint32_t head, tail;
    uint32_t values;
    wait_queue_t wait;    /* wakes the reader at each entry */
    struct spinlock lock; /* `entries`, `head` and `tail`, Linux's crc->lock */
};

/* A commit's flip on a CRTC, from the swap of its state until its event is
 * sent at the vblank that shows it: queued while the commit worker has yet
 * to program it, armed once it waits only for the vblank. */
#define DRM_FLIP_NONE   0
#define DRM_FLIP_ARMED  1
#define DRM_FLIP_QUEUED 2

/* A CRTC: a scanout engine that reads planes and drives connectors. The
 * cursor's place is the last MODE_CURSOR moved it to. `flip_pending` is its
 * flip, DRM_FLIP_*; a nonblocking commit meanwhile is EBUSY and a blocking
 * one waits. `fence_context` is the timeline of its out-fences, 0 until
 * the first, and `fence_seqno` the last point made on it. */
struct drm_crtc {
    struct drm_mode_object base;
    struct drm_device *dev;
    uint32_t index;
    char name[32];
    struct drm_plane *primary, *cursor;
    int32_t cursor_x, cursor_y;
    uint32_t gamma_size;
    uint16_t *gamma_store; /* gamma_size red, then green, then blue */
    const struct drm_crtc_funcs *funcs;
    struct drm_crtc_state *state;
    struct drm_vblank vblank;
    struct drm_crtc_crc crc;
    int flip_pending;
    uint64_t fence_context, fence_seqno;
};

/* An encoder, which Linux keeps for its interface: fixed to the CRTCs it
 * can take, and to the CRTC it drives after the last commit. */
struct drm_encoder {
    struct drm_mode_object base;
    struct drm_device *dev;
    uint32_t index;
    uint32_t encoder_type; /* DRM_MODE_ENCODER_* */
    uint32_t possible_crtcs, possible_clones;
    struct drm_crtc *crtc;
};

/* A connector's detected state, the `connection` of GETCONNECTOR. */
#define DRM_CONNECTOR_CONNECTED    1
#define DRM_CONNECTOR_DISCONNECTED 2
#define DRM_CONNECTOR_UNKNOWN      3

/* debugfs's `force` of a connector, Linux's enum drm_connector_force. */
#define DRM_FORCE_UNSPECIFIED 0
#define DRM_FORCE_OFF         1
#define DRM_FORCE_ON          2
#define DRM_FORCE_ON_DIGITAL  3

/*
 * A connector's hooks. `detect` answers DRM_CONNECTOR_* and may sleep on a
 * probe of the sink, NULL for one always connected; `get_modes` reads the
 * sink's EDID and modes into the connector (`drm_connector_update_edid`,
 * `drm_mode_probed_add`) and returns how many; `mode_valid` answers 0 for a
 * mode the hardware can drive. `force` tells the driver the debug force
 * changed, after the core has set `force` (76-18b).
 */
struct drm_connector_funcs {
    int (*detect)(struct drm_connector *connector, int force_probe);
    int (*get_modes)(struct drm_connector *connector);
    int32_t (*mode_valid)(struct drm_connector *connector, const struct drm_mode_modeinfo *mode);
    void (*force)(struct drm_connector *connector);
};

/* A connector's list of modes. */
struct drm_display_mode {
    struct drm_mode_modeinfo info;
    struct drm_display_mode *next;
};

/*
 * A connector: an output a sink is plugged into. Its name is Linux's,
 * the type's name and `connector_type_id` ("Virtual-1", "eDP-1"), which
 * /sys and Weston's configuration use. `modes` are the modes the last probe
 * kept, preferred first, and `probed_modes` those a probe's `get_modes`
 * gathers before they are checked. `edid` is the blob of the sink's EDID,
 * or of `edid_override` while debugfs set one (76-18b), which replaces the
 * sink's whenever the modes are read; the override is kept bytes, not a
 * blob, so it has no ID a program could name. `dpms` is the legacy DPMS
 * property's value. `encoder` is the encoder that drives it after the last
 * commit.
 */
struct drm_connector {
    struct drm_mode_object base;
    struct drm_device *dev;
    uint32_t index;
    uint32_t connector_type; /* DRM_MODE_CONNECTOR_* */
    uint32_t connector_type_id;
    char name[DRM_CONNECTOR_NAME_LEN];
    int status; /* DRM_CONNECTOR_* */
    int force;  /* DRM_FORCE_* */
    int dpms;   /* DRM_MODE_DPMS_ON or DRM_MODE_DPMS_OFF */
    int interlace_allowed, doublescan_allowed;
    uint32_t mm_width, mm_height;
    uint32_t subpixel;
    uint32_t possible_encoders; /* bit N: the encoder of index N */
    struct drm_encoder *encoder;
    struct drm_display_mode *modes;
    struct drm_display_mode *probed_modes;
    struct drm_property_blob *edid;
    uint8_t *edid_override; /* kmalloc'd, or NULL */
    uint32_t edid_override_len;
    const struct drm_connector_funcs *funcs;
    struct drm_connector_state *state;
    struct drm_property *scaling_mode_property, *max_bpc_property; /* NULL until attached */
};

/*
 * An atomic state: a commit's new states, by object index, with the old
 * ones they replace kept for the hooks and freed after; NULL where the
 * commit leaves an object alone. Before the swap, `*_old` are the objects'
 * states and `*_new` the commit's; after it, the objects hold `*_new`.
 * `allow_modeset` is set unless the atomic ioctl left out ALLOW_MODESET.
 */
#define DRM_MAX_CRTCS      8
#define DRM_MAX_PLANES     32
#define DRM_MAX_CONNECTORS 16
struct drm_atomic_state {
    struct drm_device *dev;
    uint32_t flags; /* DRM_MODE_ATOMIC_* and DRM_MODE_PAGE_FLIP_* */
    int allow_modeset;
    int legacy_cursor_update;
    struct drm_plane_state *planes_old[DRM_MAX_PLANES], *planes_new[DRM_MAX_PLANES];
    struct drm_crtc_state *crtcs_old[DRM_MAX_CRTCS], *crtcs_new[DRM_MAX_CRTCS];
    struct drm_connector_state *connectors_old[DRM_MAX_CONNECTORS],
        *connectors_new[DRM_MAX_CONNECTORS];
};

/*
 * A device's display: its limits, its objects, the properties every device
 * has, and the two commit hooks. `atomic_check` refuses a state the device
 * cannot show, with Linux's errors; `atomic_commit` makes a checked state
 * the device's, at once or, when `nonblock` is set, before the vblank,
 * sending each CRTC's event and signalling its out-fence when the new state
 * reaches the screen. NULL hooks take the core's helpers,
 * `drm_atomic_helper_check` and `drm_atomic_helper_commit`. `fb_dirty` says
 * DIRTYFB sends its rectangles to the planes that show the framebuffer as
 * FB_DAMAGE_CLIPS, as Linux's drm_atomic_helper_dirtyfb does; without it
 * DIRTYFB answers ENOSYS. `fb_create`, when set, checks a framebuffer
 * ADDFB or ADDFB2 made, before it gets its ID, as Linux's driver's
 * fb_create does: it may give a legacy framebuffer its object's modifier,
 * and an error refuses it; `fb_destroy` lets go what it took, before the
 * last put of the framebuffer drops its objects. `commit_fence_wait`, when
 * set, is called for each pending plane fence when the commit worker
 * reaches it, in Linux's wait order, so the driver can do what its wait
 * would, as i915's boosts the request. Called in process context without
 * the driver's interrupt or engine locks, with interrupts masked. It may
 * not sleep, and the fence is borrowed.
 * `commit_fence_timeout_ms`, when nonzero, bounds each plane's wait to that
 * many milliseconds from when it begins, uninterruptibly. On timeout the
 * commit stops waiting for the rest and programs the planes, as Linux's
 * intel_atomic_commit_fence_wait with i915_fence_timeout
 * (intel_display.c:7262-7280); 0 waits as Linux's
 * drm_atomic_helper_wait_for_fences, a blocking commit interruptibly and
 * a deferred one until every fence has signalled.
 */
struct drm_mode_config {
    uint32_t min_width, max_width, min_height, max_height;
    uint32_t cursor_width, cursor_height;
    uint32_t preferred_depth;
    int prefer_shadow;
    int async_page_flip;
    int fb_dirty;
    /* No format modifiers, as Linux's drivers that set it: planes have no
     * IN_FORMATS, ADDFB2_MODIFIERS is 0 and ADDFB2 refuses them. */
    int fb_modifiers_not_supported;
    uint32_t num_crtc, num_plane, num_connector, num_encoder;
    struct drm_crtc *crtcs[DRM_MAX_CRTCS];
    struct drm_plane *planes[DRM_MAX_PLANES];
    struct drm_connector *connectors[DRM_MAX_CONNECTORS];
    struct drm_encoder *encoders[DRM_MAX_CONNECTORS];
    struct drm_framebuffer *fbs;
    struct drm_property *properties;
    struct drm_property_blob *blobs;
    /* The standard properties, Linux's, made by drm_mode_config_init. */
    struct drm_property *edid_property, *dpms_property, *path_property, *tile_property;
    struct drm_property *link_status_property, *non_desktop_property, *plane_type_property;
    struct drm_property *prop_src_x, *prop_src_y, *prop_src_w, *prop_src_h;
    struct drm_property *prop_crtc_x, *prop_crtc_y, *prop_crtc_w, *prop_crtc_h;
    struct drm_property *prop_fb_id, *prop_in_fence_fd, *prop_out_fence_ptr, *prop_crtc_id;
    struct drm_property *prop_fb_damage_clips, *prop_active, *prop_mode_id, *prop_vrr_enabled;
    struct drm_property *degamma_lut_property, *degamma_lut_size_property, *ctm_property;
    struct drm_property *gamma_lut_property, *gamma_lut_size_property;
    struct drm_property *modifiers_property, *size_hints_property;
    int32_t (*atomic_check)(struct drm_device *dev, struct drm_atomic_state *state);
    int32_t (*atomic_commit)(struct drm_device *dev, struct drm_atomic_state *state, int nonblock);
    int32_t (*fb_create)(struct drm_device *dev, struct drm_framebuffer *fb);
    void (*fb_destroy)(struct drm_framebuffer *fb);
    void (*commit_fence_wait)(struct drm_device *dev, struct dma_fence *fence);
    uint32_t commit_fence_timeout_ms;
};

/* The index masks of Linux's drm_crtc_mask, drm_plane_mask and
 * drm_connector_mask. */
#define drm_crtc_mask(crtc)           (1u << (crtc)->index)
#define drm_plane_mask(plane)         (1u << (plane)->index)
#define drm_connector_mask(connector) (1u << (connector)->index)

/* ---- Making a device's display, in drm_mode.c ---- */

/** Give `dev` its display: the standard properties and the default limits.
 * 0 or -ENOMEM. Called once by a driver with DRIVER_MODESET before it makes
 * its CRTCs, planes, encoders and connectors. */
int32_t drm_mode_config_init(struct drm_device *dev);
/** Free what the core made for `dev`'s display: its framebuffers, blobs,
 * properties, the objects' states, modes and gamma tables. For the
 * driver's `release`, once no file is open. */
void drm_mode_config_cleanup(struct drm_device *dev);
/** Make `plane` a plane of `dev` of DRM_PLANE_TYPE_* `type` on the CRTCs of
 * `possible_crtcs` (0: the CRTC whose primary or cursor it becomes), with
 * the `format_count` formats `formats` and the modifiers `modifiers` up to
 * DRM_FORMAT_MOD_INVALID, or NULL for linear alone: its ID, its properties
 * (IN_FORMATS among them) and its first state, off. 0, -EINVAL past
 * DRM_MAX_PLANES, or -ENOMEM. */
int32_t drm_universal_plane_init(struct drm_device *dev, struct drm_plane *plane,
                                 uint32_t possible_crtcs, const struct drm_plane_funcs *funcs,
                                 const uint32_t *formats, uint32_t format_count,
                                 const uint64_t *modifiers, int type);
/** Make `crtc` a CRTC of `dev` with its primary plane `primary` and cursor
 * plane `cursor` (or NULL), named "crtc-N": its ID, its properties and its
 * first state, off. 0, -EINVAL past DRM_MAX_CRTCS, or -ENOMEM. */
int32_t drm_crtc_init_with_planes(struct drm_device *dev, struct drm_crtc *crtc,
                                  struct drm_plane *primary, struct drm_plane *cursor,
                                  const struct drm_crtc_funcs *funcs);
/** Give `crtc` a gamma table of `size` entries for GETGAMMA and SETGAMMA,
 * Linux's ramp of entry N at N << 8. 0, -EINVAL for 0 or over 65536
 * entries, or -ENOMEM. */
int32_t drm_mode_crtc_set_gamma_size(struct drm_crtc *crtc, uint32_t size);
/** Attach the colour properties, Linux's drm_crtc_enable_color_mgmt:
 * DEGAMMA_LUT with its size when `degamma_lut_size` is not 0, CTM when
 * `has_ctm`, GAMMA_LUT with its size when `gamma_lut_size` is not 0. A CRTC
 * with GAMMA_LUT or DEGAMMA_LUT takes SETGAMMA as a commit of that blob. */
void drm_crtc_enable_color_mgmt(struct drm_crtc *crtc, uint32_t degamma_lut_size, int has_ctm,
                                uint32_t gamma_lut_size);
/** Make `encoder` an encoder of `dev` of DRM_MODE_ENCODER_* `type`; the
 * driver sets `possible_crtcs`. 0 or -EINVAL past the table. */
int32_t drm_encoder_init(struct drm_device *dev, struct drm_encoder *encoder, uint32_t type);
/** Make `connector` a connector of `dev` of DRM_MODE_CONNECTOR_* `type`,
 * named by the type and the lowest free number of that type, with Linux's
 * standard properties (EDID on all but virtual and writeback connectors)
 * and its first state, unrouted. 0, -EINVAL past DRM_MAX_CONNECTORS, or
 * -ENOMEM. */
int32_t drm_connector_init(struct drm_device *dev, struct drm_connector *connector,
                           const struct drm_connector_funcs *funcs, uint32_t type);
/** Let `encoder` drive `connector`. 0. */
int32_t drm_connector_attach_encoder(struct drm_connector *connector, struct drm_encoder *encoder);
/** Give a virtual connector the EDID property, for a driver whose virtual
 * sink has an EDID (virtio-gpu, bochs). */
void drm_connector_attach_edid_property(struct drm_connector *connector);
/** Attach "scaling mode" to `connector` with the DRM_MODE_SCALE_* values
 * whose bits `mask` has, two at least, Linux's
 * drm_connector_attach_scaling_mode_property; the driver sets the state's
 * first value. 0, -EINVAL for a mask of fewer, or -ENOMEM. */
int32_t drm_connector_attach_scaling_mode_property(struct drm_connector *connector, uint32_t mask);
/** Attach "max bpc", `min` to `max` bits a colour, to `connector`, its
 * state's value `max`, Linux's drm_connector_attach_max_bpc_property. 0,
 * or -ENOMEM. */
int32_t drm_connector_attach_max_bpc_property(struct drm_connector *connector, uint32_t min,
                                              uint32_t max);
/** Attach FB_DAMAGE_CLIPS to `plane`, for a driver that copies only damage. */
void drm_plane_enable_fb_damage_clips(struct drm_plane *plane);
/** Attach "rotation", the bitmask of `supported` DRM_MODE_ROTATE_* and
 * DRM_MODE_REFLECT_* bits, starting at `rotation`. 0 or -ENOMEM. */
int32_t drm_plane_create_rotation_property(struct drm_plane *plane, uint32_t rotation,
                                           uint32_t supported);
/** Attach "zpos", a range from `min` to `max` starting at `zpos`, immutable
 * when `min` and `max` are equal. 0 or -ENOMEM. */
int32_t drm_plane_create_zpos_property(struct drm_plane *plane, uint32_t zpos, uint32_t min,
                                       uint32_t max);
/** Attach "alpha", a range from 0 to 0xffff starting opaque. 0 or
 * -ENOMEM. */
int32_t drm_plane_create_alpha_property(struct drm_plane *plane);

/* ---- Properties ---- */

/** A new property of `dev` named `name` with DRM_MODE_PROP_* `flags` and
 * room for `num_values` values, or NULL with no memory. The make calls
 * below fill the values. */
struct drm_property *drm_property_create(struct drm_device *dev, uint32_t flags, const char *name,
                                         uint32_t num_values);
struct drm_property *drm_property_create_range(struct drm_device *dev, uint32_t flags,
                                               const char *name, uint64_t min, uint64_t max);
struct drm_property *drm_property_create_signed_range(struct drm_device *dev, uint32_t flags,
                                                      const char *name, int64_t min, int64_t max);
struct drm_property *drm_property_create_bool(struct drm_device *dev, uint32_t flags,
                                              const char *name);
struct drm_property *drm_property_create_enum(struct drm_device *dev, uint32_t flags,
                                              const char *name,
                                              const struct drm_prop_enum_list *list, uint32_t n);
/** A bitmask of the bits of `list` that `supported` has. */
struct drm_property *drm_property_create_bitmask(struct drm_device *dev, uint32_t flags,
                                                 const char *name,
                                                 const struct drm_prop_enum_list *list, uint32_t n,
                                                 uint64_t supported);
struct drm_property *drm_property_create_object(struct drm_device *dev, uint32_t flags,
                                                const char *name, uint32_t type);
/** Attach `prop` to `obj` with the value `value`; nothing past
 * DRM_OBJECT_MAX_PROPERTY. */
void drm_object_attach_property(struct drm_mode_object *obj, struct drm_property *prop,
                                uint64_t value);
/** Set the value `obj` keeps for `prop`, the immutable ones' way: 0, or
 * -EINVAL when `prop` is not attached. */
int32_t drm_object_property_set_value(struct drm_mode_object *obj, struct drm_property *prop,
                                      uint64_t value);
/** The value of `prop` on `obj` as GETPROPERTIES reports it: from the
 * object's state for a mutable property of an atomic driver, else the kept
 * one. 0 or -EINVAL. */
int32_t drm_object_property_get_value(struct drm_mode_object *obj, struct drm_property *prop,
                                      uint64_t *value);
/** The property of ID `id` attached to `obj`, or NULL. */
struct drm_property *drm_object_find_property(const struct drm_mode_object *obj, uint32_t id);
/** Whether `value` may be set on `prop`, Linux's
 * drm_property_change_valid_get: in range, among the enum's values, of the
 * bitmask's bits, or naming a blob or an object of the property's type. */
int drm_property_change_valid(struct drm_property *prop, uint64_t value);

/** A blob of `dev` of `length` bytes copied from `data`, or zeroed for
 * NULL, with one reference, the caller's; NULL with *err -EINVAL for a
 * length of 0 or -ENOMEM. */
struct drm_property_blob *drm_property_create_blob(struct drm_device *dev, uint32_t length,
                                                   const void *data, int32_t *err);
/** The blob of ID `id`, with a reference taken, or NULL. */
struct drm_property_blob *drm_property_lookup_blob(struct drm_device *dev, uint32_t id);
struct drm_property_blob *drm_property_blob_get(struct drm_property_blob *blob);
/** Drop a reference; the last frees the blob and its ID. NULL is
 * allowed. */
void drm_property_blob_put(struct drm_property_blob *blob);
/**
 * Replace the blob `*slot` with the blob of ID `id`, or none for 0, as
 * Linux's drm_property_replace_blob_from_id: -EINVAL for an ID of no blob,
 * or of one whose length is not `expected_size` (when not negative) or not
 * a whole number of `expected_elem_size` (when not negative). `*replaced`
 * says whether the slot changed.
 */
int32_t drm_property_replace_blob_from_id(struct drm_device *dev, struct drm_property_blob **slot,
                                          uint64_t id, int32_t expected_size,
                                          int32_t expected_elem_size, int *replaced);

/* ---- Finding objects ---- */

/** The mode object of ID `id` and DRM_MODE_OBJECT_* `type`, or of any type
 * for DRM_MODE_OBJECT_ANY, with a reference taken on a framebuffer or a
 * blob; NULL for none. `file` is the caller's, NULL for the kernel. */
struct drm_mode_object *drm_mode_object_find(struct drm_device *dev, struct drm_file *file,
                                             uint32_t id, uint32_t type);
/** Drop the reference `drm_mode_object_find` took, if any. */
void drm_mode_object_put(struct drm_mode_object *obj);
struct drm_crtc *drm_crtc_find(struct drm_device *dev, struct drm_file *file, uint32_t id);
struct drm_plane *drm_plane_find(struct drm_device *dev, struct drm_file *file, uint32_t id);
struct drm_encoder *drm_encoder_find(struct drm_device *dev, struct drm_file *file, uint32_t id);
struct drm_connector *drm_connector_lookup(struct drm_device *dev, struct drm_file *file,
                                           uint32_t id);
/** The framebuffer of ID `id`, with a reference taken, or NULL. */
struct drm_framebuffer *drm_framebuffer_lookup(struct drm_device *dev, struct drm_file *file,
                                               uint32_t id);
void drm_framebuffer_get(struct drm_framebuffer *fb);
/** Drop a reference; the last frees the framebuffer, its ID and its holds
 * on its buffer objects. NULL is allowed. */
void drm_framebuffer_put(struct drm_framebuffer *fb);
/** Whether `plane` takes `format` with `modifier`. */
int drm_plane_has_format(const struct drm_plane *plane, uint32_t format, uint64_t modifier);
/** -ENOSPC unless the 16.16 source rectangle lies inside `fb`. */
int32_t drm_framebuffer_check_src_coords(uint32_t src_x, uint32_t src_y, uint32_t src_w,
                                         uint32_t src_h, const struct drm_framebuffer *fb);
/**
 * A framebuffer of `file`'s buffer objects as `r` describes it, Linux's
 * drm_internal_framebuffer_create, with one reference, the caller's, and
 * no owner; NULL with *err set: -EINVAL for flags, sizes, a format, pitch,
 * offset or modifier the device refuses, -ERANGE for a size past 32 bits,
 * -ENOENT for a handle of no object, -ENOMEM.
 */
struct drm_framebuffer *drm_internal_framebuffer_create(struct drm_device *dev,
                                                        const struct drm_mode_fb_cmd2 *r,
                                                        struct drm_file *file, int32_t *err);

/* ---- Modes and probing ---- */

/** The refresh rate of `mode` in Hz, rounded, as Linux's drm_mode_vrefresh. */
uint32_t drm_mode_vrefresh(const struct drm_mode_modeinfo *mode);
/** Name `mode` "WxH", with "i" for an interlaced one. */
void drm_mode_set_name(struct drm_mode_modeinfo *mode);
/** Whether two modes have the same timings, clock and flags. */
int drm_mode_equal(const struct drm_mode_modeinfo *a, const struct drm_mode_modeinfo *b);
/** Linux's drm_mode_validate_basic: 0 for a mode whose type, flags, clock
 * and timings are sane, else -EINVAL. */
int32_t drm_mode_validate_basic(const struct drm_mode_modeinfo *mode);
/** A program's mode `in` as the kernel keeps it, Linux's
 * drm_mode_convert_umode: its type cut to the known bits. 0, -ERANGE for a
 * clock or refresh past INT_MAX, -EINVAL for an unknown aspect ratio or a
 * mode `drm_mode_validate_basic` refuses. */
int32_t drm_mode_convert_umode(struct drm_mode_modeinfo *out, const struct drm_mode_modeinfo *in);
/** `n` divided by `d`, which is not 0: the kernel links no libgcc, whose
 * `__udivdi3` an i686 64-bit division would call. */
uint64_t drm_div64(uint64_t n, uint64_t d);
/** Add a copy of `mode` to `connector`'s probed modes. 0 or -ENOMEM. */
int32_t drm_mode_probed_add(struct drm_connector *connector, const struct drm_mode_modeinfo *mode);
/**
 * Probe `connector` as Linux's drm_helper_probe_single_connector_modes:
 * its status by `detect` and the debug force, its modes by `get_modes` when
 * connected, or the modes up to 1024x768 when it gave none, each kept only
 * if sane, at most `max_width` by `max_height` (0: no limit), of flags the
 * connector allows and valid to the driver, without duplicates, sorted
 * preferred first, then by area, refresh and clock. Returns the number of
 * modes. May sleep in `detect` and `get_modes`.
 */
int drm_helper_probe_single_connector_modes(struct drm_connector *connector, uint32_t max_width,
                                            uint32_t max_height);
/** Add the modes of Linux's DMT table no larger than `hdisplay` by
 * `vdisplay` and no faster than 61 Hz, Linux's drm_add_modes_noedid. The
 * number added. */
int drm_add_modes_noedid(struct drm_connector *connector, uint32_t hdisplay, uint32_t vdisplay);
/** The CVT mode of `hdisplay` by `vdisplay` at `vrefresh` Hz (60 for 0)
 * into `m`, Linux's drm_cvt_mode, with reduced blanking, interlace or
 * margins as asked. 1, or 0 for a size of 0. */
int drm_cvt_mode(int hdisplay, int vdisplay, int vrefresh, int reduced, int interlaced, int margins,
                 struct drm_mode_modeinfo *m);
/** Parse the EDID `edid` of `len` bytes into its modes, in the order Linux
 * adds them, through `add(mode, arg)` for each; the number added, 0 for an
 * EDID that is not valid. For drm_connector_update_edid and its tests. */
int drm_edid_parse_modes(const uint8_t *edid, uint32_t len,
                         int (*add)(const struct drm_mode_modeinfo *mode, void *arg), void *arg);

/* ---- The commit lock and the atomic commit, in drm_atomic.c ---- */

/** Start the commit worker, kdrm, which runs the queued commits, the
 * hotplug events and the drivers' works, once: 0, or -ENOMEM. Process
 * context; `drm_dev_register` calls it, so that an interrupt handler that
 * queues a work only wakes it. */
int32_t drm_atomic_worker_start(void);
/** Take `dev`'s commit lock, its `mode_lock`, sleeping while another
 * thread holds it: 0, or -EINTR for a signal. A holder may take it again,
 * and lets it go as often, as Linux's acquire context takes a modeset lock
 * it holds already; so a handler the dispatcher runs under the lock, or
 * the worker under a work's device's lock, takes it as any caller does. */
int32_t drm_modeset_lock(struct drm_device *dev);
/** Take the commit lock with signals ignored, as Linux's acquire context
 * without DRM_MODESET_ACQUIRE_INTERRUPTIBLE: for a release that must take
 * a framebuffer off the display whatever the caller's signals, and for a
 * driver's commit of its own. */
void drm_modeset_lock_nointr(struct drm_device *dev);
/** Let the commit lock go once, and at the holder's last take wake the
 * next taker. */
void drm_modeset_unlock(struct drm_device *dev);
/** Whether the calling thread holds `dev`'s commit lock. */
int drm_modeset_held(const struct drm_device *dev);

/** A new empty atomic state of `dev`, with ALLOW_MODESET; NULL with no
 * memory. Freed with `drm_atomic_state_free`. */
struct drm_atomic_state *drm_atomic_state_alloc(struct drm_device *dev);
/** Free `state`: the states it holds that no object took, with their
 * references and their unsent events. */
void drm_atomic_state_free(struct drm_atomic_state *state);
/** Free a plane's or a CRTC's state and drop what it holds: its
 * framebuffer, fence and blobs, and a CRTC's unsent event. NULL is
 * allowed. */
void drm_plane_state_destroy(struct drm_plane_state *ps);
void drm_crtc_state_destroy(struct drm_crtc_state *cs);
/** A new FLIP_COMPLETE event of `crtc` with `user_data`, not yet reserved,
 * or NULL with no memory; for the ATOMIC and PAGE_FLIP ioctls. */
struct drm_pending_event *drm_crtc_flip_event(struct drm_crtc *crtc, uint64_t user_data);
/** The commit's state of each object, duplicated from the object's on first
 * use, with the states it depends on (a plane's and a connector's CRTC);
 * NULL with no memory. */
struct drm_crtc_state *drm_atomic_get_crtc_state(struct drm_atomic_state *state,
                                                 struct drm_crtc *crtc);
struct drm_plane_state *drm_atomic_get_plane_state(struct drm_atomic_state *state,
                                                   struct drm_plane *plane);
struct drm_connector_state *drm_atomic_get_connector_state(struct drm_atomic_state *state,
                                                           struct drm_connector *connector);
/** The new and old states of an object in `state`, or NULL when the commit
 * leaves it alone; for the drivers' hooks. */
#define drm_atomic_get_new_crtc_state(s, c)  ((s)->crtcs_new[(c)->index])
#define drm_atomic_get_old_crtc_state(s, c)  ((s)->crtcs_old[(c)->index])
#define drm_atomic_get_new_plane_state(s, p) ((s)->planes_new[(p)->index])
#define drm_atomic_get_old_plane_state(s, p) ((s)->planes_old[(p)->index])
/** Route a plane state to `crtc` (or none), keeping both CRTCs' plane
 * masks. 0 or -ENOMEM. */
int32_t drm_atomic_set_crtc_for_plane(struct drm_atomic_state *state,
                                      struct drm_plane_state *plane_state, struct drm_crtc *crtc);
/** Make a plane state show `fb` (or nothing), moving the reference. */
void drm_atomic_set_fb_for_plane(struct drm_plane_state *plane_state, struct drm_framebuffer *fb);
/** Route a connector state to `crtc` (or none), keeping both CRTCs'
 * connector masks. 0 or -ENOMEM. */
int32_t drm_atomic_set_crtc_for_connector(struct drm_atomic_state *state,
                                          struct drm_connector_state *conn_state,
                                          struct drm_crtc *crtc);
/** Set a CRTC state's mode, through a new MODE_ID blob, and enable it; or
 * clear and disable it for NULL. 0 or -ENOMEM. */
int32_t drm_atomic_set_mode_for_crtc(struct drm_crtc_state *crtc_state,
                                     const struct drm_mode_modeinfo *mode);
/** Set a CRTC state's mode from the MODE_ID blob `blob`, or clear it for
 * NULL: 0, or -EINVAL for a blob that is not one sane mode. */
int32_t drm_atomic_set_mode_prop_for_crtc(struct drm_crtc_state *crtc_state,
                                          struct drm_property_blob *blob);
/** Add every connector routed to `crtc`, and every plane it shows, to
 * `state`. 0 or -ENOMEM. */
int32_t drm_atomic_add_affected_connectors(struct drm_atomic_state *state, struct drm_crtc *crtc);
int32_t drm_atomic_add_affected_planes(struct drm_atomic_state *state, struct drm_crtc *crtc);
/** Set `prop` of `obj` to `value` in `state`, as the atomic ioctl does:
 * 0, -EINVAL for a value the property refuses or a property the object
 * cannot set, -EACCES for a CRTC_ID of no CRTC, -ENOMEM, -EFAULT. */
int32_t drm_atomic_set_property(struct drm_atomic_state *state, struct drm_file *file,
                                struct drm_mode_object *obj, struct drm_property *prop,
                                uint64_t value);
/** The value of a mutable property in an object's current state. 0 or
 * -EINVAL. */
int32_t drm_atomic_get_property(struct drm_mode_object *obj, struct drm_property *prop,
                                uint64_t *value);
/** Whether a CRTC state needs a full modeset: its mode, enable, active or
 * connectors changed. */
#define drm_atomic_crtc_needs_modeset(cs) \
    ((cs)->mode_changed || (cs)->active_changed || (cs)->connectors_changed)
/** Check `state` as Linux's drm_atomic_check_only: the core's checks of each
 * plane, CRTC and connector, the driver's `atomic_check`, and the refusal of
 * a modeset without ALLOW_MODESET. 0 or Linux's error. */
int32_t drm_atomic_check_only(struct drm_atomic_state *state);
/** Check and commit `state`, waiting for it to reach the screen, or, for
 * the nonblocking call, returning before the vblank it waits for. 0, the
 * check's error, or the commit's; `state` stays the caller's to free, and
 * holds nothing of a commit that succeeded. May sleep. */
int32_t drm_atomic_commit(struct drm_atomic_state *state);
int32_t drm_atomic_nonblocking_commit(struct drm_atomic_state *state);
/** Turn every CRTC, plane and connector of `dev` off in one commit, which
 * drops the framebuffers they show, Linux's drm_atomic_helper_shutdown, for
 * a driver's unplug: 0 or the commit's error. May sleep. */
int32_t drm_atomic_helper_shutdown(struct drm_device *dev);
/** The core's check, Linux's drm_atomic_helper_check: routing, modeset
 * marks, the CRTCs' `mode_valid`, then the planes' and CRTCs'
 * `atomic_check`. */
int32_t drm_atomic_helper_check(struct drm_device *dev, struct drm_atomic_state *state);
/**
 * The core's commit, Linux's drm_atomic_helper_commit with its default
 * tail: wait for a flip pending on a CRTC it touches, or for a nonblocking
 * commit answer -EBUSY; for a blocking one wait for the in-fences
 * (-EINTR for a signal); swap the states in; then disable the CRTCs that
 * go off or change mode, update the planes between each CRTC's begin and
 * flush, enable the CRTCs that come on, arm each CRTC's event for its next
 * vblank, or send it now on a CRTC without vblanks or that went off, and
 * free the old states. A blocking commit then waits for its flips, at most
 * 100 ms; a nonblocking one returns before them, and when an in-fence has
 * not signalled it leaves the tail to the commit worker, which runs it once
 * the fences signal. Each CRTC the commit leaves or makes active has an
 * event, the caller's or one of the core's, whose sending ends its pending
 * flip; a legacy cursor update has none and waits for no flip of its own.
 * 0, -EBUSY, -EINTR or -ENOMEM.
 */
int32_t drm_atomic_helper_commit(struct drm_device *dev, struct drm_atomic_state *state,
                                 int nonblock);
/**
 * Clip a plane state to its CRTC as Linux's
 * drm_atomic_helper_check_plane_state: set `src`, `dst` and `visible`.
 * `min_scale` and `max_scale` bound src/dst in 16.16 (DRM_PLANE_NO_SCALING
 * for none); `can_position` lets a visible plane cover less than the CRTC;
 * `can_update_disabled` lets it change on a CRTC that is off. 0, -ERANGE
 * for a scale out of bounds, or -EINVAL.
 */
#define DRM_PLANE_NO_SCALING (1 << 16)
int32_t drm_atomic_helper_check_plane_state(struct drm_plane_state *plane_state,
                                            const struct drm_crtc_state *crtc_state,
                                            int32_t min_scale, int32_t max_scale, int can_position,
                                            int can_update_disabled);

/* A legacy mode set, Linux's drm_mode_set: a CRTC, the framebuffer it shows
 * from (x, y), the mode, or NULL to turn it off, and its connectors. */
struct drm_mode_set {
    struct drm_crtc *crtc;
    struct drm_framebuffer *fb;
    uint32_t x, y;
    const struct drm_mode_modeinfo *mode;
    struct drm_connector **connectors;
    uint32_t num_connectors;
};

/** Commit `set` as Linux's drm_atomic_helper_set_config, the legacy
 * SETCRTC and the console's mode set; the caller holds the commit lock.
 * 0 or the commit's error. May sleep. */
int32_t drm_atomic_helper_set_config(struct drm_mode_set *set);

/** Flush `rect` of `fb` to the display, the commit DIRTYFB makes, for the
 * console's shadow: each plane that shows `fb` gets it as FB_DAMAGE_CLIPS.
 * Takes the commit lock. 0, -ENOSYS for a device whose framebuffers need
 * no flush (`fb_dirty` unset), or the commit's error. May sleep. */
int32_t drm_framebuffer_dirty(struct drm_device *dev, struct drm_framebuffer *fb,
                              const struct drm_mode_rect *rect);
/** Run the queued device work and hotplug events, then advance each
 * nonblocking commit's plane waits in order and finish those whose waits
 * ended. Process context without the big lock or driver locks: it takes the
 * big lock for the device works, the hotplug events and the tails of a
 * device without DRIVER_KMS_UNLOCKED. Returns how many works, events and
 * commit tails ran. May sleep in driver hooks. */
int drm_atomic_commit_work(void);
#ifdef RACH2_HOSTTEST
/* The takes of the big lock `drm_atomic_commit_work` would make, which the
 * host tests count, having no big lock. */
extern int drm_worker_bkl_takes;
#endif
/** Have the commit worker send `connector`'s hotplug event, to user space
 * and the console, as Linux's modeset retry work does after a link failed
 * to train: for a driver's commit, which holds the commit lock the
 * console's answer takes. Once while one waits; any context. */
void drm_connector_hotplug_later(struct drm_connector *connector);

/*
 * A driver's work for the commit worker, which runs `fn` in process context
 * with the big kernel lock held and no commit lock, as Linux's drivers
 * queue work on their own workqueues: a hotplug's probe, which may sleep on
 * the sink and may commit, as Linux's delayed work too. `queued` and `due`
 * are the core's: set while the work waits, and the tick of rtc_ticks_now
 * it waits for, 0 for none; `dev` holds a reference to its device
 * meanwhile.
 */
struct drm_work {
    void (*fn)(struct drm_work *work);
    struct drm_device *dev;
    struct drm_work *next;
    int queued;
    uint64_t due;
};

/** Have the commit worker run `work` once, before the hotplug events it
 * sends; a work already waiting stays queued once, and runs now. Any
 * context. */
void drm_dev_work_later(struct drm_device *dev, struct drm_work *work);

/** The same, `ms` milliseconds from now at the soonest, as Linux's
 * queue_delayed_work; a work already waiting keeps the sooner of its time
 * and this one. Any context. */
void drm_dev_work_later_ms(struct drm_device *dev, struct drm_work *work, uint32_t ms);

/** Take `work` off the worker's queue if it waits; for a driver going
 * away, or an owner that does the work's job itself. 1 when this call took
 * it off, 0 when it was not queued, as when the worker has taken it to run.
 * A caller under the big lock that sees 0 for a work the worker took sees
 * it after its `fn` has begun, and does not wait for it to end, so an `fn`
 * that sleeps holds a reference to its owner. Any context. */
int drm_dev_work_cancel(struct drm_work *work);

/* ---- Vblanks, their events and CRCs, in drm_vblank.c ---- */

/*
 * Linux's drm_vblank.c. A CRTC whose driver gives `enable_vblank` has
 * vblanks; a device has them when any CRTC does. The count rises by one at
 * each `drm_crtc_handle_vblank` while the interrupt is on, and by one more
 * each time the CRTC is turned on, so that a program never sees the same
 * count across a modeset. A holder of vblanks (`drm_crtc_vblank_get`)
 * turns the interrupt on; once the last has let go for
 * DRM_VBLANK_OFFDELAY_MS, the next vblank turns it off, as Linux's
 * drm_vblank_offdelay does. The core's commit turns a CRTC's vblanks off
 * before its `atomic_disable` returns and on after its `atomic_enable`, and
 * arms each commit's event for the vblank after the commit has programmed
 * the CRTC; a driver with its own `atomic_commit` does both itself.
 */
#define DRM_VBLANK_OFFDELAY_MS 5000

/** Whether `crtc` has vblanks: its driver gives `enable_vblank`. */
#define drm_crtc_has_vblank(crtc) ((crtc)->funcs && (crtc)->funcs->enable_vblank)
/** Whether any CRTC of `dev` has vblanks, Linux's drm_dev_has_vblank. */
int drm_dev_has_vblank(const struct drm_device *dev);
/** Give a new CRTC its vblank and CRC records, off, with the CRC source
 * "auto"; from `drm_crtc_init_with_planes`. */
void drm_crtc_vblank_init(struct drm_crtc *crtc);
/** Take a hold on `crtc`'s vblanks, turning the interrupt on for the first:
 * 0, -EINVAL for a CRTC without vblanks or one that is off, or
 * `enable_vblank`'s error. Any context. */
int32_t drm_crtc_vblank_get(struct drm_crtc *crtc);
/** Let a hold go; the off-delay runs from the last. Any context. */
void drm_crtc_vblank_put(struct drm_crtc *crtc);
/** The vblank count of `crtc`, and the time of the last vblank in
 * `*time_ns` when it is not NULL, read as one pair against the vblank
 * interrupt. Any context. */
uint64_t drm_crtc_vblank_count_and_time(struct drm_crtc *crtc, int64_t *time_ns);
/*
 * A driver's scanout position, Linux's get_scanout_position of
 * drm_crtc_helper_funcs: the line `*vpos` and pixel `*hpos` the CRTC
 * scans for the timings `mode`, counted from the end of the vertical
 * blank, so negative inside it, read between the CLOCK_MONOTONIC times
 * `*stime` and `*etime` (drm_vblank_ktime); 1, or 0 when it cannot say.
 */
typedef int (*drm_vblank_get_scanout_position_func)(struct drm_crtc *crtc, int in_vblank_irq,
                                                    int32_t *vpos, int32_t *hpos, int64_t *stime,
                                                    int64_t *etime,
                                                    const struct drm_mode_modeinfo *mode);
/**
 * A `get_vblank_timestamp` from a scanout position, Linux's
 * drm_crtc_vblank_helper_get_vblank_timestamp_internal: the position read
 * up to three times until the reading takes at most 20 us, Linux's
 * drm_timestamp_precision, and `*time_ns` set to the end of that read less
 * the lines and pixels since the end of the vertical blank at `mode`'s
 * clock. 1, or 0 for a mode without a clock or a position not given.
 */
int drm_crtc_vblank_helper_get_vblank_timestamp_internal(
    struct drm_crtc *crtc, int64_t *time_ns, int in_vblank_irq,
    drm_vblank_get_scanout_position_func get_scanout_position,
    const struct drm_mode_modeinfo *mode);
/** CLOCK_MONOTONIC in nanoseconds, Linux's ktime_get, which a scanout
 * position's times are read from. Any context. */
int64_t drm_vblank_ktime(void);
/** Queue the commit event `e` for the next vblank of `crtc`, with the hold
 * the caller took, which the sending lets go. */
void drm_crtc_arm_vblank_event(struct drm_crtc *crtc, struct drm_pending_event *e);
/** Send `e` now, stamped as Linux's drm_crtc_send_vblank_event stamps it:
 * the last vblank's count and time on a device with vblanks, else count 0
 * and the time now. */
void drm_crtc_send_vblank_event(struct drm_crtc *crtc, struct drm_pending_event *e);
/** `drm_crtc_send_vblank_event` with the device's `event_lock` held. */
void drm_crtc_send_vblank_event_locked(struct drm_crtc *crtc, struct drm_pending_event *e);
/** Turn `crtc`'s vblanks off, sending its queued events with the count
 * reached and waking its waiters, and keep them off until
 * `drm_crtc_vblank_on`. */
void drm_crtc_vblank_off(struct drm_crtc *crtc);
/** Let `crtc`'s vblanks come again, counting one, and turn the interrupt
 * on if anyone holds them. */
void drm_crtc_vblank_on(struct drm_crtc *crtc);
/**
 * Wait until `crtc`'s `flip_pending` is below `below`, DRM_FLIP_ARMED for
 * no flip at all and DRM_FLIP_QUEUED for one programmed, or for
 * `timeout_ms`, 0 for no limit; sleeps. 0 once it is, -ETIME at the limit,
 * or -EINTR for a signal when `intr` is set.
 */
int32_t drm_crtc_wait_flip(struct drm_crtc *crtc, int below, int intr, uint32_t timeout_ms);
/** Turn every CRTC of a device being unplugged off, as `drm_dev_unplug`
 * does: each queued event sent, its fence signalled with -ENODEV. */
void drm_vblank_unplug(struct drm_device *dev);
/** Free what `crtc` still holds of events and CRC entries, for
 * `drm_mode_config_cleanup`. */
void drm_crtc_vblank_cleanup(struct drm_crtc *crtc);

/*
 * The CRC files of debugfs (76-18b), Linux's drm_debugfs_crc.c, on kernel
 * buffers: crc/control's write chooses a source, crc/data's open starts it
 * and its release stops it, and each read takes one entry as a line.
 */

/** crc/control's write of `len` bytes at `buf`: the source named, a final
 * newline dropped, which the driver must know. `len`, 0 for 0, -EINVAL for
 * a CRTC without CRCs or a source the driver refuses, -E2BIG past a page
 * less a byte, or -EBUSY while crc/data is open. */
int32_t drm_crtc_crc_set_source(struct drm_crtc *crtc, const char *buf, uint32_t len);
/** crc/data's open: 0, -EIO while the CRTC is off, -EINVAL for a CRTC
 * without CRCs or a source the driver refuses, -EBUSY while it is open
 * already, -ENOMEM, or `set_crc_source`'s error. Takes the commit lock;
 * -EINTR for a signal while waiting for it. */
int32_t drm_crtc_crc_open(struct drm_crtc *crtc);
/** crc/data's release: stop the source and drop the entries. */
void drm_crtc_crc_release(struct drm_crtc *crtc);
/** crc/data's read: the oldest entry as Linux writes it, "0x%08x" frame or
 * ten X's, then " 0x%08x" for each value, a newline and a NUL, into `buf`
 * of `count` bytes, waiting for one unless `nonblock`; sleeps. Returns the
 * line's length with its NUL, 0 for a CRTC without CRCs, -EAGAIN, -EINTR,
 * or -EINVAL for a buffer shorter than a line. */
int32_t drm_crtc_crc_read(struct drm_crtc *crtc, char *buf, uint32_t count, int nonblock);
/** Whether crc/data has an entry to read, for its poll. */
int drm_crtc_crc_ready(const struct drm_crtc *crtc);

/** debugfs's `force` write of a connector, Linux's connector_write: "on",
 * "digital", "off" or "unspecified", with or without a newline, up to a
 * NUL, as sysfs_streq takes them, set its DRM_FORCE_* for the next probe.
 * `len`, or -EINVAL past eleven bytes or for another word. (drm_mode.c) */
int32_t drm_connector_force_write(struct drm_connector *connector, const char *buf, uint32_t len);
/** debugfs's `edid_override` write, Linux's drm_edid_override_set: keep a
 * copy of the `len` bytes at `edid` as the connector's EDID from the next
 * probe on: 0, -EINVAL for bytes that are not a valid EDID of the blocks
 * they name, or -ENOMEM. (drm_edid.c) */
int32_t drm_edid_override_set(struct drm_connector *connector, const void *edid, uint32_t len);
/** Drop the override, for the write "reset". (drm_edid.c) */
void drm_edid_override_reset(struct drm_connector *connector);

/* ------------------------------------------------------------------------
 * Devices
 */

/*
 * A device a driver has bound. `features` are the driver's identity's as
 * this device has them, since a driver may lose one on a device (virtio-gpu
 * without 3D keeps its syncobjs; a driver without display drops
 * DRIVER_MODESET and DRIVER_ATOMIC). `index` is N of cardN, set at
 * registration. `unique` is GET_UNIQUE's answer: "pci:DDDD:BB:DD.F" for a
 * PCI device and the platform device's name for the rest
 * ("simple-framebuffer.0", "vkms"). `pci` is the PCI function, for
 * /sys's ids, or NULL. `pci_ident`, or NULL, is the identity /sys reports
 * for that function in place of its own, as a driver that stands in for
 * another chip sets it before registering (sysfs.h's
 * `sysfs_pci_virtual_add`). `master` is the file that holds the master role,
 * or NULL. `mode_lock`, the commit lock, keeps the mode state, and
 * `object_lock`, `event_lock` and `vbl_lock` keep the objects, the events
 * and the vblanks (the lock rules above).
 */
struct drm_device {
    const struct drm_driver *driver;
    uint32_t features;
    int index;
    volatile uint32_t refs; /* changed atomically, as each object holds one */
    int registered;
    int unplugged;
    char unique[32];
    const pci_device_t *pci;
    const struct sysfs_pci_ident *pci_ident;
    /* The directory of /sys the device's `drm` goes below, a handle of
     * sysfs.h set by the driver, as virtio-gpu sets its virtio device's;
     * 0 for its PCI function's, or a platform device's without one. */
    int sysfs_parent;
    struct drm_file *master;
    uint32_t open_count;
    struct drm_file *files;              /* its open files, through `dev_next` */
    uint32_t next_master_id;             /* the last master record's id */
    struct spinlock object_lock;         /* the three lists below and their objects' names */
    struct drm_gem_object *objects;      /* its buffer objects, through `dev_next` */
    struct drm_gem_object *mmap_objects; /* those with an mmap offset, by offset */
    struct drm_mmap_node *mmap_nodes;    /* drivers' further offsets, by offset */
    struct spinlock event_lock;          /* its files' events, its CRTCs' queued events and flips */
    struct spinlock vbl_lock;            /* its CRTCs' vblank counts, holds and interrupts */
    struct drm_mode_config mode_config;
    int debugfs;              /* its debugfs directory is made (drm_debugfs.c) */
    struct mutex mode_lock;   /* the commit lock, `drm_modeset_lock`'s */
    uint32_t mode_lock_depth; /* the holder's takes past its first */
    void *driver_priv;
};

/* Whether a device has the feature `f`, a DRIVER_* bit. */
#define drm_core_check_feature(dev, f) (((dev)->features & (f)) != 0)

/** Probe and register the display devices, once, after the PCI drivers
 * have started: in the order the aperture handoff needs, bochs and
 * virtio-gpu, then fbdrm on what they left, then vkms when `rach2.vkms`
 * asks and vgem when `rach2.vgem` asks; and /dev/udmabuf. Logs "drm: start". */
void drm_start(void);

/** Fill a new device for `driver`, with one reference, the caller's, and
 * the driver's features; its spinlocks are made and every other member is
 * zero, and the driver sets `unique`, `pci` and its own before it registers.
 * Registers nothing. */
void drm_dev_init(struct drm_device *dev, const struct drm_driver *driver);
/** Make the spinlocks of a new file's two handle tables, a class for each,
 * as Linux's drm_gem_open and drm_syncobj_open make `table_lock` and
 * `syncobj_table_lock`. */
void drm_file_locks_init(struct drm_file *file);
/** Register the device under the lowest free card index: its card node,
 * and its render node when it has DRIVER_RENDER, through the devfs
 * registry, with GET_UNIQUE's name made from `pci` or the driver's name
 * when the driver left `unique` empty; then make its /sys entries
 * (`drm_sysfs_register`) and tell the console (`drm_client_dev_register`).
 * 0, -ENOSPC past DRM_MAX_DEVICES, or the registry's error, with nothing
 * registered. The index stays the device's until its record is released. */
int32_t drm_dev_register(struct drm_device *dev);
/** Unplug the device: the device loss above, its /sys entries removed
 * (`drm_sysfs_unregister`). Any later ioctl of its files answers ENODEV;
 * the record stays until its last holder lets go. */
void drm_dev_unplug(struct drm_device *dev);
/** Zap every program's mapping of `obj` and of each import of its dma-buf
 * on any device that maps the import under its own address, as the shmem
 * helper's import does, so that the next touch of one asks for its page
 * again. i915 maps an import under its mmap-offset records, which this
 * does not reach. Under the big kernel lock; it may sleep for a space's
 * regions' lock (`paging_zap_device`), so the caller holds `obj`. */
void drm_gem_object_zap(struct drm_gem_object *obj);
/** Take and drop a reference to a device. */
void drm_dev_get(struct drm_device *dev);
void drm_dev_put(struct drm_device *dev);
/** The registered device of card index `index`, or NULL; for debugfs and
 * /sys, which walk the devices by index. */
struct drm_device *drm_dev_at(int index);

/*
 * A listener of the master role: the console (drm_client.c), which gives
 * up the screen while a program holds the role, and the seat's revoke
 * path. Its `changed` runs after each change of a device's master, with
 * the file that holds the role now or NULL when none does: SET_MASTER,
 * DROP_MASTER, the first open of a card with no master, and the release of
 * the master's file by close, exec or exit, after the file's framebuffers
 * have left the display. It runs in the caller's process context and may
 * sleep, for a commit; the role may change again meanwhile, which runs the
 * listeners again.
 */
struct drm_master_notifier {
    void (*changed)(struct drm_device *dev, struct drm_file *master);
    struct drm_master_notifier *next;
};

/** Add `n` to the listeners; it stays until removed. */
void drm_master_notifier_register(struct drm_master_notifier *n);
/** Take `n` from the listeners; nothing for one not added. */
void drm_master_notifier_unregister(struct drm_master_notifier *n);
/** Run every listener for `dev`'s new master `master`, or NULL. */
void drm_master_changed(struct drm_device *dev, struct drm_file *master);

/** Release what `file` holds of the master role as its file is released:
 * its magic, and the role, which the listeners are told of. */
void drm_master_release(struct drm_file *file);
/** Give `file`, just opened on a card, its master record: the role and a
 * record of its own when the device has no master, else the master's. */
void drm_master_open(struct drm_file *file);

/* ------------------------------------------------------------------------
 * The firmware framebuffer's handoff (aperture)
 */

/*
 * fbdrm drives the framebuffer the firmware set up, until a native driver
 * takes the same memory. The firmware driver acquires its range with
 * `drm_aperture_acquire_firmware`, giving the device and the hook that
 * removes it. A native driver, before it touches its hardware, calls
 * `drm_aperture_remove_conflicting` over every memory range its device
 * decodes, its BARs as it sized them: each firmware device whose range
 * overlaps is unplugged through its hook, and the console is told to leave
 * it (`drm_client_dev_unregister`) so that it moves to the native device
 * when that registers. The native driver
 * registers after the removal, so the card the console and programs find
 * is the native one. bochs, virtio-vga and i915 do this; fbdrm, on a
 * machine with none of them, keeps the screen. The native drivers probe
 * first at boot (`drm_start`), and a removal closes the table to firmware
 * devices, so fbdrm does not bind behind a native driver either.
 */
#define DRM_APERTURE_MAX 4

/** Record that `dev` drives the firmware framebuffer at physical `base`,
 * `size` bytes, and is removed through `remove`. 0, or -EBUSY when
 * another firmware device has the range, the table is full, or a native
 * driver has already removed the firmware devices, after which none binds,
 * as Linux's sysfb_disable. */
int32_t drm_aperture_acquire_firmware(struct drm_device *dev, uint64_t base, uint64_t size,
                                      void (*remove)(struct drm_device *dev));
/** Forget the ranges `dev` acquired, for a firmware driver whose bind
 * failed after it acquired them; the removal hook is not called. */
void drm_aperture_release_firmware(struct drm_device *dev);
/** Whether `dev` drives the firmware's framebuffer, having acquired a range
 * that no native driver has removed yet. */
int drm_aperture_is_firmware(const struct drm_device *dev);
/** Remove every firmware device whose range overlaps [base, base + size),
 * for the native driver `name`, which the log names. 0. */
int32_t drm_aperture_remove_conflicting(uint64_t base, uint64_t size, const char *name);

/* ------------------------------------------------------------------------
 * Helpers the drivers call
 */

/** Count a vblank of `crtc` and stamp it, wake its waiters and send its
 * due events; from the driver's vblank interrupt or timer, in any context.
 * When nobody has held vblanks for the off-delay it turns them off through
 * `disable_vblank`. 1 while vblanks are on, 0 when they are off. */
int drm_crtc_handle_vblank(struct drm_crtc *crtc);
/** Queue one frame's CRC values, `crc.values` of them, for crc/data, from
 * the driver's vblank path, in any context: 0, -EINVAL while crc/data is
 * not open, or -ENOBUFS when the reader has left no room. */
int32_t drm_crtc_add_crc_entry(struct drm_crtc *crtc, int has_frame, uint32_t frame,
                               const uint32_t *crcs);
/** Set `connector`'s EDID property and size in millimetres from the sink's
 * `edid` of `len` bytes, or from its override, cleaned as Linux cleans it,
 * and add the modes it names to the probed modes; the number added. NULL
 * `edid` with no override clears the property. */
int drm_connector_update_edid(struct drm_connector *connector, const uint8_t *edid, uint32_t len);

/** The VIC of the CEA-861 mode `mode` is, Linux's drm_match_cea_mode: its
 * timings, a clock of the same period in picoseconds as the table's or its
 * 1000/1001 one, and its picture aspect when the mode names one; 0 for
 * none. (drm_edid.c) */
uint32_t drm_match_cea_mode(const struct drm_mode_modeinfo *mode);

/** The HDMI VIC (1 to 4) of the 4K mode `mode` is, Linux's
 * drm_match_hdmi_mode; 0 for none. */
uint32_t drm_match_hdmi_mode(const struct drm_mode_modeinfo *mode);

/* What a sink's EDID says of it as HDMI, Linux's drm_display_info as the
 * Intel driver reads it: `is_hdmi` for an HDMI vendor block in a CTA
 * extension, else DVI; `bpc` the bits a colour it takes, Linux's
 * display_info.bpc, 0 for unknown; `dc_30` and `dc_36` for its 10 and 12
 * bits a colour in RGB;
 * `max_tmds_clock` its most TMDS clock in kHz, 0 when it names none;
 * `has_audio` for basic audio; `digital` for a digital input;
 * `has_hdmi_infoframe` for a sink that takes HDMI's vendor infoframe;
 * `rgb_quant_selectable` for a Q bit the sink honours (its video
 * capability block's QS); `hdmi2` for an HDMI 2.0 sink, one with SCDC or
 * YCbCr 4:2:0; `vics` a bit for each VIC of its video data blocks. */
struct drm_edid_hdmi_info {
    int is_hdmi;
    uint32_t bpc;
    int dc_30;
    int dc_36;
    uint32_t max_tmds_clock;
    int has_audio;
    int digital;
    int has_hdmi_infoframe;
    int rgb_quant_selectable;
    int hdmi2;
    uint8_t vics[32];
};

/** Read `info` from the EDID `edid` of `len` bytes, or from `connector`'s
 * override when it has one and `connector` is not NULL; all 0 for an EDID
 * that is not valid. */
void drm_edid_hdmi_info(const struct drm_connector *connector, const uint8_t *edid, uint32_t len,
                        struct drm_edid_hdmi_info *info);

/** Whether `mode` takes limited range RGB by default, Linux's
 * drm_default_rgb_quant_range: every CEA mode but VIC 1. */
int drm_default_rgb_quant_limited(const struct drm_mode_modeinfo *mode);

/* The most bytes of an infoframe packed: the header of 3, the checksum and
 * 27 of payload. */
#define DRM_INFOFRAME_MAX 31

/**
 * Pack the AVI infoframe of `mode` for the sink `info` into `buf`, Linux's
 * drm_hdmi_avi_infoframe_from_display_mode, its quantization range for RGB
 * of `limited` range, and hdmi_avi_infoframe_pack: RGB, underscan, the
 * picture's active aspect, graphics. The bytes packed, or 0 for a mode
 * whose aspect the frame cannot carry.
 */
uint32_t drm_hdmi_avi_infoframe(const struct drm_edid_hdmi_info *info,
                                const struct drm_mode_modeinfo *mode, int limited, uint8_t *buf);

/** Pack HDMI's vendor infoframe of `mode` into `buf`, Linux's
 * drm_hdmi_vendor_infoframe_from_display_mode and its pack: the HDMI VIC
 * of a 4K mode; 0 bytes for a sink that takes no such frame. */
uint32_t drm_hdmi_vendor_infoframe(const struct drm_edid_hdmi_info *info,
                                   const struct drm_mode_modeinfo *mode, uint8_t *buf);

/** Pack the source product infoframe of `vendor`, `product` and the
 * source type `sdi` into `buf`, Linux's hdmi_spd_infoframe_init and its
 * pack; the bytes packed. */
uint32_t drm_hdmi_spd_infoframe(const char *vendor, const char *product, uint8_t sdi, uint8_t *buf);
/** Tell user space that `dev`'s outputs changed: the uevent `change` of
 * its card with HOTPLUG=1, which compositors listen for, and the console
 * (`drm_client_dev_hotplug`). Process context; may sleep. (drm_sysfs.c) */
void drm_kms_hotplug_event(struct drm_device *dev);
/** The same for one connector, with CONNECTOR=its object's ID, as Linux's
 * drm_sysfs_connector_hotplug_event sends after a probe of one output. */
void drm_sysfs_connector_hotplug_event(struct drm_connector *connector);

/* ------------------------------------------------------------------------
 * The device in /sys, in drm_sysfs.c
 */

/*
 * A registered device has Linux's entries: below its PCI function's
 * directory, or below /sys/devices/platform/UNIQUE for a platform device,
 * a `drm` directory holding cardN and, with DRIVER_RENDER, renderD(128+N),
 * each with `dev`, `uevent` and the links `subsystem` and `device`, and
 * below cardN a directory cardN-NAME for each connector with `status`,
 * `enabled`, `dpms`, `modes`, `edid`, `connector_id` and `uevent`. Each
 * node and connector has its link in /sys/class/drm, and each node its
 * link MAJOR:MINOR in /sys/dev/char. Each comes with the uevent `add` and
 * goes with `remove`. A device with no PCI function behind it that reports
 * one (`sysfs_pci_virtual_add`, ../fs/sysfs.h) sets `pci` as a real one
 * does. A driver adds files of its own below `drm_sysfs_card_dir` through
 * ../fs/sysfs.h.
 */

/** Make the entries of `dev`, whose `index` is set, and send their `add`
 * uevents; called by `drm_dev_register`. 0, or -errno, with nothing left
 * made. */
int32_t drm_sysfs_register(struct drm_device *dev);
/** Send the `remove` uevents of `dev` and remove its entries, and the
 * platform device's directory when its registration made it. */
void drm_sysfs_unregister(struct drm_device *dev);
/** Add the entries of a connector of a registered device, by its `index`,
 * and send its `add`; `drm_sysfs_register` adds those the device has, and
 * a driver calls this for one that comes later. 0, or -errno. */
int32_t drm_sysfs_connector_add(struct drm_connector *connector);
/** Remove a connector's entries, after its `remove`. */
void drm_sysfs_connector_remove(struct drm_connector *connector);
/** The handle (../fs/sysfs.h) of the cardN directory of `dev`, or 0. */
int drm_sysfs_card_dir(const struct drm_device *dev);

/* ------------------------------------------------------------------------
 * The device in debugfs, in drm_debugfs.c
 */

/** Make debugfs's `dri` directory, as Linux does when DRM loads; called by
 * `drm_start`. */
void drm_debugfs_init(void);
/** Make the files of `dev`, whose `index` is set, in debugfs, as Linux
 * 6.12 does: `dri/DIR`, DIR the name of the PCI function ("0000:00:02.0")
 * or the platform device, with `name`, `clients`, `gem_names`,
 * `framebuffer`, `state`, `crtc-N/crc/control` and `crc/data`,
 * `encoder-N/bridges` and each connector's `force` and `edid_override`, as
 * the device has them; and the links `dri/N` and `dri/(128 + N)` to it.
 * Every node holds a reference to the device, so its CRTCs and connectors
 * outlive every open of their files. Called by `drm_dev_register`; a
 * failure is logged and leaves the device without files, as debugfs never
 * fails a driver on Linux. */
void drm_debugfs_register(struct drm_device *dev);
/** Take `dev`'s files and links out of debugfs; for `drm_dev_unplug`. */
void drm_debugfs_unregister(struct drm_device *dev);
/** The path from debugfs's root of the file `name` in `dev`'s directory,
 * `dri/DIR/name`, into the `cap` bytes at `path`, cut to fit: where a
 * driver makes its own files, after `drm_dev_register`, as Linux's drivers
 * make theirs in `dev->debugfs_root`. `dri/N` is a link, which no path
 * that makes a file may pass. */
void drm_debugfs_path(const struct drm_device *dev, const char *name, char *path, uint32_t cap);

/* ------------------------------------------------------------------------
 * The console as a DRM client, in drm_client.c
 */

/** A device registered: the console takes it if it has none, which is how
 * a native driver's device that replaced the firmware's comes to it. Only a
 * device with mode setting and dumb buffers is taken. */
void drm_client_dev_register(struct drm_device *dev);
/** A device is going: the console leaves it, and gets the keyboard and
 * /fb back if a master of it held the screen. Does not sleep. */
void drm_client_dev_unregister(struct drm_device *dev);
/** `dev`'s outputs changed: while no master holds the console's device, the
 * console sets its mode again on the outputs connected now. Process
 * context; may sleep. */
void drm_client_dev_hotplug(struct drm_device *dev);
/** The loader's framebuffer has gone, with the device whose aperture
 * showed it, which its driver gives to another owner: a console still on
 * it draws nothing from now on, and /fb's native mode has no screen, until
 * a device registers, on which the console then sets its own mode. Nothing
 * for a console on a buffer of its own. Does not sleep. */
void drm_client_boot_screen_gone(void);

/* ------------------------------------------------------------------------
 * The drivers' probes. Each finds its devices, binds and registers them,
 * and returns how many it bound, or -errno. bochs, i915 and fbdrm reach
 * PC-only code and are called through `arch_drm_probe` (sys/arch/arch.h).
 */

/** QEMU's standard VGA and bochs display, as Linux's bochs-drm. */
int32_t bochs_probe(void);
/** The framebuffer the loader left, as Linux's simpledrm. */
int32_t fbdrm_probe(void);
/** Intel's graphics, when `rach2.i915` asks for it; see i915/Readme.md. */
int32_t i915_probe(void);
/** The i915 devices' timer, from the boot processor's tick or its service
 * event with interrupts off: forcewake's release once its tick has come
 * (`i915_uncore_tick`). Nothing while no device is bound. Its deadline is
 * `i915_idle_ticks`, in idle_timer.h. */
void i915_tick(void);
/** The virtio GPUs on virtio-pci, and the driver registered for one bound
 * later through /sys/bus/pci/drivers/virtio-pci/bind. */
int32_t virtio_gpu_probe(void);
struct virtio_dev;
/** Bind virtio-gpu to `vdev`, a device of a transport other than virtio-pci,
 * such as the shared-memory transport's (../virtio/shmem.c), once its device
 * has answered; 0 or -errno. See virtio_gpu.c. */
int32_t virtio_gpu_bind(struct virtio_dev *vdev);
/** vkms, a display in software, when the command line has `rach2.vkms`. */
int32_t vkms_probe(void);
/** vgem, buffers in memory with no display, with `rach2.vgem`. */
int32_t vgem_probe(void);
/** vkms's vblank timer, from the boot processor's tick with interrupts
 * off: each vblank due on its CRTC since the last call. Nothing while no
 * vkms device holds vblanks. */
void vkms_tick(void);

/* ------------------------------------------------------------------------
 * The ioctl handlers of the core's table, one per number, in the files the
 * comments below name. Each takes the device, the argument the dispatcher
 * copied in and copies back out, and the caller's file, and answers 0 or
 * -errno; one for a feature the device lacks answers -EOPNOTSUPP, as Linux
 * does.
 */

/* drm.c (76-4a): the device's identity and the client's view. */
int32_t drm_version(struct drm_device *dev, void *data, struct drm_file *file);
int32_t drm_getunique(struct drm_device *dev, void *data, struct drm_file *file);
int32_t drm_getclient(struct drm_device *dev, void *data, struct drm_file *file);
int32_t drm_getstats(struct drm_device *dev, void *data, struct drm_file *file);
int32_t drm_getcap(struct drm_device *dev, void *data, struct drm_file *file);
int32_t drm_setclientcap(struct drm_device *dev, void *data, struct drm_file *file);
int32_t drm_setversion(struct drm_device *dev, void *data, struct drm_file *file);
/** SET_UNIQUE: -EINVAL, as Linux 6.12. */
int32_t drm_invalid_op(struct drm_device *dev, void *data, struct drm_file *file);
/** BLOCK, UNBLOCK, ADD_DRAW, RM_DRAW, FINISH, UPDATE_DRAW, ATTACHMODE and
 * DETACHMODE: 0, as Linux 6.12. */
int32_t drm_noop(struct drm_device *dev, void *data, struct drm_file *file);

/* drm_auth.c (76-4a): authentication and the master role. */
int32_t drm_getmagic(struct drm_device *dev, void *data, struct drm_file *file);
int32_t drm_authmagic(struct drm_device *dev, void *data, struct drm_file *file);
int32_t drm_setmaster_ioctl(struct drm_device *dev, void *data, struct drm_file *file);
int32_t drm_dropmaster_ioctl(struct drm_device *dev, void *data, struct drm_file *file);

/* drm_gem.c (76-4a): handles, names and dumb buffers. */
int32_t drm_gem_close_ioctl(struct drm_device *dev, void *data, struct drm_file *file);
int32_t drm_gem_flink_ioctl(struct drm_device *dev, void *data, struct drm_file *file);
int32_t drm_gem_open_ioctl(struct drm_device *dev, void *data, struct drm_file *file);
int32_t drm_mode_create_dumb_ioctl(struct drm_device *dev, void *data, struct drm_file *file);
int32_t drm_mode_mmap_dumb_ioctl(struct drm_device *dev, void *data, struct drm_file *file);
int32_t drm_mode_destroy_dumb_ioctl(struct drm_device *dev, void *data, struct drm_file *file);

/* drm_prime.c (76-4b): export and import through dma-bufs. */
int32_t drm_prime_handle_to_fd_ioctl(struct drm_device *dev, void *data, struct drm_file *file);
int32_t drm_prime_fd_to_handle_ioctl(struct drm_device *dev, void *data, struct drm_file *file);

/* drm_mode.c (76-5a): mode objects, properties, blobs, framebuffers and
 * leases. */
int32_t drm_mode_getresources(struct drm_device *dev, void *data, struct drm_file *file);
int32_t drm_mode_getplane_res(struct drm_device *dev, void *data, struct drm_file *file);
int32_t drm_mode_getcrtc(struct drm_device *dev, void *data, struct drm_file *file);
int32_t drm_mode_getplane(struct drm_device *dev, void *data, struct drm_file *file);
int32_t drm_mode_getencoder(struct drm_device *dev, void *data, struct drm_file *file);
int32_t drm_mode_getconnector(struct drm_device *dev, void *data, struct drm_file *file);
int32_t drm_mode_gamma_get_ioctl(struct drm_device *dev, void *data, struct drm_file *file);
int32_t drm_mode_getproperty_ioctl(struct drm_device *dev, void *data, struct drm_file *file);
int32_t drm_mode_getblob_ioctl(struct drm_device *dev, void *data, struct drm_file *file);
int32_t drm_mode_createblob_ioctl(struct drm_device *dev, void *data, struct drm_file *file);
int32_t drm_mode_destroyblob_ioctl(struct drm_device *dev, void *data, struct drm_file *file);
int32_t drm_mode_obj_get_properties_ioctl(struct drm_device *dev, void *data,
                                          struct drm_file *file);
int32_t drm_mode_getfb(struct drm_device *dev, void *data, struct drm_file *file);
int32_t drm_mode_getfb2_ioctl(struct drm_device *dev, void *data, struct drm_file *file);
int32_t drm_mode_addfb_ioctl(struct drm_device *dev, void *data, struct drm_file *file);
int32_t drm_mode_addfb2_ioctl(struct drm_device *dev, void *data, struct drm_file *file);
int32_t drm_mode_rmfb_ioctl(struct drm_device *dev, void *data, struct drm_file *file);
int32_t drm_mode_closefb_ioctl(struct drm_device *dev, void *data, struct drm_file *file);
int32_t drm_mode_create_lease_ioctl(struct drm_device *dev, void *data, struct drm_file *file);
int32_t drm_mode_list_lessees_ioctl(struct drm_device *dev, void *data, struct drm_file *file);
int32_t drm_mode_get_lease_ioctl(struct drm_device *dev, void *data, struct drm_file *file);
int32_t drm_mode_revoke_lease_ioctl(struct drm_device *dev, void *data, struct drm_file *file);

/* drm_legacy.c (76-5a): the calls before atomic, made atomic commits. */
int32_t drm_mode_setcrtc(struct drm_device *dev, void *data, struct drm_file *file);
int32_t drm_mode_setplane(struct drm_device *dev, void *data, struct drm_file *file);
int32_t drm_mode_cursor_ioctl(struct drm_device *dev, void *data, struct drm_file *file);
int32_t drm_mode_cursor2_ioctl(struct drm_device *dev, void *data, struct drm_file *file);
int32_t drm_mode_gamma_set_ioctl(struct drm_device *dev, void *data, struct drm_file *file);
int32_t drm_connector_property_set_ioctl(struct drm_device *dev, void *data, struct drm_file *file);
int32_t drm_mode_obj_set_property_ioctl(struct drm_device *dev, void *data, struct drm_file *file);
int32_t drm_mode_page_flip_ioctl(struct drm_device *dev, void *data, struct drm_file *file);
int32_t drm_mode_dirtyfb_ioctl(struct drm_device *dev, void *data, struct drm_file *file);

/* drm_atomic.c (76-5a, then 76-5b): the atomic commit. */
int32_t drm_mode_atomic_ioctl(struct drm_device *dev, void *data, struct drm_file *file);

/* drm_vblank.c (76-5b): vblank waits and sequences. */
int32_t drm_wait_vblank_ioctl(struct drm_device *dev, void *data, struct drm_file *file);
int32_t drm_crtc_get_sequence_ioctl(struct drm_device *dev, void *data, struct drm_file *file);
int32_t drm_crtc_queue_sequence_ioctl(struct drm_device *dev, void *data, struct drm_file *file);

/* ------------------------------------------------------------------------
 * Sync objects, in drm_syncobj.c (76-3b)
 *
 * A syncobj holds a fence: none, a binary fence, or for a timeline the
 * newest point of a chain of points (`struct dma_fence_chain`, dmabuf.h),
 * each of whose numbers covers the points below it. It is held by each
 * handle any file has to it, by each syncobj file and by each thread that
 * waits on it, and holds its fence. A driver signals one by giving it a
 * fence (`drm_syncobj_replace_fence`) or a point (`drm_syncobj_add_point`)
 * and waits on it through `drm_syncobj_find_fence`. Everything here runs
 * under the big kernel lock; what may sleep says so.
 */
struct syncobj_wait;
struct syncobj_eventfd;
struct dma_fence_chain;
struct drm_syncobj {
    volatile uint32_t refs;           /* atomic */
    struct spinlock lock;             /* the three members below */
    struct dma_fence *fence;          /* NULL, a binary fence or the newest point */
    struct syncobj_wait *waits;       /* threads waiting for a point to be submitted */
    struct syncobj_eventfd *eventfds; /* eventfds waiting for a point to be submitted */
};

/** A new syncobj with one reference, the caller's, signalled at once with
 * DRM_SYNCOBJ_CREATE_SIGNALED in `flags`; NULL with no memory. */
struct drm_syncobj *drm_syncobj_create(uint32_t flags);
/** Take and drop a reference; the last frees the syncobj, its fence and
 * the eventfds still waiting on it. drm_syncobj_put accepts NULL. */
void drm_syncobj_get(struct drm_syncobj *syncobj);
void drm_syncobj_put(struct drm_syncobj *syncobj);
/** The syncobj of `file`'s `handle`, with a reference, or NULL. */
struct drm_syncobj *drm_syncobj_find(struct drm_file *file, uint32_t handle);
/** The syncobj's fence, with a reference, or NULL, as Linux's
 * drm_syncobj_fence_get. */
struct dma_fence *drm_syncobj_fence_get(struct drm_syncobj *syncobj);
/** Make `fence`, which may be NULL, the syncobj's, with a reference of its
 * own, and tell its waiters and eventfds. Does not sleep. */
void drm_syncobj_replace_fence(struct drm_syncobj *syncobj, struct dma_fence *fence);
/** Add the point `point` of `fence` to the timeline in `chain`, a record
 * of dma_fence_chain_alloc that the syncobj takes, as Linux's
 * drm_syncobj_add_point: the point follows the syncobj's fence, and the
 * signalled points just before it are dropped. Does not sleep. */
void drm_syncobj_add_point(struct drm_syncobj *syncobj, struct dma_fence_chain *chain,
                           struct dma_fence *fence, uint64_t point);
/**
 * The fence of `point` of `file`'s syncobj `handle` (0 for its newest), as
 * Linux's drm_syncobj_find_fence, with a reference, in `*fence`: the
 * signalled stub for a point gone by. With
 * DRM_SYNCOBJ_WAIT_FLAGS_WAIT_FOR_SUBMIT in `flags` it waits up to five
 * seconds for the point to be submitted, and may sleep. 0, -ENOENT for no
 * such handle, -EINVAL for any other flag or a point not submitted, -ETIME
 * or -EINTR.
 */
int32_t drm_syncobj_find_fence(struct drm_file *file, uint32_t handle, uint64_t point,
                               uint32_t flags, struct dma_fence **fence);
/** Drop every syncobj handle of `file`, as its release does. */
void drm_syncobj_release(struct drm_file *file);

/* The ioctls, Linux's: each answers EOPNOTSUPP on a device without
 * DRIVER_SYNCOBJ, and the timeline ones (TRANSFER, TIMELINE_WAIT, EVENTFD,
 * TIMELINE_SIGNAL and QUERY) without DRIVER_SYNCOBJ_TIMELINE. */
int32_t drm_syncobj_create_ioctl(struct drm_device *dev, void *data, struct drm_file *file);
int32_t drm_syncobj_destroy_ioctl(struct drm_device *dev, void *data, struct drm_file *file);
int32_t drm_syncobj_handle_to_fd_ioctl(struct drm_device *dev, void *data, struct drm_file *file);
int32_t drm_syncobj_fd_to_handle_ioctl(struct drm_device *dev, void *data, struct drm_file *file);
int32_t drm_syncobj_transfer_ioctl(struct drm_device *dev, void *data, struct drm_file *file);
int32_t drm_syncobj_wait_ioctl(struct drm_device *dev, void *data, struct drm_file *file);
int32_t drm_syncobj_timeline_wait_ioctl(struct drm_device *dev, void *data, struct drm_file *file);
int32_t drm_syncobj_eventfd_ioctl(struct drm_device *dev, void *data, struct drm_file *file);
int32_t drm_syncobj_reset_ioctl(struct drm_device *dev, void *data, struct drm_file *file);
int32_t drm_syncobj_signal_ioctl(struct drm_device *dev, void *data, struct drm_file *file);
int32_t drm_syncobj_timeline_signal_ioctl(struct drm_device *dev, void *data,
                                          struct drm_file *file);
int32_t drm_syncobj_query_ioctl(struct drm_device *dev, void *data, struct drm_file *file);

#endif /* _DRM_H_ */
