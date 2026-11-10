/*
 * vgpushm.h: the layout of the shared-memory transport that carries
 * virtio-gpu between Rach2's driver and the device backend in a Linux VM.
 *
 * One region of shared memory holds a control area, the split virtqueues and
 * a heap from which the driver allocates its commands and every resource's
 * backing; an offset into the region is the DMA address the device sees.
 * bed/shm/Readme.md states the rules this header's checks enforce.
 *
 * The header is C89 for the kernel and C99 for Linux. The includer supplies
 * uint8_t to uint64_t (kint.h in the kernel, <stdint.h> elsewhere) and may
 * define VGS_MB() as a full memory barrier before including it. Exactly one
 * file of a program defines VGS_IMPLEMENTATION before including it, which
 * compiles the functions; every other file sees their declarations.
 */
#ifndef VGPUSHM_H
#define VGPUSHM_H

/* "VGSH" as little-endian bytes; the region's first word once laid out. */
#define VGS_MAGIC         0x48534756u
#define VGS_VERSION_MAJOR 1
#define VGS_VERSION_MINOR 0

#define VGS_PAGE             4096u
#define VGS_CTRL_SIZE        4096u
/* A region is a power of two from 32 KiB to 64 GiB. */
#define VGS_REGION_MIN       32768u
#define VGS_REGION_MAX_SHIFT 36

/* Offsets of the three blocks of the control area; each has one writer. */
#define VGS_LAYOUT_OFF 0u
#define VGS_DRIVER_OFF 256u
#define VGS_DEVICE_OFF 1024u

/* The virtio-gpu queues, by index, and the largest size either may have. */
#define VGS_QUEUES        2
#define VGS_QUEUE_CONTROL 0
#define VGS_QUEUE_CURSOR  1
#define VGS_QUEUE_MAX     256u

/* Descriptors one chain may hold; a device's max_chain is at most this. */
#define VGS_CHAIN_MAX 16u

/*
 * The fixed pair of doorbell vectors. A ring on VGS_VEC_QUEUE means "look at
 * your queues", on VGS_VEC_CONTROL "look at the control area". The value
 * written to ivshmem's doorbell register names the peer rung and the vector.
 */
#define VGS_VEC_QUEUE           0u
#define VGS_VEC_CONTROL         1u
#define VGS_DOORBELL(peer, vec) ((((uint32_t)(peer)) << 16) | (uint32_t)(vec))

/* Transport features, negotiated apart from virtio's. */
#define VGS_TF_DOORBELL 0x1u /* both sides ring doorbells instead of polling */
#define VGS_TF_ALL      VGS_TF_DOORBELL

/*
 * Virtio feature bits the transport knows. Device-specific bits 0 to 23
 * (virtio-gpu's) pass through; of the rest only VERSION_1, which is
 * required, may be accepted. Indirect descriptors, event indices and packed
 * rings are refused by this version.
 */
#define VGS_F_DEVICE_MASK   0x0000000000ffffffULL
#define VGS_F_INDIRECT_DESC (1ULL << 28)
#define VGS_F_EVENT_IDX     (1ULL << 29)
#define VGS_F_VERSION_1     (1ULL << 32)
#define VGS_F_RING_PACKED   (1ULL << 34)
#define VGS_F_ALLOWED       (VGS_F_DEVICE_MASK | VGS_F_VERSION_1)

/* Split virtqueue flags, as virtio 1.x defines them. */
#define VGS_DESC_F_NEXT          1u
#define VGS_DESC_F_WRITE         2u
#define VGS_DESC_F_INDIRECT      4u
#define VGS_AVAIL_F_NO_INTERRUPT 1u
#define VGS_USED_F_NO_NOTIFY     1u

/* The driver's states, in vgs_driver.state. */
#define VGS_DRV_NONE  0u /* no driver: the region is not laid out */
#define VGS_DRV_RESET 1u /* layout and epoch set; waiting for the offer */
#define VGS_DRV_OK    2u /* features accepted and queues set: go */
#define VGS_DRV_GONE  3u /* the driver detached in order */

/* The device's states, in vgs_device.state. */
#define VGS_DEV_NONE        0u /* no device */
#define VGS_DEV_READY       1u /* features, limits and config offered */
#define VGS_DEV_RUNNING     2u /* the driver's setup accepted */
#define VGS_DEV_FAILED      3u /* the driver's setup refused; see error */
#define VGS_DEV_NEEDS_RESET 4u /* a fault while running; see error */
#define VGS_DEV_GONE        5u /* the device detached in order */

