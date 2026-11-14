/*
 * virtio_blk.c: the virtio block driver, as Linux's virtio_blk: bring-up of
 * each device on the shared virtio core, a request queue on each of its
 * virtqueues, the polled and the interrupt-driven transfer, flushes,
 * discards and writes of zeroes, and a device that fails or is removed,
 * each disk under its queues' locks and never the big lock.
 * `sys/block/Readme.md` describes the design.
 */

#include "virtio_blk.h"
#include "block.h"
#include "disk.h"
#include "diskcmd.h"
#include "loop.h"

#include "../arch/arch.h"
#include "../cmdline.h"
#include "../fs/bcache.h"
#include "../fs/devfs.h"
#include "../fs/vfs.h"
#include "../klog.h"
#include "../latency.h"
#include "../mm/paging.h"
#include "../proc/percpu.h"
#include "../proc/smp.h"
#include "../timer.h"
#include "../udelay.h"
#include "../virtio/virtio.h"

#include "../lib/kerrno.h"
#include "../lib/kint.h"
#include "../lib/kirq.h"
#include "../lib/kstring.h"
#include "../lib/spinlock.h"

/* The block device's feature bits (virtio 1.2, 5.2.3) that this driver
 * takes; GEOMETRY and TOPOLOGY only describe the disk, and the legacy,
 * zoned, lifetime and secure-erase bits it does not use. */
#define VBLK_F_SIZE_MAX     1
#define VBLK_F_SEG_MAX      2
#define VBLK_F_RO           5
#define VBLK_F_BLK_SIZE     6
#define VBLK_F_FLUSH        9
#define VBLK_F_CONFIG_WCE   11
#define VBLK_F_MQ           12
#define VBLK_F_DISCARD      13
#define VBLK_F_WRITE_ZEROES 14
#define VBLK_WANTED                                                                           \
    (VIRTIO_BIT(VBLK_F_SIZE_MAX) | VIRTIO_BIT(VBLK_F_SEG_MAX) | VIRTIO_BIT(VBLK_F_RO) |       \
     VIRTIO_BIT(VBLK_F_BLK_SIZE) | VIRTIO_BIT(VBLK_F_FLUSH) | VIRTIO_BIT(VBLK_F_CONFIG_WCE) | \
     VIRTIO_BIT(VBLK_F_MQ) | VIRTIO_BIT(VBLK_F_DISCARD) | VIRTIO_BIT(VBLK_F_WRITE_ZEROES))

/* The device's configuration, as offsets of `struct virtio_blk_config`
 * (virtio 1.2, 5.2.4), and the bytes of it this driver reads. */
#define VBLK_CFG_CAPACITY    0
#define VBLK_CFG_SIZE_MAX    8
#define VBLK_CFG_SEG_MAX     12
#define VBLK_CFG_BLK_SIZE    20
#define VBLK_CFG_WRITEBACK   32
#define VBLK_CFG_NUM_QUEUES  34
#define VBLK_CFG_MAX_DISCARD 36
#define VBLK_CFG_MAX_ZEROES  48
#define VBLK_CFG_BYTES       60

/* Request types and the status byte (virtio 1.2, 5.2.6). A status this
 * driver writes before the device answers, so that a buffer returned with
 * no status written is an error. */
#define VBLK_T_IN           0
#define VBLK_T_OUT          1
#define VBLK_T_FLUSH        4
#define VBLK_T_GET_ID       8
#define VBLK_T_DISCARD      11
#define VBLK_T_WRITE_ZEROES 13
#define VBLK_S_OK           0
#define VBLK_S_IOERR        1
#define VBLK_S_UNSUPP       2
#define VBLK_S_NONE         0xFFu
#define VBLK_S_RESET        0x100 /* the driver gave the device up; no byte */
#define VBLK_ID_BYTES       20

/*
 * The sizes this driver chooses. A queue has VBLK_SLOTS commands in flight
 * at most, each with 128 bytes of the queue's page: the request header at
 * 0, the status byte at 16, a discard's range at 32 and GET_ID's answer at
 * 64. A chain is the header, the data and the status, and with indirect
 * tables of VIRTQ_INDIRECT_MAX descriptors carries VBLK_SEGS pieces of
 * data; VBLK_TABLES of them for each queue serve the requests the block
 * layer lets one device have in flight, eight, and a polled command beside
 * them takes the queue's own descriptors when no table is free.
 */
#define VBLK_DISKS      8
#define VBLK_SLOTS      16
#define VBLK_SLOT_BYTES 128u
#define VBLK_HDR        0u
#define VBLK_STATUS     16u
#define VBLK_RANGE      32u
#define VBLK_ID         64u
#define VBLK_SEGS       (VIRTQ_INDIRECT_MAX - 2)
#define VBLK_TABLES     8
#define VBLK_PAGE       4096u

/* The waits of a polled command: a step of 10 us, thirty seconds with
 * interrupts on and five with them off, AHCI's and NVMe's bounds and for
 * their reasons. */
#define VBLK_POLL_US          10u
#define VBLK_CMD_STEPS        3000000u
#define VBLK_CMD_STEPS_MASKED 500000u

/* What a slot is doing. */
#define VBLK_FREE      0
#define VBLK_ISSUED    1 /* with the device */
#define VBLK_FLUSH_DUE 2 /* its write answered; its flush half waits for room */

struct vblk;
struct vblk_queue;

/**
 * A command on a queue: a request of the interrupt-driven path, or, with
 * `request` NULL, a polled command whose caller reads `done` and `status`
 * and frees the slot.
 */
struct vblk_slot {
    int state;
    uint32_t type; /* VBLK_T_* as last issued */
    uint32_t fseq; /* the disk's `wseq` when this flush was issued */
    struct block_request *request;
    uint32_t generation; /* the request's, which its completion gives back */
    volatile int done;
    volatile int status;   /* VBLK_S_*, or VBLK_S_RESET */
    volatile uint8_t *mem; /* the slot's bytes of its queue's page */
    uint64_t addr;         /* their address as the device reads it */
    struct vblk_queue *q;
};

/**
 * One virtqueue and the requests waiting for it, under `lock`, taken with
 * interrupts off, before the virtqueue's own lock and the disk's `lock`,
 * and the request pool's after them, as Linux's blk-mq hardware context
 * runs a queue's requests (block/blk-mq.c). `sg` is the chain being built,
 * under the lock.
 */
struct vblk_queue {
    struct spinlock lock;
    struct vblk *vb;
    struct virtqueue *vq;
    int index;
    struct vblk_slot slots[VBLK_SLOTS];
    struct block_request *head, *tail; /* waiting, oldest first */
    int unkicked;                      /* a chain was added that the device was not told of */
    volatile uint8_t *page;
    uint64_t page_addr;
    struct virtio_sg sg[VBLK_SEGS + 2];
};

/**
 * A virtio block device and the disk it is. `lock`, taken with interrupts
 * off inside a queue's lock or alone, covers `wseq`, the disk's
 * `unflushed`, `pollers`, the capacity and `disk`; `dead` and `closed` are
 * set once, under it, and read anywhere. `dev` is the transport's record
 * until the removal lets it go: a caller reaches it under a queue's lock
 * with the disk not dead, or counted in `pollers`, which the removal waits
 * for. The record is never freed: a disk that has gone keeps answering EIO
 * to whatever still names it, and the record serves a new device once
 * nothing holds the disk.
 */
struct vblk {
    struct spinlock lock;
    struct virtio_dev *dev;
    char name[16]; /* the device's name on the virtio bus, for the log */
    int number;    /* the place among the devices this driver took */
    int used;      /* the record belongs to a device, or to a disk still held */
    int up;        /* brought up and registered */
    int started;   /* on its interrupts */
    int gone;      /* removed from the transport */
    volatile int dead;
    volatile int closed;       /* shutting down: no new command is taken */
    volatile int config_dirty; /* the configuration changed, and its read
                                * waits for a process (`virtio_blk_service`) */
    int config_poll;           /* no interrupt tells of a change: a polled
                                * transfer reads the capacity first */
    uint32_t pollers;          /* callers inside the device: polled commands
                                * and readers of its configuration */
    uint32_t wseq;             /* writes answered without a flush, counted */
    int cache;                 /* a volatile write cache, which FLUSH empties */
    int ro;
    uint32_t seg_max;   /* data pieces one chain may carry */
    uint32_t seg_bytes; /* the bytes one piece may hold */
    uint32_t size_max;  /* the device's `size_max`, or 0 for a device with none */
    uint16_t nqueues;
    block_device_t bdev;
    struct disk *disk;
    char serial[VBLK_ID_BYTES + 1];
    struct vblk_queue q[VIRTIO_MAX_QUEUES];
};

