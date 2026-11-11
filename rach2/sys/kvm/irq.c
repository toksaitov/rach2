/* Full and split interrupt controllers, GSI routes, MSIs and irqfds.
 * The big lock owns the controllers; eventfd hooks defer legacy inputs. */

#include "kvm.h"

#include "../arch/x86/kcpu.h"
#include "../io/eventfd.h"
#include "../lib/kerrno.h"
#include "../lib/kstring.h"
#include "../lib/spinlock.h"
#include "../mm/kheap.h"
#include "../mm/usercopy.h"

/* The most GSIs KVM_CAP_SPLIT_IRQCHIP may reserve for the caller's I/O
 * APIC, Linux's MAX_NR_RESERVED_IOAPIC_PINS. */
#define MAX_IOAPIC_PINS KVM_MAX_IRQ_ROUTES

/* An MSI's address and data, as Linux's struct msi_msg reads them. */
#define MSI_ADDR_DEST_SHIFT 12
#define MSI_ADDR_LOGICAL    0x4u
#define MSI_ADDR_REDIR_HINT 0x8u
#define MSI_DATA_LEVEL      0x8000u
#define MSI_DATA_MODE_MASK  0x700u

/* The host's TSC-deadline timer, leaf 1 ECX bit 24. */
#define CPUID_DEADLINE (1u << 24)

/* One route of a GSI, while `valid`: an MSI, or for `sint` a synthetic
 * interrupt of a SynIC. */
struct route {
    uint32_t valid;
    uint32_t chips;  /* the IRQCHIP destinations, one bit per chip */
    uint32_t pin[3]; /* the pin of each destination in `chips` */
    uint32_t sint;   /* KVM_IRQ_ROUTING_HV_SINT */
    uint32_t address_lo;
    uint32_t address_hi;
    uint32_t data;
    uint32_t hv_vcpu, hv_sint; /* the SINT's VP index and number */
};

/*
 * An irqfd: the eventfd `ev`, whose rise raises `gsi`. `route` is the
 * GSI's route as KVM_SET_GSI_ROUTING last left it, copied under the VM's
 * `lock`, since the hook reads it from the signaller's context without the
 * big lock. `vm` is NULL once the VM is gone while the eventfd's last close
 * has taken the hook off and its hangup is yet to run, which then frees the
 * record.
 */
struct irqfd {
    struct eventfd_hook hook; /* first, for the hook's callbacks */
    struct kvm_vm *vm;
    struct eventfd *ev;
    struct eventfd *resample; /* held until this irqfd is removed, NULL for pulses */
    uint32_t gsi;
    struct route route; /* under the VM's `lock` */
    int pending;        /* a legacy delivery deferred to the big lock, under iv->lock */
    struct irqfd *next; /* the VM's list, under the big lock */
};

/* irq.c's share of a VM. */
struct irq_vm {
    uint32_t ioapic_pins; /* the GSIs KVM_CAP_SPLIT_IRQCHIP reserved */
    struct route *routes; /* by GSI, `nr_routes` of them */
    uint32_t nr_routes;
    struct irqfd *irqfds;
    int pending;          /* any irqfd needs the big lock, under `lock` */
    struct spinlock lock; /* the irqfds' `route` */
};

/** irq.c's share of `vm`, or NULL before the split controller. */
static struct irq_vm *irq_of(struct kvm_vm *vm)
{
    return (struct irq_vm *)vm->irq;
}

/** The interrupt the MSI `r` of `vm` raises, as Linux's kvm_set_msi_irq:
 * the destination in the address's bits 19:12, and with
 * KVM_X2APIC_API_USE_32BIT_IDS its bits 31:8 in the high address's. */
