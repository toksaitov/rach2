/* vhost_net.c: /dev/vhost-net, as vhost_net.h describes it: the guest's
 * memory, its two rings, the frames between them and a tap interface, and
 * the kernel thread that takes the guest's transmit ring. */
#include "vhost_net.h"
#include "tap.h"
#include "../ethernet/ethernet.h"
#include "../tcp/tcp.h"
#include "../../fs/devfs.h"
#include "../../fs/fd.h"
#include "../../fs/file.h"
#include "../../fs/krffs.h"
#include "../../io/eventfd.h"
#include "../../klog.h"
#include "../../lib/kerrno.h"
#include "../../lib/kirq.h"
#include "../../lib/kstring.h"
#include "../../lib/spinlock.h"
#include "../../mm/kheap.h"
#include "../../mm/paging.h"
#include "../../mm/pin.h"
#include "../../mm/usercopy.h"
#include "../../proc/mutex.h"
#include "../../proc/process.h"
#include "../../proc/sched.h"
#include "../../proc/smp.h"
#include "../../proc/wait_queue.h"
#include "../../syscall_internal.h"

/* The split ring's flags (virtio 1.0 2.6.5, 2.6.6, 2.6.8). */
#define DESC_NEXT      1u
#define DESC_WRITE     2u
#define DESC_INDIRECT  4u
#define USED_NO_NOTIFY 1u
#define AVAIL_NO_INTR  1u

/* VHOST_SET_VRING_ADDR's one flag, a log of the used ring's writes. */
#define VRING_F_LOG 1u

#define VN_RX      0
#define VN_TX      1
#define VN_HDR_MAX 12   /* struct virtio_net_hdr_mrg_rxbuf */
#define VN_BUDGET  128u /* frames of a transmit ring a pass takes */
#define VN_TXBUF   (VN_HDR_MAX + TAP_PI + ETH_HDR_SIZE + ETH_MTU)
#define VN_UNBIND  (-1) /* VHOST_FILE_UNBIND */

/* One region of the guest's memory, as VHOST_SET_MEM_TABLE gives it, with
 * the pin that follows its pages (PIN_FOLLOW), which holds each page the
 * device has touched until the page changes. */
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
    /* The kick's descriptor, held as Linux's vhost holds the kick's file, so
     * that its last close by the program leaves the ring its kick. */
    fd_entry_t kickfile;
    int has_kickfile;
    uint32_t busyloop; /* VHOST_SET_VRING_BUSYLOOP_TIMEOUT's, kept, unused */
    /* A page the ring met that its region's pin does not hold, which the
     * thread brings in (`vn_refill`): `want` set, the region and the page
     * of its pin, and the table's `gen` they were read under. Written under
     * `lock` for the receive ring and the big lock for the transmit ring,
     * read by the thread under both. */
    int want;
    uint32_t want_reg, want_page, want_gen;
};

/*
 * One open of /dev/vhost-net. The receive path runs in the context of
 * whoever sends on the tap, an interrupt handler among them, without the
 * big lock, so `lock` is over everything it reads: the regions, `dead`,
 * `hdr`, `features`, `tap` and the receive ring. A change of those is made
 * under the big lock and `lock`; the transmit ring and the thread, under
 * the big lock alone, read them without `lock`. A pin's change of a page
 * runs under the big lock and takes `lock` (`vn_forget`), so it waits for
 * both.
 */
struct vnet {
    struct tap_backend tb; /* first: the tap's */
    struct vnet *next;     /* on `vn_devs` */
    struct spinlock lock;
    /* Held across each request but the two that read the features, and
     * across the last close, as Linux's `dev->mutex`, so that a request
     * that sleeps, in a pin, a release or a wait for the thread's refill,
     * lets no other request of the device run meanwhile. */
    struct mutex mutex;
    struct mm *mm; /* the owner's, VHOST_SET_OWNER; compared, never used */
    uint64_t features;
    uint32_t hdr;          /* bytes of the virtio header before each frame */
    struct vn_region *reg; /* VHOST_NET_REGIONS of them, or NULL */
    uint32_t nreg;
    volatile uint32_t last; /* the region the last lookup found */
    struct pin_holder holder;
    int dead;           /* an unmap took a pin back: no more access */
    uint32_t gen;       /* changes with each table and reset */
    uint32_t refilling; /* the thread's `pin_refill` of a region sleeps */
    struct vn_vq vq[2];
    struct tap *tap;
    /* The tap's descriptor while `tap` is set, held as Linux's vhost holds
     * the tap's file, so that its last close by the program leaves the
     * rings their tap. */
    fd_entry_t tapfile;
    int has_tapfile;
    uint32_t to_guest, from_guest; /* frames moved since the tap came */
    uint8_t txbuf[VN_TXBUF];
};

/* Every open device, under the big lock. */
static struct vnet *vn_devs;
static wait_queue_t vn_wq;        /* the thread sleeps here */
static volatile uint32_t vn_work; /* a kick came since the thread looked */
static process_t *vn_thread;
static wait_queue_t vn_idle_wq; /* a table's change waits for a refill */
/* The attachments that have ended and their frames, under the big lock. */
static uint32_t vn_detaches;
static uint64_t vn_detached_to, vn_detached_from;

/** The region that holds `a`, a guest-physical address, or QEMU's with
 * `va`; NULL for none. */
static struct vn_region *vn_find(struct vnet *n, uint64_t a, int va)
{
    struct vn_region *r;
    uint32_t i = n->last;

