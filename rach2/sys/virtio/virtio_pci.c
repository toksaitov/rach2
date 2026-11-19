/*
 * virtio_pci.c: the virtio PCI transport: a function's vendor capabilities
 * and the windows they name, its common configuration, its queues'
 * notifications, and its interrupts by MSI-X or by its INTx line; or, for
 * a function without the capabilities, the legacy interface in its I/O
 * BAR.
 */

#include "virtio.h"

#include "../fs/synthfs.h"
#include "../fs/sysfs.h"
#include "../vfio/vfio.h"
#include "../irq.h"
#include "../klog.h"
#include "../pci.h"
#include "../lib/kerrno.h"
#include "../lib/kstring.h"
#include "../lib/spinlock.h"
#include "../mm/kheap.h"
#include "../mm/paging.h"

/* The vendor of every virtio-pci function, and its device ids (virtio 1.2,
 * 4.1.2): 0x1040 plus the device type for a device of virtio 1.x alone,
 * 0x1000 to 0x103F for a transitional one, whose type is then its
 * subsystem device id. */
#define VP_VENDOR            0x1AF4u
#define VP_MODERN_BASE       0x1040u
#define VP_MODERN_LAST       0x107Fu
#define VP_TRANSITION_FIRST  0x1000u
#define VP_TRANSITION_LAST   0x103Fu
#define PCI_SUBSYSTEM_VENDOR 0x2C
#define PCI_SUBSYSTEM_ID     0x2E

/* The vendor capability (virtio 1.2, 4.1.4): cap_vndr, cap_next, cap_len,
 * cfg_type, bar, id, two bytes of padding, offset and length, each 32 bits;
 * the notification capability adds its multiplier, and the shared memory
 * capability the upper halves of its offset and length. */
#define PCI_CAP_ID_VNDR   0x09
#define VP_CAP_LEN        2
#define VP_CAP_TYPE       3
#define VP_CAP_BAR        4
#define VP_CAP_ID         5
#define VP_CAP_OFFSET     8
#define VP_CAP_LENGTH     12
#define VP_CAP_MULTIPLIER 16
#define VP_CAP_OFFSET_HI  16
#define VP_CAP_LENGTH_HI  20

#define VP_CFG_COMMON 1
#define VP_CFG_NOTIFY 2
#define VP_CFG_ISR    3
#define VP_CFG_DEVICE 4
#define VP_CFG_SHM    8

/* The common configuration (virtio 1.2, 4.1.4.3). */
#define VP_DEVICE_FEATURE_SELECT 0x00
#define VP_DEVICE_FEATURE        0x04
#define VP_DRIVER_FEATURE_SELECT 0x08
#define VP_DRIVER_FEATURE        0x0C
#define VP_CONFIG_MSIX_VECTOR    0x10
#define VP_NUM_QUEUES            0x12
#define VP_DEVICE_STATUS         0x14
#define VP_CONFIG_GENERATION     0x15
#define VP_QUEUE_SELECT          0x16
#define VP_QUEUE_SIZE            0x18
#define VP_QUEUE_MSIX_VECTOR     0x1A
#define VP_QUEUE_ENABLE          0x1C
#define VP_QUEUE_NOTIFY_OFF      0x1E
#define VP_QUEUE_DESC            0x20
#define VP_QUEUE_DRIVER          0x28
#define VP_QUEUE_DEVICE          0x30
#define VP_COMMON_MIN            0x38

/* The legacy interface (virtio 1.2, 4.1.4.10), in BAR0's ports: the
 * device's 32 feature bits and the driver's, the selected queue's ring as
 * a page number, its size, which the device sets, the selector, the
 * notification, the status and the ISR status; then, while MSI-X is on,
 * the configuration's and the selected queue's entries; then the device
 * configuration, at 0x18 with MSI-X on and 0x14 without. */
#define VPL_HOST_FEATURES  0x00
#define VPL_GUEST_FEATURES 0x04
#define VPL_QUEUE_PFN      0x08
#define VPL_QUEUE_NUM      0x0C
#define VPL_QUEUE_SEL      0x0E
#define VPL_QUEUE_NOTIFY   0x10
#define VPL_STATUS         0x12
#define VPL_ISR            0x13
#define VPL_CONFIG_VECTOR  0x14
#define VPL_QUEUE_VECTOR   0x16
#define VPL_CONFIG_MSIX    0x18
#define VPL_CONFIG         0x14
#define VPL_PFN_SHIFT      12

/* The ISR status byte, read only on the INTx line, which its read clears. */
#define VP_ISR_QUEUE  0x1u
#define VP_ISR_CONFIG 0x2u

/* The interrupt slots the devices share, the shared memory regions kept
 * for each, and the most of a window that is mapped. */
#define VP_SLOTS   32
#define VP_SHM     4
#define VP_MAP_MAX 0x10000u

/* How a device interrupts: not yet, by an MSI-X entry for each queue and
 * one for the configuration, by one entry for the configuration and one
 * the queues share, by its INTx line, or not at all, polled. */
#define VP_IRQ_NONE   0
#define VP_IRQ_MSIX   1
#define VP_IRQ_SHARED 2
#define VP_IRQ_INTX   3
#define VP_IRQ_POLLED 4

/* An interrupt slot's target: the configuration, every queue, or the queue
 * of that number. */
#define VP_TARGET_CONFIG (-1)
#define VP_TARGET_QUEUES (-2)

/* A window a capability names, as mapped. */
struct vp_window {
    uintptr_t base; /* the kernel's address once mapped, else the physical */
    uint32_t len;   /* the bytes mapped, 0 when absent */
    int mapped;     /* whether `base` is the kernel's, given to mmio_unmap */
    uint8_t bar;
    uint32_t offset; /* into the BAR */
};

struct vp_shm {
    uint8_t id;
    uint64_t addr, len;
};

/* A function the transport took, kept from its probe on the list `vp_list`,
 * in the order of the bus, until it is unbound (`vp_unbind`). An unbound
 * record a driver still holds stays, off the list and reaching no register,
 * until the driver's virtio_put. */
struct virtio_pci {
    struct virtio_dev dev;
    pci_device_t pci;
    struct virtio_pci *next;
    uint16_t command; /* the command word before the probe, less bus mastering */
    int claimed;      /* the probe made the kernel the function's PCI owner */
    int held;         /* a driver has `dev`, from virtio_pci_find to virtio_put */
    int gone;         /* unbound: its windows given back, its registers out of reach */
    struct vp_window common, notify, isr, device;
    uint32_t notify_mult; /* bytes between queues' notification addresses */
    struct vp_shm shm[VP_SHM];
    int nshm;
    int mode;              /* VP_IRQ_* */
    uintptr_t msix_table;  /* mapped, 0 until needed */
    uint32_t msix_entries; /* its entries */
    int vector[VP_SLOTS];  /* by MSI-X entry, -1 when none */
    int slot[VP_SLOTS];    /* by MSI-X entry, -1 when none */
    int nentries;          /* entries in use */
    int line;              /* the INTx line, while on it */
    uint16_t io;           /* a legacy function's first port, BAR0 */
    uint16_t io_len;       /* its ports */
};

/* An interrupt slot: which device and target its vector serves, and how
 * many of its handlers run now, which vp_msix_free waits out. */
struct vp_slot {
    struct virtio_pci *vp;
    int target;
    uint32_t inflight;
};

static struct virtio_pci *vp_list;
static int vp_probed; /* whether `virtio_pci_probe` has run */
static struct vp_slot vp_slots[VP_SLOTS];

/* The slots' owners and counts, which a handler without the big lock reads
 * while vp_msix_free, under it, gives a slot back. */
static struct spinlock vp_slots_lock = SPINLOCK_INIT_NAMED("virtio_slots_lock");

/** The record behind `d`. */
static struct virtio_pci *vp_of(struct virtio_dev *d)
{
    return (struct virtio_pci *)d->transport;
}

/*
 * The common configuration's fields. Once the function is unbound its
 * window is gone: a read gives 0, as of a device reset, and a write is
 * dropped, for a driver that has not yet let go of the record.
 */

/** Read the 8-bit field at `off` of the common configuration. */
static uint8_t vp_r8(struct virtio_pci *vp, uint32_t off)
{
    return vp->gone ? 0 : mmio_read8(vp->common.base + off);
}

/** Read the 16-bit field at `off` of the common configuration. */
static uint16_t vp_r16(struct virtio_pci *vp, uint32_t off)
{
    return vp->gone ? 0 : mmio_read16(vp->common.base + off);
}

/** Read the 32-bit field at `off` of the common configuration. */
static uint32_t vp_r32(struct virtio_pci *vp, uint32_t off)
{
    return vp->gone ? 0 : mmio_read32(vp->common.base + off);
}

/** Write the 8-bit field at `off` of the common configuration. */
static void vp_w8(struct virtio_pci *vp, uint32_t off, uint8_t v)
{
    if (!vp->gone) {
        mmio_write8(vp->common.base + off, v);
    }
}

