/*
 * PCI bus enumeration via configuration mechanism #1: write the target
 * address to port 0xCF8, then read or write the data at port 0xCFC.
 */

#include "pci.h"
#include "kio.h"

#define PCI_CONFIG_ADDRESS 0xCF8
#define PCI_CONFIG_DATA    0xCFC

/**
 * Pack a bus/device/function/offset tuple into a CONFIG_ADDRESS word.
 *
 * The mechanism #1 address layout, written to port 0xCF8:
 *   bit 31:     enable (must be 1)
 *   bits 23-16: bus number (0-255)
 *   bits 15-11: device number (0-31)
 *   bits 10-8:  function number (0-7)
 *   bits 7-0:   register offset, dword aligned (bits 1:0 read as 0)
 */
static uint32_t pci_config_address(uint8_t bus, uint8_t device, uint8_t function, uint8_t offset)
{
    return 0x80000000 | ((uint32_t)bus << 16) | ((uint32_t)(device & 0x1F) << 11) |
           ((uint32_t)(function & 0x07) << 8) | (offset & 0xFC);
}

/**
 * Read a 32-bit dword from configuration space.
 */
uint32_t pci_read_config32(uint8_t bus, uint8_t device, uint8_t function, uint8_t offset)
{
    uint32_t addr = pci_config_address(bus, device, function, offset);
    outl(PCI_CONFIG_ADDRESS, addr);
    return inl(PCI_CONFIG_DATA);
}

/**
 * Read a 16-bit word from configuration space.
 *
 * Config reads are always dword wide, so fetch the aligned dword and
 * select the half indexed by bit 1 of the offset.
 */
uint16_t pci_read_config16(uint8_t bus, uint8_t device, uint8_t function, uint8_t offset)
{
    uint32_t val = pci_read_config32(bus, device, function, offset & ~3);
    return (val >> ((offset & 2) * 8)) & 0xFFFF;
}

/**
 * Read an 8-bit byte from configuration space.
 *
 * Fetch the aligned dword and select the byte indexed by the low two
 * bits of the offset.
 */
uint8_t pci_read_config8(uint8_t bus, uint8_t device, uint8_t function, uint8_t offset)
{
    uint32_t val = pci_read_config32(bus, device, function, offset & ~3);
    return (val >> ((offset & 3) * 8)) & 0xFF;
}

/**
 * Write a 32-bit dword to configuration space.
 */
void pci_write_config32(uint8_t bus, uint8_t device, uint8_t function, uint8_t offset,
                        uint32_t value)
{
    uint32_t addr = pci_config_address(bus, device, function, offset);
    outl(PCI_CONFIG_ADDRESS, addr);
    outl(PCI_CONFIG_DATA, value);
}

/**
 * Write a 16-bit word to configuration space.
 *
 * The hardware path is dword wide, so read the aligned dword, splice the
 * new half into the position selected by bit 1 of the offset, and write
 * it back.
 */
