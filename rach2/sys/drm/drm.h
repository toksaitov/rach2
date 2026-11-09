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
#include "../fs/fd.h"
#include "../mm/shmem.h"
#include "../mm/vm.h"
#include "../pci.h"
#include "../proc/wait_queue.h"
#include "uapi.h"

struct sysfs_pci_ident;

/*
 * rach2/sys/drm/Readme.md tells the design; this header is its contract.
 * The rules every file of sys/drm/ and sys/dmabuf/ keeps:
 *
 * Locking. Everything here runs under the big kernel lock, which a system
 * call, an interrupt and a fault take on entry, and which a sleep lets go.
 * So what changes between two sleeps is atomic, and the only rule is what
 * may change across a sleep. A function that may sleep says so: the waits
 * on fences and vblanks, a blocking atomic commit, the copies to and from
 * user memory and a driver's commands to its device. Before sleeping, a
 * caller takes a reference on every object it uses after the sleep; an
 * object it found by ID or handle is found again or held, never assumed.
 * A device's `commit_lock`, a sleeping lock, orders commits on one device:
 * a commit takes it before it reads the state it replaces and keeps it
 * until the new state is the device's. An interrupt handler takes no
 * sleeping lock and may only signal fences, count vblanks and queue
 * events. The order, outermost first, is a device's `commit_lock`, then
 * any lock a driver keeps for its own hardware.
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
 * reads give the events already queued and then ENODEV, poll answers
 * POLLERR | POLLHUP, every mapping of its buffers is revoked so that a
 * later access raises SIGBUS (76-2a), and the driver signals every fence
 * it still owes with the error -ENODEV. The records stay until their
 * last holder lets go.
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

/* The five drivers' identities as Linux 6.12 reports them (i915_driver.c
 * and i915_driver.h, virtgpu_drv.c and virtgpu_drv.h, tiny/bochs.c,
 * tiny/simpledrm.c and vkms/vkms_drv.c). fbdrm reports itself as
 * simpledrm, the driver Linux gives the firmware's framebuffer. */
