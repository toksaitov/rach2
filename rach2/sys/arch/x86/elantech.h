#ifndef _ELANTECH_H_
#define _ELANTECH_H_

/*
 * The Elantech touchpad on the 8042's auxiliary port, in the absolute mode
 * Linux's psmouse sets: found and set up while the PS/2 mouse is brought up
 * (ps2mouse.c), it reports the packets of hardware versions 1 to 4 through
 * an event device of Linux's name and capabilities, with the slots Linux
 * gives each version, and a pointing stick's packets through a second
 * device, and moves the mouse device's pointer as Linux's mousedev does.
 */

#include "../../lib/kint.h"

/**
 * Probe the auxiliary device for an Elantech touchpad and set one up, as
 * Linux's `elantech_detect` and `elantech_init` do: the magic knock and the
 * firmware version that tells a pad from a mouse that answers the knock;
 * the reset; the queries of the capabilities, the samples, the IC body, the
 * resolution and the range, which give the hardware version; the register
 * writes of absolute mode for that version; then its event devices. Called
 * from the mouse's bring-up after the Synaptics probe found no pad, with
 * interrupts off, before reporting is enabled.
 *
 * @return 1 when the touchpad speaks the absolute protocol and its event
 *         device is registered; 0 when no Elantech touchpad answered; -1
 *         when one answered the knock but a query or the absolute mode
 *         failed, after it has been reset, so that the caller sets it up as
 *         a PS/2 mouse whose name says it is an Elantech Touchpad.
 */
int elantech_init(void);

/* What `elantech_byte` made of a byte, Linux's `psmouse_ret_t`. */
#define ELANTECH_BAD  -1 /* it ended a packet out of form */
#define ELANTECH_GOOD 0  /* the packet goes on */
#define ELANTECH_FULL 1  /* it ended a packet */

/**
 * Take the next byte of the touchpad's packet, four bytes on version 1 and
 * six on the others, in interrupt context, as Linux's
 * `elantech_process_byte` with psmouse's framing: a whole packet whose
 * constant bits or parity are wrong is dropped, a byte that comes more than
 * half a second after the one before starts the packet again, and a whole
 * packet is reported. Returns ELANTECH_*, which ps2mouse.c counts toward
 * the reconnect after five bad packets in a row.
 */
int elantech_byte(uint8_t data);

/** Whether the next byte begins a packet, which tells ps2mouse.c whether a
 * byte 0xAA may begin a device's announcement. In interrupt context. */
int elantech_packet_start(void);

/** Drop the packet under way, as Linux's `__psmouse_set_state` does at
 * each change of the port's state: when a run of commands begins and ends.
 * In interrupt context or with interrupts off. */
void elantech_packet_restart(void);

/**
 * Set the pad up again after five bad packets or its announcement, as
 * Linux's `elantech_reconnect` and psmouse's `psmouse_initialize` do: the
 * reset, the knock and firmware version, which must again be an Elantech
 * pad's, the Lenovo N24's disable and enable, absolute mode, then the rate,
 * resolution and scaling. Its event devices stay. Returns 0, or -1 when the
 * pad is gone or refuses absolute mode, and the caller probes the port
 * anew. Inside a run of `ps2mouse_cmd_begin`.
 */
int elantech_reconnect(void);

/** Remove the pad's event devices, as Linux's `elantech_disconnect` does,
 * before the port is probed anew. Process context. */
void elantech_disconnect(void);

#endif /* _ELANTECH_H_ */
