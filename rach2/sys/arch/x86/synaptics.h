#ifndef _SYNAPTICS_H_
#define _SYNAPTICS_H_

/*
 * The Synaptics touchpad on the 8042's auxiliary port, in the absolute mode
 * with W mode that Linux's psmouse sets: found and set up while the PS/2
 * mouse is brought up (ps2mouse.c), it reports six-byte packets of the
 * fingers' places, pressure and width through an event device of Linux's
 * name and capabilities, with slots for the contacts of an image sensor,
 * and moves the mouse device's pointer as Linux's mousedev does. The quirks
 * Linux chooses by the PS/2 mouse's PNP ID apply, and a pad with a
 * pass-through port carries the packets of the device behind it, which
 * ps2mouse.c reports.
 */

#include "../../lib/kint.h"

struct ps2io;

/**
 * Probe the auxiliary device for a Synaptics touchpad and set one up, as
 * Linux's `synaptics_detect` and `synaptics_init` do: the identify knock;
 * the reset, the queries of its identity, model, firmware, board,
 * capabilities, resolution and coordinates; Linux's quirks by the PS/2
 * mouse's PNP ID (i8042.h): the coordinates of the pads Linux's
 * `min_max_pnpid_table` names, a ForcePad's click and the top buttons'
 * property; the mode byte of absolute mode with W mode, 80 packets a second
 * and gestures off, and the advanced gesture mode of a pad that has it;
 * then its event device. Called from the mouse's bring-up after the reset
 * and the defaults, with interrupts off, before reporting is enabled.
 *
 * @return 1 when the touchpad speaks the absolute protocol and its event
 *         device is registered; 0 when no Synaptics touchpad answered; -1
 *         when one answered the knock but a query or the mode failed, after
 *         it has been set back to relative mode, so that the caller sets it
 *         up as a PS/2 mouse whose name says it is a Synaptics TouchPad.
 */
int synaptics_init(void);

/* What `synaptics_byte` made of a byte, Linux's `psmouse_ret_t`. */
#define SYNAPTICS_BAD  -1 /* it broke the packet's form */
#define SYNAPTICS_GOOD 0  /* the packet goes on */
#define SYNAPTICS_FULL 1  /* it ended a packet */

/**
 * Take the next byte of the touchpad's six-byte packet, in interrupt
 * context, as Linux's `synaptics_process_byte` with psmouse's framing: a
 * byte that breaks the packet's form or one that comes more than half a
 * second after the one before starts the packet again, and a whole packet
 * is reported, a pass-through packet's to the guest. Returns SYNAPTICS_*,
 * by which the caller counts bad packets in a row.
 */
int synaptics_byte(uint8_t data);

/** Whether the next byte begins a packet, as Linux's psmouse asks when it
 * looks for a device's announcement. */
int synaptics_packet_start(void);

/** Drop the packet under way, as Linux's `__psmouse_set_state` does at
 * each change of the port's state: when a run of commands begins and ends.
 * In interrupt context or with interrupts off. */
void synaptics_packet_restart(void);

/** Whether the pad set up has a pass-through port, Linux's
 * SYN_CAP_PASS_THROUGH, on which `synaptics_pt` reaches the guest. */
int synaptics_has_pass_through(void);

/**
 * The pass-through port as a line, Linux's `synaptics_pt_write` and the
 * pass-through packets: a byte sent goes as a sliced request ended by the
 * rate 0x28, and each byte of the guest's comes in a pass-through packet
 * read whole. With interrupts off during the bring-up, or inside a run of
 * `ps2mouse_cmd_begin`, with the pad's reporting disabled.
 */
extern const struct ps2io synaptics_pt;

/** Set the pad's mode for the guest's packets of `packet_size` bytes,
 * three or four, Linux's `synaptics_pt_activate`. */
void synaptics_pt_activate(int packet_size);

/**
 * Bring the pad back after the fifth bad packet in a row, as Linux's
 * `synaptics_reconnect` and `psmouse_initialize` do: the reset, the ID and
 * the knock, three times with a second between at most; the queries, which
 * must give the identity, model and capabilities found at boot; the mode;
 * and the rate, resolution and scaling. Returns 0, or -1 when the pad
 * does not answer or is another one. Inside a run of `ps2mouse_cmd_begin`.
 */
int synaptics_reconnect(void);

/** Remove the pad's event device, before the port is probed anew. */
void synaptics_disconnect(void);

#endif /* _SYNAPTICS_H_ */
