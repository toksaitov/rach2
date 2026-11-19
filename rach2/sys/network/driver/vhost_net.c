/* vhost_net.c: /dev/vhost-net, as vhost_net.h describes it: the guest's
 * memory, its two rings, the frames between them and a tap interface, and
 * the kernel thread that takes the guest's transmit ring. */
#include "vhost_net.h"
#include "tap.h"
#include "../ethernet/ethernet.h"
#include "../../fs/devfs.h"
#include "../../fs/fd.h"
#include "../../fs/file.h"
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
#include "../../proc/process.h"
#include "../../proc/sched.h"
#include "../../proc/smp.h"
#include "../../proc/wait_queue.h"

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
#define VN_TXBUF   (VN_HDR_MAX + ETH_HDR_SIZE + ETH_MTU)
#define VN_UNBIND  (-1) /* VHOST_FILE_UNBIND */

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

/*
 * One open of /dev/vhost-net. The receive path runs in the context of
 * whoever sends on the tap, an interrupt handler among them, without the
 * big lock, so `lock` is over everything it reads: the regions, `dead`,
 * `hdr`, `features`, `tap` and the receive ring. A change of those is made
 * under the big lock and `lock`; the transmit ring and the thread, under
 * the big lock alone, read them without `lock`.
 */
struct vnet {
    struct tap_backend tb; /* first: the tap's */
    struct vnet *next;     /* on `vn_devs` */
    struct spinlock lock;
    struct mm *mm; /* the owner's, VHOST_SET_OWNER; compared, never used */
    uint64_t features;
    uint32_t hdr;          /* bytes of the virtio header before each frame */
    struct vn_region *reg; /* VHOST_NET_REGIONS of them, or NULL */
    uint32_t nreg;
    volatile uint32_t last; /* the region the last lookup found */
    struct pin_holder holder;
    int dead;     /* an unmap took a pin back: no more access */
    uint32_t gen; /* changes with each table and reset */
    struct vn_vq vq[2];
    struct tap *tap;
    uint8_t txbuf[VN_TXBUF];
};

/* Every open device, under the big lock. */
static struct vnet *vn_devs;
static wait_queue_t vn_wq;        /* the thread sleeps here */
static volatile uint32_t vn_work; /* a kick came since the thread looked */
static process_t *vn_thread;

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

/** Ask the guest for no kicks on `q` with `on`, or for kicks again. */
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
    sched_mb(); /* the entry before the index */
    q->used_idx++;
    return vn_put16(n, q->used + 2, q->used_idx);
}

/** Raise the ring's interrupt unless the guest asked for none, or, with
 * VIRTIO_F_NOTIFY_ON_EMPTY, whatever it asked once the available ring is
 * empty, as Linux's vhost_notify without the event index. */