/** Write the 16-bit field at `off` of the common configuration. */
static void vp_w16(struct virtio_pci *vp, uint32_t off, uint16_t v)
{
    if (!vp->gone) {
        mmio_write16(vp->common.base + off, v);
    }
}

/** Write the 32-bit field at `off` of the common configuration. */
static void vp_w32(struct virtio_pci *vp, uint32_t off, uint32_t v)
{
    if (!vp->gone) {
        mmio_write32(vp->common.base + off, v);
    }
}

/** A 64-bit field as two 32-bit writes, low first (virtio 1.2, 4.1.3.1). */
static void vp_w64(struct virtio_pci *vp, uint32_t off, uint64_t v)
{
    vp_w32(vp, off, (uint32_t)v);
    vp_w32(vp, off + 4u, (uint32_t)(v >> 32));
}

/*
 * The legacy interface's registers, in BAR0's ports, which a legacy
 * function decodes once vp_attach has turned I/O decoding on. Once the
 * function is unbound, a read gives 0 and a write is dropped, as for the
 * common configuration.
 */

/** Read the 8-bit legacy register at `off`. */
static uint8_t vpl_r8(struct virtio_pci *vp, uint32_t off)
{
    return vp->gone ? 0 : pci_io_read8((uint16_t)(vp->io + off));
}

/** Read the 16-bit legacy register at `off`. */
static uint16_t vpl_r16(struct virtio_pci *vp, uint32_t off)
{
    return vp->gone ? 0 : pci_io_read16((uint16_t)(vp->io + off));
}

/** Read the 32-bit legacy register at `off`. */
static uint32_t vpl_r32(struct virtio_pci *vp, uint32_t off)
{
    return vp->gone ? 0 : pci_io_read32((uint16_t)(vp->io + off));
}

/** Write the 8-bit legacy register at `off`. */
static void vpl_w8(struct virtio_pci *vp, uint32_t off, uint8_t v)
{
    if (!vp->gone) {
        pci_io_write8((uint16_t)(vp->io + off), v);
    }
}

/** Write the 16-bit legacy register at `off`. */
static void vpl_w16(struct virtio_pci *vp, uint32_t off, uint16_t v)
{
    if (!vp->gone) {
        pci_io_write16((uint16_t)(vp->io + off), v);
    }
}

/** Write the 32-bit legacy register at `off`. */
static void vpl_w32(struct virtio_pci *vp, uint32_t off, uint32_t v)
{
    if (!vp->gone) {
        pci_io_write32((uint16_t)(vp->io + off), v);
    }
}

/**
 * Give the configuration MSI-X entry `entry` and read back what the device
 * took. A legacy function has the register only while MSI-X is on, which
 * it is while `vp` holds entries (vp_msix_take); without them the port is
 * the device configuration's, and nothing is written.
 */
static uint16_t vp_config_vector(struct virtio_pci *vp, uint16_t entry)
{
    if (!vp->dev.legacy) {
        vp_w16(vp, VP_CONFIG_MSIX_VECTOR, entry);
        return vp_r16(vp, VP_CONFIG_MSIX_VECTOR);
    }
    if (vp->nentries == 0) {
        return VIRTIO_NO_VECTOR;
    }
    vpl_w16(vp, VPL_CONFIG_VECTOR, entry);
    return vpl_r16(vp, VPL_CONFIG_VECTOR);
}

/** The ISR status byte, which its read clears. */
static uint8_t vp_isr_status(struct virtio_pci *vp)
{
    if (vp->gone) {
        return 0;
    }
    return vp->dev.legacy ? pci_io_read8((uint16_t)(vp->io + VPL_ISR)) : mmio_read8(vp->isr.base);
}

/** The device status byte; `get_status` of virtio.h. */
static uint8_t vp_get_status(struct virtio_dev *d)
{
    return vp_r8(vp_of(d), VP_DEVICE_STATUS);
}

/** Write the device status byte; `set_status` of virtio.h. */
static void vp_set_status(struct virtio_dev *d, uint8_t status)
{
    vp_w8(vp_of(d), VP_DEVICE_STATUS, status);
}

/** The device's 64 feature bits, read as two words through the select
 * field. */
static uint64_t vp_get_features(struct virtio_dev *d)
{
    struct virtio_pci *vp = vp_of(d);
    uint32_t lo, hi;

    vp_w32(vp, VP_DEVICE_FEATURE_SELECT, 0);
    lo = vp_r32(vp, VP_DEVICE_FEATURE);
    vp_w32(vp, VP_DEVICE_FEATURE_SELECT, 1);
    hi = vp_r32(vp, VP_DEVICE_FEATURE);
    return ((uint64_t)hi << 32) | lo;
}

/** Write the driver's 64 feature bits, two words through the select
 * field. */
static void vp_set_features(struct virtio_dev *d, uint64_t features)
{
    struct virtio_pci *vp = vp_of(d);

    vp_w32(vp, VP_DRIVER_FEATURE_SELECT, 0);
    vp_w32(vp, VP_DRIVER_FEATURE, (uint32_t)features);
    vp_w32(vp, VP_DRIVER_FEATURE_SELECT, 1);
    vp_w32(vp, VP_DRIVER_FEATURE, (uint32_t)(features >> 32));
}

/** The configuration generation; `config_generation` of virtio.h. */
static uint32_t vp_config_generation(struct virtio_dev *d)
{
    return vp_r8(vp_of(d), VP_CONFIG_GENERATION);
}

/** A field of 1, 2 or 4 bytes is one access of its width, as virtio 1.2,
 * 4.1.3.1 asks; any other length goes a byte at a time. Past the window a
 * read gives zeros and a write is dropped. */
static void vp_config_read(struct virtio_dev *d, uint32_t offset, void *buf, uint32_t len)
{
    struct virtio_pci *vp = vp_of(d);
    uintptr_t a = vp->device.base + offset;
    uint8_t *out = (uint8_t *)buf;
    uint32_t i;

    if (!vp->device.base || offset > vp->device.len || len > vp->device.len - offset) {
        k_memset(buf, 0, len);
        return;
    }
    if (len == 1) {
        *out = mmio_read8(a);
    } else if (len == 2 && !(offset & 1u)) {
        uint16_t v = mmio_read16(a);

        k_memcpy(out, &v, 2);
    } else if (len == 4 && !(offset & 3u)) {
        uint32_t v = mmio_read32(a);

        k_memcpy(out, &v, 4);
    } else {
        for (i = 0; i < len; i++) {
            out[i] = mmio_read8(a + i);
        }
    }
}

/** Write device configuration as `vp_config_read` reads it. */
static void vp_config_write(struct virtio_dev *d, uint32_t offset, const void *buf, uint32_t len)
{
    struct virtio_pci *vp = vp_of(d);
    uintptr_t a = vp->device.base + offset;
    const uint8_t *in = (const uint8_t *)buf;
    uint32_t i;

    if (!vp->device.base || offset > vp->device.len || len > vp->device.len - offset) {
        return;
    }
    if (len == 2 && !(offset & 1u)) {
        uint16_t v;

        k_memcpy(&v, in, 2);
        mmio_write16(a, v);
    } else if (len == 4 && !(offset & 3u)) {
        uint32_t v;

        k_memcpy(&v, in, 4);
        mmio_write32(a, v);
    } else {
        for (i = 0; i < len; i++) {
            mmio_write8(a + i, in[i]);
        }
    }
}

/** The size queue `index` offers, 0 past the device's queue count. */
static uint16_t vp_queue_max(struct virtio_dev *d, uint16_t index)
{
    struct virtio_pci *vp = vp_of(d);

    if (index >= vp_r16(vp, VP_NUM_QUEUES)) {
        return 0;
    }
    vp_w16(vp, VP_QUEUE_SELECT, index);
    return vp_r16(vp, VP_QUEUE_SIZE);
}

/** The MSI-X entry queue `index` uses under `vp`'s plan. */
static uint16_t vp_queue_entry(const struct virtio_pci *vp, uint16_t index)
{
    if (vp->mode == VP_IRQ_MSIX) {
        return (uint16_t)(index + 1u);
    }
    if (vp->mode == VP_IRQ_SHARED) {
        return 1;
    }
    return VIRTIO_NO_VECTOR;
}

/**
 * Select queue `vq->index` and give it the size, the MSI-X entry and the
 * three addresses, then enable it (virtio 1.2, 4.1.5.1.3). Returns 0, -EIO
 * when the device refuses the entry or names a notification address past
 * its window, or -ENODEV once the function is unbound.
 */
