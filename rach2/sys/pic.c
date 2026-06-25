#include "pic.h"

/**
 * Remap the two 8259A PICs and run their ICW1..ICW4 init sequence.
 *
 * Each PIC is initialized by writing ICW1 to its command port (0x20 master,
 * 0xA0 slave) and then ICW2..ICW4 to its data port (0x21 master, 0xA1 slave).
 * ICW1 = 0x11 starts the sequence in cascade mode and announces that ICW4 will
 * follow. ICW2 sets the vector base: the master's eight IRQs map to 0x20..0x27
 * and the slave's to 0x28..0x2F, moving the hardware interrupts off the CPU's
 * reserved exception vectors (0..31). ICW3 wires the cascade (master 0x04 has
 * IRQ2 driving the slave; slave 0x02 is its cascade identity). ICW4 = 0x01
 * selects 8086/88 mode. The two trailing writes of 0x00 to each data port set
 * the interrupt mask to zero, unmasking every IRQ line.
 */
void init_pic(void)
{
    /* ICW1: begin init, cascade mode, ICW4 to follow. */
    outb(0x20, 0x11);
    outb(0xA0, 0x11);
    /* ICW2: master IRQs at 0x20..0x27, slave at 0x28..0x2F. */
    outb(0x21, 0x20);
    outb(0xA1, 0x28);
    outb(0x21, 0x04);
    outb(0xA1, 0x02);
    outb(0x21, 0x01);
    outb(0xA1, 0x01);
    outb(0x21, 0x0);
    outb(0xA1, 0x0);
}

void reset_master_pic(void)
{
    outb(0x20, 0x20);
}

void reset_slave_pic(void)
{
    outb(0xA0, 0x20);
}
