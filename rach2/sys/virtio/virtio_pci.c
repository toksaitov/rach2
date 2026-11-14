/*
 * virtio_pci.c: the virtio 1.x PCI transport: a function's vendor
 * capabilities and the windows they name, its common configuration, its
 * queues' notifications, and its interrupts by MSI-X or by its INTx line.
 */

#include "virtio.h"

#include "../fs/synthfs.h"
#include "../fs/sysfs.h"
#include "../irq.h"
#include "../klog.h"
#include "../pci.h"
#include "../lib/kerrno.h"
#include "../lib/kstring.h"
#include "../lib/spinlock.h"
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

/* The ISR status byte, read only on the INTx line, which its read clears. */
#define VP_ISR_QUEUE  0x1u
#define VP_ISR_CONFIG 0x2u

/* Devices this transport holds, the interrupt slots they share, the shared
 * memory regions kept for each, and the most of a window that is mapped. */
#define VP_DEVICES 4
#define VP_SLOTS   16
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
    uintptr_t base; /* the kernel's address, 0 when absent */
    uint32_t len;   /* the bytes mapped */
    uint8_t bar;
    uint32_t offset; /* into the BAR */
};

struct vp_shm {
    uint8_t id;
    uint64_t addr, len;
};

struct virtio_pci {
    struct virtio_dev dev;
    pci_device_t pci;
    int used;
    struct vp_window common, notify, isr, device;
    uint32_t notify_mult; /* bytes between queues' notification addresses */
    struct vp_shm shm[VP_SHM];
    int nshm;
    int mode;                          /* VP_IRQ_* */
    uintptr_t msix_table;              /* mapped, 0 until needed */
    uint32_t msix_entries;             /* its entries */
    int vector[VIRTIO_MAX_QUEUES + 1]; /* by MSI-X entry, -1 when none */
    int slot[VIRTIO_MAX_QUEUES + 1];   /* by MSI-X entry, -1 when none */
    int nentries;                      /* entries in use */
    int line;                          /* the INTx line, while on it */
    char name[16];                     /* "virtioN" */
};

/* An interrupt slot: which device and target its vector serves, and how
 * many of its handlers run now, which vp_msix_free waits out. */
struct vp_slot {
    struct virtio_pci *vp;
    int target;
    uint32_t inflight;
};

static struct virtio_pci vp_devs[VP_DEVICES];
static struct vp_slot vp_slots[VP_SLOTS];

/* The slots' owners and counts, which a handler without the big lock reads
 * while vp_msix_free, under it, gives a slot back. */
static struct spinlock vp_slots_lock = SPINLOCK_INIT_NAMED("virtio_slots_lock");

/** The record behind `d`. */
static struct virtio_pci *vp_of(struct virtio_dev *d)
{
    return (struct virtio_pci *)d->transport;
}

/** Read the 8-bit field at `off` of the common configuration. */
static uint8_t vp_r8(struct virtio_pci *vp, uint32_t off)
{
    return mmio_read8(vp->common.base + off);
}

/** Read the 16-bit field at `off` of the common configuration. */
static uint16_t vp_r16(struct virtio_pci *vp, uint32_t off)
{
    return mmio_read16(vp->common.base + off);
}

/** Read the 32-bit field at `off` of the common configuration. */
static uint32_t vp_r32(struct virtio_pci *vp, uint32_t off)
{
    return mmio_read32(vp->common.base + off);
}

/** Write the 8-bit field at `off` of the common configuration. */
static void vp_w8(struct virtio_pci *vp, uint32_t off, uint8_t v)
{
    mmio_write8(vp->common.base + off, v);
}

/** Write the 16-bit field at `off` of the common configuration. */
static void vp_w16(struct virtio_pci *vp, uint32_t off, uint16_t v)
{
    mmio_write16(vp->common.base + off, v);
}

/** Write the 32-bit field at `off` of the common configuration. */
static void vp_w32(struct virtio_pci *vp, uint32_t off, uint32_t v)
{
    mmio_write32(vp->common.base + off, v);
}