/* Results of the checks; 0 is success. */
#define VGS_OK         0
#define VGS_E_MAGIC    1  /* not a laid-out region */
#define VGS_E_VERSION  2  /* a major version this code does not speak */
#define VGS_E_LAYOUT   3  /* the control area, rings or heap misplaced */
#define VGS_E_STATE    4  /* a state the step does not allow */
#define VGS_E_EPOCH    5  /* the other side answers another epoch */
#define VGS_E_ROLE     6  /* a peer id that cannot be the other side's */
#define VGS_E_FEATURES 7  /* features outside the offer or refused */
#define VGS_E_LIMITS   8  /* limits outside what the transport allows */
#define VGS_E_QUEUE    9  /* a queue's size or placement wrong */
#define VGS_E_RANGE    10 /* an offset and length outside the heap */
#define VGS_E_CHAIN    11 /* a descriptor chain malformed or too long */
#define VGS_E_RING     12 /* a ring index moved by more than the queue */
#define VGS_E_ID       13 /* an object id zero, unknown or in use */
#define VGS_E_QUOTA    14 /* an object or a byte past the limits */
#define VGS_E_RACE     15 /* a block changed under every read of it */

/* The region's layout, at VGS_LAYOUT_OFF; written once by the driver. */
struct vgs_layout {
    uint32_t magic;       /* VGS_MAGIC, written last */
    uint16_t major;       /* VGS_VERSION_MAJOR */
    uint16_t minor;       /* VGS_VERSION_MINOR */
    uint32_t ctrl_size;   /* VGS_CTRL_SIZE */
    uint32_t page_size;   /* VGS_PAGE */
    uint64_t region_size; /* bytes in the region */
    uint64_t ring_off;    /* the ring area: the queues' parts, nothing else */
    uint64_t ring_len;
    uint64_t heap_off; /* the heap: commands, responses and backing */
    uint64_t heap_len;
    uint64_t reserved;
};

/* One virtqueue's setup; offsets into the region, in the ring area. */
struct vgs_queue {
    uint16_t size; /* entries: a power of two, 0 for a queue not used */
    uint16_t reserved0;
    uint32_t reserved1;
    uint64_t desc;  /* 16 * size bytes, 16-aligned */
    uint64_t avail; /* 6 + 2 * size bytes, 2-aligned */
    uint64_t used;  /* 6 + 8 * size bytes, 4-aligned */
};

/* The driver's block, at VGS_DRIVER_OFF; written only by the driver. */
struct vgs_driver {
    uint32_t epoch;              /* the connection's epoch, never 0 once laid out */
    uint32_t state;              /* VGS_DRV_*, written after every other field */
    uint32_t peer;               /* the driver's ivshmem peer id, below 65536 */
    uint32_t error;              /* VGS_E_* the driver last met, for the device's log */
    uint64_t transport_features; /* accepted VGS_TF_* */
    uint64_t features;           /* accepted virtio features */
    uint32_t events_seen;        /* the device's events_seq the driver has handled */
    uint32_t reserved[3];
    struct vgs_queue queue[VGS_QUEUES];
};

/*
 * The device's quotas on what the driver may make it hold. Each side's
 * transport table enforces them; a request past one fails, as a full region
 * does on the driver's side, and is never silently trimmed.
 */
struct vgs_limits {
    uint32_t max_resources; /* live resources */
    uint32_t max_contexts;  /* live contexts */
    uint64_t max_backing;   /* bytes of backing over every resource */
    uint32_t max_entries;   /* memory entries in one ATTACH_BACKING */
    uint32_t max_chain;     /* descriptors in one chain, <= VGS_CHAIN_MAX */
    uint32_t max_cmd;       /* bytes one chain may carry each way, >= VGS_PAGE */
    uint16_t max_queue;     /* the largest queue size the device takes */
    uint16_t reserved;
};

/* The device's block, at VGS_DEVICE_OFF; written only by the device. */
struct vgs_device {
    uint32_t epoch;              /* the driver's epoch this block answers */
    uint32_t state;              /* VGS_DEV_*, written after every other field */
    uint32_t peer;               /* the device's ivshmem peer id */
    uint32_t error;              /* VGS_E_* behind VGS_DEV_FAILED or NEEDS_RESET */
    uint64_t transport_features; /* offered VGS_TF_* */
    uint64_t features;           /* offered virtio features */
    struct vgs_limits limits;
    uint32_t events_seq;   /* display events raised; a change is EVENT_DISPLAY */
    uint32_t num_scanouts; /* virtio_gpu_config's, 1 to 16 */
    uint32_t num_capsets;  /* virtio_gpu_config's */
    uint32_t reserved;
};

/* A split virtqueue's descriptor; addr is an offset into the region. */
struct vgs_desc {
    uint64_t addr;
    uint32_t len;
    uint16_t flags;
    uint16_t next;
};

/* One checked segment of a chain, copied out of the descriptor table. */
struct vgs_seg {
    uint64_t addr;
    uint32_t len;
    uint32_t write; /* 1 when the device writes it */
};

/*
 * A descriptor chain as the device sees it after vgs_chain_read: readable
 * segments first, then writable ones, every one inside the heap.
 */
struct vgs_chain {
    uint32_t n;      /* segments */
    uint32_t nread;  /* the first nread are readable */
    uint32_t rbytes; /* bytes the device may read */
    uint32_t wbytes; /* bytes the device may write */
    struct vgs_seg seg[VGS_CHAIN_MAX];
};

