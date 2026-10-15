/*
 * pci.c: the devices on the bus, found by walking configuration space, their
 * base address registers and capabilities, and the messages a device sends
 * in place of an interrupt line.
 *
 * How configuration space and the bus's I/O space are reached, and what a
 * message is written to, are the architecture's (arch/x86/pciconf.c on a
 * PC); the rest is the bus's own, the same on every machine.
 */

#include "arch/arch.h"
#include "klog.h"
#include "pci.h"
#include "kio.h"
#include "lib/kirq.h"

/** The word is taken from its aligned dword, the half that bit 1 of the
 * offset selects. */
uint16_t pci_read_config16(uint8_t bus, uint8_t device, uint8_t function, uint8_t offset)
{
    uint32_t val = pci_read_config32(bus, device, function, offset & ~3);
    return (val >> ((offset & 2) * 8)) & 0xFFFF;
}

/** The byte is taken from its aligned dword, the byte that the low two bits
 * of the offset select. */
uint8_t pci_read_config8(uint8_t bus, uint8_t device, uint8_t function, uint8_t offset)
{
    uint32_t val = pci_read_config32(bus, device, function, offset & ~3);
    return (val >> ((offset & 3) * 8)) & 0xFF;
}

/**
 * Report whether a device responds at this bus/device/function.
 *
 * An absent slot returns all ones from configuration space, so a vendor
 * ID of 0xFFFF means no device.
 */
static bool pci_device_exists(uint8_t bus, uint8_t device, uint8_t function)
{
    uint16_t vendor = pci_read_config16(bus, device, function, PCI_VENDOR_ID);
    return vendor != 0xFFFF;
}

/**
 * Fill `out` with the place, identity, class, interrupt line register and
 * six BARs of a function.
 */
static void pci_read_device_info(uint8_t bus, uint8_t device, uint8_t function, pci_device_t *out)
{
    int i;

    out->bus = bus;
    out->device = device;
    out->function = function;
    out->vendor_id = pci_read_config16(bus, device, function, PCI_VENDOR_ID);
    out->device_id = pci_read_config16(bus, device, function, PCI_DEVICE_ID);
    out->class_code = pci_read_config8(bus, device, function, PCI_CLASS);
    out->subclass = pci_read_config8(bus, device, function, PCI_SUBCLASS);
    out->prog_if = pci_read_config8(bus, device, function, PCI_PROG_IF);
    out->revision = pci_read_config8(bus, device, function, PCI_REVISION_ID);
    out->irq = pci_read_config8(bus, device, function, PCI_INTERRUPT_LINE);

    /* A type 0 header has six BARs at offsets PCI_BAR0, +4, ... +0x14. */
    for (i = 0; i < 6; i++) {
        out->bar[i] = pci_read_config32(bus, device, function, PCI_BAR0 + i * 4);
    }
}

/**
 * A single-function device may answer for all eight function numbers as
 * function 0, so functions 1 to 7 are read only when function 0's header
 * type has bit 7 set, which marks a multi-function device.
 * `pci_find_device_skip`, `pci_nth` and `pci_find_nth_by_id` read the slots
 * the same way.
 */
bool pci_find_device(uint8_t class_code, uint8_t subclass, pci_device_t *out)
{
    uint16_t bus;
    uint8_t device, function;

    for (bus = 0; bus < 256; bus++) {
        for (device = 0; device < 32; device++) {
            if (!pci_device_exists((uint8_t)bus, device, 0)) {
                continue;
            }

            for (function = 0; function < 8; function++) {
                uint8_t cc, sc;

                if (!pci_device_exists((uint8_t)bus, device, function)) {
                    continue;
                }

                cc = pci_read_config8((uint8_t)bus, device, function, PCI_CLASS);
                sc = pci_read_config8((uint8_t)bus, device, function, PCI_SUBCLASS);

                if (cc == class_code && sc == subclass) {
                    pci_read_device_info((uint8_t)bus, device, function, out);
                    return true;
                }

                /* Bit 7 of the header type marks a multi-function device. */
                if (function == 0) {
                    uint8_t header_type =
                        pci_read_config8((uint8_t)bus, device, 0, PCI_HEADER_TYPE);
                    if (!(header_type & 0x80)) {
                        break;
                    }
                }
            }
        }
    }

    return false;
}

