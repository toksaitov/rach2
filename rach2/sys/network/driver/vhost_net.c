/* vhost_net.c: /dev/vhost-net, as vhost_net.h describes it: the guest's
 * memory, its two rings, the frames between them and a tap interface, and
 * the kernel thread that takes the guest's transmit ring. */
#include "vhost_net.h"
#include "tap.h"
#include "../ethernet/ethernet.h"
#include "../tcp/tcp.h"
#include "../../arch/arch.h"
#include "../../fs/debugfs.h"
#include "../../fs/devfs.h"
#include "../../fs/cpuinfo.h"
#include "../../fs/fd.h"
#include "../../fs/file.h"
#include "../../io/eventfd.h"
#include "../../klog.h"
#include "../../lib/kerrno.h"
#include "../../lib/kirq.h"
#include "../../lib/kstring.h"
#include "../../mm/kheap.h"
#include "../../mm/paging.h"
#include "../../mm/pin.h"
#include "../../mm/usercopy.h"
#include "../../proc/process.h"
#include "../../proc/sched.h"
#include "../../proc/smp.h"
#include "../../proc/wait_queue.h"

/* The ioctls, Linux's numbers (linux/vhost.h). */
#define VHOST_GET_FEATURES         0x8008af00u
#define VHOST_SET_FEATURES         0x4008af00u
#define VHOST_SET_OWNER            0x0000af01u
#define VHOST_RESET_OWNER          0x0000af02u
#define VHOST_SET_MEM_TABLE        0x4008af03u
#define VHOST_SET_VRING_NUM        0x4008af10u
#define VHOST_SET_VRING_ADDR       0x4028af11u
#define VHOST_SET_VRING_BASE       0x4008af12u
#define VHOST_GET_VRING_BASE       0xc008af12u
#define VHOST_SET_VRING_KICK       0x4008af20u
#define VHOST_SET_VRING_CALL       0x4008af21u
#define VHOST_SET_VRING_ERR        0x4008af22u
#define VHOST_SET_BACKEND_FEATURES 0x4008af25u
#define VHOST_GET_BACKEND_FEATURES 0x8008af26u
#define VHOST_NET_SET_BACKEND      0x4008af30u

/* The features: the backend adds and strips the virtio header (bit 27, which
 * the guest sees as VIRTIO_F_ANY_LAYOUT), and the 1.0 layout. */
#define F_NET_HDR   (1ull << 27)
#define F_MRG_RXBUF (1ull << 15)
#define F_VERSION_1 (1ull << 32)
#define VN_FEATURES (F_NET_HDR | F_VERSION_1)

/* The split ring's flags. */
#define DESC_NEXT      1u
#define DESC_WRITE     2u
#define DESC_INDIRECT  4u
#define USED_NO_NOTIFY 1u
#define AVAIL_NO_INTR  1u

#define VN_RX      0
#define VN_TX      1
#define VN_REGIONS 64   /* QEMU's default for the kernel's vhost */
#define VN_MAX     4    /* devices at once */
#define VN_HDR_MAX 12   /* struct virtio_net_hdr_mrg_rxbuf */
#define VN_BUDGET  128u /* frames of a transmit ring a pass takes */
#define VN_TXBUF   (VN_HDR_MAX + ETH_HDR_SIZE + ETH_MTU)

/* One region of the guest's memory, as VHOST_SET_MEM_TABLE gives it, with
 * the pin of its pages; `pin` is NULL for a region that could not be
 * pinned, device memory, which no ring or buffer is in. */
struct vn_region {
    uint64_t gpa, size, uaddr;
    struct pin *pin;
};

struct vnet;

/* A ring. Its addresses are QEMU's, which the regions translate. */
struct vn_vq {
    struct eventfd_hook hook; /* first: the kick's */
    struct vnet *n;
    uint32_t num;
    uint64_t desc, avail, used;
    int addr_set;
    uint16_t last_avail; /* the next entry of the available ring to take */
    uint16_t used_idx;   /* the used ring's index, as this side wrote it */
    int used_known;      /* `used_idx` read from the ring since the base */
    int no_notify;       /* USED_NO_NOTIFY is set in the ring */
    int backend;         /* VHOST_NET_SET_BACKEND gave the ring the tap */
    int broken;          /* the guest gave a bad descriptor */
    volatile int kicked; /* the kick came; the thread looks */
    struct eventfd *kick, *call, *err;
};

/* One open of /dev/vhost-net. */
struct vnet {
    struct tap_backend tb; /* first: the tap's */
    int slot;              /* in `vn_devs` */
    struct mm *mm;         /* the owner's, VHOST_SET_OWNER */
    uint64_t features;
    uint32_t hdr; /* bytes of the virtio header before each frame */
    struct vn_region reg[VN_REGIONS];
    uint32_t nreg, last;
    struct pin_holder holder;
    int dead; /* an unmap took a pin back: no more access */
    struct vn_vq vq[2];
    struct tap *tap;
    uint8_t txbuf[VN_TXBUF];
};

static struct vnet *vn_devs[VN_MAX];
static wait_queue_t vn_wq;        /* the thread sleeps here */
static volatile uint32_t vn_work; /* a kick came since the thread looked */
static process_t *vn_thread;
/* Since boot, for /proc/stat, in vhost_net.h's order. Racy increments; they
 * only count. */