static int vp_queue_enable(struct virtio_dev *d, struct virtqueue *vq)
{
    struct virtio_pci *vp = vp_of(d);
    uint16_t entry = vp_queue_entry(vp, vq->index);
    uint32_t off;

    if (vp->gone) {
        return -ENODEV;
    }
    vp_w16(vp, VP_QUEUE_SELECT, vq->index);
    vp_w16(vp, VP_QUEUE_SIZE, vq->size);
    vp_w16(vp, VP_QUEUE_MSIX_VECTOR, entry);
    if (vp_r16(vp, VP_QUEUE_MSIX_VECTOR) != entry) {
        klog(KLOG_ERR, "virtio", "queue: %s: %d: the device refused msi-x entry %d", vp->dev.name,
             vq->index, entry);
        return -EIO;
    }
    off = (uint32_t)vp_r16(vp, VP_QUEUE_NOTIFY_OFF) * vp->notify_mult;
    if (off > vp->notify.len || vp->notify.len - off < 2u) {
        klog(KLOG_ERR, "virtio", "queue: %s: %d: its notification lies past the window",
             vp->dev.name, vq->index);
        return -EIO;
    }
    vp_w64(vp, VP_QUEUE_DESC, vq->desc_addr);
    vp_w64(vp, VP_QUEUE_DRIVER, vq->avail_addr);
    vp_w64(vp, VP_QUEUE_DEVICE, vq->used_addr);
    vq->notify = vp->notify.base + off;
    vq->vector = entry;
    vp_w16(vp, VP_QUEUE_ENABLE, 1);
    return 0;
}

/** Without VIRTIO_F_NOTIFICATION_DATA the queue's number is what is
 * written (virtio 1.2, 4.1.5.2); nothing once the function is unbound. */
static void vp_notify(struct virtio_dev *d, struct virtqueue *vq)
{
    if (!vp_of(d)->gone) {
        mmio_write16(vq->notify, vq->index);
    }
}

/** Take the interrupt for slot `k`'s target, counted in the slot while it
 * runs, as Linux's synchronize_irq waits for a handler in flight
 * (kernel/irq/manage.c:134-151). */
static void vp_fire(int k)
{
    struct virtio_pci *vp;
    int target;
    uint32_t flags = spin_lock_irqsave(&vp_slots_lock);

    vp = vp_slots[k].vp;
    target = vp_slots[k].target;
    if (!vp) {
        spin_unlock_irqrestore(&vp_slots_lock, flags);
        return;
    }
    vp_slots[k].inflight++;
    spin_unlock_irqrestore(&vp_slots_lock, flags);
    if (target == VP_TARGET_CONFIG) {
        virtio_interrupt_config(&vp->dev);
    } else if (target == VP_TARGET_QUEUES) {
        virtio_interrupt_queues(&vp->dev);
    } else if (target >= 0 && target < VIRTIO_MAX_QUEUES) {
        virtio_interrupt_queue(vp->dev.queues[target]);
    }
    flags = spin_lock_irqsave(&vp_slots_lock);
    vp_slots[k].inflight--;
    spin_unlock_irqrestore(&vp_slots_lock, flags);
}

/* A handler is given only the saved registers, so each slot has a handler
 * of its own that names it. */
#define VP_SLOT_HANDLER(k)                  \
    static void vp_slot##k(isr_params_t *p) \
    {                                       \
        (void)p;                            \
        vp_fire(k);                         \
    }
VP_SLOT_HANDLER(0)
VP_SLOT_HANDLER(1)
VP_SLOT_HANDLER(2)
VP_SLOT_HANDLER(3)
VP_SLOT_HANDLER(4)
VP_SLOT_HANDLER(5)
VP_SLOT_HANDLER(6)
VP_SLOT_HANDLER(7)
VP_SLOT_HANDLER(8)
VP_SLOT_HANDLER(9)
VP_SLOT_HANDLER(10)
VP_SLOT_HANDLER(11)
VP_SLOT_HANDLER(12)
VP_SLOT_HANDLER(13)
VP_SLOT_HANDLER(14)
VP_SLOT_HANDLER(15)
VP_SLOT_HANDLER(16)
VP_SLOT_HANDLER(17)
VP_SLOT_HANDLER(18)
VP_SLOT_HANDLER(19)
VP_SLOT_HANDLER(20)
VP_SLOT_HANDLER(21)
VP_SLOT_HANDLER(22)
VP_SLOT_HANDLER(23)
VP_SLOT_HANDLER(24)
VP_SLOT_HANDLER(25)
VP_SLOT_HANDLER(26)
VP_SLOT_HANDLER(27)
VP_SLOT_HANDLER(28)
VP_SLOT_HANDLER(29)
VP_SLOT_HANDLER(30)
VP_SLOT_HANDLER(31)

static const isr_handler_t vp_slot_handler[VP_SLOTS] = {
    vp_slot0,  vp_slot1,  vp_slot2,  vp_slot3,  vp_slot4,  vp_slot5,  vp_slot6,  vp_slot7,
    vp_slot8,  vp_slot9,  vp_slot10, vp_slot11, vp_slot12, vp_slot13, vp_slot14, vp_slot15,
    vp_slot16, vp_slot17, vp_slot18, vp_slot19, vp_slot20, vp_slot21, vp_slot22, vp_slot23,
    vp_slot24, vp_slot25, vp_slot26, vp_slot27, vp_slot28, vp_slot29, vp_slot30, vp_slot31};

/** The INTx line, which several devices may share: each device on it reads
 * its ISR status, which says whether it raised the line and why. */
static void vp_intx(isr_params_t *p)
{
    struct virtio_pci *vp;

    (void)p;
    for (vp = vp_list; vp; vp = vp->next) {
        uint8_t isr;

        if (vp->mode != VP_IRQ_INTX) {
            continue;
        }
        isr = vp_isr_status(vp);
        if (isr & VP_ISR_QUEUE) {
            virtio_interrupt_queues(&vp->dev);
        }
        if (isr & VP_ISR_CONFIG) {
            virtio_interrupt_config(&vp->dev);
        }
    }
}

/** Free slots: how many. */
static int vp_slots_free(void)
{
    int k, n = 0;

    for (k = 0; k < VP_SLOTS; k++) {
        n += vp_slots[k].vp == 0;
    }
    return n;
}

/** Set or clear the Interrupt Disable bit of `vp`'s command word. */
static void vp_intx_disable(struct virtio_pci *vp, int off)
{
    uint16_t cmd = pci_read_config16(vp->pci.bus, vp->pci.device, vp->pci.function, PCI_COMMAND);

    cmd = (uint16_t)(off ? cmd | PCI_CMD_INTERRUPT_DIS : cmd & ~PCI_CMD_INTERRUPT_DIS);
    pci_write_config16(vp->pci.bus, vp->pci.device, vp->pci.function, PCI_COMMAND, cmd);
}

/** Mask MSI-X and release its remapping entries before `vp`'s vectors and
 * slots, waiting for each slot's handlers still running on other
 * processors, which may run without the big lock. Also undoes a partial
 * setup. */
static void vp_msix_free(struct virtio_pci *vp)
{
    int e;
    uint32_t flags, n;

    pci_msix_disable(&vp->pci);
    for (e = 0; e < VP_SLOTS; e++) {
        if (vp->vector[e] >= 0) {
            isr_free_vector(vp->vector[e]);
        }
        if (vp->slot[e] >= 0) {
            flags = spin_lock_irqsave(&vp_slots_lock);
            vp_slots[vp->slot[e]].vp = 0;
            spin_unlock_irqrestore(&vp_slots_lock, flags);
            do {
                flags = spin_lock_irqsave(&vp_slots_lock);
                n = vp_slots[vp->slot[e]].inflight;
                spin_unlock_irqrestore(&vp_slots_lock, flags);
            } while (n != 0);
        }
        vp->vector[e] = -1;
        vp->slot[e] = -1;
    }
    vp->nentries = 0;
}

/**
 * Map `vp`'s MSI-X table, once. Returns its entries, or 0 when the
 * function has no MSI-X or its table cannot be mapped.
 */
static uint32_t vp_msix_map(struct virtio_pci *vp)
{
    uint8_t bir;
    uint32_t offset, entries;
    uint64_t phys;

    if (vp->msix_table) {
        return vp->msix_entries;
    }
    if (pci_msix_table(&vp->pci, &bir, &offset, &entries) != 0 || entries == 0 || bir > 5) {
        return 0;
    }
    phys = pci_bar_address64(&vp->pci, (int)bir);
    if (phys == 0 || (uint64_t)(uintptr_t)(phys + offset) != phys + offset) {
        return 0;
    }
    vp->msix_table = map_mmio((uintptr_t)(phys + offset), entries * 16u);
    if (!vp->msix_table) {
        return 0;
    }
    vp->msix_entries = entries;
    return entries;
}

/**
 * Give `vp` `n` MSI-X entries, entry 0 the configuration's and the rest
 * the queues', each with a vector and a slot, and program every entry
 * through PCI's remapper before irq_start lifts the function mask.
 * MSI-X is held, enabled and masked, meanwhile. Returns 0, or -1 with
 * everything given back.
 */