/** The scan reads every slot up to and including `skip_dev` without
 * matching it, and matches from the slot after. */
bool pci_find_device_skip(uint8_t class_code, uint8_t subclass, pci_device_t *skip_dev,
                          pci_device_t *out)
{
    uint16_t bus;
    uint8_t device, function;
    bool past_skip = (skip_dev == NULL);

    for (bus = 0; bus < 256; bus++) {
        for (device = 0; device < 32; device++) {
            if (!pci_device_exists((uint8_t)bus, device, 0)) {
                continue;
            }

            for (function = 0; function < 8; function++) {
                uint8_t cc, sc;

                if (!pci_device_exists((uint8_t)bus, device, function)) {
                    continue;
                }

                if (skip_dev != NULL && bus == skip_dev->bus && device == skip_dev->device &&
                    function == skip_dev->function) {
                    past_skip = true;
                    /* Header type bit 7 marks a multi-function device. */
                    if (function == 0) {
                        uint8_t header_type =
                            pci_read_config8((uint8_t)bus, device, 0, PCI_HEADER_TYPE);
                        if (!(header_type & 0x80)) {
                            break;
                        }
                    }
                    continue;
                }

                if (!past_skip) {
                    /* Header type bit 7 marks a multi-function device. */
                    if (function == 0) {
                        uint8_t header_type =
                            pci_read_config8((uint8_t)bus, device, 0, PCI_HEADER_TYPE);
                        if (!(header_type & 0x80)) {
                            break;
                        }
                    }
                    continue;
                }

                cc = pci_read_config8((uint8_t)bus, device, function, PCI_CLASS);
                sc = pci_read_config8((uint8_t)bus, device, function, PCI_SUBCLASS);

                if (cc == class_code && sc == subclass) {
                    pci_read_device_info((uint8_t)bus, device, function, out);
                    return true;
                }

                /* Bit 7 of the header type marks a multi-function device. */
                if (function == 0) {
                    uint8_t header_type =
                        pci_read_config8((uint8_t)bus, device, 0, PCI_HEADER_TYPE);
                    if (!(header_type & 0x80)) {
                        break;
                    }
                }
            }
        }
    }

    return false;
}

bool pci_find_device_by_id(uint16_t vendor_id, uint16_t device_id, pci_device_t *out)
{
    return pci_find_nth_by_id(vendor_id, device_id, 0, out);
}

bool pci_nth(int nth, pci_device_t *out)
{
    uint16_t bus;
    uint8_t device, function;
    int seen = 0;

    for (bus = 0; bus < 256; bus++) {
        for (device = 0; device < 32; device++) {
            if (!pci_device_exists((uint8_t)bus, device, 0)) {
                continue;
            }
            for (function = 0; function < 8; function++) {
                if (pci_device_exists((uint8_t)bus, device, function) && seen++ == nth) {
                    pci_read_device_info((uint8_t)bus, device, function, out);
                    return true;
                }
                if (function == 0 &&
                    !(pci_read_config8((uint8_t)bus, device, 0, PCI_HEADER_TYPE) & 0x80)) {
                    break;
                }
            }
        }
    }
    return false;
}

bool pci_find_nth_by_id(uint16_t vendor_id, uint16_t device_id, int nth, pci_device_t *out)
{
    uint16_t bus;
    uint8_t device, function;
    int seen = 0;

    for (bus = 0; bus < 256; bus++) {
        for (device = 0; device < 32; device++) {
            if (!pci_device_exists((uint8_t)bus, device, 0)) {
                continue;
            }

            for (function = 0; function < 8; function++) {
                uint16_t vid, did;

                if (!pci_device_exists((uint8_t)bus, device, function)) {
                    continue;
                }

                vid = pci_read_config16((uint8_t)bus, device, function, PCI_VENDOR_ID);
                did = pci_read_config16((uint8_t)bus, device, function, PCI_DEVICE_ID);

                if (vid == vendor_id && did == device_id) {
                    if (seen++ == nth) {
                        pci_read_device_info((uint8_t)bus, device, function, out);
                        return true;
                    }
                }

                if (function == 0) {
                    uint8_t header_type =
                        pci_read_config8((uint8_t)bus, device, 0, PCI_HEADER_TYPE);
                    if (!(header_type & 0x80)) {
                        break;
                    }
                }
            }
        }
    }

    return false;
}

