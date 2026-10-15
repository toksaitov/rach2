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
 * port, whose bit 0 says one does and whose bit 5 says it is the mouse's. */
#define PS2_DATA_PORT          0x60
#define PS2_STATUS_PORT        0x64
#define PS2_STATUS_OUTPUT_FULL 0x01
#define PS2_STATUS_MOUSE_DATA  0x20

/**
 * Handle the PS/2 keyboard interrupt: read the port and hand on the code,
 * when the controller has one of the keyboard's waiting. A second interrupt
 * for one byte, which the vector raised for a byte waiting at the switch to
 * the I/O APIC and the line's own message can make (`ps2_waiting` in
 * platform.c), finds the output buffer empty and reads nothing: the data
 * port would give the last byte again. A byte the status marks as the
 * mouse's is left unread for the mouse's IRQ 12 handler: read here, it would
 * be taken for a key and lost from the mouse's packet.
 *
 * Runs in interrupt context with an arbitrary process's page directory
 * loaded, so it translates through the kernel's own directory around the
 * work, which may touch kernel data the interrupted one does not map.
 */
static void kbd_irq(isr_params_t *params)
{
    uint8_t scancode;
    uintptr_t saved;
    uint8_t status;

    (void)params;
    status = inb(PS2_STATUS_PORT);
    if (!(status & PS2_STATUS_OUTPUT_FULL) || (status & PS2_STATUS_MOUSE_DATA)) {
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