static int vp_msix_take(struct virtio_pci *vp, int n)
{
    int e, k = 0, vector;
    uint32_t flags;

    if (pci_msix_hold(&vp->pci) != 0) {
        return -1;
    }
    for (e = 0; e < n; e++) {
        vector = isr_alloc_vector();
        if (vector < 0) {
            vp_msix_free(vp);
            return -1;
        }
        while (vp_slots[k].vp) {
            k++;
        }
        flags = spin_lock_irqsave(&vp_slots_lock);
        vp_slots[k].target = e == 0                    ? VP_TARGET_CONFIG
                             : vp->mode == VP_IRQ_MSIX ? e - 1
                                                       : VP_TARGET_QUEUES;
        vp_slots[k].vp = vp;
        spin_unlock_irqrestore(&vp_slots_lock, flags);
        vp->vector[e] = vector;
        vp->slot[e] = k;
        vp->nentries = e + 1;
        register_isr_handler((uint8_t)vector, vp_slot_handler[k], vp->dev.name);
        if (vp->dev.irq_unlocked) {
            isr_register_flags((uint8_t)vector, vp_slot_handler[k], ISR_UNLOCKED);
        }
        if (pci_msix_entry_set(&vp->pci, vp->msix_table, (unsigned int)e, (uint8_t)vector) != 0) {
            vp_msix_free(vp);
            return -1;
        }
    }
    if (vp_config_vector(vp, 0) != 0) {
        vp_msix_free(vp);
        return -1;
    }
    return 0;
}

/**
 * Choose how `vp` interrupts, as Linux's virtio_pci does: an MSI-X entry
 * for each queue and one for the configuration where the table and the
 * free vectors allow; else one entry for the configuration and one the
 * queues share; else the INTx line; else no interrupts, and the driver
 * polls. -ENODEV once the function is unbound.
 */
static int vp_irq_setup(struct virtio_dev *d, uint16_t nqueues)
{
    struct virtio_pci *vp = vp_of(d);
    uint32_t entries;
    int want = nqueues + 1;

    if (vp->gone) {
        return -ENODEV;
    }
    entries = isr_msi_ready() ? vp_msix_map(vp) : 0;

    vp_msix_free(vp);
    vp->mode = VP_IRQ_NONE;
    d->polled = 0;
    if (entries >= (uint32_t)want && vp_slots_free() >= want) {
        vp->mode = VP_IRQ_MSIX;
        if (vp_msix_take(vp, want) == 0) {
            vp_intx_disable(vp, 1);
            klog(KLOG_INFO, "virtio",
                 "interrupts: %s: msi-x, a vector for each of %d queues "
                 "and the configuration",
                 vp->dev.name, nqueues);
            return 0;
        }
    }
    if (entries >= 2u && vp_slots_free() >= 2) {
        vp->mode = VP_IRQ_SHARED;
        if (vp_msix_take(vp, 2) == 0) {
            vp_intx_disable(vp, 1);
            klog(KLOG_INFO, "virtio",
                 "interrupts: %s: msi-x, one vector for the queues and one "
                 "for the configuration",
                 vp->dev.name);
            return 0;
        }
    }
    vp_msix_free(vp);
    (void)vp_config_vector(vp, VIRTIO_NO_VECTOR);
    if (vp->pci.irq == 0 || vp->pci.irq >= 16) {
        vp->mode = VP_IRQ_POLLED;
        d->polled = 1;
        vp_intx_disable(vp, 1);
        klog(KLOG_INFO, "virtio", "interrupts: %s: polled, no msi-x and no interrupt line",
             vp->dev.name);
        return 0;
    }
    vp->mode = VP_IRQ_INTX;
    vp->line = vp->pci.irq;
    register_isr_handler((uint8_t)IRQ_VECTOR(vp->line), vp_intx, "virtio");
    vp_intx_disable(vp, 0);
    klog(KLOG_INFO, "virtio", "interrupts: %s: intx line %d", vp->dev.name, vp->line);
    return 0;
}

/** Lift the function mask after every entry has its complete message. */
static void vp_irq_start(struct virtio_dev *d)
{
    struct virtio_pci *vp = vp_of(d);

    if ((vp->mode == VP_IRQ_MSIX || vp->mode == VP_IRQ_SHARED) && vp->vector[0] >= 0) {
        pci_msix_unmask(&vp->pci);
    }
}

/** Turn MSI-X off and give back its vectors and slots, or leave the INTx
 * line, whose handler then passes the device over; nothing once the
 * function is unbound, which released them. */
static void vp_irq_release(struct virtio_dev *d)
{
    struct virtio_pci *vp = vp_of(d);

    if (vp->gone) {
        return;
    }
    if (vp->mode == VP_IRQ_MSIX || vp->mode == VP_IRQ_SHARED) {
        vp_msix_free(vp);
    }
    vp_intx_disable(vp, 1);
    vp->mode = VP_IRQ_NONE;
}

/** A page the device reads by its physical address, from any memory the
 * kernel reaches (`alloc_dma64_frame`): every address of virtio 1.x is 64
 * bits wide, rings and buffers alike, as Linux's virtio-pci sets a DMA mask
 * of 64 bits, so the device's pages leave the 64 MiB pool below
 * HARD_MEMORY_LIMIT to the devices whose registers take 32 bits. */
static void *vp_dma_alloc(struct virtio_dev *d, uint32_t bytes, uint64_t *addr)
{
    void *frame;

    (void)d;
    if (bytes == 0 || bytes > PAGE_SIZE) {
        return 0;
    }
    frame = alloc_dma64_frame();
    if (!frame) {
        return 0;
    }
    k_memset(frame, 0, PAGE_SIZE);
    *addr = (uint64_t)paging_virt_to_phys((uintptr_t)frame);
    return frame;
}

/** Give back a page `vp_dma_alloc` gave, or the run `vp_ring_alloc` gave
 * for a ring of more than a page. */
static void vp_dma_free(struct virtio_dev *d, void *p, uint32_t bytes)
{
    (void)d;
    if (bytes > PAGE_SIZE) {
        kfree_pages(p, (bytes + PAGE_SIZE - 1u) / PAGE_SIZE);
    } else {
        free_dma64_frame(p);
    }
}

/**
 * The memory of a queue's rings: a page from `vp_dma_alloc`, or, for the
 * larger queue a legacy device states, a run of consecutive frames
 * (`kalloc_contig`), zeroed, since the device reads the rings as one piece
 * by the address of their first frame; `ring_alloc` of virtio.h.
 */
static void *vp_ring_alloc(struct virtio_dev *d, uint16_t index, uint32_t bytes, uint64_t *addr)
{
    uint32_t pages = (bytes + PAGE_SIZE - 1u) / PAGE_SIZE;
    uintptr_t phys;
    void *run;

    (void)index;
    if (bytes <= PAGE_SIZE) {
        return vp_dma_alloc(d, bytes, addr);
    }
    run = kalloc_contig(pages, &phys);
    if (!run) {
        return 0;
    }
    k_memset(run, 0, pages * PAGE_SIZE);
    *addr = (uint64_t)phys;
    return run;
}

/** Shared memory region `id` from the capabilities read at attach; 0, or
 * -ENOENT. */
static int vp_shm_region(struct virtio_dev *d, uint8_t id, uint64_t *addr, uint64_t *len)
{
    struct virtio_pci *vp = vp_of(d);
    int i;

    for (i = 0; i < vp->nshm; i++) {
        if (vp->shm[i].id == id) {
            *addr = vp->shm[i].addr;
            *len = vp->shm[i].len;
            return 0;
        }
    }
    return -ENOENT;
}

/** The driver has let go of `d`: an unbound record is freed now, a bound
 * one stays on the list for the next driver; `put` of virtio.h. */
static void vp_put(struct virtio_dev *d)
{
    struct virtio_pci *vp = vp_of(d);

    vp->held = 0;
    if (vp->gone) {
        kfree(vp);
    }
}

/** Turn the function's bus mastering on or off; `dma_enable` of virtio.h.
 * Off, the function can start no memory access, its MSI-X messages among
 * them, as Linux's pci_clear_master leaves it. */
static void vp_dma_enable(struct virtio_dev *d, int on)
{
    struct virtio_pci *vp = vp_of(d);

    if (on) {
        pci_enable_bus_master(&vp->pci);
    } else {
        pci_disable_bus_master(&vp->pci);
    }
}

static const struct virtio_ops vp_ops = {"virtio-pci",
                                         vp_get_status,
                                         vp_set_status,
                                         vp_get_features,
                                         vp_set_features,
                                         vp_config_generation,
                                         vp_config_read,
                                         vp_config_write,
                                         vp_queue_max,
                                         vp_queue_enable,
                                         vp_notify,
                                         vp_irq_setup,
                                         vp_irq_start,
                                         vp_irq_release,
                                         vp_dma_alloc,
                                         vp_dma_free,
                                         vp_shm_region,
                                         vp_ring_alloc,
                                         NULL,
                                         NULL,
                                         NULL,
                                         vp_put,
                                         NULL,
                                         NULL,
                                         NULL,
                                         vp_dma_enable};

