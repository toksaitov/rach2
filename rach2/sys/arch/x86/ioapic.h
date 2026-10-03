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
 * register to it. Without an AML interpreter the _PRT is not read, so a
 * device whose PIRQ reaches only the inputs above 15 is not routed.
 *
 * Every input starts masked. A line is unmasked when a handler is
 * registered on its vector (isr.c), and goes to the processor this kernel
 * booted on.
 */

/** Map the I/O APICs the MADT lists and mask every input: the count made
 * ready, 0 when none was. */
int ioapic_init(const struct acpi_info *a);

/**
 * Send ISA line `line` to `vector` on the local APIC `dest`, unmasked:
 * level-triggered when `pci`, through the MADT's override for the line. 0,
 * or -1 when no I/O APIC has the line's input or the line has none.
 */
int ioapic_route_line(int line, int pci, uint8_t vector, uint8_t dest);

/** Mask the input line `line` was routed to. */
void ioapic_mask_line(int line);

/** Whether the input of `line` has an interrupt it has not yet sent. */
int ioapic_line_pending(int line);

#endif /* _IOAPIC_H_ */