/** One command as the driver issues it. `buf` is the memory the device is
 * given and `frames` the page frames of a read into frames, one of them;
 * `id` receives GET_ID's answer. */
struct vblk_cmd {
    uint32_t type;
    uint64_t sector;
    uint32_t count; /* sectors */
    const uint8_t *buf;
    const uintptr_t *frames;
    uint8_t *id;
};

static struct vblk vblks[VBLK_DISKS];
static int vblk_registered; /* the driver is registered for later devices */

/* Work waits for a process: a removed disk that something still held, or a
 * configuration change its interrupt could not read. Set anywhere, and
 * cleared by `virtio_blk_service` before it looks. */
static volatile int vblk_waiting;

/* Why the last bring-up step failed, for the line that reports it. */
static const char *vblk_why = "";

static int vblk_read(block_device_t *device, uint64_t lba, uint32_t count, void *buffer);
static int vblk_write(block_device_t *device, uint64_t lba, uint32_t count, const void *buffer);
static void vblk_kill(struct vblk *vb, const char *why);

/** A little-endian field of `n` bytes, up to 8, at `p`. */
static uint64_t vblk_le(const uint8_t *p, int n)
{
    uint64_t v = 0;

    while (n-- > 0) {
        v = (v << 8) | p[n];
    }
    return v;
}

/** Write `v` at `p` as a little-endian field of `n` bytes. */
static void vblk_put_le(volatile uint8_t *p, uint64_t v, int n)
{
    int i;

    for (i = 0; i < n; i++) {
        p[i] = (uint8_t)(v >> (8 * i));
    }
}

/** The polled wait's bound in steps, by whether interrupts are on. */
static uint32_t vblk_cmd_steps(void)
{
    return kirq_enabled() ? VBLK_CMD_STEPS : VBLK_CMD_STEPS_MASKED;
}

/**
 * One step of a polled wait: with interrupts off, answer the requests
 * other processors left this one (`smp_service`), as AHCI's and NVMe's
 * steps do, since a processor dropping a translation everywhere waits for
 * this one's answer and no interrupt brings it; then the delay.
 */
static void vblk_poll_step(void)
{
    if (!kirq_enabled()) {
        smp_service();
    }
    k_udelay(VBLK_POLL_US);
}

/** The ATA command the disk policy judges a request type by: a read by a
 * read, a write by a write, a flush by a flush and GET_ID by IDENTIFY; a
 * discard, a write of zeroes and any other by 0, which the read-only policy
 * does not allow. */
static uint8_t vblk_policy_command(uint32_t type)
{
    switch (type) {
    case VBLK_T_IN:
        return DISKCMD_READ_DMA_EXT;
    case VBLK_T_OUT:
        return DISKCMD_WRITE_DMA_EXT;
    case VBLK_T_FLUSH:
        return DISKCMD_FLUSH_CACHE_EXT;
    case VBLK_T_GET_ID:
        return DISKCMD_IDENTIFY;
    default:
        return 0;
    }
}

/**
 * The queue a caller on this processor uses: processor N takes queue N
 * modulo the queues, as Linux's blk-mq maps each processor to a hardware
 * queue (block/blk-mq-cpumap.c).
 */
static struct vblk_queue *vblk_queue_here(struct vblk *vb)
{
    return &vb->q[arch_cpu_id() % vb->nqueues];
}

/*
 * Building a chain.
 */

/**
 * Append the `bytes` at `buf` to `q`'s chain from piece `n`, a piece for
 * each run within a page and within the device's `size_max`, at the
 * device's addresses of their frames. Returns the pieces after them, or -1
 * when a page has no physical address or the pieces pass `limit`.
 */
static int vblk_buf_segs(struct vblk_queue *q, int n, int limit, const uint8_t *buf, uint32_t bytes)
{
    struct vblk *vb = q->vb;

    while (bytes > 0) {
        uint32_t piece = VBLK_PAGE - (uint32_t)((uintptr_t)buf & (VBLK_PAGE - 1u));
        uintptr_t phys = paging_virt_to_phys((uintptr_t)buf);
        uint64_t addr;

        if (piece > bytes) {
            piece = bytes;
        }
        if (piece > vb->seg_bytes) {
            piece = vb->seg_bytes;
        }
        if (phys == 0 || n >= limit || virtio_dma_addr(vb->dev, phys, &addr) != 0) {
            return -1;
        }
        q->sg[n].addr = addr;
        q->sg[n].len = piece;
        n++;
        buf += piece;
        bytes -= piece;
    }
    return n;
}

/** Append `bytes` in the page frames `frames`, a page each, to `q`'s chain
 * from piece `n`, as `vblk_buf_segs` appends a buffer. */
static int vblk_frame_segs(struct vblk_queue *q, int n, int limit, const uintptr_t *frames,
                           uint32_t bytes)
{
    struct vblk *vb = q->vb;
    uint32_t off = 0;

    while (bytes > 0) {
        uint32_t piece = VBLK_PAGE - (off & (VBLK_PAGE - 1u));
        uint64_t addr;

        if (piece > bytes) {
            piece = bytes;
        }
        if (piece > vb->seg_bytes) {
            piece = vb->seg_bytes;
        }
        if (n >= limit ||
            virtio_dma_addr(vb->dev, frames[off / VBLK_PAGE] + (off & (VBLK_PAGE - 1u)), &addr) !=
                0) {
            return -1;
        }
        q->sg[n].addr = addr;
        q->sg[n].len = piece;
        n++;
        off += piece;
        bytes -= piece;
    }
    return n;
}

/**
 * Build `c` in slot `s` as `q`'s chain: the header the device reads, then
 * the data, a discard's range or room for GET_ID's answer, then the status
 * byte the device writes, preset so that a buffer returned unwritten fails.
 * Returns the pieces, with the device's in `*in`; -EPERM when the disk
 * policy does not allow the command, or -E2BIG when its data cannot be
 * described in the pieces a chain carries. `q`'s lock is held.
 */
static int vblk_build(struct vblk_queue *q, struct vblk_slot *s, const struct vblk_cmd *c, int *in)
{
    struct vblk *vb = q->vb;
    int limit = (int)vb->seg_max + 1, n = 1, first_in;

    if (!diskcmd_allowed(vblk_policy_command(c->type))) {
        klog(KLOG_WARN, "virtio-blk", "%s: refused: request type %u: %s", vb->name,
             (unsigned)c->type, diskcmd_policy_banner);
        return -EPERM;
    }
    vblk_put_le(s->mem + VBLK_HDR, c->type, 4);
    vblk_put_le(s->mem + VBLK_HDR + 4u, 0, 4);
    vblk_put_le(s->mem + VBLK_HDR + 8u,
                c->type == VBLK_T_IN || c->type == VBLK_T_OUT ? c->sector : 0, 8);
    s->mem[VBLK_STATUS] = VBLK_S_NONE;
    q->sg[0].addr = s->addr + VBLK_HDR;
    q->sg[0].len = 16;
    first_in = 1;
    if (c->type == VBLK_T_OUT) {
        n = vblk_buf_segs(q, 1, limit, c->buf, c->count * BLOCK_SECTOR_SIZE);
        first_in = n;
    } else if (c->type == VBLK_T_IN) {
        n = c->frames ? vblk_frame_segs(q, 1, limit, c->frames, c->count * BLOCK_SECTOR_SIZE)
                      : vblk_buf_segs(q, 1, limit, c->buf, c->count * BLOCK_SECTOR_SIZE);
    } else if (c->type == VBLK_T_DISCARD || c->type == VBLK_T_WRITE_ZEROES) {
        vblk_put_le(s->mem + VBLK_RANGE, c->sector, 8);
        vblk_put_le(s->mem + VBLK_RANGE + 8u, c->count, 4);
        vblk_put_le(s->mem + VBLK_RANGE + 12u, 0, 4); /* no unmap: BLKZEROOUT's NOUNMAP */
        q->sg[1].addr = s->addr + VBLK_RANGE;
        q->sg[1].len = 16;
        n = 2;
        first_in = 2;
    } else if (c->type == VBLK_T_GET_ID) {
        q->sg[1].addr = s->addr + VBLK_ID;
        q->sg[1].len = VBLK_ID_BYTES;
        n = 2;
    }
    if (n < 0) {
        return -E2BIG;
    }
    q->sg[n].addr = s->addr + VBLK_STATUS;
    q->sg[n].len = 1;
    *in = n + 1 - first_in;
    s->type = c->type;
    return n + 1;
}

/**
 * Build `c` in slot `s` and offer it on `q`'s virtqueue, without telling
 * the device. Returns 0; 1 when the queue has no room now and a completion
 * will make it; -EPERM when the disk policy refuses it; -E2BIG when it does
 * not fit one chain. `q`'s lock is held.
 */
