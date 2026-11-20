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
#define VIRTIO_CORE_FEATURES                                                 \
    (VIRTIO_BIT(VIRTIO_F_VERSION_1) | VIRTIO_BIT(VIRTIO_F_INDIRECT_DESC) |   \
     VIRTIO_BIT(VIRTIO_F_EVENT_IDX) | VIRTIO_BIT(VIRTIO_F_ACCESS_PLATFORM) | \
     VIRTIO_BIT(VIRTIO_F_ORDER_PLATFORM))

/* The bytes of a page of indirect tables, and of one descriptor. */
#define VIRTQ_PAGE_BYTES 4096u
#define VIRTQ_DESC_BYTES ((uint32_t)sizeof(struct virtq_desc))

/** Add `bits` to the device's status. */
static void virtio_status_add(struct virtio_dev *d, uint8_t bits)
{
    d->ops->set_status(d, (uint8_t)(d->ops->get_status(d) | bits));
}

/* The feature bits a legacy device has: 32, of which the core takes the
 * ring's two (virtio 1.2, 6). */
#define VIRTIO_LEGACY_MASK     0xFFFFFFFFull
#define VIRTIO_LEGACY_FEATURES (VIRTIO_BIT(VIRTIO_F_INDIRECT_DESC) | VIRTIO_BIT(VIRTIO_F_EVENT_IDX))

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
    if (d->ops->dma_enable) {
        d->ops->dma_enable(d, 1);
    }
    virtio_status_add(d, VIRTIO_STATUS_ACKNOWLEDGE);
    virtio_status_add(d, VIRTIO_STATUS_DRIVER);
    offered = d->ops->get_features(d);
    if (d->ops->get_status(d) & VIRTIO_STATUS_FAILED) {
        return -ENODEV; /* the transport has no device to offer, and has said why */
    }
    if (d->legacy) {
        /* A legacy device settles its features at the write, without
         * FEATURES_OK, as Linux's virtio_features_ok skips it. */
        accept = offered & VIRTIO_LEGACY_MASK &
                 ((wanted & ~VIRTIO_RESERVED_MASK) | VIRTIO_LEGACY_FEATURES);
        d->ops->set_features(d, accept);
    } else if (!(offered & VIRTIO_BIT(VIRTIO_F_VERSION_1))) {
        klog(KLOG_WARN, "virtio", "refused: %s: a legacy device, without VERSION_1", d->name);
        return virtio_fail(d, -ENODEV);
    } else {
        accept = offered & ((wanted & ~VIRTIO_RESERVED_MASK) | VIRTIO_CORE_FEATURES);
        d->ops->set_features(d, accept);
        virtio_status_add(d, VIRTIO_STATUS_FEATURES_OK);
    }
    if (!d->legacy && !(d->ops->get_status(d) & VIRTIO_STATUS_FEATURES_OK)) {
        klog(KLOG_WARN, "virtio", "refused: %s: the device did not take features %x:%x", d->name,
             (unsigned)(accept >> 32), (unsigned)accept);
        return virtio_fail(d, -EIO);
    }
    d->features = accept;
    klog(KLOG_INFO, "virtio", "features: %s: offered %x:%x, taken %x:%x", d->name,
         (unsigned)(offered >> 32), (unsigned)offered, (unsigned)(accept >> 32), (unsigned)accept);
    r = d->ops->irq_setup(d, nqueues);
    if (r != 0) {
        return virtio_fail(d, r);
    }
    d->nqueues = nqueues;
    return 0;
}

int virtio_has(const struct virtio_dev *d, unsigned bit)
{
    return bit < 64 && (d->features & VIRTIO_BIT(bit)) != 0;
}

uint64_t virtio_offered(struct virtio_dev *d)
{
    return d->ops->get_features(d);
}

int virtio_halt(struct virtio_dev *d)
{
    if (virtio_reset(d) == 0) {
        return 0;
    }
    if (d->ops->dma_enable) {
        d->ops->dma_enable(d, 0);
        klog(KLOG_WARN, "virtio", "stopped: %s: its memory access turned off", d->name);
    }
    return -ETIMEDOUT;
}