static uint64_t vn_counts[VHOST_NET_COUNTS];
/* The cycle counter and the processor at the first kick since the thread
 * last ran, 0 when none is pending; and each processor's core siblings, a
 * bit each, filled at the thread's first wake on or from it (0: not yet). */
static volatile uint64_t vn_kick_at;
static volatile int vn_kick_cpu;
static uint64_t vn_core[VN_CPUS];
/* For the request-response breakdown: the cycle counter at the last TCP
 * segment with data put on a receive ring (the request), 0 once its reply
 * came; whether a pure acknowledgement came back since; and the counter at
 * the last segment with data taken off a transmit ring (the reply), 0 once
 * the next request went out. */
static uint64_t vn_req_at, vn_reply_at;
static int vn_req_acked;
/* The cycle counter at the last interrupt of a passed-through device since
 * the request, 0 when none came. */
static uint64_t vn_msi_at;
/* 1 once a device interrupt came since the request. */
static int vn_msi_seen;
/* 1: a doorbell on a transmit ring takes the frames at once, in the
 * doorbell's context, when the big lock is free, and wakes the thread only
 * when it is not or the budget runs out; 0: the thread takes every one. Set
 * through debugfs net/vhost_inline. */
static int vn_inline;

/** The region that holds `a`, a guest-physical address, or QEMU's with
 * `va`; NULL for none. */
static struct vn_region *vn_find(struct vnet *n, uint64_t a, int va)
{
    struct vn_region *r;
    uint32_t i;

    if (n->last < n->nreg) {
        r = &n->reg[n->last];
        if (a >= (va ? r->uaddr : r->gpa) && a - (va ? r->uaddr : r->gpa) < r->size) {
            return r;
        }
    }
    for (i = 0; i < n->nreg; i++) {
        r = &n->reg[i];
        if (a >= (va ? r->uaddr : r->gpa) && a - (va ? r->uaddr : r->gpa) < r->size) {
            n->last = i;
            return r;
        }
    }
    return 0;
}

/** Copy `len` bytes between the guest's memory at `a`, by guest address or
 * QEMU's with `va`, and `out`, or from `in` into the guest when `in` is not
 * NULL: 0, or -EFAULT when a byte is in no pinned region. Any context; does
 * not sleep. */
static int vn_mem(struct vnet *n, uint64_t a, int va, const void *in, void *out, uint32_t len)
{
    const uint8_t *src = (const uint8_t *)in;
    uint8_t *dst = (uint8_t *)out;

    while (len != 0) {
        struct vn_region *r = vn_find(n, a, va);
        uint64_t off, ua;
        uintptr_t phys;
        uint32_t chunk, fl;
        uint8_t *m;

        if (!r || !r->pin || n->dead) {
            return -EFAULT;
        }
        off = a - (va ? r->uaddr : r->gpa);
        ua = r->uaddr + off;
        phys = pin_phys(r->pin, (uint32_t)((ua >> 12) - (r->uaddr >> 12)));
        if (phys == 0) {
            return -EFAULT;
        }
        chunk = 4096u - (uint32_t)(ua & 4095u);
        if (chunk > len) {
            chunk = len;
        }
        if ((uint64_t)chunk > r->size - off) {
            chunk = (uint32_t)(r->size - off);
        }
        m = (uint8_t *)kmap_cpu(phys, &fl);
        if (src) {
            k_memcpy(m + (uint32_t)(ua & 4095u), src, chunk);
            src += chunk;
        } else {
            k_memcpy(dst, m + (uint32_t)(ua & 4095u), chunk);
            dst += chunk;
        }
        kunmap_cpu(m, fl);
        a += chunk;
        len -= chunk;
    }
    return 0;
}

/** Read the 16-bit word at QEMU's address `va`: 0, or -EFAULT. */
static int vn_get16(struct vnet *n, uint64_t va, uint16_t *v)
{
    return vn_mem(n, va, 1, NULL, v, 2);
}

/** Write the 16-bit word at QEMU's address `va`: 0, or -EFAULT. */
static int vn_put16(struct vnet *n, uint64_t va, uint16_t v)
{
    return vn_mem(n, va, 1, &v, NULL, 2);
}

/** Whether the ring `q` moves frames now. */
static int vn_ready(const struct vnet *n, const struct vn_vq *q)
{
    return n->tap && q->backend && q->addr_set && q->num && !q->broken && !n->dead;
}

/** Stop the ring `q` for a bad descriptor or address, logged once, and tell
 * QEMU through the error eventfd. */
static void vn_break(struct vnet *n, struct vn_vq *q)
{
    if (!q->broken) {
        klog(KLOG_WARN, "vhost-net", "broken: ring %d: bad descriptor or address",
             (int)(q - n->vq));
        if (q->err) {
            eventfd_signal(q->err);
        }
    }
    q->broken = 1;
}

/** Read the used ring's index once after a base was set: 0, or -EFAULT. */
static int vn_used_init(struct vnet *n, struct vn_vq *q)
{
    if (q->used_known) {
        return 0;
    }
    if (vn_get16(n, q->used + 2, &q->used_idx) != 0) {
        return -EFAULT;
    }
    q->used_known = 1;
    return 0;
}

/** Ask the guest for kicks on `q`, or for none with `on`. */
static void vn_quiet(struct vnet *n, struct vn_vq *q, int on)
{
    if (q->no_notify != on) {
        (void)vn_put16(n, q->used, on ? (uint16_t)USED_NO_NOTIFY : 0);
        q->no_notify = on;
    }
}