static int vblk_offer(struct vblk_queue *q, struct vblk_slot *s, const struct vblk_cmd *c)
{
    int in, total = vblk_build(q, s, c, &in), rc;

    if (total < 0) {
        return total;
    }
    rc = virtqueue_add(q->vq, q->sg, (unsigned)(total - in), (unsigned)in, s);
    if (rc == -ENOSPC) {
        return 1;
    }
    return rc == 0 ? 0 : -E2BIG;
}

/** A free slot of `q`, marked issued, or NULL. `q`'s lock is held. */
static struct vblk_slot *vblk_slot_take(struct vblk_queue *q)
{
    int i;

    for (i = 0; i < VBLK_SLOTS; i++) {
        struct vblk_slot *s = &q->slots[i];

        if (s->state == VBLK_FREE) {
            s->state = VBLK_ISSUED;
            s->done = 0;
            s->status = VBLK_S_NONE;
            s->request = 0;
            return s;
        }
    }
    return 0;
}

/** Give `s` back. `q`'s lock is held. */
static void vblk_slot_free(struct vblk_slot *s)
{
    s->state = VBLK_FREE;
    s->request = 0;
}

/*
 * The write cache. A device whose cache is on (`vblk_cache_mode`) answers
 * a write, of data or of zeroes, once the write is in its cache, and has no
 * FUA: a write is durable after a FLUSH that follows its answer, as Linux's
 * block layer makes a FUA write on a queue without FUA a write and a flush
 * (block/blk-flush.c). A discard the cache holds is on the medium after the
 * next FLUSH, which Linux sends at each fsync; `block_flush` sends one only
 * to a disk marked unflushed, so a discard marks the disk too.
 */

/** A write or a discard was answered without a flush, or the flush after
 * it failed: the disk is marked, and the count of such answers moves on. */
static void vblk_unflushed(struct vblk *vb)
{
    uint32_t flags = spin_lock_irqsave(&vb->lock);

    vb->wseq++;
    vb->bdev.unflushed = 1;
    spin_unlock_irqrestore(&vb->lock, flags);
}

/** The count a flush covers, read before it is issued. */
static uint32_t vblk_wseq(struct vblk *vb)
{
    uint32_t flags = spin_lock_irqsave(&vb->lock);
    uint32_t n = vb->wseq;

    spin_unlock_irqrestore(&vb->lock, flags);
    return n;
}

/**
 * A flush issued when the count of unflushed writes was `fseq` finished
 * well. It covers every write answered before it was issued (virtio 1.2,
 * 5.2.6.2), and so clears the mark unless a write was answered unflushed
 * while it ran, as NVMe's `nvme_flushed` does and for its reason.
 */
static void vblk_flushed(struct vblk *vb, uint32_t fseq)
{
    uint32_t flags = spin_lock_irqsave(&vb->lock);

    if (vb->wseq == fseq) {
        vb->bdev.unflushed = 0;
    }
    spin_unlock_irqrestore(&vb->lock, flags);
}

/**
 * Offer the flush that makes the write in slot `s`, which the device has
 * answered, durable: 0, 1 when the queue has no room yet, which leaves the
 * slot VBLK_FLUSH_DUE for `vblk_start_next`, or a negative errno. An offer
 * marks `q` unkicked, and `vblk_start_next`, which every answer is
 * followed by, tells the device. `q`'s lock is held.
 */
static int vblk_flush_half(struct vblk_queue *q, struct vblk_slot *s)
{
    struct vblk_cmd c;
    int rc;

    k_memset(&c, 0, sizeof(c));
    c.type = VBLK_T_FLUSH;
    s->fseq = vblk_wseq(q->vb);
    rc = vblk_offer(q, s, &c);
    s->state = rc == 1 ? VBLK_FLUSH_DUE : VBLK_ISSUED;
    if (rc == 0) {
        block_count_command();
        block_count_flush();
        q->unkicked = 1;
    }
    return rc;
}

/*
 * The request queue.
 */

/** Answer the request in slot `s` with `rc` and free the slot. `q`'s lock
 * is held, and the request pool's lock nests inside it. */
static void vblk_end(struct vblk_slot *s, int rc)
{
    struct block_request *r = s->request;
    uint32_t generation = s->generation;

    vblk_slot_free(s);
    latency_span(LAT_DISK, r->taken_us, timer_now_us());
    block_request_complete(r, generation, rc);
}

/** The command for request `r`. */
static void vblk_request_cmd(const struct block_request *r, struct vblk_cmd *c)
{
    k_memset(c, 0, sizeof(*c));
    c->sector = r->lba;
    c->count = r->count;
    c->buf = r->page;
    c->frames = r->frames;
    if (r->flags & BLOCK_REQ_FLUSH) {
        c->type = VBLK_T_FLUSH;
    } else if (r->flags & BLOCK_REQ_DISCARD) {
        c->type = VBLK_T_DISCARD;
    } else if (r->flags & BLOCK_REQ_ZEROES) {
        c->type = VBLK_T_WRITE_ZEROES;
    } else {
        c->type = r->write ? VBLK_T_OUT : VBLK_T_IN;
    }
}

/**
 * Offer what waits on `q` while it has room and slots: first the flush
 * halves that found no room, then the waiting requests, oldest first, and
 * then tell the device once of what was offered since it was last told,
 * the flush halves an answer offered among them. A dead disk answers each
 * waiting request with EIO. `q`'s lock is held.
 */
static void vblk_start_next(struct vblk_queue *q)
{
    struct vblk *vb = q->vb;
    int i;

    if (vb->dead) {
        while (q->head) {
            struct block_request *r = q->head;

            q->head = r->queued;
            r->queued = 0;
            block_request_complete(r, r->generation, -EIO);
        }
        q->tail = 0;
        return;
    }
    for (i = 0; i < VBLK_SLOTS; i++) {
        struct vblk_slot *s = &q->slots[i];
        int rc;

        if (s->state != VBLK_FLUSH_DUE) {
            continue;
        }
        rc = vblk_flush_half(q, s);
        if (rc == 1) {
            goto kick;
        }
        if (rc < 0) {
            vblk_unflushed(vb);
            vblk_end(s, -EIO);
        }
    }
    while (q->head) {
        struct block_request *r = q->head;
        struct vblk_slot *s = vblk_slot_take(q);
        struct vblk_cmd c;
        int rc;

        if (!s) {
            break;
        }
        vblk_request_cmd(r, &c);
        if (c.type == VBLK_T_FLUSH) {
            s->fseq = vblk_wseq(vb);
        }
        rc = vblk_offer(q, s, &c);
        if (rc == 1) {
            vblk_slot_free(s);
            break;
        }
        q->head = r->queued;
        if (!q->head) {
            q->tail = 0;
        }
        r->queued = 0;
        if (rc < 0) {
            if (rc == -E2BIG) {
                klog(KLOG_WARN, "virtio-blk", "%s: refused: a request of more than %u pieces",
                     vb->name, (unsigned)vb->seg_max);
            }
            vblk_slot_free(s);
            block_request_complete(r, r->generation, -EIO);
            continue;
        }
        if (c.type == VBLK_T_FLUSH) {
            block_count_flush();
        }
        block_count_command();
        s->request = r;
        s->generation = r->generation;
        q->unkicked = 1;
    }
kick:
    if (q->unkicked) {
        q->unkicked = 0;
        virtqueue_kick(q->vq);
    }
}

/**
 * The device has answered slot `s` with `status`. A polled command is
 * marked done for its caller, who frees the slot. A request is answered.
 * On a disk with a cache, a write of data or of zeroes whose request has
 * `BLOCK_REQ_NOFLUSH` marks the disk, and so does a discard; any other
 * write that succeeded has its flush offered in its slot first, and is
 * answered with that flush. A flush that succeeded clears the mark
 * (`vblk_flushed`); one that fails or cannot be offered, a write's or a
 * flush request's, answers EIO and marks the disk, since what the cache
 * holds is on no medium. `q`'s lock is held.
 */