/* Kinds of object a transport table holds. */
#define VGS_OBJ_RESOURCE 0u
#define VGS_OBJ_CONTEXT  1u
#define VGS_OBJ_KINDS    2u

/*
 * Object ids the driver makes: a slot in its low 24 bits and a generation
 * that moves each time the slot is reused in the high 8, so that an id kept
 * past its object's end names nothing live. The device treats ids as opaque.
 */
#define VGS_ID_MAKE(slot, gen) ((((uint32_t)(gen) & 0xffu) << 24) | ((uint32_t)(slot) & 0xffffffu))
#define VGS_ID_SLOT(id)        ((uint32_t)(id) & 0xffffffu)
#define VGS_ID_GEN(id)         (((uint32_t)(id) >> 24) & 0xffu)

/* One live object in a side's own transport table; id 0 is a free entry. */
struct vgs_obj {
    uint32_t id;
    uint32_t epoch; /* the epoch it was made in; it dies with it */
    uint32_t kind;  /* VGS_OBJ_* */
    uint32_t reserved;
    uint64_t bytes; /* backing charged to it */
};

/*
 * A side's transport table: the objects of the current epoch and what they
 * hold against the limits. It lives in the side's own memory, never in the
 * region, which the other side could change under it.
 */
struct vgs_table {
    struct vgs_obj *obj; /* cap entries, owned by the caller */
    uint32_t cap;
    uint32_t epoch;
    uint32_t count[VGS_OBJ_KINDS];
    uint64_t bytes;
    struct vgs_limits limits;
};

/* The C89 compile-time check of the layout both architectures must share. */
#define VGS_ASSERT(name, cond) typedef char vgs_assert_##name[(cond) ? 1 : -1]
VGS_ASSERT(layout_size, sizeof(struct vgs_layout) == 64);
VGS_ASSERT(queue_size, sizeof(struct vgs_queue) == 32);
VGS_ASSERT(driver_size, sizeof(struct vgs_driver) == 112);
VGS_ASSERT(limits_size, sizeof(struct vgs_limits) == 32);
VGS_ASSERT(device_size, sizeof(struct vgs_device) == 80);
VGS_ASSERT(desc_size, sizeof(struct vgs_desc) == 16);
VGS_ASSERT(driver_fits, VGS_DRIVER_OFF + sizeof(struct vgs_driver) <= VGS_DEVICE_OFF);
VGS_ASSERT(device_fits, VGS_DEVICE_OFF + sizeof(struct vgs_device) <= VGS_CTRL_SIZE);

#ifndef VGS_MB
#define VGS_MB() __sync_synchronize()
#endif

/* Pointers to the control area's blocks of a mapped region. */
#define VGS_LAYOUT(base) \
    ((volatile struct vgs_layout *)((volatile uint8_t *)(base) + VGS_LAYOUT_OFF))
#define VGS_DRIVER(base) \
    ((volatile struct vgs_driver *)((volatile uint8_t *)(base) + VGS_DRIVER_OFF))
#define VGS_DEVICE(base) \
    ((volatile struct vgs_device *)((volatile uint8_t *)(base) + VGS_DEVICE_OFF))

const char *vgs_strerror(int err);
int vgs_layout_make(struct vgs_layout *l, uint64_t region_size, uint64_t ring_len);
int vgs_check_layout(const struct vgs_layout *l, uint64_t mapped);
int vgs_snap_layout(volatile const void *base, struct vgs_layout *l);
int vgs_snap_driver(volatile const void *base, struct vgs_driver *d);
int vgs_snap_device(volatile const void *base, struct vgs_device *d);
int vgs_check_limits(const struct vgs_limits *lim);
int vgs_check_offer(const struct vgs_device *dev, uint32_t epoch, uint32_t driver_peer);
int vgs_check_queue(const struct vgs_layout *l, const struct vgs_queue *q, uint32_t max_queue);
int vgs_check_accept(const struct vgs_layout *l, const struct vgs_driver *drv,
                     const struct vgs_device *offer);
int vgs_check_range(const struct vgs_layout *l, uint64_t addr, uint64_t len);
uint32_t vgs_queue_bytes(uint32_t size);
uint64_t vgs_queue_place(struct vgs_queue *q, uint64_t off, uint32_t size);
int vgs_chain_read(volatile const void *base, const struct vgs_layout *l, const struct vgs_queue *q,
                   const struct vgs_limits *lim, uint32_t head, struct vgs_chain *c);
int vgs_avail_read(volatile const void *base, const struct vgs_queue *q, uint16_t last,
                   uint16_t *idx);
uint32_t vgs_avail_head(volatile const void *base, const struct vgs_queue *q, uint16_t pos);
int vgs_used_read(volatile const void *base, const struct vgs_queue *q, uint16_t last,
                  uint16_t *idx);
int vgs_used_elem(volatile const void *base, const struct vgs_queue *q, uint16_t pos, uint32_t *id,
                  uint32_t *len);
void vgs_desc_write(volatile void *base, const struct vgs_queue *q, uint32_t i,
                    const struct vgs_desc *d);
