/*
 * shmem.c: the virtio transport over one region of memory shared with a
 * VM, the test bed's way to Rach2's GPU in a Linux VM (vgpushm.h), and its
 * control node, /dev/vgpu-shm, through which ivshmemd hands it the region.
 */

#include "virtio.h"

#include "../arch/arch.h"
#include "../drm/drm.h"
#include "../fs/devfs.h"
#include "../fs/fd.h"
#include "../fs/file.h"
#include "../fs/tmpfs.h"
#include "../fs/vfs.h"
#include "../io/eventfd.h"
#include "../klog.h"
#include "../lib/kerrno.h"
#include "../lib/kirq.h"
#include "../lib/kstring.h"
#include "../mm/kheap.h"
#include "../mm/paging.h"
#include "../mm/shmem.h"
#include "../mm/usercopy.h"
#include "../proc/process.h"
#include "../proc/sched.h"
#include "../proc/wait_queue.h"
#include "../timer.h"

#define VGS_MB() arch_dma_fence()
#define VGS_IMPLEMENTATION
#include "vgpushm.h"

/* Blobs map from the VM's pages through Rach2's KVM, which the x86-64
 * kernel alone has; elsewhere VGS_TF_PAGES is never accepted. */
#if defined(__x86_64__) && !defined(RACH2_HOSTTEST)
#include "../kvm/kvm.h"
#define VSHM_PAGES 1
#else
#define VSHM_PAGES 0
#endif

/* The ring area: a page for each queue, which holds the core's largest. */
#define VSHM_RING_LEN (VGS_QUEUES * VGS_PAGE)

/* How long the thread sleeps between looks at the region: a tick when it
 * polls, and with doorbells a tenth of a second, a look that finds work no
 * doorbell announced counting in `polls`. */
#define VSHM_POLL_TICKS MS_TO_TICKS(1)
#define VSHM_IDLE_TICKS MS_TO_TICKS(100)

/* The most the driver waits for the device to take its setup. */
#define VSHM_ANSWER_TICKS MS_TO_TICKS(10000)

/* A bind refused is tried again after a second. */
#define VSHM_RETRY_TICKS MS_TO_TICKS(1000)

struct vshm;

/* A doorbell the device rings: the hook on its eventfd and the vector. */
struct vshm_bell {
    struct eventfd_hook hook; /* first, so that the hook is the record */
    struct vshm *vs;
    struct eventfd *ev;
    uint32_t vector;
    int hooked; /* on the eventfd's list */
};

/* A frame of the region and its page, in the table sorted by frame. */
struct vshm_rev {
    uintptr_t phys;
    uint32_t page;
};

/*
 * The transport's record of one attached region. `refs` counts the attach
 * and each bind of virtio-gpu until that bind's virtio_put, and each map
 * of the VM's pages until its map_free. The record is freed with the last;
 * the detach gives back the region's pages and window, and the driver's
 * and maps' remaining references keep the record until they let go. A
 * record detached while objects still hold pages of its heap, as a buffer
 * whose dma-buf exists does through a reset, keeps its heap's map and table of
 * frames until they give the last back, and a later region over the same
 * memory leaves those frames out of its heap meanwhile.
 * `me` is the driver's block as last stored and `dev` the device's as last
 * read; both are this file's own copies.
 */
struct vshm {
    struct virtio_dev vdev; /* first, so that the device is the record */
    uint32_t refs;
    int attached; /* the region is mapped and its frames held */
    int stop;     /* 0, or 1 + VGS_DETACH_*: the thread is to detach */
    int bound;    /* virtio-gpu is bound over it */
    volatile uint8_t *base;
    uint64_t size;
    uint32_t npages;
    uintptr_t *frames; /* by page, each with a reference held */
    struct vshm_rev *rev;
    struct vgs_layout l;
    struct vgs_driver me;
    struct vgs_device dev;
    uint32_t events; /* the device's events_seq last passed to the driver */
    uint8_t status;  /* the virtio status byte the core sees */
    int doorbells;   /* both sides accepted VGS_TF_DOORBELL */
    int bells;       /* the four eventfds are held and none has closed */
    struct vshm_bell in[2];
    struct eventfd *out[2];
    uint32_t *heap_map; /* a bit for each page of the heap in use */
    uint32_t heap_pages, heap_used, heap_rover;
    struct vshm *gone_next; /* on vshm_kept, while `gone` */
    int gone;               /* detached with heap pages objects still hold */
    uintptr_t window;       /* where the window was, while `gone` */
    struct spinlock heap_lock;
    uint32_t binds, failures;
    uint64_t rung[2], woken[2], polls, refused;
    uint64_t maps, map_pages, map_live, revoked; /* as struct vgs_info has them */
};

/*
 * A blob of the VM's memory mapped from its pages: KVM's map of them, the
 * record whose counts it moves, and its owner's revoke, which the map's
 * taking back calls through `vshm_map_revoked`.
 */
struct virtio_map {
#if VSHM_PAGES
    struct kvm_gpamap *m;
    void (*revoke)(void *owner);
    void *owner;
#endif
    struct vshm *vs;
    uint64_t pages;
    int live;
};

static struct vshm *vshm_cur;         /* the region attached, or detaching */
static struct vshm *vshm_kept;        /* detached records whose heap pages objects hold */
static volatile uint32_t vshm_rang;   /* a bit for each vector rung since the thread looked */
static wait_queue_t vshm_wq;          /* the thread sleeps here */
static wait_queue_t vshm_detached_wq; /* a detacher waits here */
#ifndef RACH2_HOSTTEST
static int vshm_ready; /* the queues are made */
static process_t *vshm_thread;
#endif

/* What a sleep of the thread waits for: a doorbell, a detach or `due`. */
struct vshm_wait {
    struct vshm *vs;
    uint64_t due;
};

/** The transport's record of the device `d`. */
static struct vshm *to_vs(struct virtio_dev *d)
{
    return (struct vshm *)d->transport;
}

/* ------------------------------------------------------------------------
 * The region's blocks
 */

/** Store the driver's block from `vs->me`: its other fields, then its
 * epoch, then its state, as the transport's rules order them, and ring the
 * control vector. */
static void vshm_store(struct vshm *vs)
{
    volatile struct vgs_driver *v = VGS_DRIVER(vs->base);
    const uint8_t *src = (const uint8_t *)&vs->me;
    volatile uint8_t *dst = (volatile uint8_t *)v;
    uint32_t i;

    v->state = VGS_DRV_RESET;
    VGS_MB();
    for (i = 8; i < sizeof(vs->me); i++) {
        dst[i] = src[i];
    }
    VGS_MB();
    v->epoch = vs->me.epoch;
    VGS_MB();
    v->state = vs->me.state;
    VGS_MB();
    if (vs->bells) {
        eventfd_signal(vs->out[VGS_VEC_CONTROL]);
        vs->rung[VGS_VEC_CONTROL]++;
    }
}

/** Read the device's block into `vs->dev`: 1 when it answers the driver's
 * epoch, else 0, with the copy left as it was. */
static int vshm_snap(struct vshm *vs)
{
    struct vgs_device d;

    if (vgs_snap_device(vs->base, &d) != VGS_OK || d.epoch != vs->me.epoch) {
        return 0;
    }
    vs->dev = d;
    return 1;
}