void pci_enable_bus_master(pci_device_t *dev)
{
    uint16_t cmd = pci_read_config16(dev->bus, dev->device, dev->function, PCI_COMMAND);
    cmd |= PCI_CMD_BUS_MASTER;
    pci_write_config16(dev->bus, dev->device, dev->function, PCI_COMMAND, cmd);
}

/**
 * The write that actually stops a device writing into memory. A driver that
 * turns its receiver and transmitter off has told the device to stop; this
 * takes away its ability to do it anyway, which is what matters when the
 * memory it holds addresses for is about to belong to something else.
 */
void pci_disable_bus_master(pci_device_t *dev)
{
    uint16_t cmd = pci_read_config16(dev->bus, dev->device, dev->function, PCI_COMMAND);
    cmd &= (uint16_t)~PCI_CMD_BUS_MASTER;
    pci_write_config16(dev->bus, dev->device, dev->function, PCI_COMMAND, cmd);
}

void pci_enable_memory_space(pci_device_t *dev)
{
    uint16_t cmd = pci_read_config16(dev->bus, dev->device, dev->function, PCI_COMMAND);
    cmd |= PCI_CMD_MEMORY_SPACE;
    pci_write_config16(dev->bus, dev->device, dev->function, PCI_COMMAND, cmd);
}

/** The Interrupt Disable bit masks the line while it is set. */
void pci_enable_interrupts(pci_device_t *dev)
{
    uint16_t cmd = pci_read_config16(dev->bus, dev->device, dev->function, PCI_COMMAND);
    cmd &= ~PCI_CMD_INTERRUPT_DIS;
    pci_write_config16(dev->bus, dev->device, dev->function, PCI_COMMAND, cmd);
}

/** An I/O BAR has its flags in bits 1:0 (mask 0xFFFFFFFC), a memory BAR in
 * bits 3:0 (mask 0xFFFFFFF0). */
uint32_t pci_bar_address(uint32_t bar)
{
    if (bar & 0x01) {
        return bar & 0xFFFFFFFC;
    } else {
        return bar & 0xFFFFFFF0;
    }
}

bool pci_bar_is_memory(uint32_t bar)
{
    return !(bar & 0x01);
}

/** A memory BAR's type, bits 2:1, is 0b10 (0x04 in place) for a 64-bit
 * BAR. */
bool pci_bar_is_64bit(uint32_t bar)
{
    return pci_bar_is_memory(bar) && ((bar & 0x06) == 0x04);
}

uint64_t pci_bar_address64(const pci_device_t *dev, int i)
{
    uint64_t phys;

    if (i < 0 || i > 5 || !pci_bar_is_memory(dev->bar[i])) {
        return 0;
    }
    phys = pci_bar_address(dev->bar[i]);
    if (pci_bar_is_64bit(dev->bar[i])) {
        if (i == 5) {
            return 0;
        }
        phys |= (uint64_t)dev->bar[i + 1] << 32;
    }
    return phys;
}

/**
 * The list is read only when the status register says one exists, and the
 * walk is bounded twice over: a fixed hop count, and an end at any offset
 * outside the capability region. A device that answers its own pointer with
 * garbage therefore ends the search, not the machine.
 */
uint8_t pci_find_capability(uint8_t bus, uint8_t device, uint8_t function, uint8_t cap_id)
{
    uint16_t status = pci_read_config16(bus, device, function, PCI_STATUS);
    uint8_t next;
    int hops;

    if (!(status & 0x0010)) {
        return 0; /* the device declares no capability list */
    }
    next = pci_read_config8(bus, device, function, PCI_CAP_PTR);
    for (hops = 0; hops < 48; hops++) {
        uint8_t offset, id;

        /* All ones is an absent device's; the mask would turn it into 0xFC. */
        if (next == 0xFF) {
            return 0;
        }
        offset = (uint8_t)(next & 0xFC);
        if (offset < 0x40) {
            return 0;
        }
        id = pci_read_config8(bus, device, function, offset);
        if (id == cap_id) {
            return offset;
        }
        next = pci_read_config8(bus, device, function, (uint8_t)(offset + 1));
    }
    return 0;
}

/** Header type 1 is a bridge's, and its secondary bus number is the byte at
 * offset 0x19. */
