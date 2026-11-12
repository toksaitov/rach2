/*
 * virtio.h: virtio 1.x devices as a driver sees them, whatever carries them:
 * the status and feature bits, the split virtqueue, and the table of
 * operations by which a transport, virtio-pci first, reaches its device.
 */

#ifndef _VIRTIO_H_
#define _VIRTIO_H_

#include "../pci.h"
#include "../lib/kint.h"
#include "../lib/spinlock.h"

struct shmem;

/*
 * Every structure the device reads or writes is little-endian (virtio 1.2,
 * 1.4). Rach2's processors are little-endian, so the fields below are read
 * and written as they are.
 */

/* The device status byte (virtio 1.2, 2.1). */
#define VIRTIO_STATUS_ACKNOWLEDGE 0x01u
#define VIRTIO_STATUS_DRIVER      0x02u
#define VIRTIO_STATUS_DRIVER_OK   0x04u
#define VIRTIO_STATUS_FEATURES_OK 0x08u
#define VIRTIO_STATUS_NEEDS_RESET 0x40u
#define VIRTIO_STATUS_FAILED      0x80u

/* Feature bits that the ring and the transport own (virtio 1.2, 6); a
 * device's own bits are below 24. */
#define VIRTIO_F_INDIRECT_DESC   28
#define VIRTIO_F_EVENT_IDX       29
#define VIRTIO_F_VERSION_1       32
#define VIRTIO_F_ACCESS_PLATFORM 33
#define VIRTIO_F_RING_PACKED     34
#define VIRTIO_F_ORDER_PLATFORM  36
#define VIRTIO_BIT(n)            ((uint64_t)1 << (n))

/* Device types (virtio 1.2, 5), the ids a driver asks a transport for. */
#define VIRTIO_ID_NET     1
#define VIRTIO_ID_BLOCK   2
#define VIRTIO_ID_CONSOLE 3
#define VIRTIO_ID_RNG     4
#define VIRTIO_ID_BALLOON 5
#define VIRTIO_ID_SCSI    8
#define VIRTIO_ID_9P      9
#define VIRTIO_ID_GPU     16
#define VIRTIO_ID_INPUT   18
#define VIRTIO_ID_VSOCK   19
#define VIRTIO_ID_SOUND   25

/* The MSI-X entry number that means no vector (virtio 1.2, 4.1.5.1.2). */
#define VIRTIO_NO_VECTOR 0xFFFFu

/*
 * The split virtqueue (virtio 1.2, 2.7): a table of descriptors, the ring
 * the driver offers buffers on, and the ring the device returns them on.
 * The driver's ring is `flags`, `idx`, `ring[size]` and `used_event`, 16
 * bits each; the device's is `flags`, `idx`, `ring[size]` of the element
 * below, and `avail_event`.
 */
struct virtq_desc {
    uint64_t addr;  /* the buffer's address as the device reads it */
    uint32_t len;   /* its length in bytes */
    uint16_t flags; /* VIRTQ_DESC_F_* */
    uint16_t next;  /* the next descriptor of the chain, with F_NEXT */
};

#define VIRTQ_DESC_F_NEXT     1u
#define VIRTQ_DESC_F_WRITE    2u /* the device writes the buffer */
#define VIRTQ_DESC_F_INDIRECT 4u /* the buffer is a table of descriptors */

struct virtq_used_elem {
    uint32_t id;  /* the head of the chain the device has finished */
    uint32_t len; /* the bytes it wrote into the chain's buffers */
};

#define VIRTQ_AVAIL_F_NO_INTERRUPT 1u
#define VIRTQ_USED_F_NO_NOTIFY     1u

/*
 * The largest queue this driver makes. A queue of 128 has its descriptors
 * and both rings within one page, 3342 bytes, so that a transport whose
 * memory comes a page at a time gives it in one piece; a device that offers
 * more is given 128, which the transport writes as the queue's size.
 */
#define VIRTQ_MAX_SIZE 128

/*
 * Indirect tables (VIRTIO_F_INDIRECT_DESC): one page per queue, cut into
 * VIRTQ_INDIRECT_TABLES tables of VIRTQ_INDIRECT_LEN descriptors. A buffer
 * of more segments than the queue has free descriptors goes into one when
 * the device offered the feature and a table is free.
 */