/** Put the chain `head`, of which `len` bytes were written, on the used
 * ring: 0, or -EFAULT. */
static int vn_used(struct vnet *n, struct vn_vq *q, uint16_t head, uint32_t len)
{
    uint32_t elem[2];

    elem[0] = head;
    elem[1] = len;
    if (vn_mem(n, q->used + 4u + 8u * (uint32_t)(q->used_idx % q->num), 1, elem, NULL, 8) != 0) {
        return -EFAULT;
    }
    sched_mb(); /* the ring in program order */
    q->used_idx++;
    return vn_put16(n, q->used + 2, q->used_idx);
}

/** Raise the ring's interrupt unless the guest asked for none. */
static void vn_notify(struct vnet *n, struct vn_vq *q)
{
    uint16_t flags = 0;

    sched_mb(); /* the used index, then the guest's flags */
    if (vn_get16(n, q->avail, &flags) == 0 && !(flags & AVAIL_NO_INTR) && q->call) {
        eventfd_signal(q->call);
        vn_counts[2]++;
    }
}

/** The next chain the guest offers on `q`: 1 with its head in `*head`, 0
 * when the ring is empty, after asking for a kick and looking again, as
 * the guest may have added one in between; -EFAULT. */
static int vn_next(struct vnet *n, struct vn_vq *q, uint16_t *head)
{
    uint16_t idx;

    if (vn_get16(n, q->avail + 2, &idx) != 0) {
        return -EFAULT;
    }
    if (idx == q->last_avail) {
        vn_quiet(n, q, 0);
        sched_mb(); /* the flag, then the index */
        if (vn_get16(n, q->avail + 2, &idx) != 0) {
            return -EFAULT;
        }
        if (idx == q->last_avail) {
            return 0;
        }
    }
    sched_mb(); /* the ring in program order */
    if (vn_get16(n, q->avail + 4u + 2u * (uint32_t)(q->last_avail % q->num), head) != 0) {
        return -EFAULT;
    }
    if (*head >= q->num) {
        return -EFAULT;
    }
    q->last_avail++;
    if ((uint16_t)(idx - q->last_avail) != 0) {
        vn_quiet(n, q, 1);
    }
    return 1;
}

/* A descriptor, as the guest lays it out. */
struct vn_desc {
    uint64_t addr;
    uint32_t len;
    uint16_t flags, next;
};

/** Read descriptor `i` of `q`: 0, or -EFAULT for one past the table, an
 * indirect one, which the features do not offer, or one not in memory. */
static int vn_desc(struct vnet *n, struct vn_vq *q, uint32_t i, struct vn_desc *d)
{
    if (i >= q->num || vn_mem(n, q->desc + 16u * i, 1, NULL, d, 16) != 0 ||
        (d->flags & DESC_INDIRECT)) {
        return -EFAULT;
    }
    return 0;
}

/**
 * The tap's `rx`: write the frame of `len` bytes, after a zero header, into
 * the next buffer the guest offers on its receive ring, and raise the ring's
 * interrupt. 1 when the frame was taken, also when it was dropped for a
 * buffer too short, whose chain goes back empty; 0 when the ring has no
 * buffer or does not move frames.
 */
/** Split the time from the request to a frame from the guest, at `now`, at
 * the device interrupt that came last before it: count the frames that had
 * one, the cycles from the request to that interrupt, and from it to the
 * frame, at `base`, `base` + 1 and `base` + 2. */
static void vn_msi_split(uint64_t now, int base)
{
    uint64_t at = vn_msi_at;

    if (at && at >= vn_req_at && at <= now) {
        vn_counts[base]++;
        vn_counts[base + 1] += at - vn_req_at;
        vn_counts[base + 2] += now - at;
    }
}

void vhost_net_note_msi(void)
{
    int cpu = arch_cpu_id();

    if (cpu < VN_CPUS) {
        vn_counts[VN_MSI_CPU0 + cpu]++;
    }
    if (vn_req_at) {
        vn_counts[VN_MSIS_IN_EXCHANGE]++;
        vn_msi_at = arch_cycles();
        if (!vn_msi_seen) {
            vn_msi_seen = 1;
            vn_counts[VN_C_MSIS]++;
            vn_counts[VN_C_MSIS + 1] += vn_msi_at - vn_req_at;
        }
    }
}

/**
 * Time a request-response exchange by its frames: a TCP segment with data
 * to the guest is a request, the first pure acknowledgement from the guest
 * after it is the peer's acknowledgement, the next segment with data from
 * the guest is the reply, and the next request ends Rach2's turn. Adds the
 * cycles from the request to the acknowledgement and to the reply, and from
 * the reply to the next request, with their counts; and the processor each
 * request is sent from, and whether it is the one the guest last rang its
 * doorbell from. `to_guest` is 1 for a frame put on a receive ring. Other
 * frames count nothing.
 */
