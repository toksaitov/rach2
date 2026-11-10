/*
 * iobus.c: the in-kernel bus of ioeventfds and coalesced zones, as Linux's
 * kvm_io_bus, the ioeventfd half of eventfd.c and coalesced_mmio.c: a
 * guest's write to a port or an MMIO address that an ioeventfd names signals
 * its eventfd, one inside a coalesced zone is queued on the VM's ring, and
 * either goes no further, so the guest enters again without a trip to the
 * caller.
 */

#include "kvm.h"

#include "../io/eventfd.h"
#include "../lib/kerrno.h"
#include "../lib/kstring.h"
#include "../mm/kheap.h"
#include "../mm/usercopy.h"

/* The buses an ioeventfd or a zone sits on. */
#define BUS_MMIO 0
#define BUS_PIO  1
#define BUS_CCW  2 /* s390's virtio notifications: registered, never written */
#define NR_BUSES 3

/* The most devices on one bus other than ioeventfds, which Linux's
 * kvm_io_bus_register_dev leaves out of NR_IOBUS_DEVS: here the zones. */
#define MAX_BUS_DEVS 1000u

/* One ioeventfd. `len` 0 matches any write at `addr`; otherwise a write of
 * exactly `len` bytes there, of `datamatch` unless `wildcard`. */
struct ioeventfd {
    struct ioeventfd *next;
    struct eventfd *ev;
    uint64_t addr;
    uint64_t datamatch;
    uint32_t len;
    int bus;
    int wildcard;
};

/* One coalesced zone: the writes that lie wholly in `size` bytes at `addr`
 * of bus `bus`. */
struct zone {
    struct zone *next;
    uint64_t addr;
    uint32_t size;
    int bus;
};

/* iobus.c's share of a VM. `nzones` counts each bus's zones, which an
 * ioeventfd's add is refused beside at MAX_BUS_DEVS, as Linux's is. */
struct iobus {
    struct ioeventfd *list;
    struct zone *zones;
    uint32_t nzones[NR_BUSES];
};

/** The VM's bus, made at first need, or NULL when memory is short. */
static struct iobus *bus_get(struct kvm_vm *vm)
{
    if (!vm->iobus) {
        vm->iobus = kzalloc(sizeof(struct iobus));
    }
    return (struct iobus *)vm->iobus;
}

/** The bus the flags `flags` name, as Linux's ioeventfd_bus_from_flags. */
static int bus_of(uint32_t flags)
{
    if (flags & KVM_IOEVENTFD_FLAG_PIO) {
        return BUS_PIO;
    }
    if (flags & KVM_IOEVENTFD_FLAG_VIRTIO_CCW_NOTIFY) {
        return BUS_CCW;
    }
    return BUS_MMIO;
}

/** Whether `p` and `q` would both match some write, which Linux refuses,
 * as its ioeventfd_check_collision. */
static int collide(const struct ioeventfd *p, const struct ioeventfd *q)
{
    return p->bus == q->bus && p->addr == q->addr &&
           (!p->len || !q->len ||
            (p->len == q->len && (p->wildcard || q->wildcard || p->datamatch == q->datamatch)));
}

/** KVM_IOEVENTFD's assign, as Linux's kvm_assign_ioeventfd. 0; -EINVAL
 * for a length not 0, 1, 2, 4 or 8, a range that wraps, an unknown flag or
 * DATAMATCH with length 0; -EEXIST for a collision; -ENOSPC for a full
 * bus; eventfd_get's error; -ENOMEM. */
static int32_t assign(struct kvm_vm *vm, const struct kvm_ioeventfd *a)
{
    struct iobus *b;
    struct ioeventfd *p, *q;
    int32_t err = 0;

    switch (a->len) {
    case 0:
    case 1:
    case 2:
    case 4:
    case 8:
        break;
    default:
        return -EINVAL;
    }
    if (a->addr + a->len < a->addr || (a->flags & ~KVM_IOEVENTFD_VALID_FLAG_MASK) ||
        (!a->len && (a->flags & KVM_IOEVENTFD_FLAG_DATAMATCH))) {
        return -EINVAL;
    }
    b = bus_get(vm);
    if (!b) {
        return -ENOMEM;
    }
    p = kzalloc(sizeof(*p));
    if (!p) {
        return -ENOMEM;
    }
    p->ev = eventfd_get(a->fd, &err);
    if (!p->ev) {
        kfree(p);
        return err;
    }
    p->addr = a->addr;
    p->len = a->len;
    p->bus = bus_of(a->flags);
    p->wildcard = !(a->flags & KVM_IOEVENTFD_FLAG_DATAMATCH);
    p->datamatch = p->wildcard ? 0 : a->datamatch;
    err = 0;
    for (q = b->list; q; q = q->next) {
        if (collide(p, q)) {
            err = -EEXIST;
            break;
        }
    }
    if (err == 0 && b->nzones[p->bus] >= MAX_BUS_DEVS) {
        err = -ENOSPC;
    }
    if (err != 0) {
        eventfd_put(p->ev);
        kfree(p);
        return err;
    }
    p->next = b->list;
    b->list = p;
    return 0;
}

