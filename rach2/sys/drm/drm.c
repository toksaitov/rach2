/*
 * drm.c: the DRM core: the start of the display devices, their card and
 * render nodes, the open files and their events, the ioctl dispatcher with
 * the core's table, the firmware framebuffer's handoff and the core's own
 * ioctls of identity and capabilities.
 */

#include "drm.h"
#include "../virtio/virtio.h"
#include "../dmabuf/dmabuf.h"

#include "../arch/arch.h"
#include "../fs/devfs.h"
#include "../fs/file.h"
#include "../io/poll.h"
#include "../klog.h"
#include "../lib/kerrno.h"
#include "../lib/kirq.h"
#include "../lib/kstring.h"
#include "../mm/kheap.h"
#include "../mm/paging.h"
#include "../mm/usercopy.h"
#include "../proc/percpu.h"
#include "../proc/process.h"
#include "../proc/smp.h"

/* Linux's O_EXCL, which an open of a node refuses with EBUSY. */
#define DRM_O_EXCL 0200

/* The interface version SET_VERSION answers, Linux's DRM_IF_MAJOR and
 * DRM_IF_MINOR. */
#define DRM_IF_MAJOR 1
#define DRM_IF_MINOR 4

/* The bytes of events a file may have queued, Linux's at open. */
#define DRM_EVENT_SPACE 4096

/* An argument of up to this many bytes is copied to the stack, a larger one
 * to the heap, as Linux's drm_ioctl does. */
#define DRM_IOCTL_STACK 128

/* GET_CLIENT's uid, Linux's overflowuid. */
#define DRM_OVERFLOW_UID 65534

/* A device's node: its registry record, its name and its type. */
struct drm_minor {
    struct devfs_node node;
    char name[DEVFS_NODE_NAME_MAX];
    struct drm_device *dev;
    int type; /* DRM_MINOR_PRIMARY or DRM_MINOR_RENDER */
};

/* The devices by card index, from registration to the release of the
 * record, and the two nodes of each. */
static struct drm_device *devices[DRM_MAX_DEVICES];
static struct drm_minor minors[DRM_MAX_DEVICES][2];

static struct drm_master_notifier *master_notifiers;

/* A firmware device's range of the framebuffer, while it drives it. */
struct drm_aperture {
    struct drm_device *dev; /* NULL: the slot is free */
    uint64_t base, size;
    void (*remove)(struct drm_device *dev);
};

static struct drm_aperture apertures[DRM_APERTURE_MAX];

/* Set once a native driver has removed the firmware devices: no firmware
 * device binds after, as Linux's sysfb_disable. */
static int aperture_native;

void drm_start(void)
{
    int32_t n;

    klog(KLOG_INFO, "drm", "start");
    drm_debugfs_init();
    n = udmabuf_start();
    if (n < 0) {
        klog(KLOG_WARN, "drm", "udmabuf: error %d", (int)n);
    }
    arch_drm_probe(ARCH_DRM_NATIVE);
    n = virtio_gpu_probe();
    if (n < 0 && n != -ENODEV) {
        klog(KLOG_WARN, "drm", "virtio_gpu: error %d", (int)n);
    }
    n = vgpu_shm_start();
    if (n < 0) {
        klog(KLOG_WARN, "drm", "vgpu-shm: error %d", (int)n);
    }
    arch_drm_probe(ARCH_DRM_FIRMWARE);
    n = vkms_probe();
    if (n < 0 && n != -ENODEV) {
        klog(KLOG_WARN, "drm", "vkms: error %d", (int)n);
    }
    n = vgem_probe();
    if (n < 0 && n != -ENODEV) {
        klog(KLOG_WARN, "drm", "vgem: error %d", (int)n);
    }
}

/* ------------------------------------------------------------------------
 * Devices
 */

void drm_file_locks_init(struct drm_file *file)
{
    spin_lock_init(&file->objects.lock, "drm_table_lock");
    spin_lock_init(&file->syncobjs.lock, "drm_syncobj_table_lock");
}

void drm_dev_init(struct drm_device *dev, const struct drm_driver *driver)
{
    k_memset(dev, 0, sizeof(*dev));
    dev->driver = driver;
    dev->features = driver->ident.features;
    dev->index = -1;
    dev->refs = 1;
    spin_lock_init(&dev->object_lock, "drm_object_lock");
    spin_lock_init(&dev->event_lock, "drm_event_lock");
    spin_lock_init(&dev->vbl_lock, "drm_vbl_lock");
    mutex_init(&dev->mode_lock, "drm_mode_lock");
}

void drm_dev_get(struct drm_device *dev)
{
    uint32_t old;

    do {
        old = dev->refs;
    } while (arch_atomic_cmpxchg((volatile unsigned int *)&dev->refs, old, old + 1u) != old);
}

/* Atomic, as Linux's kref (drm_drv.c:810-829), since a buffer object's
 * last reference, which holds the device, may go on any processor. */
void drm_dev_put(struct drm_device *dev)
{
    uint32_t old;

    if (!dev) {
        return;
    }
    do {
        old = dev->refs;
    } while (arch_atomic_cmpxchg((volatile unsigned int *)&dev->refs, old, old - 1u) != old);
    if (old != 1u) {
        return;
    }
    if (dev->index >= 0 && devices[dev->index] == dev) {
        devices[dev->index] = NULL;
    }
    if (dev->driver->release) {
        dev->driver->release(dev);
    }
}

struct drm_device *drm_dev_at(int index)
{
    struct drm_device *dev;

    if (index < 0 || index >= DRM_MAX_DEVICES) {
        return NULL;
    }
    dev = devices[index];
    return dev && dev->registered && !dev->unplugged ? dev : NULL;
}

/** Append `n` in decimal to `s` at `*at`, within `room`. */
static void put_dec(char *s, uint32_t *at, uint32_t room, uint32_t n)
{
    char digits[10];
    int i = 0;

    do {
        digits[i++] = (char)('0' + n % 10);
        n /= 10;
    } while (n);
    while (i > 0 && *at + 1 < room) {
        s[(*at)++] = digits[--i];
    }
    s[*at] = '\0';
}

/** Append `n` as `width` lowercase hex digits to `s` at `*at`. */
static void put_hex(char *s, uint32_t *at, uint32_t room, uint32_t n, int width)
{
    while (width-- > 0 && *at + 1 < room) {
        s[(*at)++] = "0123456789abcdef"[(n >> (4 * width)) & 0xf];
    }
    s[*at] = '\0';
}

/** Append the string `t` to `s` at `*at`. */
static void put_str(char *s, uint32_t *at, uint32_t room, const char *t)
{
    while (*t && *at + 1 < room) {
        s[(*at)++] = *t++;
    }
    s[*at] = '\0';
}

/** GET_UNIQUE's name when the driver gave none: Linux's drm_pci_set_busid,
 * "pci:0000:BB:DD.F", for a PCI function, else the driver's name. */
static void default_unique(struct drm_device *dev)
{
    uint32_t at = 0;

    if (dev->pci) {
        put_str(dev->unique, &at, sizeof(dev->unique), "pci:0000:");
        put_hex(dev->unique, &at, sizeof(dev->unique), dev->pci->bus, 2);
        put_str(dev->unique, &at, sizeof(dev->unique), ":");
        put_hex(dev->unique, &at, sizeof(dev->unique), dev->pci->device, 2);
        put_str(dev->unique, &at, sizeof(dev->unique), ".");
        put_dec(dev->unique, &at, sizeof(dev->unique), dev->pci->function);
    } else {
        put_str(dev->unique, &at, sizeof(dev->unique), dev->driver->ident.name);
    }
}

