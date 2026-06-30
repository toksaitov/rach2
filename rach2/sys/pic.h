#ifndef _PIC_H_
#define _PIC_H_

#include "kio.h"

/** Remaps the 8259 PIC IRQs to vectors 0x20 (master) and 0x28 (slave). */
void init_pic(void);

/** Unmask master IRQ line `irq` (0..7) once its handler is installed. */
void pic_unmask_irq(uint8_t irq);

/** Sends End Of Interrupt to the master PIC. */
void reset_master_pic(void);

/** Sends End Of Interrupt to the slave PIC. */
void reset_slave_pic(void);

#endif