/** Start a new epoch: the rings emptied and the driver's block stored with
 * the next epoch, never 0, in `state`, RESET or, at a detach, GONE. */
static void vshm_epoch(struct vshm *vs, uint32_t state)
{
    uint32_t epoch = vs->me.epoch == 0xFFFFFFFFu ? 1u : vs->me.epoch + 1u, peer = vs->me.peer;

    k_memset((void *)(uintptr_t)(vs->base + vs->l.ring_off), 0, (size_t)vs->l.ring_len);
    k_memset(&vs->me, 0, sizeof(vs->me));
    k_memset(&vs->dev, 0, sizeof(vs->dev));
    vs->me.epoch = epoch;
    vs->me.state = state;
    vs->me.peer = peer;
    vs->doorbells = 0;
    vs->events = 0;
    vshm_store(vs);
}

/* ------------------------------------------------------------------------
 * The heap
 */

/** Whether heap page `i` is in use; under the heap's lock. */
static int heap_bit(const struct vshm *vs, uint32_t i)
{
    return (vs->heap_map[i / 32u] >> (i % 32u)) & 1u;
}

/** Mark `n` heap pages from `i` used, or free when `used` is 0. */
static void heap_mark(struct vshm *vs, uint32_t i, uint32_t n, int used)
{
    for (; n > 0; i++, n--) {
        if (used) {
            vs->heap_map[i / 32u] |= 1u << (i % 32u);
        } else {
            vs->heap_map[i / 32u] &= ~(1u << (i % 32u));
        }
    }
}

/**
 * Take a run of the heap's pages that follow each other, at most `n` and at
 * least `least` of them, the first fit from the rover, which moves past
 * it, so that pieces taken one after another follow each other too and a
 * chain of them is one descriptor; its length goes in `*got`. Returns the
 * first heap page, or the heap's size when no such run is free: a full
 * region is ENOMEM to the driver, never memory from elsewhere.
 */
static uint32_t heap_take(struct vshm *vs, uint32_t n, uint32_t least, uint32_t *got)
{
    uint32_t flags, pass, i, k, start;

    flags = spin_lock_irqsave(&vs->heap_lock);
    for (pass = 0; pass < 2 && least > 0 && least <= n && n <= vs->heap_pages; pass++) {
        start = pass == 0 ? vs->heap_rover : 0;
        for (i = start; i + least <= vs->heap_pages; i++) {
            for (k = 0; k < n && i + k < vs->heap_pages && !heap_bit(vs, i + k); k++) {
            }
            if (k >= least) {
                heap_mark(vs, i, k, 1);
                vs->heap_used += k;
                vs->heap_rover = i + k < vs->heap_pages ? i + k : 0;
                spin_unlock_irqrestore(&vs->heap_lock, flags);
                *got = k;
                return i;
            }
            i += k;
        }
    }
    spin_unlock_irqrestore(&vs->heap_lock, flags);
    *got = 0;
    return vs->heap_pages;
}

/** Count in `refused` what the heap could not give. */
static void heap_refused(struct vshm *vs)
{
    uint32_t flags = spin_lock_irqsave(&vs->heap_lock);

    vs->refused++;
    spin_unlock_irqrestore(&vs->heap_lock, flags);
}

/** The heap's free pages. */
static uint32_t heap_free_pages(struct vshm *vs)
{
    uint32_t flags = spin_lock_irqsave(&vs->heap_lock), n = vs->heap_pages - vs->heap_used;

    spin_unlock_irqrestore(&vs->heap_lock, flags);
    return n;
}

/** Give back `n` heap pages from `i`, each that is in use. */
static void heap_give(struct vshm *vs, uint32_t i, uint32_t n)
{
    uint32_t flags = spin_lock_irqsave(&vs->heap_lock);

    for (; n > 0 && i < vs->heap_pages; i++, n--) {
        if (heap_bit(vs, i)) {
            heap_mark(vs, i, 1, 0);
            vs->heap_used--;
        }
    }
    spin_unlock_irqrestore(&vs->heap_lock, flags);
}

/** Give back heap page `i` if it is in use: 1 when it was, else 0. */
static int heap_drop(struct vshm *vs, uint32_t i)
{
    uint32_t flags = spin_lock_irqsave(&vs->heap_lock);
    int used = i < vs->heap_pages && heap_bit(vs, i);

    if (used) {
        heap_mark(vs, i, 1, 0);
        vs->heap_used--;
    }
    spin_unlock_irqrestore(&vs->heap_lock, flags);
    return used;
}

/** The region's page of heap page `i`. */
static uint32_t heap_page(const struct vshm *vs, uint32_t i)
{
    return (uint32_t)(vs->l.heap_off / VGS_PAGE) + i;
}

/** The region's page whose frame is `phys`, or `npages` when the region
 * has no such frame: a binary search of the table sorted by frame. */
static uint32_t vshm_page_of(const struct vshm *vs, uintptr_t phys)
{
    uint32_t lo = 0, hi = vs->npages, mid;

    phys &= ~(uintptr_t)(VGS_PAGE - 1);
    while (lo < hi) {
        mid = lo + (hi - lo) / 2u;
        if (vs->rev[mid].phys == phys) {
            return vs->rev[mid].page;
        }
        if (vs->rev[mid].phys < phys) {
            lo = mid + 1u;
        } else {
            hi = mid;
        }
    }
    return vs->npages;
}

/** Sort the table of frames by frame, a heap sort, so that a region of
 * 16384 pages sorts in a few hundred thousand steps whatever the order of
 * its frames. */
static void vshm_sort(struct vshm_rev *a, uint32_t n)
{
    uint32_t start, end, root, child;
    struct vshm_rev t;

    if (n < 2) {
        return;
    }
    for (start = n / 2u; start-- > 0;) {
        for (root = start; (child = 2u * root + 1u) < n; root = child) {
            if (child + 1u < n && a[child].phys < a[child + 1u].phys) {
                child++;
            }
            if (a[root].phys >= a[child].phys) {
                break;
            }
            t = a[root];
            a[root] = a[child];
            a[child] = t;
        }
    }
    for (end = n - 1u; end > 0; end--) {
        t = a[0];
        a[0] = a[end];
        a[end] = t;
        for (root = 0; (child = 2u * root + 1u) < end; root = child) {
            if (child + 1u < end && a[child].phys < a[child + 1u].phys) {
                child++;
            }
            if (a[root].phys >= a[child].phys) {
                break;
            }
            t = a[root];
            a[root] = a[child];
            a[child] = t;
        }
    }
}

/* ------------------------------------------------------------------------
 * Waiting in the thread
 */

/** Whether the thread's sleep should end: a doorbell, a detach, or its
 * time. */
static int vshm_woken(void *arg)
{
    const struct vshm_wait *w = (const struct vshm_wait *)arg;

    return vshm_rang != 0 || w->vs->stop || tick_count() >= w->due;
}

/** Sleep the thread at most `ticks`, until a doorbell or a detach. */
static void vshm_sleep(struct vshm *vs, uint32_t ticks)
{
    struct vshm_wait w;
    uint32_t flags;

    w.vs = vs;
    w.due = tick_count() + ticks;
    flags = kirq_save();
    current_proc->wakeup_tick = w.due;
    kirq_restore(flags);
    (void)wq_wait_event_idle(&vshm_wq, vshm_woken, &w);
    current_proc->wakeup_tick = 0;
}