static int32_t drm_node_open(struct devfs_node *node, fd_entry_t *e, int flags);
static const struct file_ops drm_fops;

/** Fill node `type` of card `index` for `dev`: "dri/cardN" at minor N or
 * "dri/renderD(128 + N)" at minor 128 + N. */
static struct drm_minor *minor_fill(struct drm_device *dev, int index, int type)
{
    struct drm_minor *m = &minors[index][type == DRM_MINOR_RENDER];
    uint32_t at = 0;
    int minor = type == DRM_MINOR_RENDER ? DRM_MINOR_RENDER_BASE + index : index;

    put_str(m->name, &at, sizeof(m->name), type == DRM_MINOR_RENDER ? "dri/renderD" : "dri/card");
    put_dec(m->name, &at, sizeof(m->name), (uint32_t)minor);
    m->dev = dev;
    m->type = type;
    m->node.name = m->name;
    m->node.major = DRM_MAJOR;
    m->node.minor = (uint16_t)minor;
    /* udev's modes on Linux: the card for its group, the render node for
     * everyone. The registry's nodes are root's. */
    m->node.mode = type == DRM_MINOR_RENDER ? 0666 : 0660;
    m->node.ops = &drm_fops;
    m->node.open = drm_node_open;
    m->node.priv = m;
    return m;
}

int32_t drm_dev_register(struct drm_device *dev)
{
    struct drm_minor *card, *render = NULL;
    int32_t rc;
    int i;

    for (i = 0; i < DRM_MAX_DEVICES && devices[i]; i++) {
    }
    if (i == DRM_MAX_DEVICES) {
        return -ENOSPC;
    }
    (void)drm_atomic_worker_start();
    if (!dev->unique[0]) {
        default_unique(dev);
    }
    card = minor_fill(dev, i, DRM_MINOR_PRIMARY);
    rc = devfs_node_register(&card->node);
    if (rc != 0) {
        return rc;
    }
    if (drm_core_check_feature(dev, DRIVER_RENDER)) {
        render = minor_fill(dev, i, DRM_MINOR_RENDER);
        rc = devfs_node_register(&render->node);
        if (rc != 0) {
            devfs_node_unregister(&card->node);
            return rc;
        }
    }
    dev->index = i;
    rc = drm_sysfs_register(dev);
    if (rc != 0) {
        if (render) {
            devfs_node_unregister(&render->node);
        }
        devfs_node_unregister(&card->node);
        return rc;
    }
    devices[i] = dev;
    dev->registered = 1;
    drm_debugfs_register(dev);
    klog(KLOG_INFO, "drm", "%s: card%d", dev->driver->ident.name, i);
    drm_client_dev_register(dev);
    return 0;
}

void drm_gem_object_zap(struct drm_gem_object *obj)
{
    struct drm_gem_object *imported;
    struct dma_buf *buf = obj->dma_buf;
    int i;

    paging_zap_device(obj, 0, (uint32_t)(obj->size >> PAGE_SHIFT));
    if (!buf) {
        return;
    }
    for (i = 0; i < DRM_MAX_DEVICES; i++) {
        if (!devices[i]) {
            continue;
        }
        for (imported = drm_gem_object_next(devices[i], NULL); imported;
             imported = drm_gem_object_next(devices[i], imported)) {
            if (imported->import == buf) {
                paging_zap_device(imported, 0, (uint32_t)(imported->size >> PAGE_SHIFT));
            }
        }
    }
}

void drm_dev_unplug(struct drm_device *dev)
{
    struct drm_gem_object *obj;
    struct drm_file *f;
    int was;

    if (dev->unplugged) {
        return;
    }
    /* Set under the mode lock, which a KMS row holds when it tests it, so
     * that a row under way ends before the display goes. Read again there:
     * another unplug may have run while this one slept for the lock. */
    if (drm_core_check_feature(dev, DRIVER_MODESET)) {
        drm_modeset_lock_nointr(dev);
        was = dev->unplugged;
        dev->unplugged = 1;
        drm_modeset_unlock(dev);
        if (was) {
            return;
        }
    } else {
        dev->unplugged = 1;
    }
    if (dev->registered) {
        drm_debugfs_unregister(dev);
        drm_sysfs_unregister(dev);
        devfs_node_unregister(&minors[dev->index][0].node);
        devfs_node_unregister(&minors[dev->index][1].node);
        drm_client_dev_unregister(dev);
    }
    for (obj = drm_gem_object_next(dev, NULL); obj; obj = drm_gem_object_next(dev, obj)) {
        drm_gem_object_zap(obj);
    }
    if (drm_core_check_feature(dev, DRIVER_MODESET)) {
        drm_vblank_unplug(dev);
    }
    for (f = dev->files; f; f = f->dev_next) {
        wq_wake_all(&f->event_wait);
    }
    if (dev->driver->unplug) {
        dev->driver->unplug(dev);
    }
    drm_dev_put(dev);
}

/* ------------------------------------------------------------------------
 * The master role's listeners
 */

void drm_master_notifier_register(struct drm_master_notifier *n)
{
    n->next = master_notifiers;
    master_notifiers = n;
}

void drm_master_notifier_unregister(struct drm_master_notifier *n)
{
    struct drm_master_notifier **link;

    for (link = &master_notifiers; *link; link = &(*link)->next) {
        if (*link == n) {
            *link = n->next;
            return;
        }
    }
}

void drm_master_changed(struct drm_device *dev, struct drm_file *master)
{
    struct drm_master_notifier *n;

    for (n = master_notifiers; n; n = n->next) {
        n->changed(dev, master);
    }
}

/* ------------------------------------------------------------------------
 * Open files and their events
 */

/**
 * The open of a node, its registry's `open`: a new file of the device on
 * the descriptor `e`, which the driver's `open` sees, master of a card with
 * none. ENODEV for a device unplugged, EBUSY under O_EXCL, ENOMEM, or the
 * driver's error.
 */
static int32_t drm_node_open(struct devfs_node *node, fd_entry_t *e, int flags)
{
    struct drm_minor *m = (struct drm_minor *)node->priv;
    struct drm_device *dev = m->dev;
    struct drm_file *file;
    int32_t rc;

    if (!dev || dev->unplugged) {
        return -ENODEV;
    }
    if (flags & DRM_O_EXCL) {
        return -EBUSY;
    }
    file = kzalloc(sizeof(*file));
    if (!file) {
        return -ENOMEM;
    }
    drm_file_locks_init(file);
    file->dev = dev;
    file->minor_type = m->type;
    file->pid = current_proc ? current_proc->tgid : 0;
    file->uid = current_proc ? current_proc->euid : 0;
    /* Linux: capable(CAP_SYS_ADMIN) at open. */
    file->authenticated = current_proc && proc_privileged(current_proc);
    file->event_space = DRM_EVENT_SPACE;
    wait_queue_init(&file->event_wait);
    drm_dev_get(dev);
    if (dev->driver->open) {
        rc = dev->driver->open(dev, file);
        if (rc != 0) {
            drm_dev_put(dev);
            kfree(file);
            return rc;
        }
    }
    file->dev_next = dev->files;
    dev->files = file;
    dev->open_count++;
    if (file->minor_type == DRM_MINOR_PRIMARY) {
        drm_master_open(file);
    }
    e->priv = file;
    return 0;
}