static void vn_trace(const uint8_t *f, uint32_t len, int to_guest)
{
    uint32_t ihl, total, doff, data;
    uint64_t now;
    const uint8_t *t;
    int cpu;

    if (len < ETH_HDR_SIZE + 20u || f[12] != 0x08 || f[13] != 0x00 || f[23] != 6u) {
        return;
    }
    ihl = (f[14] & 15u) * 4u;
    total = (uint32_t)f[16] << 8 | f[17];
    if (ihl < 20u || len < ETH_HDR_SIZE + ihl + 20u || total < ihl + 20u) {
        return;
    }
    t = f + ETH_HDR_SIZE + ihl;
    doff = (t[12] >> 4) * 4u;
    if (doff < 20u || total < ihl + doff || (t[13] & 0x07u)) {
        return; /* SYN, FIN and RST are no part of an exchange */
    }
    data = total - ihl - doff;
    now = arch_cycles();
    if (to_guest) {
        if (!data) {
            return;
        }
        if (vn_reply_at) {
            vn_counts[VN_TR_TURNS]++;
            vn_counts[VN_TR_TURN_CYCLES] += now - vn_reply_at;
            vn_reply_at = 0;
        }
        vn_counts[VN_TR_REQS]++;
        cpu = arch_cpu_id();
        if (cpu < VN_CPUS) {
            vn_counts[VN_REQ_CPU0 + cpu]++;
        }
        if (cpu == vn_kick_cpu) {
            vn_counts[VN_REQ_ON_GUEST]++;
        }
        vn_req_at = now;
        vn_req_acked = 0;
        vn_msi_at = 0;
        vn_msi_seen = 0;
    } else if (!data) {
        if (vn_req_at && !vn_req_acked) {
            vn_counts[VN_TR_ACKS]++;
            vn_counts[VN_TR_ACK_CYCLES] += now - vn_req_at;
            vn_req_acked = 1;
            vn_msi_split(now, VN_A_MSIS);
        }
    } else {
        if (vn_req_at) {
            vn_counts[VN_TR_REPLIES]++;
            vn_counts[VN_TR_REPLY_CYCLES] += now - vn_req_at;
            vn_msi_split(now, VN_B_MSIS);
            vn_req_at = 0;
        }
        vn_reply_at = now;
    }
}

/**
 * Put one frame from the tap device into the guest's receive ring.
 *
 * Runs in the sender's context.  Returns 1 when the frame was queued and 0
 * when it could not be queued: the ring not ready or out of room.
 */
static int vn_rx(struct tap_backend *b, const uint8_t *frame, uint16_t len)
{
    /* No offload, and num_buffers 1 in the 1.0 header, as virtio 1.0
     * 5.1.6.3.1 asks of a device without VIRTIO_NET_F_MRG_RXBUF. */
    static const uint8_t vhdr[VN_HDR_MAX] = {0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 1, 0};
    struct vnet *n = (struct vnet *)b;
    struct vn_vq *q = &n->vq[VN_RX];
    uint32_t total = n->hdr + len, off = 0, steps = 0, i;
    struct vn_desc d;
    uint16_t head;
    int r;

    if (!vn_ready(n, q) || vn_used_init(n, q) != 0) {
        return 0;
    }
    r = vn_next(n, q, &head);
    if (r <= 0) {
        if (r < 0) {
            vn_break(n, q);
        }
        vn_counts[1]++;
        return 0;
    }
    i = head;
    for (;;) {
        uint32_t doff = 0;

        if (++steps > q->num || vn_desc(n, q, i, &d) != 0 || !(d.flags & DESC_WRITE)) {
            vn_break(n, q);
            return 0;
        }
        while (doff < d.len && off < total) {
            uint32_t take = d.len - doff;
            const uint8_t *src;

            if (take > total - off) {
                take = total - off;
            }
            if (off < n->hdr) {
                if (take > n->hdr - off) {
                    take = n->hdr - off;
                }
                src = vhdr + off;
            } else {
                src = frame + (off - n->hdr);
            }
            if (vn_mem(n, d.addr + doff, 0, src, NULL, take) != 0) {
                vn_break(n, q);
                return 0;
            }
            doff += take;
            off += take;
        }
        if (off == total || !(d.flags & DESC_NEXT)) {
            break;
        }
        i = d.next;
    }
    if (vn_used(n, q, head, off == total ? total : 0) != 0) {
        vn_break(n, q);
        return 0;
    }
    vn_counts[0]++;
    vn_trace(frame, len, 1);
    vn_notify(n, q);
    return 1;
}

/**
 * Take what the guest put on its transmit ring, at most VN_BUDGET frames,
 * and give each frame to the tap as a write would; then raise the ring's
 * interrupt. A frame shorter than an Ethernet header or longer than the
 * interface carries is dropped. The kernel thread calls it, under the big
 * lock.
 */
