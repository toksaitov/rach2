/*
 * pci.h: the PCI bus, its configuration space, the search for a device by
 * class or identity, its base address registers and capabilities, and the
 * messages it sends in place of an interrupt line.
 */

#ifndef _PCI_H_
#define _PCI_H_

#include "arch/arch.h"
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
 * A function on the bus as a search finds it: where it is, its identity and
 * class from its configuration header, its interrupt line register, and its
 * six base address registers as read.
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
 * Configuration space, by bus, device, function and offset, the offset in
 * the first 256 bytes. The dword read and the writes are the architecture's,
 * by whatever mechanism its machine has (arch/x86/pciconf.c: the ECAM window
 * where the platform mapped one, ports 0xCF8 and 0xCFC otherwise); the
 * narrower reads are taken from a dword. Each access is one load or store,
 * or runs with interrupts off, so an interrupt handler may make one. A
 * function that is not there reads as all ones.
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

/**
 * Write a 16-bit word to PCI configuration space, as one access of a word's
 * width and not a read and a write of its dword: the status word beside the
 * command word has bits that a write of one clears, and writing it back
 * would clear every error the device had latched there.
 */
void pci_write_config16(uint8_t bus, uint8_t device, uint8_t function, uint8_t offset,
                        uint16_t value);

/** Write an 8-bit byte to PCI configuration space, as one access of a
 * byte's width, for the reason `pci_write_config16` gives. */
void pci_write_config8(uint8_t bus, uint8_t device, uint8_t function, uint8_t offset,
                       uint8_t value);

/**
 * A dword of a function's extended configuration space, offsets 256 to
 * 4095, which only an ECAM window reaches; below 256 by whichever mechanism
 * serves the bus. All ones for an offset of 256 or more when no ECAM window
 * covers the bus.
 */
uint32_t pci_read_config32_ext(uint8_t bus, uint8_t device, uint8_t function, uint16_t offset);

/** Write a dword of extended configuration space; nothing when no ECAM
 * window covers the bus and the offset is 256 or more. */
void pci_write_config32_ext(uint8_t bus, uint8_t device, uint8_t function, uint16_t offset,
                            uint32_t value);

/** Write a word of extended space as one 16-bit store, so adjacent
 * write-one-to-clear bits are untouched. No write outside an ECAM window
 * at offsets of 256 or more. `offset` must be even. */
void pci_write_config16_ext(uint8_t bus, uint8_t device, uint8_t function, uint16_t offset,
                            uint16_t value);

/** Write one byte of extended space, with the same reach as the word. */
void pci_write_config8_ext(uint8_t bus, uint8_t device, uint8_t function, uint16_t offset,
                           uint8_t value);

/**
 * Read or write a byte, a word or a dword at `port` of the bus's I/O space,
 * where a device's I/O base address register points: the architecture's
 * (arch/x86/pciconf.c: the processor's own ports).
 */
uint8_t pci_io_read8(uint16_t port);
uint16_t pci_io_read16(uint16_t port);
uint32_t pci_io_read32(uint16_t port);
void pci_io_write8(uint16_t port, uint8_t value);
void pci_io_write16(uint16_t port, uint16_t value);
void pci_io_write32(uint16_t port, uint32_t value);

/*
 * The searches below read one list of the functions on the buses, which the
 * first of them fills by walking bus 0, every bus behind a PCI-to-PCI or
 * CardBus bridge and every other root bus, once for the life of the kernel
 * (pci.c). The list is in
 * the order of bus, device and function; functions 1 to 7 of a device are on
 * it only when function 0's header says it has several. A search reads the
 * configuration space of the function it returns, so `out`'s BARs and
 * interrupt line are as they are now.
 */

/** Find the first function by class and subclass; true and fills `out` on a
 * match, false when none matches. */
bool pci_find_device(uint8_t class_code, uint8_t subclass, pci_device_t *out);