/** The bytes of each page of `vq`'s indirect tables: the tables a page
 * holds, which is all of them when they take less than a page. */
static uint32_t virtqueue_ind_bytes(const struct virtqueue *vq)
{
    uint32_t n = vq->ind_tables < vq->ind_per_page ? vq->ind_tables : vq->ind_per_page;

    return n * vq->ind_len * VIRTQ_DESC_BYTES;
}

/**
 * Give `vq` the indirect tables its device asks for (`indirect_len` and
 * `indirect_tables`), a page of them at a time from dma_alloc. A page that
 * cannot be had leaves the tables before it, and none when the first
 * cannot. The lengths are cut to a power of two within their bounds.
 */
static void virtqueue_ind_setup(struct virtio_dev *d, struct virtqueue *vq)
{
    uint16_t len = d->indirect_len ? d->indirect_len : VIRTQ_INDIRECT_LEN;
    uint16_t tables = d->indirect_tables ? d->indirect_tables : VIRTQ_INDIRECT_TABLES;
    uint16_t want, p;

    if (len > VIRTQ_INDIRECT_MAX) {
        len = VIRTQ_INDIRECT_MAX;
    }
    while (len & (len - 1u)) {
        len &= (uint16_t)(len - 1u);
    }
    if (len < VIRTQ_INDIRECT_LEN) {
        len = VIRTQ_INDIRECT_LEN;
    }
    if (tables > VIRTQ_INDIRECT_TABLES) {
        tables = VIRTQ_INDIRECT_TABLES;
    }
    vq->ind_len = len;
    vq->ind_per_page = (uint16_t)(VIRTQ_PAGE_BYTES / (len * VIRTQ_DESC_BYTES));
    vq->ind_tables = tables;
    want = (uint16_t)((tables + vq->ind_per_page - 1u) / vq->ind_per_page);
    for (p = 0; p < want; p++) {
        vq->ind_page[p] = (volatile struct virtq_desc *)d->ops->dma_alloc(
            d, virtqueue_ind_bytes(vq), &vq->ind_page_addr[p]);
        if (!vq->ind_page[p]) {
            break;
        }
        vq->ind_pages++;
    }
    if (vq->ind_pages < want) {
        vq->ind_tables = (uint16_t)(vq->ind_pages * vq->ind_per_page);
    }
    if (vq->ind_tables > 0) {
        vq->indirect = vq->ind_page[0];
        vq->indirect_addr = vq->ind_page_addr[0];
        vq->indirect_free = (uint32_t)((1ul << vq->ind_tables) - 1u);
    }
}

/** Free `vq`'s record, and its memory when `device_stopped` says the device
 * no longer reads it. */
static void virtqueue_free(struct virtio_dev *d, struct virtqueue *vq, int device_stopped)
{
    uint16_t p;

    if (device_stopped) {
        for (p = 0; p < vq->ind_pages; p++) {
            d->ops->dma_free(d, (void *)(uintptr_t)vq->ind_page[p], virtqueue_ind_bytes(vq));
        }
        if (vq->ring_mem) {
            d->ops->dma_free(d, vq->ring_mem, vq->ring_bytes);
        }
    }
    kfree(vq);
}

/**
 * Lay out a queue of `size` in its ring memory: the descriptors first,
 * aligned to 16; the driver's ring after them, aligned to 2; the device's
 * ring after that, aligned to 4 (virtio 1.2, 2.7), or to `align` on a
 * legacy device (2.7, "Legacy Interfaces"). Returns the bytes.
 */
static uint32_t virtqueue_layout(uint16_t size, uint32_t align, uint32_t *avail_off,
                                 uint32_t *used_off)
{
    uint32_t n = size;

    *avail_off = 16u * n;
    *used_off = (*avail_off + 6u + 2u * n + align - 1u) & ~(align - 1u);
    return *used_off + 6u + 8u * n;
}

/**
 * The size of queue `index` of `d`, which offers `max`: the largest power
 * of two up to VIRTQ_MAX_SIZE that `max` allows; on a legacy device `max`
 * itself, which must be a power of two, as Linux's vring_alloc_queue_split
 * asks. 0, with a record, for a legacy queue that cannot be had.
 */
