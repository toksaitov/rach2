/*
 * virtqueue.c: the virtio core that every transport shares: a device's
 * reset, feature negotiation and bring-up, and its split virtqueues.
 */

#include "virtio.h"

#include "../arch/arch.h"
#include "../klog.h"
#include "../udelay.h"
#include "../lib/kerrno.h"
#include "../mm/kheap.h"

/* How long a reset may take: VIRTIO_RESET_POLLS reads of the status,
 * VIRTIO_RESET_STEP_US apart, 100 ms in all. */
#define VIRTIO_RESET_POLLS   1000
#define VIRTIO_RESET_STEP_US 100

/* Reads of the configuration while its generation keeps changing. */
#define VIRTIO_CONFIG_TRIES 8

/* The feature bits virtio 1.2 reserves for the rings and the transport,
 * 24 to 40, which a driver's `wanted` does not choose; the core accepts
 * those of them it handles. */
#define VIRTIO_RESERVED_MASK (((VIRTIO_BIT(41) - 1u) >> 24) << 24)
#define VIRTIO_CORE_FEATURES                                               \
    (VIRTIO_BIT(VIRTIO_F_VERSION_1) | VIRTIO_BIT(VIRTIO_F_INDIRECT_DESC) | \
     VIRTIO_BIT(VIRTIO_F_ACCESS_PLATFORM) | VIRTIO_BIT(VIRTIO_F_ORDER_PLATFORM))

/* The bytes of an indirect table page. */
#define VIRTQ_INDIRECT_BYTES \
    ((uint32_t)(VIRTQ_INDIRECT_TABLES * VIRTQ_INDIRECT_LEN * sizeof(struct virtq_desc)))

/** Add `bits` to the device's status. */
static void virtio_status_add(struct virtio_dev *d, uint8_t bits)
{
    d->ops->set_status(d, (uint8_t)(d->ops->get_status(d) | bits));
}

int virtio_reset(struct virtio_dev *d)
{
    int i;

    d->ops->set_status(d, 0);
    for (i = 0; i < VIRTIO_RESET_POLLS; i++) {
        if (d->ops->get_status(d) == 0) {
            return 0;
        }
        k_udelay(VIRTIO_RESET_STEP_US);
    }
    klog(KLOG_ERR, "virtio", "reset: %s: the status did not clear in 100 ms", d->name);
    return -ETIMEDOUT;
}

/** Mark `d` FAILED and pass `err` on. */
static int virtio_fail(struct virtio_dev *d, int err)
{
    virtio_status_add(d, VIRTIO_STATUS_FAILED);
    return err;
}

int virtio_init(struct virtio_dev *d, uint64_t wanted, uint16_t nqueues)
{
    uint64_t offered, accept;
    int r, i;

    if (nqueues > VIRTIO_MAX_QUEUES) {
        return -EINVAL;
    }
    for (i = 0; i < VIRTIO_MAX_QUEUES; i++) {
        if (d->queues[i]) {
            return -EBUSY;
        }
    }
    d->features = 0;
    d->nqueues = 0;
    r = virtio_reset(d);
    if (r != 0) {
        return r;
    }
    virtio_status_add(d, VIRTIO_STATUS_ACKNOWLEDGE);
    virtio_status_add(d, VIRTIO_STATUS_DRIVER);
    offered = d->ops->get_features(d);
    if (!(offered & VIRTIO_BIT(VIRTIO_F_VERSION_1))) {
        klog(KLOG_WARN, "virtio", "refused: %s: a legacy device, without VERSION_1", d->name);
        return virtio_fail(d, -ENODEV);
    }
    accept = offered & ((wanted & ~VIRTIO_RESERVED_MASK) | VIRTIO_CORE_FEATURES);
    d->ops->set_features(d, accept);
    virtio_status_add(d, VIRTIO_STATUS_FEATURES_OK);
    if (!(d->ops->get_status(d) & VIRTIO_STATUS_FEATURES_OK)) {
        klog(KLOG_WARN, "virtio", "refused: %s: the device did not take features %x:%x", d->name,
             (unsigned)(accept >> 32), (unsigned)accept);
        return virtio_fail(d, -EIO);
    }
    d->features = accept;
    r = d->ops->irq_setup(d, nqueues);
    if (r != 0) {
        return virtio_fail(d, r);
    }
    d->nqueues = nqueues;
    klog(KLOG_INFO, "virtio", "features: %s: offered %x:%x, taken %x:%x", d->name,
         (unsigned)(offered >> 32), (unsigned)offered, (unsigned)(accept >> 32), (unsigned)accept);
    return 0;
}