static void vn_tx(struct vnet *n)
{
    struct vn_vq *q = &n->vq[VN_TX];
    uint32_t budget = VN_BUDGET, done = 0;
    struct vn_desc d;
    uint16_t head;
    int r;

    if (!vn_ready(n, q) || vn_used_init(n, q) != 0) {
        return;
    }
    vn_quiet(n, q, 1);
    tcp_batch_begin();
    while (budget-- != 0 && n->tap) {
        uint32_t len = 0, steps = 0, i;
        int over = 0;

        r = vn_next(n, q, &head);
        if (r <= 0) {
            if (r < 0) {
                vn_break(n, q);
            }
            break;
        }
        for (i = head;;) {
            if (++steps > q->num || vn_desc(n, q, i, &d) != 0 || (d.flags & DESC_WRITE)) {
                goto broken;
            }
            if (d.len > VN_TXBUF - len) {
                over = 1;
            } else if (vn_mem(n, d.addr, 0, NULL, n->txbuf + len, d.len) != 0) {
                goto broken;
            } else {
                len += d.len;
            }
            if (!(d.flags & DESC_NEXT)) {
                break;
            }
            i = d.next;
        }
        if (!over && len >= n->hdr + ETH_HDR_SIZE) {
            vn_trace(n->txbuf + n->hdr, len - n->hdr, 0);
            (void)tap_inject(n->tap, n->txbuf + n->hdr, len - n->hdr);
        }
        if (vn_used(n, q, head, 0) != 0) {
            goto broken;
        }
        done++;
    }
    tcp_batch_end();
    vn_counts[4] += done;
    if (budget == (uint32_t)-1) {
        /* The budget ran out with frames left: come back after a yield. */
        q->kicked = 1;
        vn_work = 1;
    }
    if (done) {
        vn_notify(n, q);
    }
    return;
broken:
    tcp_batch_end();
    vn_break(n, q);
}

/** Note the processor and the time of a kick that will wake the thread, and
 * count kicks by processor. Interrupts are off. */
static void vn_place_kick(void)
{
    int cpu = arch_cpu_id();

    if (cpu < VN_CPUS) {
        vn_counts[VN_KICK_CPU0 + cpu]++;
    }
    if (!vn_kick_at) {
        vn_kick_cpu = cpu;
        vn_kick_at = arch_cycles();
    }
}

/** The core siblings of processor `cpu`, cached. */
static uint64_t vn_siblings(int cpu)
{
    if (!vn_core[cpu]) {
        vn_core[cpu] = cpuinfo_siblings(cpu, CPUINFO_CORE);
    }
    return vn_core[cpu];
}

/** Count where and how soon the thread ran after the kick that woke it:
 * the cycles from the kick, and whether it runs on the kicking processor or
 * on another thread of its core. */
static void vn_place_run(void)
{
    uint64_t at = vn_kick_at;
    int cpu = arch_cpu_id(), from = vn_kick_cpu;

    if (cpu < VN_CPUS) {
        vn_counts[VN_RUN_CPU0 + cpu]++;
    }
    if (!at) {
        return;
    }
    vn_kick_at = 0;
    vn_counts[VN_WAKES]++;
    vn_counts[VN_WAKE_CYCLES] += arch_cycles() - at;
    if (cpu == from) {
        vn_counts[VN_WAKE_SAME]++;
    } else if (cpu < VN_CPUS && from >= 0 && from < VN_CPUS && (vn_siblings(from) >> cpu & 1u)) {
        vn_counts[VN_WAKE_SIBLING]++;
    }
}

/** The kick's hook: the thread looks at the ring, or, under `vn_inline`, a
 * transmit ring is emptied here when the big lock is free. Called with the
 * eventfd's lock held and interrupts off, from the guest's doorbell (a KVM
 * ioeventfd, without the big lock) or a write by QEMU (with it). The
 * network stack runs with interrupts off in any case, and takes no
 * eventfd's lock that would be taken before this one. */
static int vn_kick(struct eventfd_hook *h)
{
    struct vn_vq *q = (struct vn_vq *)h;

    vn_counts[3]++;
    if (vn_inline && q == &q->n->vq[VN_TX] && bkl_try_enter()) {
        /* The guest's processor thread, between its exit and its next
         * entry; the big lock keeps the thread off the ring meanwhile. */
        vn_tx(q->n);
        bkl_exit();
        if (!vn_work) {
            return 1;
        }
    } else {
        q->kicked = 1;
        vn_work = 1;
    }
    vn_place_kick();
    wq_wake(&vn_wq, 0);
    return 1;
}

/** The kick's eventfd closed for the last time: the ring has none. Under
 * the big lock. */
static void vn_kick_hangup(struct eventfd_hook *h)
{
    struct vn_vq *q = (struct vn_vq *)h;

    if (q->kick) {
        eventfd_put(q->kick);
        q->kick = 0;
    }
}

/** Take the ring's kick away, its hook and its reference. */
static void vn_kick_drop(struct vn_vq *q)
{
    if (q->kick && eventfd_hook_remove(q->kick, &q->hook, NULL)) {
        eventfd_put(q->kick);
    }
    q->kick = 0;
}

/** The thread's wait condition: a kick came. */
static int vn_has_work(void *arg)
{
    (void)arg;
    return vn_work != 0;
}

/** The kernel thread: wait for a kick, then take each kicked transmit
 * ring and offer each kicked receive ring the frames waiting in the tap. */
static void vn_main(void)
{
    uint64_t t0;
    uint32_t i;

    arch_irq_enable();
    for (;;) {
        sched_service_point();
        (void)wq_wait_event_idle(&vn_wq, vn_has_work, NULL);
        vn_place_run();
        t0 = arch_cycles();
        bkl_enter();
        vn_counts[VN_BKL_CYCLES] += arch_cycles() - t0;
        vn_work = 0;
        sched_mb(); /* the flag, then the rings' */
        for (i = 0; i < VN_MAX; i++) {
            struct vnet *n = vn_devs[i];

            if (!n) {
                continue;
            }
            if (n->vq[VN_TX].kicked) {
                n->vq[VN_TX].kicked = 0;
                vn_tx(n);
            }
            if (n->vq[VN_RX].kicked) {
                n->vq[VN_RX].kicked = 0;
                if (n->tap) {
                    tap_backend_drain(n->tap);
                }
            }
        }
        bkl_exit();
    }
}