static uint16_t virtqueue_size(struct virtio_dev *d, uint16_t index, uint16_t max)
{
    uint16_t size = VIRTQ_MAX_SIZE;

    if (d->legacy) {
        if (max & (max - 1u)) {
            klog(KLOG_WARN, "virtio", "refused: %s: legacy queue %d of %d entries", d->name, index,
                 max);
            return 0;
        }
        return max;
    }
    while (size > max) {
        size >>= 1;
    }
    return size;
}

/** A queue's record with its arrays of `size` after it, zeroed: NULL with
 * no memory. */
static struct virtqueue *virtqueue_record(uint16_t size)
{
    uint32_t n = size;
    struct virtqueue *vq = (struct virtqueue *)kzalloc(
        sizeof(*vq) + n * (sizeof(void *) + sizeof(uint32_t) + 2u * sizeof(uint16_t) + 1u));
    uint8_t *at;

    if (!vq) {
        return 0;
    }
    at = (uint8_t *)(vq + 1);
    vq->token = (void **)(void *)at;
    at += n * sizeof(void *);
    vq->inlen = (uint32_t *)(void *)at;
    at += n * sizeof(uint32_t);
    vq->chain = (uint16_t *)(void *)at;
    at += n * sizeof(uint16_t);
    vq->next = (uint16_t *)(void *)at;
    at += n * sizeof(uint16_t);
    vq->table = (int8_t *)at;
    return vq;
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
    size = virtqueue_size(d, index, max);
    if (size == 0) {
        return -EINVAL;
    }
    vq = virtqueue_record(size);
    if (!vq) {
        return -ENOMEM;
    }
    spin_lock_init(&vq->lock, "virtqueue");
    vq->dev = d;
    vq->index = index;
    vq->size = size;
    vq->vector = VIRTIO_NO_VECTOR;
    vq->done = done;
    vq->priv = priv;
    vq->ring_bytes =
        virtqueue_layout(size, d->legacy ? VIRTQ_LEGACY_ALIGN : 4u, &avail_off, &used_off);
    vq->ring_mem = d->ops->ring_alloc ? d->ops->ring_alloc(d, index, vq->ring_bytes, &vq->desc_addr)
                                      : d->ops->dma_alloc(d, vq->ring_bytes, &vq->desc_addr);
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
        virtqueue_ind_setup(d, vq);
    }
    for (i = 0; i < size; i++) {
        vq->next[i] = (uint16_t)(i + 1u);
        vq->table[i] = -1;
    }
    vq->event = virtio_has(d, VIRTIO_F_EVENT_IDX);
    vq->free_head = 0;
    vq->nfree = size;
    /* An indirect table holds a chain longer than the ring, as Linux's
     * virtqueue_add_split does (virtio_ring.c:576-580). */
    vq->max_chain = vq->indirect && vq->ind_len > size ? vq->ind_len : size;
    vq->max_bytes = 0;
    vq->max_seg = 0;
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

int virtio_ready(struct virtio_dev *d)
{
    d->ops->irq_start(d);
    virtio_status_add(d, VIRTIO_STATUS_DRIVER_OK);
    if (d->ops->get_status(d) & (VIRTIO_STATUS_FAILED | VIRTIO_STATUS_NEEDS_RESET)) {
        klog(KLOG_WARN, "virtio", "refused: %s: the device did not take DRIVER_OK", d->name);
        return -EIO;
    }
    return 0;
}