/** A 64-bit field as two 32-bit writes, low first (virtio 1.2, 4.1.3.1). */
static void vp_w64(struct virtio_pci *vp, uint32_t off, uint64_t v)
{
    vp_w32(vp, off, (uint32_t)v);
    vp_w32(vp, off + 4u, (uint32_t)(v >> 32));
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
 * three addresses, then enable it (virtio 1.2, 4.1.5.1.3). Returns 0, or
 * -EIO when the device refuses the entry or names a notification address
 * past its window.
 */
static int vp_queue_enable(struct virtio_dev *d, struct virtqueue *vq)
{
    struct virtio_pci *vp = vp_of(d);
    uint16_t entry = vp_queue_entry(vp, vq->index);
    uint32_t off;

    vp_w16(vp, VP_QUEUE_SELECT, vq->index);
    vp_w16(vp, VP_QUEUE_SIZE, vq->size);
    vp_w16(vp, VP_QUEUE_MSIX_VECTOR, entry);
    if (vp_r16(vp, VP_QUEUE_MSIX_VECTOR) != entry) {
        klog(KLOG_ERR, "virtio", "queue: %s: %d: the device refused msi-x entry %d", vp->name,
             vq->index, entry);
        return -EIO;
    }
    off = (uint32_t)vp_r16(vp, VP_QUEUE_NOTIFY_OFF) * vp->notify_mult;
    if (off > vp->notify.len || vp->notify.len - off < 2u) {
        klog(KLOG_ERR, "virtio", "queue: %s: %d: its notification lies past the window", vp->name,
             vq->index);
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
 * written (virtio 1.2, 4.1.5.2). */
static void vp_notify(struct virtio_dev *d, struct virtqueue *vq)
{
    (void)d;
    mmio_write16(vq->notify, vq->index);
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

static const isr_handler_t vp_slot_handler[VP_SLOTS] = {
    vp_slot0, vp_slot1, vp_slot2,  vp_slot3,  vp_slot4,  vp_slot5,  vp_slot6,  vp_slot7,
    vp_slot8, vp_slot9, vp_slot10, vp_slot11, vp_slot12, vp_slot13, vp_slot14, vp_slot15};

/** The INTx line, which several devices may share: each device on it reads
 * its ISR status, which says whether it raised the line and why. */
static void vp_intx(isr_params_t *p)
{
    int i;

    (void)p;
    for (i = 0; i < VP_DEVICES; i++) {
        struct virtio_pci *vp = &vp_devs[i];
        uint8_t isr;

        if (!vp->used || vp->mode != VP_IRQ_INTX) {
            continue;
        }
        isr = mmio_read8(vp->isr.base);
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
    for (e = 0; e <= VIRTIO_MAX_QUEUES; e++) {
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
        register_isr_handler((uint8_t)vector, vp_slot_handler[k], vp->name);
        if (vp->dev.irq_unlocked) {
            isr_register_flags((uint8_t)vector, vp_slot_handler[k], ISR_UNLOCKED);
        }
        if (pci_msix_entry_set(&vp->pci, vp->msix_table, (unsigned int)e, (uint8_t)vector) != 0) {
            vp_msix_free(vp);
            return -1;
        }
    }
    vp_w16(vp, VP_CONFIG_MSIX_VECTOR, 0);
    if (vp_r16(vp, VP_CONFIG_MSIX_VECTOR) != 0) {
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
 * polls.
 */
static int vp_irq_setup(struct virtio_dev *d, uint16_t nqueues)
{
    struct virtio_pci *vp = vp_of(d);
    uint32_t entries = isr_msi_ready() ? vp_msix_map(vp) : 0;
    int want = nqueues + 1;

    vp_msix_free(vp);
    vp->mode = VP_IRQ_NONE;
    if (entries >= (uint32_t)want && vp_slots_free() >= want) {
        vp->mode = VP_IRQ_MSIX;
        if (vp_msix_take(vp, want) == 0) {
            vp_intx_disable(vp, 1);
            klog(KLOG_INFO, "virtio",
                 "interrupts: %s: msi-x, a vector for each of %d queues "
                 "and the configuration",
                 vp->name, nqueues);
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
                 vp->name);
            return 0;
        }
    }
    vp_msix_free(vp);
    vp_w16(vp, VP_CONFIG_MSIX_VECTOR, VIRTIO_NO_VECTOR);
    if (vp->pci.irq == 0 || vp->pci.irq >= 16) {
        vp->mode = VP_IRQ_POLLED;
        vp_intx_disable(vp, 1);
        klog(KLOG_INFO, "virtio", "interrupts: %s: polled, no msi-x and no interrupt line",
             vp->name);
        return 0;
    }
    vp->mode = VP_IRQ_INTX;
    vp->line = vp->pci.irq;
    register_isr_handler((uint8_t)IRQ_VECTOR(vp->line), vp_intx, "virtio");
    vp_intx_disable(vp, 0);
    klog(KLOG_INFO, "virtio", "interrupts: %s: intx line %d", vp->name, vp->line);
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
 * line, whose handler then passes the device over. */
static void vp_irq_release(struct virtio_dev *d)
{
    struct virtio_pci *vp = vp_of(d);

    if (vp->mode == VP_IRQ_MSIX || vp->mode == VP_IRQ_SHARED) {
        vp_msix_free(vp);
    }
    vp_intx_disable(vp, 1);
    vp->mode = VP_IRQ_NONE;
}

/** A page of the DMA pool, below 4 GiB, which the device reads by its
 * physical address. */
static void *vp_dma_alloc(struct virtio_dev *d, uint32_t bytes, uint64_t *addr)
{
    void *frame;

    (void)d;
    if (bytes == 0 || bytes > PAGE_SIZE) {
        return 0;
    }
    frame = alloc_kernel_frame();
    if (!frame) {
        return 0;
    }
    k_memset(frame, 0, PAGE_SIZE);
    *addr = (uint64_t)paging_virt_to_phys((uintptr_t)frame);
    return frame;
}

/** Give back a page `vp_dma_alloc` gave. */
static void vp_dma_free(struct virtio_dev *d, void *p, uint32_t bytes)
{
    (void)d;
    (void)bytes;
    free_kernel_frame(p);
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
                                         NULL,
                                         NULL,
                                         NULL,
                                         NULL,
                                         NULL,
                                         NULL,
                                         NULL,
                                         NULL};

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
 * without wrapping. Another is passed over with a record. The region is
 * not mapped; a driver maps the pieces it needs.
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
             "refused: %s: shared memory %d: offset 0x%x%08x, length 0x%x%08x, bar %d of 0x%x%08x",
             vp->name, id, (unsigned)(off >> 32), (unsigned)off, (unsigned)(len >> 32),
             (unsigned)len, bar, (unsigned)(size >> 32), (unsigned)size);
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
    return 0;
}

/** Read, check and map the capabilities of the function `vp->pci`, and log
 * them. Returns 0, or -EIO with the reason in the log. */
static int vp_attach(struct virtio_pci *vp)
{
    int i;

    vp_read_caps(vp);
    if (vp->common.len < VP_COMMON_MIN || vp->notify.len == 0 || vp->isr.len == 0) {
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
        return -EIO;
    }
    pci_enable_memory_space(&vp->pci);
    pci_enable_bus_master(&vp->pci);
    vp_intx_disable(vp, 1);
    klog(KLOG_INFO, "virtio",
         "found: %s: %02x:%02x.%d %s, common bar %d+0x%x, notify bar %d+0x%x x%d, isr bar "
         "%d+0x%x, device %s%d+0x%x, %d queues",
         vp->name, vp->pci.bus, vp->pci.device, vp->pci.function, vp_type_name(vp->dev.id),
         vp->common.bar, vp->common.offset, vp->notify.bar, vp->notify.offset, vp->notify_mult,
         vp->isr.bar, vp->isr.offset, vp->device.len ? "bar " : "none ", vp->device.bar,
         vp->device.offset, vp_r16(vp, VP_NUM_QUEUES));
    for (i = 0; i < vp->nshm; i++) {
        klog(KLOG_INFO, "virtio", "found: %s: shared memory %d at %x:%x, %d MiB", vp->name,
             vp->shm[i].id, (unsigned)(vp->shm[i].addr >> 32), (unsigned)vp->shm[i].addr,
             (int)(vp->shm[i].len >> 20));
    }
    return 0;
}

/** Whether `p` is a function Linux's virtio-pci takes: Red Hat's vendor id
 * and a device id from 0x1000 to 0x107F, as virtio_pci_probe checks. */
static int vp_is_virtio(const pci_device_t *p)
{
    return p->vendor_id == VP_VENDOR && p->device_id >= VP_TRANSITION_FIRST &&
           p->device_id <= VP_MODERN_LAST;
}

/** Name `vp` "virtioN", N the number of virtio functions before its own
 * among the `at` first of the bus, as Linux's register_virtio_device
 * numbers the devices virtio-pci takes in the order of the bus. */
static void vp_name(struct virtio_pci *vp, int at)
{
    pci_device_t q;
    char digits[11];
    int n = 0, i, len = 6;

    for (i = 0; i < at && pci_nth(i, &q); i++) {
        n += vp_is_virtio(&q);
    }
    k_memcpy(vp->name, "virtio", 6);
    i = 0;
    do {
        digits[i++] = (char)('0' + n % 10);
        n /= 10;
    } while (n && i < (int)sizeof(digits));
    while (i > 0 && len < (int)sizeof(vp->name) - 1) {
        vp->name[len++] = digits[--i];
    }
    vp->name[len] = '\0';
}

int virtio_pci_find(uint16_t id, int nth, struct virtio_dev **out)
{
    pci_device_t p;
    struct virtio_pci *vp = 0;
    int i, k, key, seen = 0;

    for (i = 0; pci_nth(i, &p); i++) {
        if (p.vendor_id != VP_VENDOR || vp_type(&p) != id || seen++ != nth) {
            continue;
        }
        for (k = 0; k < VP_DEVICES; k++) {
            struct virtio_pci *c = &vp_devs[k];

            if (c->used && c->pci.bus == p.bus && c->pci.device == p.device &&
                c->pci.function == p.function) {
                *out = &c->dev;
                return 0;
            }
            if (!c->used && !vp) {
                vp = c;
            }
        }
        if (!vp) {
            klog(KLOG_WARN, "virtio", "refused: %02x:%02x.%d: the transport holds %d devices",
                 p.bus, p.device, p.function, VP_DEVICES);
            return -ENOMEM;
        }
        k_memset(vp, 0, sizeof(*vp));
        vp->pci = p;
        vp->dev.ops = &vp_ops;
        vp->dev.notify_unlocked = 1;
        vp->dev.transport = vp;
        vp->dev.id = id;
        vp_name(vp, i);
        vp->dev.name = vp->name;
        for (k = 0; k <= VIRTIO_MAX_QUEUES; k++) {
            vp->vector[k] = -1;
            vp->slot[k] = -1;
        }
        /* Linux's driver core shows the binding before virtio_pci_probe
         * runs, and a failure to show it fails the probe. */
        key = (int)p.bus << 8 | p.device << 3 | p.function;
        k = sysfs_pci_bind(key, "virtio-pci");
        if (k != 0) {
            klog(KLOG_WARN, "virtio", "refused: %s: driver link not added: error %d", vp->name, k);
            return k;
        }
        if (vp_attach(vp) != 0) {
            sysfs_pci_unbind(key);
            return -EIO;
        }
        k = virtio_bus_add(&vp->dev, sysfs_anchor(SYSFS_PCI_DEVICE, key),
                           pci_read_config16(p.bus, p.device, p.function, PCI_SUBSYSTEM_VENDOR));
        if (k != 0) {
            klog(KLOG_WARN, "virtio", "refused: %s: not added to the virtio bus: error %d",
                 vp->name, k);
            sysfs_pci_unbind(key);
            return k;
        }
        vp->used = 1;
        sysfs_pci_uevent(key, "bind");
        *out = &vp->dev;
        return 0;
    }
    return -ENODEV;
}

const pci_device_t *virtio_pci_function(const struct virtio_dev *d)
{
    return d && d->ops == &vp_ops ? &((const struct virtio_pci *)d->transport)->pci : NULL;
}
