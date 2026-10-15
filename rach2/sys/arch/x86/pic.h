#ifndef _PIC_H_
#define _PIC_H_

#include "../../kio.h"

/*
 * pic: the PC's two 8259A interrupt controllers, which deliver lines 0 to
 * 15 at vectors 32 to 47 until the I/O APIC takes the lines.
 */

/**
 * Initialize the pair with the master's lines at vectors 0x20 to 0x27 and
 * the slave's at 0x28 to 0x2F, every line unmasked but the keyboard's, IRQ
 * 1, which `kbd_init` unmasks once its handler is installed. Called once,
 * at boot, from `init_idt`, with interrupts off.
 */
void init_pic(void);

/**
 * Unmask master IRQ line `irq` (0..7) once its handler is installed; a
 * larger `irq` is ignored.
 *
 * The 8259 latches a masked line's request, so a request that came while
 * the line was masked is delivered once it is unmasked. Nothing after
 * `pic_retire`: the I/O APIC then unmasks a line when its handler is
 * registered, and the 8259 stays masked.
 */
void pic_unmask_irq(uint8_t irq);

/**
 * Mask all sixteen lines for good, when the I/O APIC takes them. The pair
 * stays initialized at vectors 32 to 47, so a line it latched while masked
 * is still in its request register.
 */
void pic_retire(void);

/**
 * Whether IRQ line `irq` (0..15) is raised and not yet dispatched; 0 for a
 * larger `irq`.
 *
 * Read from the 8259's request register, as a message's arrival is read
 * from the APIC's: a line raised while interrupts were off is waiting here,
 * and a pump that drained its work is not looking at a lost edge.
 */
int pic_irq_pending(uint8_t irq);

/**
 * Mask IRQ line `irq` (0..15), master or slave; a larger `irq` is ignored.
 *
 * For a line this kernel has no handler for and that a device may assert
 * anyway, such as the SCI: with ACPI enabled, a power button or a thermal
 * event raises it, level-triggered, nothing here clears it, and an unmasked
 * one would be taken forever. Once the I/O APIC has the lines, the line is
 * held masked there instead (`isr_line_hold`).
 */
void pic_mask_irq(uint8_t irq);

/** Sends End Of Interrupt to the master PIC. */
void reset_master_pic(void);

/** Sends End Of Interrupt to the slave PIC. */
void reset_slave_pic(void);

#endif