void virtio_teardown(struct virtio_dev *d)
{
    int stopped = virtio_reset(d) == 0;
    int i;

    if (!stopped && d->ops->dma_enable) {
        d->ops->dma_enable(d, 0);
        klog(KLOG_WARN, "virtio", "stopped: %s: its memory access turned off", d->name);
        stopped = 1;
    }
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

int virtio_backing_fill(struct virtio_dev *d, struct shmem *sh, uint64_t bytes)
{
    return d->ops->backing_fill ? d->ops->backing_fill(d, sh, bytes) : 0;
}

void virtio_backing_release(struct virtio_dev *d, struct shmem *sh)
{
    if (d->ops->backing_release) {
        d->ops->backing_release(d, sh);
    }
}

int virtio_map_pages(struct virtio_dev *d, const struct virtio_run *runs, uint32_t n,
                     void (*revoke)(void *owner), void *owner, struct virtio_map **out)
{
    *out = NULL;
    if (!d->pages || !d->ops->map_pages) {
        return -ENODEV;
    }
    return d->ops->map_pages(d, runs, n, revoke, owner, out);
}

uintptr_t virtio_map_frame(struct virtio_dev *d, struct virtio_map *m, uint64_t i)
{
    return m && d->ops->map_frame ? d->ops->map_frame(m, i) : 0;
}

void virtio_map_free(struct virtio_dev *d, struct virtio_map *m)
{
    if (m && d->ops->map_free) {
        d->ops->map_free(m);
    }
}

int virtio_dma_addr(struct virtio_dev *d, uintptr_t phys, uint64_t *addr)
{
    if (d->ops->dma_addr) {
        return d->ops->dma_addr(d, phys, addr);
    }
    *addr = (uint64_t)phys;
    return 0;
}

void virtio_put(struct virtio_dev *d)
{
    if (d->ops->put) {
        d->ops->put(d);
    }
}

/**
 * The run of segments of `sg` from `k` that one descriptor of `vq` holds:
 * those of the same way, the first `out` read by the device, whose device
 * addresses follow each other, while the length stays within 32 bits and
 * within the queue's `max_seg`, as Linux's block layer merges no segment
 * past its queue's max_segment_size. Returns the index after the run, with
 * its length in `*len`.
 */
static unsigned virtqueue_run(const struct virtqueue *vq, const struct virtio_sg *sg, unsigned k,
                              unsigned out, unsigned total, uint32_t *len)
{
    uint32_t most = vq->max_seg ? vq->max_seg : 0xFFFFFFFFu;
    uint64_t end = sg[k].addr + sg[k].len;
    uint32_t n = sg[k].len;
    unsigned j = k + 1u;

    while (j < total && (j < out) == (k < out) && sg[j].addr == end && n <= most &&
           sg[j].len <= most - n) {
        n += sg[j].len;
        end += sg[j].len;
        j++;
    }
    *len = n;
    return j;
}

/** The descriptors a buffer takes once its runs are merged, with the bytes
 * of its longest way in `*most`. */
static unsigned virtqueue_descs(const struct virtqueue *vq, const struct virtio_sg *sg,
                                unsigned out, unsigned total, uint64_t *most)
{
    uint64_t bytes[2];
    unsigned k = 0, n = 0;
    uint32_t len;

    bytes[0] = bytes[1] = 0;
    while (k < total) {
        int way = k >= out;

        k = virtqueue_run(vq, sg, k, out, total, &len);
        bytes[way] += len;
        n++;
    }
    *most = bytes[0] > bytes[1] ? bytes[0] : bytes[1];
    return n;
}

int virtqueue_chain_fits(const struct virtqueue *vq, const struct virtio_sg *sg, unsigned out,
                         unsigned in)
{
    uint64_t most;
    unsigned n = virtqueue_descs(vq, sg, out, out + in, &most);

    return n <= vq->max_chain && (vq->max_bytes == 0 || most <= vq->max_bytes);
}

/** Fill descriptor `dd` with the run of `len` bytes from the segment `seg`,
 * which the device writes when `write` is set; `next` is linked when
 * another run follows, which `more` says. */
static void virtqueue_fill(volatile struct virtq_desc *dd, const struct virtio_sg *seg, int write,
                           uint32_t len, int more, uint16_t next)
{
    uint16_t flags = write ? VIRTQ_DESC_F_WRITE : 0;

    dd->addr = seg->addr;
    dd->len = len;
    if (more) {
        flags |= VIRTQ_DESC_F_NEXT;
        dd->next = next;
    }
    dd->flags = flags;
}

int virtqueue_add(struct virtqueue *vq, const struct virtio_sg *sg, unsigned out, unsigned in,
                  void *token)
{
    unsigned total = out + in, descs, k, j, d;
    uint16_t head, i, slot;
    uint32_t flags, len;
    uint64_t most;
    int t;

    if (total == 0 || !token) {
        return -EINVAL;
    }
    descs = virtqueue_descs(vq, sg, out, total, &most);
    if (descs > vq->max_chain || (vq->max_bytes != 0 && most > vq->max_bytes)) {
        return -E2BIG;
    }
    flags = spin_lock_irqsave(&vq->lock);
    head = vq->free_head;
    if (descs <= vq->nfree) {
        i = head;
        for (k = 0; k < total; k = j) {
            uint16_t next = vq->next[i];

            j = virtqueue_run(vq, sg, k, out, total, &len);
            virtqueue_fill(&vq->desc[i], &sg[k], k >= out, len, j < total, next);
            if (j < total) {
                i = next;
            }
        }
        vq->free_head = vq->next[i];
        vq->nfree = (uint16_t)(vq->nfree - descs);
        vq->chain[head] = (uint16_t)descs;
        vq->table[head] = -1;
    } else if (vq->indirect && vq->indirect_free != 0 && vq->nfree > 0 && descs <= vq->ind_len) {
        volatile struct virtq_desc *tbl;
        uint64_t tbl_addr;
        uint32_t at;

        t = 0;
        while (!(vq->indirect_free & (1u << t))) {
            t++;
        }
        vq->indirect_free &= ~(1u << t);
        at = (uint32_t)(t % vq->ind_per_page) * vq->ind_len;
        tbl = vq->ind_page[t / vq->ind_per_page] + at;
        tbl_addr = vq->ind_page_addr[t / vq->ind_per_page] + (uint64_t)at * VIRTQ_DESC_BYTES;
        d = 0;
        for (k = 0; k < total; k = j) {
            j = virtqueue_run(vq, sg, k, out, total, &len);
            virtqueue_fill(&tbl[d], &sg[k], k >= out, len, j < total, (uint16_t)(d + 1u));
            d++;
        }
        vq->free_head = vq->next[head];
        vq->desc[head].addr = tbl_addr;
        vq->desc[head].len = (uint32_t)(descs * VIRTQ_DESC_BYTES);
        vq->desc[head].flags = VIRTQ_DESC_F_INDIRECT;
        vq->nfree--;
        vq->chain[head] = 1;
        vq->table[head] = (int8_t)t;
    } else {
        spin_unlock_irqrestore(&vq->lock, flags);
        return -ENOSPC;
    }
    vq->token[head] = token;
    vq->inlen[head] = 0;
    for (k = out; k < total; k++) {
        vq->inlen[head] += sg[k].len;
    }
    slot = (uint16_t)(vq->avail_idx & (vq->size - 1u));
    vq->avail[2 + slot] = head;
    arch_dma_fence(); /* the chain and its ring entry before the index */
    vq->avail_idx++;
    vq->avail[1] = vq->avail_idx;
    vq->added++;
    spin_unlock_irqrestore(&vq->lock, flags);
    return 0;
}

/**
 * With VIRTIO_F_EVENT_IDX the device keeps, after its ring's entries, the
 * driver's index at which it wants the next notify, `avail_event`, and asks
 * for none while it has not taken the buffers it was told of; Linux's
 * virtqueue_kick_prepare_split notifies only when the buffers added since
 * the last kick, from `old` to `new`, pass that index
 * (drivers/virtio/virtio_ring.c:734-763 in 6.12.115). The index is read
 * after the ring's, in the queue's lock, which keeps the last kick's index.
 */
void virtqueue_kick(struct virtqueue *vq)
{
    uint32_t flags = spin_lock_irqsave(&vq->lock);
    uint16_t old = vq->kicked_idx, now = vq->avail_idx, event;
    int notify;

    vq->kicked_idx = now;
    arch_dma_fence(); /* the index is out before the device's word is read */
    if (vq->event) {
        event = vq->used[2u + 4u * vq->size];
        notify = (uint16_t)(now - event - 1u) < (uint16_t)(now - old);
    } else {
        notify = !(vq->used[0] & VIRTQ_USED_F_NO_NOTIFY);
    }
    spin_unlock_irqrestore(&vq->lock, flags);
    if (notify) {
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
            last = vq->next[last];
        }
    }
    vq->next[last] = vq->free_head;
    vq->free_head = head;
    vq->nfree = (uint16_t)(vq->nfree + vq->chain[head]);
    vq->chain[head] = 0;
    vq->token[head] = 0;
}