static void msi_irq(const struct kvm_vm *vm, const struct route *r, struct kvm_lapic_irq *irq)
{
    irq->dest_id = (r->address_lo >> MSI_ADDR_DEST_SHIFT) & 0xFFu;
    if (vm->x2apic_format) {
        irq->dest_id |= r->address_hi & 0xFFFFFF00u;
    }
    irq->vector = r->data & 0xFFu;
    irq->dest_mode = (r->address_lo & MSI_ADDR_LOGICAL) != 0;
    irq->trig_mode = (r->data & MSI_DATA_LEVEL) != 0;
    irq->delivery_mode = r->data & MSI_DATA_MODE_MASK;
    irq->redir_hint = (r->address_lo & MSI_ADDR_REDIR_HINT) != 0;
    irq->level = 1;
    irq->shorthand = 0;
}

/** Whether the MSI `r` is one `vm` refuses, as Linux's
 * kvm_msi_route_invalid: with KVM_X2APIC_API_USE_32BIT_IDS, one whose high
 * address has bits below the destination's. */
static int msi_invalid(const struct kvm_vm *vm, const struct route *r)
{
    return vm->x2apic_format && (r->address_hi & 0xFFu);
}

/** Raise the route `r` at `level`, as Linux's kvm_set_msi and
 * kvm_hv_set_sint: -EINVAL for an MSI `msi_invalid` refuses, -1 for a
 * level of 0 or no destination, else the APICs that took it. Safe from an
 * irqfd's hook. */
static int set_msi(struct kvm_vm *vm, const struct route *r, uint32_t level)
{
    struct kvm_lapic_irq irq;

    if (!r->sint && msi_invalid(vm, r)) {
        return -EINVAL;
    }
    if (!level) {
        return -1;
    }
    if (r->sint) {
        return kvm_hv_synic_set_irq(vm, r->hv_vcpu, r->hv_sint);
    }
    msi_irq(vm, r, &irq);
    return kvm_lapic_deliver(vm, NULL, &irq);
}

/** Raise all controller destinations of a route, or its one MSI/SINT:
 * the APICs or pins that took it, or -1 when none did, as Linux's
 * kvm_set_irq skips each negative result, so that an MSI `msi_invalid`
 * refuses is -1 here and -EINVAL only for KVM_SIGNAL_MSI. */
static int set_route(struct kvm_vm *vm, const struct route *r, uint32_t source, uint32_t level,
                     int status)
{
    uint32_t chip;
    int n, result = -1;

    if (!r->chips) {
        n = set_msi(vm, r, level);
        return n < 0 ? -1 : n;
    }
    for (chip = 0; chip < 3u; chip++) {
        if (!(r->chips & (1u << chip))) {
            continue;
        }
        n = chip == KVM_IRQCHIP_IOAPIC
                ? kvm_ioapic_set(vm, r->pin[chip], source, level != 0, status)
                : kvm_pic_set(vm, r->pin[chip] + chip * 8u, source, level != 0);
        if (n >= 0) {
            result = (result < 0 ? 0 : result) + n;
        }
    }
    return result;
}

/** Copy the route of `f`'s GSI into it, under the VM's lock. */
static void irqfd_update(struct irq_vm *iv, struct irqfd *f)
{
    struct route r;
    uint32_t flags;

    k_memset(&r, 0, sizeof(r));
    if (f->gsi < iv->nr_routes) {
        r = iv->routes[f->gsi];
    }
    flags = spin_lock_irqsave(&iv->lock);
    f->route = r;
    spin_unlock_irqrestore(&iv->lock, flags);
}

static uint32_t events[KVM_EVENT_TOTAL];
static struct spinlock events_lock = SPINLOCK_INIT_NAMED("events_lock");

/** Count one event under a lock of its own, since an irqfd's runs with an
 * eventfd's lock held and perhaps without the big lock. */
void kvm_event_add(enum kvm_event event)
{
    uint32_t flags = spin_lock_irqsave(&events_lock);

    events[event]++;
    spin_unlock_irqrestore(&events_lock, flags);
}

/** Read an event's count. */
uint32_t kvm_event_count(enum kvm_event event)
{
    return events[event];
}

/** The eventfd of the irqfd `h` rose: raise its GSI's route and take the
 * counter, as Linux's irqfd_wakeup. Runs with the eventfd's lock held and
 * interrupts off, perhaps without the big lock. */
