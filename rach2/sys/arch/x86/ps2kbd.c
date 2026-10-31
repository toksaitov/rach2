/*
 * ps2kbd.c: the PS/2 keyboard's interrupt, which reads the 8042's data port
 * and hands the code on, and the command that lights the keyboard's LEDs.
 * Everything that is not about the 8042 is in the input layer, which takes a
 * USB keyboard's codes too and treats them the same.
 */

#include "i8042.h"
#include "isr.h"
#include "pic.h"
#include "portio.h"
#include "ps2kbd.h"

#include "../../evdev.h"
#include "../../input.h"
#include "../../klog.h"
#include "../../mm/paging.h"
#include "../../timer.h"

/* The keyboard's command that sets its LEDs, followed by a byte of them:
 * bit 0 Scroll Lock, bit 1 Num Lock, bit 2 Caps Lock, as kd.h's LED_*. The
 * keyboard answers each byte with ACK, or with RESEND when it did not take
 * it. */
#define KBD_CMD_SET_LEDS 0xED
#define KBD_ACK          0xFA
#define KBD_RESEND       0xFE

/* The time from the end of one LED command to the start of the next, and
 * how long a byte's answer may take, in ticks of 10 ms; how many times a
 * byte is written before the command is given up. */
#define LEDS_GAP_TICKS  5
#define LEDS_WAIT_TICKS 10
#define LEDS_TRIES      3

/* Where the LED command is: none in progress, the command byte due or out,
 * the LED byte due or out. */
#define LEDS_IDLE    0
#define LEDS_COMMAND 1
#define LEDS_VALUE   2

/* The keyboard's event device, named as Linux names a translated one. */
static struct input_set1 ps2_keyboard;

/*
 * The LED command's state. The tick (`ps2kbd_tick`) and the interrupt
 * (`input_ps2_reply`) both change it, each on the boot processor with
 * interrupts off: the tick is that processor's, and line 1 is routed to the
 * processor that took its handler at boot.
 */
static struct {
    int ready;          /* `kbd_init` has run */
    int stage;          /* LEDS_IDLE, LEDS_COMMAND or LEDS_VALUE */
    int out;            /* the stage's byte is written and its answer awaited */
    int tries;          /* the stage's byte has been written this many times */
    int shown;          /* the LEDs the keyboard shows, -1 before the first command */
    unsigned int value; /* the LEDs being sent */
    uint32_t at;        /* `kernel_ticks` at the last write, or at the end */
    int said;           /* a command given up has been logged */
} leds;

/**
 * Write the byte of the LED command's stage, counting the try. A byte the
 * controller cannot take now is not out, and the tick writes it again.
 */
static void leds_write(uint32_t now)
{
    uint8_t byte = leds.stage == LEDS_COMMAND ? KBD_CMD_SET_LEDS : (uint8_t)(leds.value & 0x07u);

    leds.tries++;
    if (ps2_kbd_write(byte) == 0) {
        leds.out = 1;
        leds.at = now;
    }
}

/** End the LED command, the keyboard showing `value` or taken to show it. */
static void leds_end(uint32_t now)
{
    leds.shown = (int)leds.value;
    leds.stage = LEDS_IDLE;
    leds.out = 0;
    leds.at = now;
}

int input_ps2_reply(uint8_t code)
{
    if (leds.stage == LEDS_IDLE || !leds.out || (code != KBD_ACK && code != KBD_RESEND)) {
        return 0;
    }
    leds.out = 0;
    if (code == KBD_RESEND) {
        if (leds.tries < LEDS_TRIES) {
            leds_write(kernel_ticks);
        }
    } else if (leds.stage == LEDS_COMMAND) {
        leds.stage = LEDS_VALUE;
        leds.tries = 0;
        leds_write(kernel_ticks);
    } else {
        leds_end(kernel_ticks);
    }
    return 1;
}

void ps2kbd_tick(void)
{
    uint32_t now = kernel_ticks;

    if (!leds.ready) {
        return;
    }
    if (leds.stage == LEDS_IDLE) {
        unsigned int wanted = input_leds();

        if ((int)wanted == leds.shown || now - leds.at < LEDS_GAP_TICKS) {
            return;
        }
        leds.stage = LEDS_COMMAND;
        leds.value = wanted;
        leds.tries = 0;
        leds.out = 0;
    }
    if (leds.out && now - leds.at < LEDS_WAIT_TICKS) {
        return;
    }
    if (leds.tries >= LEDS_TRIES) {
        if (!leds.said) {
            leds.said = 1;
            klog(KLOG_WARN, "kbd", "leds: the keyboard did not take %x", leds.value);
        }
        leds_end(now);
        return;
    }
    leds.out = 0;
    leds_write(now);
}

/**
 * Handle the PS/2 keyboard interrupt: read the port and hand on the code,
 * when the controller has one of the keyboard's waiting. A second interrupt
 * for one byte, which the vector raised for a byte waiting at the switch to
 * the I/O APIC and the line's own message can make (`ps2_waiting` in
 * platform.c), finds the output buffer empty and reads nothing: the data
 * port would give the last byte again. A byte the status marks as the
 * mouse's is left unread for the mouse's IRQ 12 handler: read here, it would
 * be taken for a key and lost from the mouse's packet. The answer to an LED
 * command goes to that command (`input_ps2_reply`); any other code goes to
 * the keyboard's event device and then to the console.
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
    if (!input_ps2_reply(scancode)) {
        input_set1_event(&ps2_keyboard, scancode);
        input_key(scancode);
    }
    paging_leave_kernel(saved);
}

void kbd_init(void)
{
    ps2_keyboard.dev = input_keyboard_register(
        "AT Translated Set 2 keyboard", "isa0060/serio0/input0", EVDEV_BUS_I8042, 0x0001, 0x0001);
    set_isr_handler(33, &kbd_irq, "kbd");
    pic_unmask_irq(1);
    leds.shown = -1;
    leds.ready = 1;
}