/** Free the events queued for `file` and leave its reserved ones to their
 * senders, unattached, as Linux's drm_events_release (drm_file.c:189-211). */
static void events_release(struct drm_file *file)
{
    struct drm_pending_event *e, *queued;
    uint32_t flags = spin_lock_irqsave(&file->dev->event_lock);

    while ((e = file->pending) != NULL) {
        file->pending = e->pending_next;
        e->pending_next = NULL;
        e->file = NULL;
    }
    queued = file->events;
    file->events = NULL;
    file->events_tail = NULL;
    spin_unlock_irqrestore(&file->dev->event_lock, flags);
    while ((e = queued) != NULL) {
        queued = e->next;
        kfree(e);
    }
}

/**
 * The release of a file, as Linux's drm_file_free: its events, its
 * display objects, its syncobj and buffer handles, its master role, then
 * the driver's `postclose`, and the device's reference.
 */
static void drm_file_release(struct drm_file *file)
{
    struct drm_device *dev = file->dev;
    struct drm_file **link;

    events_release(file);
    drm_mode_file_release(file);
    drm_syncobj_release(file);
    drm_gem_release(file);
    if (file->minor_type == DRM_MINOR_PRIMARY) {
        drm_master_release(file);
    }
    if (dev->driver->postclose) {
        dev->driver->postclose(dev, file);
    }
    for (link = &dev->files; *link; link = &(*link)->dev_next) {
        if (*link == file) {
            *link = file->dev_next;
            break;
        }
    }
    dev->open_count--;
    kfree(file);
    drm_dev_put(dev);
}

/** The close of a node's last descriptor of an open file description,
 * which the descriptor's release calls without the big lock: the release
 * of the file reaches the display, the syncobjs and the driver, and takes
 * it. */
static void drm_node_close(fd_entry_t *e)
{
    struct drm_file *file = (struct drm_file *)e->priv;

    if (file) {
        e->priv = NULL;
        bkl_enter();
        drm_file_release(file);
        bkl_exit();
    }
}

int32_t drm_event_reserve(struct drm_file *file, struct drm_pending_event *e)
{
    uint32_t flags = spin_lock_irqsave(&file->dev->event_lock);

    if (file->event_space < e->length) {
        spin_unlock_irqrestore(&file->dev->event_lock, flags);
        return -ENOMEM;
    }
    file->event_space -= e->length;
    e->file = file;
    e->dev = file->dev;
    e->next = NULL;
    e->pending_next = file->pending;
    file->pending = e;
    spin_unlock_irqrestore(&file->dev->event_lock, flags);
    return 0;
}

/** Take `e` off its file's reserved events. The device's `event_lock` is
 * held. */
static void pending_unlink(struct drm_pending_event *e)
{
    struct drm_pending_event **link;

    for (link = &e->file->pending; *link; link = &(*link)->pending_next) {
        if (*link == e) {
            *link = e->pending_next;
            break;
        }
    }
    e->pending_next = NULL;
}

/** Queue `e`, whose fence is already taken off, on its file and wake the
 * reader, or free it when it has no file. The event lock of `e`'s device
 * is held. */
static void event_queue_locked(struct drm_pending_event *e)
{
    struct drm_file *file = e->dev ? e->file : NULL;

    if (!file) {
        kfree(e);
        return;
    }
    pending_unlink(e);
    e->next = NULL;
    if (file->events_tail) {
        file->events_tail->next = e;
    } else {
        file->events = e;
    }
    file->events_tail = e;
    wq_wake(&file->event_wait, WQ_DATA);
}

/**
 * As Linux's drm_send_event_helper (drm_file.c:727-760): the event's file
 * is read, and its reader woken, under the event lock of the device it was
 * reserved on, since a release of the file on another processor clears
 * `file` under that lock before it frees the file. An event never reserved
 * has no device to lock, and is freed.
 */
void drm_send_event(struct drm_pending_event *e)
{
    struct drm_device *dev = e->dev;
    struct dma_fence *fence = e->fence;
    uint32_t flags;

    e->fence = NULL;
    if (fence) {
        dma_fence_signal(fence);
        dma_fence_put(fence);
    }
    if (!dev) {
        kfree(e);
        return;
    }
    flags = spin_lock_irqsave(&dev->event_lock);
    event_queue_locked(e);
    spin_unlock_irqrestore(&dev->event_lock, flags);
}

void drm_send_event_locked(struct drm_pending_event *e)
{
    struct dma_fence *fence = e->fence;

    e->fence = NULL;
    if (fence) {
        dma_fence_signal(fence);
        dma_fence_put(fence);
    }
    event_queue_locked(e);
}

void drm_event_cancel_free(struct drm_pending_event *e)
{
    uint32_t flags;

    dma_fence_put(e->fence);
    e->fence = NULL;
    if (e->dev) {
        flags = spin_lock_irqsave(&e->dev->event_lock);
        if (e->file) {
            pending_unlink(e);
            e->file->event_space += e->length;
        }
        spin_unlock_irqrestore(&e->dev->event_lock, flags);
    }
    kfree(e);
}

/** Whether a reader of `arg`, a file, has an event. */
static int events_readable(void *arg)
{
    struct drm_file *file = (struct drm_file *)arg;

    return file->events != NULL;
}

/**
 * Take the first queued event of `file` into `*out` if it fits in `room`
 * bytes, as Linux's drm_read under its event lock: 1 taken, 0 none queued,
 * -1 the first does not fit. The event keeps its space until `event_done`,
 * so that the count stays within DRM_EVENT_SPACE while its copy sleeps.
 */
static int event_take(struct drm_file *file, uint32_t room, struct drm_pending_event **out)
{
    struct drm_pending_event *ev;
    uint32_t flags = spin_lock_irqsave(&file->dev->event_lock);
    int rc = 1;

    ev = file->events;
    if (!ev) {
        rc = 0;
    } else if (ev->length > room) {
        rc = -1;
    } else {
        file->events = ev->next;
        if (!file->events) {
            file->events_tail = NULL;
        }
        *out = ev;
    }
    spin_unlock_irqrestore(&file->dev->event_lock, flags);
    return rc;
}

/** Give back the space of `ev`, taken by event_take and copied, and free it. */
static void event_done(struct drm_file *file, struct drm_pending_event *ev)
{
    uint32_t flags = spin_lock_irqsave(&file->dev->event_lock);

    file->event_space += ev->length;
    spin_unlock_irqrestore(&file->dev->event_lock, flags);
    kfree(ev);
}

/** Put `ev`, taken by event_take, back at the head of `file`'s queue, and
 * wake the readers and pollers that found the queue empty meanwhile, as
 * Linux's drm_read does after a failed copy. */
static void event_put_back(struct drm_file *file, struct drm_pending_event *ev)
{
    uint32_t flags = spin_lock_irqsave(&file->dev->event_lock);

    ev->next = file->events;
    file->events = ev;
    if (!file->events_tail) {
        file->events_tail = ev;
    }
    spin_unlock_irqrestore(&file->dev->event_lock, flags);
    wq_wake(&file->event_wait, WQ_DATA);
}

/**
 * read of a node: the queued events that fit in `count` bytes, whole, as
 * Linux's drm_read; 0 when the first does not fit. Blocks for one unless
 * O_NONBLOCK, which answers EAGAIN; EINTR for a signal, EFAULT for a bad
 * buffer before any event was read. Each event leaves the queue before its
 * copy, which may sleep, so that another reader cannot free it meanwhile.
 * An unplugged device changes nothing here, as on Linux: a compositor that
 * polls the node keeps its loop, and learns of the loss from its ioctls.
 */