static int irqfd_signal(struct eventfd_hook *h)
{
    struct irqfd *f = (struct irqfd *)h;
    struct irq_vm *iv = irq_of(f->vm);
    struct route r;
    uint32_t i;
    int deferred;

    spin_lock(&iv->lock);
    r = f->route;
    deferred = r.chips != 0 || f->resample != NULL;
    if (deferred) {
        f->pending = iv->pending = 1;
    }
    spin_unlock(&iv->lock);
    if (deferred) {
        for (i = 0; i < f->vm->nr_vcpus; i++) {
            kvm_lapic_wake(f->vm->vcpus[i]);
        }
    } else if (r.valid) {
        set_msi(f->vm, &r, 1);
        kvm_event_add(KVM_EVENT_IRQFD);
    }
    return 1;
}

/** Whether an interrupt-context irqfd is waiting for the big lock. */
int kvm_irq_pending(struct kvm_vm *vm)
{
    struct irq_vm *iv = irq_of(vm);
    uint32_t flags;
    int pending;

    if (!iv) {
        return 0;
    }
    flags = spin_lock_irqsave(&iv->lock);
    pending = iv->pending;
    spin_unlock_irqrestore(&iv->lock, flags);
    return pending;
}

/** Deliver deferred irqfds before guest entry, under the big lock. */
void kvm_irq_process(struct kvm_vm *vm)
{
    struct irq_vm *iv = irq_of(vm);
    struct irqfd *f;
    uint32_t flags;
    int pending;

    if (!iv) {
        return;
    }
    flags = spin_lock_irqsave(&iv->lock);
    iv->pending = 0;
    spin_unlock_irqrestore(&iv->lock, flags);
    for (f = iv->irqfds; f; f = f->next) {
        flags = spin_lock_irqsave(&iv->lock);
        pending = f->pending;
        f->pending = 0;
        spin_unlock_irqrestore(&iv->lock, flags);
        if (pending) {
            kvm_irq_set(vm, f->gsi, f->resample ? KVM_IRQ_SRC_RESAMPLE : KVM_IRQ_SRC_USER, 1, 0);
            if (!f->resample) {
                kvm_irq_set(vm, f->gsi, KVM_IRQ_SRC_USER, 0, 0);
            }
        }
    }
}

/** Take `f` off its VM's list. */
static void irqfd_unlink(struct irq_vm *iv, struct irqfd *f)
{
    struct irqfd **p;

    for (p = &iv->irqfds; *p; p = &(*p)->next) {
        if (*p == f) {
            *p = f->next;
            return;
        }
    }
}

/** Lower a removed resampler's source once no other irqfd shares it. */
static void irqfd_lower(struct irq_vm *iv, struct irqfd *f)
{
    struct irqfd *other;

    if (!f->resample) {
        return;
    }
    for (other = iv->irqfds; other; other = other->next) {
        if (other->resample && other->gsi == f->gsi) {
            return;
        }
    }
    kvm_irq_set(f->vm, f->gsi, KVM_IRQ_SRC_RESAMPLE, 0, 0);
}

/** The irqfd's eventfd closed for the last time: the irqfd goes, as
 * Linux's irqfd_deactivate on EPOLLHUP. Under the big lock. */
static void irqfd_hangup(struct eventfd_hook *h)
{
    struct irqfd *f = (struct irqfd *)h;

    if (f->vm) {
        irqfd_unlink(irq_of(f->vm), f);
        irqfd_lower(irq_of(f->vm), f);
    }
    if (f->resample) {
        eventfd_put(f->resample);
    }
    eventfd_put(f->ev);
    kfree(f);
}

/** Free `f`, taking its hook off the eventfd, unless the eventfd's last
 * close took it off first, whose hangup then frees it. */
static void irqfd_free(struct irq_vm *iv, struct irqfd *f)
{
    irqfd_unlink(iv, f);
    irqfd_lower(iv, f);
    if (eventfd_hook_remove(f->ev, &f->hook, NULL)) {
        if (f->resample) {
            eventfd_put(f->resample);
        }
        eventfd_put(f->ev);
        kfree(f);
    } else {
        f->vm = NULL;
    }
}

