/*
 * pci.c - PCI bus enumeration implementation
 *
 * Scans the PCI bus to find devices by class/subclass or vendor/device ID.
 * Uses configuration mechanism #1 (ports 0xCF8 and 0xCFC).
 */

#include "pci.h"
#include "kio.h"
#include "serial.h"

/* PCI Configuration ports */
#define PCI_CONFIG_ADDRESS  0xCF8
#define PCI_CONFIG_DATA     0xCFC

/*
 * Build PCI configuration address
 * Format:
 *   Bit 31:    Enable bit (1)
 *   Bits 23-16: Bus number (0-255)
 *   Bits 15-11: Device number (0-31)
 *   Bits 10-8:  Function number (0-7)
 *   Bits 7-0:   Register offset (DWORD aligned, bits 1:0 = 0)
 */
static uint32_t pci_config_address(uint8_t bus, uint8_t device, uint8_t function, uint8_t offset)
{
    return 0x80000000 |
           ((uint32_t)bus << 16) |
           ((uint32_t)(device & 0x1F) << 11) |
           ((uint32_t)(function & 0x07) << 8) |
           (offset & 0xFC);
}

/*
 * Read 32-bit value from PCI configuration space
 */
uint32_t pci_read_config32(uint8_t bus, uint8_t device, uint8_t function, uint8_t offset)
{
    uint32_t addr = pci_config_address(bus, device, function, offset);
    outl(PCI_CONFIG_ADDRESS, addr);
    return inl(PCI_CONFIG_DATA);
}

/*
 * Read 16-bit value from PCI configuration space
 */
uint16_t pci_read_config16(uint8_t bus, uint8_t device, uint8_t function, uint8_t offset)
{
    uint32_t val = pci_read_config32(bus, device, function, offset & ~3);
    return (val >> ((offset & 2) * 8)) & 0xFFFF;
}

/*
 * Read 8-bit value from PCI configuration space
 */
uint8_t pci_read_config8(uint8_t bus, uint8_t device, uint8_t function, uint8_t offset)
{
    uint32_t val = pci_read_config32(bus, device, function, offset & ~3);
    return (val >> ((offset & 3) * 8)) & 0xFF;
}

/*
 * Write 32-bit value to PCI configuration space
 */
void pci_write_config32(uint8_t bus, uint8_t device, uint8_t function, uint8_t offset, uint32_t value)
{
    uint32_t addr = pci_config_address(bus, device, function, offset);
    outl(PCI_CONFIG_ADDRESS, addr);
    outl(PCI_CONFIG_DATA, value);
}

/*
 * Write 16-bit value to PCI configuration space
 */
void pci_write_config16(uint8_t bus, uint8_t device, uint8_t function, uint8_t offset, uint16_t value)
{
    uint32_t addr = pci_config_address(bus, device, function, offset & ~3);
    uint32_t old_val, new_val;
    int shift = (offset & 2) * 8;

    outl(PCI_CONFIG_ADDRESS, addr);
    old_val = inl(PCI_CONFIG_DATA);
    new_val = (old_val & ~(0xFFFF << shift)) | ((uint32_t)value << shift);
    outl(PCI_CONFIG_DATA, new_val);
}

/*
 * Check if a device exists at the given bus/device/function
 */
static bool pci_device_exists(uint8_t bus, uint8_t device, uint8_t function)
{
    uint16_t vendor = pci_read_config16(bus, device, function, PCI_VENDOR_ID);
    return vendor != 0xFFFF;
}

/*
 * Read device information into pci_device_t structure
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

    /* Read all 6 BARs */
    for (i = 0; i < 6; i++) {
        out->bar[i] = pci_read_config32(bus, device, function, PCI_BAR0 + i * 4);
    }
}

/*
 * Find a PCI device by class and subclass
 */
bool pci_find_device(uint8_t class_code, uint8_t subclass, pci_device_t *out)
{
    uint16_t bus;
    uint8_t device, function;

    for (bus = 0; bus < 256; bus++) {
        for (device = 0; device < 32; device++) {
            /* Check function 0 first */
            if (!pci_device_exists((uint8_t)bus, device, 0)) {
                continue;
            }

            /* Check all functions if multi-function device */
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

                /* Only check other functions if bit 7 of header type is set */
                if (function == 0) {
                    uint8_t header_type = pci_read_config8((uint8_t)bus, device, 0, PCI_HEADER_TYPE);
                    if (!(header_type & 0x80)) {
                        break;  /* Not multi-function */
                    }
                }
            }
        }
    }

    return false;
}