int virtio_has(const struct virtio_dev *d, unsigned bit)
{
    return bit < 64 && (d->features & VIRTIO_BIT(bit)) != 0;
}

/** Free `vq`'s record, and its memory when `device_stopped` says the device
 * no longer reads it. */
static void virtqueue_free(struct virtio_dev *d, struct virtqueue *vq, int device_stopped)
{
    if (device_stopped) {
        if (vq->indirect) {
            d->ops->dma_free(d, (void *)(uintptr_t)vq->indirect, VIRTQ_INDIRECT_BYTES);
        }
        if (vq->ring_mem) {
            d->ops->dma_free(d, vq->ring_mem, vq->ring_bytes);
        }
    }
    kfree(vq);
}

/**
 * Lay out a queue of `vq->size` in its ring memory: the descriptors first,
 * aligned to 16; the driver's ring after them, aligned to 2; the device's
 * ring after that, aligned to 4 (virtio 1.2, 2.7). Returns the bytes.
 */
static uint32_t virtqueue_layout(uint16_t size, uint32_t *avail_off, uint32_t *used_off)
{
    uint32_t n = size;

    *avail_off = 16u * n;
    *used_off = (*avail_off + 6u + 2u * n + 3u) & ~3u;
    return *used_off + 6u + 8u * n;
}

int virtio_queue_setup(struct virtio_dev *d, uint16_t index, virtqueue_done_t done, void *priv,
                       struct virtqueue **out)
{
    struct virtqueue *vq;
    uint32_t avail_off, used_off;
    uint16_t max, size, i;
    uintptr_t base;
    int r;

    if (index >= d->nqueues) {
        return -ENOENT;
    }
    if (d->queues[index]) {
        return -EBUSY;
    }
    max = d->ops->queue_max(d, index);
    if (max == 0) {
        return -ENOENT;
    }
    size = VIRTQ_MAX_SIZE;
    while (size > max) {
        size >>= 1;
    }
    vq = (struct virtqueue *)kzalloc(sizeof(*vq));
    if (!vq) {
        return -ENOMEM;
    }
    vq->dev = d;
    vq->index = index;
    vq->size = size;
    vq->vector = VIRTIO_NO_VECTOR;
    vq->done = done;
    vq->priv = priv;
    vq->ring_bytes = virtqueue_layout(size, &avail_off, &used_off);
    vq->ring_mem = d->ops->dma_alloc(d, vq->ring_bytes, &vq->desc_addr);
    if (!vq->ring_mem) {
        kfree(vq);
        return -ENOMEM;
    }
    base = (uintptr_t)vq->ring_mem; /* page-aligned, so each part is aligned */
    vq->desc = (volatile struct virtq_desc *)base;
    vq->avail = (volatile uint16_t *)(base + avail_off);
    vq->used = (volatile uint16_t *)(base + used_off);
    vq->used_ring = (volatile struct virtq_used_elem *)(base + used_off + 4u);
    vq->avail_addr = vq->desc_addr + avail_off;
    vq->used_addr = vq->desc_addr + used_off;
    if (virtio_has(d, VIRTIO_F_INDIRECT_DESC)) {
        vq->indirect = (volatile struct virtq_desc *)d->ops->dma_alloc(d, VIRTQ_INDIRECT_BYTES,
                                                                       &vq->indirect_addr);
        if (vq->indirect) {
            vq->indirect_free = (uint32_t)((1ul << VIRTQ_INDIRECT_TABLES) - 1u);
        }
    }
    for (i = 0; i < size; i++) {
        vq->desc[i].next = (uint16_t)(i + 1u);
        vq->table[i] = -1;
    }
    vq->free_head = 0;
    vq->nfree = size;
    r = d->ops->queue_enable(d, vq);
    if (r != 0) {
        virtqueue_free(d, vq, 1);
        return r;
    }
    d->queues[index] = vq;
    *out = vq;
    klog(KLOG_INFO, "virtio", "queue: %s: %d of %d entries, vector %d%s", d->name, index, size,
         vq->vector == VIRTIO_NO_VECTOR ? -1 : (int)vq->vector, vq->indirect ? ", indirect" : "");
    return 0;
}