/**
 * KVM_IRQFD, as Linux's kvm_irqfd: assign raises the GSI `gsi` on each
 * rise of the eventfd `fd`, and at once when it is above 0 already;
 * deassign takes off every irqfd of that eventfd and GSI. 0; -EINVAL for an
 * unknown flag, RESAMPLE without the full controller, or no controller;
 * -EBUSY for an eventfd another irqfd of the VM has;
 * eventfd_get's error; -ENOMEM; -EFAULT.
 */
static int32_t irqfd(struct kvm_vm *vm, uintptr_t arg)
{
    struct irq_vm *iv = irq_of(vm);
    struct kvm_irqfd a;
    struct irqfd *f, *next;
    struct eventfd *ev;
    int32_t err = 0;
    int r;

    if (copy_from_user(&a, (const void __user *)arg, sizeof(a)) != 0) {
        return -EFAULT;
    }
    if (a.flags & ~(KVM_IRQFD_FLAG_DEASSIGN | KVM_IRQFD_FLAG_RESAMPLE)) {
        return -EINVAL;
    }
    if (a.flags & KVM_IRQFD_FLAG_DEASSIGN) {
        ev = eventfd_get((int)a.fd, &err);
        if (!ev) {
            return err;
        }
        if (iv) {
            for (f = iv->irqfds; f; f = next) {
                next = f->next;
                if (f->ev == ev && f->gsi == a.gsi) {
                    irqfd_free(iv, f);
                }
            }
        }
        eventfd_put(ev);
        return 0;
    }
    if (!iv || ((a.flags & KVM_IRQFD_FLAG_RESAMPLE) && vm->irqchip != KVM_IRQCHIP_KERNEL)) {
        return -EINVAL;
    }
    ev = eventfd_get((int)a.fd, &err);
    if (!ev) {
        return err;
    }
    for (f = iv->irqfds; f; f = f->next) {
        if (f->ev == ev) {
            eventfd_put(ev);
            return -EBUSY;
        }
    }
    f = kzalloc(sizeof(*f));
    if (!f) {
        eventfd_put(ev);
        return -ENOMEM;
    }
    if (a.flags & KVM_IRQFD_FLAG_RESAMPLE) {
        f->resample = eventfd_get((int)a.resamplefd, &err);
        if (!f->resample) {
            eventfd_put(ev);
            kfree(f);
            return err;
        }
    }
    f->hook.signal = irqfd_signal;
    f->hook.hangup = irqfd_hangup;
    f->vm = vm;
    f->ev = ev;
    f->gsi = a.gsi;
    irqfd_update(iv, f);
    f->next = iv->irqfds;
    iv->irqfds = f;
    r = eventfd_hook_add(ev, &f->hook);
    if (r == 1) {
        kvm_irq_set(vm, f->gsi, f->resample ? KVM_IRQ_SRC_RESAMPLE : KVM_IRQ_SRC_USER, 1, 0);
        if (!f->resample) {
            kvm_irq_set(vm, f->gsi, KVM_IRQ_SRC_USER, 0, 0);
        }
    }
    return 0;
}

/** A zeroed controller record with its lock made, or NULL. */
static struct irq_vm *irq_vm_alloc(void)
{
    struct irq_vm *iv = kzalloc(sizeof(*iv));

    if (iv) {
        spin_lock_init(&iv->lock, "kvm_irq_vm");
    }
    return iv;
}

/** KVM_ENABLE_CAP(KVM_CAP_SPLIT_IRQCHIP), as Linux takes it: the local
 * APICs in the kernel and the first `args[0]` GSIs the caller's I/O APIC's.
 * 0; -EINVAL for more pins than routes; -EEXIST once a controller is in
 * the kernel or a vCPU exists; -ENOMEM. */