/** The tap's `gone`: its descriptor closed, so the rings have no tap. */
static void vn_gone(struct tap_backend *b)
{
    struct vnet *n = (struct vnet *)b;

    n->tap = 0;
    n->vq[VN_RX].backend = 0;
    n->vq[VN_TX].backend = 0;
}

/** A pin's holder: an unmap took the guest's pages back; no ring touches
 * them again. Under the big lock. */
static void vn_release(struct pin *pin, void *owner)
{
    struct vnet *n = (struct vnet *)owner;

    (void)pin;
    if (!n->dead) {
        klog(KLOG_WARN, "vhost-net", "stopped: the guest's memory was unmapped");
    }
    n->dead = 1;
}

/** Unpin the first `count` regions of `reg`. */
static void vn_unpin(struct vn_region *reg, uint32_t count)
{
    uint32_t i;

    for (i = 0; i < count; i++) {
        unpin(reg[i].pin);
        reg[i].pin = 0;
    }
}

/** VHOST_SET_MEM_TABLE: pin each region of the table at `arg`, then let the
 * old table go. */
static int32_t vn_set_mem(struct vnet *n, uintptr_t arg)
{
    struct vn_region *reg;
    uint32_t head[2], i, pinned;
    uint64_t r4[4];

    if (copy_from_user(head, (const void __user *)arg, sizeof(head)) != 0) {
        return -EFAULT;
    }
    if (head[0] > VN_REGIONS) {
        return -E2BIG;
    }
    reg = (struct vn_region *)kzalloc(VN_REGIONS * sizeof(*reg));
    if (!reg) {
        return -ENOMEM;
    }
    for (i = 0; i < head[0]; i++) {
        if (copy_from_user(r4, (const void __user *)(arg + 8u + 32u * i), sizeof(r4)) != 0) {
            vn_unpin(reg, i);
            kfree(reg);
            return -EFAULT;
        }
        reg[i].gpa = r4[0];
        reg[i].size = r4[1];
        reg[i].uaddr = r4[2];
        if (r4[1] == 0 || (r4[2] & 4095u) ||
            pin_user(n->mm, (uintptr_t)r4[2], (uintptr_t)r4[1], PIN_WRITE | PIN_LONGTERM,
                     &n->holder, &reg[i].pin) != 0) {
            reg[i].pin = 0;
        }
    }
    for (i = 0, pinned = 0; i < head[0]; i++) {
        pinned += reg[i].pin != 0;
    }
    klog(KLOG_INFO, "vhost-net", "table: %u regions, %u pinned", (unsigned)head[0],
         (unsigned)pinned);
    vn_unpin(n->reg, n->nreg);
    k_memcpy(n->reg, reg, VN_REGIONS * sizeof(*reg));
    n->nreg = head[0];
    n->last = 0;
    n->dead = 0;
    kfree(reg);
    return 0;
}

/** Let go of the tap when neither ring has it. */
static void vn_detach(struct vnet *n)
{
    if (n->tap && !n->vq[VN_RX].backend && !n->vq[VN_TX].backend) {
        (void)tap_backend_set(n->tap, 0);
        n->tap = 0;
    }
}

/** VHOST_NET_SET_BACKEND: give ring `index` the tap of descriptor `fd`, or
 * take it away with -1. */
static int32_t vn_set_backend(struct vnet *n, uint32_t index, int32_t fd)
{
    struct vn_vq *q = &n->vq[index];
    struct tap *t;
    int32_t r;

    if (fd == -1) {
        q->backend = 0;
        vn_detach(n);
        return 0;
    }
    t = tap_of(current_proc ? fd_get(current_proc->fds, fd) : 0);
    if (!t) {
        return -ENOTSOCK;
    }
    if (n->tap && n->tap != t) {
        return -EBUSY;
    }
    r = tap_backend_set(t, &n->tb);
    if (r != 0) {
        return r;
    }
    n->tap = t;
    q->backend = 1;
    q->broken = 0;
    q->used_known = 0;
    q->no_notify = 0;
    q->kicked = 1;
    vn_work = 1;
    wq_wake(&vn_wq, 0);
    return 0;
}

/** Replace the eventfd `*slot` with that of descriptor `fd`, or none with
 * -1. */
static int32_t vn_set_fd(struct eventfd **slot, int32_t fd)
{
    struct eventfd *ev = 0;
    int32_t err = 0;

    if (fd != -1) {
        ev = eventfd_get(fd, &err);
        if (!ev) {
            return err;
        }
    }
    if (*slot) {
        eventfd_put(*slot);
    }
    *slot = ev;
    return 0;
}

/** VHOST_SET_VRING_KICK: hook the eventfd of `fd`, or none with -1. */
static int32_t vn_set_kick(struct vn_vq *q, int32_t fd)
{
    struct eventfd *ev;
    int32_t err = 0;

    vn_kick_drop(q);
    if (fd == -1) {
        return 0;
    }
    ev = eventfd_get(fd, &err);
    if (!ev) {
        return err;
    }
    q->kick = ev;
    q->hook.signal = vn_kick;
    q->hook.hangup = vn_kick_hangup;
    q->hook.next = 0;
    if (eventfd_hook_add(ev, &q->hook) == 1) {
        q->kicked = 1;
        vn_work = 1;
        wq_wake(&vn_wq, 0);
    }
    return 0;
}