#define VIRTQ_INDIRECT_TABLES 16
#define VIRTQ_INDIRECT_LEN    16

/* The queues a device may have set up at once. */
#define VIRTIO_MAX_QUEUES 8

struct virtio_dev;
struct virtqueue;

/**
 * Called once for each buffer the device has returned, with the token the
 * driver gave `virtqueue_add` and the bytes the device wrote. It runs from
 * `virtqueue_reap`, in an interrupt handler or in the caller that polls,
 * with the queue's lock released, so it may add buffers to its own queue.
 */
typedef void (*virtqueue_done_t)(struct virtqueue *vq, void *token, uint32_t len);

/* A segment of a buffer: its address as the device reads it, from
 * `virtio_dma_alloc`, and its length. */
struct virtio_sg {
    uint64_t addr;
    uint32_t len;
};

/*
 * A split virtqueue. The ring memory comes from the transport's
 * `ring_alloc`, or its `dma_alloc` without one, in one piece of
 * `ring_bytes`; the record is the core's.
 * `lock` guards the descriptors, the shadow fields and `token`, and is
 * taken with interrupts off, since `virtqueue_reap` runs in a handler.
 */
struct virtqueue {
    struct virtio_dev *dev;
    uint16_t index;  /* the queue's number on its device */
    uint16_t size;   /* descriptors, a power of two up to VIRTQ_MAX_SIZE */
    uint16_t vector; /* the transport's interrupt entry, or VIRTIO_NO_VECTOR */
    uint16_t pad;
    uintptr_t notify; /* where the transport rings this queue; its own */
    volatile struct virtq_desc *desc;
    volatile uint16_t *avail; /* flags, idx, ring[size], used_event */
    volatile uint16_t *used;  /* flags, idx, then the elements */
    volatile struct virtq_used_elem *used_ring;
    uint64_t desc_addr, avail_addr, used_addr; /* as the device reads them */
    void *ring_mem;
    uint32_t ring_bytes;
    volatile struct virtq_desc *indirect; /* the tables, or NULL */
    uint64_t indirect_addr;
    uint32_t indirect_free;         /* a bit for each free table */
    uint16_t max_chain;             /* descriptors one chain may hold; the transport's */
    uint32_t max_bytes;             /* bytes one chain may carry each way, 0 for any */
    uint16_t free_head;             /* the first free descriptor */
    uint16_t nfree;                 /* free descriptors */
    uint16_t avail_idx;             /* the driver's ring index as last published */
    uint16_t kicked_idx;            /* avail_idx at the last virtqueue_kick */
    int event;                      /* VIRTIO_F_EVENT_IDX was negotiated */
    uint16_t last_used;             /* the device's ring index as last reaped */
    void *token[VIRTQ_MAX_SIZE];    /* by head; NULL for a free head */
    uint32_t inlen[VIRTQ_MAX_SIZE]; /* bytes the device may write into a head's chain */
    uint16_t chain[VIRTQ_MAX_SIZE]; /* descriptors a head's chain holds */
    uint16_t next[VIRTQ_MAX_SIZE];  /* the free list's and chains' links: the
                                     * device's table is written, never read */
    int8_t table[VIRTQ_MAX_SIZE];   /* a head's indirect table, or -1 */
    virtqueue_done_t done;
    void *priv; /* the driver's */
    uint32_t added, completed;
    struct spinlock lock;
};

/* A run of a device's own memory, as a device that maps its buffers from
 * its pages reports one (`map_pages`): the first page's address as the
 * device addresses its memory, page-aligned, and the pages. */
struct virtio_run {
    uint64_t addr;
    uint64_t pages;
};

struct virtio_map;