static void vblk_answer(struct vblk_queue *q, struct vblk_slot *s, int status)
{
    struct vblk *vb = q->vb;
    struct block_request *r = s->request;
    int rc = status == VBLK_S_OK ? 0 : status == VBLK_S_UNSUPP ? -EOPNOTSUPP : -EIO;
    int wrote = s->type == VBLK_T_OUT || s->type == VBLK_T_WRITE_ZEROES;

    if (status != VBLK_S_OK) {
        klog(KLOG_WARN, "virtio-blk", "%s: request type %u failed: status %d", vb->name,
             (unsigned)s->type, status);
    }
    if (!r) {
        s->status = status;
        s->done = 1;
        return;
    }
    if (rc == 0 && vb->cache &&
        (s->type == VBLK_T_DISCARD || (wrote && (r->flags & BLOCK_REQ_NOFLUSH)))) {
        vblk_unflushed(vb);
    } else if (rc == 0 && wrote && vb->cache) {
        if (vblk_flush_half(q, s) >= 0) {
            return; /* answered with the flush, or offered by vblk_start_next */
        }
        vblk_unflushed(vb);
        rc = -EIO;
    } else if (s->type == VBLK_T_FLUSH) {
        if (rc == 0) {
            vblk_flushed(vb, s->fseq);
        } else {
            vblk_unflushed(vb);
            rc = -EIO; /* UNSUPP too: what the cache holds is on no medium */
        }
    }
    vblk_end(s, rc);
}

/**
 * The virtqueue's `done`: the device has returned the chain of slot
 * `token`, with `len` bytes written. Runs from the queue's interrupt, or
 * from a polled caller's reap, with the virtqueue's lock released; takes
 * `q`'s lock, answers the slot and offers what waits.
 */
static void vblk_vq_done(struct virtqueue *vq, void *token, uint32_t len)
{
    struct vblk_slot *s = (struct vblk_slot *)token;
    struct vblk_queue *q = s->q;
    uint32_t flags;
    int status;

    (void)vq;
    (void)len;
    flags = spin_lock_irqsave(&q->lock);
    if (s->state != VBLK_ISSUED) {
        spin_unlock_irqrestore(&q->lock, flags);
        klog(KLOG_WARN, "virtio-blk", "%s: a buffer returned for no command", q->vb->name);
        return;
    }
    status = s->mem[VBLK_STATUS];
    vblk_answer(q, s, status);
    vblk_start_next(q);
    spin_unlock_irqrestore(&q->lock, flags);
}

/** Reap `q`'s virtqueue from a polled caller. */
static void vblk_reap(struct vblk_queue *q)
{
    (void)virtqueue_reap(q->vq);
}

/*
 * Polled commands, for the boot and for a caller that cannot sleep.
 */

/**
 * Count a caller into the device, a polled command or a reader of its
 * configuration, so that a removal waits for it before it lets the
 * transport go, and a shutdown before it resets the device; 0, or -EIO for
 * a disk out of use or shutting down. From any context.
 */
static int vblk_poller_in(struct vblk *vb)
{
    uint32_t flags = spin_lock_irqsave(&vb->lock);
    int rc = vb->dead || vb->closed ? -EIO : 0;

    if (rc == 0) {
        vb->pollers++;
    }
    spin_unlock_irqrestore(&vb->lock, flags);
    return rc;
}

/** Count a caller out. */
static void vblk_poller_out(struct vblk *vb)
{
    uint32_t flags = spin_lock_irqsave(&vb->lock);

    vb->pollers--;
    spin_unlock_irqrestore(&vb->lock, flags);
}

/**
 * End the polled command in slot `s` once done: copy GET_ID's answer for
 * `c`, free the slot, offer what waits, and set `*rc`. 1 when it was done,
 * 0 when it is still with the device. Takes and gives back `q`'s lock.
 */
static int vblk_polled_end(struct vblk_queue *q, struct vblk_slot *s, const struct vblk_cmd *c,
                           int *rc)
{
    uint32_t flags = spin_lock_irqsave(&q->lock);
    uint32_t i;

    block_count_poll();
    if (!s->done) {
        spin_unlock_irqrestore(&q->lock, flags);
        return 0;
    }
    *rc = s->status == VBLK_S_OK ? 0 : s->status == VBLK_S_UNSUPP ? -EOPNOTSUPP : -EIO;
    if (*rc == 0 && c->id) {
        for (i = 0; i < VBLK_ID_BYTES; i++) {
            c->id[i] = s->mem[VBLK_ID + i];
        }
    }
    vblk_slot_free(s);
    vblk_start_next(q);
    spin_unlock_irqrestore(&q->lock, flags);
    return 1;
}

/**
 * Issue `c` on this processor's queue and poll for its answer: 0 when the
 * device answered OK, -EOPNOTSUPP when it answered UNSUPP, -EIO when it
 * answered an error, did not answer within the wait, or the disk is out of
 * use, and -EPERM or -E2BIG when the command could not be built
 * (`vblk_offer`).
 *
 * The answer may be taken by this loop's reap or by the queue's interrupt
 * on any processor, which marks the slot done either way under `q`'s lock;
 * the slot stays this caller's until it frees it. The wait neither sleeps
 * nor allocates, so it serves a page fault with interrupts off. A command
 * not answered within the bound, or a queue with no room for that long,
 * takes the disk out of use (`vblk_kill`): Linux's virtio_blk has no
 * timeout and waits for ever, which a caller with interrupts off cannot.
 * The caller is counted in the device (`vblk_poller_in`).
 */
static int vblk_polled_counted(struct vblk *vb, const struct vblk_cmd *c)
{
    struct vblk_queue *q = vblk_queue_here(vb);
    struct vblk_slot *s = 0;
    uint32_t steps = vblk_cmd_steps(), flags;
    int rc;

    for (;;) {
        flags = spin_lock_irqsave(&q->lock);
        if (vb->dead) {
            spin_unlock_irqrestore(&q->lock, flags);
            return -EIO;
        }
        s = vblk_slot_take(q);
        if (s) {
            rc = vblk_offer(q, s, c);
            if (rc == 0) {
                if (c->type == VBLK_T_FLUSH) {
                    block_count_flush();
                }
                block_count_command();
                virtqueue_kick(q->vq);
                spin_unlock_irqrestore(&q->lock, flags);
                break;
            }
            vblk_slot_free(s);
            if (rc < 0) {
                spin_unlock_irqrestore(&q->lock, flags);
                return rc;
            }
        }
        spin_unlock_irqrestore(&q->lock, flags);
        vblk_reap(q);
        if (--steps == 0) {
            vblk_kill(vb, "its queue had no room for thirty seconds");
            return -EIO;
        }
        vblk_poll_step();
    }
    for (;;) {
        vblk_reap(q);
        if (vblk_polled_end(q, s, c, &rc)) {
            return rc;
        }
        if (steps-- == 0) {
            break;
        }
        vblk_poll_step();
    }
    klog(KLOG_WARN, "virtio-blk", "%s: request type %u failed: %s", vb->name, (unsigned)c->type,
         kirq_enabled() ? "no answer in thirty seconds"
                        : "no answer in five seconds with interrupts off");
    vblk_kill(vb, "a command was not answered");
    if (!vblk_polled_end(q, s, c, &rc)) {
        rc = -EIO; /* not reached: the kill marks every polled slot done */
    }
    return rc;
}

/** `vblk_polled_counted` for a caller not yet counted in the device: -EIO
 * at once for a disk out of use or shutting down. */
static int vblk_polled(struct vblk *vb, const struct vblk_cmd *c)
{
    int rc = vblk_poller_in(vb);

    if (rc == 0) {
        rc = vblk_polled_counted(vb, c);
        vblk_poller_out(vb);
    }
    return rc;
}

/** Flush the disk's cache by polling, clearing the unflushed mark as a
 * queued flush clears it. The count of unflushed writes is read before the
 * flush is issued, so the flush covers at least those. `counted` says the
 * caller is counted in the device already, as the shutdown is. */
static int vblk_flush_polled(struct vblk *vb, int counted)
{
    struct vblk_cmd c;
    uint32_t fseq = vblk_wseq(vb);
    int rc;

    k_memset(&c, 0, sizeof(c));
    c.type = VBLK_T_FLUSH;
    rc = counted ? vblk_polled_counted(vb, &c) : vblk_polled(vb, &c);
    if (rc == 0) {
        vblk_flushed(vb, fseq);
    }
    return rc != 0 ? -EIO : 0;
}

static void vblk_config_work(struct vblk *vb);

/**
 * A polled transfer of `count` sectors at `lba`, from `src` for a write or
 * into `dst` for a read, in commands of at most the disk's `max_sectors`,
 * halved down to a sector when a buffer's pages need more pieces than a
 * chain carries. A write on a disk with a cache is followed by a flush, so
 * it is on the medium when it returns; when the flush fails, or the write
 * fails after a part of it reached the cache, the write fails with EIO and
 * the disk is marked unflushed. A device whose transport gives it no
 * interrupt has its capacity read first (`config_poll`).
 */