/*
 * Find a PCI device by class and subclass, optionally skipping past a previous device
 */
bool pci_find_device_skip(uint8_t class_code, uint8_t subclass, pci_device_t *skip_dev, pci_device_t *out)
{
    uint16_t bus;
    uint8_t device, function;
    bool past_skip = (skip_dev == NULL);

    for (bus = 0; bus < 256; bus++) {
        for (device = 0; device < 32; device++) {
            /* Check function 0 first */
            if (!pci_device_exists((uint8_t)bus, device, 0)) {
                continue;
            }

            /* Check all functions if multi-function device */
            for (function = 0; function < 8; function++) {
                uint8_t cc, sc;

                if (!pci_device_exists((uint8_t)bus, device, function)) {
                    continue;
                }

                /* If we're at the skip device, mark that we've passed it */
                if (skip_dev != NULL &&
                    bus == skip_dev->bus &&
                    device == skip_dev->device &&
                    function == skip_dev->function) {
                    past_skip = true;
                    /* Continue to next function/device */
                    if (function == 0) {
                        uint8_t header_type = pci_read_config8((uint8_t)bus, device, 0, PCI_HEADER_TYPE);
                        if (!(header_type & 0x80)) {
                            break;  /* Not multi-function */
                        }
                    }
                    continue;
                }

                if (!past_skip) {
                    /* Only check other functions if bit 7 of header type is set */
                    if (function == 0) {
                        uint8_t header_type = pci_read_config8((uint8_t)bus, device, 0, PCI_HEADER_TYPE);
                        if (!(header_type & 0x80)) {
                            break;  /* Not multi-function */
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

                /* Only check other functions if bit 7 of header type is set */
                if (function == 0) {
                    uint8_t header_type = pci_read_config8((uint8_t)bus, device, 0, PCI_HEADER_TYPE);
                    if (!(header_type & 0x80)) {
                        break;  /* Not multi-function */
                    }
                }
            }
        }
    }

    return false;
}

/*
 * Find a PCI device by vendor and device ID
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
                    uint8_t header_type = pci_read_config8((uint8_t)bus, device, 0, PCI_HEADER_TYPE);
                    if (!(header_type & 0x80)) {
                        break;
                    }
                }
            }
        }
    }

    return false;
}

/*
 * Enable bus mastering for a PCI device
 */
void pci_enable_bus_master(pci_device_t *dev)
{
    uint16_t cmd = pci_read_config16(dev->bus, dev->device, dev->function, PCI_COMMAND);
    cmd |= PCI_CMD_BUS_MASTER;
    pci_write_config16(dev->bus, dev->device, dev->function, PCI_COMMAND, cmd);
}

/*
 * Enable memory space access for a PCI device
 */
void pci_enable_memory_space(pci_device_t *dev)
{
    uint16_t cmd = pci_read_config16(dev->bus, dev->device, dev->function, PCI_COMMAND);
    cmd |= PCI_CMD_MEMORY_SPACE;
    pci_write_config16(dev->bus, dev->device, dev->function, PCI_COMMAND, cmd);
}

/*
 * Enable interrupts for a PCI device (clear Interrupt Disable bit)
 */
void pci_enable_interrupts(pci_device_t *dev)
{
    uint16_t cmd = pci_read_config16(dev->bus, dev->device, dev->function, PCI_COMMAND);
    cmd &= ~PCI_CMD_INTERRUPT_DIS;
    pci_write_config16(dev->bus, dev->device, dev->function, PCI_COMMAND, cmd);
}

/*
 * Get the physical address from a BAR
 */
uint32_t pci_bar_address(uint32_t bar)
{
    if (bar & 0x01) {
        /* I/O space BAR */
        return bar & 0xFFFFFFFC;
    } else {
        /* Memory space BAR */
        return bar & 0xFFFFFFF0;
    }
}

/*
 * Check if a BAR is memory-mapped
 */
bool pci_bar_is_memory(uint32_t bar)
{
    return !(bar & 0x01);
}

/*
 * Check if a BAR is 64-bit
 */
bool pci_bar_is_64bit(uint32_t bar)
{
    return pci_bar_is_memory(bar) && ((bar & 0x06) == 0x04);
}

/*
 * Initialize PCI subsystem
 */
void init_pci(void)
{
    serial_printf("PCI: Bus scan complete\n");
}