/** Find the next class/subclass match after the function `skip_dev` is,
 * so that a caller can walk every function of one class; NULL `skip_dev`
 * starts from the first function, and a `skip_dev` not on the list finds
 * nothing. */
bool pci_find_device_skip(uint8_t class_code, uint8_t subclass, pci_device_t *skip_dev,
                          pci_device_t *out);

/** Find the first device by vendor and device ID; true and fills `out` on a
 * match. */
bool pci_find_device_by_id(uint16_t vendor_id, uint16_t device_id, pci_device_t *out);

/**
 * The `nth` device of that identity on the bus, counting from zero.
 *
 * A machine may have several cards of one kind, and a driver that takes them
 * all calls this with each `nth` in turn until it returns false.
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
 * The capability list, which a status bit says the function has. It starts
 * at the pointer at 0x34 of configuration space and is a singly linked list
 * of (id, next) pairs; a bounded walk is the only safe way to read it,
 * because a device answering its own pointer field with garbage would
 * otherwise loop forever. The PCI Express capability's link control word
 * has the ASPM bits.
 */
#define PCI_STATUS_CAP_LIST     0x0010
#define PCI_CAP_PTR             0x34
#define PCI_CAP_ID_MSI          0x05
#define PCI_CAP_ID_MSIX         0x11
#define PCI_CAP_ID_PCIE         0x10
#define PCI_CAP_ID_PM           0x01
#define PCI_CAP_ID_AF           0x13 /* Advanced Features: a reset of its own */
#define PCIE_LINK_CONTROL       0x10 /* offset within the PCIe capability */
#define PCIE_LINK_CTL_ASPM_MASK 0x0003

/**
 * Find a capability by id; its configuration-space offset, or 0 if absent.
 *
 * The walk follows at most 48 links and stops at a pointer of all ones or
 * one below the capability region at 0x40, so a malformed list ends the
 * search, not the machine.
 */
uint8_t pci_find_capability(uint8_t bus, uint8_t device, uint8_t function, uint8_t cap_id);

/* The MSI capability's control word. */
#define PCI_MSI_ENABLE          0x0001u
#define PCI_MSI_MULTIPLE_CAP    0x000eu /* log2 of the vectors the device can use */
#define PCI_MSI_MULTIPLE_ENABLE 0x0070u
#define PCI_MSI_64BIT           0x0080u
#define PCI_MSI_PER_VECTOR      0x0100u /* the device can mask its own vectors */
#define PCI_MSIX_ENABLE         0x8000u

/**
 * The address a device writes, and the data it writes there, to raise
 * `vector`: the architecture's (arch/x86/pciconf.c, the local APIC's page).
 */
void pci_msi_message(uint8_t vector, uint32_t *address, uint32_t *data);

/**
 * Program one message on `dev`, the architecture's message for `vector`
 * (`pci_msi_message`), and enable it, with MSI-X disabled and the vector's
 * own mask cleared where the device has one: 0, or -1 with the reason in the
 * log when the device has no MSI capability or one that runs past the end of
 * its configuration space. A device that offers several vectors is told to
 * use one. The architecture's message controller must be ready, and the
 * device a bus master, since a message is a memory write. While a remapper
 * is in place (`pci_msi_remap_start`) the message written is its remapped
 * one, and -1 is also returned when it has no entry to give.
 */
int pci_msi_enable(const pci_device_t *dev, uint8_t vector);

/** Stop `dev` from sending MSI messages; a device with no MSI capability is
 * left alone. A remapped message is rewritten in the compatibility format
 * and its entry given back; otherwise the address and data stay. */
void pci_msi_disable(const pci_device_t *dev);

/* The MSI-X capability: its control word's function mask and table size,
 * and the BAR index in the low bits of the table's offset. An entry of the
 * table is four dwords: the address, its upper half, the data, and the
 * vector control, whose low bit masks the entry. */
#define PCI_MSIX_MASK_ALL  0x4000u
#define PCI_MSIX_TABLE_LEN 0x07FFu
#define PCI_MSIX_BIR       0x7u