static int32_t drm_read(fd_entry_t *e, char __user *buf, uint32_t count)
{
    struct drm_file *file = (struct drm_file *)e->priv;
    struct drm_pending_event *ev = NULL;
    uint32_t done = 0;
    int got;

    for (;;) {
        got = event_take(file, count - done, &ev);
        if (got < 0 || (got == 0 && done)) {
            return (int32_t)done;
        }
        if (got == 0) {
            if (e->ofd && (e->ofd->status_flags & OFD_NONBLOCK)) {
                return -EAGAIN;
            }
            if (wq_wait_event(&file->event_wait, events_readable, file) < 0) {
                return -EINTR;
            }
            continue;
        }
        if (copy_to_user(buf + done, &ev->event, ev->length) != 0) {
            event_put_back(file, ev);
            return done ? (int32_t)done : -EFAULT;
        }
        done += ev->length;
        event_done(file, ev);
    }
}

/** poll of a node: POLLIN while an event is queued and nothing else, of an
 * unplugged device too, as Linux's drm_poll. */
static short drm_poll(fd_entry_t *e, short events, struct wait_queue **wq)
{
    struct drm_file *file = (struct drm_file *)e->priv;

    *wq = &file->event_wait;
    return (short)(events & (file->events ? POLLIN : 0));
}

/** ioctl of a node: the dispatcher. */
static int32_t drm_node_ioctl(fd_entry_t *e, uint32_t request, uintptr_t arg)
{
    return drm_ioctl((struct drm_file *)e->priv, request, arg);
}

/** mmap of a node: a buffer object's pages (`drm_gem_mmap`). */
static int32_t drm_node_mmap(fd_entry_t *e, struct vma *vma)
{
    return drm_gem_mmap((struct drm_file *)e->priv, vma);
}

/* read and poll take only the event lock; ioctl takes the big lock for a
 * row without DRM_UNLOCKED, and close for the file's release. */
static const struct file_ops drm_fops = {
    drm_read,       NULL,          drm_poll, drm_node_ioctl,
    drm_node_close, drm_node_mmap, NULL,     FILE_OPS_UNLOCKED};

/** Whether the held descriptor belongs to this DRM core's node operations. */
int drm_is_file(const fd_entry_t *entry)
{
    return entry && entry->ops == &drm_fops && entry->priv != NULL;
}

/* ------------------------------------------------------------------------
 * The dispatcher
 */

#define DRM_IOCTL_DEF(cmd, func, flags) {cmd, flags, func, #cmd}

/* The core's ioctls, Linux 6.12's drm_ioctls with their flags. A number no
 * row names answers EINVAL. */
