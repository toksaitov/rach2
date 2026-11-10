/*
 * irq.c: the split interrupt controller's side in the kernel, as Linux's
 * irqchip.c, irq_comm.c and the irqfd half of eventfd.c keep it on x86:
 * KVM_CAP_SPLIT_IRQCHIP, the GSI routes of MSIs and of Hyper-V's synthetic
 * interrupts, KVM_IRQ_LINE,
 * KVM_SIGNAL_MSI, and irqfds that raise a GSI when an eventfd is
 * signalled. The PIC and the I/O APIC are the caller's.
 */

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
    uint32_t sint; /* KVM_IRQ_ROUTING_HV_SINT */
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
    uint32_t gsi;
    struct route route; /* under the VM's `lock` */
    struct irqfd *next; /* the VM's list, under the big lock */
};

/* irq.c's share of a VM. */
struct irq_vm {
    uint32_t ioapic_pins; /* the GSIs KVM_CAP_SPLIT_IRQCHIP reserved */
    struct route *routes; /* by GSI, `nr_routes` of them */
    uint32_t nr_routes;
    struct irqfd *irqfds;
    struct spinlock lock; /* the irqfds' `route` */
};

/** irq.c's share of `vm`, or NULL before the split controller. */
static struct irq_vm *irq_of(struct kvm_vm *vm)
{
    return (struct irq_vm *)vm->irq;
}

/** The interrupt the MSI `r` raises, as Linux's kvm_set_msi_irq, with
 * the destination in the address's bits 19:12 alone, since no extended
 * destination is offered (KVM_CAP_X2APIC_API). */
static void msi_irq(const struct route *r, struct kvm_lapic_irq *irq)
{
    irq->dest_id = (r->address_lo >> MSI_ADDR_DEST_SHIFT) & 0xFFu;
    irq->vector = r->data & 0xFFu;
    irq->dest_mode = (r->address_lo & MSI_ADDR_LOGICAL) != 0;
    irq->trig_mode = (r->data & MSI_DATA_LEVEL) != 0;
    irq->delivery_mode = r->data & MSI_DATA_MODE_MASK;
    irq->redir_hint = (r->address_lo & MSI_ADDR_REDIR_HINT) != 0;
    irq->level = 1;
    irq->shorthand = 0;
}

/** Raise the route `r` at `level`, as Linux's kvm_set_msi and
 * kvm_hv_set_sint: -1 for a level of 0 or no destination, else the APICs
 * that took it. Safe from an irqfd's hook. */
