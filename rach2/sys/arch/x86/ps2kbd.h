#ifndef _PS2KBD_H_
#define _PS2KBD_H_

/*
 * The PS/2 keyboard, on the 8042's first port: IRQ 1, one scancode of set 1
 * a byte, which it gives to the input layer (../../input.h), its LEDs,
 * which follow the input layer's `input_leds`, and the reconnect after its
 * self-test code, which keeps its event device, or unbinds it and probes
 * serio0 anew as serio0's `bind_mode` allows.
 */

/**
 * Register the keyboard's event device, take its interrupt, vector 33, and
 * unmask IRQ 1, which `init_pic` left masked. The controller's configuration
 * byte is read first, with interrupts off, for its translation, which on a
 * portable chassis has every probe of the keyboard skip GETID; the bytes
 * waiting before and after that read are drained by it, before IRQ 1 is
 * unmasked. When the read fails, the controller's setup has failed, as
 * Linux's i8042_controller_init fails it: `kbd_stop` binds nothing and
 * leaves IRQ 1 and IRQ 12 masked. With no event device free serio0 is left
 * without a driver. Called once, at boot, from `platform_keyboard_init`,
 * after the firmware's tables are found.
 */
void kbd_init(void);

/**
 * Give up the 8042's ports after its setup failed (`i8042_failed`): mask
 * IRQ 1 and IRQ 12, unbind atkbd from serio0, its event device gone when
 * `kbd_init` bound one, and stop the LED command, the probe and the
 * handler, so that nothing more is written to the controller, as Linux's
 * i8042_probe, failing, registers no port. Called from `kbd_init`, and from
 * `init_mouse`, with interrupts off, when the mouse's setup failed the
 * controller.
 */
void kbd_stop(void);

/**
 * Move the LED command one step, from the tick (`platform_tick`): start
 * one, 50 ms after the last ended, when `input_leds` differs from what the
 * keyboard shows; write a byte the controller could not take before; and
 * write a byte again when its answer has not come in 100 ms. A byte written
 * three times without an ACK, or one the controller has refused for 500 ms,
 * ends the command, and the next starts only when the LEDs wanted change. The
 * keyboard's answers are taken from its interrupt (`input_ps2_reply`), which
 * writes the LED byte after the command's ACK. Writes only when the
 * controller's input buffer is empty, so it never waits, and a byte refused
 * counts no write. While a reconnect or a probe of serio0 runs, it moves that
 * on in place of the LED command; a reconnect a BAT left due starts when the
 * LED command under way ends, and none starts meanwhile; with no driver bound
 * it does nothing. Nothing before `kbd_init`.
 */
void ps2kbd_tick(void);

#endif /* _PS2KBD_H_ */