/*
 * The legacy interface's operations, as Linux's virtio_pci_legacy.c drives
 * a function without the capabilities: the rest are the modern table's.
 */

/** The device status byte of a legacy function. */
static uint8_t vpl_get_status(struct virtio_dev *d)
{
    return vpl_r8(vp_of(d), VPL_STATUS);
}

/** Write the device status byte of a legacy function. */
static void vpl_set_status(struct virtio_dev *d, uint8_t status)
{
    vpl_w8(vp_of(d), VPL_STATUS, status);
}

/** A legacy device's 32 feature bits. */
static uint64_t vpl_get_features(struct virtio_dev *d)
{
    return vpl_r32(vp_of(d), VPL_HOST_FEATURES);
}

/** Write the driver's feature bits, of which a legacy device has 32. */
static void vpl_set_features(struct virtio_dev *d, uint64_t features)
{
    vpl_w32(vp_of(d), VPL_GUEST_FEATURES, (uint32_t)features);
}

/** A legacy device has no configuration generation: 0, always. */
static uint32_t vpl_config_generation(struct virtio_dev *d)
{
    (void)d;
    return 0;
}

/**
 * The port of byte `offset` of a legacy device's configuration, which
 * starts after the MSI-X registers while MSI-X is on, into `*port`: 0, or
 * -1 when `len` bytes from there pass BAR0 or the function is unbound.
 */
static int vpl_config_port(struct virtio_pci *vp, uint32_t offset, uint32_t len, uint16_t *port)
{
    uint32_t base = vp->nentries > 0 ? VPL_CONFIG_MSIX : VPL_CONFIG;

    if (vp->gone || base > vp->io_len || offset > vp->io_len - base ||
        len > vp->io_len - base - offset) {
        return -1;
    }
    *port = (uint16_t)(vp->io + base + offset);
    return 0;
}

/** Read a legacy device's configuration as `vp_config_read` reads it. */
static void vpl_config_read(struct virtio_dev *d, uint32_t offset, void *buf, uint32_t len)
{
    uint8_t *out = (uint8_t *)buf;
    uint16_t port;
    uint32_t i;

    if (vpl_config_port(vp_of(d), offset, len, &port) != 0) {
        k_memset(buf, 0, len);
        return;
    }
    if (len == 2 && !(offset & 1u)) {
        uint16_t v = pci_io_read16(port);

        k_memcpy(out, &v, 2);
    } else if (len == 4 && !(offset & 3u)) {
        uint32_t v = pci_io_read32(port);

        k_memcpy(out, &v, 4);
    } else {
        for (i = 0; i < len; i++) {
            out[i] = pci_io_read8((uint16_t)(port + i));
        }
    }
}

/** Write a legacy device's configuration as `vp_config_write` writes it. */
static void vpl_config_write(struct virtio_dev *d, uint32_t offset, const void *buf, uint32_t len)
{
    const uint8_t *in = (const uint8_t *)buf;
    uint16_t port;
    uint32_t i;

    if (vpl_config_port(vp_of(d), offset, len, &port) != 0) {
        return;
    }
    if (len == 2 && !(offset & 1u)) {
        uint16_t v;

        k_memcpy(&v, in, 2);
        pci_io_write16(port, v);
    } else if (len == 4 && !(offset & 3u)) {
        uint32_t v;

        k_memcpy(&v, in, 4);
        pci_io_write32(port, v);
    } else {
        for (i = 0; i < len; i++) {
            pci_io_write8((uint16_t)(port + i), in[i]);
        }
    }
}

/** The size legacy queue `index` has, which the device fixes; 0 for a
 * queue it lacks. */
static uint16_t vpl_queue_max(struct virtio_dev *d, uint16_t index)
{
    struct virtio_pci *vp = vp_of(d);

    vpl_w16(vp, VPL_QUEUE_SEL, index);
    return vpl_r16(vp, VPL_QUEUE_NUM);
}

/**
 * Select legacy queue `vq->index`, give it its MSI-X entry while MSI-X is
 * on, and activate it by its ring's page number (virtio 1.2, 4.1.5.1.3.1),
 * which takes 32 bits: the rings must begin on a page below 2^44. 0; -EIO
 * when the device refuses the entry or the rings lie where it cannot take
 * them; -ENODEV once the function is unbound.
 */
static int vpl_queue_enable(struct virtio_dev *d, struct virtqueue *vq)
{
    struct virtio_pci *vp = vp_of(d);
    uint16_t entry = vp_queue_entry(vp, vq->index);
    uint64_t pfn = vq->desc_addr >> VPL_PFN_SHIFT;

    if (vp->gone) {
        return -ENODEV;
    }
    if ((vq->desc_addr & (VIRTQ_LEGACY_ALIGN - 1u)) != 0 || pfn > 0xFFFFFFFFu) {
        klog(KLOG_ERR, "virtio",
             "queue: %s: %d: rings at 0x%x%08x, which a legacy device cannot take", vp->dev.name,
             vq->index, (unsigned)(vq->desc_addr >> 32), (unsigned)vq->desc_addr);
        return -EIO;
    }
    vpl_w16(vp, VPL_QUEUE_SEL, vq->index);
    if (vp->nentries > 0) {
        vpl_w16(vp, VPL_QUEUE_VECTOR, entry);
        if (vpl_r16(vp, VPL_QUEUE_VECTOR) != entry) {
            klog(KLOG_ERR, "virtio", "queue: %s: %d: the device refused msi-x entry %d",
                 vp->dev.name, vq->index, entry);
            return -EIO;
        }
    }
    vq->notify = (uintptr_t)(vp->io + VPL_QUEUE_NOTIFY);
    vq->vector = entry;
    vpl_w32(vp, VPL_QUEUE_PFN, (uint32_t)pfn);
    return 0;
}

/** Write the queue's number to the legacy notification port; nothing once
 * the function is unbound. */
static void vpl_notify(struct virtio_dev *d, struct virtqueue *vq)
{
    if (!vp_of(d)->gone) {
        pci_io_write16((uint16_t)vq->notify, vq->index);
    }
}

static const struct virtio_ops vp_legacy_ops = {"virtio-pci",
                                                vpl_get_status,
                                                vpl_set_status,
                                                vpl_get_features,
                                                vpl_set_features,
                                                vpl_config_generation,
                                                vpl_config_read,
                                                vpl_config_write,
                                                vpl_queue_max,
                                                vpl_queue_enable,
                                                vpl_notify,
                                                vp_irq_setup,
                                                vp_irq_start,
                                                vp_irq_release,
                                                vp_dma_alloc,
                                                vp_dma_free,
                                                vp_shm_region,
                                                vp_ring_alloc,
                                                NULL,
                                                NULL,
                                                NULL,
                                                vp_put,
                                                NULL,
                                                NULL,
                                                NULL,
                                                vp_dma_enable};

/** The device type of virtio-pci function `p`, or 0 when it is none. */
static uint16_t vp_type(const pci_device_t *p)
{
    if (p->vendor_id != VP_VENDOR) {
        return 0;
    }
    if (p->device_id >= VP_MODERN_BASE && p->device_id <= VP_MODERN_LAST) {
        return (uint16_t)(p->device_id - VP_MODERN_BASE);
    }
    if (p->device_id >= VP_TRANSITION_FIRST && p->device_id <= VP_TRANSITION_LAST) {
        return pci_read_config16(p->bus, p->device, p->function, PCI_SUBSYSTEM_ID);
    }
    return 0;
}

/** A short name for device type `id`, for the log. */
static const char *vp_type_name(uint16_t id)
{
    switch (id) {
    case VIRTIO_ID_NET:
        return "net";
    case VIRTIO_ID_BLOCK:
        return "block";
    case VIRTIO_ID_CONSOLE:
        return "console";
    case VIRTIO_ID_RNG:
        return "rng";
    case VIRTIO_ID_BALLOON:
        return "balloon";
    case VIRTIO_ID_SCSI:
        return "scsi";
    case VIRTIO_ID_9P:
        return "9p";
    case VIRTIO_ID_GPU:
        return "gpu";
    case VIRTIO_ID_INPUT:
        return "input";
    case VIRTIO_ID_VSOCK:
        return "vsock";
    case VIRTIO_ID_SOUND:
        return "sound";
    default:
        return "device";
    }
}

/**
 * Take the capability at `cap` as window `w` when `w` has none yet and its
 * BAR is a programmed memory window the kernel can address; a capability
 * in an I/O BAR is passed over for a later one of the same type.
 */