/*
 * A transport's operations on its device. virtio-pci is the first
 * (virtio_pci.c); another, such as a transport over one shared region of
 * memory, is a file of its own that fills the same table, and a driver
 * reaches its device only through the calls below, never through the
 * transport. Unless a call says otherwise it runs in a driver's process
 * context and may not be called from a handler.
 *
 * get_status, set_status: the device status byte. A write of 0 resets.
 * get_features: the 64 feature bits the device offers; a transport with no
 *     device to offer them adds FAILED to the status.
 * set_features: the bits the driver accepts.
 * config_generation: a count the device changes whenever its configuration
 *     changes, read before and after a read of several fields.
 * config_read, config_write: `len` bytes at `offset` of the device-specific
 *     configuration, in accesses of the fields' own widths where `len` is
 *     1, 2 or 4, and bytewise otherwise.
 * queue_max: the most descriptors queue `index` takes, 0 when the device
 *     has no such queue.
 * queue_enable: give the device `vq`'s size, addresses and vector, which
 *     the transport sets in `vq->vector`, and enable it; 0 or -errno.
 *     `vq->notify` is the transport's to fill, and so are `vq->max_chain`
 *     and `vq->max_bytes` for a device that bounds a chain, which the core
 *     sets to the queue's size and 0 before the call.
 * notify: tell the device queue `vq` has new buffers. A handler may call it.
 * irq_setup: prepare interrupts for `nqueues` queues, numbered 0 up, before
 *     they are set up, with every message held; 0 or -errno.
 * irq_start: let the interrupts through, once the queues are set up.
 * irq_release: stop the interrupts and give back what irq_setup took.
 * dma_alloc: zeroed memory the device can read and write by one address,
 *     at most a page, or `d->dma_max` bytes where the transport sets it,
 *     page-aligned, with that address in `*addr`; NULL when none. A larger
 *     buffer is a list of pieces.
 * dma_free: give back what dma_alloc gave, with its `bytes`; where
 *     `d->dma_max` is set, any whole pages of it, each given back once.
 * shm_region: shared memory region `id` (virtio 1.2, 2.10) as the device
 *     exposes it, its physical address and length; 0, or -ENOENT.
 *
 * The rest are optional, for a transport whose device reaches only memory
 * the transport gives it, as one over a single shared region does; NULL
 * keeps what virtio-pci does.
 *
 * ring_alloc: the memory of queue `index`'s rings, as dma_alloc gives it;
 *     NULL takes it from dma_alloc. dma_free gives it back.
 * backing_fill: give the pages of `sh`, `bytes` long and none given yet,
 *     frames the device reaches, zeroed; 0, or -ENOMEM when the transport
 *     has no room. NULL: a driver's pages come from anywhere, as they are
 *     first asked for.
 * backing_release: give back what backing_fill took for `sh`, once the
 *     device no longer reads it and before the pages are freed; nothing for
 *     pages it did not give.
 * dma_addr: the address by which the device reaches the frame at physical
 *     `phys`, in `*addr`; 0, or -EFAULT for a frame it cannot reach. NULL:
 *     the physical address.
 * put: the driver has let go of `d` for good, after its removal (below);
 *     the transport may free its record.
 *
 * The last three are for a device whose buffers live in its own memory and
 * reach the driver as lists of its pages, not through a shared memory
 * region, as the shared region's VM gives them (VGS_TF_PAGES); a transport
 * without them leaves all three NULL, and `pages` clear.
 *
 * map_pages: hold the frames behind `n` runs of the device's memory, in
 *     order: 0 with the map in `*out`, or -EINVAL,
 *     -EFAULT for memory that cannot be shared, -ENODEV, -ENOMEM. When the
 *     transport must take the frames back before map_free, as when the
 *     device's memory changes or its VM ends, it calls `revoke(owner)`,
 *     which stops every new use of them and does not sleep; the frames
 *     stay held until map_free, so that a use begun before the revoke
 *     never reaches a frame reused since.
 * map_frame: the frame of page `i` of `m`, or 0 once it is taken back.
 * map_free: let go of `m` and its frames, without its `revoke`, once
 *     nothing uses them.
 */
