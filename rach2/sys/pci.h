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
#define PCI_VENDOR_ID       0x00    /* 16 bits */
#define PCI_DEVICE_ID       0x02    /* 16 bits */
#define PCI_COMMAND         0x04    /* 16 bits */
#define PCI_STATUS          0x06    /* 16 bits */
#define PCI_REVISION_ID     0x08    /* 8 bits */
#define PCI_PROG_IF         0x09    /* 8 bits */
#define PCI_SUBCLASS        0x0A    /* 8 bits */
#define PCI_CLASS           0x0B    /* 8 bits */
#define PCI_CACHE_LINE_SIZE 0x0C    /* 8 bits */
#define PCI_LATENCY_TIMER   0x0D    /* 8 bits */
#define PCI_HEADER_TYPE     0x0E    /* 8 bits */
#define PCI_BIST            0x0F    /* 8 bits */
#define PCI_BAR0            0x10    /* 32 bits */
#define PCI_BAR1            0x14    /* 32 bits */
#define PCI_BAR2            0x18    /* 32 bits */
#define PCI_BAR3            0x1C    /* 32 bits */
#define PCI_BAR4            0x20    /* 32 bits */
#define PCI_BAR5            0x24    /* 32 bits */
#define PCI_INTERRUPT_LINE  0x3C    /* 8 bits */
#define PCI_INTERRUPT_PIN   0x3D    /* 8 bits */

/*
 * PCI class codes
 */
#define PCI_CLASS_MULTIMEDIA    0x04
#define PCI_SUBCLASS_AUDIO      0x03    /* HD Audio */

/*
 * PCI command register bits
 */
#define PCI_CMD_IO_SPACE        0x0001
#define PCI_CMD_MEMORY_SPACE    0x0002
#define PCI_CMD_BUS_MASTER      0x0004
#define PCI_CMD_INTERRUPT_DIS   0x0400

/*
 * PCI device information structure
 */
typedef struct {
    uint16_t vendor_id;
    uint16_t device_id;
    uint8_t  class_code;
    uint8_t  subclass;
    uint8_t  prog_if;
    uint8_t  revision;
    uint8_t  bus;
    uint8_t  device;
    uint8_t  function;
    uint8_t  irq;
    uint32_t bar[6];        /* Base Address Registers */
} pci_device_t;

/** Read a 32-bit dword from PCI configuration space. */
uint32_t pci_read_config32(uint8_t bus, uint8_t device, uint8_t function, uint8_t offset);

/** Read a 16-bit word from PCI configuration space. */
uint16_t pci_read_config16(uint8_t bus, uint8_t device, uint8_t function, uint8_t offset);

/** Read an 8-bit byte from PCI configuration space. */
uint8_t pci_read_config8(uint8_t bus, uint8_t device, uint8_t function, uint8_t offset);

/** Write a 32-bit dword to PCI configuration space. */
void pci_write_config32(uint8_t bus, uint8_t device, uint8_t function, uint8_t offset, uint32_t value);

/** Write a 16-bit word to PCI configuration space. */
void pci_write_config16(uint8_t bus, uint8_t device, uint8_t function, uint8_t offset, uint16_t value);

/** Find the first device by class and subclass; true and fills `out` on a match. */
bool pci_find_device(uint8_t class_code, uint8_t subclass, pci_device_t *out);

/** Find the next class/subclass match after `skip_dev`; NULL `skip_dev` starts from the first slot. */
bool pci_find_device_skip(uint8_t class_code, uint8_t subclass, pci_device_t *skip_dev, pci_device_t *out);

/** Find the first device by vendor and device ID; true and fills `out` on a match. */
bool pci_find_device_by_id(uint16_t vendor_id, uint16_t device_id, pci_device_t *out);

/** Set the Bus Master bit so the device can drive DMA. */
void pci_enable_bus_master(pci_device_t *dev);

/** Set the Memory Space bit so the device responds to its memory BARs (MMIO). */
void pci_enable_memory_space(pci_device_t *dev);

/** Clear the Interrupt Disable bit to re-enable the device's legacy INTx line. */
void pci_enable_interrupts(pci_device_t *dev);

/** Recover a BAR's base address by masking off its type bits. */
uint32_t pci_bar_address(uint32_t bar);

/** True if a BAR is memory-mapped (bit 0 clear), false if I/O space. */
bool pci_bar_is_memory(uint32_t bar);

/** True if a BAR is a 64-bit memory BAR (uses two consecutive BARs). */
bool pci_bar_is_64bit(uint32_t bar);

/*
 * Initialize the PCI subsystem and scan for devices, printing each
 * discovered device to serial for debugging.
 */
void init_pci(void);

#endif /* _PCI_H_ */