static void vp_take_window(struct virtio_pci *vp, uint8_t cap, struct vp_window *w)
{
    const pci_device_t *p = &vp->pci;
    uint8_t bar = pci_read_config8(p->bus, p->device, p->function, (uint8_t)(cap + VP_CAP_BAR));
    uint32_t off =
        pci_read_config32(p->bus, p->device, p->function, (uint8_t)(cap + VP_CAP_OFFSET));
    uint32_t len =
        pci_read_config32(p->bus, p->device, p->function, (uint8_t)(cap + VP_CAP_LENGTH));
    uint64_t phys;

    if (w->len != 0 || bar > 5 || len == 0 || !pci_bar_is_memory(p->bar[bar])) {
        return;
    }
    phys = pci_bar_address64(p, bar);
    if (phys == 0 || (uint64_t)(uintptr_t)(phys + off + len) != phys + off + len) {
        return;
    }
    w->bar = bar;
    w->offset = off;
    w->len = len < VP_MAP_MAX ? len : VP_MAP_MAX;
    w->base = (uintptr_t)(phys + off); /* physical until vp_map_windows */
}

/**
 * Record the shared memory region the capability at `cap` names, as Linux's
 * vp_get_shm_region takes it: the first capability of each id whose BAR is
 * a programmed memory BAR, and whose offset and length lie within the BAR
 * without wrapping. Another is passed over with a record by the function's
 * address, as Linux's dev_err names the PCI device. The region is not
 * mapped; a driver maps the pieces it needs.
 */
static void vp_take_shm(struct virtio_pci *vp, uint8_t cap)
{
    const pci_device_t *p = &vp->pci;
    uint8_t bar = pci_read_config8(p->bus, p->device, p->function, (uint8_t)(cap + VP_CAP_BAR));
    uint8_t id = pci_read_config8(p->bus, p->device, p->function, (uint8_t)(cap + VP_CAP_ID));
    uint64_t off, len, phys, size;
    int i;

    for (i = 0; i < vp->nshm; i++) {
        if (vp->shm[i].id == id) {
            return;
        }
    }
    if (vp->nshm >= VP_SHM || bar > 5 || !pci_bar_is_memory(p->bar[bar])) {
        return;
    }
    off = pci_read_config32(p->bus, p->device, p->function, (uint8_t)(cap + VP_CAP_OFFSET)) |
          ((uint64_t)pci_read_config32(p->bus, p->device, p->function,
                                       (uint8_t)(cap + VP_CAP_OFFSET_HI))
           << 32);
    len = pci_read_config32(p->bus, p->device, p->function, (uint8_t)(cap + VP_CAP_LENGTH)) |
          ((uint64_t)pci_read_config32(p->bus, p->device, p->function,
                                       (uint8_t)(cap + VP_CAP_LENGTH_HI))
           << 32);
    phys = pci_bar_address64(p, bar);
    size = pci_bar_size64(p, bar);
    if (phys == 0 || len == 0 || off + len < off || off + len > size) {
        klog(KLOG_WARN, "virtio",
             "refused: %02x:%02x.%d: shared memory %d: offset 0x%x%08x, length 0x%x%08x, bar %d "
             "of 0x%x%08x",
             p->bus, p->device, p->function, id, (unsigned)(off >> 32), (unsigned)off,
             (unsigned)(len >> 32), (unsigned)len, bar, (unsigned)(size >> 32), (unsigned)size);
        return;
    }
    vp->shm[vp->nshm].id = id;
    vp->shm[vp->nshm].addr = phys + off;
    vp->shm[vp->nshm].len = len;
    vp->nshm++;
}

/**
 * Walk `vp`'s capability list for its vendor capabilities, at most 48
 * links, as `pci_find_capability` bounds its walk.
 */
static void vp_read_caps(struct virtio_pci *vp)
{
    const pci_device_t *p = &vp->pci;
    uint8_t cap, id, type, len;
    int links;

    if (!(pci_read_config16(p->bus, p->device, p->function, PCI_STATUS) & PCI_STATUS_CAP_LIST)) {
        return;
    }
    cap = (uint8_t)(pci_read_config8(p->bus, p->device, p->function, PCI_CAP_PTR) & 0xFCu);
    for (links = 0; links < 48 && cap >= 0x40 && cap <= 0xE8; links++) {
        id = pci_read_config8(p->bus, p->device, p->function, cap);
        if (id == PCI_CAP_ID_VNDR) {
            len = pci_read_config8(p->bus, p->device, p->function, (uint8_t)(cap + VP_CAP_LEN));
            type = pci_read_config8(p->bus, p->device, p->function, (uint8_t)(cap + VP_CAP_TYPE));
            if (type == VP_CFG_COMMON && len >= 16) {
                vp_take_window(vp, cap, &vp->common);
            } else if (type == VP_CFG_NOTIFY && len >= 20 && vp->notify.len == 0) {
                vp_take_window(vp, cap, &vp->notify);
                vp->notify_mult = pci_read_config32(p->bus, p->device, p->function,
                                                    (uint8_t)(cap + VP_CAP_MULTIPLIER));
            } else if (type == VP_CFG_ISR && len >= 16) {
                vp_take_window(vp, cap, &vp->isr);
            } else if (type == VP_CFG_DEVICE && len >= 16) {
                vp_take_window(vp, cap, &vp->device);
            } else if (type == VP_CFG_SHM && len >= 24) {
                vp_take_shm(vp, cap);
            }
        }
        cap =
            (uint8_t)(pci_read_config8(p->bus, p->device, p->function, (uint8_t)(cap + 1)) & 0xFCu);
    }
}

/** Map window `w`, whose base holds its physical address; 0, or -1. */
static int vp_map_window(struct vp_window *w)
{
    uintptr_t va;

    if (w->len == 0) {
        return 0;
    }
    va = map_mmio(w->base, w->len);
    if (!va) {
        return -1;
    }
    w->base = va;
    w->mapped = 1;
    return 0;
}

/** Give back the windows of `vp` that `vp_map_window` mapped and its MSI-X
 * table, put back the command word the function had before `vp_attach` less
 * bus mastering, so that a function the transport does not keep decodes as
 * it did and masters no more, even when an earlier owner left a device that
 * never reset mastering, and give back the kernel's PCI claim when the
 * probe took it, as Linux's pci_disable_device ends a probe or a binding,
 * so that another driver, vfio-pci among them, may have the function. */
static void vp_detach(struct virtio_pci *vp)
{
    struct vp_window *w[4];
    int i;

    w[0] = &vp->common;
    w[1] = &vp->notify;
    w[2] = &vp->isr;
    w[3] = &vp->device;
    for (i = 0; i < 4; i++) {
        if (w[i]->mapped) {
            mmio_unmap(w[i]->base, w[i]->len);
            w[i]->mapped = 0;
            w[i]->base = 0;
            w[i]->len = 0;
        }
    }
    if (vp->msix_table) {
        mmio_unmap(vp->msix_table, vp->msix_entries * 16u);
        vp->msix_table = 0;
        vp->msix_entries = 0;
    }
    pci_write_config16(vp->pci.bus, vp->pci.device, vp->pci.function, PCI_COMMAND, vp->command);
    if (vp->claimed) {
        pci_unclaim(vp->pci.bus, (uint8_t)(vp->pci.device << 3 | vp->pci.function),
                    PCI_OWNER_KERNEL);
        vp->claimed = 0;
    }
}

/**
 * Take the function `vp->pci`, which has no virtio 1.x capabilities, by its
 * legacy interface, as Linux's virtio_pci_legacy_probe does after the
 * modern probe finds none: a transitional device id of revision 0 whose
 * BAR0 is ports, enough of them for the registers. Turns on its I/O
 * decoding, and its memory decoding for its MSI-X table, which makes the
 * kernel its owner when it had none, as vp_attach does.
 * 0, or -ENODEV, with nothing changed, for a function without it.
 */
static int vp_attach_legacy(struct virtio_pci *vp)
{
    const pci_device_t *p = &vp->pci;
    struct pci_resource r;
    uint16_t cmd;

    if (p->device_id < VP_TRANSITION_FIRST || p->device_id > VP_TRANSITION_LAST ||
        pci_read_config8(p->bus, p->device, p->function, PCI_REVISION_ID) != 0) {
        return -ENODEV;
    }
    pci_resource(p->bus, p->device, p->function, 0, &r);
    if (!(r.flags & PCI_IORESOURCE_IO) || r.start == 0 || r.end > 0xFFFFu ||
        r.end - r.start + 1u < VPL_CONFIG_MSIX) {
        return -ENODEV;
    }
    vp->io = (uint16_t)r.start;
    vp->io_len = (uint16_t)(r.end - r.start + 1u);
    vp->dev.legacy = 1;
    vp->dev.ops = &vp_legacy_ops;
    vp->claimed = pci_owner(p->bus, (uint8_t)(p->device << 3 | p->function)) == NULL;
    pci_enable_memory_space(&vp->pci);
    cmd = pci_read_config16(p->bus, p->device, p->function, PCI_COMMAND);
    pci_write_config16(p->bus, p->device, p->function, PCI_COMMAND,
                       (uint16_t)(cmd | PCI_CMD_IO_SPACE));
    vp_intx_disable(vp, 1);
    return 0;
}