static const struct drm_ioctl_desc drm_ioctls[] = {
    DRM_IOCTL_DEF(DRM_IOCTL_VERSION, drm_version, DRM_RENDER_ALLOW | DRM_UNLOCKED),
    DRM_IOCTL_DEF(DRM_IOCTL_GET_UNIQUE, drm_getunique, 0),
    DRM_IOCTL_DEF(DRM_IOCTL_GET_MAGIC, drm_getmagic, 0),
    DRM_IOCTL_DEF(DRM_IOCTL_GET_CLIENT, drm_getclient, 0),
    DRM_IOCTL_DEF(DRM_IOCTL_GET_STATS, drm_getstats, DRM_UNLOCKED),
    DRM_IOCTL_DEF(DRM_IOCTL_GET_CAP, drm_getcap, DRM_RENDER_ALLOW | DRM_UNLOCKED),
    DRM_IOCTL_DEF(DRM_IOCTL_SET_CLIENT_CAP, drm_setclientcap, 0),
    DRM_IOCTL_DEF(DRM_IOCTL_SET_VERSION, drm_setversion, DRM_MASTER),

    DRM_IOCTL_DEF(DRM_IOCTL_SET_UNIQUE, drm_invalid_op,
                  DRM_AUTH | DRM_MASTER | DRM_ROOT_ONLY | DRM_UNLOCKED),
    DRM_IOCTL_DEF(DRM_IOCTL_BLOCK, drm_noop, DRM_AUTH | DRM_MASTER | DRM_ROOT_ONLY | DRM_UNLOCKED),
    DRM_IOCTL_DEF(DRM_IOCTL_UNBLOCK, drm_noop,
                  DRM_AUTH | DRM_MASTER | DRM_ROOT_ONLY | DRM_UNLOCKED),
    DRM_IOCTL_DEF(DRM_IOCTL_AUTH_MAGIC, drm_authmagic, DRM_MASTER),

    DRM_IOCTL_DEF(DRM_IOCTL_SET_MASTER, drm_setmaster_ioctl, 0),
    DRM_IOCTL_DEF(DRM_IOCTL_DROP_MASTER, drm_dropmaster_ioctl, 0),

    DRM_IOCTL_DEF(DRM_IOCTL_ADD_DRAW, drm_noop,
                  DRM_AUTH | DRM_MASTER | DRM_ROOT_ONLY | DRM_UNLOCKED),
    DRM_IOCTL_DEF(DRM_IOCTL_RM_DRAW, drm_noop,
                  DRM_AUTH | DRM_MASTER | DRM_ROOT_ONLY | DRM_UNLOCKED),
    DRM_IOCTL_DEF(DRM_IOCTL_FINISH, drm_noop, DRM_AUTH | DRM_UNLOCKED),
    DRM_IOCTL_DEF(DRM_IOCTL_WAIT_VBLANK, drm_wait_vblank_ioctl, DRM_UNLOCKED | DRM_KMS),
    DRM_IOCTL_DEF(DRM_IOCTL_UPDATE_DRAW, drm_noop,
                  DRM_AUTH | DRM_MASTER | DRM_ROOT_ONLY | DRM_UNLOCKED),

    DRM_IOCTL_DEF(DRM_IOCTL_GEM_CLOSE, drm_gem_close_ioctl, DRM_RENDER_ALLOW),
    DRM_IOCTL_DEF(DRM_IOCTL_GEM_FLINK, drm_gem_flink_ioctl, DRM_AUTH),
    DRM_IOCTL_DEF(DRM_IOCTL_GEM_OPEN, drm_gem_open_ioctl, DRM_AUTH),

    DRM_IOCTL_DEF(DRM_IOCTL_MODE_GETRESOURCES, drm_mode_getresources,
                  DRM_MODESET_LOCK | DRM_UNLOCKED | DRM_KMS),

    DRM_IOCTL_DEF(DRM_IOCTL_PRIME_HANDLE_TO_FD, drm_prime_handle_to_fd_ioctl, DRM_RENDER_ALLOW),
    DRM_IOCTL_DEF(DRM_IOCTL_PRIME_FD_TO_HANDLE, drm_prime_fd_to_handle_ioctl, DRM_RENDER_ALLOW),

    DRM_IOCTL_DEF(DRM_IOCTL_MODE_GETPLANERESOURCES, drm_mode_getplane_res,
                  DRM_MODESET_LOCK | DRM_UNLOCKED | DRM_KMS),
    DRM_IOCTL_DEF(DRM_IOCTL_MODE_GETCRTC, drm_mode_getcrtc,
                  DRM_MODESET_LOCK | DRM_UNLOCKED | DRM_KMS),
    DRM_IOCTL_DEF(DRM_IOCTL_MODE_SETCRTC, drm_mode_setcrtc,
                  DRM_MASTER | DRM_MODESET_LOCK | DRM_UNLOCKED | DRM_KMS),
    DRM_IOCTL_DEF(DRM_IOCTL_MODE_GETPLANE, drm_mode_getplane,
                  DRM_MODESET_LOCK | DRM_UNLOCKED | DRM_KMS),
    DRM_IOCTL_DEF(DRM_IOCTL_MODE_SETPLANE, drm_mode_setplane,
                  DRM_MASTER | DRM_MODESET_LOCK | DRM_UNLOCKED | DRM_KMS),
    DRM_IOCTL_DEF(DRM_IOCTL_MODE_CURSOR, drm_mode_cursor_ioctl,
                  DRM_MASTER | DRM_MODESET_LOCK | DRM_UNLOCKED | DRM_KMS),
    DRM_IOCTL_DEF(DRM_IOCTL_MODE_GETGAMMA, drm_mode_gamma_get_ioctl,
                  DRM_MODESET_LOCK | DRM_UNLOCKED | DRM_KMS),
    DRM_IOCTL_DEF(DRM_IOCTL_MODE_SETGAMMA, drm_mode_gamma_set_ioctl,
                  DRM_MASTER | DRM_MODESET_LOCK | DRM_UNLOCKED | DRM_KMS),
    DRM_IOCTL_DEF(DRM_IOCTL_MODE_GETENCODER, drm_mode_getencoder,
                  DRM_MODESET_LOCK | DRM_UNLOCKED | DRM_KMS),
    DRM_IOCTL_DEF(DRM_IOCTL_MODE_GETCONNECTOR, drm_mode_getconnector,
                  DRM_MODESET_LOCK | DRM_UNLOCKED | DRM_KMS),
    DRM_IOCTL_DEF(DRM_IOCTL_MODE_ATTACHMODE, drm_noop, DRM_MASTER | DRM_UNLOCKED),
    DRM_IOCTL_DEF(DRM_IOCTL_MODE_DETACHMODE, drm_noop, DRM_MASTER | DRM_UNLOCKED),
    DRM_IOCTL_DEF(DRM_IOCTL_MODE_GETPROPERTY, drm_mode_getproperty_ioctl,
                  DRM_MODESET_LOCK | DRM_UNLOCKED | DRM_KMS),
    DRM_IOCTL_DEF(DRM_IOCTL_MODE_SETPROPERTY, drm_connector_property_set_ioctl,
                  DRM_MASTER | DRM_MODESET_LOCK | DRM_UNLOCKED | DRM_KMS),
    DRM_IOCTL_DEF(DRM_IOCTL_MODE_GETPROPBLOB, drm_mode_getblob_ioctl,
                  DRM_MODESET_LOCK | DRM_UNLOCKED | DRM_KMS),
    DRM_IOCTL_DEF(DRM_IOCTL_MODE_GETFB, drm_mode_getfb, DRM_MODESET_LOCK | DRM_UNLOCKED | DRM_KMS),
    DRM_IOCTL_DEF(DRM_IOCTL_MODE_GETFB2, drm_mode_getfb2_ioctl,
                  DRM_MODESET_LOCK | DRM_UNLOCKED | DRM_KMS),
    DRM_IOCTL_DEF(DRM_IOCTL_MODE_ADDFB, drm_mode_addfb_ioctl,
                  DRM_MODESET_LOCK | DRM_UNLOCKED | DRM_KMS),
    DRM_IOCTL_DEF(DRM_IOCTL_MODE_ADDFB2, drm_mode_addfb2_ioctl,
                  DRM_MODESET_LOCK | DRM_UNLOCKED | DRM_KMS),
    DRM_IOCTL_DEF(DRM_IOCTL_MODE_RMFB, drm_mode_rmfb_ioctl,
                  DRM_MODESET_LOCK | DRM_UNLOCKED | DRM_KMS),
    DRM_IOCTL_DEF(DRM_IOCTL_MODE_CLOSEFB, drm_mode_closefb_ioctl,
                  DRM_MODESET_LOCK | DRM_UNLOCKED | DRM_KMS),
    DRM_IOCTL_DEF(DRM_IOCTL_MODE_PAGE_FLIP, drm_mode_page_flip_ioctl,
                  DRM_MASTER | DRM_UNLOCKED | DRM_KMS | DRM_MODESET_LOCK),
    DRM_IOCTL_DEF(DRM_IOCTL_MODE_DIRTYFB, drm_mode_dirtyfb_ioctl,
                  DRM_MASTER | DRM_MODESET_LOCK | DRM_UNLOCKED | DRM_KMS),
    DRM_IOCTL_DEF(DRM_IOCTL_MODE_CREATE_DUMB, drm_mode_create_dumb_ioctl, 0),
    DRM_IOCTL_DEF(DRM_IOCTL_MODE_MAP_DUMB, drm_mode_mmap_dumb_ioctl, 0),
    DRM_IOCTL_DEF(DRM_IOCTL_MODE_DESTROY_DUMB, drm_mode_destroy_dumb_ioctl, 0),
    DRM_IOCTL_DEF(DRM_IOCTL_MODE_OBJ_GETPROPERTIES, drm_mode_obj_get_properties_ioctl,
                  DRM_MODESET_LOCK | DRM_UNLOCKED | DRM_KMS),
    DRM_IOCTL_DEF(DRM_IOCTL_MODE_OBJ_SETPROPERTY, drm_mode_obj_set_property_ioctl,
                  DRM_MASTER | DRM_MODESET_LOCK | DRM_UNLOCKED | DRM_KMS),
    DRM_IOCTL_DEF(DRM_IOCTL_MODE_CURSOR2, drm_mode_cursor2_ioctl,
                  DRM_MASTER | DRM_MODESET_LOCK | DRM_UNLOCKED | DRM_KMS),
    DRM_IOCTL_DEF(DRM_IOCTL_MODE_ATOMIC, drm_mode_atomic_ioctl,
                  DRM_MASTER | DRM_UNLOCKED | DRM_KMS | DRM_MODESET_LOCK),
    DRM_IOCTL_DEF(DRM_IOCTL_MODE_CREATEPROPBLOB, drm_mode_createblob_ioctl,
                  DRM_MODESET_LOCK | DRM_UNLOCKED | DRM_KMS),
    DRM_IOCTL_DEF(DRM_IOCTL_MODE_DESTROYPROPBLOB, drm_mode_destroyblob_ioctl,
                  DRM_MODESET_LOCK | DRM_UNLOCKED | DRM_KMS),

    DRM_IOCTL_DEF(DRM_IOCTL_SYNCOBJ_CREATE, drm_syncobj_create_ioctl,
                  DRM_RENDER_ALLOW | DRM_UNLOCKED),
    DRM_IOCTL_DEF(DRM_IOCTL_SYNCOBJ_DESTROY, drm_syncobj_destroy_ioctl,
                  DRM_RENDER_ALLOW | DRM_UNLOCKED),
    DRM_IOCTL_DEF(DRM_IOCTL_SYNCOBJ_HANDLE_TO_FD, drm_syncobj_handle_to_fd_ioctl,
                  DRM_RENDER_ALLOW | DRM_UNLOCKED),
    DRM_IOCTL_DEF(DRM_IOCTL_SYNCOBJ_FD_TO_HANDLE, drm_syncobj_fd_to_handle_ioctl,
                  DRM_RENDER_ALLOW | DRM_UNLOCKED),
    DRM_IOCTL_DEF(DRM_IOCTL_SYNCOBJ_TRANSFER, drm_syncobj_transfer_ioctl,
                  DRM_RENDER_ALLOW | DRM_UNLOCKED),
    DRM_IOCTL_DEF(DRM_IOCTL_SYNCOBJ_WAIT, drm_syncobj_wait_ioctl, DRM_RENDER_ALLOW | DRM_UNLOCKED),
    DRM_IOCTL_DEF(DRM_IOCTL_SYNCOBJ_TIMELINE_WAIT, drm_syncobj_timeline_wait_ioctl,
                  DRM_RENDER_ALLOW | DRM_UNLOCKED),
    DRM_IOCTL_DEF(DRM_IOCTL_SYNCOBJ_EVENTFD, drm_syncobj_eventfd_ioctl,
                  DRM_RENDER_ALLOW | DRM_UNLOCKED),
    DRM_IOCTL_DEF(DRM_IOCTL_SYNCOBJ_RESET, drm_syncobj_reset_ioctl,
                  DRM_RENDER_ALLOW | DRM_UNLOCKED),
    DRM_IOCTL_DEF(DRM_IOCTL_SYNCOBJ_SIGNAL, drm_syncobj_signal_ioctl,
                  DRM_RENDER_ALLOW | DRM_UNLOCKED),
    DRM_IOCTL_DEF(DRM_IOCTL_SYNCOBJ_TIMELINE_SIGNAL, drm_syncobj_timeline_signal_ioctl,
                  DRM_RENDER_ALLOW | DRM_UNLOCKED),
    DRM_IOCTL_DEF(DRM_IOCTL_SYNCOBJ_QUERY, drm_syncobj_query_ioctl,
                  DRM_RENDER_ALLOW | DRM_UNLOCKED),
    DRM_IOCTL_DEF(DRM_IOCTL_CRTC_GET_SEQUENCE, drm_crtc_get_sequence_ioctl,
                  DRM_MODESET_LOCK | DRM_UNLOCKED | DRM_KMS),
    DRM_IOCTL_DEF(DRM_IOCTL_CRTC_QUEUE_SEQUENCE, drm_crtc_queue_sequence_ioctl,
                  DRM_MODESET_LOCK | DRM_UNLOCKED | DRM_KMS),
    DRM_IOCTL_DEF(DRM_IOCTL_MODE_CREATE_LEASE, drm_mode_create_lease_ioctl,
                  DRM_MASTER | DRM_MODESET_LOCK | DRM_UNLOCKED | DRM_KMS),
    DRM_IOCTL_DEF(DRM_IOCTL_MODE_LIST_LESSEES, drm_mode_list_lessees_ioctl,
                  DRM_MASTER | DRM_MODESET_LOCK | DRM_UNLOCKED | DRM_KMS),
    DRM_IOCTL_DEF(DRM_IOCTL_MODE_GET_LEASE, drm_mode_get_lease_ioctl,
                  DRM_MASTER | DRM_MODESET_LOCK | DRM_UNLOCKED | DRM_KMS),
    DRM_IOCTL_DEF(DRM_IOCTL_MODE_REVOKE_LEASE, drm_mode_revoke_lease_ioctl,
                  DRM_MASTER | DRM_MODESET_LOCK | DRM_UNLOCKED | DRM_KMS),
};

