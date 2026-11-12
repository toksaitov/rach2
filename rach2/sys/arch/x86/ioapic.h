#ifndef _IOAPIC_H_
#define _IOAPIC_H_

#include "acpi.h"

#include "../../lib/kint.h"

/*
 * ioapic: the I/O APICs the MADT lists, which take the machine's interrupt
 * lines in place of the 8259 pair.
 *
 * Each ISA line, 0 to 15, arrives on its vector, 32 plus the line, as it
 * did through the 8259, so a driver and /proc/interrupts see the same
 * numbers on either controller. A line goes to the global system interrupt
 * the MADT's overrides give it, with their polarity and trigger; a line a
 * PCI device's interrupt line register gives is level-triggered, since
 * SeaBIOS and OVMF route a device's PIRQ to that ISA line and set the
 * register to it. The _PRT is not read, so a device whose PIRQ reaches only
 * the inputs above 15 is not routed.
 *
 * Every input starts masked. A line is unmasked when a handler is
 * registered on its vector (isr.c), and goes to the processor this kernel
 * booted on.
 */

/*
 * The registers of the I/O APIC of id `id`, by the index the select
 * register takes, reached through this interface so that a host test can
 * model them, as `lapic_io` lets it model the local APIC. Without a call to
 * `ioapic_set_io` the driver maps each I/O APIC and reaches its registers
 * through the select register and the window.
 */
struct ioapic_io {
    uint32_t (*read)(uint8_t id, uint32_t reg);
    void (*write)(uint8_t id, uint32_t reg, uint32_t value);
};

/** Reach every I/O APIC's registers through `ops` in place of the mapped
 * pair, for a host test; NULL reads and writes the pair. */
void ioapic_set_io(const struct ioapic_io *ops);

/**
 * Whether the boot line keeps the I/O APIC off: `noapic`, or any `maxcpus=0`,
 * which Linux 6.12.115 turns into the same disable_ioapic_support
 * (arch/x86/kernel/apic/io_apic.c, lines 191 to 197; kernel/smp.c, lines
 * 975 to 982; arch/x86/kernel/smpboot.c, lines 1005 to 1008). Linux's
 * ioapic_is_disabled then also keeps interrupt remapping and the move to
 * x2APIC mode from starting (apic.c, lines 1898 to 1901), and so does this
 * kernel (lapic.c, iommu/vtd.c).
 */
int ioapic_disabled(void);

/** Whether interrupt remapping may yet start in the x2APIC format, which
 * lets the local APIC stay in x2APIC mode (lapic.c): not on the i686
 * kernel, not while `ioapic_disabled`, and only where the firmware's DMAR
 * table offers remapping without opting out of the format, as the boot
 * line's `intremap=` lets it. */
int ioapic_remappable(void);

/** Map the I/O APICs the MADT lists and mask every input: the count made
 * ready, 0 when none was. */
int ioapic_init(const struct acpi_info *a);

/**
 * Send ISA line `line` to `vector` on the local APIC `dest`, unmasked:
 * level-triggered when `pci`, through the MADT's override for the line. 0,
 * or -1 when no I/O APIC has the line's input or the line has none.
 */
int ioapic_route_line(int line, int pci, uint8_t vector, uint8_t dest);

/** Mask the input ISA line `line` was routed to; nothing for a line not
 * routed. */
void ioapic_mask_line(int line);

/** Mask every routed line, and return the lines that were unmasked, line N
 * at bit N, for `ioapic_unmask_lines`. An edge that comes to a masked input
 * is not kept; a level stays asserted until the line is unmasked. */
uint16_t ioapic_mask_routed(void);

/** Unmask the routed lines whose bits are set in `which`, and raise the
 * vector of each edge-triggered one among them once on this processor, by
 * a self-interrupt, for an edge the masked input dropped. */
void ioapic_unmask_lines(uint16_t which);

/** Whether the input of `line` has an interrupt it has not yet sent; 0 for
 * a line not routed. */
int ioapic_line_pending(int line);

/*
 * An interrupt remapper's way to the redirection entries. `map` takes an
 * entry of the remapper's table for the compatibility-format redirection
 * entry (low, high) of the I/O APIC of id `id`, and gives the high half
 * that names it in the remapped format, the low half staying: the entry's
 * handle, or -1. `unmap` gives a handle's entry back.
 */
struct ioapic_remap {
    int (*map)(uint8_t id, uint32_t low, uint32_t high, uint32_t *rhigh);
    void (*unmap)(int handle);
};

/**
 * Send every line routed, and every line routed from now on, through `ops`:
 * each routed entry is rewritten in the remapped format by the one write of
 * its high half. 0, or -1 when an entry could not be had, in which case
 * every line is as it was. The remapper takes compatibility-format
 * interrupts until this returns.
 */
int ioapic_remap_start(const struct ioapic_remap *ops);

/** Rewrite every routed entry in the compatibility format and give its entry
 * back; lines routed from now on are not remapped. */
void ioapic_remap_stop(void);

#endif /* _IOAPIC_H_ */