bool pci_find_bridge_for_bus(uint8_t bus, pci_device_t *out)
{
    uint16_t b;
    uint8_t d, f;

    for (b = 0; b < 256; b++) {
        for (d = 0; d < 32; d++) {
            for (f = 0; f < 8; f++) {
                uint16_t vendor = pci_read_config16((uint8_t)b, d, f, PCI_VENDOR_ID);
                uint8_t header;

                if (vendor == 0xFFFF) {
                    continue;
                }
                header = pci_read_config8((uint8_t)b, d, f, PCI_HEADER_TYPE);
                if ((header & 0x7F) != 0x01) {
                    continue; /* not a bridge */
                }
                if (pci_read_config8((uint8_t)b, d, f, 0x19) != bus) {
                    continue;
                }
                out->bus = (uint8_t)b;
                out->device = d;
                out->function = f;
                out->vendor_id = vendor;
                out->device_id = pci_read_config16((uint8_t)b, d, f, PCI_DEVICE_ID);
                return true;
            }
        }
    }
    return false;
}

/**
 * The order is the one the PCI specification requires and no other: the
 * enable bit is cleared first, in case firmware left it set, and the count
 * of vectors with it, so that nothing is sent while the address and the
 * data are written; an MSI-X capability is disabled, because a device with
 * both enabled is undefined; the address and the data are written; a device
 * that can mask its own vectors has this one's mask cleared, since firmware
 * may have left it set and the vector would then be programmed, enabled and
 * silent; and the enable bit is written last, when everything it acts on is
 * written. A count of zero tells a device that offers several vectors to use
 * one.
 *
 * `last` is the last byte the writes reach: the data's, or with per-vector
 * masking the mask register's, past the data. A capability that would run
 * past the 256 bytes an offset can name fails the call, since the writes
 * would wrap around onto the identity registers at offset zero.
 */
int pci_msi_enable(const pci_device_t *dev, uint8_t vector)
{
    uint8_t cap = pci_find_capability(dev->bus, dev->device, dev->function, PCI_CAP_ID_MSI);
    uint8_t msix = pci_find_capability(dev->bus, dev->device, dev->function, PCI_CAP_ID_MSIX);
    uint16_t control;
    uint32_t address, data;
    unsigned int last;

    if (cap == 0) {
        klog(KLOG_WARN, "pci", "msi: %x:%x.%d has no msi capability", dev->bus, dev->device,
             dev->function);
        return -1;
    }
    control = pci_read_config16(dev->bus, dev->device, dev->function, (uint8_t)(cap + 2));

    last = (unsigned int)cap + ((control & PCI_MSI_64BIT) ? 12u : 8u) + 1u;
    if ((control & PCI_MSI_PER_VECTOR) != 0) {
        last = (unsigned int)cap + ((control & PCI_MSI_64BIT) ? 16u : 12u) + 3u;
    }
    if (last > 0xFFu) {
        klog(KLOG_WARN, "pci", "msi: %x:%x.%d has a capability that runs past the end of its space",
             dev->bus, dev->device, dev->function);
        return -1;
    }

    control = (uint16_t)(control & ~(PCI_MSI_ENABLE | PCI_MSI_MULTIPLE_ENABLE));
    pci_write_config16(dev->bus, dev->device, dev->function, (uint8_t)(cap + 2), control);
    if (msix != 0) {
        uint16_t mxc = pci_read_config16(dev->bus, dev->device, dev->function, (uint8_t)(msix + 2));

        if ((mxc & PCI_MSIX_ENABLE) != 0) {
            /* MSI beside an enabled MSI-X is undefined by the specification. */
            pci_write_config16(dev->bus, dev->device, dev->function, (uint8_t)(msix + 2),
                               (uint16_t)(mxc & ~PCI_MSIX_ENABLE));
        }
    }
    pci_msi_message(vector, &address, &data);
    pci_write_config32(dev->bus, dev->device, dev->function, (uint8_t)(cap + 4), address);
    if ((control & PCI_MSI_64BIT) != 0) {
        pci_write_config32(dev->bus, dev->device, dev->function, (uint8_t)(cap + 8), 0);
        pci_write_config16(dev->bus, dev->device, dev->function, (uint8_t)(cap + 12),
                           (uint16_t)data);
    } else {
        pci_write_config16(dev->bus, dev->device, dev->function, (uint8_t)(cap + 8),
                           (uint16_t)data);
    }
    if ((control & PCI_MSI_PER_VECTOR) != 0) {
        /* The mask register follows the data; its low bit is this vector. */
        uint8_t mask_at = (uint8_t)(cap + ((control & PCI_MSI_64BIT) ? 16u : 12u));

        pci_write_config32(dev->bus, dev->device, dev->function, mask_at, 0);
    }
    pci_write_config16(dev->bus, dev->device, dev->function, (uint8_t)(cap + 2),
                       (uint16_t)(control | PCI_MSI_ENABLE));
    klog(KLOG_DEBUG, "pci", "msi: %x:%x.%d on vector %d", dev->bus, dev->device, dev->function,
         vector);
    return 0;
}