int32_t kvm_irq_enable_split(struct kvm_vm *vm, const struct kvm_enable_cap *cap)
{
    struct irq_vm *iv;

    if (cap->args[0] > MAX_IOAPIC_PINS) {
        return -EINVAL;
    }
    if (vm->irqchip != KVM_IRQCHIP_NONE || vm->nr_vcpus) {
        return -EEXIST;
    }
    iv = irq_vm_alloc();
    if (!iv) {
        return -ENOMEM;
    }
    iv->ioapic_pins = (uint32_t)cap->args[0];
    vm->irq = iv;
    vm->irqchip = KVM_IRQCHIP_SPLIT;
    return 0;
}

/** Make the full controller and Linux's 40 default routes, before vCPUs. */
static int32_t create_irqchip(struct kvm_vm *vm)
{
    struct irq_vm *iv;
    struct route *r;
    uint32_t pin, chip;
    int32_t result;

    if (vm->irqchip != KVM_IRQCHIP_NONE) {
        return -EEXIST;
    }
    if (vm->nr_vcpus) {
        return -EINVAL;
    }
    iv = irq_vm_alloc();
    if (!iv) {
        return -ENOMEM;
    }
    iv->nr_routes = KVM_IOAPIC_NUM_PINS;
    iv->routes = kzalloc(iv->nr_routes * sizeof(*iv->routes));
    if (!iv->routes) {
        kfree(iv);
        return -ENOMEM;
    }
    result = kvm_pic_create(vm);
    if (!result) {
        result = kvm_ioapic_create(vm);
    }
    if (result) {
        kvm_pic_free(vm);
        kfree(iv->routes);
        kfree(iv);
        return result;
    }
    for (pin = 0; pin < iv->nr_routes; pin++) {
        r = &iv->routes[pin];
        r->valid = 1;
        r->chips = 1u << KVM_IRQCHIP_IOAPIC;
        r->pin[KVM_IRQCHIP_IOAPIC] = pin;
        if (pin < 16u) {
            chip = pin / 8u;
            r->chips |= 1u << chip;
            r->pin[chip] = pin & 7u;
        }
    }
    vm->irq = iv;
    vm->irqchip = KVM_IRQCHIP_KERNEL;
    return 0;
}

/** Copy one controller's state, preserving the unused ABI bytes. */
static int32_t irqchip_state(struct kvm_vm *vm, uintptr_t arg, int set)
{
    struct kvm_irqchip chip;

    if (copy_from_user(&chip, (const void __user *)arg, sizeof(chip)) != 0) {
        return -EFAULT;
    }
    if (vm->irqchip != KVM_IRQCHIP_KERNEL) {
        return -ENXIO;
    }
    if (chip.chip_id < KVM_IRQCHIP_IOAPIC) {
        kvm_pic_state(vm, chip.chip_id, &chip.chip.pic, set);
    } else if (chip.chip_id == KVM_IRQCHIP_IOAPIC) {
        kvm_ioapic_state(vm, &chip.chip.ioapic, set);
    } else {
        return -EINVAL;
    }
    return !set && copy_to_user((void __user *)arg, &chip, sizeof(chip)) ? -EFAULT : 0;
}

/** Validate and add one route; return 0 or -EINVAL for a conflict. */
static int route_add(struct kvm_vm *vm, struct route *t, struct kvm_irq_routing_entry e)
{
    if (e.type == KVM_IRQ_ROUTING_IRQCHIP && !e.flags && vm->irqchip == KVM_IRQCHIP_KERNEL) {
        uint32_t chip = e.u.irqchip.irqchip, pin = e.u.irqchip.pin;

        if (chip > KVM_IRQCHIP_IOAPIC || (t->valid && !t->chips) || (t->chips & (1u << chip)) ||
            pin >= (chip == KVM_IRQCHIP_IOAPIC ? KVM_IOAPIC_NUM_PINS : 8u)) {
            return -EINVAL;
        }
        t->chips |= 1u << chip;
        t->pin[chip] = pin;
    } else if (!t->valid && e.type == KVM_IRQ_ROUTING_MSI && !(e.flags & ~KVM_MSI_VALID_DEVID)) {
        t->address_lo = e.u.msi.address_lo;
        t->address_hi = e.u.msi.address_hi;
        t->data = e.u.msi.data;
        if (msi_invalid(vm, t)) {
            return -EINVAL;
        }
    } else if (!t->valid && e.type == KVM_IRQ_ROUTING_HV_SINT && !e.flags) {
        t->sint = 1;
        t->hv_vcpu = e.u.hv_sint.vcpu;
        t->hv_sint = e.u.hv_sint.sint;
    } else {
        return -EINVAL;
    }
    t->valid = 1;
    return 0;
}

