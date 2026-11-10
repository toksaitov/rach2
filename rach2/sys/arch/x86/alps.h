#ifndef _ALPS_H_
#define _ALPS_H_

/*
 * The ALPS touchpad on the 8042's auxiliary port, of protocol versions 1 to
 * 8, as Linux 6.12's psmouse and alps drivers drive it: found and set up
 * while the PS/2 mouse is brought up (ps2mouse.c), it reports its packets
 * through an event device of Linux's name and capabilities, a DualPoint's
 * stick through a second, and a mouse on its external port through a third,
 * and moves the mouse device's pointer as Linux's mousedev does.
 */

#include "../../lib/kint.h"

/**
 * Probe the auxiliary device for an ALPS touchpad and set one up, as
 * Linux's `psmouse_extensions` tries the ALPS protocol: the reset to
 * defaults, `alps_detect` with its E6, E7 and EC reports and the model
 * table, the probes of the version's set-up, `alps_init` with the version's
 * absolute mode, and psmouse's rate, resolution and scaling. Called from the
 * mouse's bring-up when no Synaptics touchpad answered, with interrupts off,
 * before reporting is enabled.
 *
 * @return 1 when the touchpad speaks its absolute protocol and its event
 *         devices are registered, after which `alps_byte` takes its bytes;
 *         0 when no ALPS touchpad answered, or the reports matched no
 *         version Linux drives; -1 when one was identified but its set-up
 *         failed, after it has been reset, so that the caller sets it up as
 *         a PS/2 mouse named by `alps_mouse_name`.
 */
int alps_init(void);

/**
 * The name Linux gives a mouse whose ALPS set-up failed: `proto`, the name
 * of the protocol it speaks, such as "ImPS/2", then "ALPS GlidePoint", or
 * "ALPS DualPoint TouchPad" for a pad with a stick. The text is kept in a
 * buffer of this file's, which the next call overwrites.
 */
const char *alps_mouse_name(const char *proto);

/**
 * Take the next byte from the touchpad, in interrupt context, as psmouse's
 * `psmouse_receive_byte` and `psmouse_handle_byte` frame it for Linux's
 * `alps_process_byte`: a byte more than half a second after the one before
 * starts the packet again, a byte that breaks the packet's form drops it,
 * and a whole packet is reported.
 */
void alps_byte(uint8_t data);

/**
 * Report the last six-byte packet of a pad that interleaves a stick's PS/2
 * packets with its own, once 20 ms have passed with no byte after it, as
 * Linux's `alps_flush_packet` timer does. Called from the platform's tick,
 * on the processor that takes IRQ 12, with interrupts off.
 */
void alps_tick(void);

#endif /* _ALPS_H_ */