/**
 * Find `dev`'s MSI-X capability: its offset, or 0 when it has none or one
 * whose twelve bytes would run past the end of configuration space.
 */
static uint8_t pci_msix_capability(const pci_device_t *dev)
{
    uint8_t cap = pci_find_capability(dev->bus, dev->device, dev->function, PCI_CAP_ID_MSIX);

    return cap > 0xF4u ? 0 : cap;
}

int pci_msix_table(const pci_device_t *dev, uint8_t *bar, uint32_t *offset, uint32_t *entries)
{
    uint8_t cap = pci_msix_capability(dev);
    uint32_t where;
    uint16_t control;

    if (cap == 0) {
        return -1;
    }
    where = pci_read_config32(dev->bus, dev->device, dev->function, (uint8_t)(cap + 4));
    control = pci_read_config16(dev->bus, dev->device, dev->function, (uint8_t)(cap + 2));
    *bar = (uint8_t)(where & PCI_MSIX_BIR);
    *offset = where & ~PCI_MSIX_BIR;
    *entries = (control & PCI_MSIX_TABLE_LEN) + 1u;
    return 0;
}

/** MSI goes down first, since a device with both enabled is undefined. */
int pci_msix_hold(const pci_device_t *dev)
{
    uint8_t cap = pci_msix_capability(dev);
    uint16_t control;

    if (cap == 0) {
        return -1;
    }
    pci_msi_disable(dev);
    control = pci_read_config16(dev->bus, dev->device, dev->function, (uint8_t)(cap + 2));
    pci_write_config16(dev->bus, dev->device, dev->function, (uint8_t)(cap + 2),
                       (uint16_t)(control | PCI_MSIX_ENABLE | PCI_MSIX_MASK_ALL));
    return 0;
}

/**
 * MSI-X is held first, enabled with every vector masked, because some
 * devices answer their table only while MSI-X is enabled; the entry is
 * written with the same address and data MSI sends, and its own mask
 * cleared; and the function mask comes off last, when the entry is whole.
 */
int pci_msix_enable(const pci_device_t *dev, uintptr_t table, uint8_t vector)
{
    uint8_t cap = pci_msix_capability(dev);
    uint16_t control;
    uint32_t address, data;

    if (!table || pci_msix_hold(dev) != 0) {
        klog(KLOG_WARN, "pci", "msi-x: %x:%x.%d has no usable msi-x capability", dev->bus,
             dev->device, dev->function);
        return -1;
    }
    control = pci_read_config16(dev->bus, dev->device, dev->function, (uint8_t)(cap + 2));
    pci_msi_message(vector, &address, &data);
    mmio_write32(table, address);
    mmio_write32(table + 4, 0);
    mmio_write32(table + 8, data);
    mmio_write32(table + 12, 0);
    pci_write_config16(dev->bus, dev->device, dev->function, (uint8_t)(cap + 2),
                       (uint16_t)((control | PCI_MSIX_ENABLE) & ~PCI_MSIX_MASK_ALL));
    klog(KLOG_DEBUG, "pci", "msi-x: %x:%x.%d on vector %d", dev->bus, dev->device, dev->function,
         vector);
    return 0;
}

void pci_msi_disable(const pci_device_t *dev)
{
    uint8_t cap = pci_find_capability(dev->bus, dev->device, dev->function, PCI_CAP_ID_MSI);
    uint16_t control;

    if (cap == 0) {
        return;
    }
    control = pci_read_config16(dev->bus, dev->device, dev->function, (uint8_t)(cap + 2));
    pci_write_config16(dev->bus, dev->device, dev->function, (uint8_t)(cap + 2),
                       (uint16_t)(control & ~PCI_MSI_ENABLE));
}