/**
 * Where `dev`'s MSI-X table is: the BAR it is in, its offset into that BAR,
 * and its number of entries. 0, or -1 when the device has no MSI-X
 * capability or one that runs past the end of its configuration space.
 */
int pci_msix_table(const pci_device_t *dev, uint8_t *bar, uint32_t *offset, uint32_t *entries);

/**
 * Enable `dev`'s MSI-X with every vector masked by the function mask, and
 * MSI disabled: 0, or -1 when the device has no MSI-X capability.
 *
 * A device may tie a vector to what it sets up only while MSI-X is on, as
 * QEMU's NVMe controller ties a completion queue's vector when the queue is
 * created, so a driver that will take messages holds MSI-X this way before
 * that setup. A message raised meanwhile waits in the device's pending bits
 * until `pci_msix_enable` lifts the mask.
 */
int pci_msix_hold(const pci_device_t *dev);

/**
 * Program entry 0 of `dev`'s MSI-X table, which the caller has mapped at
 * `table`, to send `vector` to this processor, and enable MSI-X with MSI
 * disabled: 0, or -1 with the reason in the log. The local APIC must be
 * ready and the device a bus master.
 */
int pci_msix_enable(const pci_device_t *dev, uintptr_t table, uint8_t vector);

/** Disable `dev`'s MSI-X, and give back the remapping entry of each entry
 * `pci_msix_enable` or `pci_msix_entry_set` wrote, which is rewritten in the
 * compatibility format. */
void pci_msix_disable(const pci_device_t *dev);

/**
 * Program entry `index` of `dev`'s MSI-X table, which the caller has mapped
 * at `table`, to send `vector` to this processor, and unmask it; MSI-X must
 * be enabled (`pci_msix_hold`). While a remapper is in place the entry gets
 * a remapping entry of its own. 0, or -1 for an index past the table, no
 * MSI-X capability, or no record or remapping entry to give. For a driver
 * of several vectors, as VFIO is.
 */
int pci_msix_entry_set(const pci_device_t *dev, uintptr_t table, unsigned int index,
                       uint8_t vector);

/** Mask entry `index` of `dev`'s MSI-X table, which `pci_msix_entry_set`
 * programmed, and give its remapping entry back; nothing for an entry not
 * programmed. */
void pci_msix_entry_clear(const pci_device_t *dev, unsigned int index);

/** Lift `dev`'s MSI-X function mask, which `pci_msix_hold` set, once its
 * entries are programmed. */
void pci_msix_unmask(const pci_device_t *dev);

/*
 * An interrupt remapper's way to the messages devices send. `map` takes an
 * entry of the remapper's table for the compatibility-format message
 * (address, data) that `pci_msi_message` made for `dev`, and gives the
 * remapped address and data to write in its place: the entry's handle, or
 * -1. `unmap` gives a handle's entry back.
 */
struct pci_msi_remap {
    int (*map)(const pci_device_t *dev, uint32_t address, uint32_t data, uint32_t *raddress,
               uint32_t *rdata);
    void (*unmap)(int handle);
};

/**
 * Send every message enabled through this file, MSI and MSI-X, through
 * `ops`, those enabled from now on too: each enabled message is rewritten
 * in the remapped format, by the one write of its address. 0, or -1 when an
 * entry could not be had, in which case every message is as it was. The
 * remapper takes compatibility-format messages until this returns.
 */
int pci_msi_remap_start(const struct pci_msi_remap *ops);

/** Rewrite every enabled message in the compatibility format, its data
 * first, and give its entry back; messages enabled from now on are not
 * remapped. The remapper must take compatibility-format messages. */
void pci_msi_remap_stop(void);

/**
 * Find the bridge whose secondary bus is `bus`, filling `out`'s place, vendor
 * and device ID and nothing else; true if found.
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

/* A function's resources as Linux numbers them: BARs 0 to 5, then its
 * expansion ROM. */