void vgs_avail_push(volatile void *base, const struct vgs_queue *q, uint16_t *idx, uint32_t head);
void vgs_used_push(volatile void *base, const struct vgs_queue *q, uint16_t *idx, uint32_t id,
                   uint32_t len);
void vgs_table_init(struct vgs_table *t, struct vgs_obj *obj, uint32_t cap,
                    const struct vgs_limits *lim, uint32_t epoch);
int vgs_table_add(struct vgs_table *t, uint32_t kind, uint32_t id);
struct vgs_obj *vgs_table_find(struct vgs_table *t, uint32_t kind, uint32_t id);
int vgs_table_charge(struct vgs_table *t, struct vgs_obj *o, uint64_t bytes);
void vgs_table_del(struct vgs_table *t, struct vgs_obj *o);
uint32_t vgs_table_epoch(struct vgs_table *t, uint32_t epoch);

#ifdef VGS_IMPLEMENTATION

/* Ring parts' addresses, from offsets vgs_check_queue has passed. */
#define VGS_AT(base, off)  ((volatile uint8_t *)(base) + (off))
#define VGS_U16(base, off) (*(volatile uint16_t *)VGS_AT(base, off))
#define VGS_U32(base, off) (*(volatile uint32_t *)VGS_AT(base, off))

/** Return a short lowercase description of a VGS_E_* result. */
const char *vgs_strerror(int err)
{
    static const char *const msg[] = {
        "ok",
        "bad magic",
        "unknown major version",
        "bad layout",
        "bad state",
        "epoch mismatch",
        "bad peer id",
        "bad features",
        "bad limits",
        "bad queue",
        "range outside the heap",
        "bad descriptor chain",
        "ring index moved too far",
        "bad object id",
        "over quota",
        "block changing under every read",
    };

    if (err < 0 || err > VGS_E_RACE) {
        return "unknown error";
    }
    return msg[err];
}

/** Return nonzero when x is a nonzero power of two. */
static int vgs_pow2(uint64_t x)
{
    return x != 0 && (x & (x - 1)) == 0;
}

/**
 * Fill a layout for a region of region_size bytes: the control area, a ring
 * area of ring_len bytes after it, and the rest as the heap.
 *
 * Returns VGS_OK, or VGS_E_LAYOUT when the sizes break the layout's rules.
 * The magic is set; the caller stores it last when writing the region.
 */
int vgs_layout_make(struct vgs_layout *l, uint64_t region_size, uint64_t ring_len)
{
    l->magic = VGS_MAGIC;
    l->major = VGS_VERSION_MAJOR;
    l->minor = VGS_VERSION_MINOR;
    l->ctrl_size = VGS_CTRL_SIZE;
    l->page_size = VGS_PAGE;
    l->region_size = region_size;
    l->ring_off = VGS_CTRL_SIZE;
    l->ring_len = ring_len;
    l->heap_off = VGS_CTRL_SIZE + ring_len;
    l->heap_len = region_size > l->heap_off ? region_size - l->heap_off : 0;
    l->reserved = 0;
    return vgs_check_layout(l, region_size);
}

/**
 * Check a layout read from a region of which mapped bytes are mapped.
 *
 * The control area is the first page; the ring area follows it and the heap
 * follows the ring area, each page-aligned, nonempty and inside the region,
 * whose size is a power of two from VGS_REGION_MIN to 2^VGS_REGION_MAX_SHIFT
 * and no larger than what is mapped. Returns VGS_OK or the rule broken.
 */
int vgs_check_layout(const struct vgs_layout *l, uint64_t mapped)
{
    uint64_t pm = VGS_PAGE - 1;

    if (l->magic != VGS_MAGIC) {
        return VGS_E_MAGIC;
    }
    if (l->major != VGS_VERSION_MAJOR) {
        return VGS_E_VERSION;
    }
    if (l->ctrl_size != VGS_CTRL_SIZE || l->page_size != VGS_PAGE) {
        return VGS_E_LAYOUT;
    }
    if (!vgs_pow2(l->region_size) || l->region_size < VGS_REGION_MIN ||
        l->region_size > (1ULL << VGS_REGION_MAX_SHIFT) || l->region_size > mapped) {
        return VGS_E_LAYOUT;
    }
    if (l->ring_off != VGS_CTRL_SIZE || l->ring_len == 0 || (l->ring_len & pm) != 0 ||
        l->ring_len >= l->region_size) {
        return VGS_E_LAYOUT;
    }
    if (l->heap_off != l->ring_off + l->ring_len || l->heap_off >= l->region_size ||
        l->heap_len == 0 || (l->heap_len & pm) != 0 || l->heap_len > l->region_size - l->heap_off) {
        return VGS_E_LAYOUT;
    }
    return VGS_OK;
}

/*
 * Copy n bytes, a multiple of 4, of a volatile block into private memory a
 * 32-bit word at a time, so that no word the other side stores is torn.
 * Each word is stored as its four little-endian bytes: a store through a
 * uint32_t pointer into a structure of other types breaks C's aliasing
 * rule, and an optimizing compiler may then read the structure as unset.
 */