static int vblk_rw(block_device_t *device, uint64_t lba, uint32_t count, const void *src, void *dst)
{
    struct vblk *vb = (struct vblk *)device->drvdata;
    const uint8_t *at = dst ? (const uint8_t *)dst : (const uint8_t *)src;
    uint32_t most = device->max_sectors;
    struct vblk_cmd c;
    int rc, cached = 0;

    if (vb->config_poll) {
        vblk_config_work(vb);
    }
    if (count == 0 || lba > device->sector_count || count > device->sector_count - lba) {
        return -EIO;
    }
    if (src && vb->ro) {
        return -EIO;
    }
    k_memset(&c, 0, sizeof(c));
    c.type = dst ? VBLK_T_IN : VBLK_T_OUT;
    while (count > 0) {
        c.sector = lba;
        c.count = count > most ? most : count;
        c.buf = at;
        rc = vblk_polled(vb, &c);
        if (rc == -E2BIG && c.count > 1) {
            most = c.count / 2; /* a buffer of more pieces than its pages */
            continue;
        }
        if (rc != 0) {
            if (cached) {
                vblk_unflushed(vb);
            }
            return rc == -EOPNOTSUPP ? rc : -EIO;
        }
        cached = src && vb->cache;
        lba += c.count;
        at += c.count * BLOCK_SECTOR_SIZE;
        count -= c.count;
    }
    if (cached && vblk_flush_polled(vb, 0) != 0) {
        vblk_unflushed(vb);
        return -EIO;
    }
    return 0;
}

/** The device's read, polled. */
static int vblk_read(block_device_t *device, uint64_t lba, uint32_t count, void *buffer)
{
    return vblk_rw(device, lba, count, 0, buffer);
}

/** The device's write, polled and on the medium when it returns. */
static int vblk_write(block_device_t *device, uint64_t lba, uint32_t count, const void *buffer)
{
    return vblk_rw(device, lba, count, buffer, 0);
}

/*
 * Configuration changes.
 */

/**
 * Give the disk the capacity `sectors`, read from the configuration, when
 * it differs from the one it has: the device's count and the whole-disk
 * window's, which `/dev/diskN` and the size ioctls read, as Linux's
 * virtblk_update_capacity gives it to the disk with
 * set_capacity_and_notify (drivers/block/virtio_blk.c). The partition views
 * keep their ranges, as Linux's partitions do until their table is read
 * again. A disk that grows gets its count before its window and one that
 * shrinks after it, so that the window never passes the disk. Returns the
 * capacity it had. `vb`'s lock is held.
 */
static uint64_t vblk_capacity_set(struct vblk *vb, uint64_t sectors)
{
    uint64_t was = vb->bdev.sector_count;
    struct disk *d = vb->disk;

    int window = d && d->whole.present;

    if (sectors > was) {
        vb->bdev.sector_count = sectors;
        if (window) {
            d->whole.part.device.sector_count = sectors;
        }
    } else if (sectors < was) {
        if (window) {
            d->whole.part.device.sector_count = sectors;
        }
        vb->bdev.sector_count = sectors;
    }
    return was;
}

/**
 * Read the capacity again after the configuration may have changed, as
 * Linux's virtblk_config_changed_work does from its work item, apart from
 * any request (drivers/block/virtio_blk.c). The reader is counted in the
 * device for the read (`vblk_poller_in`), so a removal waits for it before
 * the transport goes, and a disk out of use or shutting down is not read.
 * On a transport whose configuration is register reads (`config_atomic`)
 * the read is under `vb`'s lock, so that two readers give the disk their
 * capacities in the order they read them; any other transport's read is a
 * process's, which the big lock orders. From the configuration's interrupt
 * or a polled transfer on such a transport, and from a process otherwise.
 */
static void vblk_config_work(struct vblk *vb)
{
    uint8_t cap[8];
    uint64_t now, was;
    uint32_t flags;
    int atomic;

    if (vblk_poller_in(vb) != 0) {
        return;
    }
    atomic = vb->dev->config_atomic;
    if (atomic) {
        flags = spin_lock_irqsave(&vb->lock);
        virtio_config_read(vb->dev, VBLK_CFG_CAPACITY, cap, sizeof(cap));
    } else {
        virtio_config_read(vb->dev, VBLK_CFG_CAPACITY, cap, sizeof(cap));
        flags = spin_lock_irqsave(&vb->lock);
    }
    now = vblk_le(cap, 8);
    was = vblk_capacity_set(vb, now);
    spin_unlock_irqrestore(&vb->lock, flags);
    if (now != was) {
        klog(KLOG_INFO, "virtio-blk", "%s: capacity: %u MiB, was %u MiB", vb->name,
             (unsigned)(now >> 11), (unsigned)(was >> 11));
    }
    vblk_poller_out(vb);
}

/**
 * The device's `config_changed`, from its interrupt: read the capacity
 * where the transport lets a handler read the configuration, and otherwise
 * leave the read to the next process that enters the kernel
 * (`virtio_blk_service`).
 */
static void vblk_config_changed(struct virtio_dev *d)
{
    struct vblk *vb = (struct vblk *)d->priv;

    if (!vb) {
        return;
    }
    if (d->config_atomic) {
        vblk_config_work(vb);
    } else {
        vb->config_dirty = 1;
        vblk_waiting = 1;
    }
}

/*
 * The interrupt-driven path.
 */

/**
 * The device's `submit`: queue `r` on this processor's queue and offer
 * what the queue has room for, under the queue's lock, as Linux's
 * virtio_queue_rq adds the request under its queue's lock
 * (drivers/block/virtio_blk.c:456-482). Returns 0, or -EIO for a range
 * past the disk, a write to a read-only disk or a disk out of use or
 * shutting down. It reaches the transport only under the queue's lock with
 * the disk not dead, which a removal's `vblk_kill` waits for.
 */
static int vblk_submit(block_device_t *device, struct block_request *r)
{
    struct vblk *vb = (struct vblk *)device->drvdata;
    struct vblk_queue *q;
    uint32_t flags;

    if (!(r->flags & BLOCK_REQ_FLUSH) && (r->count == 0 || r->lba > device->sector_count ||
                                          r->count > device->sector_count - r->lba)) {
        return -EIO;
    }
    if (vb->dead || vb->closed ||
        (vb->ro && (r->write || (r->flags & (BLOCK_REQ_DISCARD | BLOCK_REQ_ZEROES))))) {
        return -EIO;
    }
    r->queued = 0;
    q = vblk_queue_here(vb);
    flags = spin_lock_irqsave(&q->lock);
    if (vb->closed) {
        spin_unlock_irqrestore(&q->lock, flags);
        return -EIO;
    }
    if (q->tail) {
        q->tail->queued = r;
    } else {
        q->head = r;
    }
    q->tail = r;
    vblk_start_next(q);
    spin_unlock_irqrestore(&q->lock, flags);
    return 0;
}

/** The device's `flush`: `r` queued as a flush request, or, with `r` NULL,
 * a flush by polling for a caller that cannot sleep. */
static int vblk_flush_submit(block_device_t *device, struct block_request *r)
{
    if (!r) {
        return vblk_flush_polled((struct vblk *)device->drvdata, 0);
    }
    r->flags |= BLOCK_REQ_FLUSH;
    return vblk_submit(device, r);
}

/*
 * Failure and removal.
 */

/** Mark `vb` out of use; 1 when this call did, 0 when it already was. */
static int vblk_mark_dead(struct vblk *vb)
{
    uint32_t flags = spin_lock_irqsave(&vb->lock);
    int was = vb->dead;

    vb->dead = 1;
    spin_unlock_irqrestore(&vb->lock, flags);
    return !was;
}

/**
 * Stop the device of `vb`, which is marked out of use, from any context
 * and without freeing anything: reset it, or stop it from reaching memory
 * when the reset does not end (`virtio_halt`), so that it writes into no
 * buffer it was given again; then answer every request in a slot or
 * waiting with EIO and mark every polled command done, whose caller frees
 * its slot. Returns `virtio_halt`'s result, and counts in `*lost`, when it
 * is not NULL, the commands the device had not answered.
 */
static int vblk_halt(struct vblk *vb, int *lost)
{
    uint32_t flags;
    int i, k, n = 0, rc = virtio_halt(vb->dev);

    for (i = 0; i < vb->nqueues; i++) {
        struct vblk_queue *q = &vb->q[i];
        struct block_request *r;

        flags = spin_lock_irqsave(&q->lock);
        for (k = 0; k < VBLK_SLOTS; k++) {
            struct vblk_slot *s = &q->slots[k];

            if (s->state == VBLK_FREE || s->done) {
                continue;
            }
            n++;
            if (s->request) {
                vblk_end(s, -EIO);
            } else {
                s->status = VBLK_S_RESET;
                s->done = 1;
            }
        }
        for (r = q->head; r; r = r->queued) {
            n++;
        }
        vblk_start_next(q);
        spin_unlock_irqrestore(&q->lock, flags);
    }
    if (lost) {
        *lost = n;
    }
    return rc;
}

