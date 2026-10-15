/*
 * ps2kbd.c: the PS/2 keyboard's interrupt, which reads the 8042's data port
 * and hands the code on. Everything that is not about the 8042 is in the
 * input layer, which takes a USB keyboard's codes too and treats them the
 * same.
 */

#include "isr.h"
#include "pic.h"
#include "portio.h"
#include "ps2kbd.h"

#include "../../input.h"
#include "../../mm/paging.h"

/* The 8042's data port, where a byte from the keyboard waits, and its status
 * port, whose bit 0 says one does. */
#define PS2_DATA_PORT          0x60
#define PS2_STATUS_PORT        0x64
#define PS2_STATUS_OUTPUT_FULL 0x01

/**
 * Handle the PS/2 keyboard interrupt: read the port and hand on the code,
 * when the controller has one waiting. A second interrupt for one byte,
 * which the vector raised for a byte waiting at the switch to the I/O APIC
 * and the line's own message can make (`ps2_waiting` in platform.c), finds
 * the output buffer empty and reads nothing: the data port would give the
 * last byte again. A waiting byte is taken whether the status marks it the
 * keyboard's or the mouse's.
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

    if (!(inb(PS2_STATUS_PORT) & PS2_STATUS_OUTPUT_FULL)) {
        return;
    }
    scancode = inb(PS2_DATA_PORT);
    saved = paging_enter_kernel();
    input_key(scancode);
    paging_leave_kernel(saved);
}

void kbd_init(void)
{
    set_isr_handler(33, &kbd_irq, "kbd");
    pic_unmask_irq(1);
}
