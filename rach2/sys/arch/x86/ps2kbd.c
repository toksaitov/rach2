/*
 * ps2kbd.c: the PS/2 keyboard's interrupt, which reads the 8042's data port
 * and hands the code on. Everything that is not about the 8042 lives in the
 * input layer, because a USB keyboard's codes arrive by another road and
 * are treated the same.
 */

#include "isr.h"
#include "pic.h"
#include "portio.h"
#include "ps2kbd.h"

#include "../../input.h"
#include "../../mm/paging.h"

/* The 8042's data port, where a byte from the keyboard waits. */
#define PS2_DATA_PORT 0x60

/**
 * Handle the PS/2 keyboard interrupt: read the port and hand on the code.
 *
 * Runs in interrupt context with an arbitrary process's page directory
 * loaded, so it translates through the kernel's own directory around the
 * work, which may touch kernel data the interrupted one does not map.
 */
static void kbd_irq(isr_params_t *params)
{
    uint8_t scancode;
    uintptr_t saved;

    (void)params;

    scancode = inb(PS2_DATA_PORT);
    saved = paging_enter_kernel();
    input_key(scancode);
    paging_leave_kernel(saved);
}

void kbd_init(void)
{
    set_isr_handler(33, &kbd_irq, "kbd"); /* install the IRQ 1 keyboard handler */
    pic_unmask_irq(1); /* now safe to take keyboard interrupts (masked since init_pic) */
}