static void vgs_copy_in(void *dst, volatile const void *src, uint32_t n)
{
    uint8_t *d = (uint8_t *)dst;
    volatile const uint32_t *s = (volatile const uint32_t *)src;
    uint32_t i, w;

    for (i = 0; i < n / 4; i++) {
        w = s[i];
        d[4 * i] = (uint8_t)w;
        d[4 * i + 1] = (uint8_t)(w >> 8);
        d[4 * i + 2] = (uint8_t)(w >> 16);
        d[4 * i + 3] = (uint8_t)(w >> 24);
    }
}

/**
 * Copy the layout out of a region into private memory, where it is checked
 * and used; the driver writes it once, before any state, so one copy holds.
 */
int vgs_snap_layout(volatile const void *base, struct vgs_layout *l)
{
    VGS_MB();
    vgs_copy_in(l, VGS_LAYOUT(base), sizeof(*l));
    return l->magic == VGS_MAGIC ? VGS_OK : VGS_E_MAGIC;
}

/*
 * Copy a driver's or device's block, whose first two words are its epoch
 * and state and whose writer changes its other fields only before storing a
 * new epoch or state: read those two words, the block and the two words
 * again, and keep the copy when all three reads agree. The other side may
 * write at any time, so a few tries are made before VGS_E_RACE.
 */
static int vgs_snap(volatile const void *blk, void *out, uint32_t n)
{
    volatile const uint32_t *w = (volatile const uint32_t *)blk;
    const uint32_t *o = (const uint32_t *)out;
    uint32_t e0, s0;
    int tries;

    for (tries = 0; tries < 8; tries++) {
        e0 = w[0];
        s0 = w[1];
        VGS_MB();
        vgs_copy_in(out, blk, n);
        VGS_MB();
        if (o[0] == e0 && o[1] == s0 && w[0] == e0 && w[1] == s0) {
            return VGS_OK;
        }
    }
    return VGS_E_RACE;
}

/** Copy the driver's block out of a region; see vgs_snap. */
int vgs_snap_driver(volatile const void *base, struct vgs_driver *d)
{
    return vgs_snap(VGS_DRIVER(base), d, sizeof(*d));
}

/** Copy the device's block out of a region; see vgs_snap. */
int vgs_snap_device(volatile const void *base, struct vgs_device *d)
{
    return vgs_snap(VGS_DEVICE(base), d, sizeof(*d));
}

/**
 * Check a device's limits: every one nonzero, chains of 2 to VGS_CHAIN_MAX
 * descriptors, at least a page per chain, and a power-of-two largest queue
 * no larger than VGS_QUEUE_MAX.
 */
int vgs_check_limits(const struct vgs_limits *lim)
{
    if (lim->max_resources == 0 || lim->max_contexts == 0 || lim->max_backing == 0 ||
        lim->max_entries == 0) {
        return VGS_E_LIMITS;
    }
    if (lim->max_chain < 2 || lim->max_chain > VGS_CHAIN_MAX || lim->max_cmd < VGS_PAGE) {
        return VGS_E_LIMITS;
    }
    if (!vgs_pow2(lim->max_queue) || lim->max_queue > VGS_QUEUE_MAX) {
        return VGS_E_LIMITS;
    }
    return VGS_OK;
}

/**
 * The driver's check of the device's offer for its epoch: the state READY,
 * or RUNNING once the setup is accepted, the epoch the driver's, a peer id other than the driver's,
 * VERSION_1 offered, sane limits and 1 to 16 scanouts.
 */
int vgs_check_offer(const struct vgs_device *dev, uint32_t epoch, uint32_t driver_peer)
{
    int err;

    if (dev->epoch != epoch) {
        return VGS_E_EPOCH;
    }
    if (dev->state != VGS_DEV_READY && dev->state != VGS_DEV_RUNNING) {
        return VGS_E_STATE;
    }
    if (dev->peer > 0xffffu || dev->peer == driver_peer) {
        return VGS_E_ROLE;
    }
    if ((dev->features & VGS_F_VERSION_1) == 0) {
        return VGS_E_FEATURES;
    }
    if ((err = vgs_check_limits(&dev->limits)) != VGS_OK) {
        return err;
    }
    if (dev->num_scanouts == 0 || dev->num_scanouts > 16) {
        return VGS_E_LIMITS;
    }
    return VGS_OK;
}

/** Return the bytes of a queue's three parts, each rounded up to 16. */
uint32_t vgs_queue_bytes(uint32_t size)
{
    return 16 * size + ((6 + 2 * size + 15) & ~15u) + ((6 + 8 * size + 15) & ~15u);
}

/* Return nonzero when [a, a+alen) and [b, b+blen) overlap. */
static int vgs_overlap(uint64_t a, uint64_t alen, uint64_t b, uint64_t blen)
{
    return a < b + blen && b < a + alen;
}