/**
 * Replace the routing table and refresh each irqfd's route. A GSI has
 * one MSI or SINT, or at most one pin per in-kernel controller. Returns
 * 0; -EINVAL without a controller, for an unknown type or flag, a pin or
 * GSI out of range, or conflicting routes; -ENOMEM; -EFAULT.
 */
static int32_t set_routing(struct kvm_vm *vm, uintptr_t arg)
{
    struct irq_vm *iv = irq_of(vm);
    struct kvm_irq_routing head;
    struct kvm_irq_routing_entry *ue = NULL;
    struct route *table = NULL;
    struct irqfd *f;
    uint32_t i, n = 0;
    int32_t r;

    if (!iv) {
        return -EINVAL;
    }
    if (copy_from_user(&head, (const void __user *)arg, sizeof(head)) != 0) {
        return -EFAULT;
    }
    if (head.nr > KVM_MAX_IRQ_ROUTES || head.flags) {
        return -EINVAL;
    }
    if (head.nr) {
        ue = kmalloc(head.nr * sizeof(*ue));
        if (!ue) {
            return -ENOMEM;
        }
        if (copy_from_user(ue, (const void __user *)(arg + sizeof(head)), head.nr * sizeof(*ue)) !=
            0) {
            kfree(ue);
            return -EFAULT;
        }
    }
    r = -EINVAL;
    for (i = 0; i < head.nr; i++) {
        if (ue[i].gsi >= KVM_MAX_IRQ_ROUTES) {
            goto out;
        }
        if (ue[i].gsi + 1u > n) {
            n = ue[i].gsi + 1u;
        }
    }
    if (n) {
        r = -ENOMEM;
        table = kzalloc(n * sizeof(*table));
        if (!table) {
            goto out;
        }
    }
    r = -EINVAL;
    for (i = 0; i < head.nr; i++) {
        struct route *t = &table[ue[i].gsi];

        if (route_add(vm, t, ue[i])) {
            goto out;
        }
    }
    kfree(iv->routes);
    iv->routes = table;
    iv->nr_routes = n;
    table = NULL;
    for (f = iv->irqfds; f; f = f->next) {
        irqfd_update(iv, f);
    }
    r = 0;
out:
    kfree(table);
    kfree(ue);
    return r;
}

/** Raise `gsi` at `level`, as Linux's kvm_set_irq: -1 for a GSI with no
 * route or one that reached no APIC, else the APICs that took it. */
int kvm_irq_set(struct kvm_vm *vm, uint32_t gsi, uint32_t source, uint32_t level, int status)
{
    struct irq_vm *iv = irq_of(vm);

    if (!iv || gsi >= iv->nr_routes || !iv->routes[gsi].valid) {
        return -1;
    }
    return set_route(vm, &iv->routes[gsi], source, level, status);
}

/** Whether a GSI is routed to the named controller pin. */
static int route_matches(struct irq_vm *iv, uint32_t gsi, uint32_t chip, uint32_t pin)
{
    const struct route *r;

    if (!iv || gsi >= iv->nr_routes || chip > KVM_IRQCHIP_IOAPIC) {
        return 0;
    }
    r = &iv->routes[gsi];
    return r->valid && (r->chips & (1u << chip)) && r->pin[chip] == pin;
}

/** Deassert the shared resampling source before notifying its producers. */
int kvm_irq_resample(struct kvm_vm *vm, uint32_t chip, uint32_t pin)
{
    struct irq_vm *iv = irq_of(vm);
    struct irqfd *f;
    int found = 0;

    if (!iv) {
        return 0;
    }
    for (f = iv->irqfds; f; f = f->next) {
        if (f->resample && route_matches(iv, f->gsi, chip, pin)) {
            kvm_irq_set(vm, f->gsi, KVM_IRQ_SRC_RESAMPLE, 0, 0);
            found = 1;
        }
    }
    for (f = iv->irqfds; f; f = f->next) {
        if (f->resample && route_matches(iv, f->gsi, chip, pin)) {
            eventfd_signal(f->resample);
        }
    }
    return found;
}