static int set_msi(struct kvm_vm *vm, const struct route *r, uint32_t level)
{
    struct kvm_lapic_irq irq;

    if (!level) {
        return -1;
    }
    if (r->sint) {
        return kvm_hv_synic_set_irq(vm, r->hv_vcpu, r->hv_sint);
    }
    msi_irq(r, &irq);
    return kvm_lapic_deliver(vm, NULL, &irq);
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

/** The eventfd of the irqfd `h` rose: raise its GSI's route and take the
 * counter, as Linux's irqfd_wakeup. Runs with the eventfd's lock held and
 * interrupts off, perhaps without the big lock. */
static int irqfd_signal(struct eventfd_hook *h)
{
    struct irqfd *f = (struct irqfd *)h;
    struct irq_vm *iv = irq_of(f->vm);
    struct route r;

    spin_lock(&iv->lock);
    r = f->route;
    spin_unlock(&iv->lock);
    if (r.valid) {
        set_msi(f->vm, &r, 1);
    }
    return 1;
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

/** The irqfd's eventfd closed for the last time: the irqfd goes, as
 * Linux's irqfd_deactivate on EPOLLHUP. Under the big lock. */
static void irqfd_hangup(struct eventfd_hook *h)
{
    struct irqfd *f = (struct irqfd *)h;

    if (f->vm) {
        irqfd_unlink(irq_of(f->vm), f);
    }
    eventfd_put(f->ev);
    kfree(f);
}

/** Free `f`, taking its hook off the eventfd, unless the eventfd's last
 * close took it off first, whose hangup then frees it. */
static void irqfd_free(struct irq_vm *iv, struct irqfd *f)
{
    irqfd_unlink(iv, f);
    if (eventfd_hook_remove(f->ev, &f->hook, NULL)) {
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
 * unknown flag, RESAMPLE, which needs the I/O APIC in the kernel, or no
 * split controller; -EBUSY for an eventfd another irqfd of the VM has;
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
    if (!iv || (a.flags & KVM_IRQFD_FLAG_RESAMPLE)) {
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
    f->hook.signal = irqfd_signal;
    f->hook.hangup = irqfd_hangup;
    f->vm = vm;
    f->ev = ev;
    f->gsi = a.gsi;
    irqfd_update(iv, f);
    f->next = iv->irqfds;
    iv->irqfds = f;
    r = eventfd_hook_add(ev, &f->hook);
    if (r == 1 && f->route.valid) {
        set_msi(vm, &f->route, 1);
    }
    return 0;
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
    iv = kzalloc(sizeof(*iv));
    if (!iv) {
        return -ENOMEM;
    }
    iv->ioapic_pins = (uint32_t)cap->args[0];
    vm->irq = iv;
    vm->irqchip = KVM_IRQCHIP_SPLIT;
    return 0;
}

/**
 * KVM_SET_GSI_ROUTING, as Linux's kvm_set_irq_routing with the split
 * controller: the table replaced whole by `nr` routes of MSIs and SINTs,
 * one a GSI, and each irqfd given its GSI's new route. 0; -EINVAL with no
 * split controller, for more than KVM_MAX_IRQ_ROUTES routes, a flag, a GSI
 * past them, an irqchip route (the PIC and I/O APIC are the caller's), a
 * type not offered, or a second route of a GSI; -ENOMEM; -EFAULT.
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

        if (t->valid) {
            goto out;
        }
        if (ue[i].type == KVM_IRQ_ROUTING_MSI && !(ue[i].flags & ~KVM_MSI_VALID_DEVID)) {
            t->address_lo = ue[i].u.msi.address_lo;
            t->address_hi = ue[i].u.msi.address_hi;
            t->data = ue[i].u.msi.data;
        } else if (ue[i].type == KVM_IRQ_ROUTING_HV_SINT && !ue[i].flags) {
            t->sint = 1;
            t->hv_vcpu = ue[i].u.hv_sint.vcpu;
            t->hv_sint = ue[i].u.hv_sint.sint;
        } else {
            goto out;
        }
        t->valid = 1;
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
static int set_irq(struct kvm_vm *vm, uint32_t gsi, uint32_t level)
{
    struct irq_vm *iv = irq_of(vm);

    if (gsi >= iv->nr_routes || !iv->routes[gsi].valid) {
        return -1;
    }
    return set_msi(vm, &iv->routes[gsi], level);
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
    l.irq = set_irq(vm, (uint32_t)l.irq, l.level);
    if (status && copy_to_user((void __user *)arg, &l, sizeof(l)) != 0) {
        return -EFAULT;
    }
    return 0;
}

/** KVM_SIGNAL_MSI, as Linux's kvm_send_userspace_msi: the APICs that took
 * it, -EPERM (Linux's -1) when it named none; -EINVAL with no controller
 * in the kernel or an unknown flag; -EFAULT. */
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
    return n < 0 ? -EPERM : n;
}

int32_t kvm_irq_ioctl(struct kvm_vm *vm, uint32_t request, uintptr_t arg)
{
    switch (request) {
    case KVM_CREATE_IRQCHIP:
        /* The PIC and the I/O APIC in the kernel are not offered; QEMU
         * needs kernel-irqchip=split, which its PC machines do not
         * default to. */
        return vm->irqchip != KVM_IRQCHIP_NONE ? -EEXIST : -ENXIO;
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

    if (!iv) {
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
    kfree(iv->routes);
    kfree(iv);
    vm->irq = NULL;
}