/** Read, check and map the capabilities of the function `vp->pci`, and turn
 * on its memory decoding, which makes the kernel the function's owner when
 * it had none (`claimed`); bus mastering, which the command word kept for
 * `vp_detach` leaves out, waits for a reset that finished
 * (`vp_acknowledge`). A function without the capabilities is taken by its
 * legacy interface when it has one (`vp_attach_legacy`). Returns 0, or -EIO
 * with the reason in the log and every window it mapped given back
 * (`vp_detach`). */
static int vp_attach(struct virtio_pci *vp)
{
    vp->command = pci_read_config16(vp->pci.bus, vp->pci.device, vp->pci.function, PCI_COMMAND);
    vp->command &= (uint16_t)~PCI_CMD_BUS_MASTER;
    vp_read_caps(vp);
    if (vp->common.len < VP_COMMON_MIN || vp->notify.len == 0 || vp->isr.len == 0) {
        if (vp_attach_legacy(vp) == 0) {
            return 0;
        }
        klog(KLOG_WARN, "virtio", "refused: %02x:%02x.%d: no %s capability in a memory BAR",
             vp->pci.bus, vp->pci.device, vp->pci.function,
             vp->common.len < VP_COMMON_MIN ? "common"
             : vp->notify.len == 0          ? "notify"
                                            : "isr");
        return -EIO;
    }
    if (vp_map_window(&vp->common) != 0 || vp_map_window(&vp->notify) != 0 ||
        vp_map_window(&vp->isr) != 0 || vp_map_window(&vp->device) != 0) {
        klog(KLOG_WARN, "virtio", "refused: %02x:%02x.%d: a window could not be mapped",
             vp->pci.bus, vp->pci.device, vp->pci.function);
        vp_detach(vp);
        return -EIO;
    }
    vp->claimed = pci_owner(vp->pci.bus, (uint8_t)(vp->pci.device << 3 | vp->pci.function)) == NULL;
    pci_enable_memory_space(&vp->pci);
    vp_intx_disable(vp, 1);
    return 0;
}

/** Log the windows and shared memory of `vp`, by the name the bus gave
 * it. */
static void vp_log_found(struct virtio_pci *vp)
{
    int i;

    if (vp->dev.legacy) {
        klog(KLOG_INFO, "virtio", "found: %s: %02x:%02x.%d %s, legacy, io 0x%x, %d ports",
             vp->dev.name, vp->pci.bus, vp->pci.device, vp->pci.function, vp_type_name(vp->dev.id),
             vp->io, vp->io_len);
        return;
    }
    klog(KLOG_INFO, "virtio",
         "found: %s: %02x:%02x.%d %s, common bar %d+0x%x, notify bar %d+0x%x x%d, isr bar "
         "%d+0x%x, device %s%d+0x%x, %d queues",
         vp->dev.name, vp->pci.bus, vp->pci.device, vp->pci.function, vp_type_name(vp->dev.id),
         vp->common.bar, vp->common.offset, vp->notify.bar, vp->notify.offset, vp->notify_mult,
         vp->isr.bar, vp->isr.offset, vp->device.len ? "bar " : "none ", vp->device.bar,
         vp->device.offset, vp_r16(vp, VP_NUM_QUEUES));
    for (i = 0; i < vp->nshm; i++) {
        klog(KLOG_INFO, "virtio", "found: %s: shared memory %d at %x:%x, %d MiB", vp->dev.name,
             vp->shm[i].id, (unsigned)(vp->shm[i].addr >> 32), (unsigned)vp->shm[i].addr,
             (int)(vp->shm[i].len >> 20));
    }
}

/** Whether `p` is a function Linux's virtio-pci takes: Red Hat's vendor id
 * and a device id from 0x1000 to 0x107F, as virtio_pci_probe checks. */
static int vp_is_virtio(const pci_device_t *p)
{
    return p->vendor_id == VP_VENDOR && p->device_id >= VP_TRANSITION_FIRST &&
           p->device_id <= VP_MODERN_LAST;
}

/** The record of the function `p`, or NULL when the transport has none. */
static struct virtio_pci *vp_record(const pci_device_t *p)
{
    struct virtio_pci *vp;

    for (vp = vp_list; vp; vp = vp->next) {
        if (vp->pci.bus == p->bus && vp->pci.device == p->device &&
            vp->pci.function == p->function) {
            return vp;
        }
    }
    return NULL;
}

/**
 * Reset the device of `vp` and set ACKNOWLEDGE, as Linux's
 * register_virtio_device does before it publishes a device, whether a
 * driver of its type exists or not, so that no DRIVER_OK or queue a
 * previous owner left stands. Bus mastering, which firmware, an earlier
 * owner or a binding whose reset did not finish may have left on, is
 * turned off before the reset and on, as Linux's virtio_pci_probe does,
 * only once the reset has finished, so that a device still working through
 * what an earlier binding gave it cannot reach memory freed since. 0, or
 * -ETIMEDOUT with a record and bus mastering left off when the reset did
 * not finish.
 */
static int vp_acknowledge(struct virtio_pci *vp)
{
    int k;

    pci_disable_bus_master(&vp->pci);
    k = virtio_reset(&vp->dev);
    if (k != 0) {
        klog(KLOG_WARN, "virtio", "refused: %02x:%02x.%d: the device did not reset: error %d",
             vp->pci.bus, vp->pci.device, vp->pci.function, k);
        return k;
    }
    pci_enable_bus_master(&vp->pci);
    vp->dev.ops->set_status(&vp->dev, VIRTIO_STATUS_ACKNOWLEDGE);
    return 0;
}

/**
 * Take the virtio function `p`, as Linux's virtio_pci_probe does: show it
 * bound to virtio-pci, read and map its capabilities, reset its device and
 * acknowledge it (`vp_acknowledge`), add its virtio device to the bus,
 * which names it, log it and send the function's "bind" uevent, and keep
 * its record at the end of `vp_list`, in `*out` when `out` is not NULL.
 * 0, or -ENOMEM, -EIO, -ETIMEDOUT, or the error of showing it in /sys,
 * with nothing of it left: no record, binding, window or PCI claim, and
 * its command word as it was, less the bus mastering of a function whose
 * capabilities were mapped.
 */
static int vp_probe_one(const pci_device_t *p, struct virtio_pci **out)
{
    struct virtio_pci *vp, **tail;
    int k, key;

    vp = (struct virtio_pci *)kzalloc(sizeof(*vp));
    if (!vp) {
        klog(KLOG_WARN, "virtio", "refused: %02x:%02x.%d: no memory for its record", p->bus,
             p->device, p->function);
        return -ENOMEM;
    }
    vp->pci = *p;
    vp->dev.ops = &vp_ops;
    vp->dev.notify_unlocked = 1;
    vp->dev.config_atomic = 1;
    vp->dev.transport = vp;
    vp->dev.id = vp_type(p);
    vp->dev.name = "virtio";
    for (k = 0; k < VP_SLOTS; k++) {
        vp->vector[k] = -1;
        vp->slot[k] = -1;
    }
    /* Linux's driver core shows the binding before virtio_pci_probe
     * runs, and a failure to show it fails the probe. */
    key = (int)p->bus << 8 | p->device << 3 | p->function;
    k = sysfs_pci_bind(key, "virtio-pci");
    if (k != 0) {
        klog(KLOG_WARN, "virtio", "refused: %02x:%02x.%d: driver link not added: error %d", p->bus,
             p->device, p->function, k);
        kfree(vp);
        return k;
    }
    k = vp_attach(vp);
    if (k == 0) {
        k = vp_acknowledge(vp);
        if (k == 0) {
            k = virtio_bus_add(
                &vp->dev, sysfs_anchor(SYSFS_PCI_DEVICE, key),
                pci_read_config16(p->bus, p->device, p->function, PCI_SUBSYSTEM_VENDOR));
            if (k != 0) {
                klog(KLOG_WARN, "virtio",
                     "refused: %02x:%02x.%d: not added to the virtio bus: error %d", p->bus,
                     p->device, p->function, k);
            }
        }
        if (k != 0) {
            vp_detach(vp);
        }
    }
    if (k != 0) {
        sysfs_pci_unbind(key);
        kfree(vp);
        return k;
    }
    vp_log_found(vp);
    for (tail = &vp_list; *tail; tail = &(*tail)->next) {
    }
    *tail = vp;
    sysfs_pci_uevent(key, "bind");
    if (out) {
        *out = vp;
    }
    return 0;
}

/** The key of the function `p` in /sys, bus << 8 | device << 3 | function. */
static int vp_key(const pci_device_t *p)
{
    return (int)p->bus << 8 | p->device << 3 | p->function;
}

/**
 * Let go of the function of `vp`, as Linux's virtio_pci_remove does when
 * the function is unbound from virtio-pci: its child driver removed first
 * (`removed` of virtio.h), as unregister_virtio_device removes it, the
 * device reset, its interrupts released, its virtio device taken off the
 * bus, which gives its number back, the record taken off `vp_list`, its
 * windows and PCI claim given back with its command word less bus
 * mastering, as pci_disable_device leaves it (`vp_detach`), so that putting
 * the word back cannot turn on the mastering that the child's teardown
 * turned off after a reset that did not finish (`dma_enable`), and the
 * function shown unbound with its "unbind" uevent. The record is freed
 * now, or at the driver's virtio_put while one still holds it. May sleep,
 * in the child's removal.
 */