struct virtio_ops {
    const char *name;
    uint8_t (*get_status)(struct virtio_dev *d);
    void (*set_status)(struct virtio_dev *d, uint8_t status);
    uint64_t (*get_features)(struct virtio_dev *d);
    void (*set_features)(struct virtio_dev *d, uint64_t features);
    uint32_t (*config_generation)(struct virtio_dev *d);
    void (*config_read)(struct virtio_dev *d, uint32_t offset, void *buf, uint32_t len);
    void (*config_write)(struct virtio_dev *d, uint32_t offset, const void *buf, uint32_t len);
    uint16_t (*queue_max)(struct virtio_dev *d, uint16_t index);
    int (*queue_enable)(struct virtio_dev *d, struct virtqueue *vq);
    void (*notify)(struct virtio_dev *d, struct virtqueue *vq);
    int (*irq_setup)(struct virtio_dev *d, uint16_t nqueues);
    void (*irq_start)(struct virtio_dev *d);
    void (*irq_release)(struct virtio_dev *d);
    void *(*dma_alloc)(struct virtio_dev *d, uint32_t bytes, uint64_t *addr);
    void (*dma_free)(struct virtio_dev *d, void *p, uint32_t bytes);
    int (*shm_region)(struct virtio_dev *d, uint8_t id, uint64_t *addr, uint64_t *len);
    void *(*ring_alloc)(struct virtio_dev *d, uint16_t index, uint32_t bytes, uint64_t *addr);
    int (*backing_fill)(struct virtio_dev *d, struct shmem *sh, uint64_t bytes);
    void (*backing_release)(struct virtio_dev *d, struct shmem *sh);
    int (*dma_addr)(struct virtio_dev *d, uintptr_t phys, uint64_t *addr);
    void (*put)(struct virtio_dev *d);
    int (*map_pages)(struct virtio_dev *d, const struct virtio_run *runs, uint32_t n,
                     void (*revoke)(void *owner), void *owner, struct virtio_map **out);
    uintptr_t (*map_frame)(struct virtio_map *m, uint64_t i);
    void (*map_free)(struct virtio_map *m);
};

/*
 * A device as a driver holds it. The transport fills `ops`, `transport`,
 * `id` and `name` when it finds the device; the core fills `features` and
 * `queues`; the driver sets `config_changed` and `priv`.
 */
struct virtio_dev {
    const struct virtio_ops *ops;
    void *transport;   /* the transport's record of the device */
    uint16_t id;       /* VIRTIO_ID_* */
    uint16_t nqueues;  /* the count given to virtio_init */
    uint64_t features; /* negotiated, valid once virtio_init returns 0 */
    const char *name;  /* for the log, as "virtio0" */
    /* Called on a configuration change, from the interrupt handler, as
     * Linux's config_changed is; NULL ignores it. */
    void (*config_changed)(struct virtio_dev *d);
    /* Called by a transport whose device can go away, in process context,
     * when it has gone for good: the driver stops using it, unplugs what it
     * made over it and later calls virtio_put. NULL ignores it. */
    void (*removed)(struct virtio_dev *d);
    void *priv; /* the driver's */
    /* Set by the driver before virtio_init when its queues' `done` and its
     * `config_changed` take only locks of their own: virtio-pci then runs
     * the device's MSI-X handlers without the big lock (ISR_UNLOCKED). A
     * shared INTx line and the other transports keep it. */
    int irq_unlocked;
    /* The most bytes one dma_alloc gives, in whole pages that follow each
     * other in the device's addresses; 0 for one page, as on virtio-pci. */
    uint32_t dma_max;
    /* The device maps its buffers from its own pages (`map_pages`), as
     * the transport and the device have agreed; set by the transport. */
    int pages;
    /* `notify` is one register write that takes no lock and wakes no
     * thread, so a driver may make it without the big lock; set by the
     * transport, as virtio-pci sets it. */
    int notify_unlocked;
    struct virtqueue *queues[VIRTIO_MAX_QUEUES];
    uint32_t config_events;
};

/*
 * The core (virtqueue.c), the same for every transport. A driver brings a
 * device up as virtio 1.2, 3.1.1 orders it: virtio_init, which resets the
 * device, negotiates its features and prepares its interrupts; then
 * virtio_queue_setup for each queue; then virtio_ready. virtio_teardown
 * undoes them all.
 */

/**
 * Reset the device and wait, a bounded time, until its status reads 0.
 *
 * @return 0, or -ETIMEDOUT when the status has not cleared after 100 ms;
 *         the device may then still be using what it was given.
 */