#define PCI_ROM_RESOURCE 6
#define PCI_RESOURCES    7

/* Linux's IORESOURCE_ flags, which /sys's `resource` file shows. */
#define PCI_IORESOURCE_IO        0x00000100u
#define PCI_IORESOURCE_MEM       0x00000200u
#define PCI_IORESOURCE_PREFETCH  0x00002000u
#define PCI_IORESOURCE_READONLY  0x00004000u
#define PCI_IORESOURCE_SIZEALIGN 0x00040000u
#define PCI_IORESOURCE_MEM_64    0x00100000u

/** One resource of a function: the addresses it decodes, `start` to `end`
 * inclusive, and Linux's flags for it; all 0 for one it lacks. */
struct pci_resource {
    uint64_t start;
    uint64_t end;
    uint32_t flags;
};

/**
 * Resource `i` (0 to PCI_RESOURCES - 1) of the function at bus:device.
 * function into `*r`, as Linux's pci_read_bases decodes it: the size the
 * walk found when it listed the function, and the address and type the
 * BAR holds now, the upper half of a 64-bit BAR taken with it and its own
 * slot left empty. The flags are the BAR's low bits with IORESOURCE_IO or
 * IORESOURCE_MEM, IORESOURCE_PREFETCH and IORESOURCE_MEM_64 as they apply,
 * and IORESOURCE_SIZEALIGN; the ROM's are its enable bit with
 * IORESOURCE_MEM, IORESOURCE_PREFETCH and IORESOURCE_READONLY. A function
 * the walk did not list, and a resource it lacks, give all 0.
 */
void pci_resource(uint8_t bus, uint8_t device, uint8_t function, int i, struct pci_resource *r);

/** Recover a BAR's base address by masking off its type bits. */
uint32_t pci_bar_address(uint32_t bar);

/** True if a BAR is memory-mapped (bit 0 clear), false if I/O space. */
bool pci_bar_is_memory(uint32_t bar);

/** True if a BAR is a 64-bit memory BAR (uses two consecutive BARs). */
bool pci_bar_is_64bit(uint32_t bar);

/**
 * The physical address memory BAR `i` of `dev` decodes, with the BAR after
 * it as the upper half when it is a 64-bit one: 0 for an I/O BAR, an
 * unprogrammed one, or a 64-bit BAR in the last slot. A driver maps it only
 * when it is within `uintptr_t`, which on i686 is 32 bits.
 */
uint64_t pci_bar_address64(const pci_device_t *dev, int i);

/**
 * The size in bytes of memory BAR `i` of `dev`, a 64-bit one with the BAR
 * after it, found as the PCI specification sizes a BAR: all ones written,
 * the bits that stay clear read back, and the value put back, with the
 * device's memory decoding off meanwhile so that no access lands at the
 * partial address. 0 for an I/O BAR, a 64-bit one in the last slot, or one
 * that decodes nothing. Call it before the driver starts the device, as
 * Linux sizes BARs at enumeration, and with no access to the BAR in
 * flight.
 */
uint64_t pci_bar_size64(const pci_device_t *dev, int i);

/*
 * A window onto part of a memory BAR, which `pci_bar_map` makes and
 * `pci_bar_unmap` takes away: the kernel address the part is reached at,
 * and the physical range behind it. A BAR larger than the kernel's MMIO
 * region, 128 MiB, such as the 4 GiB QEMU gives virtio-gpu's host memory
 * high above 4 GiB, is reached a window at a time.
 */
struct pci_window {
    uintptr_t va; /* 0 while no window is mapped */
    uint64_t phys;
    size_t size;
    /* The BAR's size, which the caller sets from `pci_bar_size64` before
     * the first map so that a window past the BAR's end is refused; 0 for
     * a size not known. */
    uint64_t bar_size;
};