/** Take the vectors rung since the thread last looked. */
static uint32_t vshm_take_rang(void)
{
    uint32_t flags = kirq_save(), r = vshm_rang;

    vshm_rang = 0;
    kirq_restore(flags);
    return r;
}

/**
 * Wait until the device's block answers the driver's epoch in `want`, or
 * for `ticks` at most, 0 for no bound, ending early at a detach. Returns 1
 * with `vs->dev` read when it does, else 0. Called by the thread, which
 * binds the driver. A doorbell rung while the thread looks ends its next
 * sleep at once, so a device that keeps ringing without answering would
 * keep the thread from sleeping; the tick does not switch a kernel thread,
 * so each look that fails ends at a service point, as vhost's worker calls
 * cond_resched between works (drivers/vhost/vhost.c:440 in 6.12.115).
 */
static int vshm_await(struct vshm *vs, uint32_t want, uint32_t ticks)
{
    uint64_t end = tick_count() + ticks;

    for (;;) {
        if (vshm_snap(vs) && (vs->dev.state == want || vs->dev.state == VGS_DEV_FAILED)) {
            return vs->dev.state == want;
        }
        if (vs->stop || (ticks != 0 && tick_count() >= end)) {
            return 0;
        }
        vshm_sleep(vs, vs->bells ? VSHM_IDLE_TICKS : VSHM_POLL_TICKS);
        (void)vshm_take_rang();
        sched_service_point();
    }
}

/* ------------------------------------------------------------------------
 * The transport's operations (virtio.h)
 */

/** The status byte as the core last set it, with FAILED where the device
 * refused the setup. */
static uint8_t vs_get_status(struct virtio_dev *d)
{
    return to_vs(d)->status;
}

/**
 * The status byte, which the handshake carries: 0 starts a new epoch, the
 * device answering it with its offer; FEATURES_OK is kept only for features
 * within the offer; DRIVER_OK stores the driver's setup and waits for the
 * device to take it, adding FAILED when it refuses it or does not answer.
 */
static void vs_set_status(struct virtio_dev *d, uint8_t status)
{
    struct vshm *vs = to_vs(d);
    uint8_t added = (uint8_t)(status & ~vs->status);

    if (!vs->attached) {
        vs->status = status;
        return;
    }
    if (status == 0) {
        vshm_epoch(vs, vs->stop ? VGS_DRV_GONE : VGS_DRV_RESET);
        vs->status = 0;
        return;
    }
    if ((added & VIRTIO_STATUS_FEATURES_OK) &&
        (vs->me.features & ~(vs->dev.features & VGS_F_ALLOWED)) != 0) {
        status = (uint8_t)(status & ~VIRTIO_STATUS_FEATURES_OK);
    }
    vs->status = status;
    if (added & VIRTIO_STATUS_DRIVER_OK) {
        vs->me.state = VGS_DRV_OK;
        vshm_store(vs);
        if (!vshm_await(vs, VGS_DEV_RUNNING, VSHM_ANSWER_TICKS)) {
            klog(KLOG_WARN, "vgpu-shm", "refused: epoch %u: %s", (unsigned)vs->me.epoch,
                 vs->dev.state == VGS_DEV_FAILED ? vgs_strerror((int)vs->dev.error)
                                                 : "no answer to the setup");
            vs->status |= VIRTIO_STATUS_FAILED;
            return;
        }
        vs->doorbells = (vs->me.transport_features & VGS_TF_DOORBELL) != 0;
        klog(KLOG_INFO, "vgpu-shm", "running: epoch %u, device peer %u, %s", (unsigned)vs->me.epoch,
             (unsigned)vs->dev.peer, vs->doorbells ? "doorbells" : "polling");
    }
}

/**
 * The device's offer for the driver's epoch, waited for without bound,
 * since the VM may still be booting. At a detach, or for an offer the
 * transport's checks refuse, 0 with FAILED added to the status, which ends
 * the core's bring-up.
 */
static uint64_t vs_get_features(struct virtio_dev *d)
{
    struct vshm *vs = to_vs(d);
    int err;

    if (!vs->attached || !vshm_await(vs, VGS_DEV_READY, 0)) {
        vs->status |= VIRTIO_STATUS_FAILED;
        return 0;
    }
    err = vgs_check_offer(&vs->dev, vs->me.epoch, vs->me.peer);
    if (err != VGS_OK) {
        klog(KLOG_WARN, "vgpu-shm", "refused: epoch %u: the offer: %s", (unsigned)vs->me.epoch,
             vgs_strerror(err));
        vs->status |= VIRTIO_STATUS_FAILED;
        return 0;
    }
    return vs->dev.features;
}

/** The features the driver takes, and the doorbells when both sides have
 * them. */
static void vs_set_features(struct virtio_dev *d, uint64_t features)
{
    struct vshm *vs = to_vs(d);

    vs->me.features = features;
    vs->me.transport_features = vs->bells ? vs->dev.transport_features & VGS_TF_DOORBELL : 0;
    if (VSHM_PAGES && vs->bells) {
        vs->me.transport_features |= vs->dev.transport_features & VGS_TF_PAGES;
    }
    vs->vdev.pages = (vs->me.transport_features & VGS_TF_PAGES) != 0;
}

/** The configuration never changes within an epoch but for its events,
 * which the driver reads and clears as one word. */
static uint32_t vs_config_generation(struct virtio_dev *d)
{
    (void)d;
    return 0;
}

/**
 * virtio-gpu's configuration (virtio 1.2, 5.7.4) made from the device's
 * block: events_read the display event while events_seq differs from what
 * the driver has seen, events_clear 0, and the scanouts and capsets.
 */
static void vs_config_read(struct virtio_dev *d, uint32_t offset, void *buf, uint32_t len)
{
    struct vshm *vs = to_vs(d);
    uint32_t cfg[4];
    uint32_t i;
    uint8_t *out = (uint8_t *)buf;

    cfg[0] = vs->dev.events_seq != vs->me.events_seen ? 1u : 0u;
    cfg[1] = 0;
    cfg[2] = vs->dev.num_scanouts;
    cfg[3] = vs->dev.num_capsets;
    for (i = 0; i < len; i++) {
        out[i] = offset + i < sizeof(cfg) ? ((const uint8_t *)cfg)[offset + i] : 0;
    }
}

/** A write of events_clear marks the device's events seen, as one store of
 * the driver block's word, which the device does not act on. */
static void vs_config_write(struct virtio_dev *d, uint32_t offset, const void *buf, uint32_t len)
{
    struct vshm *vs = to_vs(d);
    uint32_t v = 0;

    if (offset != 4 || len != 4 || !vs->attached) {
        return;
    }
    k_memcpy(&v, buf, 4);
    if (v & 1u) {
        vs->me.events_seen = vs->dev.events_seq;
        VGS_DRIVER(vs->base)->events_seen = vs->me.events_seen;
        VGS_MB();
    }
}

/** The device's largest queue for both of virtio-gpu's queues. */
static uint16_t vs_queue_max(struct virtio_dev *d, uint16_t index)
{
    struct vshm *vs = to_vs(d);

    return index < VGS_QUEUES ? vs->dev.limits.max_queue : 0;
}

/** Queue `index`'s rings: its page of the ring area, at the offset that is
 * the device's address of it. */
