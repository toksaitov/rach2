#ifndef _PIC_H_
#define _PIC_H_

#include "../../kio.h"

/** Remaps the 8259 PIC IRQs to vectors 0x20 (master) and 0x28 (slave). */
void init_pic(void);

/**
 * Unmask master IRQ line `irq` (0..7) once its handler is installed.
 *
 * Nothing after `pic_retire`: the I/O APIC then unmasks a line when its
 * handler is registered, and the 8259 stays masked.
 */
void pic_unmask_irq(uint8_t irq);

/**
 * Mask all sixteen lines for good, when the I/O APIC takes them. The pair
 * stays initialized at vectors 32 to 47, so a line it latched while masked
 * is still in its request register.
 */
void pic_retire(void);

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
 * here clears, and an unmasked one would be taken forever. Once the I/O
 * APIC has the lines, the line is held masked there instead.
 */
void pic_mask_irq(uint8_t irq);

/** Sends End Of Interrupt to the master PIC. */
void reset_master_pic(void);

/** Sends End Of Interrupt to the slave PIC. */
void reset_slave_pic(void);

#endif
