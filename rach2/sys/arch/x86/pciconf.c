/*
 * The PC's half of PCI: configuration mechanism #1, which writes the target
 * address to port 0xCF8 and then reads or writes the data at port 0xCFC; the
 * bus's I/O space, which is the processor's own ports; and the message a
 * device sends to raise a vector, a write to the local APIC.
 *
 * The address port is one register that every access shares, so an access
 * taken apart in the middle by another one reads or writes whatever the
 * second selected. Until this milestone configuration space had one caller,
 * the boot, and no interrupt handler touched it; now the USB handler can
 * silence its controller from inside an interrupt. Every access below is
 * therefore taken with interrupts off, which is two instructions on a path
 * that already costs two port writes.
 */

#include "lapic.h"
#include "portio.h"

#include "../../pci.h"

#include "../../lib/kirq.h"

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
    uint32_t flags = kirq_save();
    uint32_t value;

    outl(PCI_CONFIG_ADDRESS, addr);
    value = inl(PCI_CONFIG_DATA);
    kirq_restore(flags);
    return value;
}

/**
 * Write a 32-bit dword to configuration space.
 */
void pci_write_config32(uint8_t bus, uint8_t device, uint8_t function, uint8_t offset,
                        uint32_t value)
{
    uint32_t addr = pci_config_address(bus, device, function, offset);
    uint32_t flags = kirq_save();

    outl(PCI_CONFIG_ADDRESS, addr);
    outl(PCI_CONFIG_DATA, value);
    kirq_restore(flags);
}

/**
 * Write a 16-bit word to configuration space.
 *
 * The data port takes an access of the width the register has, at the port
 * offset the register's own address selects, so a halfword write is one
 * `outw` at `0xCFC` or `0xCFE`. It is not a read of the dword and a write of
 * it back: the other half of the dword holding the command word is the
 * status word, whose bits are cleared by writing them as one, and writing it
 * back would clear every fault the bus had latched there.
 */
void pci_write_config16(uint8_t bus, uint8_t device, uint8_t function, uint8_t offset,
                        uint16_t value)
{
    uint32_t addr = pci_config_address(bus, device, function, offset & ~3);
    uint32_t flags = kirq_save();

    outl(PCI_CONFIG_ADDRESS, addr);
    outw((uint16_t)(PCI_CONFIG_DATA + (offset & 2)), value);
    kirq_restore(flags);
}

uint8_t pci_io_read8(uint16_t port)
{
    return inb(port);
}

uint16_t pci_io_read16(uint16_t port)
{
    return inw(port);
}

uint32_t pci_io_read32(uint16_t port)
{
    return ind(port);
}

void pci_io_write8(uint16_t port, uint8_t value)
{
    outb(port, value);
}

void pci_io_write16(uint16_t port, uint16_t value)
{
    outw(port, value);
}

void pci_io_write32(uint16_t port, uint32_t value)
{
    outd(port, value);
}

/* Where a message to the local APIC is written: its page, with the
 * destination processor's identity in bits 12 to 19. */
#define PCI_MSI_ADDRESS_BASE 0xFEE00000u

/**
 * The message that raises `vector`: a write to the local APIC of this
 * processor, the vector in the data's low byte, fixed delivery, edge.
 */
void pci_msi_message(uint8_t vector, uint32_t *address, uint32_t *data)
{
    *address = PCI_MSI_ADDRESS_BASE | (lapic_id() << 12);
    *data = vector;
}