static void *vs_ring_alloc(struct virtio_dev *d, uint16_t index, uint32_t bytes, uint64_t *addr)
{
    struct vshm *vs = to_vs(d);
    uint64_t off = vs->l.ring_off + (uint64_t)index * VGS_PAGE;

    if (!vs->attached || index >= VGS_QUEUES || bytes > VGS_PAGE) {
        return NULL;
    }
    k_memset((void *)(uintptr_t)(vs->base + off), 0, VGS_PAGE);
    *addr = off;
    return (void *)(uintptr_t)(vs->base + off);
}

/** Record queue `vq`'s setup for the device, checked as the device will
 * check it, with the device's bounds on a chain. 0, or -EINVAL. */
static int vs_queue_enable(struct virtio_dev *d, struct virtqueue *vq)
{
    struct vshm *vs = to_vs(d);
    struct vgs_queue *q;

    if (!vs->attached || vq->index >= VGS_QUEUES) {
        return -EINVAL;
    }
    q = &vs->me.queue[vq->index];
    q->size = vq->size;
    q->reserved0 = 0;
    q->reserved1 = 0;
    q->desc = vq->desc_addr;
    q->avail = vq->avail_addr;
    q->used = vq->used_addr;
    if (vgs_check_queue(&vs->l, q, vs->dev.limits.max_queue) != VGS_OK) {
        k_memset(q, 0, sizeof(*q));
        return -EINVAL;
    }
    vq->vector = VGS_VEC_QUEUE;
    vq->max_chain =
        (uint16_t)(vs->dev.limits.max_chain < vq->size ? vs->dev.limits.max_chain : vq->size);
    vq->max_bytes = vs->dev.limits.max_cmd;
    return 0;
}

/** Ring the device's queue vector when doorbells were accepted; otherwise
 * the device finds the buffers when it next looks. A handler may call it. */
static void vs_notify(struct virtio_dev *d, struct virtqueue *vq)
{
    struct vshm *vs = to_vs(d);

    (void)vq;
    if (vs->attached && vs->doorbells && vs->bells) {
        eventfd_signal(vs->out[VGS_VEC_QUEUE]);
        vs->rung[VGS_VEC_QUEUE]++;
    }
}

/** Nothing to prepare: the doorbells were given at the attach. 0, or
 * -EINVAL for more queues than the region has. */
static int vs_irq_setup(struct virtio_dev *d, uint16_t nqueues)
{
    (void)d;
    return nqueues <= VGS_QUEUES ? 0 : -EINVAL;
}

/** Nothing to start: the thread serves the doorbells. */
static void vs_irq_start(struct virtio_dev *d)
{
    (void)d;
}

/** Nothing to stop: the doorbells are given back at the detach. */
static void vs_irq_release(struct virtio_dev *d)
{
    (void)d;
}

/** Zeroed pages of the heap that follow each other, `bytes` rounded up to
 * pages, with the offset of the first as the device's address; NULL when the
 * heap has no such run. */
static void *vs_dma_alloc(struct virtio_dev *d, uint32_t bytes, uint64_t *addr)
{
    struct vshm *vs = to_vs(d);
    uint32_t n = bytes / VGS_PAGE + (bytes % VGS_PAGE != 0), i, got;
    uint64_t off;

    if (!vs->attached || n == 0) {
        return NULL;
    }
    i = heap_take(vs, n, n, &got);
    if (i >= vs->heap_pages) {
        heap_refused(vs);
        return NULL;
    }
    off = (uint64_t)heap_page(vs, i) * VGS_PAGE;
    k_memset((void *)(uintptr_t)(vs->base + off), 0, (size_t)n * VGS_PAGE);
    *addr = off;
    return (void *)(uintptr_t)(vs->base + off);
}

static void gone_give(struct vshm *vs, uintptr_t frame);
static void vshm_gone_free(struct vshm *vs);

/** Give back the heap pages of `bytes` at `p`, after the detach too, which
 * keeps the record's heap while they are out; nothing for a queue's rings,
 * which the ring area holds. */
static void vs_dma_free(struct virtio_dev *d, void *p, uint32_t bytes)
{
    struct vshm *vs = to_vs(d);
    uintptr_t at = (uintptr_t)p, base = vs->gone ? vs->window : (uintptr_t)vs->base;
    uint32_t n = bytes / VGS_PAGE + (bytes % VGS_PAGE != 0), i, first;

    if ((!vs->attached && !vs->gone) || at < base + vs->l.heap_off || at >= base + vs->size) {
        return;
    }
    first = (uint32_t)((at - base - vs->l.heap_off) / VGS_PAGE);
    if (vs->attached) {
        heap_give(vs, first, n);
        return;
    }
    for (i = first; i < first + n && i < vs->heap_pages; i++) {
        gone_give(vs, vs->frames[heap_page(vs, i)]);
    }
    if (vs->heap_used == 0) {
        vshm_gone_free(vs);
    }
}

/** No shared memory region beside the transport's own: -ENOENT, so that
 * virtio-gpu has no host-visible memory. */
static int vs_shm_region(struct virtio_dev *d, uint8_t id, uint64_t *addr, uint64_t *len)
{
    (void)d;
    (void)id;
    (void)addr;
    (void)len;
    return -ENOENT;
}

/**
 * Give `sh` the `n` heap pages of the run from heap page `first` as its
 * pages from `at` on: each frame zeroed through the region's window and
 * adopted by `sh` with a reference of its own. 0, or -ENOMEM with the
 * pages not adopted given back to the heap.
 */
static int backing_adopt(struct vshm *vs, struct shmem *sh, uint32_t at, uint32_t first, uint32_t n)
{
    uint32_t i;
    uintptr_t frame, got;

    k_memset((void *)(uintptr_t)(vs->base + (uint64_t)heap_page(vs, first) * VGS_PAGE), 0,
             (size_t)n * VGS_PAGE);
    for (i = 0; i < n; i++) {
        frame = vs->frames[heap_page(vs, first + i)];
        frame_ref(frame);
        got = shmem_adopt(sh, at + i, frame);
        if (got != frame) {
            if (got) {
                frame_unref(got);
            }
            heap_give(vs, first + i, n - i);
            return -ENOMEM;
        }
        frame_unref(got); /* the mapping's reference shmem_adopt adds */
    }
    return 0;
}

/** Give the heap back each of the first `n` pages of `sh` that is the
 * region's frame for a heap page. */
static void backing_give(struct vshm *vs, struct shmem *sh, uint32_t n)
{
    uint32_t i, page;
    uintptr_t frame;

    for (i = 0; i < n; i++) {
        frame = shmem_resident(sh, i);
        page = frame ? vshm_page_of(vs, frame) : vs->npages;
        if (page < vs->npages && page >= heap_page(vs, 0) && vs->frames[page] == frame) {
            heap_give(vs, page - heap_page(vs, 0), 1);
        }
    }
}

/**
 * Give `sh` its pages from the heap: one run that follows itself where the
 * heap has one, so that the device's memory entries for them are one, else
 * the free runs from the rover in turn, since pages a few long-lived
 * commands hold may leave no run as long as a large buffer among many free
 * pages. 0, or -ENOMEM, counted in `refused`, with nothing taken.
 */
