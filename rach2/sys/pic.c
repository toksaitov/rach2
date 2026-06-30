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
 * selects 8086/88 mode. The trailing OCW1 mask writes leave every line unmasked
 * except IRQ1: the PS/2 keyboard is the one always-live device whose interrupt
 * can arrive before its handler is installed (the PIT's early IRQ0 is a harmless
 * lost tick, and every other device stays silent until its driver enables it).
 * An early IRQ1 would be acknowledged without reading port 0x60, stranding the
 * controller's output buffer; it stays masked until `pic_unmask_irq(1)` runs once
 * the keyboard handler is in place.
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
    outb(0x21, 0x02); /* OCW1: mask IRQ1 (keyboard) until its handler is ready */
    outb(0xA1, 0x0);
}

/**
 * Unmask one master PIC IRQ line, called once its handler is installed.
 *
 * The 8259 latches a masked line's request, so a keystroke that arrived while the
 * line was masked is delivered as soon as it is unmasked. `irq` is a master line
 * (0..7); a larger value is ignored.
 */
void pic_unmask_irq(uint8_t irq)
{
    uint8_t mask;

    if (irq > 7) {
        return;
    }
    mask = inb(0x21);
    mask = (uint8_t)(mask & ~(1u << irq));
    outb(0x21, mask);
}

void reset_master_pic(void)
{
    outb(0x20, 0x20);
}

void reset_slave_pic(void)
{
    outb(0xA0, 0x20);
}