/** KVM_IOEVENTFD's deassign, as Linux's kvm_deassign_ioeventfd: the one
 * ioeventfd of this eventfd, bus, address, length and match. 0; -ENOENT
 * for none; eventfd_get's error. */
static int32_t deassign(struct kvm_vm *vm, const struct kvm_ioeventfd *a)
{
    struct iobus *b = (struct iobus *)vm->iobus;
    struct ioeventfd **pp, *p;
    struct eventfd *ev;
    int wildcard = !(a->flags & KVM_IOEVENTFD_FLAG_DATAMATCH);
    int bus = bus_of(a->flags);
    int32_t err = 0;

    ev = eventfd_get(a->fd, &err);
    if (!ev) {
        return err;
    }
    err = -ENOENT;
    for (pp = b ? &b->list : NULL; pp && *pp; pp = &(*pp)->next) {
        p = *pp;
        if (p->bus != bus || p->ev != ev || p->addr != a->addr || p->len != a->len ||
            p->wildcard != wildcard || (!wildcard && p->datamatch != a->datamatch)) {
            continue;
        }
        *pp = p->next;
        eventfd_put(p->ev);
        kfree(p);
        err = 0;
        break;
    }
    eventfd_put(ev);
    return err;
}

/** Whether `len` bytes at `addr` lie wholly in the zone `z`, as Linux's
 * coalesced_mmio_in_range. */
static int zone_holds(const struct zone *z, uint64_t addr, uint32_t len)
{
    return addr + len >= addr && addr >= z->addr && addr + len <= z->addr + z->size;
}

/** KVM_REGISTER_COALESCED_MMIO, as Linux's
 * kvm_vm_ioctl_register_coalesced_mmio. 0; -EINVAL for a `pio` not 0 or 1;
 * -ENOSPC for a full bus; -ENOMEM. */
static int32_t zone_add(struct kvm_vm *vm, const struct kvm_coalesced_mmio_zone *a)
{
    struct iobus *b;
    struct zone *z;

    if (a->pio > 1u) {
        return -EINVAL;
    }
    b = bus_get(vm);
    z = b ? kzalloc(sizeof(*z)) : NULL;
    if (!z) {
        return -ENOMEM;
    }
    z->addr = a->addr;
    z->size = a->size;
    z->bus = a->pio ? BUS_PIO : BUS_MMIO;
    if (b->nzones[z->bus] >= MAX_BUS_DEVS) {
        kfree(z);
        return -ENOSPC;
    }
    b->nzones[z->bus]++;
    z->next = b->zones;
    b->zones = z;
    return 0;
}

/** KVM_UNREGISTER_COALESCED_MMIO, as Linux's
 * kvm_vm_ioctl_unregister_coalesced_mmio: every zone of this bus that holds
 * the whole range goes, and none is no error. 0; -EINVAL for a `pio` not 0
 * or 1. */
static int32_t zone_remove(struct kvm_vm *vm, const struct kvm_coalesced_mmio_zone *a)
{
    struct iobus *b = (struct iobus *)vm->iobus;
    struct zone **zp, *z;
    int bus = a->pio ? BUS_PIO : BUS_MMIO;

    if (a->pio > 1u) {
        return -EINVAL;
    }
    zp = b ? &b->zones : NULL;
    while (zp && *zp) {
        z = *zp;
        if (z->bus != bus || !zone_holds(z, a->addr, a->size)) {
            zp = &z->next;
            continue;
        }
        *zp = z->next;
        b->nzones[bus]--;
        kfree(z);
    }
    return 0;
}

int32_t kvm_iobus_ioctl(struct kvm_vm *vm, uint32_t request, uintptr_t arg)
{
    struct kvm_coalesced_mmio_zone z;
    struct kvm_ioeventfd a;

    if (request == KVM_REGISTER_COALESCED_MMIO || request == KVM_UNREGISTER_COALESCED_MMIO) {
        if (copy_from_user(&z, (const void __user *)arg, sizeof(z)) != 0) {
            return -EFAULT;
        }
        return request == KVM_REGISTER_COALESCED_MMIO ? zone_add(vm, &z) : zone_remove(vm, &z);
    }
    if (request != KVM_IOEVENTFD) {
        return -ENOTTY;
    }
    if (copy_from_user(&a, (const void __user *)arg, sizeof(a)) != 0) {
        return -EFAULT;
    }
    if (a.flags & KVM_IOEVENTFD_FLAG_DEASSIGN) {
        return deassign(vm, &a);
    }
    return assign(vm, &a);
}