static int vs_backing_fill(struct virtio_dev *d, struct shmem *sh, uint64_t bytes)
{
    struct vshm *vs = to_vs(d);
    uint32_t n, done, first, got;
    int rc;

    if (!vs->attached || bytes == 0 || bytes / VGS_PAGE > vs->heap_pages) {
        return -ENOMEM;
    }
    n = (uint32_t)(bytes / VGS_PAGE + (bytes % VGS_PAGE != 0));
    first = heap_take(vs, n, n, &got);
    if (first < vs->heap_pages) {
        rc = backing_adopt(vs, sh, 0, first, n);
    } else {
        rc = heap_free_pages(vs) >= n ? 0 : -ENOMEM;
        for (done = 0; rc == 0 && done < n; done += got) {
            first = heap_take(vs, n - done, 1, &got);
            rc = first < vs->heap_pages ? backing_adopt(vs, sh, done, first, got) : -ENOMEM;
        }
    }
    if (rc != 0) {
        heap_refused(vs);
        backing_give(vs, sh, n);
        shmem_free(sh);
        sh->size = bytes;
    }
    return rc;
}

/** The heap page of `vs` that is the region's `frame`, or the heap's size
 * for a frame not in its heap. */
static uint32_t heap_of(const struct vshm *vs, uintptr_t frame)
{
    uint32_t page = vshm_page_of(vs, frame), base = heap_page(vs, 0);

    if (page >= vs->npages || page < base || vs->frames[page] != frame) {
        return vs->heap_pages;
    }
    return page - base;
}

/** Give a detached record's heap back its `frame`, which nothing holds
 * now, and the attached region's heap the same frame, which its attach left
 * out for this record (`vshm_gone_reserve`). */
static void gone_give(struct vshm *vs, uintptr_t frame)
{
    struct vshm *cur = vshm_cur;

    if (!heap_drop(vs, heap_of(vs, frame))) {
        return;
    }
    if (cur && cur != vs && cur->attached) {
        (void)heap_drop(cur, heap_of(cur, frame));
    }
}

/** Give the heap back the pages `vs_backing_fill` gave `sh`: each page that
 * holds the region's frame for a heap page, the detached region's too. */
static void vs_backing_release(struct virtio_dev *d, struct shmem *sh)
{
    struct vshm *vs = to_vs(d);
    uint32_t n = (uint32_t)(sh->size / VGS_PAGE + (sh->size % VGS_PAGE != 0));

    uintptr_t frame;
    uint32_t i;

    if (vs->attached) {
        backing_give(vs, sh, n);
    } else if (vs->gone) {
        for (i = 0; i < n; i++) {
            frame = shmem_resident(sh, i);
            if (frame) {
                gone_give(vs, frame);
            }
        }
        if (vs->heap_used == 0) {
            vshm_gone_free(vs);
        }
    }
}

/** The offset of the region's frame `phys`, which must lie in the heap. */
static int vs_dma_addr(struct virtio_dev *d, uintptr_t phys, uint64_t *addr)
{
    struct vshm *vs = to_vs(d);
    uint32_t page;

    if (!vs->attached) {
        return -EFAULT;
    }
    page = vshm_page_of(vs, phys);
    if (page >= vs->npages || page < heap_page(vs, 0)) {
        return -EFAULT;
    }
    *addr = (uint64_t)page * VGS_PAGE + (phys & (VGS_PAGE - 1));
    return 0;
}

#if VSHM_PAGES
/** KVM took a map's frames back: count it, then tell its owner, which
 * stops every use of them. Under the big lock; does not sleep. */
static void vshm_map_revoked(void *owner)
{
    struct virtio_map *vm = (struct virtio_map *)owner;

    if (vm->live) {
        vm->live = 0;
        vm->vs->map_live--;
        vm->vs->map_pages -= vm->pages;
        vm->vs->revoked++;
    }
    vm->revoke(vm->owner);
}
#endif

/**
 * Hold the frames behind runs of the VM's physical pages: the VM is the one
 * whose KVM ioeventfd signals the doorbell the device rings for the queues,
 * so a transport without doorbells has none; its slots and its maker's
 * memory give the frames (`kvm_gpamap_new`). 0, -ENODEV without the VM, or
 * the map's error.
 */
static int vs_map_pages(struct virtio_dev *d, const struct virtio_run *runs, uint32_t n,
                        void (*revoke)(void *owner), void *owner, struct virtio_map **out)
{
#if VSHM_PAGES
    struct vshm *vs = to_vs(d);
    struct kvm_gpa_run *g;
    struct virtio_map *vm;
    struct kvm_vm *kvm;
    uint32_t i;
    int rc;

    *out = NULL;
    if (!vs->attached || !vs->bells || !vs->in[VGS_VEC_QUEUE].ev) {
        return -ENODEV;
    }
    g = (struct kvm_gpa_run *)kmalloc((size_t)n * sizeof(*g));
    vm = (struct virtio_map *)kzalloc(sizeof(*vm));
    if (!g || !vm || n == 0) {
        kfree(g);
        kfree(vm);
        return n == 0 ? -EINVAL : -ENOMEM;
    }
    for (i = 0; i < n; i++) {
        g[i].gpa = runs[i].addr;
        g[i].pages = runs[i].pages;
        vm->pages += runs[i].pages; /* exact once the map takes them */
    }
    vm->vs = vs;
    vm->revoke = revoke;
    vm->owner = owner;
    kvm = kvm_vm_by_eventfd(vs->in[VGS_VEC_QUEUE].ev);
    rc = kvm ? kvm_gpamap_new(kvm, g, n, vshm_map_revoked, vm, &vm->m) : -ENODEV;
    kfree(g);
    if (rc != 0) {
        kfree(vm);
        return rc;
    }
    vm->live = 1;
    vs->refs++;
    vs->maps++;
    vs->map_live++;
    vs->map_pages += vm->pages;
    *out = vm;
    return 0;
#else
    (void)d;
    (void)runs;
    (void)n;
    (void)revoke;
    (void)owner;
    *out = NULL;
    return -ENODEV;
#endif
}

/** The frame of page `i` of a map, or 0 once KVM took it back. */
static uintptr_t vs_map_frame(struct virtio_map *vm, uint64_t i)
{
#if VSHM_PAGES
    return vm->live ? kvm_gpamap_frame(vm->m, i) : 0;
#else
    (void)vm;
    (void)i;
    return 0;
#endif
}

static void vs_put(struct virtio_dev *d);

/** Let go of a map and its hold on the record. */
static void vs_map_free(struct virtio_map *vm)
{
    if (vm->live) {
        vm->vs->map_live--;
        vm->vs->map_pages -= vm->pages;
    }
#if VSHM_PAGES
    kvm_gpamap_free(vm->m);
#endif
    vs_put(&vm->vs->vdev);
    kfree(vm);
}

static void vshm_free(struct vshm *vs);

/** A driver's bind or a map has let go: free the record at its last
 * reference. */
static void vs_put(struct virtio_dev *d)
{
    struct vshm *vs = to_vs(d);

    if (vs->refs > 0 && --vs->refs == 0) {
        vshm_free(vs);
    }
}

static const struct virtio_ops vshm_ops = {
    "vgpu-shm",      vs_get_status,      vs_set_status,
    vs_get_features, vs_set_features,    vs_config_generation,
    vs_config_read,  vs_config_write,    vs_queue_max,
    vs_queue_enable, vs_notify,          vs_irq_setup,
    vs_irq_start,    vs_irq_release,     vs_dma_alloc,
    vs_dma_free,     vs_shm_region,      vs_ring_alloc,
    vs_backing_fill, vs_backing_release, vs_dma_addr,
    vs_put,          vs_map_pages,       vs_map_frame,
    vs_map_free};