/*
 * Check one part of a queue: aligned, and inside the ring area. The area is
 * inside the region by vgs_check_layout, so the sum cannot overflow.
 */
static int vgs_part_ok(const struct vgs_layout *l, uint64_t off, uint64_t len, uint64_t align)
{
    if ((off & (align - 1)) != 0 || off < l->ring_off) {
        return 0;
    }
    if (off - l->ring_off > l->ring_len || len > l->ring_len - (off - l->ring_off)) {
        return 0;
    }
    return 1;
}

/**
 * Check one queue's setup against a checked layout: an unused queue is all
 * zeros; a used one has a power-of-two size up to max_queue and its three
 * parts aligned, inside the ring area and apart from each other.
 */
int vgs_check_queue(const struct vgs_layout *l, const struct vgs_queue *q, uint32_t max_queue)
{
    uint64_t dl, al, ul;

    if (q->size == 0) {
        return q->desc == 0 && q->avail == 0 && q->used == 0 ? VGS_OK : VGS_E_QUEUE;
    }
    if (!vgs_pow2(q->size) || q->size > max_queue || q->size > VGS_QUEUE_MAX) {
        return VGS_E_QUEUE;
    }
    dl = 16 * (uint64_t)q->size;
    al = 6 + 2 * (uint64_t)q->size;
    ul = 6 + 8 * (uint64_t)q->size;
    if (!vgs_part_ok(l, q->desc, dl, 16) || !vgs_part_ok(l, q->avail, al, 2) ||
        !vgs_part_ok(l, q->used, ul, 4)) {
        return VGS_E_QUEUE;
    }
    if (vgs_overlap(q->desc, dl, q->avail, al) || vgs_overlap(q->desc, dl, q->used, ul) ||
        vgs_overlap(q->avail, al, q->used, ul)) {
        return VGS_E_QUEUE;
    }
    return VGS_OK;
}

/* Return nonzero when two used queues share a byte of the ring area. */
static int vgs_queues_overlap(const struct vgs_queue *a, const struct vgs_queue *b)
{
    uint64_t pa[3], la[3], pb[3], lb[3];
    int i, j;

    if (a->size == 0 || b->size == 0) {
        return 0;
    }
    pa[0] = a->desc;
    la[0] = 16 * (uint64_t)a->size;
    pa[1] = a->avail;
    la[1] = 6 + 2 * (uint64_t)a->size;
    pa[2] = a->used;
    la[2] = 6 + 8 * (uint64_t)a->size;
    pb[0] = b->desc;
    lb[0] = 16 * (uint64_t)b->size;
    pb[1] = b->avail;
    lb[1] = 6 + 2 * (uint64_t)b->size;
    pb[2] = b->used;
    lb[2] = 6 + 8 * (uint64_t)b->size;
    for (i = 0; i < 3; i++) {
        for (j = 0; j < 3; j++) {
            if (vgs_overlap(pa[i], la[i], pb[j], lb[j])) {
                return 1;
            }
        }
    }
    return 0;
}

/**
 * The device's check of the driver's setup against the device's own copy of
 * its offer: the same epoch, the state OK, a peer id other than the
 * device's, features within the offer and the transport's allowed set with
 * VERSION_1 among them, and the control queue set and both queues valid and
 * apart. Returns VGS_OK or the rule broken, which the device reports in its
 * error field with VGS_DEV_FAILED.
 */
int vgs_check_accept(const struct vgs_layout *l, const struct vgs_driver *drv,
                     const struct vgs_device *offer)
{
    int i, err;

    if (drv->epoch != offer->epoch) {
        return VGS_E_EPOCH;
    }
    if (drv->state != VGS_DRV_OK) {
        return VGS_E_STATE;
    }
    if (drv->peer > 0xffffu || drv->peer == offer->peer) {
        return VGS_E_ROLE;
    }
    if ((drv->features & ~(offer->features & VGS_F_ALLOWED)) != 0 ||
        (drv->features & VGS_F_VERSION_1) == 0) {
        return VGS_E_FEATURES;
    }
    if ((drv->transport_features & ~(offer->transport_features & VGS_TF_ALL)) != 0) {
        return VGS_E_FEATURES;
    }
    if (drv->queue[VGS_QUEUE_CONTROL].size == 0) {
        return VGS_E_QUEUE;
    }
    for (i = 0; i < VGS_QUEUES; i++) {
        if ((err = vgs_check_queue(l, &drv->queue[i], offer->limits.max_queue)) != VGS_OK) {
            return err;
        }
    }
    if (vgs_queues_overlap(&drv->queue[0], &drv->queue[1])) {
        return VGS_E_QUEUE;
    }
    return VGS_OK;
}

/**
 * Check that len bytes at offset addr, a DMA address of the transport, lie
 * inside the heap: nonzero, without overflow, and never in the control area
 * or the rings. Every buffer either side names goes through this.
 */
int vgs_check_range(const struct vgs_layout *l, uint64_t addr, uint64_t len)
{
    if (len == 0 || addr < l->heap_off) {
        return VGS_E_RANGE;
    }
    if (addr - l->heap_off > l->heap_len || len > l->heap_len - (addr - l->heap_off)) {
        return VGS_E_RANGE;
    }
    return VGS_OK;
}

