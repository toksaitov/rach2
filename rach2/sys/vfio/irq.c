/*
 * irq.c: a vfio-pci device's interrupts into its owner's eventfds, as
 * Linux's vfio_pci_intrs.c gives them: the line, masked by Interrupt
 * Disable at each interrupt until the owner unmasks it; MSI; MSI-X entry
 * by entry, each through its remapping entry when the host remaps; and the
 * error and request eventfds, which this kernel never signals.
 */

#include "vfio.h"

#include "../io/eventfd.h"
#include "../irq.h"
#include "../klog.h"
#include "../lib/kerrno.h"
#include "../lib/kstring.h"
#include "../mm/kheap.h"
#include "../mm/paging.h"
#include "../mm/usercopy.h"

#define CMD_MEMORY  0x0002u
#define STATUS_INTX 0x0008u /* the line is asserted */

/** The device's function. */
#define DEV(d) (d)->pci.bus, (d)->pci.device, (d)->pci.function

/* The interrupt each message vector VFIO took is for, or NULL. */
static struct vfio_irq *vector_irq[256];

/* The platform lines VFIO's line handler is on; it is never taken off,
 * since a line's chain keeps its handlers, and passes over a line that no
 * device of VFIO's has now. */
static unsigned char line_hooked[256];

/* An unmask eventfd's hook, which unmasks the line when the owner writes
 * the eventfd, Linux's virqfd for INTx. */
struct unmask_hook {
    struct eventfd_hook h; /* first, so that the hook is the record */
    struct vfio_device *d;
};

/** The interrupts the device has of the kind `index`, as Linux's
 * vfio_pci_get_irq_count counts them. */
uint32_t vfio_irq_count(const struct vfio_device *d, uint32_t index)
{
    uint16_t flags;

    switch (index) {
    case VFIO_PCI_INTX_IRQ_INDEX:
        return d->intx_ok ? 1u : 0u;
    case VFIO_PCI_MSI_IRQ_INDEX:
        if (d->msi_cap == 0) {
            return 0;
        }
        flags = pci_read_config16(DEV(d), (uint8_t)(d->msi_cap + 2));
        return 1u << ((flags >> 1) & 0x7u);
    case VFIO_PCI_MSIX_IRQ_INDEX:
        return d->msix_cap != 0 ? d->msix_entries : 0u;
    case VFIO_PCI_ERR_IRQ_INDEX:
        return d->pcie_cap != 0 ? 1u : 0u;
    case VFIO_PCI_REQ_IRQ_INDEX:
        return 1;
    default:
        return 0;
    }
}

/** The eventfd descriptor `i` of a SET_IRQS's data. */
static int32_t fd_at(const uint8_t *data, uint32_t i)
{
    int32_t fd;

    k_memcpy(&fd, data + 4 * i, sizeof(fd));
    return fd;
}

/** Write the command word for the line's state now. */
static void intx_apply(struct vfio_device *d)
{
    uint16_t cmd = pci_read_config16(DEV(d), PCI_COMMAND);

    pci_write_config16(DEV(d), PCI_COMMAND, vfio_config_command(d, cmd));
}

/** A message's interrupt: its eventfd's signal. */
static void msi_isr(isr_params_t *params)
{
    struct vfio_irq *q = vector_irq[params->vector & 0xFFu];

    if (q != NULL && q->trigger != NULL) {
        eventfd_signal(q->trigger);
    }
}

/**
 * A platform line's interrupt: each device of VFIO's on the line whose
 * line is asserted and not masked is masked by Interrupt Disable and its
 * eventfd signalled, as Linux's vfio_intx_handler with
 * pci_check_and_mask_intx. The owner unmasks it once it has served it.
 */