/** Stop every ring and let go of the tap, the eventfds and the pins. */
static void vn_reset(struct vnet *n)
{
    int i;

    n->vq[VN_RX].backend = 0;
    n->vq[VN_TX].backend = 0;
    vn_detach(n);
    for (i = 0; i < 2; i++) {
        struct vn_vq *q = &n->vq[i];

        vn_kick_drop(q);
        (void)vn_set_fd(&q->call, -1);
        (void)vn_set_fd(&q->err, -1);
        k_memset(q, 0, sizeof(*q));
        q->n = n;
    }
    vn_unpin(n->reg, n->nreg);
    n->nreg = 0;
    n->mm = 0;
    n->features = 0;
    n->hdr = 0;
    n->dead = 0;
}

/** VHOST_SET_VRING_NUM, VHOST_SET_VRING_BASE and VHOST_GET_VRING_BASE, on
 * the ring state at `arg`. */
static int32_t vn_ring_state(struct vnet *n, uint32_t req, uintptr_t arg)
{
    uint32_t st[2];

    if (copy_from_user(st, (const void __user *)arg, sizeof(st)) != 0) {
        return -EFAULT;
    }
    if (st[0] > 1) {
        return -EINVAL;
    }
    if (req == VHOST_SET_VRING_NUM) {
        if (st[1] == 0 || st[1] > 32768u || (st[1] & (st[1] - 1u))) {
            return -EINVAL;
        }
        n->vq[st[0]].num = st[1];
        return 0;
    }
    if (req == VHOST_SET_VRING_BASE) {
        n->vq[st[0]].last_avail = (uint16_t)st[1];
        n->vq[st[0]].used_known = 0;
        return 0;
    }
    st[1] = n->vq[st[0]].last_avail;
    return copy_to_user((void __user *)arg, st, sizeof(st)) != 0 ? -EFAULT : 0;
}

/** VHOST_SET_VRING_ADDR: the index and flags, then the descriptor table,
 * the used ring, the available ring and the log's address, which no log
 * uses. */
static int32_t vn_ring_addr(struct vnet *n, uintptr_t arg)
{
    uint64_t addr[5];
    uint32_t st[2];

    if (copy_from_user(addr, (const void __user *)arg, sizeof(addr)) != 0) {
        return -EFAULT;
    }
    k_memcpy(st, &addr[0], sizeof(st));
    if (st[0] > 1 || (addr[1] & 15u) || (addr[2] & 3u) || (addr[3] & 1u)) {
        return -EINVAL;
    }
    n->vq[st[0]].desc = addr[1];
    n->vq[st[0]].used = addr[2];
    n->vq[st[0]].avail = addr[3];
    n->vq[st[0]].addr_set = 1;
    return 0;
}

/** VHOST_SET_VRING_KICK, _CALL, _ERR and VHOST_NET_SET_BACKEND, on the
 * ring file at `arg`. */
static int32_t vn_ring_file(struct vnet *n, uint32_t req, uintptr_t arg)
{
    uint32_t st[2];

    if (copy_from_user(st, (const void __user *)arg, sizeof(st)) != 0) {
        return -EFAULT;
    }
    if (st[0] > 1) {
        return -EINVAL;
    }
    if (req == VHOST_SET_VRING_KICK) {
        return vn_set_kick(&n->vq[st[0]], (int32_t)st[1]);
    }
    if (req == VHOST_SET_VRING_CALL) {
        klog(KLOG_INFO, "vhost-net", "ring %u: call fd %d", st[0], (int32_t)st[1]);
        return vn_set_fd(&n->vq[st[0]].call, (int32_t)st[1]);
    }
    if (req == VHOST_SET_VRING_ERR) {
        return vn_set_fd(&n->vq[st[0]].err, (int32_t)st[1]);
    }
    return vn_set_backend(n, st[0], (int32_t)st[1]);
}

/** The requests an open answers before VHOST_SET_OWNER: 1 for another. */
static int32_t vn_ioctl_any(struct vnet *n, uint32_t req, uintptr_t arg)
{
    uint64_t v = 0;

    switch (req) {
    case VHOST_SET_OWNER:
        if (n->mm) {
            return -EBUSY;
        }
        n->mm = current_proc ? current_proc->mm : 0;
        return n->mm ? 0 : -EINVAL;
    case VHOST_GET_FEATURES:
        v = VN_FEATURES;
        return copy_to_user((void __user *)arg, &v, sizeof(v)) != 0 ? -EFAULT : 0;
    case VHOST_GET_BACKEND_FEATURES:
        return copy_to_user((void __user *)arg, &v, sizeof(v)) != 0 ? -EFAULT : 0;
    case VHOST_SET_BACKEND_FEATURES:
        if (copy_from_user(&v, (const void __user *)arg, sizeof(v)) != 0) {
            return -EFAULT;
        }
        return v == 0 ? 0 : -EOPNOTSUPP;
    default:
        return 1;
    }
}