#define DRM_CORE_IOCTLS ((int)(sizeof(drm_ioctls) / sizeof(drm_ioctls[0])))

/** The row of `nr` in `table` of `n` rows, or NULL. */
static const struct drm_ioctl_desc *ioctl_row(const struct drm_ioctl_desc *table, int n,
                                              uint32_t nr)
{
    int i;

    for (i = 0; i < n; i++) {
        if (UAPI_IOC_NR(table[i].cmd) == nr) {
            return &table[i];
        }
    }
    return NULL;
}

/** Whether `file` may call an ioctl of the flags `flags`, Linux's
 * drm_ioctl_permit: 0 or -EACCES. */
static int32_t ioctl_permit(uint32_t flags, const struct drm_file *file)
{
    int render = file->minor_type == DRM_MINOR_RENDER;

    if ((flags & DRM_ROOT_ONLY) && !(current_proc && proc_privileged(current_proc))) {
        return -EACCES;
    }
    if ((flags & DRM_AUTH) && !render && !file->authenticated) {
        return -EACCES;
    }
    if ((flags & DRM_MASTER) && !drm_is_current_master(file)) {
        return -EACCES;
    }
    if (!(flags & DRM_RENDER_ALLOW) && render) {
        return -EACCES;
    }
    return 0;
}

/** Whether `row` needs no big lock on `dev`, as ioctl_call judges it. */
static int ioctl_unlocked(struct drm_device *dev, const struct drm_ioctl_desc *row)
{
    return (row->flags & DRM_UNLOCKED) &&
           (!(row->flags & DRM_KMS) || drm_core_check_feature(dev, DRIVER_KMS_UNLOCKED));
}

/**
 * Call `row`'s handler on `kdata` for `file` if the file may call it. The
 * master rules and every handler not marked DRM_UNLOCKED run under the big
 * lock, and so does a DRM_KMS row on a device without DRIVER_KMS_UNLOCKED;
 * an unlocked row takes it only to ask whether the file is master, as
 * Linux's drm_is_current_master takes master_mutex. A DRM_MODESET_LOCK row
 * then holds the device's `mode_lock`, taken after the big lock and with
 * a signal ending the wait for it (-EINTR), and finds the device unplugged
 * only while it holds it, so that an unplug waits for it to end.
 */
static int32_t ioctl_call(struct drm_device *dev, const struct drm_ioctl_desc *row, void *kdata,
                          struct drm_file *file)
{
    int unlocked = ioctl_unlocked(dev, row);
    int big = !unlocked || (row->flags & DRM_MASTER);
    int mode = (row->flags & DRM_MODESET_LOCK) != 0;
    int32_t rc;

    if (big) {
        bkl_enter();
    }
    /* Linux's drm_file_update_pid: a file passed on is the user's now,
     * unless it was master, whose owner the master rules keep. */
    if (!file->was_master && current_proc) {
        file->pid = current_proc->tgid;
    }
    rc = dev->unplugged ? -ENODEV : ioctl_permit(row->flags, file);
    if (big && unlocked) {
        bkl_exit();
        big = 0;
    }
    if (rc == 0 && mode && (rc = drm_modeset_lock(dev)) == 0 && dev->unplugged) {
        rc = -ENODEV;
        drm_modeset_unlock(dev);
    }
    if (rc == 0) {
        rc = row->func(dev, kdata, file);
        if (mode) {
            drm_modeset_unlock(dev);
        }
    }
    if (big) {
        bkl_exit();
    }
    return rc;
}

/**
 * Dispatch `cmd` for the held file, copying its argument at `arg`: 0 or a
 * negative errno. An unlocked row runs without an inherited big lock too,
 * from its argument's copy in through its copy out, restoring the caller's
 * depth on every return; ioctl_call takes the lock for master permission.
 */
