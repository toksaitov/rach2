#include "pit.h"

/**
 * Program the 8254 PIT channel 0 to raise IRQ0 at `frequency` Hz.
 *
 * Channel 0 is the system timer line (IRQ0, vector 32); the divisor scales the
 * fixed 1.193182 MHz input clock down to the requested rate. The caller
 * installs the IRQ0 handler.
 */
void init_pit(uint32_t frequency)
{
    uint32_t divisor = 1193182 / frequency;

    outb(0x43, 0x36);
    outb(0x40, (uint8_t) (divisor & 0xFF));
    outb(0x40, (uint8_t) ((divisor >> 8) & 0xFF));
}