    if (i < n->nreg) {
        r = &n->reg[i];
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

/** Note a kick of `q` for the thread and wake it. */
static void vn_wake(struct vn_vq *q)
{
    q->kicked = 1;
    vn_work = 1;
    wq_wake(&vn_wq, 0);
}

/**
 * The kernel's map of the page of the guest's memory that holds `a`, by
 * guest address or QEMU's with `va`, in `*m` with kmap_cpu's flags in
 * `*fl`, and the bytes of the page from `a` on, at most `len` and no more
 * than its region has: that count, or -EFAULT for an address in no region
 * or after the memory went, or -EAGAIN for a page the region's pin does not
 * hold now, which is noted on `q` for the thread, woken to bring it in.
 * Any context; does not sleep. The caller unmaps `*m` with kunmap_cpu.
 */
static int32_t vn_map(struct vn_vq *q, uint64_t a, int va, uint32_t len, uint8_t **m, uint32_t *fl)
{
    struct vnet *n = q->n;
    struct vn_region *r = vn_find(n, a, va);
    uint64_t off, ua;
    uintptr_t phys;
    uint32_t chunk, page;

    if (!r || !r->pin || n->dead || !pin_live(r->pin)) {
        return -EFAULT;
    }
    off = a - (va ? r->uaddr : r->gpa);
    ua = r->uaddr + off;
    page = (uint32_t)((ua >> 12) - (r->uaddr >> 12));
    phys = pin_phys(r->pin, page);
    if (phys == 0) {
        if (!q->want) {
            q->want_reg = (uint32_t)(r - n->reg);
            q->want_page = page;
            q->want_gen = n->gen;
            q->want = 1;
        }
        vn_wake(q);
        return -EAGAIN;
    }
    chunk = 4096u - (uint32_t)(ua & 4095u);
    if (chunk > len) {
        chunk = len;
    }
    if ((uint64_t)chunk > r->size - off) {
        chunk = (uint32_t)(r->size - off);
    }
    *m = (uint8_t *)kmap_cpu(phys, fl) + (uint32_t)(ua & 4095u);
    return (int32_t)chunk;
}

/** Copy `len` bytes between the guest's memory at `a`, by guest address or
 * QEMU's with `va`, and `out`, or from `in` into the guest when `in` is not
 * NULL, for the ring `q`: 0, or vn_map's -EFAULT or -EAGAIN. Any context;
 * does not sleep. */
static int vn_mem(struct vn_vq *q, uint64_t a, int va, const void *in, void *out, uint32_t len)
{
    const uint8_t *src = (const uint8_t *)in;
    uint8_t *dst = (uint8_t *)out;

    while (len != 0) {
        uint32_t chunk, fl;
        int32_t got;
        uint8_t *m;

        got = vn_map(q, a, va, len, &m, &fl);
        if (got < 0) {
            return got;
        }
        chunk = (uint32_t)got;
        if (src) {
            k_memcpy(m, src, chunk);
            src += chunk;
        } else {
            k_memcpy(dst, m, chunk);
            dst += chunk;
        }
        kunmap_cpu(m, fl);
        a += chunk;
        len -= chunk;
    }
    return 0;
}

/**
 * Read (`out`) or write the 16-bit word of a ring at QEMU's address `va`,
 * which the rings' alignment keeps even, with one aligned access, so that
 * a guest that reads or writes an index at once on another processor sees
 * the old word or the new, never half of each: 0, or vn_map's -EFAULT or
 * -EAGAIN.
 */
static int vn_word(struct vn_vq *q, uint64_t va, uint16_t *out, uint16_t v)
{
    uint32_t fl;
    int32_t got;
    uint8_t *m;

    got = vn_map(q, va, 1, 2, &m, &fl);
    if (got < 0) {
        return got;
    }
    if (got < 2) {
        kunmap_cpu(m, fl);
        return -EFAULT; /* odd, or the region's last byte */
    }
    if (out) {
        *out = *(volatile uint16_t *)(void *)m;
    } else {
        *(volatile uint16_t *)(void *)m = v;
    }
    kunmap_cpu(m, fl);
    return 0;
}

/** Read the ring's 16-bit word at QEMU's address `va`: 0, or -EFAULT or
 * -EAGAIN. */
static int vn_get16(struct vn_vq *q, uint64_t va, uint16_t *v)
{
    return vn_word(q, va, v, 0);
}

/** Write the ring's 16-bit word at QEMU's address `va`: 0, or -EFAULT or
 * -EAGAIN. */
static int vn_put16(struct vn_vq *q, uint64_t va, uint16_t v)
{
    return vn_word(q, va, NULL, v);
}

/** Whether the ring `q` moves frames now. */
static int vn_ready(const struct vnet *n, const struct vn_vq *q)
{
    return n->tap && q->backend && q->addr_set && q->num && !q->broken && !n->dead;
}

/** Stop the ring `q` for a bad descriptor or address, logged once, and tell
 * QEMU through the error eventfd, as Linux's vq_err. */
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

/** Read the used ring's index once after a base was set: 0, or -EFAULT or
 * -EAGAIN. */
static int vn_used_init(struct vn_vq *q)
{
    int r;

    if (q->used_known) {
        return 0;
    }
    r = vn_get16(q, q->used + 2, &q->used_idx);
    if (r != 0) {
        return r;
    }
    q->used_known = 1;
    return 0;
}

/** Ask the guest for no kicks on `q` with `on`, or for kicks again: 0, or
 * -EFAULT or -EAGAIN, with the flag as it was. */
static int vn_quiet(struct vn_vq *q, int on)
{
    int r = 0;

    if (q->no_notify != on) {
        r = vn_put16(q, q->used, on ? (uint16_t)USED_NO_NOTIFY : 0);
        if (r == 0) {
            q->no_notify = on;
        }
    }
    return r;
}

/** Put the chain `head`, of which `len` bytes were written, on the used
 * ring: 0, or -EFAULT or -EAGAIN, with the index as it was. */
static int vn_used(struct vn_vq *q, uint16_t head, uint32_t len)
{
    uint32_t elem[2];
    int r;

    elem[0] = head;
    elem[1] = len;
    r = vn_mem(q, q->used + 4u + 8u * (uint32_t)(q->used_idx % q->num), 1, elem, NULL, 8);
    if (r != 0) {
        return r;
    }
    sched_mb(); /* the entry before the index */
    r = vn_put16(q, q->used + 2, (uint16_t)(q->used_idx + 1u));
    if (r == 0) {
        q->used_idx++;
    }
    return r;
}

/** Raise the ring's interrupt unless the guest asked for none, or, with
 * VIRTIO_F_NOTIFY_ON_EMPTY, whatever it asked once the available ring is
 * empty, as Linux's vhost_notify without the event index. A flag that
 * cannot be read now raises it, which costs the guest a look. */
static void vn_notify(struct vnet *n, struct vn_vq *q)
{
    uint16_t flags = 0, idx = 0;
    int r;

    sched_mb(); /* the used index, then the guest's flags */
    if (!q->call) {
        return;
    }
    r = vn_get16(q, q->avail, &flags);
    if (r == -EFAULT) {
        return;
    }
    if ((n->features & VHOST_F_NOTIFY_ON_EMPTY) && vn_get16(q, q->avail + 2, &idx) == 0 &&
        idx == q->last_avail) {
        flags = 0;
    }
    if (r != 0 || !(flags & AVAIL_NO_INTR)) {
        eventfd_signal(q->call);
    }
}

/** The next chain the guest offers on `q`: 1 with its head in `*head`, 0
 * when the ring is empty, after asking for a kick and looking again, as
 * the guest may have added one in between; -EFAULT, or -EAGAIN with
 * nothing taken. */
static int vn_next(struct vn_vq *q, uint16_t *head)
{
    uint16_t idx;
    int r;

    r = vn_get16(q, q->avail + 2, &idx);
    if (r != 0) {
        return r;
    }
    if (idx == q->last_avail) {
        r = vn_quiet(q, 0);
        if (r != 0) {
            return r;
        }
        sched_mb(); /* the flag, then the index */
        r = vn_get16(q, q->avail + 2, &idx);
        if (r != 0) {
            return r;
        }
        if (idx == q->last_avail) {
            return 0;
        }
    }
    if ((uint16_t)(idx - q->last_avail) > q->num) {
        return -EFAULT; /* more offered than the ring holds */
    }
    sched_mb(); /* the index, then the entry */
    r = vn_get16(q, q->avail + 4u + 2u * (uint32_t)(q->last_avail % q->num), head);
    if (r != 0) {
        return r;
    }
    if (*head >= q->num) {
        return -EFAULT;
    }
    q->last_avail++;
    if ((uint16_t)(idx - q->last_avail) != 0) {
        (void)vn_quiet(q, 1);
    }
    return 1;
}

/* A descriptor, as the guest lays it out. */
struct vn_desc {
    uint64_t addr;
    uint32_t len;
    uint16_t flags, next;
};

/** Read descriptor `i` of `q`: 0; -EFAULT for one past the table, an
 * indirect one, which the features do not offer, or one not in memory;
 * -EAGAIN. */
static int vn_desc(struct vn_vq *q, uint32_t i, struct vn_desc *d)
{
    int r;

    if (i >= q->num) {
        return -EFAULT;
    }
    r = vn_mem(q, q->desc + 16u * i, 1, NULL, d, 16);
    if (r != 0) {
        return r;
    }
    return (d->flags & DESC_INDIRECT) ? -EFAULT : 0;
}

/** The bytes the guest may write in the chain from `head` of `q`, the
 * receive ring's: 0 with the sum in `*room`; -EFAULT for a chain that
 * loops, holds a descriptor the device may only read or a bad one;
 * -EAGAIN. */
static int vn_room(struct vn_vq *q, uint16_t head, uint32_t *room)
{
    struct vn_desc d;
    uint32_t i = head, steps = 0, sum = 0;
    int r;

    for (;;) {
        if (++steps > q->num) {
            return -EFAULT;
        }
        r = vn_desc(q, i, &d);
        if (r != 0) {
            return r;
        }
        if (!(d.flags & DESC_WRITE)) {
            return -EFAULT;
        }
        sum = d.len >= 0x10000u - sum ? 0x10000u : sum + d.len; /* frames are shorter */
        if (!(d.flags & DESC_NEXT)) {
            break;
        }
        i = d.next;
    }
    *room = sum;
    return 0;
}

/**
 * Write the frame of `len` bytes, after the virtio header, into the chain
 * `head` of the receive ring `q`, which `vn_room` found long enough, and
 * put it on the used ring: 0; -EFAULT, also for a chain the guest changed
 * meanwhile into one too short or bad; -EAGAIN with nothing on the used
 * ring.
 */
static int vn_fill(struct vnet *n, struct vn_vq *q, uint16_t head, const uint8_t *frame,
                   uint32_t len)
{
    /* No offload, and num_buffers 1 in the 1.0 header, as virtio 1.0
     * 5.1.6.3.1 asks of a device without VIRTIO_NET_F_MRG_RXBUF. */
    static const uint8_t vhdr[VN_HDR_MAX] = {0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 1, 0};
    uint32_t total = n->hdr + len, off = 0, i = head, steps = 0;
    struct vn_desc d;
    int r;

    while (off < total) {
        uint32_t doff = 0;

        if (++steps > q->num) {
            return -EFAULT;
        }
        r = vn_desc(q, i, &d);
        if (r != 0) {
            return r;
        }
        if (!(d.flags & DESC_WRITE)) {
            return -EFAULT;
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
            r = vn_mem(q, d.addr + doff, 0, src, NULL, take);
            if (r != 0) {
                return r;
            }
            doff += take;
            off += take;
        }
        if (off < total && !(d.flags & DESC_NEXT)) {
            return -EFAULT;
        }
        i = d.next;
    }
    return vn_used(q, head, total);
}

/**
 * The tap's `rx`: write the frame of `len` bytes, after the virtio header,
 * into the next buffer the guest offers on its receive ring, and raise the
 * ring's interrupt. 1 when the frame was taken, also when it was dropped
 * for a buffer too short, which stays the guest's, as Linux's handle_rx
 * discards such a frame and gives the buffer back (vhost_discard_vq_desc);
 * 0 when the ring has no buffer or does not move frames, and the frame
 * waits in the tap. A page of the guest's memory that its pin does not hold
 * now also leaves the frame in the tap, the ring as it was, while the
 * thread brings the page in and offers the tap's frames again. Called with
 * the tap's lock held and interrupts off, in the sender's context; takes
 * `lock`.
 */
static int vn_rx(struct tap_backend *b, const uint8_t *frame, uint16_t len)
{
    struct vnet *n = (struct vnet *)b;
    struct vn_vq *q = &n->vq[VN_RX];
    uint32_t flags = spin_lock_irqsave(&n->lock), room = 0;
    uint16_t head, last;
    int r = 0;

    if (!vn_ready(n, q) || q->want || vn_used_init(q) != 0) {
        goto out;
    }
    last = q->last_avail;
    r = vn_next(q, &head);
    if (r <= 0) {
        if (r == -EFAULT) {
            vn_break(n, q);
        }
        q->last_avail = last;
        r = 0;
        goto out;
    }
    r = vn_room(q, head, &room);
    if (r == 0 && room < n->hdr + len) {
        q->last_avail--;
        r = 1;
        goto out;
    }
    if (r == 0) {
        r = vn_fill(n, q, head, frame, len);
    }
    if (r != 0) {
        if (r == -EFAULT) {
            vn_break(n, q);
        }
        q->last_avail = last;
        r = 0;
        goto out;
    }
    n->to_guest++;
    vn_notify(n, q);
    r = 1;
out:
    spin_unlock_irqrestore(&n->lock, flags);
    return r;
}

/**
 * Take what the guest put on its transmit ring, at most VN_BUDGET frames,
 * and give each frame to the tap as a write would, after its chain is on
 * the used ring; then raise the ring's interrupt. A frame shorter than the
 * header and an Ethernet header, or longer than the interface carries, is
 * dropped. A page its pin does not hold now ends the pass with the chain
 * still the ring's, for the thread to bring the page in. The kernel thread
 * calls it, under the big lock. 1 when the budget ran out with frames left.
 */
static int vn_tx(struct vnet *n)
{
    struct vn_vq *q = &n->vq[VN_TX];
    uint32_t budget = VN_BUDGET, done = 0;
    struct vn_desc d;
    uint16_t head, last;
    int r;

    if (!vn_ready(n, q) || q->want) {
        return 0;
    }
    r = vn_used_init(q);
    if (r == 0) {
        r = vn_quiet(q, 1);
    }
    while (r == 0 && budget != 0) {
        uint32_t len = 0, steps = 0, i;
        int over = 0;

        last = q->last_avail;
        r = vn_next(q, &head);
        if (r <= 0) {
            break;
        }
        r = 0;
        for (i = head; r == 0;) {
            if (++steps > q->num) {
                r = -EFAULT;
                break;
            }
            r = vn_desc(q, i, &d);
            if (r == 0 && (d.flags & DESC_WRITE)) {
                r = -EFAULT;
            }
            if (r != 0) {
                break;
            }
            if (d.len > VN_TXBUF - len) {
                over = 1;
            } else {
                r = vn_mem(q, d.addr, 0, NULL, n->txbuf + len, d.len);
                len += d.len;
            }
            if (r != 0 || !(d.flags & DESC_NEXT)) {
                break;
            }
            i = d.next;
        }
        if (r == 0) {
            r = vn_used(q, head, 0);
        }
        if (r != 0) {
            q->last_avail = last;
            break;
        }
        if (!over && len >= n->hdr + ETH_HDR_SIZE) {
            n->from_guest++;
            (void)tap_inject(n->tap, n->txbuf + n->hdr, len - n->hdr);
        }
        done++;
        budget--;
    }
    if (done) {
        vn_notify(n, q);
    }
    if (r == -EFAULT) {
        vn_break(n, q);
    }
    return budget == 0;
}

/** The kick's hook: the thread looks at the ring. Called with the
 * eventfd's lock held and interrupts off, from the guest's doorbell (a KVM
 * ioeventfd, perhaps without the big lock) or a write by QEMU, so it
 * only marks the ring and wakes the thread, as Linux's vhost_poll_wakeup
 * queues the work. */
static int vn_kick(struct eventfd_hook *h)
{
    vn_wake((struct vn_vq *)h);
    return 1;
}

/** The kick's eventfd closed for the last time: the ring has none. The
 * ring holds the kick's descriptor, so only a close of that, after the
 * hook has gone, could come here. Under the big lock. */
static void vn_kick_hangup(struct eventfd_hook *h)
{
    struct vn_vq *q = (struct vn_vq *)h;

    if (q->kick) {
        eventfd_put(q->kick);
        q->kick = 0;
    }
}

/**
 * Ask the guest for no more kicks on the receive ring while it has buffers,
 * as Linux's handle_rx keeps the ring's notifications off: a kick of it
 * matters only once the tap holds frames the ring had no room for, and
 * `vn_next` asks for kicks again when it finds the ring empty. Under the
 * big lock.
 */
static void vn_rx_quiet(struct vnet *n)
{
    struct vn_vq *q = &n->vq[VN_RX];
    uint32_t flags = spin_lock_irqsave(&n->lock);
    uint16_t idx;

    if (vn_ready(n, q) && !q->want && vn_used_init(q) == 0 && vn_quiet(q, 1) == 0) {
        sched_mb(); /* the flag, then the index */
        if (vn_get16(q, q->avail + 2, &idx) != 0 || idx == q->last_avail) {
            (void)vn_quiet(q, 0);
        }
    }
    spin_unlock_irqrestore(&n->lock, flags);
}

/**
 * Bring in the page of the guest's memory a ring of `n` wants, the first
 * ring's that does, and kick the ring to go on, or break it when the page
 * cannot be had, as Linux's copy through the owner's tables fails with
 * -EFAULT. The request is read under `lock`, which the receive path holds
 * while it writes one. A request of a table that has since been replaced,
 * or a refill during which the table changed, which `gen` tells, is let go
 * and the ring kicked to look again in the current table, never broken.
 * The refill may sleep, letting the big lock go; `refilling` keeps a
 * change of the table or a reset from letting the pin go meanwhile. 1 when
 * a request was taken, after which `n` may have gone; 0 when no ring
 * wants a page. Under the big lock.
 */
static int vn_refill(struct vnet *n)
{
    struct vn_vq *q;
    struct pin *pin = 0;
    uint32_t flags, reg, page, gen;
    int i, r;

    flags = spin_lock_irqsave(&n->lock);
    for (i = 0; i < 2 && !n->vq[i].want; i++) {
    }
    if (i == 2) {
        spin_unlock_irqrestore(&n->lock, flags);
        return 0;
    }
    q = &n->vq[i];
    reg = q->want_reg;
    page = q->want_page;
    gen = q->want_gen;
    if (gen != n->gen) {
        q->want = 0;
    } else if (reg < n->nreg) {
        pin = n->reg[reg].pin;
    }
    spin_unlock_irqrestore(&n->lock, flags);
    if (gen != n->gen) {
        vn_wake(q);
        return 1;
    }
    n->refilling++;
    r = pin ? pin_refill(pin, page) : -EFAULT;
    flags = spin_lock_irqsave(&n->lock);
    if (q->want && q->want_gen == gen) {
        q->want = 0;
    }
    if (r != 0 && n->gen == gen) {
        vn_break(n, q);
    }
    spin_unlock_irqrestore(&n->lock, flags);
    vn_wake(q);
    n->refilling--;
    wq_wake_all(&vn_idle_wq);
    return 1;
}

/** The thread's wait condition: a kick came. */
static int vn_has_work(void *arg)
{
    (void)arg;
    return vn_work != 0;
}

/**
 * One pass of the thread: take each kicked transmit ring, its frames given
 * to the stack as one batch of TCP's acknowledgements (`tcp_batch_begin`),
 * offer each kicked receive ring the frames waiting in the tap, and then
 * bring in one page a ring wants, whose kick brings the thread round
 * again. Under the big lock.
 */
static void vn_pass(void)
{
    struct vnet *n;
    int more;

    vn_work = 0;
    sched_mb(); /* the flag, then the rings' */
    for (n = vn_devs; n; n = n->next) {
        if (n->vq[VN_TX].kicked) {
            n->vq[VN_TX].kicked = 0;
            tcp_batch_begin();
            more = vn_tx(n);
            tcp_batch_end();
            if (more) {
                vn_wake(&n->vq[VN_TX]);
            }
        }
        if (n->vq[VN_RX].kicked) {
            n->vq[VN_RX].kicked = 0;
            if (n->tap) {
                vn_rx_quiet(n);
                tap_backend_drain(n->tap);
            }
        }
    }
    for (n = vn_devs; n && !vn_refill(n); n = n->next) {
    }
}

/** The kernel thread, `vhost-net`: wait for a kick, then make a pass. An
 * ordinary thread, as Linux's vhost worker is. It holds the big lock, as
 * every kernel thread does, and the tick does not switch it
 * (`no_tick_switch`), so it gives way where it sleeps and at a service
 * point after each pass, as that worker calls cond_resched between works
 * (drivers/vhost/vhost.c:440). */
static void vn_main(void)
{
    arch_irq_enable();
    for (;;) {
        sched_service_point();
        (void)wq_wait_event_idle(&vn_wq, vn_has_work, NULL);
        vn_pass();
    }
}

/** The tap's `gone`: its descriptor's last close let the backend go, so
 * the rings have no tap. The device holds the descriptor while it has the
 * tap (`tapfile`), so this comes only if the tap is let go some other way.
 * Under the big lock. */
static void vn_gone(struct tap_backend *b)
{
    struct vnet *n = (struct vnet *)b;
    uint32_t flags = spin_lock_irqsave(&n->lock);

    n->tap = 0;
    n->vq[VN_RX].backend = 0;
    n->vq[VN_TX].backend = 0;
    spin_unlock_irqrestore(&n->lock, flags);
}

/** A pin's holder: the owner's address space ended and took the guest's
 * pages back; no ring touches them again, and a receive under way has
 * ended once `lock` is taken. Under the big lock, which keeps the thread
 * off the rings. */
static void vn_release(struct pin *pin, void *owner)
{
    struct vnet *n = (struct vnet *)owner;
    uint32_t flags;

    (void)pin;
    if (!n->dead) {
        klog(KLOG_WARN, "vhost-net", "stopped: the owner's address space ended");
    }
    flags = spin_lock_irqsave(&n->lock);
    n->dead = 1;
    spin_unlock_irqrestore(&n->lock, flags);
}

/** A pin's holder's `forget`: a change of the guest's pages took some from
 * a region's pin; a receive that may use one has ended once `lock` is
 * taken, and the transmit thread, which holds the big lock while it copies,
 * is not in the middle of one. */
static void vn_forget(struct pin *pin, void *owner)
{
    struct vnet *n = (struct vnet *)owner;
    uint32_t flags;

    (void)pin;
    flags = spin_lock_irqsave(&n->lock);
    spin_unlock_irqrestore(&n->lock, flags);
}

/** The thread's refill condition: no refill of `arg`'s regions sleeps. */
static int vn_not_refilling(void *arg)
{
    return ((struct vnet *)arg)->refilling == 0;
}

/** Wait until the thread is not refilling a region of `n`, so that a pin
 * may go. Under the big lock; may sleep. */
static void vn_quiesce(struct vnet *n)
{
    while (n->refilling != 0) {
        (void)wq_wait_event_idle(&vn_idle_wq, vn_not_refilling, n);
    }
}

/** Unpin the regions of the first `count` of `reg` that `which` marks,
 * a bit each. */
static void vn_unpin(struct vn_region *reg, uint32_t count, uint64_t which)
{
    uint32_t i;

    for (i = 0; i < count; i++) {
        if (which >> i & 1u) {
            unpin(reg[i].pin);
        }
        reg[i].pin = 0;
    }
}

/** Whether a pin of the regions `reg`, `count` of them, is no longer live,
 * since an unmap took it back. */
static int vn_any_dead(const struct vn_region *reg, uint32_t count)
{
    uint32_t i;

    for (i = 0; i < count; i++) {
        if (reg[i].pin && !pin_live(reg[i].pin)) {
            return 1;
        }
    }
    return 0;
}

/** Whether the pin `p` is that of one of the first `count` regions of
 * `reg`. */
static int vn_has_pin(const struct vn_region *reg, uint32_t count, const struct pin *p)
{
    uint32_t i;

    for (i = 0; i < count; i++) {
        if (reg[i].pin == p) {
            return 1;
        }
    }
    return 0;
}

/** The pins of the regions `reg`, `count` of them, as a bit each, but those
 * of `keep`, of `nkeep` regions, too, and those an earlier region of `reg`
 * has, so that each pin is let go once. */
static uint64_t vn_pins_but(const struct vn_region *reg, uint32_t count,
                            const struct vn_region *keep, uint32_t nkeep)
{
    uint64_t which = 0;
    uint32_t i;

    for (i = 0; i < count; i++) {
        if (reg[i].pin && !vn_has_pin(keep, nkeep, reg[i].pin) && !vn_has_pin(reg, i, reg[i].pin)) {
            which |= (uint64_t)1 << i;
        }
    }
    return which;
}

/**
 * Fill `reg` from the `count` regions of the table at `arg`: each given a
 * pin that follows its pages (PIN_FOLLOW), which holds none yet, or the
 * pin of a region the device's table has whole, which the bit of `*fresh`
 * for it leaves out. 0; -EFAULT for an unreadable table or a region that is
 * empty, wraps or leaves user space, as Linux's vhost_set_memory refuses
 * one; -ENOMEM; on an error nothing new is kept. May sleep, letting the big
 * lock go.
 */
static int32_t vn_pin_table(struct vnet *n, uintptr_t arg, uint32_t count, struct vn_region *reg,
                            uint64_t *fresh)
{
    uint32_t i, j;
    uint64_t r4[4];
    int r;

    *fresh = 0;
    for (i = 0; i < count; i++) {
        if (copy_from_user(r4, (const void __user *)(arg + 8u + 32u * i), sizeof(r4)) != 0) {
            r = -EFAULT;
            goto fail;
        }
        reg[i].gpa = r4[0];
        reg[i].size = r4[1];
        reg[i].uaddr = r4[2];
        reg[i].pin = 0;
        for (j = 0; j < n->nreg; j++) {
            if (n->reg[j].pin && pin_live(n->reg[j].pin) && n->reg[j].gpa == r4[0] &&
                n->reg[j].size == r4[1] && n->reg[j].uaddr == r4[2]) {
                reg[i].pin = n->reg[j].pin;
                break;
            }
        }
        if (reg[i].pin) {
            continue;
        }
        if (r4[1] == 0 || r4[2] + r4[1] < r4[2] ||
            (uint64_t)(uintptr_t)(r4[2] + r4[1] - 1u) != r4[2] + r4[1] - 1u) {
            r = -EFAULT;
            goto fail;
        }
        r = pin_user(current_proc->mm, (uintptr_t)r4[2], (uintptr_t)r4[1], PIN_WRITE | PIN_FOLLOW,
                     &n->holder, &reg[i].pin);
        if (r != 0) {
            reg[i].pin = 0;
            r = r == -ENOMEM ? -ENOMEM : -EFAULT;
            goto fail;
        }
        *fresh |= (uint64_t)1 << i;
    }
    return 0;
fail:
    vn_unpin(reg, i, *fresh);
    return r;
}

/**
 * VHOST_SET_MEM_TABLE: give each region of the table at `arg` a pin that
 * follows its pages, keeping the pin of a region the old table has whole,
 * then let the old table go, as Linux's vhost_set_memory replaces its
 * table. Any memory the process maps is taken, a writable shared mapping of
 * a disk file among it, since the pins hold a page only until it changes
 * and a write-back's taking of write access is such a change. 0; -E2BIG for
 * more than VHOST_NET_REGIONS; -EFAULT for an unreadable table or a bad
 * region; -ENOMEM; each of which leaves the old table. The old pins go once
 * no refill of the thread's uses them. Under the big lock and the device's
 * mutex, which keeps the table and the owner as they are while a pin
 * sleeps.
 */
static int32_t vn_set_mem(struct vnet *n, uintptr_t arg)
{
    struct vn_region *reg, *old;
    uint32_t head[2], flags, nold;
    uint64_t fresh, gone;
    int32_t r;

    if (copy_from_user(head, (const void __user *)arg, sizeof(head)) != 0) {
        return -EFAULT;
    }
    if (head[0] > VHOST_NET_REGIONS) {
        return -E2BIG;
    }
    reg = (struct vn_region *)kzalloc(VHOST_NET_REGIONS * sizeof(*reg));
    if (!reg) {
        return -ENOMEM;
    }
    r = vn_pin_table(n, arg, head[0], reg, &fresh);
    if (r != 0) {
        kfree(reg);
        return r;
    }
    gone = vn_pins_but(n->reg, n->nreg, reg, head[0]);
    old = n->reg;
    nold = n->nreg;
    flags = spin_lock_irqsave(&n->lock);
    n->reg = reg;
    n->nreg = head[0];
    n->last = 0;
    n->dead = vn_any_dead(reg, head[0]);
    n->gen++;
    spin_unlock_irqrestore(&n->lock, flags);
    if (old) {
        vn_quiesce(n);
        vn_unpin(old, nold, gone);
        kfree(old);
    }
    return 0;
}

/** Whether the `len` bytes at QEMU's address `va` lie in pinned regions:
 * their first and last bytes, since the rings are small. */
static int vn_in_memory(struct vnet *n, uint64_t va, uint64_t len)
{
    struct vn_region *r = vn_find(n, va, 1), *s = vn_find(n, va + len - 1u, 1);

    return r && r->pin && s && s->pin;
}

/** Whether the rings of `q` lie in the guest's memory, as Linux's
 * vhost_vq_access_ok asks before a ring gets its backend. */
static int vn_rings_ok(struct vnet *n, struct vn_vq *q)
{
    return q->addr_set && q->num && vn_in_memory(n, q->desc, 16u * (uint64_t)q->num) &&
           vn_in_memory(n, q->avail, 6u + 2u * (uint64_t)q->num) &&
           vn_in_memory(n, q->used, 6u + 8u * (uint64_t)q->num);
}

/**
 * Let go of the tap when neither ring has it, logging the frames moved, and
 * then of its descriptor, whose release is the tap's last close when the
 * program has closed its own. The tap's lock, which `tap_backend_set`
 * takes, waits for a receive under way. Under the big lock; the release
 * may sleep.
 */
static void vn_detach(struct vnet *n)
{
    struct tap *t = n->tap;
    fd_entry_t file;
    uint32_t flags;

    if (n->vq[VN_RX].backend || n->vq[VN_TX].backend) {
        return;
    }
    if (t) {
        (void)tap_backend_set(t, 0);
        flags = spin_lock_irqsave(&n->lock);
        n->tap = 0;
        spin_unlock_irqrestore(&n->lock, flags);
        vn_detaches++;
        vn_detached_to += n->to_guest;
        vn_detached_from += n->from_guest;
        klog(KLOG_INFO, "vhost-net", "detached: %s: %u frames to the guest, %u from it",
             tap_name(t), n->to_guest, n->from_guest);
    }
    if (n->has_tapfile) {
        file = n->tapfile;
        n->has_tapfile = 0;
        fd_release_backing(&file);
    }
}

/** VHOST_NET_SET_BACKEND: give ring `index` the tap of descriptor `fd`, or
 * take it away with -1. 0; -ENOTSOCK for a descriptor that is no tap with
 * an interface; -EBUSY for a tap another backend has, or one other than the
 * other ring's; -EFAULT for a ring with no size or address, or one outside
 * the guest's pinned memory. Under the big lock. */
static int32_t vn_set_backend(struct vnet *n, uint32_t index, int32_t fd)
{
    struct vn_vq *q = &n->vq[index];
    fd_entry_t *e;
    struct tap *t;
    uint32_t flags;
    int32_t r;

    if (fd == VN_UNBIND) {
        flags = spin_lock_irqsave(&n->lock);
        q->backend = 0;
        spin_unlock_irqrestore(&n->lock, flags);
        vn_detach(n);
        return 0;
    }
    e = fd >= 0 ? fd_get(current_proc->fds, fd) : 0;
    t = tap_of(e);
    if (!t) {
        return -ENOTSOCK;
    }
    if (n->tap && n->tap != t) {
        return -EBUSY;
    }
    if (!vn_rings_ok(n, q)) {
        return -EFAULT;
    }
    r = tap_backend_set(t, &n->tb);
    if (r != 0) {
        return r;
    }
    if (!n->has_tapfile) {
        n->tapfile = *e;
        fd_ofd_ref(e->ofd);
        n->has_tapfile = 1;
        n->to_guest = 0;
        n->from_guest = 0;
    }
    flags = spin_lock_irqsave(&n->lock);
    n->tap = t;
    q->backend = 1;
    q->broken = 0;
    q->used_known = 0;
    q->no_notify = 0;
    spin_unlock_irqrestore(&n->lock, flags);
    vn_wake(q);
    return 0;
}

/** Replace the eventfd `*slot` with that of descriptor `fd`, or none with
 * -1, under `lock`, since the receive path signals it. 0, or eventfd_get's
 * -EBADF or -EINVAL. */
static int32_t vn_set_fd(struct vnet *n, struct eventfd **slot, int32_t fd)
{
    struct eventfd *ev = 0, *old;
    int32_t err = 0;
    uint32_t flags;

    if (fd != VN_UNBIND) {
        ev = eventfd_get(fd, &err);
        if (!ev) {
            return err;
        }
    }
    flags = spin_lock_irqsave(&n->lock);
    old = *slot;
    *slot = ev;
    spin_unlock_irqrestore(&n->lock, flags);
    if (old) {
        eventfd_put(old);
    }
    return 0;
}

/**
 * VHOST_SET_VRING_KICK: hook the eventfd of `fd`, holding its descriptor,
 * both taken from one lookup, or none with -1, and then let the old kick
 * go. 0, or eventfd_get's
 * -EBADF or -EINVAL with the old kick kept. Under the big lock; the
 * release of the old descriptor may sleep.
 */
static int32_t vn_set_kick(struct vn_vq *q, int32_t fd)
{
    struct eventfd *ev = 0, *old = q->kick;
    fd_entry_t file, oldfile, *e;
    int32_t err = 0;
    int had = q->has_kickfile;

    k_memset(&file, 0, sizeof(file));
    k_memset(&oldfile, 0, sizeof(oldfile));
    if (fd != VN_UNBIND) {
        e = fd >= 0 ? fd_get(current_proc->fds, fd) : 0;
        ev = eventfd_entry_get(e, &err);
        if (!ev) {
            return err;
        }
        file = *e;
        fd_ofd_ref(file.ofd);
    }
    if (old && !eventfd_hook_remove(old, &q->hook, NULL)) {
        old = 0; /* the hangup let it go */
    }
    if (had) {
        oldfile = q->kickfile;
    }
    q->kick = 0;
    q->has_kickfile = 0;
    if (ev) {
        q->kick = ev;
        q->kickfile = file;
        q->has_kickfile = 1;
        q->hook.signal = vn_kick;
        q->hook.hangup = vn_kick_hangup;
        q->hook.next = 0;
        if (eventfd_hook_add(ev, &q->hook) == 1) {
            vn_wake(q);
        }
    }
    if (old) {
        eventfd_put(old);
    }
    if (had) {
        fd_release_backing(&oldfile);
    }
    return 0;
}

/** Stop every ring and let go of the tap, the eventfds, the descriptors
 * held, the pins and the owner, as Linux's vhost_dev_reset_owner. Under the
 * big lock and the device's mutex, so that no request of the owner's other
 * threads runs while a release or the wait for a refill sleeps; may sleep. */
static void vn_reset(struct vnet *n)
{
    struct vn_region *old;
    uint32_t flags, nold;
    int i;

    flags = spin_lock_irqsave(&n->lock);
    n->vq[VN_RX].backend = 0;
    n->vq[VN_TX].backend = 0;
    spin_unlock_irqrestore(&n->lock, flags);
    vn_detach(n);
    for (i = 0; i < 2; i++) {
        struct vn_vq *q = &n->vq[i];

        (void)vn_set_kick(q, VN_UNBIND);
        (void)vn_set_fd(n, &q->call, VN_UNBIND);
        (void)vn_set_fd(n, &q->err, VN_UNBIND);
        k_memset(q, 0, sizeof(*q));
        q->n = n;
    }
    old = n->reg;
    nold = n->nreg;
    flags = spin_lock_irqsave(&n->lock);
    n->reg = 0;
    n->nreg = 0;
    n->gen++;
    spin_unlock_irqrestore(&n->lock, flags);
    if (old) {
        vn_quiesce(n);
        vn_unpin(old, nold, vn_pins_but(old, nold, 0, 0));
        kfree(old);
    }
    n->last = 0;
    n->mm = 0;
    n->features = 0;
    n->hdr = 0;
    n->dead = 0;
}

/** VHOST_SET_VRING_NUM, VHOST_SET_VRING_BASE, VHOST_GET_VRING_BASE and the
 * busy-poll timeout's two requests, on the ring state at `arg`, as Linux's
 * vhost_vring_ioctl: -ENOBUFS for a ring past the two, -EBUSY to change the
 * size or base of a ring that has the tap, -EINVAL for a size that is no
 * power of two up to 32768 or a base past 0xffff. The timeout is kept and
 * given back, and the thread never polls. */
static int32_t vn_ring_state(struct vnet *n, uint32_t req, uintptr_t arg)
{
    struct vn_vq *q;
    uint32_t st[2], flags;

    if (copy_from_user(st, (const void __user *)arg, sizeof(st)) != 0) {
        return -EFAULT;
    }
    if (st[0] > 1) {
        return -ENOBUFS;
    }
    q = &n->vq[st[0]];
    if (req == VHOST_SET_VRING_BUSYLOOP_TIMEOUT) {
        q->busyloop = st[1];
        return 0;
    }
    if (req == VHOST_GET_VRING_BUSYLOOP_TIMEOUT) {
        st[1] = q->busyloop;
        return copy_to_user((void __user *)arg, st, sizeof(st)) != 0 ? -EFAULT : 0;
    }
    if (req == VHOST_GET_VRING_BASE) {
        flags = spin_lock_irqsave(&n->lock);
        st[1] = q->last_avail;
        spin_unlock_irqrestore(&n->lock, flags);
        return copy_to_user((void __user *)arg, st, sizeof(st)) != 0 ? -EFAULT : 0;
    }
    if (q->backend) {
        return -EBUSY;
    }
    if (req == VHOST_SET_VRING_NUM && (st[1] == 0 || st[1] > 32768u || (st[1] & (st[1] - 1u)))) {
        return -EINVAL;
    }
    if (req == VHOST_SET_VRING_BASE && st[1] > 0xFFFFu) {
        return -EINVAL;
    }
    flags = spin_lock_irqsave(&n->lock);
    if (req == VHOST_SET_VRING_NUM) {
        q->num = st[1];
    } else {
        q->last_avail = (uint16_t)st[1];
        q->used_known = 0;
    }
    spin_unlock_irqrestore(&n->lock, flags);
    return 0;
}

/** VHOST_SET_VRING_ADDR: the index and flags, then the descriptor table,
 * the used ring, the available ring and the log's address, which no log
 * uses. -ENOBUFS for a ring past the two, -EOPNOTSUPP for a flag but the
 * log's, -EINVAL for a table or ring not aligned as virtio asks. */
static int32_t vn_ring_addr(struct vnet *n, uintptr_t arg)
{
    struct vn_vq *q;
    uint64_t addr[5];
    uint32_t st[2], flags;

    if (copy_from_user(addr, (const void __user *)arg, sizeof(addr)) != 0) {
        return -EFAULT;
    }
    k_memcpy(st, &addr[0], sizeof(st));
    if (st[0] > 1) {
        return -ENOBUFS;
    }
    if (st[1] & ~VRING_F_LOG) {
        return -EOPNOTSUPP;
    }
    if ((addr[1] & 15u) || (addr[2] & 3u) || (addr[3] & 1u)) {
        return -EINVAL;
    }
    q = &n->vq[st[0]];
    flags = spin_lock_irqsave(&n->lock);
    q->desc = addr[1];
    q->used = addr[2];
    q->avail = addr[3];
    q->addr_set = 1;
    q->used_known = 0;
    q->no_notify = 0;
    spin_unlock_irqrestore(&n->lock, flags);
    return 0;
}

/** VHOST_SET_VRING_KICK, _CALL, _ERR and VHOST_NET_SET_BACKEND, on the
 * ring file at `arg`; -ENOBUFS for a ring past the two. */
static int32_t vn_ring_file(struct vnet *n, uint32_t req, uintptr_t arg)
{
    uint32_t st[2];

    if (copy_from_user(st, (const void __user *)arg, sizeof(st)) != 0) {
        return -EFAULT;
    }
    if (st[0] > 1) {
        return -ENOBUFS;
    }
    if (req == VHOST_SET_VRING_KICK) {
        return vn_set_kick(&n->vq[st[0]], (int32_t)st[1]);
    }
    if (req == VHOST_SET_VRING_CALL) {
        return vn_set_fd(n, &n->vq[st[0]].call, (int32_t)st[1]);
    }
    if (req == VHOST_SET_VRING_ERR) {
        return vn_set_fd(n, &n->vq[st[0]].err, (int32_t)st[1]);
    }
    return vn_set_backend(n, st[0], (int32_t)st[1]);
}

/** VHOST_SET_FEATURES: the features at `arg`, and with them the length of
 * the header the backend adds and strips, as Linux's vhost_net_set_features.
 * -EOPNOTSUPP for one not offered. */
static int32_t vn_set_features(struct vnet *n, uintptr_t arg)
{
    uint64_t v;
    uint32_t flags, hdr = 0;

    if (copy_from_user(&v, (const void __user *)arg, sizeof(v)) != 0) {
        return -EFAULT;
    }
    if (v & ~(uint64_t)VHOST_NET_FEATURES) {
        return -EOPNOTSUPP;
    }
    if (v & VHOST_NET_F_VIRTIO_NET_HDR) {
        hdr = (v & (VHOST_F_VERSION_1 | VHOST_F_MRG_RXBUF)) ? 12u : 10u;
    }
    flags = spin_lock_irqsave(&n->lock);
    n->features = v;
    n->hdr = hdr;
    spin_unlock_irqrestore(&n->lock, flags);
    return 0;
}

/** The requests Linux answers whoever the caller: 1 for another. Under the
 * device's mutex, but for the two that read the features. */
static int32_t vn_ioctl_any(struct vnet *n, uint32_t req, uintptr_t arg)
{
    uint64_t v = 0;

    switch (req) {
    case VHOST_SET_OWNER:
        if (n->mm) {
            return -EBUSY;
        }
        n->mm = current_proc->mm;
        return n->mm ? 0 : -EINVAL;
    case VHOST_GET_FEATURES:
        v = VHOST_NET_FEATURES;
        return copy_to_user((void __user *)arg, &v, sizeof(v)) != 0 ? -EFAULT : 0;
    case VHOST_SET_FEATURES:
        return vn_set_features(n, arg);
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

/** The requests of the owner alone, as `vn_ioctl` says. Under the big lock
 * and the device's mutex. */
static int32_t vn_ioctl_owner(struct vnet *n, uint32_t req, uintptr_t arg)
{
    switch (req) {
    case VHOST_RESET_OWNER:
        vn_reset(n);
        return 0;
    case VHOST_SET_MEM_TABLE:
        return vn_set_mem(n, arg);
    case VHOST_SET_VRING_NUM:
    case VHOST_SET_VRING_BASE:
    case VHOST_GET_VRING_BASE:
    case VHOST_SET_VRING_BUSYLOOP_TIMEOUT:
    case VHOST_GET_VRING_BUSYLOOP_TIMEOUT:
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

/**
 * The descriptor's control, Linux's vhost-net ioctls: -EPERM from a caller
 * whose address space is not the owner's, as Linux's
 * vhost_dev_check_owner, -E2BIG, -ENOBUFS, -EBUSY, -EINVAL, -EOPNOTSUPP,
 * -ENOTSOCK and eventfd_get's errors as the requests' own functions say,
 * and -ENOTTY for another request. Each request but the two that read the
 * features runs under the device's mutex, as Linux's vhost_net_ioctl takes
 * `dev->mutex`, so that one that sleeps, VHOST_RESET_OWNER waiting for the
 * thread's refill among them, lets no other change the device midway.
 * Under the big lock.
 */
static int32_t vn_ioctl(fd_entry_t *e, uint32_t req, uintptr_t arg)
{
    struct vnet *n = (struct vnet *)e->priv;
    int32_t r;

    if (!n) {
        return -EBADFD;
    }
    if (req == VHOST_GET_FEATURES || req == VHOST_GET_BACKEND_FEATURES) {
        return vn_ioctl_any(n, req, arg);
    }
    mutex_lock(&n->mutex);
    r = vn_ioctl_any(n, req, arg);
    if (r == 1 && (!n->mm || n->mm != current_proc->mm)) {
        r = -EPERM;
    }
    if (r == 1) {
        r = vn_ioctl_owner(n, req, arg);
    }
    mutex_unlock(&n->mutex);
    return r;
}

/** An open: a device of its own. 0, or -ENOMEM. */
static int32_t vn_open(struct devfs_node *node, struct fd_entry *e, int flags)
{
    struct vnet *n;

    (void)node;
    (void)flags;
    n = (struct vnet *)kzalloc(sizeof(*n));
    if (!n) {
        return -ENOMEM;
    }
    spin_lock_init(&n->lock, "vnet");
    mutex_init(&n->mutex, "vhost-net");
    n->tb.rx = vn_rx;
    n->tb.gone = vn_gone;
    n->holder.release = vn_release;
    n->holder.owner = n;
    n->holder.flags = 0;
    n->holder.forget = vn_forget;
    n->vq[VN_RX].n = n;
    n->vq[VN_TX].n = n;
    n->next = vn_devs;
    vn_devs = n;
    e->priv = n;
    return 0;
}

/** The last close: the device stops and goes, once a request that sleeps
 * has let the mutex go. Under the big lock, which keeps the thread off the
 * list. */
static void vn_close(fd_entry_t *e)
{
    struct vnet *n = (struct vnet *)e->priv, **pp;

    if (!n) {
        return;
    }
    e->priv = 0;
    mutex_lock(&n->mutex);
    vn_reset(n);
    mutex_unlock(&n->mutex);
    for (pp = &vn_devs; *pp && *pp != n; pp = &(*pp)->next) {
    }
    if (*pp) {
        *pp = n->next;
    }
    kfree(n);
}

static const struct file_ops vn_ops = {NULL, NULL, NULL, vn_ioctl, vn_close, NULL, NULL, 0};

/* Linux's numbers for the device, 10:238, read and written by everyone, as
 * /dev/kvm is; a root that runs udev gives both the group and mode its
 * rules name. */
static struct devfs_node vn_node = {"vhost-net", 10, 238, 0666, &vn_ops, vn_open, NULL, 0};

void vhost_net_detached_frames(uint32_t *n, uint64_t *to, uint64_t *from)
{
    *n = vn_detaches;
    *to = vn_detached_to;
    *from = vn_detached_from;
}

void vhost_net_start(void)
{
    uintptr_t base;
    uint32_t flags;
    int32_t r;

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
    proc_set_comm(vn_thread, "vhost-net");
    setup_process_stack(vn_thread, vn_main);
    sched_make_ready(vn_thread);
    kirq_restore(flags);
    r = devfs_node_register(&vn_node);
    if (r != 0) {
        klog(KLOG_WARN, "vhost-net", "/dev/vhost-net not made: error %d", r);
    }
}