/** A controller acknowledged a pin: notify the PIT and resampling irqfds. */
void kvm_irq_ack(struct kvm_vm *vm, uint32_t chip, uint32_t pin)
{
    if (route_matches(irq_of(vm), 0, chip, pin)) {
        kvm_pit_ack(vm);
    }
    kvm_irq_resample(vm, chip, pin);
}

/** Discard the PIT's accumulated ticks when its GSI is unmasked. */
void kvm_irq_mask(struct kvm_vm *vm, uint32_t chip, uint32_t pin, int masked)
{
    if (!masked && route_matches(irq_of(vm), 0, chip, pin)) {
        kvm_pit_unmask(vm);
    }
}

/** Take legacy-controller I/O inside KVM, including string port reads. */
int kvm_irq_io(struct kvm_vcpu *v)
{
    struct kvm_exit *ex = &v->exit;
    uint32_t size, i, value;
    uint64_t wide;
    int write, handled;

    if (v->vm->irqchip != KVM_IRQCHIP_KERNEL) {
        return 0;
    }
    if (ex->kind == KVM_X_MMIO) {
        size = ex->mmio.len;
        if (!size || size > 8u) {
            return 0;
        }
        wide = 0;
        write = ex->mmio.is_write != 0;
        k_memcpy(&wide, ex->mmio.data, size);
        handled = kvm_ioapic_io(v->vm, ex->mmio.phys_addr, size, &wide, write);
        if (!handled) {
            return 0;
        }
        if (!write) {
            k_memcpy(ex->mmio.data, &wide, size);
        }
    } else if (ex->kind == KVM_X_IO) {
        size = ex->io.size;
        if ((size != 1 && size != 2 && size != 4) || !ex->io.count || ex->io.count > 4096u / size) {
            return 0;
        }
        write = ex->io.direction == KVM_EXIT_IO_OUT;
        for (i = 0; i < ex->io.count; i++) {
            value = 0;
            k_memcpy(&value, ex->data + i * size, size);
            if (!kvm_pic_io(v->vm, ex->io.port, size, &value, write) &&
                !kvm_pit_io(v->vm, ex->io.port, size, &value, write)) {
                return 0;
            }
            if (!write) {
                k_memcpy(ex->data + i * size, &value, size);
            }
        }
    } else {
        return 0;
    }
    if (!write && kvm_backend && kvm_backend->complete) {
        kvm_backend->complete(v);
    }
    return 1;
}

/** KVM_IRQ_LINE and, with `status`, KVM_IRQ_LINE_STATUS, which writes
 * back what the line reached. 0; -ENXIO with no controller in the kernel;
 * -EFAULT. */
static int32_t irq_line(struct kvm_vm *vm, uintptr_t arg, int status)
{
    struct kvm_irq_level l;

    if (copy_from_user(&l, (const void __user *)arg, sizeof(l)) != 0) {
        return -EFAULT;
    }
    if (!irq_of(vm)) {
        return -ENXIO;
    }
    l.irq = kvm_irq_set(vm, (uint32_t)l.irq, KVM_IRQ_SRC_USER, l.level, status);
    if (status && copy_to_user((void __user *)arg, &l, sizeof(l)) != 0) {
        return -EFAULT;
    }
    return 0;
}

/** KVM_SIGNAL_MSI, as Linux's kvm_send_userspace_msi: the APICs that took
 * it, -EPERM (Linux's -1) when it named none; -EINVAL with no controller
 * in the kernel, an unknown flag or an MSI `msi_invalid` refuses;
 * -EFAULT. */