/**
 * Lay out a queue of size entries at offset off of the ring area, its parts
 * in order and each 16-aligned. Returns the offset after it, where the next
 * queue may go; vgs_check_queue says whether it fits.
 */
uint64_t vgs_queue_place(struct vgs_queue *q, uint64_t off, uint32_t size)
{
    q->size = (uint16_t)size;
    q->reserved0 = 0;
    q->reserved1 = 0;
    q->desc = off;
    q->avail = off + 16 * (uint64_t)size;
    q->used = q->avail + ((6 + 2 * (uint64_t)size + 15) & ~15ULL);
    return off + vgs_queue_bytes(size);
}

/**
 * Read the chain that starts at descriptor head of a queue the device has
 * accepted, copying each descriptor once and checking it.
 *
 * A chain has at most lim->max_chain descriptors and never more than the
 * queue holds, so a loop ends in VGS_E_CHAIN; it names no indirect table;
 * its readable segments come before its writable ones; every segment is
 * inside the heap; and each way carries at most lim->max_cmd bytes. The
 * device works only from the copy in c, never from the table again.
 */
int vgs_chain_read(volatile const void *base, const struct vgs_layout *l, const struct vgs_queue *q,
                   const struct vgs_limits *lim, uint32_t head, struct vgs_chain *c)
{
    uint32_t i = head, steps = 0, max = lim->max_chain;
    struct vgs_desc d;

    c->n = c->nread = c->rbytes = c->wbytes = 0;
    if (max > q->size) {
        max = q->size;
    }
    if (max > VGS_CHAIN_MAX) {
        max = VGS_CHAIN_MAX;
    }
    for (;;) {
        if (i >= q->size || steps == max) {
            return VGS_E_CHAIN;
        }
        vgs_copy_in(&d, VGS_AT(base, q->desc + 16 * (uint64_t)i), sizeof(d));
        if ((d.flags & ~(VGS_DESC_F_NEXT | VGS_DESC_F_WRITE)) != 0) {
            return VGS_E_CHAIN;
        }
        if (vgs_check_range(l, d.addr, d.len) != VGS_OK) {
            return VGS_E_RANGE;
        }
        if (d.flags & VGS_DESC_F_WRITE) {
            if (d.len > lim->max_cmd - c->wbytes) {
                return VGS_E_CHAIN;
            }
            c->wbytes += d.len;
        } else {
            if (c->n != c->nread || d.len > lim->max_cmd - c->rbytes) {
                return VGS_E_CHAIN;
            }
            c->rbytes += d.len;
            c->nread++;
        }
        c->seg[c->n].addr = d.addr;
        c->seg[c->n].len = d.len;
        c->seg[c->n].write = (d.flags & VGS_DESC_F_WRITE) != 0;
        c->n++;
        steps++;
        if ((d.flags & VGS_DESC_F_NEXT) == 0) {
            return VGS_OK;
        }
        i = d.next;
    }
}

/**
 * Read a queue's avail index into idx. last is the index the reader has
 * consumed to; an index more than the queue's size ahead of it is a fault.
 */
int vgs_avail_read(volatile const void *base, const struct vgs_queue *q, uint16_t last,
                   uint16_t *idx)
{
    uint16_t v = VGS_U16(base, q->avail + 2);

    VGS_MB();
    if ((uint16_t)(v - last) > q->size) {
        return VGS_E_RING;
    }
    *idx = v;
    return VGS_OK;
}

/** Return the head the avail ring holds at pos; vgs_chain_read checks it. */
uint32_t vgs_avail_head(volatile const void *base, const struct vgs_queue *q, uint16_t pos)
{
    return VGS_U16(base, q->avail + 4 + 2 * (uint64_t)(pos & (q->size - 1)));
}

/** Read a queue's used index into idx, checked as vgs_avail_read does. */
int vgs_used_read(volatile const void *base, const struct vgs_queue *q, uint16_t last,
                  uint16_t *idx)
{
    uint16_t v = VGS_U16(base, q->used + 2);

    VGS_MB();
    if ((uint16_t)(v - last) > q->size) {
        return VGS_E_RING;
    }
    *idx = v;
    return VGS_OK;
}

/**
 * Read the used element at pos into id and len. An id outside the queue is
 * VGS_E_CHAIN; the driver further checks that id is a head it has
 * outstanding and len no more than that chain's writable bytes.
 */
int vgs_used_elem(volatile const void *base, const struct vgs_queue *q, uint16_t pos, uint32_t *id,
                  uint32_t *len)
{
    uint64_t e = q->used + 4 + 8 * (uint64_t)(pos & (q->size - 1));

    *id = VGS_U32(base, e);
    *len = VGS_U32(base, e + 4);
    return *id < q->size ? VGS_OK : VGS_E_CHAIN;
}