/**
 * With VIRTIO_F_EVENT_IDX the device interrupts only for the entry that
 * passes the driver's `used_event`, after its ring's entries, as Linux's
 * virtqueue_get_buf_ctx_split asks for the next one (virtio_ring.c:866-874).
 * The reap points it at `until`, the end of what it takes, before it takes
 * anything, so that the first buffer returned after those raises an
 * interrupt; one returned while it pointed there extends `until`. The
 * pointing is in the queue's lock, so that a later reap's mark is never
 * overwritten by an earlier one's.
 */
int virtqueue_reap(struct virtqueue *vq)
{
    uint32_t flags = spin_lock_irqsave(&vq->lock);
    uint16_t until = vq->used[1], left, now;
    int n = 0;

    while (vq->event && !vq->quiet) {
        vq->avail[2u + vq->size] = until;
        arch_dma_fence(); /* the mark is out before the device's index is read */
        now = vq->used[1];
        if (now == until) {
            break;
        }
        until = now;
    }
    spin_unlock_irqrestore(&vq->lock, flags);
    for (;;) {
        uint16_t used_idx;
        uint32_t id, len, over;
        void *token;

        flags = spin_lock_irqsave(&vq->lock);
        used_idx = vq->used[1];
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
        /* Past `until`, here or in a reap that interrupted this one. */
        left = (uint16_t)(until - vq->last_used);
        if (left == 0 || left > vq->size) {
            spin_unlock_irqrestore(&vq->lock, flags);
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
        over = vq->inlen[id];
        virtqueue_release(vq, (uint16_t)id);
        vq->completed++;
        spin_unlock_irqrestore(&vq->lock, flags);
        if (len > over) {
            klog(KLOG_ERR, "virtio", "queue: %s: %d: the device wrote %u bytes into head %d of %u",
                 vq->dev->name, vq->index, (unsigned)len, (int)id, (unsigned)over);
            len = over;
        }
        if (vq->done) {
            vq->done(vq, token, len);
        }
        n++;
    }
    if (n > 0 && vq->batch) {
        vq->batch(vq);
    }
    return n;
}

/**
 * Linux's virtqueue_disable_cb_split writes 0 as the `used_event` of a
 * queue it keeps quiet (drivers/virtio/virtio_ring.c:879-903 in 6.12.115),
 * which the device passes at its first buffer; the index before the last
 * one reaped is the one it reaches last, 65535 buffers on. Turning the
 * interrupt on points `used_event` at the next buffer, and the device's
 * index is read after it, so that a buffer returned before the write is
 * reported to the caller, as virtqueue_enable_cb_prepare_split and
 * virtqueue_poll_split do together.
 */
int virtqueue_irq(struct virtqueue *vq, int on)
{
    uint32_t flags = spin_lock_irqsave(&vq->lock);
    int waiting;

    vq->quiet = !on;
    if (vq->event) {
        vq->avail[2u + vq->size] = on ? vq->last_used : (uint16_t)(vq->last_used - 1u);
    } else {
        vq->avail[0] = on ? 0 : VIRTQ_AVAIL_F_NO_INTERRUPT;
    }
    arch_dma_fence(); /* the word is out before the device's index is read */
    waiting = on && vq->used[1] != vq->last_used;
    spin_unlock_irqrestore(&vq->lock, flags);
    return waiting;
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

    for (i = 0; i < (int)d->nqueues; i++) {
        virtio_interrupt_queue(d->queues[i]);
    }
}

void virtio_interrupt_config(struct virtio_dev *d)
{
    /* Read once: a driver clears it while a handler may run elsewhere. */
    void (*changed)(struct virtio_dev *) = ((volatile struct virtio_dev *)d)->config_changed;

    d->config_events++;
    if (changed) {
        changed(d);
    }
}
