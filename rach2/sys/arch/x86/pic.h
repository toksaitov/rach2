#ifndef _PIC_H_
#define _PIC_H_

#include "../../kio.h"

/** Remaps the 8259 PIC IRQs to vectors 0x20 (master) and 0x28 (slave). */
void init_pic(void);

/** Unmask master IRQ line `irq` (0..7) once its handler is installed. */
void pic_unmask_irq(uint8_t irq);

/**
 * Whether IRQ line `irq` (0..15) is raised and not yet dispatched.
 *
 * Read from the 8259's request register, which is what a message's arrival
 * is read from at the APIC: a line raised while the interrupt flag was held
 * down is waiting here, and a pump that drained its work is not looking at
 * a lost edge.
 */
int pic_irq_pending(uint8_t irq);

/**
 * Mask IRQ line `irq` (0..15), master or slave.
 *
 * For a line this kernel has no handler for and that a device may assert
 * anyway. The SCI is the case it was written for: with ACPI enabled, a power
 * button or a thermal event raises a level-triggered interrupt that nothing
 * here clears, and an unmasked one would be taken forever.
 */
void pic_mask_irq(uint8_t irq);

/** Sends End Of Interrupt to the master PIC. */
void reset_master_pic(void);

/** Sends End Of Interrupt to the slave PIC. */
void reset_slave_pic(void);

#endif