int virtio_reset(struct virtio_dev *d);

/**
 * Reset `d`, acknowledge it, and negotiate its features: the device's own
 * bits in `wanted` that it offers, with VIRTIO_F_VERSION_1, which it must
 * offer, and the ring and platform bits the core handles
 * (VIRTIO_F_INDIRECT_DESC, VIRTIO_F_ACCESS_PLATFORM,
 * VIRTIO_F_ORDER_PLATFORM). Then prepare interrupts for `nqueues` queues.
 *
 * @return 0 with `d->features` set; -ETIMEDOUT when the reset did not
 *         finish, -ENODEV for a device without VIRTIO_F_VERSION_1 or one
 *         the transport marked FAILED while offering its features, -EIO
 *         when it refused the features, -EINVAL for more than
 *         VIRTIO_MAX_QUEUES queues, or the transport's error from its
 *         interrupts. On an error the status has FAILED set where the
 *         device answers.
 */
int virtio_init(struct virtio_dev *d, uint64_t wanted, uint16_t nqueues);

/** Whether feature bit `bit` was negotiated. */
int virtio_has(const struct virtio_dev *d, unsigned bit);

/**
 * Set up queue `index`, below the `nqueues` given to virtio_init, with
 * `done` called for each returned buffer and `priv` kept for it.
 *
 * @return 0 with `*out` set; -ENOENT when the device has no such queue,
 *         -EBUSY when it is already set up, -ENOMEM when its memory cannot
 *         be had, or the transport's error.
 */
int virtio_queue_setup(struct virtio_dev *d, uint16_t index, virtqueue_done_t done, void *priv,
                       struct virtqueue **out);

/**
 * Let the interrupts through and set DRIVER_OK: the device is live.
 *
 * @return 0, or -EIO when the device answers with FAILED or NEEDS_RESET,
 *         as a transport whose device checks the driver's setup then does.
 */
int virtio_ready(struct virtio_dev *d);

/**
 * Reset `d`, stop its interrupts, and free its queues. Memory the device
 * was given is freed only when the reset finished; otherwise it is kept,
 * since a device that did not reset may still write into it. The device can
 * be brought up again with virtio_init.
 */
void virtio_teardown(struct virtio_dev *d);

/**
 * Read `len` bytes at `offset` of the device-specific configuration as one
 * consistent snapshot: the read is repeated while the configuration
 * generation changes across it, at most eight times.
 */
void virtio_config_read(struct virtio_dev *d, uint32_t offset, void *buf, uint32_t len);

/** Write `len` bytes at `offset` of the device-specific configuration. */
void virtio_config_write(struct virtio_dev *d, uint32_t offset, const void *buf, uint32_t len);

/** Memory for buffers the device reads or writes, through the transport;
 * see `dma_alloc` above. */
void *virtio_dma_alloc(struct virtio_dev *d, uint32_t bytes, uint64_t *addr);

/** Give back what virtio_dma_alloc gave. */
void virtio_dma_free(struct virtio_dev *d, void *p, uint32_t bytes);

/** Shared memory region `id`, through the transport; see `shm_region`. */
int virtio_shm_region(struct virtio_dev *d, uint8_t id, uint64_t *addr, uint64_t *len);

/** Give `sh`'s pages from memory the device reaches; see `backing_fill`.
 * 0 at once on a transport without the operation. */
int virtio_backing_fill(struct virtio_dev *d, struct shmem *sh, uint64_t bytes);

/** Give back what virtio_backing_fill took; see `backing_release`. */
void virtio_backing_release(struct virtio_dev *d, struct shmem *sh);

/** The device's address of the frame at `phys`; see `dma_addr`. */
int virtio_dma_addr(struct virtio_dev *d, uintptr_t phys, uint64_t *addr);

/** Hold the frames behind runs of the device's memory; see `map_pages`.
 * -ENODEV on a transport without it. Under the big lock; does not sleep. */
int virtio_map_pages(struct virtio_dev *d, const struct virtio_run *runs, uint32_t n,
                     void (*revoke)(void *owner), void *owner, struct virtio_map **out);

/** The frame of page `i` of a map; see `map_frame`. */
uintptr_t virtio_map_frame(struct virtio_dev *d, struct virtio_map *m, uint64_t i);

