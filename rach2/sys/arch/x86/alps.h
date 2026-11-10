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

/* What `alps_byte` made of a byte, Linux's `psmouse_ret_t`. */
#define ALPS_BAD  -1 /* it broke the packet's form */
#define ALPS_GOOD 0  /* the packet goes on */
#define ALPS_FULL 1  /* it ended a packet */

/**
 * Take the next byte from the touchpad, in interrupt context, as psmouse's
 * `psmouse_receive_byte` and `psmouse_handle_byte` frame it for Linux's
 * `alps_process_byte`: a byte more than half a second after the one before
 * starts the packet again, a byte that breaks the packet's form drops it,
 * and a whole packet is reported. Returns ALPS_*, which ps2mouse.c counts
 * toward the reconnect after `alps_reset_after` bad bytes in a row.
 */
int alps_byte(uint8_t data);

/** Whether the next byte begins a packet, which tells ps2mouse.c whether a
 * byte 0xAA may begin a device's announcement. In interrupt context. */
int alps_packet_start(void);

/** Drop the packet under way and the count of bad bytes, as Linux's
 * `__psmouse_set_state` does at each change of the port's state: when a
 * run of commands begins and ends. In interrupt context or with
 * interrupts off. */
void alps_packet_restart(void);

/** The bad bytes in a row after which ps2mouse.c reconnects the pad,
 * Linux's `resetafter` for ALPS: twice the packet's size, 12 or 16, so that
 * two broken packets pass. */
int alps_reset_after(void);

/**
 * Set the pad up again after its run of bad bytes or its announcement, as
 * Linux's `alps_reconnect` and psmouse's `psmouse_initialize` do: the
 * reset, the identification, which must again find a pad Linux drives, the
 * version's set-up, then the rate, resolution and scaling. Its event
 * devices stay. Returns 0, or -1 when no such pad answers or it refuses its
 * set-up, and the caller probes the port anew. Inside a run of
 * `ps2mouse_cmd_begin`.
 */
int alps_reconnect(void);

/** Reset the pad and remove its event devices, as Linux's
 * `alps_disconnect` does, before the port is probed anew. Process
 * context. */
void alps_disconnect(void);

/**
 * Poll the pad for a packet, as Linux's `alps_poll` does for psmouse's
 * resync: the stick's port left for the pad's on a version 2 DualPoint, the
 * packet of the pad's size, which must begin with the protocol's first
 * byte, and when `badbyte`, the first byte of the packet the resync
 * dropped, was the stick's, the stick's three bytes in its place. Returns
 * the packet's size with its bytes in `buf`, of 8, or -1. Inside a run of
 * `ps2mouse_cmd_begin`.
 */
int alps_poll(uint8_t badbyte, uint8_t *buf);

/**
 * Report the last six-byte packet of a pad that interleaves a stick's PS/2
 * packets with its own, once 20 ms have passed with no byte after it, as
 * Linux's `alps_flush_packet` timer does. Called from the platform's tick,
 * on the processor that takes IRQ 12, with interrupts off.
 */
void alps_tick(void);

#endif /* _ALPS_H_ */