int32_t drm_ioctl(struct drm_file *file, uint32_t cmd, uintptr_t arg)
{
    struct drm_device *dev = file->dev;
    const struct drm_ioctl_desc *row;
    uint32_t nr = UAPI_IOC_NR(cmd);
    uint32_t in_size, out_size, ksize;
    uint32_t irqflags;
    char stack[DRM_IOCTL_STACK];
    char *kdata;
    int depth = 0;
    int32_t rc;

    if (dev->unplugged) {
        return -ENODEV;
    }
    if (UAPI_IOC_TYPE(cmd) != DRM_IOCTL_BASE) {
        return -ENOTTY;
    }
    if (nr >= DRM_COMMAND_BASE && nr < DRM_COMMAND_END) {
        row = dev->driver->ioctls ? ioctl_row(dev->driver->ioctls, dev->driver->num_ioctls, nr)
                                  : NULL;
    } else {
        row = ioctl_row(drm_ioctls, DRM_CORE_IOCTLS, nr);
    }
    if (!row || !row->func) {
        return -EINVAL;
    }
    /* A traced entry or another kernel caller may still hold the big lock. */
    if (ioctl_unlocked(dev, row)) {
        irqflags = kirq_save();
        depth = bkl_drop();
        kirq_restore(irqflags);
    }
    in_size = out_size = UAPI_IOC_SIZE(cmd);
    if (!(UAPI_IOC_DIR(cmd) & UAPI_IOC_DIR(row->cmd) & UAPI_IOC_WRITE)) {
        in_size = 0;
    }
    if (!(UAPI_IOC_DIR(cmd) & UAPI_IOC_DIR(row->cmd) & UAPI_IOC_READ)) {
        out_size = 0;
    }
    ksize = in_size > out_size ? in_size : out_size;
    if (UAPI_IOC_SIZE(row->cmd) > ksize) {
        ksize = UAPI_IOC_SIZE(row->cmd);
    }
    kdata = ksize <= sizeof(stack) ? stack : (char *)kmalloc(ksize);
    if (!kdata) {
        rc = -ENOMEM;
        goto restore;
    }
    if (in_size && copy_from_user(kdata, (const void __user *)arg, in_size) != 0) {
        rc = -EFAULT;
        goto out;
    }
    if (ksize > in_size) {
        k_memset(kdata + in_size, 0, ksize - in_size);
    }
    rc = ioctl_call(dev, row, kdata, file);
    if (out_size && copy_to_user((void __user *)arg, kdata, out_size) != 0) {
        rc = -EFAULT;
    }
out:
    if (kdata != stack) {
        kfree(kdata);
    }
restore:
    if (depth) {
        irqflags = kirq_save();
        bkl_take(depth);
        kirq_restore(irqflags);
    }
    return rc;
}

/* ------------------------------------------------------------------------
 * The firmware framebuffer's handoff
 */

int32_t drm_aperture_acquire_firmware(struct drm_device *dev, uint64_t base, uint64_t size,
                                      void (*remove)(struct drm_device *dev))
{
    int i, free = -1;

    if (aperture_native) {
        return -EBUSY;
    }
    for (i = 0; i < DRM_APERTURE_MAX; i++) {
        if (!apertures[i].dev) {
            if (free < 0) {
                free = i;
            }
        } else if (base < apertures[i].base + apertures[i].size &&
                   apertures[i].base < base + size) {
            return -EBUSY;
        }
    }
    if (free < 0) {
        return -EBUSY;
    }
    apertures[free].dev = dev;
    apertures[free].base = base;
    apertures[free].size = size;
    apertures[free].remove = remove;
    return 0;
}

void drm_aperture_release_firmware(struct drm_device *dev)
{
    int i;

    for (i = 0; i < DRM_APERTURE_MAX; i++) {
        if (apertures[i].dev == dev) {
            apertures[i].dev = NULL;
        }
    }
}

int drm_aperture_is_firmware(const struct drm_device *dev)
{
    int i;

    for (i = 0; i < DRM_APERTURE_MAX; i++) {
        if (apertures[i].dev == dev) {
            return 1;
        }
    }
    return 0;
}

int32_t drm_aperture_remove_conflicting(uint64_t base, uint64_t size, const char *name)
{
    int i;

    aperture_native = 1;
    for (i = 0; i < DRM_APERTURE_MAX; i++) {
        struct drm_aperture a = apertures[i];

        if (!a.dev || base >= a.base + a.size || a.base >= base + size) {
            continue;
        }
        /* The slot is freed first, so that a remove hook that registers
         * again, or removes another, sees a consistent table. */
        apertures[i].dev = NULL;
        klog(KLOG_INFO, "drm", "aperture: %s takes the framebuffer of %s", name,
             a.dev->driver ? a.dev->driver->ident.name : "?");
        drm_client_dev_unregister(a.dev);
        if (a.remove) {
            a.remove(a.dev);
        }
    }
    return 0;
}

/* ------------------------------------------------------------------------
 * The core's ioctls of identity and capabilities
 */

int32_t drm_noop(struct drm_device *dev, void *data, struct drm_file *file)
{
    (void)dev;
    (void)data;
    (void)file;
    return 0;
}

int32_t drm_invalid_op(struct drm_device *dev, void *data, struct drm_file *file)
{
    (void)dev;
    (void)data;
    (void)file;
    return -EINVAL;
}

/**
 * Copy `value` to the user's `buf` of `*len` bytes, cut to fit and without
 * a terminator, and set `*len` to its whole length, as Linux's
 * drm_copy_field: 0, or -EFAULT.
 */
static int32_t copy_field(char __user *buf, size_t *len, const char *value)
{
    size_t n = k_strlen(value);
    size_t cut = n > *len ? *len : n;

    *len = n;
    if (cut && buf && copy_to_user(buf, value, cut) != 0) {
        return -EFAULT;
    }
    return 0;
}

/** VERSION: the driver's version, and its name, date and description cut
 * to the user's lengths, which are set to the whole ones. */
int32_t drm_version(struct drm_device *dev, void *data, struct drm_file *file)
{
    struct drm_version *v = (struct drm_version *)data;
    const struct drm_ident *id = &dev->driver->ident;
    int32_t rc;

    (void)file;
    v->version_major = id->major;
    v->version_minor = id->minor;
    v->version_patchlevel = id->patchlevel;
    rc = copy_field(v->name, &v->name_len, id->name);
    if (rc == 0) {
        rc = copy_field(v->date, &v->date_len, id->date);
    }
    if (rc == 0) {
        rc = copy_field(v->desc, &v->desc_len, id->desc);
    }
    return rc;
}

/** GET_UNIQUE, as Linux 6.12: the bus name of the file's master record,
 * empty until SET_VERSION gave it, copied whole when it fits the user's
 * length, which is set to its length either way. libdrm's drmOpenByName
 * takes only a card whose answer is empty. */
int32_t drm_getunique(struct drm_device *dev, void *data, struct drm_file *file)
{
    struct drm_unique *u = (struct drm_unique *)data;
    size_t n = file->set_busid ? k_strlen(dev->unique) : 0;

    if (u->unique_len >= n && copy_to_user(u->unique, dev->unique, n) != 0) {
        return -EFAULT;
    }
    u->unique_len = n;
    return 0;
}

/** GET_CLIENT: index 0 is the caller, whose authentication libva reads;
 * every other index EINVAL, as Linux 6.12 keeps no list. */
int32_t drm_getclient(struct drm_device *dev, void *data, struct drm_file *file)
{
    struct drm_client *c = (struct drm_client *)data;

    (void)dev;
    if (c->idx != 0) {
        return -EINVAL;
    }
    c->auth = file->authenticated;
    c->pid = current_proc ? current_proc->tgid : 0;
    c->uid = DRM_OVERFLOW_UID;
    c->magic = 0;
    c->iocs = 0;
    return 0;
}