static void intx_isr(isr_params_t *params)
{
    unsigned int line = (unsigned int)params->vector - IRQ_VECTOR(0);
    int i;

    for (i = 0; i < vfio_ndevices; i++) {
        struct vfio_device *d = &vfio_devices[i];

        if (d->refs == 0 || d->irq_type != VFIO_PCI_INTX_IRQ_INDEX || d->pci.irq != line ||
            d->intx_masked) {
            continue;
        }
        if (pci_read_config16(DEV(d), PCI_STATUS) & STATUS_INTX) {
            d->intx_masked = 1;
            intx_apply(d);
            if (d->intx.trigger != NULL) {
                eventfd_signal(d->intx.trigger);
            }
        }
    }
}

/** Unmask the line, unless the device still asserts it: then it stays
 * masked and the eventfd is signalled again, as Linux's
 * __vfio_pci_intx_unmask. */
static void intx_unmask(struct vfio_device *d)
{
    if (d->irq_type != VFIO_PCI_INTX_IRQ_INDEX || !d->intx_masked) {
        return;
    }
    if (pci_read_config16(DEV(d), PCI_STATUS) & STATUS_INTX) {
        if (d->intx.trigger != NULL) {
            eventfd_signal(d->intx.trigger);
        }
        return;
    }
    d->intx_masked = 0;
    intx_apply(d);
}

/** The unmask eventfd rose: unmask, taking the count. */
static int unmask_signal(struct eventfd_hook *h)
{
    struct vfio_device *d = ((struct unmask_hook *)h)->d;

    if (d->irq_type == VFIO_PCI_INTX_IRQ_INDEX && d->intx_masked &&
        !(pci_read_config16(DEV(d), PCI_STATUS) & STATUS_INTX)) {
        d->intx_masked = 0;
        intx_apply(d);
    }
    return 1;
}

/** Drop the unmask eventfd, if any. */
static void unmask_drop(struct vfio_device *d)
{
    if (d->intx_unmask == NULL) {
        return;
    }
    (void)eventfd_hook_remove(d->intx_unmask, d->unmask_hook, NULL);
    eventfd_put(d->intx_unmask);
    kfree(d->unmask_hook);
    d->intx_unmask = NULL;
    d->unmask_hook = NULL;
}

/** Make the eventfd `fd` the line's unmask eventfd, or with -1 drop it. */
static int32_t unmask_set(struct vfio_device *d, int32_t fd)
{
    struct unmask_hook *u;
    struct eventfd *ev;
    int32_t err = 0;

    unmask_drop(d);
    if (fd < 0) {
        return 0;
    }
    ev = eventfd_get(fd, &err);
    if (ev == NULL) {
        return err;
    }
    u = (struct unmask_hook *)kzalloc(sizeof(*u));
    if (u == NULL) {
        eventfd_put(ev);
        return -ENOMEM;
    }
    u->h.signal = unmask_signal;
    u->d = d;
    d->intx_unmask = ev;
    d->unmask_hook = &u->h;
    if (eventfd_hook_add(ev, &u->h) == 1) {
        intx_unmask(d);
    }
    return 0;
}

/** Replace the eventfd `*slot` by the one of `fd`, or with -1 by none. */
static int32_t trigger_set(struct eventfd **slot, int32_t fd)
{
    struct eventfd *ev = NULL;
    int32_t err = 0;

    if (fd >= 0) {
        ev = eventfd_get(fd, &err);
        if (ev == NULL) {
            return err;
        }
    }
    if (*slot != NULL) {
        eventfd_put(*slot);
    }
    *slot = ev;
    return 0;
}

/** Stop the line: its eventfds dropped and Interrupt Disable on. */
static void intx_disable(struct vfio_device *d)
{
    unmask_drop(d);
    (void)trigger_set(&d->intx.trigger, -1);
    d->irq_type = -1;
    d->intx_masked = 0;
    intx_apply(d);
}

/** Take the line with the eventfd `fd`: 0 or the errno. */
static int32_t intx_enable(struct vfio_device *d, int32_t fd)
{
    int32_t r = trigger_set(&d->intx.trigger, fd);

    if (r != 0) {
        return r;
    }
    if (!line_hooked[d->pci.irq]) {
        register_isr_handler((uint8_t)IRQ_VECTOR(d->pci.irq), intx_isr, "vfio-intx");
        line_hooked[d->pci.irq] = 1;
    }
    d->irq_type = VFIO_PCI_INTX_IRQ_INDEX;
    d->intx_masked = 0;
    intx_apply(d);
    return 0;
}