/** Let go of a map; see `map_free`. NULL is ignored. */
void virtio_map_free(struct virtio_dev *d, struct virtio_map *m);

/** The driver lets go of a removed device; see `put`. */
void virtio_put(struct virtio_dev *d);

/**
 * Whether a buffer of `out` segments the device reads and `in` it writes,
 * in `sg`, fits one chain of `vq`: no more descriptors than `max_chain` and
 * no more bytes each way than `max_bytes`, once adjacent segments of the
 * same way are one descriptor, as virtqueue_add makes them.
 */
int virtqueue_chain_fits(const struct virtqueue *vq, const struct virtio_sg *sg, unsigned out,
                         unsigned in);

/**
 * Offer a buffer of `out` segments the device reads followed by `in`
 * segments it writes, all in `sg`, with `token`, which `done` is given back.
 * The buffer is published at once; virtqueue_kick tells the device.
 * Segments of the same way that are adjacent in the device's addresses go
 * in one descriptor.
 *
 * A chain goes into the queue's descriptors when enough are free, and into
 * an indirect table when they are not, the feature was negotiated and a
 * table is free and long enough.
 *
 * @return 0; -EINVAL for no segments or a NULL token; -E2BIG for a buffer
 *         that does not fit one chain (virtqueue_chain_fits); -ENOSPC when
 *         it fits neither way now, and a completion will make room.
 */
int virtqueue_add(struct virtqueue *vq, const struct virtio_sg *sg, unsigned out, unsigned in,
                  void *token);

/** Tell the device about the buffers added since the last call, unless it
 * has asked not to be told: by VIRTQ_USED_F_NO_NOTIFY, or with
 * VIRTIO_F_EVENT_IDX by an `avail_event` past them. A handler may call it. */
void virtqueue_kick(struct virtqueue *vq);

/**
 * Take every buffer the device had returned when the call began, free its
 * descriptors and call `done` for each. An interrupt handler calls it, and
 * so may a driver that polls. A buffer returned meanwhile, perhaps one that
 * a `done` added, raises an interrupt of its own, with VIRTIO_F_EVENT_IDX
 * through the `used_event` the call writes first, and is left to the next
 * call, so that a device that keeps answering cannot hold the caller, and
 * the big lock, until a whole backlog is drained. Returns the count taken.
 */
int virtqueue_reap(struct virtqueue *vq);

/** Free descriptors in `vq` now. */
uint16_t virtqueue_free_count(struct virtqueue *vq);

/*
 * The transports' entries into the core, from their interrupt handlers.
 */

/** Reap queue `vq`; for an interrupt that names the queue. */
void virtio_interrupt_queue(struct virtqueue *vq);

/** Reap every queue of `d`; for an interrupt shared by them. */
void virtio_interrupt_queues(struct virtio_dev *d);

/** Tell the driver its configuration changed. */
void virtio_interrupt_config(struct virtio_dev *d);

/*
 * virtio-pci (virtio_pci.c).
 */

/**
 * Find the `nth` virtio-pci function of device type `id`, counting from
 * zero, read its capabilities, map them and log them, as
 * `virtio: found: 00:02.0 gpu: common bar 4+0x0 ...`. A second call for the
 * same function returns the same record.
 *
 * @return 0 with `*out` set; -ENODEV when there is no such function, -EIO
 *         when it lacks a capability virtio 1.x requires or one cannot be
 *         mapped, -ENOMEM when every record is taken.
 */
int virtio_pci_find(uint16_t id, int nth, struct virtio_dev **out);

/** The PCI function of `d`, for a driver that needs its class or BARs, or
 * NULL for a device of another transport. */
const pci_device_t *virtio_pci_function(const struct virtio_dev *d);

/*
 * The shared-memory transport (shmem.c).
 */

/** Register /dev/vgpu-shm, through which ivshmemd(8) attaches a region
 * shared with a VM whose device serves virtio-gpu (vgpu-shm(4)); 0 or the
 * registration's error. Called once from drm_start. */
int32_t vgpu_shm_start(void);

#endif /* _VIRTIO_H_ */