static int32_t signal_msi(struct kvm_vm *vm, uintptr_t arg)
{
    struct kvm_msi m;
    struct route r;
    int n;

    if (copy_from_user(&m, (const void __user *)arg, sizeof(m)) != 0) {
        return -EFAULT;
    }
    if (!irq_of(vm) || (m.flags & ~KVM_MSI_VALID_DEVID)) {
        return -EINVAL;
    }
    k_memset(&r, 0, sizeof(r));
    r.valid = 1;
    r.address_lo = m.address_lo;
    r.address_hi = m.address_hi;
    r.data = m.data;
    n = set_msi(vm, &r, 1);
    if (n == -EINVAL) {
        return n;
    }
    return n < 0 ? -EPERM : n;
}

int32_t kvm_irq_ioctl(struct kvm_vm *vm, uint32_t request, uintptr_t arg)
{
    switch (request) {
    case KVM_CREATE_IRQCHIP:
        return create_irqchip(vm);
    case KVM_GET_IRQCHIP:
        return irqchip_state(vm, arg, 0);
    case KVM_SET_IRQCHIP:
        return irqchip_state(vm, arg, 1);
    case KVM_CREATE_PIT:
    case KVM_CREATE_PIT2:
    case KVM_GET_PIT:
    case KVM_SET_PIT:
    case KVM_GET_PIT2:
    case KVM_SET_PIT2:
    case KVM_REINJECT_CONTROL:
        return kvm_pit_ioctl(vm, request, arg);
    case KVM_IRQ_LINE:
        return irq_line(vm, arg, 0);
    case KVM_IRQ_LINE_STATUS:
        return irq_line(vm, arg, 1);
    case KVM_SET_GSI_ROUTING:
        return set_routing(vm, arg);
    case KVM_IRQFD:
        return irqfd(vm, arg);
    case KVM_SIGNAL_MSI:
        return signal_msi(vm, arg);
    default:
        return -ENOTTY;
    }
}

int kvm_irq_eoi_exits(struct kvm_vm *vm, uint32_t vector)
{
    struct irq_vm *iv = irq_of(vm);
    uint32_t i, n;

    if (!iv || vm->irqchip != KVM_IRQCHIP_SPLIT) {
        return 0;
    }
    n = iv->ioapic_pins < iv->nr_routes ? iv->ioapic_pins : iv->nr_routes;
    for (i = 0; i < n; i++) {
        if (iv->routes[i].valid && !iv->routes[i].sint && (iv->routes[i].data & MSI_DATA_LEVEL) &&
            (iv->routes[i].data & 0xFFu) == vector) {
            return 1;
        }
    }
    return 0;
}

/** The host's TSC-deadline timer, which Linux requires to offer the
 * guest's. */
static int host_deadline(void)
{
    uint32_t a, b, c, d;

    kcpu_id(1, &a, &b, &c, &d);
    return (c & CPUID_DEADLINE) != 0;
}

int32_t kvm_irq_check_extension(long cap)
{
    switch (cap) {
    case KVM_CAP_IRQCHIP:
    case KVM_CAP_IRQFD:
    case KVM_CAP_IRQFD_RESAMPLE:
    case KVM_CAP_SIGNAL_MSI:
    case KVM_CAP_SPLIT_IRQCHIP:
    case KVM_CAP_IRQ_INJECT_STATUS:
    case KVM_CAP_PIT:
    case KVM_CAP_PIT2:
    case KVM_CAP_PIT_STATE2:
    case KVM_CAP_REINJECT_CONTROL:
        return 1;
    case KVM_CAP_IRQ_ROUTING:
        return (int32_t)KVM_MAX_IRQ_ROUTES;
    case KVM_CAP_TSC_DEADLINE_TIMER:
        return host_deadline();
    default:
        return 0;
    }
}

void kvm_irq_free(struct kvm_vm *vm)
{
    struct irq_vm *iv = irq_of(vm);

    if (!iv) {
        return;
    }
    while (iv->irqfds) {
        irqfd_free(iv, iv->irqfds);
    }
    kvm_pit_free(vm);
    kvm_ioapic_free(vm);
    kvm_pic_free(vm);
    kfree(iv->routes);
    kfree(iv);
    vm->irq = NULL;
}