/** GET_STATS: no counters, as Linux 6.12. */
int32_t drm_getstats(struct drm_device *dev, void *data, struct drm_file *file)
{
    (void)dev;
    (void)file;
    k_memset(data, 0, sizeof(struct drm_stats));
    return 0;
}

/** GET_CAP, as Linux 6.12's drm_getcap: the caps of rendering for every
 * driver, the rest only for one with DRIVER_MODESET, EOPNOTSUPP without,
 * and EINVAL for a cap unknown. */
int32_t drm_getcap(struct drm_device *dev, void *data, struct drm_file *file)
{
    struct drm_get_cap *req = (struct drm_get_cap *)data;
    const struct drm_mode_config *mc = &dev->mode_config;

    (void)file;
    req->value = 0;
    switch (req->capability) {
    case DRM_CAP_TIMESTAMP_MONOTONIC:
        req->value = 1;
        return 0;
    case DRM_CAP_PRIME:
        req->value = DRM_PRIME_CAP_IMPORT | DRM_PRIME_CAP_EXPORT;
        return 0;
    case DRM_CAP_SYNCOBJ:
        req->value = drm_core_check_feature(dev, DRIVER_SYNCOBJ);
        return 0;
    case DRM_CAP_SYNCOBJ_TIMELINE:
        req->value = drm_core_check_feature(dev, DRIVER_SYNCOBJ_TIMELINE);
        return 0;
    default:
        break;
    }
    if (!drm_core_check_feature(dev, DRIVER_MODESET)) {
        return -EOPNOTSUPP;
    }
    switch (req->capability) {
    case DRM_CAP_DUMB_BUFFER:
        req->value = dev->driver->dumb_create != NULL;
        break;
    case DRM_CAP_VBLANK_HIGH_CRTC:
    case DRM_CAP_CRTC_IN_VBLANK_EVENT:
        req->value = 1;
        break;
    case DRM_CAP_ADDFB2_MODIFIERS:
        req->value = !mc->fb_modifiers_not_supported;
        break;
    case DRM_CAP_DUMB_PREFERRED_DEPTH:
        req->value = mc->preferred_depth;
        break;
    case DRM_CAP_DUMB_PREFER_SHADOW:
        req->value = (uint64_t)mc->prefer_shadow;
        break;
    case DRM_CAP_ASYNC_PAGE_FLIP:
        req->value = (uint64_t)mc->async_page_flip;
        break;
    case DRM_CAP_PAGE_FLIP_TARGET:
        /* Linux answers 1 only when every CRTC has page_flip_target, which
         * no CRTC here has. */
        req->value = mc->num_crtc == 0;
        break;
    case DRM_CAP_CURSOR_WIDTH:
        req->value = mc->cursor_width ? mc->cursor_width : 64;
        break;
    case DRM_CAP_CURSOR_HEIGHT:
        req->value = mc->cursor_height ? mc->cursor_height : 64;
        break;
    case DRM_CAP_ATOMIC_ASYNC_PAGE_FLIP:
        req->value = drm_core_check_feature(dev, DRIVER_ATOMIC) && mc->async_page_flip;
        break;
    default:
        return -EINVAL;
    }
    return 0;
}

/** SET_CLIENT_CAP, as Linux 6.12's drm_setclientcap, with its refusal of
 * atomic to a program whose name begins with X, the modesetting DDX. */
int32_t drm_setclientcap(struct drm_device *dev, void *data, struct drm_file *file)
{
    struct drm_set_client_cap *req = (struct drm_set_client_cap *)data;

    if (!drm_core_check_feature(dev, DRIVER_MODESET)) {
        return -EOPNOTSUPP;
    }
    switch (req->capability) {
    case DRM_CLIENT_CAP_STEREO_3D:
        if (req->value > 1) {
            return -EINVAL;
        }
        file->stereo_allowed = (int)req->value;
        break;
    case DRM_CLIENT_CAP_UNIVERSAL_PLANES:
        if (req->value > 1) {
            return -EINVAL;
        }
        file->universal_planes = (int)req->value;
        break;
    case DRM_CLIENT_CAP_ATOMIC:
        if (!drm_core_check_feature(dev, DRIVER_ATOMIC)) {
            return -EOPNOTSUPP;
        }
        if (current_proc && current_proc->comm[0] == 'X' && req->value == 1) {
            klog(KLOG_INFO, "drm", "broken atomic modeset userspace detected, disabling atomic");
            return -EOPNOTSUPP;
        }
        if (req->value > 2) {
            return -EINVAL;
        }
        file->atomic = (int)req->value;
        file->universal_planes = (int)req->value;
        file->aspect_ratio = (int)req->value;
        break;
    case DRM_CLIENT_CAP_ASPECT_RATIO:
        if (req->value > 1) {
            return -EINVAL;
        }
        file->aspect_ratio = (int)req->value;
        break;
    case DRM_CLIENT_CAP_WRITEBACK_CONNECTORS:
        if (!file->atomic || req->value > 1) {
            return -EINVAL;
        }
        file->writeback_connectors = (int)req->value;
        break;
    case DRM_CLIENT_CAP_CURSOR_PLANE_HOTSPOT:
        if (!drm_core_check_feature(dev, DRIVER_CURSOR_HOTSPOT)) {
            return -EOPNOTSUPP;
        }
        if (!file->atomic || req->value > 1) {
            return -EINVAL;
        }
        file->supports_virtualized_cursor_plane = (int)req->value;
        break;
    default:
        return -EINVAL;
    }
    return 0;
}

/** Give the master record `master_id` of `dev` its bus name, by marking
 * each of its files. */
static void set_busid(struct drm_device *dev, uint32_t master_id)
{
    struct drm_file *f;

    for (f = dev->files; f; f = f->dev_next) {
        if (f->master_id == master_id) {
            f->set_busid = 1;
        }
    }
}

/** SET_VERSION, as Linux 6.12: EINVAL for an interface or a driver version
 * this one is not; the versions it has go back either way. An interface of
 * 1.1 or later gives the master's record its bus name, Linux's
 * drm_set_busid, which GET_UNIQUE and debugfs's `name` show to each file of
 * the record. */
int32_t drm_setversion(struct drm_device *dev, void *data, struct drm_file *file)
{
    struct drm_set_version *sv = (struct drm_set_version *)data;
    const struct drm_ident *id = &dev->driver->ident;
    int32_t rc = 0;

    if (sv->drm_di_major != -1 && (sv->drm_di_major != DRM_IF_MAJOR || sv->drm_di_minor < 0 ||
                                   sv->drm_di_minor > DRM_IF_MINOR)) {
        rc = -EINVAL;
    } else {
        if (sv->drm_di_major != -1 && sv->drm_di_minor >= 1) {
            set_busid(dev, file->master_id);
        }
        if (sv->drm_dd_major != -1 && (sv->drm_dd_major != id->major || sv->drm_dd_minor < 0 ||
                                       sv->drm_dd_minor > id->minor)) {
            rc = -EINVAL;
        }
    }
    sv->drm_di_major = DRM_IF_MAJOR;
    sv->drm_di_minor = DRM_IF_MINOR;
    sv->drm_dd_major = id->major;
    sv->drm_dd_minor = id->minor;
    return rc;
}
