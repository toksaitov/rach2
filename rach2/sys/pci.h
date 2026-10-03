/*
 * pci.h - PCI bus enumeration for Rach2
 *
 * Functions to scan the PCI bus and find devices by class/subclass.
 * Uses I/O ports 0xCF8 (config address) and 0xCFC (config data).
 */

#ifndef _PCI_H_
#define _PCI_H_

#include "lib/kint.h"
#include "lib/kdef.h"

/*
 * PCI configuration space offsets
 */
#define PCI_VENDOR_ID       0x00 /* 16 bits */
#define PCI_DEVICE_ID       0x02 /* 16 bits */
#define PCI_COMMAND         0x04 /* 16 bits */
#define PCI_STATUS          0x06 /* 16 bits */
#define PCI_REVISION_ID     0x08 /* 8 bits */
#define PCI_PROG_IF         0x09 /* 8 bits */
#define PCI_SUBCLASS        0x0A /* 8 bits */
#define PCI_CLASS           0x0B /* 8 bits */
#define PCI_CACHE_LINE_SIZE 0x0C /* 8 bits */
#define PCI_LATENCY_TIMER   0x0D /* 8 bits */
#define PCI_HEADER_TYPE     0x0E /* 8 bits */
#define PCI_BIST            0x0F /* 8 bits */
#define PCI_BAR0            0x10 /* 32 bits */
#define PCI_BAR1            0x14 /* 32 bits */
#define PCI_BAR2            0x18 /* 32 bits */
#define PCI_BAR3            0x1C /* 32 bits */
#define PCI_BAR4            0x20 /* 32 bits */
#define PCI_BAR5            0x24 /* 32 bits */
#define PCI_INTERRUPT_LINE  0x3C /* 8 bits */
#define PCI_INTERRUPT_PIN   0x3D /* 8 bits */

/*
 * PCI class codes
 */
#define PCI_CLASS_MULTIMEDIA   0x04
#define PCI_SUBCLASS_AUDIO     0x03 /* HD Audio */
#define PCI_CLASS_MASS_STORAGE 0x01
#define PCI_SUBCLASS_SATA      0x06 /* AHCI when prog-if is 0x01 */
#define PCI_CLASS_SERIAL_BUS   0x0C
#define PCI_SUBCLASS_USB       0x03 /* xHCI when prog-if is 0x30 */

/*
 * PCI command register bits
 */
#define PCI_CMD_IO_SPACE      0x0001
#define PCI_CMD_MEMORY_SPACE  0x0002
#define PCI_CMD_BUS_MASTER    0x0004
#define PCI_CMD_INTERRUPT_DIS 0x0400

/*
 * PCI device information structure
 */
typedef struct {
    uint16_t vendor_id;
    uint16_t device_id;
    uint8_t class_code;
    uint8_t subclass;
    uint8_t prog_if;
    uint8_t revision;
    uint8_t bus;
    uint8_t device;
    uint8_t function;
    uint8_t irq;
    uint32_t bar[6]; /* Base Address Registers */
} pci_device_t;

/*
 * Configuration space, by bus, device, function and offset. The dword read
 * and the dword and word writes are the architecture's, by whatever
 * mechanism its machine has (arch/x86/pciconf.c: ports 0xCF8 and 0xCFC);
 * the narrower reads are taken from a dword.
 */

/** Read a 32-bit dword from PCI configuration space. */
uint32_t pci_read_config32(uint8_t bus, uint8_t device, uint8_t function, uint8_t offset);

/** Read a 16-bit word from PCI configuration space. */
uint16_t pci_read_config16(uint8_t bus, uint8_t device, uint8_t function, uint8_t offset);

/** Read an 8-bit byte from PCI configuration space. */
uint8_t pci_read_config8(uint8_t bus, uint8_t device, uint8_t function, uint8_t offset);

/** Write a 32-bit dword to PCI configuration space. */
void pci_write_config32(uint8_t bus, uint8_t device, uint8_t function, uint8_t offset,
                        uint32_t value);

/** Write a 16-bit word to PCI configuration space. */
void pci_write_config16(uint8_t bus, uint8_t device, uint8_t function, uint8_t offset,
                        uint16_t value);

/*
 * The bus's I/O space, where a device's I/O base address register points:
 * the architecture's (arch/x86/pciconf.c: the processor's own ports).
 */
uint8_t pci_io_read8(uint16_t port);
uint16_t pci_io_read16(uint16_t port);
uint32_t pci_io_read32(uint16_t port);
void pci_io_write8(uint16_t port, uint8_t value);
void pci_io_write16(uint16_t port, uint16_t value);
void pci_io_write32(uint16_t port, uint32_t value);