static void vp_unbind(struct virtio_pci *vp)
{
    struct virtio_pci **link;
    pci_device_t pdev = vp->pci;
    int key = vp_key(&vp->pci);

    if (vp->dev.removed) {
        vp->dev.removed(&vp->dev);
    }
    (void)virtio_reset(&vp->dev);
    if (vp->mode != VP_IRQ_NONE) {
        vp_irq_release(&vp->dev);
    }
    virtio_bus_remove(&vp->dev);
    for (link = &vp_list; *link; link = &(*link)->next) {
        if (*link == vp) {
            *link = vp->next;
            break;
        }
    }
    vp_detach(vp);
    vp->gone = 1;
    sysfs_pci_unbind(key);
    (void)sysfs_pci_uevent(key, "unbind");
    klog(KLOG_INFO, "virtio", "unbound: %s: %02x:%02x.%d", vp->dev.name, pdev.bus, pdev.device,
         pdev.function);
    if (!vp->held) {
        kfree(vp);
    }
}

/** Whether the `n` bytes at `text` are the whole of the string `word`. */
static int vp_text_is(const char *text, int n, const char *word)
{
    int i;

    for (i = 0; i < n; i++) {
        if (word[i] == '\0' || text[i] != word[i]) {
            return 0;
        }
    }
    return word[n] == '\0';
}

/** The value of the hexadecimal digit `c`, or -1. */
static int vp_hex(char c)
{
    if (c >= '0' && c <= '9') {
        return c - '0';
    }
    c = (char)(c | 0x20);
    return c >= 'a' && c <= 'f' ? c - 'a' + 10 : -1;
}

/**
 * The function a write of `bind` or `unbind` names, "0000:BB:DD.F" with
 * perhaps a newline or spaces after it, into `*out` from the buses: 0, or
 * -ENODEV for text that names no function there.
 */
static int vp_name_function(const char *buf, uint32_t len, pci_device_t *out)
{
    int b1, b0, d1, d0, f, i;

    while (len > 0 && (buf[len - 1] == '\n' || buf[len - 1] == ' ')) {
        len--;
    }
    if (len != 12 || !vp_text_is(buf, 5, "0000:") || buf[7] != ':' || buf[10] != '.') {
        return -ENODEV;
    }
    b1 = vp_hex(buf[5]);
    b0 = vp_hex(buf[6]);
    d1 = vp_hex(buf[8]);
    d0 = vp_hex(buf[9]);
    f = vp_hex(buf[11]);
    if (b1 < 0 || b0 < 0 || d1 < 0 || d0 < 0 || f < 0 || d1 > 1 || f > 7) {
        return -ENODEV;
    }
    for (i = 0; pci_nth(i, out); i++) {
        if (out->bus == (uint8_t)(b1 << 4 | b0) && out->device == (uint8_t)(d1 << 4 | d0) &&
            out->function == (uint8_t)f) {
            return 0;
        }
    }
    return -ENODEV;
}

/* Whether a write of the driver's files is under way; the unbind may sleep
 * in the child's removal, and a second write waits for none. */
static int vp_busy;

/**
 * Take the function `p` as at boot (`vp_probe_one`), then give its device
 * to the driver of its type, as Linux's bus then probes the virtio
 * drivers: -ENODEV for a function that is no virtio function or whose
 * driver_override names another driver, -EBUSY for one the transport or
 * another driver has; else the probe's error. The child driver's failure
 * leaves the function bound to virtio-pci, as on Linux.
 */
static int vp_bind(const pci_device_t *p)
{
    struct virtio_pci *vp;
    virtio_probe_t probe;
    int k;

    if (!vp_is_virtio(p) || !vfio_override_allows(vp_key(p), "virtio-pci")) {
        return -ENODEV;
    }
    if (vp_record(p) || pci_owner(p->bus, (uint8_t)(p->device << 3 | p->function)) != NULL) {
        return -EBUSY;
    }
    k = vp_probe_one(p, &vp);
    if (k != 0) {
        return k;
    }
    probe = virtio_driver_for(vp->dev.id);
    if (probe) {
        vp->held = 1;
        k = (int)probe(&vp->dev);
        if (k != 0) {
            klog(KLOG_WARN, "virtio", "bind: %s: its driver refused it: error %d", vp->dev.name, k);
        }
    }
    return 0;
}

/** /sys/bus/pci/drivers/virtio-pci/bind: take the function named
 * (`vp_bind`). */
static int32_t vp_store_bind(void *obj, int arg, const char *buf, uint32_t len)
{
    pci_device_t p;
    int k;

    (void)obj;
    (void)arg;
    k = vp_name_function(buf, len, &p);
    if (k != 0) {
        return k;
    }
    if (vp_busy) {
        return -EBUSY;
    }
    vp_busy = 1;
    k = vp_bind(&p);
    vp_busy = 0;
    return k != 0 ? k : (int32_t)len;
}

/** /sys/bus/pci/drivers/virtio-pci/unbind: let go of the function named
 * (`vp_unbind`); -ENODEV for one the transport does not have, -EBUSY while
 * another write of the directory sleeps. */
static int32_t vp_store_unbind(void *obj, int arg, const char *buf, uint32_t len)
{
    pci_device_t p;
    struct virtio_pci *vp;
    int k;

    (void)obj;
    (void)arg;
    k = vp_name_function(buf, len, &p);
    if (k != 0) {
        return k;
    }
    vp = vp_record(&p);
    if (!vp) {
        return -ENODEV;
    }
    if (vp_busy) {
        return -EBUSY;
    }
    vp_busy = 1;
    vp_unbind(vp);
    vp_busy = 0;
    return (int32_t)len;
}

static const struct sysfs_attr vp_attr_bind = {"bind", 0200u, NULL, vp_store_bind};
static const struct sysfs_attr vp_attr_unbind = {"unbind", 0200u, NULL, vp_store_unbind};

/** The directory `name` below `parent`, made when it is not there yet: its
 * handle, or -errno. */
static int vp_dir_at(int parent, const char *name)
{
    int h;

    if (parent <= 0) {
        return parent < 0 ? parent : -ENOENT;
    }
    h = sysfs_node_child(parent, name, (int)k_strlen(name));
    return h > 0 ? h : sysfs_add_dir(parent, name);
}

/** Make /sys/bus/pci/drivers/virtio-pci with `bind` and `unbind`, as Linux
 * registers the driver whether or not a function is there for it. A
 * failure leaves the functions to the boot's probe alone, with a record. */
static void vp_sys_start(void)
{
    int dir = vp_dir_at(vp_dir_at(sysfs_anchor(SYSFS_BUS_PCI, 0), "drivers"), "virtio-pci");

    if (dir < 0 || sysfs_add_attr(dir, &vp_attr_bind, NULL, 0) < 0 ||
        sysfs_add_attr(dir, &vp_attr_unbind, NULL, 0) < 0) {
        klog(KLOG_WARN, "virtio", "/sys/bus/pci/drivers/virtio-pci not made: error %d",
             dir < 0 ? dir : -ENOMEM);
    }
}

int virtio_pci_probe(void)
{
    pci_device_t p;
    int i, n = 0;

    if (!vp_probed) {
        vp_sys_start();
    }
    vp_probed = 1;
    for (i = 0; pci_nth(i, &p); i++) {
        if (vp_is_virtio(&p) && !vp_record(&p) && vp_probe_one(&p, NULL) == 0) {
            n++;
        }
    }
    return n;
}

int virtio_pci_bind(uint8_t bus, uint8_t devfn)
{
    pci_device_t p;
    int i, k;

    for (i = 0; pci_nth(i, &p); i++) {
        if (p.bus == bus && (uint8_t)(p.device << 3 | p.function) == devfn) {
            if (vp_busy) {
                return -EBUSY;
            }
            vp_busy = 1;
            k = vp_bind(&p);
            vp_busy = 0;
            return k;
        }
    }
    return -ENODEV;
}

int virtio_pci_find(uint16_t id, int nth, struct virtio_dev **out)
{
    struct virtio_pci *vp;

    if (!vp_probed) {
        virtio_pci_probe();
    }
    for (vp = vp_list; vp; vp = vp->next) {
        if (vp->dev.id == id && nth-- == 0) {
            vp->held = 1;
            *out = &vp->dev;
            return 0;
        }
    }
    return -ENODEV;
}

const pci_device_t *virtio_pci_function(const struct virtio_dev *d)
{
    return d && (d->ops == &vp_ops || d->ops == &vp_legacy_ops)
               ? &((const struct virtio_pci *)d->transport)->pci
               : NULL;
}