static void vn_notify(struct vnet *n, struct vn_vq *q)
{
    uint16_t flags = 0, idx = 0;

    sched_mb(); /* the used index, then the guest's flags */
    if (!q->call || vn_get16(n, q->avail, &flags) != 0) {
        return;
    }
    if ((n->features & VHOST_F_NOTIFY_ON_EMPTY) && vn_get16(n, q->avail + 2, &idx) == 0 &&
        idx == q->last_avail) {
        flags = 0;
    }
    if (!(flags & AVAIL_NO_INTR)) {
        eventfd_signal(q->call);
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
    if ((uint16_t)(idx - q->last_avail) > q->num) {
        return -EFAULT; /* more offered than the ring holds */
    }
    sched_mb(); /* the index, then the entry */
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

/** The bytes the guest may write in the chain from `head` of `q`, the
 * receive ring's: 0 with the sum in `*room`, or -EFAULT for a chain that
 * loops, holds a descriptor the device may only read or a bad one. */
static int vn_room(struct vnet *n, struct vn_vq *q, uint16_t head, uint32_t *room)
{
    struct vn_desc d;
    uint32_t i = head, steps = 0, sum = 0;

    for (;;) {
        if (++steps > q->num || vn_desc(n, q, i, &d) != 0 || !(d.flags & DESC_WRITE)) {
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
 * put it on the used ring: 0, or -EFAULT, also for a chain the guest
 * changed meanwhile into one too short or bad.
 */
static int vn_fill(struct vnet *n, struct vn_vq *q, uint16_t head, const uint8_t *frame,
                   uint32_t len)
{
    /* No offload, and num_buffers 1 in the 1.0 header, as virtio 1.0
     * 5.1.6.3.1 asks of a device without VIRTIO_NET_F_MRG_RXBUF. */
    static const uint8_t vhdr[VN_HDR_MAX] = {0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 1, 0};
    uint32_t total = n->hdr + len, off = 0, i = head, steps = 0;
    struct vn_desc d;

    while (off < total) {
        uint32_t doff = 0;

        if (++steps > q->num || vn_desc(n, q, i, &d) != 0 || !(d.flags & DESC_WRITE)) {
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
            if (vn_mem(n, d.addr + doff, 0, src, NULL, take) != 0) {
                return -EFAULT;
            }
            doff += take;
            off += take;
        }
        if (off < total && !(d.flags & DESC_NEXT)) {
            return -EFAULT;
        }
        i = d.next;
    }
    return vn_used(n, q, head, total);
}

/**
 * The tap's `rx`: write the frame of `len` bytes, after the virtio header,
 * into the next buffer the guest offers on its receive ring, and raise the
 * ring's interrupt. 1 when the frame was taken, also when it was dropped
 * for a buffer too short, which stays the guest's, as Linux's handle_rx
 * discards such a frame and gives the buffer back (vhost_discard_vq_desc);
 * 0 when the ring has no buffer or does not move frames, and the frame
 * waits in the tap. Called with the tap's lock held and interrupts off, in
 * the sender's context; takes `lock`.
 */
static int vn_rx(struct tap_backend *b, const uint8_t *frame, uint16_t len)
{
    struct vnet *n = (struct vnet *)b;
    struct vn_vq *q = &n->vq[VN_RX];
    uint32_t flags = spin_lock_irqsave(&n->lock), room = 0;
    uint16_t head;
    int r = 0;

    if (!vn_ready(n, q) || vn_used_init(n, q) != 0) {
        goto out;
    }
    r = vn_next(n, q, &head);
    if (r <= 0) {
        if (r < 0) {
            vn_break(n, q);
        }
        r = 0;
        goto out;
    }
    if (vn_room(n, q, head, &room) != 0) {
        vn_break(n, q);
        r = 0;
        goto out;
    }
    if (room < n->hdr + len) {
        q->last_avail--;
        r = 1;
        goto out;
    }
    if (vn_fill(n, q, head, frame, len) != 0) {
        vn_break(n, q);
        r = 0;
        goto out;
    }
    vn_notify(n, q);
    r = 1;
out:
    spin_unlock_irqrestore(&n->lock, flags);
    return r;
}

/**
 * Take what the guest put on its transmit ring, at most VN_BUDGET frames,
 * and give each frame to the tap as a write would; then raise the ring's
 * interrupt. A frame shorter than the header and an Ethernet header, or
 * longer than the interface carries, is dropped. The kernel thread calls it,
 * under the big lock. 1 when the budget ran out with frames left.
 */
static int vn_tx(struct vnet *n)
{
    struct vn_vq *q = &n->vq[VN_TX];
    uint32_t budget = VN_BUDGET, done = 0;
    struct vn_desc d;
    uint16_t head;
    int r;

    if (!vn_ready(n, q) || vn_used_init(n, q) != 0) {
        return 0;
    }
    vn_quiet(n, q, 1);
    while (budget != 0) {
        uint32_t len = 0, steps = 0, i;
        int over = 0;

        r = vn_next(n, q, &head);
        if (r <= 0) {
            if (r < 0) {
                goto broken;
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
            (void)tap_inject(n->tap, n->txbuf + n->hdr, len - n->hdr);
        }
        if (vn_used(n, q, head, 0) != 0) {
            goto broken;
        }
        done++;
        budget--;
    }
    if (done) {
        vn_notify(n, q);
    }
    return budget == 0;
broken:
    if (done) {
        vn_notify(n, q);
    }
    vn_break(n, q);
    return 0;
}

/** Note a kick of `q` for the thread and wake it. */
static void vn_wake(struct vn_vq *q)
{
    q->kicked = 1;
    vn_work = 1;
    wq_wake(&vn_wq, 0);
}

/** The kick's hook: the thread looks at the ring. Called with the
 * eventfd's lock held and interrupts off, from the guest's doorbell (a KVM
 * ioeventfd, without the big lock) or a write by QEMU (with it), so it
 * only marks the ring and wakes the thread, as Linux's vhost_poll_wakeup
 * queues the work. */
static int vn_kick(struct eventfd_hook *h)
{
    vn_wake((struct vn_vq *)h);
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

/** Take the ring's kick away, its hook and its reference. Under the big
 * lock, which orders it with the hangup. */
static void vn_kick_drop(struct vn_vq *q)
{
    if (q->kick && eventfd_hook_remove(q->kick, &q->hook, NULL)) {
        eventfd_put(q->kick);
    }
    q->kick = 0;
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

    if (vn_ready(n, q) && vn_used_init(n, q) == 0) {
        vn_quiet(n, q, 1);
        sched_mb(); /* the flag, then the index */
        if (vn_get16(n, q->avail + 2, &idx) != 0 || idx == q->last_avail) {
            vn_quiet(n, q, 0);
        }
    }
    spin_unlock_irqrestore(&n->lock, flags);
}

/** The thread's wait condition: a kick came. */
static int vn_has_work(void *arg)
{
    (void)arg;
    return vn_work != 0;
}

/** One pass of the thread: take each kicked transmit ring and offer each
 * kicked receive ring the frames waiting in the tap. Under the big lock. */
static void vn_pass(void)
{
    struct vnet *n;

    vn_work = 0;
    sched_mb(); /* the flag, then the rings' */
    for (n = vn_devs; n; n = n->next) {
        if (n->vq[VN_TX].kicked) {
            n->vq[VN_TX].kicked = 0;
            if (vn_tx(n)) {
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

/** The tap's `gone`: its descriptor closed and the tap let the backend go,
 * so the rings have no tap. Under the big lock. */
static void vn_gone(struct tap_backend *b)
{
    struct vnet *n = (struct vnet *)b;
    uint32_t flags = spin_lock_irqsave(&n->lock);

    n->tap = 0;
    n->vq[VN_RX].backend = 0;
    n->vq[VN_TX].backend = 0;
    spin_unlock_irqrestore(&n->lock, flags);
}

/** A pin's holder: an unmap took the guest's pages back; no ring touches
 * them again, and a receive under way has ended once `lock` is taken.
 * Under the big lock, which keeps the thread off the rings. */
static void vn_release(struct pin *pin, void *owner)
{
    struct vnet *n = (struct vnet *)owner;
    uint32_t flags;

    (void)pin;
    if (!n->dead) {
        klog(KLOG_WARN, "vhost-net", "stopped: the guest's memory was unmapped");
    }
    flags = spin_lock_irqsave(&n->lock);
    n->dead = 1;
    spin_unlock_irqrestore(&n->lock, flags);
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
 * Fill `reg` from the `count` regions of the table at `arg`: each pinned,
 * or given the pin of a region the device's table has whole, which the bit
 * of `*fresh` for it leaves out. 0; -EFAULT for an unreadable table;
 * -ENOMEM from a pin, past RLIMIT_MEMLOCK, after unpinning what it pinned.
 * A region that cannot be pinned for another reason, device memory, is
 * left without a pin. May sleep, letting the big lock go.
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
        if (reg[i].pin || r4[1] == 0 || (r4[2] & 4095u) || r4[2] + r4[1] < r4[2] ||
            (uint64_t)(uintptr_t)(r4[2] + r4[1] - 1u) != r4[2] + r4[1] - 1u) {
            continue;
        }
        r = pin_user(current_proc->mm, (uintptr_t)r4[2], (uintptr_t)r4[1], PIN_WRITE | PIN_LONGTERM,
                     &n->holder, &reg[i].pin);
        if (r == 0) {
            *fresh |= (uint64_t)1 << i;
        } else if (r == -ENOMEM) {
            reg[i].pin = 0;
            goto fail;
        } else {
            reg[i].pin = 0;
        }
    }
    return 0;
fail:
    vn_unpin(reg, i, *fresh);
    return r;
}

/**
 * VHOST_SET_MEM_TABLE: pin each region of the table at `arg`, keeping the
 * pin of a region the old table has whole, then let the old table go, as
 * Linux's vhost_set_memory replaces its table. 0; -E2BIG for more than
 * VHOST_NET_REGIONS; -EFAULT for an unreadable table; -ENOMEM, also from a
 * pin past RLIMIT_MEMLOCK, which leaves the old table; -EPERM when a reset
 * took the owner meanwhile. A region that cannot be pinned for another
 * reason, device memory, is kept unpinned, and a ring or buffer in it
 * breaks the ring. A pin may sleep, letting the big lock go, so a table
 * that changed meanwhile, which `gen` tells, sends the call round again.
 * Under the big lock.
 */
static int32_t vn_set_mem(struct vnet *n, uintptr_t arg)
{
    struct vn_region *reg, *old;
    uint32_t head[2], flags, gen, nold;
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
    do {
        if (n->mm != current_proc->mm) {
            kfree(reg);
            return -EPERM;
        }
        gen = n->gen;
        r = vn_pin_table(n, arg, head[0], reg, &fresh);
        if (r != 0) {
            kfree(reg);
            return r;
        }
        if (n->gen != gen) {
            vn_unpin(reg, head[0], fresh);
        }
    } while (n->gen != gen);
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

/** Let go of the tap when neither ring has it. The tap's lock, which
 * `tap_backend_set` takes, waits for a receive under way. Under the big
 * lock. */
static void vn_detach(struct vnet *n)
{
    struct tap *t = n->tap;
    uint32_t flags;

    if (t && !n->vq[VN_RX].backend && !n->vq[VN_TX].backend) {
        (void)tap_backend_set(t, 0);
        flags = spin_lock_irqsave(&n->lock);
        n->tap = 0;
        spin_unlock_irqrestore(&n->lock, flags);
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
    t = tap_of(fd >= 0 ? fd_get(current_proc->fds, fd) : 0);
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

/** VHOST_SET_VRING_KICK: hook the eventfd of `fd`, or none with -1. 0, or
 * eventfd_get's -EBADF or -EINVAL. Under the big lock. */
static int32_t vn_set_kick(struct vn_vq *q, int32_t fd)
{
    struct eventfd *ev;
    int32_t err = 0;

    vn_kick_drop(q);
    if (fd == VN_UNBIND) {
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
        vn_wake(q);
    }
    return 0;
}

/** Stop every ring and let go of the tap, the eventfds and the pins, as
 * Linux's vhost_dev_reset_owner. Under the big lock. */
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

        vn_kick_drop(q);
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
        vn_unpin(old, nold, vn_pins_but(old, nold, 0, 0));
        kfree(old);
    }
    n->last = 0;
    n->mm = 0;
    n->features = 0;
    n->hdr = 0;
    n->dead = 0;
}

/** VHOST_SET_VRING_NUM, VHOST_SET_VRING_BASE and VHOST_GET_VRING_BASE, on
 * the ring state at `arg`, as Linux's vhost_vring_ioctl: -ENOBUFS for a
 * ring past the two, -EBUSY to change a ring that has the tap, -EINVAL for
 * a size that is no power of two up to 32768 or a base past 0xffff. */
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

/** The requests Linux answers whoever the caller: 1 for another. */
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

/**
 * The descriptor's control, Linux's vhost-net ioctls: -EPERM from a caller
 * whose address space is not the owner's, as Linux's
 * vhost_dev_check_owner, -E2BIG, -ENOBUFS, -EBUSY, -EINVAL, -EOPNOTSUPP,
 * -ENOTSOCK and eventfd_get's errors as the requests' own functions say,
 * and -ENOTTY for another request. Under the big lock.
 */
static int32_t vn_ioctl(fd_entry_t *e, uint32_t req, uintptr_t arg)
{
    struct vnet *n = (struct vnet *)e->priv;
    int32_t r;

    if (!n) {
        return -EBADFD;
    }
    r = vn_ioctl_any(n, req, arg);
    if (r != 1) {
        return r;
    }
    if (!n->mm || n->mm != current_proc->mm) {
        return -EPERM;
    }
    switch (req) {
    case VHOST_RESET_OWNER:
        vn_reset(n);
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
    n->tb.rx = vn_rx;
    n->tb.gone = vn_gone;
    n->holder.release = vn_release;
    n->holder.owner = n;
    n->holder.flags = 0;
    n->vq[VN_RX].n = n;
    n->vq[VN_TX].n = n;
    n->next = vn_devs;
    vn_devs = n;
    e->priv = n;
    return 0;
}

/** The last close: the device stops and goes. Under the big lock, which
 * keeps the thread off the list. */
static void vn_close(fd_entry_t *e)
{
    struct vnet *n = (struct vnet *)e->priv, **pp;

    if (!n) {
        return;
    }
    e->priv = 0;
    vn_reset(n);
    for (pp = &vn_devs; *pp && *pp != n; pp = &(*pp)->next) {
    }
    if (*pp) {
        *pp = n->next;
    }
    kfree(n);
}

static const struct file_ops vn_ops = {NULL, NULL, NULL, vn_ioctl, vn_close, NULL, NULL, 0};

/* Linux's numbers for the device, 10:238, read and written by root. */
static struct devfs_node vn_node = {"vhost-net", 10, 238, 0600, &vn_ops, vn_open, NULL};

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