/**
 * The descriptor's control, Linux's vhost-net ioctls: -EINVAL for a ring
 * index past the two or a ring size that is no power of two up to 32768,
 * -EOPNOTSUPP for a feature not offered, -E2BIG for more regions than
 * VN_REGIONS, -ENOTSOCK for a backend that is no tap, -EBUSY for a second
 * owner or a tap another backend has, -EPERM before VHOST_SET_OWNER, and
 * -ENOTTY for another request.
 */
static int32_t vn_ioctl(fd_entry_t *e, uint32_t req, uintptr_t arg)
{
    struct vnet *n = (struct vnet *)e->priv;
    uint64_t v;
    int32_t r;

    if (!n) {
        return -EBADFD;
    }
    r = vn_ioctl_any(n, req, arg);
    if (r != 1) {
        return r;
    }
    if (!n->mm) {
        return -EPERM;
    }
    switch (req) {
    case VHOST_RESET_OWNER:
        vn_reset(n);
        return 0;
    case VHOST_SET_FEATURES:
        if (copy_from_user(&v, (const void __user *)arg, sizeof(v)) != 0) {
            return -EFAULT;
        }
        if ((v & ~VN_FEATURES) || !(v & F_NET_HDR)) {
            return -EOPNOTSUPP;
        }
        n->features = v;
        n->hdr = (v & (F_VERSION_1 | F_MRG_RXBUF)) ? 12u : 10u;
        return 0;
    case VHOST_SET_MEM_TABLE:
        return vn_set_mem(n, arg);
    case VHOST_SET_VRING_NUM:
    case VHOST_SET_VRING_BASE:
    case VHOST_GET_VRING_BASE:
        return vn_ring_state(n, req, arg);
    case VHOST_SET_VRING_ADDR:
        return vn_ring_addr(n, arg);
    case VHOST_SET_VRING_KICK:
    case VHOST_SET_VRING_CALL:
    case VHOST_SET_VRING_ERR:
    case VHOST_NET_SET_BACKEND:
        return vn_ring_file(n, req, arg);
    default:
        return -ENOTTY;
    }
}

/** An open: a device of its own, in a free slot; -ENFILE with none, or
 * -ENOMEM. */
static int32_t vn_open(struct devfs_node *node, struct fd_entry *e, int flags)
{
    struct vnet *n;
    int i;

    (void)node;
    (void)flags;
    i = 0;
    while (i < VN_MAX && vn_devs[i]) {
        i++;
    }
    if (i == VN_MAX) {
        return -ENFILE;
    }
    n = (struct vnet *)kzalloc(sizeof(*n));
    if (!n) {
        return -ENOMEM;
    }
    n->tb.rx = vn_rx;
    n->tb.gone = vn_gone;
    n->slot = i;
    n->holder.release = vn_release;
    n->holder.owner = n;
    n->holder.flags = 0;
    n->vq[VN_RX].n = n;
    n->vq[VN_TX].n = n;
    vn_devs[i] = n;
    e->priv = n;
    return 0;
}

/** The last close: the device stops and goes. */
static void vn_close(fd_entry_t *e)
{
    struct vnet *n = (struct vnet *)e->priv;

    if (!n) {
        return;
    }
    e->priv = 0;
    vn_reset(n);
    vn_devs[n->slot] = 0;
    kfree(n);
}

static const struct file_ops vn_ops = {NULL, NULL, NULL, vn_ioctl, vn_close, NULL, NULL, 0};

/* Linux's numbers for the device, 10:238, read and written by root. */
static struct devfs_node vn_node = {"vhost-net", 10, 238, 0600, &vn_ops, vn_open, NULL};

void vhost_net_counts(uint64_t *c)
{
    int i;

    for (i = 0; i < VHOST_NET_COUNTS; i++) {
        c[i] = vn_counts[i];
    }
}

void vhost_net_start(void)
{
    uintptr_t base;
    uint32_t flags;
    int32_t r = devfs_node_register(&vn_node);

    if (r != 0) {
        klog(KLOG_WARN, "vhost-net", "/dev/vhost-net not made: error %d", r);
        return;
    }
    flags = kirq_save();
    base = kstack_alloc();
    vn_thread = base ? alloc_process() : NULL;
    if (!vn_thread) {
        if (base) {
            kstack_free(base + KERNEL_STACK_SIZE);
        }
        kirq_restore(flags);
        klog(KLOG_WARN, "vhost-net", "failed: no kernel thread");
        return;
    }
    vn_thread->kernel_stack = base + KERNEL_STACK_SIZE;
    vn_thread->space_root = 0;
    vn_thread->page_dir = NULL;
    vn_thread->parent_pid = 0;
    /* Real-time, so that a doorbell rung from a guest's processor thread
     * has the thread run at once, there or on an idle processor: the vCPU's
     * loop gives way to no ordinary thread. */
    vn_thread->sched_policy = SCHED_FIFO;
    vn_thread->rt_priority = 50;
    proc_set_comm(vn_thread, "vhost-net");
    setup_process_stack(vn_thread, vn_main);
    sched_make_ready(vn_thread);
    kirq_restore(flags);
    if (debugfs_create_text("net/vhost_inline", 0644u, &tcp_switch_ops, &vn_inline) != 0 ||
        debugfs_create_text("sched/idle_poll", 0644u, &tcp_switch_ops, &arch_idle_poll_force) !=
            0) {
        klog(KLOG_WARN, "vhost-net", "failed: net/vhost_inline or sched/idle_poll not registered");
    }
}
