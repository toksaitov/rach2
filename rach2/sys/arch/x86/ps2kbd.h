#ifndef _PS2KBD_H_
#define _PS2KBD_H_

/*
 * The PS/2 keyboard, on the 8042's first port: IRQ 1, one scancode of set 1
 * a byte, which it gives to the input layer (../../input.h), its LEDs,
 * which follow the input layer's `input_leds`, and the reconnect after its
 * self-test code, which keeps its event device, or unbinds it and probes
 * serio0 anew as serio0's `bind_mode` allows.
 */

/** Register the keyboard's event device, take its interrupt, vector 33, and
 * unmask IRQ 1, which `init_pic` left masked. Called once, at boot, from
 * `platform_keyboard_init`. */
void kbd_init(void);

/**
 * Move the LED command one step, from the tick (`platform_tick`): start
 * one, 50 ms after the last ended, when `input_leds` differs from what the
 * keyboard shows; write a byte the controller could not take before; and
 * write a byte again when its answer has not come in 100 ms. A byte written
 * three times without an ACK ends the command, and the next starts only when
 * the LEDs wanted change. The keyboard's answers are taken from its interrupt
 * (`input_ps2_reply`), which writes the LED byte after the command's ACK.
 * Writes only when the controller's input buffer is empty, so it never
 * waits. While a reconnect or a probe of serio0 runs, it moves that on in
 * place of the LED command, and with no driver bound it does nothing.
 * Nothing before `kbd_init`.
 */
void ps2kbd_tick(void);

#endif /* _PS2KBD_H_ */