/** Write descriptor i of a queue. */
void vgs_desc_write(volatile void *base, const struct vgs_queue *q, uint32_t i,
                    const struct vgs_desc *d)
{
    volatile struct vgs_desc *t =
        (volatile struct vgs_desc *)VGS_AT(base, q->desc + 16 * (uint64_t)i);

    t->addr = d->addr;
    t->len = d->len;
    t->flags = d->flags;
    t->next = d->next;
}

/** Publish head in the avail ring and advance the driver's index *idx. */
void vgs_avail_push(volatile void *base, const struct vgs_queue *q, uint16_t *idx, uint32_t head)
{
    VGS_U16(base, q->avail + 4 + 2 * (uint64_t)(*idx & (q->size - 1))) = (uint16_t)head;
    VGS_MB();
    *idx = (uint16_t)(*idx + 1);
    VGS_U16(base, q->avail + 2) = *idx;
    VGS_MB();
}

/** Publish a used element and advance the device's index *idx. */
void vgs_used_push(volatile void *base, const struct vgs_queue *q, uint16_t *idx, uint32_t id,
                   uint32_t len)
{
    uint64_t e = q->used + 4 + 8 * (uint64_t)(*idx & (q->size - 1));

    VGS_U32(base, e) = id;
    VGS_U32(base, e + 4) = len;
    VGS_MB();
    *idx = (uint16_t)(*idx + 1);
    VGS_U16(base, q->used + 2) = *idx;
    VGS_MB();
}

/** Start a side's table over cap entries for an epoch, under limits. */
void vgs_table_init(struct vgs_table *t, struct vgs_obj *obj, uint32_t cap,
                    const struct vgs_limits *lim, uint32_t epoch)
{
    uint32_t i;

    t->obj = obj;
    t->cap = cap;
    t->epoch = epoch;
    t->bytes = 0;
    for (i = 0; i < VGS_OBJ_KINDS; i++) {
        t->count[i] = 0;
    }
    t->limits = *lim;
    for (i = 0; i < cap; i++) {
        obj[i].id = 0;
    }
}

/**
 * Find a live object of the current epoch by kind and id. Returns 0 for an
 * id that names none, which the caller answers as an invalid id.
 */
struct vgs_obj *vgs_table_find(struct vgs_table *t, uint32_t kind, uint32_t id)
{
    uint32_t i;

    if (id == 0) {
        return 0;
    }
    for (i = 0; i < t->cap; i++) {
        if (t->obj[i].id == id && t->obj[i].kind == kind && t->obj[i].epoch == t->epoch) {
            return &t->obj[i];
        }
    }
    return 0;
}

/**
 * Record a new object made by a command. Returns VGS_E_ID for id 0 or an id
 * already live, and VGS_E_QUOTA when its kind is at its limit or the table
 * is full.
 */
int vgs_table_add(struct vgs_table *t, uint32_t kind, uint32_t id)
{
    uint32_t i, max;

    if (kind >= VGS_OBJ_KINDS || id == 0 || vgs_table_find(t, kind, id) != 0) {
        return VGS_E_ID;
    }
    max = kind == VGS_OBJ_RESOURCE ? t->limits.max_resources : t->limits.max_contexts;
    if (t->count[kind] >= max) {
        return VGS_E_QUOTA;
    }
    for (i = 0; i < t->cap; i++) {
        if (t->obj[i].id == 0) {
            t->obj[i].id = id;
            t->obj[i].epoch = t->epoch;
            t->obj[i].kind = kind;
            t->obj[i].reserved = 0;
            t->obj[i].bytes = 0;
            t->count[kind]++;
            return VGS_OK;
        }
    }
    return VGS_E_QUOTA;
}

/**
 * Set the backing charged to an object to bytes: an attach charges its
 * entries' total and a detach 0. Returns VGS_E_QUOTA, with the charge left
 * as it was, when the table's total would pass max_backing.
 */
int vgs_table_charge(struct vgs_table *t, struct vgs_obj *o, uint64_t bytes)
{
    uint64_t rest = t->bytes - o->bytes;

    if (bytes > t->limits.max_backing - rest) {
        return VGS_E_QUOTA;
    }
    t->bytes = rest + bytes;
    o->bytes = bytes;
    return VGS_OK;
}

/** End an object's life: its backing and its count are given back. */
void vgs_table_del(struct vgs_table *t, struct vgs_obj *o)
{
    t->bytes -= o->bytes;
    t->count[o->kind]--;
    o->id = 0;
    o->bytes = 0;
}

/**
 * Move the table to a new epoch: every object of the old one ends, as the
 * connection that made it has. Returns the count of objects ended.
 */
uint32_t vgs_table_epoch(struct vgs_table *t, uint32_t epoch)
{
    uint32_t i, n = 0;

    for (i = 0; i < t->cap; i++) {
        if (t->obj[i].id != 0) {
            vgs_table_del(t, &t->obj[i]);
            n++;
        }
    }
    t->epoch = epoch;
    return n;
}

#endif /* VGS_IMPLEMENTATION */
#endif /* VGPUSHM_H */