/* ------------------------------------------------------------------------
 * The doorbells the device rings
 */

/** The device rang vector `b->vector`: note it and wake the thread, taking
 * the count as a read would. With the eventfd's lock held and interrupts
 * off. */
static int vshm_bell_signal(struct eventfd_hook *h)
{
    struct vshm_bell *b = (struct vshm_bell *)h;

    b->vs->woken[b->vector]++;
    vshm_rang |= 1u << b->vector;
    wq_wake_all(&vshm_wq);
    return 1;
}

/** The doorbell's last descriptor closed: nothing more rings it, so the
 * thread polls from now on. Under the big lock. */
static void vshm_bell_hangup(struct eventfd_hook *h)
{
    struct vshm_bell *b = (struct vshm_bell *)h;

    b->hooked = 0;
    b->vs->bells = 0;
    wq_wake_all(&vshm_wq);
}

/* ------------------------------------------------------------------------
 * Attaching and detaching
 */

/** Take a detached record off `vshm_kept` and free the tables it kept. */
static void vshm_gone_free(struct vshm *vs)
{
    struct vshm **p;

    for (p = &vshm_kept; *p && *p != vs; p = &(*p)->gone_next) {
    }
    if (*p) {
        *p = vs->gone_next;
    }
    vs->gone = 0;
    vs->gone_next = NULL;
    kfree(vs->frames);
    vs->frames = NULL;
    kfree(vs->rev);
    vs->rev = NULL;
    kfree(vs->heap_map);
    vs->heap_map = NULL;
}

/**
 * Leave out of `vs`'s heap, before the region is published, each frame of
 * it that an object of a detached record still holds, so that no two
 * objects share a frame when the same memory is attached again. Does not
 * sleep.
 */
static void vshm_gone_reserve(struct vshm *vs)
{
    struct vshm *g;
    uint32_t i, page, flags;

    for (g = vshm_kept; g; g = g->gone_next) {
        flags = spin_lock_irqsave(&g->heap_lock);
        for (i = 0; i < g->heap_pages; i++) {
            if (!heap_bit(g, i)) {
                continue;
            }
            page = vshm_page_of(vs, g->frames[heap_page(g, i)]);
            if (page < vs->npages && page >= heap_page(vs, 0) &&
                !heap_bit(vs, page - heap_page(vs, 0))) {
                heap_mark(vs, page - heap_page(vs, 0), 1, 1);
                vs->heap_used++;
            }
        }
        spin_unlock_irqrestore(&g->heap_lock, flags);
    }
}

/** Free what the attach took and the record, once nothing refers to it: a
 * detached record's heap pages that were never given back first, since
 * nothing of it holds them now. */
static void vshm_free(struct vshm *vs)
{
    uint32_t i;

    for (i = 0; vs->gone && i < vs->heap_pages; i++) {
        gone_give(vs, vs->frames[heap_page(vs, i)]);
    }
    if (vs->gone) {
        vshm_gone_free(vs);
    }
    kfree(vs->frames);
    kfree(vs->rev);
    kfree(vs->heap_map);
    kfree(vs);
}

/**
 * Clear from `vs`'s heap each page another detached record holds: the
 * attach left it out for that record (`vshm_gone_reserve`), which alone
 * gives it back, so that only the attached region's heap keeps such marks.
 */
static void vshm_unreserve(struct vshm *vs)
{
    struct vshm *g;
    uintptr_t frame;
    uint32_t i;

    for (i = 0; i < vs->heap_pages; i++) {
        if (!heap_bit(vs, i)) {
            continue;
        }
        frame = vs->frames[heap_page(vs, i)];
        for (g = vshm_kept; g; g = g->gone_next) {
            if (heap_of(g, frame) < g->heap_pages && heap_bit(g, heap_of(g, frame))) {
                (void)heap_drop(vs, i);
                break;
            }
        }
    }
}

/**
 * Give back what the region holds: the doorbells' hooks and references,
 * the window and the frames. Every operation is a no-op from here on, but
 * for giving back the heap pages objects still hold.
 */
static void vshm_release(struct vshm *vs)
{
    uint32_t i;

    vs->attached = 0;
    vs->bells = 0;
    for (i = 0; i < 2; i++) {
        if (vs->in[i].ev) {
            if (vs->in[i].hooked) {
                (void)eventfd_hook_remove(vs->in[i].ev, &vs->in[i].hook, NULL);
            }
            eventfd_put(vs->in[i].ev);
            vs->in[i].ev = NULL;
        }
        if (vs->out[i]) {
            eventfd_put(vs->out[i]);
            vs->out[i] = NULL;
        }
    }
    if (vs->base) {
        mmio_unmap((uintptr_t)vs->base, (size_t)vs->npages * VGS_PAGE);
        vs->window = (uintptr_t)vs->base;
        vs->base = NULL;
    }
    for (i = 0; vs->frames && i < vs->npages; i++) {
        if (vs->frames[i]) {
            frame_unref(vs->frames[i]);
        }
    }
    if (vs->heap_map && vs->rev && vs->frames) {
        vshm_unreserve(vs);
    }
    if (vs->heap_map && vs->rev && vs->frames && vs->heap_used > 0) {
        /* A backing holds its frames; a command's page is held by its
         * address alone, which the table keeps for `gone_give`. */
        vs->gone = 1;
        vs->gone_next = vshm_kept;
        vshm_kept = vs;
        return;
    }
    kfree(vs->frames);
    vs->frames = NULL;
    kfree(vs->rev);
    vs->rev = NULL;
    kfree(vs->heap_map);
    vs->heap_map = NULL;
}

/**
 * The thread's detach: the driver told that its device has gone, which
 * unplugs it, fails every pending fence and frees every resource, and
 * resets the transport, storing GONE in a new epoch; then the region given
 * back and the detacher woken.
 */
static void vshm_finish(struct vshm *vs)
{
    if (vs->bound) {
        if (vs->vdev.removed) {
            vs->vdev.removed(&vs->vdev);
        }
        vs->bound = 0;
    }
    if (vs->me.state != VGS_DRV_GONE) {
        vshm_epoch(vs, VGS_DRV_GONE);
    }
    klog(KLOG_INFO, "vgpu-shm", "detached: epoch %u, %s", (unsigned)vs->me.epoch,
         vs->stop == 1 + (int)VGS_DETACH_LOST ? "the device's VM died" : "in order");
    vshm_release(vs);
    vshm_cur = NULL;
    wq_wake_all(&vshm_detached_wq);
    vs_put(&vs->vdev);
}

/**
 * Serve a bound device once: answer its completions, pass on a display
 * event, and on a device that has left the epoch, failed or reset, have
 * the driver drop it, so that the next pass binds a new one in a new
 * epoch, as the transport's rules ask.
 */