/**
 * Take `vb` out of use, from any context: every later command answers
 * EIO, and the device is stopped and its commands answered as `vblk_halt`
 * says. Linux's virtio_blk resets the device the same way before it fails
 * its requests at removal (virtblk_remove, virtio_blk.c:1606-1630).
 */
static void vblk_kill(struct vblk *vb, const char *why)
{
    if (vblk_mark_dead(vb)) {
        klog(KLOG_ERR, "virtio-blk", "%s: out of use: %s", vb->name, why);
        (void)vblk_halt(vb, 0);
    }
}

/**
 * Whether anything still holds `vb`'s disk: a node open, a request the
 * device may answer, or a mount, a loop device or a held or dirty block of
 * the buffer cache on one of its views, as `usbdisk_holds` counts a stick's.
 */
static int vblk_disk_held(struct vblk *vb)
{
    struct disk *d = vb->disk;
    int k, held;

    if (!d) {
        return 0;
    }
    held = d->opens + block_device_holds(&vb->bdev);
    for (k = -1; k < d->table.count; k++) {
        block_device_t *view = k < 0 ? &d->whole.part.device : &d->views[k].part.device;

        held += vfs_device_holds(view) + bcache_device_holds(view) + block_device_holds(view);
    }
    return held;
}

/**
 * Let go of the disk of `vb`, whose device has gone, once nothing holds
 * it: its number is free again and the record serves a new device. 1 when
 * the record is free, 0 while something holds the disk. In process
 * context, under the big lock.
 */
static int vblk_reclaim(struct vblk *vb)
{
    struct disk *d = vb->disk;
    uint32_t flags;

    if (d && vblk_disk_held(vb)) {
        return 0;
    }
    if (d) {
        klog(KLOG_INFO, "virtio-blk", "disk%d: released", d->number);
        disk_release(d);
    }
    flags = spin_lock_irqsave(&vb->lock);
    vb->disk = 0;
    spin_unlock_irqrestore(&vb->lock, flags);
    vb->used = 0;
    return 1;
}

/** Give back the pages of `vb`'s queues, once the device no longer reads
 * them. */
static void vblk_free_pages(struct vblk *vb)
{
    int i;

    for (i = 0; i < VIRTIO_MAX_QUEUES; i++) {
        virtio_dma_free(vb->dev, (void *)(uintptr_t)vb->q[i].page, VBLK_PAGE);
        vb->q[i].page = 0;
        vb->q[i].vq = 0;
    }
}

/**
 * The transport is letting go of the device: unbound from virtio-pci, as
 * Linux's virtblk_remove runs (virtio_blk.c:1606-1630). The disk goes out
 * of use, its nodes are hidden, and the loop devices and the buffer cache
 * are told each of its views has gone, as when a stick is pulled; once no
 * polled caller and no reader of the configuration is inside the device,
 * as Linux's removal first ends its configuration work, the queues are
 * freed and the device given back. The disk's number is freed now when
 * nothing holds it; otherwise the disk stays, answering EIO, until its
 * last holder lets it go (`virtio_blk_service`). In process context, under
 * the big lock.
 */
static void vblk_removed(struct virtio_dev *d)
{
    struct vblk *vb = (struct vblk *)d->priv;
    uint32_t flags, pollers;
    int k;

    if (!vb) {
        return;
    }
    vblk_kill(vb, "the device was removed");
    if (vb->disk) {
        vb->disk->present = 0;
        for (k = -1; k < vb->disk->table.count; k++) {
            block_device_t *view =
                k < 0 ? &vb->disk->whole.part.device : &vb->disk->views[k].part.device;

            loop_device_gone(view);
            bcache_device_gone(view);
        }
    }
    do {
        flags = spin_lock_irqsave(&vb->lock);
        pollers = vb->pollers;
        spin_unlock_irqrestore(&vb->lock, flags);
    } while (pollers != 0);
    virtio_teardown(d);
    vblk_free_pages(vb);
    virtio_driver_unbind(d);
    d->priv = 0;
    d->removed = 0;
    d->config_changed = 0;
    flags = spin_lock_irqsave(&vb->lock);
    vb->dev = 0;
    vb->config_dirty = 0;
    vb->gone = 1;
    vb->up = 0;
    spin_unlock_irqrestore(&vb->lock, flags);
    if (!vblk_reclaim(vb)) {
        klog(KLOG_INFO, "virtio-blk", "disk%d: held: released when its last holder lets go",
             vb->disk->number);
        vblk_waiting = 1;
    }
    virtio_put(d);
}

void virtio_blk_service(void)
{
    int i;

    if (!vblk_waiting) {
        return;
    }
    vblk_waiting = 0;
    for (i = 0; i < VBLK_DISKS; i++) {
        struct vblk *vb = &vblks[i];

        if (!vb->used) {
            continue;
        }
        if (vb->gone) {
            if (!vblk_reclaim(vb)) {
                vblk_waiting = 1;
            }
        } else if (vb->config_dirty) {
            vb->config_dirty = 0;
            vblk_config_work(vb);
        }
    }
}

/**
 * Stop `vb`'s device for the machine's stop. New commands are refused, the
 * commands the device has and the requests that wait are finished, a write
 * with its flush, a cache still marked unflushed is flushed, and the
 * device is reset (`vblk_halt`), with a record of what the reset found.
 * The wait for the device is bounded as a polled command's is, so that a
 * device that never answers cannot keep the machine up.
 */
static void vblk_shutdown_one(struct vblk *vb)
{
    uint32_t steps = vblk_cmd_steps(), flags;
    int i, k, busy, lost = 0, rc;

    flags = spin_lock_irqsave(&vb->lock);
    if (vb->dead || vb->closed) {
        spin_unlock_irqrestore(&vb->lock, flags);
        return;
    }
    vb->closed = 1;
    vb->pollers++; /* this caller, for the flush below */
    spin_unlock_irqrestore(&vb->lock, flags);
    do {
        busy = 0;
        for (i = 0; i < vb->nqueues; i++) {
            struct vblk_queue *q = &vb->q[i];

            vblk_reap(q);
            flags = spin_lock_irqsave(&q->lock);
            busy += q->head != 0;
            for (k = 0; k < VBLK_SLOTS; k++) {
                busy += q->slots[k].state != VBLK_FREE;
            }
            spin_unlock_irqrestore(&q->lock, flags);
        }
        flags = spin_lock_irqsave(&vb->lock);
        busy += vb->pollers != 1;
        spin_unlock_irqrestore(&vb->lock, flags);
        if (busy) {
            vblk_poll_step();
        }
    } while (busy && !vb->dead && --steps != 0);
    if (!busy && vb->cache && vb->bdev.unflushed && vblk_flush_polled(vb, 1) != 0) {
        klog(KLOG_WARN, "virtio-blk", "%s: shutdown: the last flush failed", vb->name);
    }
    vblk_poller_out(vb);
    if (!vblk_mark_dead(vb)) {
        return; /* a command went unanswered, and the disk was reset for it */
    }
    rc = vblk_halt(vb, &lost);
    if (rc == 0 && lost == 0) {
        klog(KLOG_INFO, "virtio-blk", "%s: shutdown: reset with no request in flight", vb->name);
    } else {
        klog(KLOG_WARN, "virtio-blk", "%s: shutdown: %s, %d requests unanswered", vb->name,
             rc == 0 ? "reset" : "the reset did not end", lost);
    }
}

void virtio_blk_shutdown(void)
{
    int i;

    for (i = 0; i < VBLK_DISKS; i++) {
        struct vblk *vb = &vblks[i];

        if (vb->used && vb->up && !vb->gone) {
            vblk_shutdown_one(vb);
        }
    }
}

/*
 * Bring-up.
 */

/** The queues to ask `d` for: one for each processor up to those the
 * device offers with VIRTIO_BLK_F_MQ, and VIRTIO_MAX_QUEUES at most, as
 * Linux's init_vq takes the lesser of `num_queues` and its processors. */
static uint16_t vblk_queue_count(struct virtio_dev *d)
{
    uint16_t nq = 1;

    if (virtio_offered(d) & VIRTIO_BIT(VBLK_F_MQ)) {
        uint8_t n[2];

        virtio_config_read(d, VBLK_CFG_NUM_QUEUES, n, 2);
        nq = (uint16_t)vblk_le(n, 2);
    }
    if (nq == 0) {
        nq = 1;
    }
    if (nq > VIRTIO_MAX_QUEUES) {
        nq = VIRTIO_MAX_QUEUES;
    }
    return nq > NR_CPUS ? (uint16_t)NR_CPUS : nq;
}