void pci_write_config16(uint8_t bus, uint8_t device, uint8_t function, uint8_t offset,
                        uint16_t value)
{
    uint32_t addr = pci_config_address(bus, device, function, offset & ~3);
    uint32_t old_val, new_val;
    int shift = (offset & 2) * 8;

    outl(PCI_CONFIG_ADDRESS, addr);
    old_val = inl(PCI_CONFIG_DATA);
    new_val = (old_val & ~(0xFFFF << shift)) | ((uint32_t)value << shift);
    outl(PCI_CONFIG_DATA, new_val);
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
 * Fill `out` with the identity, class, IRQ line, and six BARs of a device.
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
 * Find the first device matching a class and subclass.
 *
 * Brute-force scans every bus/device/function of mechanism #1's address
 * space. Functions 1-7 are probed only when function 0's header type has
 * bit 7 set, which marks a multi-function device; a single-function
 * device aliases all eight function selectors onto function 0.
 *
 * @return true and fills `out` on a match, false if none responds.
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

/**
 * Find the next class/subclass match after a given device.
 *
 * Like `pci_find_device`, but resumes the enumeration past `skip_dev`:
 * the scan ignores every slot up to and including `skip_dev` before it
 * begins matching, which lets a caller walk all devices of one class one
 * at a time. A NULL `skip_dev` starts matching from the first slot.
 *
 * @return true and fills `out` on the next match, false if none remains.
 */
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
                    /* Bit 7 of the header type marks a multi-function
                     * device. */
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
                    /* Bit 7 of the header type marks a multi-function
                     * device. */
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

/**
 * Find the first device with an exact vendor and device ID.
 *
 * @return true and fills `out` on a match, false if none responds.
 */
bool pci_find_device_by_id(uint16_t vendor_id, uint16_t device_id, pci_device_t *out)
{
    uint16_t bus;
    uint8_t device, function;

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
                    pci_read_device_info((uint8_t)bus, device, function, out);
                    return true;
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

/**
 * Set the Bus Master bit in the device's command register.
 *
 * Bus mastering lets the device drive DMA on the bus.
 */
void pci_enable_bus_master(pci_device_t *dev)
{
    uint16_t cmd = pci_read_config16(dev->bus, dev->device, dev->function, PCI_COMMAND);
    cmd |= PCI_CMD_BUS_MASTER;
    pci_write_config16(dev->bus, dev->device, dev->function, PCI_COMMAND, cmd);
}

/**
 * Set the Memory Space bit in the device's command register.
 *
 * Until this bit is set the device ignores accesses to its memory BARs.
 */
void pci_enable_memory_space(pci_device_t *dev)
{
    uint16_t cmd = pci_read_config16(dev->bus, dev->device, dev->function, PCI_COMMAND);
    cmd |= PCI_CMD_MEMORY_SPACE;
    pci_write_config16(dev->bus, dev->device, dev->function, PCI_COMMAND, cmd);
}

/**
 * Clear the Interrupt Disable bit in the device's command register.
 *
 * This bit is active high, so clearing it re-enables the legacy INTx
 * line the device asserts.
 */
void pci_enable_interrupts(pci_device_t *dev)
{
    uint16_t cmd = pci_read_config16(dev->bus, dev->device, dev->function, PCI_COMMAND);
    cmd &= ~PCI_CMD_INTERRUPT_DIS;
    pci_write_config16(dev->bus, dev->device, dev->function, PCI_COMMAND, cmd);
}

/**
 * Strip the type bits from a BAR to recover its base address.
 *
 * An I/O BAR keeps bit 0 and bit 1 as flags (mask 0xFFFFFFFC); a memory
 * BAR keeps bits 3:0 as flags (mask 0xFFFFFFF0).
 */
uint32_t pci_bar_address(uint32_t bar)
{
    if (bar & 0x01) {
        return bar & 0xFFFFFFFC;
    } else {
        return bar & 0xFFFFFFF0;
    }
}

/* Bit 0 of a BAR clear means a memory-space region, set means I/O space. */
bool pci_bar_is_memory(uint32_t bar)
{
    return !(bar & 0x01);
}

/* Memory BAR type bits 2:1 equal to 0b10 (so bits = 0x04) mark a 64-bit BAR. */
bool pci_bar_is_64bit(uint32_t bar)
{
    return pci_bar_is_memory(bar) && ((bar & 0x06) == 0x04);
}

/**
 * Walk the capability list for `cap_id`.
 *
 * The list is read only when the status register says one exists, and the
 * walk is bounded twice over: a fixed hop count, and a refusal of any offset
 * outside the capability region. A device that answers its own pointer with
 * garbage therefore ends the search rather than the machine.
 *
 * @return the capability's configuration-space offset, or 0 if it is absent.
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

        /* All ones is what a bus answers for a device that is not there or has
         * stopped responding, and it has to be recognised before the alignment
         * mask, which would turn it into a plausible 0xfc and follow it. */
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

/**
 * Find the bridge whose secondary bus number is `bus`.
 *
 * Header type 1 is a bridge, and its secondary bus number sits at offset
 * 0x19. The scan covers the buses this kernel enumerates elsewhere.
 *
 * @return true when a bridge was found, with `out` filled.
 */
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