void virtio_ready(struct virtio_dev *d)
{
    d->ops->irq_start(d);
    virtio_status_add(d, VIRTIO_STATUS_DRIVER_OK);
}

void virtio_teardown(struct virtio_dev *d)
{
    int stopped = virtio_reset(d) == 0;
    int i;

    d->ops->irq_release(d);
    for (i = 0; i < VIRTIO_MAX_QUEUES; i++) {
        if (d->queues[i]) {
            virtqueue_free(d, d->queues[i], stopped);
            d->queues[i] = 0;
        }
    }
    d->nqueues = 0;
    d->features = 0;
}

void virtio_config_read(struct virtio_dev *d, uint32_t offset, void *buf, uint32_t len)
{
    uint32_t before, after;
    int i;

    for (i = 0; i < VIRTIO_CONFIG_TRIES; i++) {
        before = d->ops->config_generation(d);
        d->ops->config_read(d, offset, buf, len);
        after = d->ops->config_generation(d);
        if (before == after) {
            return;
        }
    }
}

void virtio_config_write(struct virtio_dev *d, uint32_t offset, const void *buf, uint32_t len)
{
    d->ops->config_write(d, offset, buf, len);
}

void *virtio_dma_alloc(struct virtio_dev *d, uint32_t bytes, uint64_t *addr)
{
    return d->ops->dma_alloc(d, bytes, addr);
}

void virtio_dma_free(struct virtio_dev *d, void *p, uint32_t bytes)
{
    if (p) {
        d->ops->dma_free(d, p, bytes);
    }
}

int virtio_shm_region(struct virtio_dev *d, uint8_t id, uint64_t *addr, uint64_t *len)
{
    return d->ops->shm_region(d, id, addr, len);
}

/** Fill descriptor `dd` from segment `k` of a buffer of `total`, the first
 * `out` of them read by the device; `next` is linked when one follows. */
static void virtqueue_fill(volatile struct virtq_desc *dd, const struct virtio_sg *sg, unsigned k,
                           unsigned out, unsigned total, uint16_t next)
{
    uint16_t flags = k >= out ? VIRTQ_DESC_F_WRITE : 0;

    dd->addr = sg[k].addr;
    dd->len = sg[k].len;
    if (k + 1u < total) {
        flags |= VIRTQ_DESC_F_NEXT;
        dd->next = next;
    }
    dd->flags = flags;
}

int virtqueue_add(struct virtqueue *vq, const struct virtio_sg *sg, unsigned out, unsigned in,
                  void *token)
{
    unsigned total = out + in, k;
    uint16_t head, i, slot;
    uint32_t flags;
    int t;

    if (total == 0 || !token) {
        return -EINVAL;
    }
    flags = spin_lock_irqsave(&vq->lock);
    head = vq->free_head;
    if (total <= vq->nfree) {
        i = head;
        for (k = 0; k < total; k++) {
            uint16_t next = vq->desc[i].next;

            virtqueue_fill(&vq->desc[i], sg, k, out, total, next);
            if (k + 1u < total) {
                i = next;
            }
        }
        vq->free_head = vq->desc[i].next;
        vq->nfree = (uint16_t)(vq->nfree - total);
        vq->chain[head] = (uint16_t)total;
        vq->table[head] = -1;
    } else if (vq->indirect && vq->indirect_free != 0 && vq->nfree > 0 &&
               total <= VIRTQ_INDIRECT_LEN) {
        volatile struct virtq_desc *tbl;

        t = 0;
        while (!(vq->indirect_free & (1u << t))) {
            t++;
        }
        vq->indirect_free &= ~(1u << t);
        tbl = vq->indirect + t * VIRTQ_INDIRECT_LEN;
        for (k = 0; k < total; k++) {
            virtqueue_fill(&tbl[k], sg, k, out, total, (uint16_t)(k + 1u));
        }
        vq->free_head = vq->desc[head].next;
        vq->desc[head].addr =
            vq->indirect_addr + (uint64_t)t * VIRTQ_INDIRECT_LEN * sizeof(struct virtq_desc);
        vq->desc[head].len = (uint32_t)(total * sizeof(struct virtq_desc));
        vq->desc[head].flags = VIRTQ_DESC_F_INDIRECT;
        vq->nfree--;
        vq->chain[head] = 1;
        vq->table[head] = (int8_t)t;
    } else {
        spin_unlock_irqrestore(&vq->lock, flags);
        return -ENOSPC;
    }
    vq->token[head] = token;
    slot = (uint16_t)(vq->avail_idx & (vq->size - 1u));
    vq->avail[2 + slot] = head;
    arch_dma_fence(); /* the chain and its ring entry before the index */
    vq->avail_idx++;
    vq->avail[1] = vq->avail_idx;
    vq->added++;
    spin_unlock_irqrestore(&vq->lock, flags);
    return 0;
}