/** Give back the vector of the interrupt `q` and drop its eventfd. */
static void vector_drop(struct vfio_irq *q)
{
    if (q->vector >= 0) {
        vector_irq[q->vector] = NULL;
        isr_free_vector(q->vector);
    }
    q->vector = -1;
    (void)trigger_set(&q->trigger, -1);
}

/** Turn memory decoding on, for a write of the MSI-X table: the command
 * word as it was, for `memory_restore`. */
static uint16_t memory_on(struct vfio_device *d)
{
    uint16_t cmd = pci_read_config16(DEV(d), PCI_COMMAND);

    if (!(cmd & CMD_MEMORY)) {
        pci_write_config16(DEV(d), PCI_COMMAND, (uint16_t)(cmd | CMD_MEMORY));
    }
    return cmd;
}

/** Put back the command word `memory_on` returned, if decoding was off. */
static void memory_restore(struct vfio_device *d, uint16_t cmd)
{
    if (!(cmd & CMD_MEMORY)) {
        pci_write_config16(DEV(d), PCI_COMMAND, cmd);
    }
}

/** The MSI-X table's kernel address while MSI-X is on. */
static uintptr_t msix_table(const struct vfio_device *d)
{
    return d->msix_win.va + (d->msix_offset & 0xFFFu);
}

/** Stop the messages: each entry masked and its vector given back, MSI or
 * MSI-X off, the table's window unmapped. */
static void msi_disable(struct vfio_device *d)
{
    unsigned int i;
    uint16_t cmd = memory_on(d);

    if (d->irq_type == VFIO_PCI_MSIX_IRQ_INDEX) {
        pci_msix_disable(&d->pci);
    } else {
        pci_msi_disable(&d->pci);
    }
    memory_restore(d, cmd);
    for (i = 0; i < d->nmsi; i++) {
        vector_drop(&d->msi[i]);
    }
    kfree(d->msi);
    d->msi = NULL;
    d->nmsi = 0;
    pci_bar_unmap(&d->msix_win);
    d->irq_type = -1;
    intx_apply(d);
}

/** Program entry or message `i` with its vector, as the hardware needs it
 * now. 0, or -EINVAL when the device takes no more. */
static int msi_program(struct vfio_device *d, unsigned int i)
{
    struct vfio_irq *q = &d->msi[i];

    if (d->irq_type == VFIO_PCI_MSI_IRQ_INDEX) {
        return pci_msi_enable(&d->pci, (uint8_t)q->vector) == 0 ? 0 : -EINVAL;
    }
    return pci_msix_entry_set(&d->pci, msix_table(d), i, (uint8_t)q->vector) == 0 ? 0 : -EINVAL;
}

/** Point message or entry `i` at the eventfd `fd`, with a vector of its
 * own, or with -1 mask it and give its vector back. */
static int32_t msi_set_vector(struct vfio_device *d, unsigned int i, int32_t fd)
{
    struct vfio_irq *q = &d->msi[i];
    int32_t r;
    int v;

    if (fd < 0) {
        if (d->irq_type == VFIO_PCI_MSIX_IRQ_INDEX) {
            pci_msix_entry_clear(&d->pci, i);
        }
        vector_drop(q);
        return 0;
    }
    r = trigger_set(&q->trigger, fd);
    if (r != 0 || q->vector >= 0) {
        return r;
    }
    v = isr_alloc_vector();
    if (v < 0) {
        (void)trigger_set(&q->trigger, -1);
        return -ENOSPC;
    }
    q->vector = v;
    vector_irq[v] = q;
    register_isr_handler((uint8_t)v, msi_isr,
                         d->irq_type == VFIO_PCI_MSIX_IRQ_INDEX ? "vfio-msix" : "vfio-msi");
    r = msi_program(d, i);
    if (r != 0) {
        vector_drop(q);
    }
    return r;
}