/**
 * Whether `d`'s write cache is on, as Linux's virtblk_get_cache_mode reads
 * it (drivers/block/virtio_blk.c): the configuration's `writeback` byte,
 * `writeback` here, when VIRTIO_BLK_F_CONFIG_WCE was taken, and otherwise
 * on exactly when the device has FLUSH. A cache that is on in a device
 * without FLUSH could never be emptied, so the device is switched to
 * writethrough through that byte, which CONFIG_WCE lets the driver write
 * (virtio 1.2, 5.2.6.2), and the byte is read back. A device that keeps
 * its cache on all the same is driven with the cache on: the FLUSH each
 * durable write needs is sent as Linux sends it, the device answers
 * UNSUPP, and the write fails with EIO instead of being reported durable.
 */
static int vblk_cache_mode(struct virtio_dev *d, uint8_t writeback)
{
    uint8_t off = 0;

    if (!virtio_has(d, VBLK_F_CONFIG_WCE)) {
        return virtio_has(d, VBLK_F_FLUSH);
    }
    if (writeback == 0 || virtio_has(d, VBLK_F_FLUSH)) {
        return writeback != 0;
    }
    virtio_config_write(d, VBLK_CFG_WRITEBACK, &off, 1);
    virtio_config_read(d, VBLK_CFG_WRITEBACK, &writeback, 1);
    if (writeback == 0) {
        klog(KLOG_INFO, "virtio-blk", "%s: cache: set to writethrough, since it has no flush",
             d->name);
        return 0;
    }
    klog(KLOG_WARN, "virtio-blk",
         "%s: cache: on with no flush, and it stays on; durable writes will fail", d->name);
    return 1;
}

/**
 * Take `vb`'s limits from its configuration `cfg`: refuse a block size
 * other than 512, which the block layer cannot count in, as NVMe refuses a
 * namespace of 4096-byte blocks; then whether it is read-only, whether its
 * write cache is on (`vblk_cache_mode`), and the pieces and bytes a chain
 * may carry: `size_max` bounds every piece, as Linux makes it the queue's
 * largest segment. 0, or -1.
 */
static int vblk_limits(struct vblk *vb, const uint8_t *cfg)
{
    struct virtio_dev *d = vb->dev;
    uint32_t blk = (uint32_t)vblk_le(cfg + VBLK_CFG_BLK_SIZE, 4);
    uint32_t seg_max = (uint32_t)vblk_le(cfg + VBLK_CFG_SEG_MAX, 4);
    uint32_t size_max = (uint32_t)vblk_le(cfg + VBLK_CFG_SIZE_MAX, 4);

    if (virtio_has(d, VBLK_F_BLK_SIZE) && blk != BLOCK_SECTOR_SIZE) {
        klog(KLOG_WARN, "virtio-blk",
             "%s: refused: blocks of %u bytes; the block layer counts in 512-byte sectors", d->name,
             (unsigned)blk);
        vblk_why = "a block size other than 512";
        return -1;
    }
    vb->ro = virtio_has(d, VBLK_F_RO);
    vb->cache = vblk_cache_mode(d, cfg[VBLK_CFG_WRITEBACK]);
    vb->seg_max = VBLK_SEGS;
    if (virtio_has(d, VBLK_F_SEG_MAX) && seg_max != 0 && seg_max < vb->seg_max) {
        vb->seg_max = seg_max;
    }
    vb->size_max = virtio_has(d, VBLK_F_SIZE_MAX) ? size_max : 0;
    vb->seg_bytes = VBLK_PAGE;
    if (vb->size_max != 0 && vb->size_max < vb->seg_bytes) {
        vb->seg_bytes = vb->size_max;
    }
    return 0;
}

/**
 * Set up `vb`'s `nqueues` queues: each its page of slots and its
 * virtqueue, whose chain may cut the pieces a command carries and whose
 * descriptors describe `size_max` bytes at most each (`max_seg`), so that
 * the core joins no two pieces past it. 0, or -1 with `vblk_why` set.
 */
static int vblk_queues(struct vblk *vb)
{
    int i, k;

    for (i = 0; i < vb->nqueues; i++) {
        struct vblk_queue *q = &vb->q[i];

        spin_lock_init(&q->lock, "vblk_queue");
        q->vb = vb;
        q->index = i;
        q->head = q->tail = 0;
        q->page = (volatile uint8_t *)virtio_dma_alloc(vb->dev, VBLK_PAGE, &q->page_addr);
        if (!q->page) {
            vblk_why = "no page for its commands";
            return -1;
        }
        for (k = 0; k < VBLK_SLOTS; k++) {
            q->slots[k].state = VBLK_FREE;
            q->slots[k].q = q;
            q->slots[k].mem = q->page + (uint32_t)k * VBLK_SLOT_BYTES;
            q->slots[k].addr = q->page_addr + (uint64_t)k * VBLK_SLOT_BYTES;
        }
        if (virtio_queue_setup(vb->dev, (uint16_t)i, vblk_vq_done, q, &q->vq) != 0) {
            vblk_why = "a queue was refused";
            return -1;
        }
        q->vq->max_seg = vb->size_max;
        if (q->vq->max_chain >= 3 && (uint32_t)q->vq->max_chain - 2u < vb->seg_max) {
            vb->seg_max = (uint32_t)q->vq->max_chain - 2u;
        }
    }
    return 0;
}

/**
 * Make `vb`'s block device, polled, of the capacity in `cfg`: a request
 * of as many whole pages as a chain's pieces describe, at most the block
 * layer's 1 MiB, write-through when the cache is on, read-only when the
 * device is.
 */
static void vblk_disk_init(struct vblk *vb, const uint8_t *cfg)
{
    uint32_t per_page = (VBLK_PAGE + vb->seg_bytes - 1u) / vb->seg_bytes;
    uint32_t pages = vb->seg_max / per_page;

    k_memset(&vb->bdev, 0, sizeof(vb->bdev));
    vb->bdev.max_sectors = pages > 0 ? pages * (VBLK_PAGE / BLOCK_SECTOR_SIZE)
                                     : vb->seg_max * vb->seg_bytes / BLOCK_SECTOR_SIZE;
    if (vb->bdev.max_sectors > BLOCK_REQ_MAX_SECTORS) {
        vb->bdev.max_sectors = BLOCK_REQ_MAX_SECTORS;
    }
    if (vb->bdev.max_sectors == 0) {
        vb->bdev.max_sectors = 1;
    }
    vb->bdev.sector_size = BLOCK_SECTOR_SIZE;
    vb->bdev.sector_count = vblk_le(cfg + VBLK_CFG_CAPACITY, 8);
    vb->bdev.flags = BLOCK_FAULT_SAFE | BLOCK_POLLABLE | (vb->cache ? BLOCK_WRITE_THROUGH : 0u) |
                     (vb->ro ? BLOCK_READ_ONLY : 0u);
    vb->bdev.read = vblk_read;
    vb->bdev.write = vblk_write;
    vb->bdev.drvdata = vb;
    vb->wseq = 0;
}

/** Ask the device for its serial, VIRTIO_BLK_T_GET_ID's 20 bytes, as
 * Linux's serial attribute does; a device that answers UNSUPP has none. */
static void vblk_get_id(struct vblk *vb)
{
    struct vblk_cmd c;
    uint8_t id[VBLK_ID_BYTES];
    int k;

    k_memset(vb->serial, 0, sizeof(vb->serial));
    k_memset(&c, 0, sizeof(c));
    c.type = VBLK_T_GET_ID;
    c.id = id;
    if (vblk_polled(vb, &c) == 0) {
        for (k = 0; k < VBLK_ID_BYTES && id[k] != 0; k++) {
            vb->serial[k] = (char)id[k];
        }
    }
}

/**
 * Bring `vb`'s device up as Linux's virtblk_probe does
 * (virtio_blk.c:1385-1560): its features, with a queue for each processor
 * up to those it offers (`vblk_queue_count`); its limits and its cache from
 * the configuration (`vblk_limits`); its queues; DRIVER_OK; its disk,
 * polled; and its serial by GET_ID. Returns 0, or -1 with `vblk_why` set
 * and the device reset.
 */