/** The ioeventfd on bus `bus` a write of `len` bytes of `data` at `addr`
 * hits, as Linux's ioeventfd_in_range, or NULL. */
static struct ioeventfd *find(struct iobus *b, int bus, uint64_t addr, uint32_t len,
                              const uint8_t *data)
{
    struct ioeventfd *p;
    uint64_t value = 0;
    uint32_t i;

    for (i = 0; i < len && i < 8u; i++) {
        value |= (uint64_t)data[i] << (i * 8u);
    }
    for (p = b->list; p; p = p->next) {
        if (p->bus != bus || p->addr != addr) {
            continue;
        }
        if (!p->len || (p->len == len && (p->wildcard || p->datamatch == value))) {
            return p;
        }
    }
    return NULL;
}

/** Queue a write of `len` bytes of `data` at `addr` of bus `bus` on the
 * VM's ring when a zone holds it, as Linux's coalesced_mmio_write: 1 when
 * queued, 0 when no zone holds it or the ring has no room, which leaves the
 * write to the caller. The ring's page is the caller's to scribble on, so
 * `last` is read once and checked. */
static int coalesce(struct kvm_vm *vm, int bus, uint64_t addr, uint32_t len, const uint8_t *data)
{
    struct iobus *b = (struct iobus *)vm->iobus;
    struct kvm_coalesced_mmio_ring *ring = vm->ring;
    struct kvm_coalesced_mmio *e;
    struct zone *z;
    uint32_t insert;

    for (z = b->zones; z; z = z->next) {
        if (z->bus == bus && zone_holds(z, addr, len)) {
            break;
        }
    }
    if (!z || !ring || len > sizeof(e->data)) {
        return 0;
    }
    insert = ring->last;
    if (insert >= KVM_COALESCED_MMIO_MAX || (insert + 1u) % KVM_COALESCED_MMIO_MAX == ring->first) {
        return 0;
    }
    e = &ring->coalesced_mmio[insert];
    e->phys_addr = addr;
    e->len = len;
    k_memcpy(e->data, data, len);
    e->pio = bus == BUS_PIO;
    k_barrier(); /* the entry before the index another thread reads it by */
    ring->last = (insert + 1u) % KVM_COALESCED_MMIO_MAX;
    return 1;
}

/** Give one write to the bus: an ioeventfd it hits is signalled, and
 * otherwise a zone that holds it queues it. 1 when either took it. */
static int bus_write(struct kvm_vm *vm, int bus, uint64_t addr, uint32_t len, const uint8_t *data)
{
    struct ioeventfd *p = find((struct iobus *)vm->iobus, bus, addr, len, data);

    if (!p) {
        return coalesce(vm, bus, addr, len, data);
    }
    eventfd_signal(p->ev);
    kvm_event_add(KVM_EVENT_IOEVENTFD);
    return 1;
}

int kvm_iobus_write(struct kvm_vcpu *v)
{
    struct kvm_exit *ex = &v->exit;
    uint32_t i, size;

    if (!v->vm->iobus) {
        return 0;
    }
    if (ex->kind == KVM_X_MMIO) {
        return bus_write(v->vm, BUS_MMIO, ex->mmio.phys_addr, ex->mmio.len, ex->mmio.data);
    }
    /* A string OUT is one write for each element, as Linux's kernel_pio;
     * only a miss on the first goes to the caller, and the elements after
     * a later miss are dropped. */
    size = ex->io.size;
    for (i = 0; i < ex->io.count && (i + 1u) * size <= sizeof(ex->data); i++) {
        if (!bus_write(v->vm, BUS_PIO, ex->io.port, size, ex->data + i * size)) {
            return i != 0;
        }
    }
    return 1;
}

int32_t kvm_iobus_check_extension(long cap)
{
    switch (cap) {
    case KVM_CAP_IOEVENTFD:
    case KVM_CAP_IOEVENTFD_ANY_LENGTH:
    case KVM_CAP_IOEVENTFD_NO_LENGTH:
    case KVM_CAP_COALESCED_PIO:
        return 1;
    case KVM_CAP_COALESCED_MMIO:
        return KVM_COALESCED_MMIO_PAGE_OFFSET;
    default:
        return 0;
    }
}

void kvm_iobus_free(struct kvm_vm *vm)
{
    struct iobus *b = (struct iobus *)vm->iobus;
    struct ioeventfd *p;
    struct zone *z;

    if (!b) {
        return;
    }
    while (b->list) {
        p = b->list;
        b->list = p->next;
        eventfd_put(p->ev);
        kfree(p);
    }
    while (b->zones) {
        z = b->zones;
        b->zones = z->next;
        kfree(z);
    }
    kfree(b);
    vm->iobus = NULL;
}