/** Find the first device by class and subclass; true and fills `out` on a
 * match. */
bool pci_find_device(uint8_t class_code, uint8_t subclass, pci_device_t *out);

/** Find the next class/subclass match after `skip_dev`; NULL `skip_dev`
 * starts from the first slot. */
bool pci_find_device_skip(uint8_t class_code, uint8_t subclass, pci_device_t *skip_dev,
                          pci_device_t *out);

/** Find the first device by vendor and device ID; true and fills `out` on a
 * match. */
bool pci_find_device_by_id(uint16_t vendor_id, uint16_t device_id, pci_device_t *out);

/**
 * The `nth` device of that identity on the bus, counting from zero.
 *
 * A machine may have several cards of one kind, and a driver that takes them
 * all asks for each in turn until this says there are no more.
 *
 * @return true with `*out` filled, false when there is no such device.
 */
bool pci_find_nth_by_id(uint16_t vendor_id, uint16_t device_id, int nth, pci_device_t *out);

/**
 * The `nth` function on the buses, counting from zero in the order of bus,
 * device and function, as /sys/bus/pci/devices lists them.
 *
 * @return true with `*out` filled, false when there are no more.
 */
bool pci_nth(int nth, pci_device_t *out);

/*
 * PCIe capability walk. The capability list starts at the pointer in config
 * space and is a singly linked list of (id, next) pairs; a bounded walk is
 * the only safe way to read it, because a device answering its own pointer
 * field with garbage would otherwise loop forever.
 */
#define PCI_STATUS_CAP_LIST     0x0010
#define PCI_CAP_PTR             0x34
#define PCI_CAP_ID_MSI          0x05
#define PCI_CAP_ID_MSIX         0x11
#define PCI_CAP_ID_PCIE         0x10
#define PCIE_LINK_CONTROL       0x10 /* offset within the PCIe capability */
#define PCIE_LINK_CTL_ASPM_MASK 0x0003

/**
 * Find a capability by id; its configuration-space offset, or 0 if absent.
 *
 * The walk is bounded and refuses an offset outside the capability region, so
 * a malformed list ends the search, not the machine.
 */
uint8_t pci_find_capability(uint8_t bus, uint8_t device, uint8_t function, uint8_t cap_id);

/* The MSI capability's control word. */
#define PCI_MSI_ENABLE          0x0001u
#define PCI_MSI_MULTIPLE_ENABLE 0x0070u
#define PCI_MSI_64BIT           0x0080u
#define PCI_MSI_PER_VECTOR      0x0100u /* the device can mask its own vectors */
#define PCI_MSIX_ENABLE         0x8000u

/**
 * The address a device writes, and the data it writes there, to raise
 * `vector`: the architecture's (arch/x86/pciconf.c, the local APIC's page).
 */
void pci_msi_message(uint8_t vector, uint32_t *address, uint32_t *data);

/** Program one message on `dev` and enable it: 0, or -1 with the reason in
 * the log. The architecture's message controller must be ready. */
int pci_msi_enable(const pci_device_t *dev, uint8_t vector);

/** Stop `dev` from sending messages. */
void pci_msi_disable(const pci_device_t *dev);

/**
 * Find the bridge whose secondary bus is `bus`, filling `out`; true if found.
 *
 * A PCIe link has two ends, and clearing a link state at one end alone leaves
 * it negotiated at the other, so a driver that must turn L1 off needs the
 * port above its device as well as the device.
 */
bool pci_find_bridge_for_bus(uint8_t bus, pci_device_t *out);

/** Set the Bus Master bit so the device can drive DMA. */
void pci_enable_bus_master(pci_device_t *dev);

/** Clear the Bus Master bit so the device can no longer drive DMA. */
void pci_disable_bus_master(pci_device_t *dev);

/** Set the Memory Space bit so the device responds to its memory BARs
 * (MMIO). */
void pci_enable_memory_space(pci_device_t *dev);

/** Clear the Interrupt Disable bit to re-enable the device's legacy INTx
 * line. */
void pci_enable_interrupts(pci_device_t *dev);

/** Recover a BAR's base address by masking off its type bits. */
uint32_t pci_bar_address(uint32_t bar);

/** True if a BAR is memory-mapped (bit 0 clear), false if I/O space. */
bool pci_bar_is_memory(uint32_t bar);

/** True if a BAR is a 64-bit memory BAR (uses two consecutive BARs). */
bool pci_bar_is_64bit(uint32_t bar);

#endif /* _PCI_H_ */
