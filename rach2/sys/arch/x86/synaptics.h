#ifndef _SYNAPTICS_H_
#define _SYNAPTICS_H_

/*
 * The Synaptics touchpad on the 8042's auxiliary port, in the absolute mode
 * with W mode that Linux's psmouse sets: found and set up while the PS/2
 * mouse is brought up (ps2mouse.c), it reports six-byte packets of the
 * fingers' places, pressure and width through an event device of Linux's
 * name and capabilities, with slots for the contacts of an image sensor,
 * and moves the mouse device's pointer as Linux's mousedev does.
 */

#include "../../lib/kint.h"

/**
 * Probe the auxiliary device for a Synaptics touchpad and set one up, as
 * Linux's `synaptics_detect` and `synaptics_init` do: the identify knock;
 * the reset, the queries of its identity, model, firmware, board,
 * capabilities, resolution and coordinates; the mode byte of absolute mode
 * with W mode, 80 packets a second and gestures off, and the advanced
 * gesture mode of a pad that has it; then its event device. Called from the
 * mouse's bring-up after the reset and the defaults, with interrupts off,
 * before reporting is enabled.
 *
 * @return 1 when the touchpad speaks the absolute protocol and its event
 *         device is registered; 0 when no Synaptics touchpad answered; -1
 *         when one answered the knock but a query or the mode failed, after
 *         it has been set back to relative mode, so that the caller sets it
 *         up as a PS/2 mouse whose name says it is a Synaptics TouchPad.
 */
int synaptics_init(void);

/**
 * Take the next byte of the touchpad's six-byte packet, in interrupt
 * context, as Linux's `synaptics_process_byte` with psmouse's framing: a
 * byte that breaks the packet's form or one that comes more than half a
 * second after the one before starts the packet again, and a whole packet
 * is reported.
 */
void synaptics_byte(uint8_t data);

#endif /* _SYNAPTICS_H_ */