void virtqueue_kick(struct virtqueue *vq)
{
    arch_dma_fence(); /* the index is out before the device's flag is read */
    if (!(vq->used[0] & VIRTQ_USED_F_NO_NOTIFY)) {
        vq->dev->ops->notify(vq->dev, vq);
    }
}

/** Put the chain at `head` back on the free list, with its indirect table;
 * the queue's lock is held. */
static void virtqueue_release(struct virtqueue *vq, uint16_t head)
{
    uint16_t last = head, k;

    if (vq->table[head] >= 0) {
        vq->indirect_free |= 1u << vq->table[head];
        vq->table[head] = -1;
    } else {
        for (k = 1; k < vq->chain[head]; k++) {
            last = vq->desc[last].next;
        }
    }
    vq->desc[last].next = vq->free_head;
    vq->free_head = head;
    vq->nfree = (uint16_t)(vq->nfree + vq->chain[head]);
    vq->chain[head] = 0;
    vq->token[head] = 0;
}

int virtqueue_reap(struct virtqueue *vq)
{
    int n = 0;

    for (;;) {
        uint32_t flags = spin_lock_irqsave(&vq->lock);
        uint16_t used_idx = vq->used[1];
        uint32_t id, len;
        void *token;

        if (used_idx == vq->last_used) {
            spin_unlock_irqrestore(&vq->lock, flags);
            break;
        }
        if ((uint16_t)(used_idx - vq->last_used) > vq->size) {
            spin_unlock_irqrestore(&vq->lock, flags);
            klog(KLOG_ERR, "virtio", "queue: %s: %d: the device's index ran %d past ours",
                 vq->dev->name, vq->index, (int)(uint16_t)(used_idx - vq->last_used));
            break;
        }
        arch_dma_fence(); /* the element is read after the index */
        id = vq->used_ring[vq->last_used & (vq->size - 1u)].id;
        len = vq->used_ring[vq->last_used & (vq->size - 1u)].len;
        vq->last_used++;
        if (id >= vq->size || !vq->token[id]) {
            spin_unlock_irqrestore(&vq->lock, flags);
            klog(KLOG_ERR, "virtio", "queue: %s: %d: the device returned head %d, not in use",
                 vq->dev->name, vq->index, (int)id);
            continue;
        }
        token = vq->token[id];
        virtqueue_release(vq, (uint16_t)id);
        vq->completed++;
        spin_unlock_irqrestore(&vq->lock, flags);
        if (vq->done) {
            vq->done(vq, token, len);
        }
        n++;
    }
    return n;
}

uint16_t virtqueue_free_count(struct virtqueue *vq)
{
    return vq->nfree;
}

void virtio_interrupt_queue(struct virtqueue *vq)
{
    if (vq) {
        (void)virtqueue_reap(vq);
    }
}

void virtio_interrupt_queues(struct virtio_dev *d)
{
    int i;

    for (i = 0; i < VIRTIO_MAX_QUEUES; i++) {
        virtio_interrupt_queue(d->queues[i]);
    }
}

void virtio_interrupt_config(struct virtio_dev *d)
{
    d->config_events++;
    if (d->config_changed) {
        d->config_changed(d);
    }
}