static void vshm_serve(struct vshm *vs, uint32_t rang)
{
    int n = 0, i;

    for (i = 0; i < VGS_QUEUES; i++) {
        if (vs->vdev.queues[i]) {
            n += virtqueue_reap(vs->vdev.queues[i]);
        }
    }
    if (n > 0 && vs->doorbells && !(rang & (1u << VGS_VEC_QUEUE))) {
        vs->polls++;
    }
    if (!vshm_snap(vs) || vs->dev.state != VGS_DEV_RUNNING) {
        klog(KLOG_WARN, "vgpu-shm", "lost: epoch %u: the device is %s", (unsigned)vs->me.epoch,
             vs->dev.state == VGS_DEV_NEEDS_RESET ? vgs_strerror((int)vs->dev.error)
                                                  : "no longer running the epoch");
        if (vs->vdev.removed) {
            vs->vdev.removed(&vs->vdev);
        }
        vs->bound = 0;
        return;
    }
    if (vs->dev.events_seq != vs->events) {
        vs->events = vs->dev.events_seq;
        virtio_interrupt_config(&vs->vdev);
    }
}

#ifndef RACH2_HOSTTEST
/** Whether a region is attached, for the thread's wait while none is. */
static int vshm_present(void *arg)
{
    (void)arg;
    return vshm_cur != NULL;
}

/** The transport's thread, `vgpu-shm`: it binds virtio-gpu over an attached
 * region, which waits for the device's offer, serves the bound device on each
 * doorbell, or each tick when it polls, and carries out a detach. The tick does
 * not switch it (`no_tick_switch`): it gives way where it sleeps, at the top of
 * each pass and after each look of `vshm_await` that fails, as vhost's worker
 * calls cond_resched between works (drivers/vhost/vhost.c:440 in 6.12.115),
 * so the state its steps change, which the big lock guards, never changes
 * under it.
 */
static void vshm_main(void)
{
    struct vshm *vs;
    uint32_t rang;
    int32_t rc;

    arch_irq_enable();
    for (;;) {
        sched_service_point();
        vs = vshm_cur;
        if (!vs) {
            (void)wq_wait_event_idle(&vshm_wq, vshm_present, NULL);
            continue;
        }
        if (vs->stop) {
            vshm_finish(vs);
            continue;
        }
        if (!vs->bound) {
            vs->refs++;
            vs->binds++;
            rc = virtio_gpu_bind(&vs->vdev);
            if (rc == 0) {
                vs->bound = 1;
            } else if (!vs->stop) {
                if (vs->failures++ % 64u == 0) {
                    klog(KLOG_WARN, "vgpu-shm",
                         "refused: virtio-gpu did not bind: error %d, %u time(s)", (int)rc,
                         (unsigned)vs->failures);
                }
                vshm_sleep(vs, VSHM_RETRY_TICKS);
            }
            continue;
        }
        vshm_sleep(vs, vs->bells ? VSHM_IDLE_TICKS : VSHM_POLL_TICKS);
        rang = vshm_take_rang();
        if (!vs->stop) {
            vshm_serve(vs, rang);
        }
    }
}

/** Make the thread, once: 0, or -ENOMEM. */
static int32_t vshm_thread_start(void)
{
    process_t *p;
    uintptr_t base;
    uint32_t flags;

    if (vshm_thread) {
        return 0;
    }
    flags = kirq_save();
    base = kstack_alloc();
    p = base ? alloc_process() : NULL;
    if (!p) {
        if (base) {
            kstack_free(base + KERNEL_STACK_SIZE);
        }
        kirq_restore(flags);
        return -ENOMEM;
    }
    p->kernel_stack = base + KERNEL_STACK_SIZE;
    p->space_root = 0;
    p->page_dir = NULL;
    p->parent_pid = 0;
    proc_set_comm(p, "vgpu-shm");
    setup_process_stack(p, vshm_main);
    vshm_thread = p;
    sched_make_ready(p);
    kirq_restore(flags);
    return 0;
}
#endif

/**
 * Take the region of `npages` frames, each with a reference the record now
 * holds, mapped at `base`: sort its frames, lay it out, with its magic
 * last, and make its heap, without the frames a detached record's objects
 * still hold (`vshm_gone_reserve`). The driver's epoch starts at 0, so
 * that the first reset makes epoch 1. 0, or -ENOMEM.
 */
static int32_t vshm_take_region(struct vshm *vs, uintptr_t *frames, uint32_t npages,
                                volatile uint8_t *base)
{
    struct vgs_layout l;
    const uint8_t *src = (const uint8_t *)&l;
    volatile uint8_t *dst;
    uint32_t i;

    vs->frames = frames;
    vs->npages = npages;
    vs->base = base;
    vs->size = (uint64_t)npages * VGS_PAGE;
    if (vgs_layout_make(&l, vs->size, VSHM_RING_LEN) != VGS_OK) {
        return -EINVAL;
    }
    vs->rev = (struct vshm_rev *)kmalloc((size_t)npages * sizeof(*vs->rev));
    vs->heap_pages = (uint32_t)(l.heap_len / VGS_PAGE);
    vs->heap_map = (uint32_t *)kzalloc(((size_t)vs->heap_pages + 31u) / 32u * 4u);
    if (!vs->rev || !vs->heap_map) {
        return -ENOMEM;
    }
    for (i = 0; i < npages; i++) {
        vs->rev[i].phys = frames[i] & ~(uintptr_t)(VGS_PAGE - 1);
        vs->rev[i].page = i;
    }
    vshm_sort(vs->rev, npages);
    vs->l = l;
    vshm_gone_reserve(vs);
    VGS_LAYOUT(base)->magic = 0;
    VGS_MB();
    k_memset((void *)(uintptr_t)(base + 4), 0, VGS_CTRL_SIZE - 4u);
    dst = (volatile uint8_t *)VGS_LAYOUT(base);
    for (i = 4; i < sizeof(l); i++) {
        dst[i] = src[i];
    }
    VGS_MB();
    VGS_LAYOUT(base)->magic = VGS_MAGIC;
    VGS_MB();
    vs->attached = 1;
    return 0;
}

/** A new record over no region yet, with one reference, the attach's. */
static struct vshm *vshm_new(uint32_t peer)
{
    struct vshm *vs = (struct vshm *)kzalloc(sizeof(*vs));

    if (!vs) {
        return NULL;
    }
    spin_lock_init(&vs->heap_lock, "vshm_heap");
    vs->vdev.ops = &vshm_ops;
    vs->vdev.transport = vs;
    vs->vdev.id = VIRTIO_ID_GPU;
    vs->vdev.name = "vgpu-shm";
    vs->vdev.dma_max = VGS_ATTACH_MAX;
    vs->refs = 1;
    vs->me.peer = peer;
    return vs;
}

/** What VGS_IOC_INFO reports of `vs`, or of no region when NULL. */
static void vshm_info(const struct vshm *vs, struct vgs_info *out)
{
    k_memset(out, 0, sizeof(*out));
    if (!vs) {
        return;
    }
    out->attached = (uint32_t)vs->attached;
    out->bound = (uint32_t)vs->bound;
    out->epoch = vs->me.epoch;
    out->drv_state = vs->me.state;
    out->dev_state = vs->dev.state;
    out->dev_error = vs->dev.error;
    out->doorbells = (uint32_t)vs->doorbells;
    out->binds = vs->binds;
    out->rung[0] = vs->rung[0];
    out->rung[1] = vs->rung[1];
    out->woken[0] = vs->woken[0];
    out->woken[1] = vs->woken[1];
    out->polls = vs->polls;
    out->heap_pages = vs->heap_pages;
    out->heap_used = vs->heap_used;
    out->refused = vs->refused;
    out->maps = vs->maps;
    out->map_pages = vs->map_pages;
    out->map_live = vs->map_live;
    out->revoked = vs->revoked;
}