/**
 * Map [offset, offset + size) of memory BAR `i` of `dev` into a window of
 * the MMIO region with the cache bits `cache`, PTE_NOCACHE for registers
 * or PTE_WC for a framebuffer, and fill `w`, which maps nothing before; its
 * aligned 2 MiB spans are large pages where the architecture has them
 * (`mmio_map`). Returns the window's kernel address, or 0 for no memory
 * BAR, an empty range, a range past `w->bar_size` when it is set, an
 * address an entry cannot hold, 4 GiB and up on i686, or no free run of
 * the region. Under the big kernel lock.
 */
uintptr_t pci_bar_map(const pci_device_t *dev, int i, uint64_t offset, size_t size, pte_t cache,
                      struct pci_window *w);

/** Take away the window `w` maps, if any, and give its room back to the
 * MMIO region; `w` maps nothing after. */
void pci_bar_unmap(struct pci_window *w);

/*
 * The owner of each function, so that one driver has it at a time: the
 * kernel's drivers, recorded as PCI_OWNER_KERNEL when one first turns on a
 * function's decoding, bus mastering, line or messages through this file,
 * or a driver that claims it by name, as vfio-pci does. A function is
 * named by its bus and devfn, device << 3 | function. Called under the big
 * kernel lock.
 */
#define PCI_OWNER_KERNEL "kernel"

/** Record `owner`, a string that outlives the claim, as the function's: 0,
 * also when `owner` has it already, -EBUSY when another has it, or -ENOSPC
 * when the table of owners is full. */
int pci_claim(uint8_t bus, uint8_t devfn, const char *owner);

/** Give back the function `owner` has; nothing when another has it. A
 * driver that hands its device on, as the GPU's to a VM, calls it with
 * PCI_OWNER_KERNEL. */
void pci_unclaim(uint8_t bus, uint8_t devfn, const char *owner);

/** The function's owner, or NULL for none. */
const char *pci_owner(uint8_t bus, uint8_t devfn);

/*
 * The function's reset, as Linux's pci_reset_function: the methods it can
 * be reset by, in Linux's order, the state of its configuration space that
 * a reset loses, and the reset with that state put back.
 */
#define PCI_RESET_NONE   0
#define PCI_RESET_FLR    1 /* PCI Express's function-level reset */
#define PCI_RESET_AF_FLR 2 /* the Advanced Features capability's, as the IGD's */
#define PCI_RESET_PM     3 /* D3hot and back, unless No_Soft_Reset */

/** The method `dev` is reset by: the first of PCI_RESET_FLR, _AF_FLR and
 * _PM its capabilities offer, or PCI_RESET_NONE. */
int pci_reset_method(const pci_device_t *dev);

/* What a reset loses: the header's sixteen dwords and the PCI Express
 * capability's control registers. */
struct pci_saved_state {
    uint32_t header[16];
    uint16_t pcie[4];   /* device and link control, and their second ones */
    uint8_t pcie_cap;   /* the PCI Express capability's offset, or 0 */
    uint8_t pcie_count; /* the words of `pcie` saved: 0, 2 or 4 */
};

/** Read `dev`'s state into `st`. */
void pci_save_state(const pci_device_t *dev, struct pci_saved_state *st);

/** Write the state `st` back to `dev`. */
void pci_restore_state(const pci_device_t *dev, const struct pci_saved_state *st);

/* How the reset waits, `ms` milliseconds at least: a process sleeps. */
typedef void (*pci_sleep_t)(unsigned int ms);

/**
 * Reset `dev` by its method, with its state saved before and restored
 * after, its decoding, bus mastering and line off meanwhile, waiting
 * through `msleep` for the function to answer again, up to 60 s as Linux
 * does. 0; -ENOTTY for a function with no method; -ETIMEDOUT when it does
 * not answer; -EIO when it answers with another identity. The caller has
 * stopped its use of the function: its messages, its line and its DMA.
 */
int pci_reset_function(const pci_device_t *dev, pci_sleep_t msleep);

#endif /* _PCI_H_ */