/** Turn on MSI, or MSI-X with its table mapped and held masked, with room
 * for every interrupt of the kind and none yet given a vector. */
static int32_t msi_enable(struct vfio_device *d, int msix)
{
    unsigned int n = msix ? d->msix_entries : 1u, i;
    uint32_t off = d->msix_offset & ~0xFFFu;
    uint32_t len = ((d->msix_offset & 0xFFFu) + d->msix_entries * 16u + 0xFFFu) & ~0xFFFu;

    d->msi = (struct vfio_irq *)kzalloc(n * sizeof(d->msi[0]));
    if (d->msi == NULL) {
        return -ENOMEM;
    }
    for (i = 0; i < n; i++) {
        d->msi[i].vdev = d;
        d->msi[i].index = i;
        d->msi[i].vector = -1;
    }
    d->nmsi = n;
    d->irq_type = msix ? VFIO_PCI_MSIX_IRQ_INDEX : VFIO_PCI_MSI_IRQ_INDEX;
    intx_apply(d);
    if (msix) {
        d->msix_win.bar_size = d->bar_size[d->msix_bar];
        if (pci_bar_map(&d->pci, d->msix_bar, off, len, PTE_NOCACHE, &d->msix_win) == 0 ||
            pci_msix_hold(&d->pci) != 0) {
            msi_disable(d);
            return -EINVAL;
        }
    }
    return 0;
}

/** The eventfds `fds` for interrupts [start, start + count) of a kind on. */
static int32_t msi_set_block(struct vfio_device *d, uint32_t start, uint32_t count,
                             const uint8_t *fds)
{
    uint16_t cmd = memory_on(d);
    int32_t r = 0;
    uint32_t i;

    for (i = 0; i < count && r == 0; i++) {
        r = msi_set_vector(d, start + i, fd_at(fds, i));
    }
    if (d->irq_type == VFIO_PCI_MSIX_IRQ_INDEX) {
        pci_msix_unmask(&d->pci);
    }
    memory_restore(d, cmd);
    return r;
}

/**
 * The TRIGGER action of MSI and MSI-X, as Linux's vfio_pci_set_msi_trigger:
 * no data and no count turns the kind off; eventfds turn it on if it is
 * not, and set the range's; no data or true signals the range's eventfds.
 * MSI of more than one message is answered 1, the messages there are,
 * since a remapped message is one vector, as Linux answers a short
 * allocation.
 */
static int32_t msi_trigger(struct vfio_device *d, const struct vfio_irq_set *h, const uint8_t *data)
{
    int msix = h->index == VFIO_PCI_MSIX_IRQ_INDEX;
    uint32_t i;
    int32_t r;

    if (d->irq_type == (int)h->index && h->count == 0 && (h->flags & VFIO_IRQ_SET_DATA_NONE)) {
        msi_disable(d);
        return 0;
    }
    if (d->irq_type != (int)h->index && d->irq_type != -1) {
        return -EINVAL;
    }
    if (h->flags & VFIO_IRQ_SET_DATA_EVENTFD) {
        if (d->irq_type != (int)h->index) {
            if (!msix && h->start + h->count > 1) {
                return 1;
            }
            r = msi_enable(d, msix);
            if (r != 0) {
                return r;
            }
            r = msi_set_block(d, h->start, h->count, data);
            if (r != 0) {
                msi_disable(d);
            }
            return r;
        }
        return msi_set_block(d, h->start, h->count, data);
    }
    if (d->irq_type != (int)h->index) {
        return -EINVAL;
    }
    for (i = h->start; i < h->start + h->count; i++) {
        int on = (h->flags & VFIO_IRQ_SET_DATA_NONE) || data[i - h->start];

        if (on && d->msi[i].trigger != NULL) {
            eventfd_signal(d->msi[i].trigger);
        }
    }
    return 0;
}

/** The line's MASK and UNMASK actions, as Linux's vfio_pci_set_intx_mask
 * and _unmask. */