#ifndef RACH2_HOSTTEST
/**
 * The caller's descriptor `fd` as a region: a tmpfs file, a memfd, sealed
 * against shrinking, whose size is a power of two the transport takes.
 * Returns its memory, or NULL with `*err` -EBADF or -EINVAL.
 */
static struct shmem *vshm_region_of(int32_t fd, int32_t *err)
{
    fd_entry_t *e = fd_get(current_proc->fds, fd);
    struct shmem *sh;

    *err = -EBADF;
    if (!e || (e->flags & FD_FLAG_PATH) || !e->ofd || !e->ofd->vmnt ||
        e->ofd->vmnt->type != &vfs_tmpfs_type || !(sh = tmpfs_shmem(e->ofd->vnode))) {
        return NULL;
    }
    *err = -EINVAL;
    if (!(tmpfs_seals(e->ofd->vnode) & TMPFS_SEAL_SHRINK) || sh->size < VGS_REGION_MIN ||
        sh->size > VGS_ATTACH_MAX || (sh->size & (sh->size - 1u)) != 0) {
        return NULL;
    }
    return sh;
}

/**
 * VGS_IOC_ATTACH: take the caller's region and doorbells and start the
 * thread's bind over them. 0; -EBUSY while a region is attached; -EBADF,
 * -EINVAL for a region or an eventfd the transport does not take or a
 * nonzero flag, -ENOMEM.
 */
static int32_t vshm_attach(fd_entry_t *e, const struct vgs_attach *a)
{
    struct vshm *vs;
    struct shmem *sh;
    uintptr_t *frames;
    uintptr_t va;
    uint32_t npages, i, given = 0;
    int32_t rc;

    if (vshm_cur) {
        return -EBUSY;
    }
    if (a->flags != 0 || a->reserved != 0 || a->peer > 0xffffu) {
        return -EINVAL;
    }
    for (i = 0; i < 2; i++) {
        given += (a->bell_in[i] >= 0) + (a->bell_out[i] >= 0);
    }
    if (given != 0 && given != 4) {
        return -EINVAL;
    }
    sh = vshm_region_of(a->memfd, &rc);
    if (!sh) {
        return rc;
    }
    rc = vshm_thread_start();
    if (rc != 0) {
        return rc;
    }
    vs = vshm_new(a->peer);
    npages = (uint32_t)(sh->size / VGS_PAGE);
    frames = vs ? (uintptr_t *)kzalloc((size_t)npages * sizeof(*frames)) : NULL;
    if (!frames) {
        kfree(vs);
        return -ENOMEM;
    }
    for (i = 0; i < npages; i++) {
        frames[i] = shmem_map_page(sh, i);
        if (!frames[i]) {
            break;
        }
    }
    va = i == npages ? mmio_map_frames(frames, npages, 0) : 0;
    vs->frames = frames;
    vs->npages = npages;
    if (!va) {
        vshm_release(vs);
        vshm_free(vs);
        return -ENOMEM;
    }
    rc = vshm_take_region(vs, frames, npages, (volatile uint8_t *)va);
    for (i = 0; rc == 0 && given == 4 && i < 2; i++) {
        vs->in[i].vs = vs;
        vs->in[i].vector = i;
        vs->in[i].hook.signal = vshm_bell_signal;
        vs->in[i].hook.hangup = vshm_bell_hangup;
        vs->in[i].ev = eventfd_get(a->bell_in[i], &rc);
        vs->out[i] = rc == 0 ? eventfd_get(a->bell_out[i], &rc) : NULL;
    }
    if (rc != 0) {
        vshm_release(vs);
        vshm_free(vs);
        return rc;
    }
    vs->bells = given == 4;
    for (i = 0; vs->bells && i < 2; i++) {
        vs->in[i].hooked = eventfd_hook_add(vs->in[i].ev, &vs->in[i].hook) >= 0;
        vs->bells = vs->bells && vs->in[i].hooked;
    }
    klog(KLOG_INFO, "vgpu-shm", "attached: %u KiB, heap %u pages, peer %u, %s",
         (unsigned)(vs->size / 1024u), (unsigned)vs->heap_pages, (unsigned)a->peer,
         vs->bells ? "doorbells offered" : "polling");
    e->priv = vs;
    vshm_cur = vs;
    wq_wake_all(&vshm_wq);
    return 0;
}

/** Whether the region a detacher asked to take back has gone. */
static int vshm_gone(void *arg)
{
    return vshm_cur != (struct vshm *)arg;
}

/** VGS_IOC_DETACH, and the attacher's last close: have the thread detach
 * `vs`, the device left as `how` says, and wait until it has. */
static int32_t vshm_detach(struct vshm *vs, uint32_t how)
{
    if (!vs || vshm_cur != vs) {
        return -ENXIO;
    }
    if (how != VGS_DETACH_ORDERLY && how != VGS_DETACH_LOST) {
        return -EINVAL;
    }
    if (!vs->stop) {
        vs->stop = 1 + (int)how;
    }
    wq_wake_all(&vshm_wq);
    (void)wq_wait_event_nointr(&vshm_detached_wq, vshm_gone, vs);
    return 0;
}

/** The node's calls (vgpu-shm(4)), for a privileged caller. */
static int32_t vshm_ioctl(fd_entry_t *e, uint32_t request, uintptr_t arg)
{
    struct vgs_attach a;
    struct vgs_info info;
    uint32_t how;

    if (!proc_privileged(current_proc)) {
        return -EPERM;
    }
    switch (request) {
    case VGS_IOC_ATTACH:
        if (copy_from_user(&a, (const void __user *)arg, sizeof(a))) {
            return -EFAULT;
        }
        return vshm_attach(e, &a);
    case VGS_IOC_DETACH:
        if (copy_from_user(&how, (const void __user *)arg, sizeof(how))) {
            return -EFAULT;
        }
        if (e->priv != vshm_cur || !vshm_cur) {
            return -ENXIO;
        }
        return vshm_detach(vshm_cur, how);
    case VGS_IOC_INFO:
        vshm_info(vshm_cur, &info);
        return copy_to_user((void __user *)arg, &info, sizeof(info)) ? -EFAULT : 0;
    default:
        return -ENOTTY;
    }
}

/** The last close of the attacher's open: the attach ends as if its device
 * had died, since nothing is left to tell the driver of the VM. */
static void vshm_close(fd_entry_t *e)
{
    if (e->priv && e->priv == vshm_cur) {
        (void)vshm_detach(vshm_cur, VGS_DETACH_LOST);
    }
    e->priv = NULL;
}

static const struct file_ops vshm_fops = {NULL, NULL, NULL, vshm_ioctl, vshm_close, NULL, NULL, 0};
static struct devfs_node vshm_node = {"vgpu-shm", 10, 243, 0600, &vshm_fops, NULL, NULL};

int32_t vgpu_shm_start(void)
{
    if (!vshm_ready) {
        wait_queue_init(&vshm_wq);
        wait_queue_init(&vshm_detached_wq);
        vshm_ready = 1;
    }
    return devfs_node_register(&vshm_node);
}
#endif