#define DRM_IDENT_I915                                                              \
    {"i915",                                                                        \
     "Intel Graphics",                                                              \
     "20230929",                                                                    \
     1,                                                                             \
     6,                                                                             \
     0,                                                                             \
     DRIVER_GEM | DRIVER_RENDER | DRIVER_MODESET | DRIVER_ATOMIC | DRIVER_SYNCOBJ | \
         DRIVER_SYNCOBJ_TIMELINE,                                                   \
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
 * master, root, and a render node's client. */
#define DRM_AUTH         (1u << 0)
#define DRM_MASTER       (1u << 1)
#define DRM_ROOT_ONLY    (1u << 2)
#define DRM_RENDER_ALLOW (1u << 5)

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
 * NULL frees it with kfree. `enable_signaling` is called once, the first
 * time anyone waits or adds a callback, for a driver that signals only on
 * request; it returns 0 when the fence has signalled meanwhile.
 */
struct dma_fence_ops {
    const char *(*get_driver_name)(struct dma_fence *fence);
    const char *(*get_timeline_name)(struct dma_fence *fence);
    int (*enable_signaling)(struct dma_fence *fence);
    void (*release)(struct dma_fence *fence);
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
    uint32_t refs;
    int signaled;
    int32_t error; /* 0, or the -errno the work failed with */
    int64_t timestamp_ns;
    int64_t deadline_ns; /* earliest requested completion hint, INT64_MAX if none */
    struct dma_fence_cb *cbs;
    wait_queue_t wq;
    int signaling_enabled;
};

/** The first of `num` new timeline contexts. Any context. */
uint64_t dma_fence_context_alloc(uint32_t num);
/** Make `fence` an unsignalled point `seqno` of `context`, with one
 * reference, the caller's. */
void dma_fence_init(struct dma_fence *fence, const struct dma_fence_ops *ops, uint64_t context,
                    uint64_t seqno);
/** Take a reference; NULL is allowed and returned. */
struct dma_fence *dma_fence_get(struct dma_fence *fence);
/** Drop a reference, releasing the fence with the last; NULL is allowed. */
void dma_fence_put(struct dma_fence *fence);
/** Record the error a fence will signal with, before it signals. */
void dma_fence_set_error(struct dma_fence *fence, int32_t error);
/** Signal: stamp the time, run the callbacks, wake the waiters. Returns 0,
 * or -EINVAL when it had signalled. Any context, an interrupt among them. */
int32_t dma_fence_signal(struct dma_fence *fence);
/** Whether the fence has signalled, asking the driver if need be. */
int dma_fence_is_signaled(struct dma_fence *fence);
/** Run `func` when `fence` signals: 0, or -ENOENT, with nothing added, when
 * it has signalled already. */
int32_t dma_fence_add_callback(struct dma_fence *fence, struct dma_fence_cb *cb,
                               void (*func)(struct dma_fence *, struct dma_fence_cb *));
/** Take a callback back: 1 when it was pending, 0 when it ran. */
int dma_fence_remove_callback(struct dma_fence *fence, struct dma_fence_cb *cb);
/**
 * Wait for the fence until the CLOCK_MONOTONIC time `deadline_ns`, or for
 * ever when it is negative; sleeps. Returns 0 once signalled (the fence's
 * error is read from it), -ETIME at the deadline, or -EINTR for a
 * signal when `intr` is set.
 */
int32_t dma_fence_wait_until(struct dma_fence *fence, int intr, int64_t deadline_ns);

/*
 * The implicit fences of a buffer, Linux's dma_resv: outstanding writes
 * and reads. A new write joins the write fence; a new read is added.
 * Each slot holds a reference. Kept by a buffer object, or by its dma-buf
 * when it is imported, so that every device and the CPU see one set.
 */
struct dma_resv {
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
 * imports by other devices; NULL refuses those imports. `map_wc` records
 * the cache type that the exporter and its importers use.
 */
struct drm_gem_object_funcs {
    void (*free)(struct drm_gem_object *obj);
    int32_t (*mmap)(struct drm_gem_object *obj, vma_t *vma);
    int32_t (*pin)(struct drm_gem_object *obj);
    void (*unpin)(struct drm_gem_object *obj);
    uintptr_t (*page)(struct drm_gem_object *obj, uint32_t pgoff);
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
 * held. `resv` is its implicit fences, or its import's.
 */
struct drm_gem_object {
    uint32_t refs;
    struct drm_device *dev;
    uint64_t size;
    uint32_t name;
    uint64_t mmap_offset;
    struct dma_buf *dma_buf;
    struct dma_buf *import;
    struct dma_resv *resv;
    struct dma_resv resv_own;
    const struct drm_gem_object_funcs *funcs;
    int map_wc;                       /* PRIME mappings use write-combining */
    uint32_t handle_count;            /* handles any file has to it */
    struct drm_gem_object *dev_next;  /* the device's objects */
    struct drm_gem_object *mmap_next; /* the device's objects with an offset, by offset */
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
/** Take a reference to `obj`; NULL is allowed. */
void drm_gem_object_get(struct drm_gem_object *obj);
/** Drop a reference; the last frees the object through its `free` and
 * drops its device. NULL is allowed. */
void drm_gem_object_put(struct drm_gem_object *obj);
/** Give `file` a new handle to `obj`, the lowest free from 1, in `*handle`,
 * with a reference of its own: 0, or -ENOMEM. */
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
 * mmap of a card or render node: the object whose offset is the region's
 * page offset, which the file has a handle to, mapped by its `mmap` from
 * its own page 0, with a reference the region holds. -ENODEV for a device
 * unplugged or an object without `mmap`, -EINVAL for no object at that
 * offset or a range past its end, -EACCES for an object the file has no
 * handle to, or the hook's error.
 */
int32_t drm_gem_mmap(struct drm_file *file, vma_t *vma);

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

/* ------------------------------------------------------------------------
 * Open files
 */

/* The node a file is open on. */
#define DRM_MINOR_PRIMARY 0
#define DRM_MINOR_RENDER  2

/* An event queued for a file's reader, the bytes of one uapi event. */
struct drm_pending_event {
    struct drm_pending_event *next;
    struct drm_pending_event *pending_next; /* the file's reserved events */
    struct drm_file *file;                  /* NULL once the file is released unsent */
    struct dma_fence *fence;                /* the sender signals it as it sends, or NULL */
    uint32_t length;
    union {
        struct drm_event base;
        struct drm_event_vblank vbl;
        struct drm_event_crtc_sequence seq;
    } event;
};

/* A handle of a file's table: to a buffer object or to a syncobj. A table
 * is a list in the order of the ids. */
struct drm_handle {
    uint32_t id;
    void *object;
    struct dma_buf *prime; /* held export/import cache, NULL for other handles */
    struct drm_handle *next;
};

/** Add `object` to the table `list` under the lowest free id from 1, as
 * Linux's idr gives handles, into `*id`: 0, or -ENOMEM. */
int32_t drm_handle_insert(struct drm_handle **list, void *object, uint32_t *id);
/** The object of `id` in `list`, or NULL. */
void *drm_handle_find(struct drm_handle *list, uint32_t id);
/** Take `id` out of `list`, drop its PRIME cache reference and return its
 * object, or NULL for none. */
void *drm_handle_remove(struct drm_handle **list, uint32_t id);

/*
 * An open of a card or render node, one per open file description. The
 * client caps are what SET_CLIENT_CAP set. `event_space` is how many bytes
 * of events may still queue, Linux's 4096 at open.
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
 * the file. A render node's file has no master id, 0.
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
    int universal_planes, atomic, aspect_ratio, writeback_connectors, stereo_allowed,
        supports_virtualized_cursor_plane;
    struct drm_handle *objects;  /* buffer object handles */
    struct drm_handle *syncobjs; /* syncobj handles */
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
 * drm_event_cancel_free. A sender allocates `e` with kmalloc, sets its
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
/** Take back `e`, reserved and not sent, give back its space and free
 * it. */
void drm_event_cancel_free(struct drm_pending_event *e);

/**
 * The DRM ioctl dispatcher, Linux's drm_ioctl: the handler of `cmd` from
 * the core's table, or for numbers DRM_COMMAND_BASE to DRM_COMMAND_END - 1
 * the driver's, called with the kernel's copy of the argument at `arg` if
 * the caller may call it. ENODEV on a device unplugged, ENOTTY for a
 * number of another type, EINVAL for a number no row names, EACCES for a
 * caller the row's flags refuse, EFAULT, ENOMEM, or the handler's answer.
 * The argument is copied back by the direction the number and the row
 * share, whatever the handler answered. May sleep.
 */
int32_t drm_ioctl(struct drm_file *file, uint32_t cmd, uintptr_t arg);

/* ------------------------------------------------------------------------
 * Display state: mode objects and their atomic state (76-5a, 76-5b)
 */

/* A mode object's ID and type (DRM_MODE_OBJECT_*), and its properties,
 * each an ID and the value an atomic state does not hold itself. */
#define DRM_OBJECT_MAX_PROPERTY 64
struct drm_mode_object {
    uint32_t id;
    uint32_t type;
    uint32_t refs; /* framebuffers and blobs; 0 for the fixed objects */
    uint32_t count_props;
    uint32_t prop_ids[DRM_OBJECT_MAX_PROPERTY];
    uint64_t prop_values[DRM_OBJECT_MAX_PROPERTY];
};

/* A property: its name, its DRM_MODE_PROP_* flags, the range's two values
 * or the enum's and bitmask's values with their names. */
struct drm_property {
    struct drm_mode_object base;
    char name[DRM_PROP_NAME_LEN];
    uint32_t flags;
    uint32_t num_values;
    uint64_t *values;
    const char *const *enum_names; /* one per value for enums and bitmasks */
    struct drm_property *next;
};

/* A blob: bytes a property names by ID, made by the kernel or by
 * CREATEPROPBLOB, and freed by its last holder. */
struct drm_property_blob {
    struct drm_mode_object base;
    struct drm_file *owner; /* its maker, NULL for the kernel */
    uint32_t length;
    void *data;
    struct drm_property_blob *next;
};

/* A framebuffer: up to four planes of buffer objects in a fourcc format. */
struct drm_framebuffer {
    struct drm_mode_object base;
    struct drm_device *dev;
    struct drm_file *owner; /* its maker, until RMFB, CLOSEFB or close */
    uint32_t format;        /* DRM_FORMAT_* */
    uint64_t modifier;      /* DRM_FORMAT_MOD_LINEAR unless ADDFB2 gave one */
    uint32_t flags;         /* DRM_MODE_FB_* */
    uint32_t width, height;
    uint32_t pitches[4], offsets[4];
    struct drm_gem_object *obj[4];     /* held */
    struct drm_framebuffer *next;      /* the device's list */
    struct drm_framebuffer *file_next; /* the owner's list */
};

#define DRM_PLANE_TYPE_OVERLAY 0
#define DRM_PLANE_TYPE_PRIMARY 1
#define DRM_PLANE_TYPE_CURSOR  2

/*
 * A plane's state: the framebuffer it shows (held), where on its CRTC in
 * pixels, and which part of the framebuffer in 16.16 fixed point; the
 * fence to wait for before scanning out (IN_FENCE_FD, held).
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
    int visible; /* set by the check: the plane shows something */
};

/*
 * A CRTC's state: whether it is on, its mode (with the blob MODE_ID names),
 * the planes and connectors it feeds, the colour blobs, and the event to
 * send and the out-fence to signal when the commit that made this state
 * reaches the screen. The `_changed` marks are the check's, for the
 * commit's helpers.
 */
struct drm_crtc_state {
    struct drm_crtc *crtc;
    int enable;
    int active;
    struct drm_mode_modeinfo mode;
    struct drm_property_blob *mode_blob;
    uint32_t plane_mask;     /* bit N: plane N */
    uint32_t connector_mask; /* bit N: connector N */
    struct drm_property_blob *degamma_lut, *ctm, *gamma_lut;
    int vrr_enabled;
    int mode_changed, active_changed, planes_changed;
    struct drm_pending_event *event;
    int32_t __user *out_fence_ptr;
};

/* A connector's state: the CRTC it is routed to and its encoder. */
struct drm_connector_state {
    struct drm_connector *connector;
    struct drm_crtc *crtc;
    uint32_t encoder_id;
    uint32_t link_status; /* DRM_MODE_LINK_STATUS_* */
    uint32_t content_protection;
};

/*
 * A plane's hooks. `atomic_check` refuses a state the hardware cannot show
 * with -EINVAL; `atomic_update` makes the new state the hardware's and
 * `atomic_disable` turns the plane off, between the CRTC's `atomic_begin`
 * and `atomic_flush`. They may sleep in a blocking commit.
 */
struct drm_plane_funcs {
    int32_t (*atomic_check)(struct drm_plane *plane, struct drm_atomic_state *state);
    void (*atomic_update)(struct drm_plane *plane, struct drm_atomic_state *state);
    void (*atomic_disable)(struct drm_plane *plane, struct drm_atomic_state *state);
};

/* A plane: a layer a CRTC composes. */
struct drm_plane {
    struct drm_mode_object base;
    struct drm_device *dev;
    uint32_t index;
    int type; /* DRM_PLANE_TYPE_* */
    uint32_t possible_crtcs;
    const uint32_t *formats;
    uint32_t format_count;
    const uint64_t *modifiers; /* NULL: DRM_FORMAT_MOD_LINEAR alone */
    uint32_t modifier_count;
    const struct drm_plane_funcs *funcs;
    struct drm_plane_state *state;
};

/*
 * A CRTC's hooks. The atomic ones run in a commit as Linux's helpers run
 * them; `enable_vblank` and `disable_vblank` start and stop the vblank
 * interrupt (76-5b), whose handler calls `drm_crtc_handle_vblank`. The CRC
 * hooks serve debugfs's crc/control (76-18b): `set_crc_source` starts or,
 * for NULL, stops the source `name` and says how many values each frame's
 * entry has; `verify_crc_source` answers whether `name` is one, without
 * starting it; `get_crc_sources` lists them.
 */
struct drm_crtc_funcs {
    int32_t (*atomic_check)(struct drm_crtc *crtc, struct drm_atomic_state *state);
    void (*atomic_enable)(struct drm_crtc *crtc, struct drm_atomic_state *state);
    void (*atomic_disable)(struct drm_crtc *crtc, struct drm_atomic_state *state);
    void (*atomic_begin)(struct drm_crtc *crtc, struct drm_atomic_state *state);
    void (*atomic_flush)(struct drm_crtc *crtc, struct drm_atomic_state *state);
    int32_t (*enable_vblank)(struct drm_crtc *crtc);
    void (*disable_vblank)(struct drm_crtc *crtc);
    int32_t (*set_crc_source)(struct drm_crtc *crtc, const char *name);
    int32_t (*verify_crc_source)(struct drm_crtc *crtc, const char *name, uint32_t *values_cnt);
    const char *const *(*get_crc_sources)(struct drm_crtc *crtc, uint32_t *count);
};

/* A CRTC's vblank count and the time of the last, from CLOCK_MONOTONIC. */
struct drm_vblank {
    uint64_t count;
    int64_t time_ns;
    uint32_t refs; /* waiters and events that keep the interrupt on */
    int enabled;
    wait_queue_t wait;
};

/* One frame's CRC entry, which debugfs's crc/data reads. */
#define DRM_MAX_CRC_NR 10
struct drm_crtc_crc_entry {
    int has_frame_counter;
    uint32_t frame;
    uint32_t crcs[DRM_MAX_CRC_NR];
};

/* A CRTC: a scanout engine that reads planes and drives connectors. */
struct drm_crtc {
    struct drm_mode_object base;
    struct drm_device *dev;
    uint32_t index;
    char name[32];
    struct drm_plane *primary, *cursor;
    uint32_t gamma_size;
    const struct drm_crtc_funcs *funcs;
    struct drm_crtc_state *state;
    struct drm_vblank vblank;
    const char *crc_source; /* NULL while none runs */
    uint32_t crc_values;    /* values in each entry */
};

/* An encoder, which Linux keeps for its interface: one per connector here,
 * fixed to the CRTCs it can take. */
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
 * probe of the sink; `get_modes` reads the sink's EDID and modes into the
 * connector (`drm_connector_update_edid`) and returns how many; `mode_valid`
 * answers 0 for a mode the hardware can drive. `force` tells the driver the
 * debug force changed, after the core has set `force` (76-18b).
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
 * the type's name and `type_id` ("Virtual-1", "eDP-1"), which /sys and
 * Weston's configuration use. `edid` is the blob of the sink's EDID, or of
 * `edid_override` while debugfs set one (76-18b), which replaces the
 * sink's whenever the modes are read.
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
    uint32_t mm_width, mm_height;
    uint32_t subpixel;
    struct drm_encoder *encoder;
    struct drm_display_mode *modes;
    struct drm_property_blob *edid;
    struct drm_property_blob *edid_override;
    const struct drm_connector_funcs *funcs;
    struct drm_connector_state *state;
};

/*
 * An atomic state: a commit's new states, by object index, with the old
 * ones they replace kept for the hooks and freed after; NULL where the
 * commit leaves an object alone. `flags` are DRM_MODE_ATOMIC_* and
 * DRM_MODE_PAGE_FLIP_*.
 */
#define DRM_MAX_CRTCS      8
#define DRM_MAX_PLANES     32
#define DRM_MAX_CONNECTORS 16
struct drm_atomic_state {
    struct drm_device *dev;
    uint32_t flags;
    int allow_modeset;
    struct drm_plane_state *planes_old[DRM_MAX_PLANES], *planes_new[DRM_MAX_PLANES];
    struct drm_crtc_state *crtcs_old[DRM_MAX_CRTCS], *crtcs_new[DRM_MAX_CRTCS];
    struct drm_connector_state *connectors_old[DRM_MAX_CONNECTORS],
        *connectors_new[DRM_MAX_CONNECTORS];
};

/*
 * A device's display: its limits, its objects, and the two commit hooks.
 * `atomic_check` refuses a state the device cannot show, with Linux's
 * errors; `atomic_commit` makes a checked state the device's, at once or,
 * when `nonblock` is set, from a worker, sending each CRTC's event and
 * signalling its out-fence when the new state reaches the screen. NULL
 * hooks take the core's helpers (drm_atomic.c).
 */
struct drm_mode_config {
    uint32_t min_width, max_width, min_height, max_height;
    uint32_t cursor_width, cursor_height;
    uint32_t preferred_depth;
    int prefer_shadow;
    int async_page_flip;
    uint32_t num_crtc, num_plane, num_connector, num_encoder;
    struct drm_crtc *crtcs[DRM_MAX_CRTCS];
    struct drm_plane *planes[DRM_MAX_PLANES];
    struct drm_connector *connectors[DRM_MAX_CONNECTORS];
    struct drm_encoder *encoders[DRM_MAX_CONNECTORS];
    struct drm_framebuffer *fbs;
    struct drm_property *properties;
    struct drm_property_blob *blobs;
    uint32_t next_id;
    int32_t (*atomic_check)(struct drm_device *dev, struct drm_atomic_state *state);
    int32_t (*atomic_commit)(struct drm_device *dev, struct drm_atomic_state *state, int nonblock);
};

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
 * or NULL. `commit_lock` orders commits (the lock rule above).
 */
struct drm_device {
    const struct drm_driver *driver;
    uint32_t features;
    int index;
    uint32_t refs;
    int registered;
    int unplugged;
    char unique[32];
    const pci_device_t *pci;
    const struct sysfs_pci_ident *pci_ident;
    struct drm_file *master;
    uint32_t open_count;
    struct drm_file *files;              /* its open files, through `dev_next` */
    uint32_t next_master_id;             /* the last master record's id */
    struct drm_gem_object *objects;      /* its buffer objects, through `dev_next` */
    struct drm_gem_object *mmap_objects; /* those with an mmap offset, by offset */
    struct drm_mode_config mode_config;
    int commit_held; /* the commit lock: 1 while a commit holds it */
    wait_queue_t commit_wait;
    void *driver_priv;
};

/* Whether a device has the feature `f`, a DRIVER_* bit. */
#define drm_core_check_feature(dev, f) (((dev)->features & (f)) != 0)

/** Probe and register the display devices, once, after the PCI drivers
 * have started: in the order the aperture handoff needs, bochs and
 * virtio-gpu, then fbdrm on what they left, then vkms when `rach2.vkms`
 * asks; and /dev/udmabuf. Logs "drm: start". */
void drm_start(void);

/** Fill a new device for `driver`, with one reference, the caller's, and
 * the driver's features; every other member is zero, and the driver sets
 * `unique`, `pci` and its own before it registers. Registers nothing. */
void drm_dev_init(struct drm_device *dev, const struct drm_driver *driver);
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
/** Take and drop a reference to a device. */
void drm_dev_get(struct drm_device *dev);
void drm_dev_put(struct drm_device *dev);
/** The registered device of card index `index`, or NULL; for debugfs and
 * /sys, which walk the devices by index. */
struct drm_device *drm_dev_at(int index);

/*
 * A listener of the master role: the console (76-11), which gives up the
 * screen while a program holds the role, and the seat's revoke path. Its
 * `changed` runs after each change of a device's master, with the file
 * that holds the role now or NULL when none does: SET_MASTER, DROP_MASTER,
 * the first open of a card with no master, and the release of the master's
 * file by close, exec or exit. It runs in the caller's context and may
 * not sleep.
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
 * machine with none of them, keeps the screen.
 */
#define DRM_APERTURE_MAX 4

/** Record that `dev` drives the firmware framebuffer at physical `base`,
 * `size` bytes, and is removed through `remove`. 0, or -EBUSY when
 * another firmware device has the range or the table is full. */
int32_t drm_aperture_acquire_firmware(struct drm_device *dev, uint64_t base, uint64_t size,
                                      void (*remove)(struct drm_device *dev));
/** Remove every firmware device whose range overlaps [base, base + size),
 * for the native driver `name`, which the log names. 0. */
int32_t drm_aperture_remove_conflicting(uint64_t base, uint64_t size, const char *name);

/* ------------------------------------------------------------------------
 * Helpers the drivers call
 */

/** Count a vblank of `crtc` and stamp it, send its due events and wake its
 * waiters; from the driver's vblank interrupt or timer. 1 while vblanks
 * are on, 0 when they went off. (76-5b) */
int drm_crtc_handle_vblank(struct drm_crtc *crtc);
/** Queue one frame's CRC values for crc/data, from the driver's vblank
 * path. 0, or -EINVAL while no source runs. (76-18b) */
int32_t drm_crtc_add_crc_entry(struct drm_crtc *crtc, int has_frame, uint32_t frame,
                               const uint32_t *crcs);
/** Set `connector`'s EDID blob from the sink's `edid`, `len` bytes, or
 * from its override, and its modes from the blob; the number of modes.
 * (76-5a) */
int drm_connector_update_edid(struct drm_connector *connector, const uint8_t *edid, uint32_t len);
/** Tell user space that `dev`'s outputs changed: the uevent `change` of
 * its card with HOTPLUG=1, which compositors listen for. (drm_sysfs.c) */
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
 * The console as a DRM client, in drm_client.c (76-11a)
 */

/** A device registered: the console takes it if it has none, or if this
 * one is a native driver's that replaced the firmware's. */
void drm_client_dev_register(struct drm_device *dev);
/** A device is going: the console leaves it. */
void drm_client_dev_unregister(struct drm_device *dev);

/* ------------------------------------------------------------------------
 * The drivers' probes. Each finds its devices, binds and registers them,
 * and returns how many it bound, or -errno. bochs, i915 and fbdrm reach
 * PC-only code and are called through `arch_drm_probe` (sys/arch/arch.h).
 */

int32_t bochs_probe(void);
int32_t fbdrm_probe(void);
int32_t i915_probe(void);
int32_t virtio_gpu_probe(void);
int32_t vkms_probe(void);

/* ------------------------------------------------------------------------
 * The ioctl handlers of the core's table, one per number, each in the file
 * of the task that writes it. Until then each answers -EOPNOTSUPP, what
 * Linux answers for a driver without the feature.
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

/* drm_syncobj.c (76-3b): sync objects. */
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