static int32_t intx_mask_unmask(struct vfio_device *d, const struct vfio_irq_set *h,
                                const uint8_t *data, int unmask)
{
    int on;

    if (d->irq_type != VFIO_PCI_INTX_IRQ_INDEX || h->start != 0 || h->count != 1) {
        return -EINVAL;
    }
    on = (h->flags & VFIO_IRQ_SET_DATA_NONE) || ((h->flags & VFIO_IRQ_SET_DATA_BOOL) && data[0]);
    if (h->flags & VFIO_IRQ_SET_DATA_EVENTFD) {
        return unmask ? unmask_set(d, *data) : -ENOTTY;
    }
    if (on && unmask) {
        intx_unmask(d);
    } else if (on && !d->intx_masked) {
        d->intx_masked = 1;
        intx_apply(d);
    }
    return 0;
}

/** The line's TRIGGER action, as Linux's vfio_pci_set_intx_trigger. */
static int32_t intx_trigger(struct vfio_device *d, const struct vfio_irq_set *h,
                            const uint8_t *data)
{
    int is_intx = d->irq_type == VFIO_PCI_INTX_IRQ_INDEX;

    if (is_intx && h->count == 0 && (h->flags & VFIO_IRQ_SET_DATA_NONE)) {
        intx_disable(d);
        return 0;
    }
    if (!(is_intx || d->irq_type == -1) || h->start != 0 || h->count != 1) {
        return -EINVAL;
    }
    if (h->flags & VFIO_IRQ_SET_DATA_EVENTFD) {
        int32_t fd = fd_at(data, 0);

        if (is_intx) {
            return trigger_set(&d->intx.trigger, fd);
        }
        return fd < 0 ? 0 : intx_enable(d, fd);
    }
    if (!is_intx) {
        return -EINVAL;
    }
    if (((h->flags & VFIO_IRQ_SET_DATA_NONE) || data[0]) && d->intx.trigger != NULL) {
        eventfd_signal(d->intx.trigger);
    }
    return 0;
}

/** The TRIGGER action of the error and request eventfds, which hold one. */
static int32_t single_trigger(struct eventfd **slot, const struct vfio_irq_set *h,
                              const uint8_t *data)
{
    if (h->flags & VFIO_IRQ_SET_DATA_EVENTFD) {
        return h->count == 1 ? trigger_set(slot, *data) : -EINVAL;
    }
    if (h->count == 0 && (h->flags & VFIO_IRQ_SET_DATA_NONE)) {
        return trigger_set(slot, -1);
    }
    if (h->count == 1 && *slot != NULL && ((h->flags & VFIO_IRQ_SET_DATA_NONE) || data[0])) {
        eventfd_signal(*slot);
    }
    return h->count <= 1 ? 0 : -EINVAL;
}

/** Do the action of `h` with its data. -ENOTTY for an action the kind does
 * not have. */
static int32_t irq_dispatch(struct vfio_device *d, const struct vfio_irq_set *h,
                            const uint8_t *data)
{
    uint32_t action = h->flags & VFIO_IRQ_SET_ACTION_TYPE_MASK;

    switch (h->index) {
    case VFIO_PCI_INTX_IRQ_INDEX:
        if (action == VFIO_IRQ_SET_ACTION_MASK || action == VFIO_IRQ_SET_ACTION_UNMASK) {
            return intx_mask_unmask(d, h, data, action == VFIO_IRQ_SET_ACTION_UNMASK);
        }
        return action == VFIO_IRQ_SET_ACTION_TRIGGER ? intx_trigger(d, h, data) : -ENOTTY;
    case VFIO_PCI_MSI_IRQ_INDEX:
    case VFIO_PCI_MSIX_IRQ_INDEX:
        return action == VFIO_IRQ_SET_ACTION_TRIGGER ? msi_trigger(d, h, data) : -ENOTTY;
    case VFIO_PCI_ERR_IRQ_INDEX:
        return action == VFIO_IRQ_SET_ACTION_TRIGGER ? single_trigger(&d->err_trigger, h, data)
                                                     : -ENOTTY;
    default:
        return action == VFIO_IRQ_SET_ACTION_TRIGGER ? single_trigger(&d->req_trigger, h, data)
                                                     : -ENOTTY;
    }
}