static int vblk_bring_up(struct vblk *vb)
{
    struct virtio_dev *d = vb->dev;
    uint8_t cfg[VBLK_CFG_BYTES];

    k_strncpy(vb->name, d->name, sizeof(vb->name) - 1);
    vb->name[sizeof(vb->name) - 1] = '\0';
    vb->nqueues = vblk_queue_count(d);
    d->priv = vb;
    d->irq_unlocked = 1;
    d->indirect_len = VIRTQ_INDIRECT_MAX;
    d->indirect_tables = VBLK_TABLES;
    d->config_changed = vblk_config_changed;
    d->removed = vblk_removed;
    if (virtio_init(d, VBLK_WANTED, vb->nqueues) != 0) {
        vblk_why = "its features or its interrupts were refused";
        return -1;
    }
    vb->config_poll = d->polled && d->config_atomic;
    virtio_config_read(d, 0, cfg, sizeof(cfg));
    if (vblk_limits(vb, cfg) != 0 || vblk_queues(vb) != 0) {
        goto fail;
    }
    if (virtio_ready(d) != 0) {
        vblk_why = "the device did not take DRIVER_OK";
        goto fail;
    }
    vblk_disk_init(vb, cfg);
    vblk_get_id(vb);
    if (vb->dead) {
        vblk_why = "it did not answer GET_ID";
        goto fail;
    }
    klog(KLOG_INFO, "virtio-blk",
         "%s: %u MiB, %d queues, %u pieces of %u bytes, %u sectors a request, cache %s%s%s%s",
         d->name, (unsigned)(vb->bdev.sector_count >> 11), (int)vb->nqueues, (unsigned)vb->seg_max,
         (unsigned)vb->seg_bytes, (unsigned)vb->bdev.max_sectors, vb->cache ? "on" : "off",
         vb->ro ? ", read-only" : "", virtio_has(d, VBLK_F_DISCARD) ? ", discard" : "",
         virtio_has(d, VBLK_F_WRITE_ZEROES) ? ", write zeroes" : "");
    return 0;
fail:
    virtio_teardown(d);
    vblk_free_pages(vb);
    return -1;
}

/**
 * Register `vb`'s disk, as transport `virtio` at its place, with its
 * serial laid out as IDENTIFY words 10 to 19, first character in the high
 * byte, the firmware and the model words blank, as Linux's virtio_blk gives
 * a disk no model; the first as `block0` when no other driver has published
 * one.
 */
static void vblk_publish(struct vblk *vb)
{
    static uint16_t words[256];
    int i;

    for (i = 0; i < 256; i++) {
        words[i] = i >= 10 && i < 47 ? 0x2020u : 0u; /* ATA pads with spaces */
    }
    for (i = 0; i < 10; i++) {
        uint8_t hi = (uint8_t)vb->serial[2 * i], lo = (uint8_t)vb->serial[2 * i + 1];

        words[10 + i] = (uint16_t)(((uint32_t)(hi ? hi : ' ') << 8) | (lo ? lo : ' '));
    }
    if (!block0) {
        block0 = &vb->bdev;
    }
    vb->disk = disk_register(&vb->bdev, "virtio", vb->number, words);
    if (!vb->disk) {
        klog(KLOG_WARN, "virtio-blk", "%s: the registry is full", vb->name);
    }
}

/** A record for a new device: one never used, or one whose device has gone
 * and whose disk nothing holds, which is released here if it was not yet;
 * NULL when all eight are taken. */
static struct vblk *vblk_record(void)
{
    int i;

    for (i = 0; i < VBLK_DISKS; i++) {
        if (!vblks[i].used || (vblks[i].gone && vblk_reclaim(&vblks[i]))) {
            k_memset(&vblks[i], 0, sizeof(vblks[i]));
            spin_lock_init(&vblks[i].lock, "vblk_lock");
            vblks[i].number = i;
            vblks[i].used = 1;
            return &vblks[i];
        }
    }
    return 0;
}

/** Move `vb` to its queues' interrupts, or log why it keeps polling. */
static void vblk_start_one(struct vblk *vb)
{
    if (kcmdline_has("rach2.vblkpoll")) {
        klog(KLOG_INFO, "virtio-blk", "%s: polled: rach2.vblkpoll", vb->name);
        return;
    }
    if (vb->dev->polled) {
        klog(KLOG_INFO, "virtio-blk", "%s: polled: the transport gives it no interrupt", vb->name);
        return;
    }
    vb->bdev.flags &= ~BLOCK_FAULT_SAFE;
    vb->bdev.flags |= BLOCK_FRAMES;
    vb->bdev.submit = vblk_submit;
    vb->bdev.flush = vb->cache ? vblk_flush_submit : 0;
    if (virtio_has(vb->dev, VBLK_F_DISCARD)) {
        uint8_t v[4];

        virtio_config_read(vb->dev, VBLK_CFG_MAX_DISCARD, v, 4);
        vb->bdev.max_discard = vblk_le(v, 4) ? (uint32_t)vblk_le(v, 4) : 0xFFFFFFFFu;
    }
    if (virtio_has(vb->dev, VBLK_F_WRITE_ZEROES)) {
        uint8_t v[4];

        virtio_config_read(vb->dev, VBLK_CFG_MAX_ZEROES, v, 4);
        vb->bdev.max_zeroes = vblk_le(v, 4) ? (uint32_t)vblk_le(v, 4) : 0xFFFFFFFFu;
    }
    vb->started = 1;
    klog(KLOG_INFO, "virtio-blk", "%s: interrupts: %d queues", vb->name, (int)vb->nqueues);
}

/**
 * Take device `d`, which the transport took after the boot's probe, as a
 * write to virtio-pci's `bind` gives it: the driver's probe of
 * `virtio_driver_register`. The disk is brought up, registered and its
 * table read, goes to its interrupts at once, since the boot's
 * `virtio_blk_start` has run, and then has its whole-disk window
 * published, `/dev/diskN`, as the boot publishes every disk's after their
 * start. 0, or -errno with `d` given back.
 */
static int32_t vblk_probe(struct virtio_dev *d)
{
    struct vblk *vb = vblk_record();
    int rc;

    if (!vb) {
        klog(KLOG_WARN, "virtio-blk", "%s: a device past the %d this driver drives", d->name,
             VBLK_DISKS);
        virtio_put(d);
        return -ENOSPC;
    }
    vb->dev = d;
    rc = virtio_driver_bind(d, "virtio_blk");
    if (rc != 0 || vblk_bring_up(vb) != 0) {
        if (rc == 0) {
            virtio_driver_unbind(d);
            klog(KLOG_ERR, "virtio-blk", "%s: unusable: %s", d->name, vblk_why);
        }
        d->priv = 0;
        d->removed = 0;
        d->config_changed = 0;
        vb->used = 0;
        virtio_put(d);
        return rc != 0 ? rc : -EIO;
    }
    vb->up = 1;
    vblk_publish(vb);
    if (vb->disk) {
        (void)disk_scan(vb->disk, &vb->bdev);
    }
    vblk_start_one(vb);
    if (vb->disk) {
        devfs_disk_publish(vb->disk);
    }
    virtio_driver_bound(d);
    return 0;
}

int virtio_blk_init(void)
{
    struct virtio_dev *d;
    int i, n, published = 0;

    for (n = 0; virtio_pci_find(VIRTIO_ID_BLOCK, n, &d) == 0; n++) {
        struct vblk *vb = 0;

        for (i = 0; i < VBLK_DISKS; i++) {
            if (vblks[i].used && vblks[i].dev == d) {
                vb = &vblks[i];
            }
        }
        if (!vb) {
            vb = vblk_record();
            if (!vb) {
                klog(KLOG_WARN, "virtio-blk", "%s: a device past the %d this driver drives",
                     d->name, VBLK_DISKS);
                continue;
            }
            vb->dev = d;
            if (virtio_driver_bind(d, "virtio_blk") != 0 || vblk_bring_up(vb) != 0) {
                klog(KLOG_ERR, "virtio-blk", "%s: unusable: %s", d->name, vblk_why);
                virtio_driver_unbind(d);
                d->priv = 0;
                d->removed = 0;
                d->config_changed = 0;
                vb->used = 0;
                virtio_put(d);
                continue;
            }
            vb->up = 1;
            virtio_driver_bound(d);
        }
        if (vb->up && !vb->dead) {
            vblk_publish(vb);
            published += vb->disk != 0;
        }
    }
    if (!vblk_registered) {
        vblk_registered = virtio_driver_register(VIRTIO_ID_BLOCK, vblk_probe) == 0;
    }
    if (n == 0) {
        klog(KLOG_INFO, "virtio-blk", "none: no virtio block device");
    }
    return published > 0 ? 0 : -1;
}

void virtio_blk_start(void)
{
    int i;

    for (i = 0; i < VBLK_DISKS; i++) {
        struct vblk *vb = &vblks[i];

        if (vb->used && vb->up && !vb->dead && !vb->started) {
            vblk_start_one(vb);
        }
    }
}
