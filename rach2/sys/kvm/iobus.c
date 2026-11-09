/*
 * iobus.c: the in-kernel bus of ioeventfds, as Linux's kvm_io_bus and the
 * ioeventfd half of eventfd.c: a guest's write to a port or an MMIO address
 * that an ioeventfd names signals its eventfd and goes no further, so the
 * guest enters again without a trip to the caller.
 */

#include "kvm.h"

#include "../io/eventfd.h"
#include "../lib/kerrno.h"
#include "../mm/kheap.h"
#include "../mm/usercopy.h"

/* The buses an ioeventfd sits on, by its flags. */
#define BUS_MMIO 0
#define BUS_PIO  1
#define BUS_CCW  2 /* s390's virtio notifications: registered, never written */
#define NR_BUSES 3

/* The most devices on one bus, Linux's NR_IOBUS_DEVS. */
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

/* iobus.c's share of a VM. */
struct iobus {
    struct ioeventfd *list;
    uint32_t count[NR_BUSES];
};

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
    struct iobus *b = (struct iobus *)vm->iobus;
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
    if (!b) {
        b = kzalloc(sizeof(*b));
        if (!b) {
            return -ENOMEM;
        }
        vm->iobus = b;
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
    if (err == 0 && b->count[p->bus] >= MAX_BUS_DEVS) {
        err = -ENOSPC;
    }
    if (err != 0) {
        eventfd_put(p->ev);
        kfree(p);
        return err;
    }
    b->count[p->bus]++;
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
        b->count[bus]--;
        eventfd_put(p->ev);
        kfree(p);
        err = 0;
        break;
    }
    eventfd_put(ev);
    return err;
}

int32_t kvm_iobus_ioctl(struct kvm_vm *vm, uint32_t request, uintptr_t arg)
{
    struct kvm_ioeventfd a;

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

int kvm_iobus_write(struct kvm_vcpu *v)
{
    struct iobus *b = (struct iobus *)v->vm->iobus;
    struct kvm_exit *ex = &v->exit;
    struct ioeventfd *p;
    uint32_t i, size;

    if (!b) {
        return 0;
    }
    if (ex->kind == KVM_X_MMIO) {
        p = find(b, BUS_MMIO, ex->mmio.phys_addr, ex->mmio.len, ex->mmio.data);
        if (!p) {
            return 0;
        }
        eventfd_signal(p->ev);
        return 1;
    }
    /* A string OUT signals once for each element, as Linux's kernel_pio;
     * only a miss on the first goes to the caller, and the elements after
     * a later miss are dropped. */
    size = ex->io.size;
    for (i = 0; i < ex->io.count && (i + 1u) * size <= sizeof(ex->data); i++) {
        p = find(b, BUS_PIO, ex->io.port, size, ex->data + i * size);
        if (!p) {
            return i != 0;
        }
        eventfd_signal(p->ev);
    }
    return 1;
}

int32_t kvm_iobus_check_extension(long cap)
{
    switch (cap) {
    case KVM_CAP_IOEVENTFD:
    case KVM_CAP_IOEVENTFD_ANY_LENGTH:
    case KVM_CAP_IOEVENTFD_NO_LENGTH:
        return 1;
    default:
        return 0;
    }
}

void kvm_iobus_free(struct kvm_vm *vm)
{
    struct iobus *b = (struct iobus *)vm->iobus;
    struct ioeventfd *p;

    if (!b) {
        return;
    }
    while (b->list) {
        p = b->list;
        b->list = p->next;
        eventfd_put(p->ev);
        kfree(p);
    }
    kfree(b);
    vm->iobus = NULL;
}