/**
 * VFIO_DEVICE_SET_IRQS: the header checked as Linux's
 * vfio_set_irqs_validate_and_prepare checks it, its data copied in, and
 * its action done.
 */
int32_t vfio_irq_set(struct vfio_device *d, uintptr_t arg)
{
    struct vfio_irq_set h;
    uint32_t size, max;
    uint8_t *data = NULL;
    int32_t r;

    if (copy_from_user(&h, (const void __user *)arg, sizeof(h)) != 0) {
        return -EFAULT;
    }
    max = h.index < VFIO_PCI_NUM_IRQS ? vfio_irq_count(d, h.index) : 0;
    if (h.argsz < sizeof(h) || h.index >= VFIO_PCI_NUM_IRQS || h.start >= max ||
        h.count >= 0xFFFFFFFFu - h.start || h.start + h.count > max ||
        (h.flags & ~(VFIO_IRQ_SET_DATA_TYPE_MASK | VFIO_IRQ_SET_ACTION_TYPE_MASK)) != 0) {
        return -EINVAL;
    }
    switch (h.flags & VFIO_IRQ_SET_DATA_TYPE_MASK) {
    case VFIO_IRQ_SET_DATA_NONE:
        size = 0;
        break;
    case VFIO_IRQ_SET_DATA_BOOL:
        size = 1;
        break;
    case VFIO_IRQ_SET_DATA_EVENTFD:
        size = 4;
        break;
    default:
        return -EINVAL;
    }
    if (size != 0) {
        if (h.argsz - sizeof(h) < h.count * size) {
            return -EINVAL;
        }
        data = (uint8_t *)kmalloc(h.count * size);
        if (data == NULL) {
            return -ENOMEM;
        }
        if (copy_from_user(data, (const void __user *)(arg + sizeof(h)), h.count * size) != 0) {
            kfree(data);
            return -EFAULT;
        }
    }
    r = irq_dispatch(d, &h, data);
    kfree(data);
    return r;
}

/** Stop every interrupt of the device and drop every eventfd: at the
 * device's release and before a reset. */
void vfio_irq_disable(struct vfio_device *d)
{
    if (d->irq_type == VFIO_PCI_INTX_IRQ_INDEX) {
        intx_disable(d);
    } else if (d->irq_type == VFIO_PCI_MSI_IRQ_INDEX || d->irq_type == VFIO_PCI_MSIX_IRQ_INDEX) {
        msi_disable(d);
    }
    (void)trigger_set(&d->err_trigger, -1);
    (void)trigger_set(&d->req_trigger, -1);
}

/** Program the interrupts again after a reset, which lost them, as
 * Linux's pci_restore_msi_state. */
void vfio_irq_restore(struct vfio_device *d)
{
    uint16_t cmd;
    unsigned int i;

    if (d->irq_type != VFIO_PCI_MSI_IRQ_INDEX && d->irq_type != VFIO_PCI_MSIX_IRQ_INDEX) {
        intx_apply(d);
        return;
    }
    cmd = memory_on(d);
    if (d->irq_type == VFIO_PCI_MSIX_IRQ_INDEX && pci_msix_hold(&d->pci) != 0) {
        klog(KLOG_WARN, "vfio", "reset: %s: MSI-X lost", d->name);
    }
    for (i = 0; i < d->nmsi; i++) {
        if (d->msi[i].vector >= 0 && msi_program(d, i) != 0) {
            klog(KLOG_WARN, "vfio", "reset: %s: interrupt %u lost", d->name, i);
        }
    }
    if (d->irq_type == VFIO_PCI_MSIX_IRQ_INDEX) {
        pci_msix_unmask(&d->pci);
    }
    memory_restore(d, cmd);
    intx_apply(d);
}
