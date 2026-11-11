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
 * packet is reported. The parity check of versions 1 and 2 runs while
 * `paritycheck` is set, and each packet is logged while `debug` is 2 or
 * more. Returns ELANTECH_*, which ps2mouse.c counts toward
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

/**
 * The pad's `set_rate`, as Linux's `elantech_set_rate_restore_reg_07`: the
 * rate through `ps2mouse_set_rate`, then on firmware 0x381f17, which loses
 * its mode at a change of rate, register 0x07 written again. Returns the
 * rate set. Inside a run of `ps2mouse_cmd_begin`.
 */
unsigned int elantech_set_rate(unsigned int rate);

/** The size of the pad's packet, 4 on version 1 and 6 on the others, which
 * psmouse's resync polls for. */
int elantech_packet_size(void);

/**
 * Add the pad's files below `dir`, its serio port's directory in /sys, as
 * Linux's `elantech_attr_group` shows them: reg_07, reg_10, reg_11 and
 * reg_20 to reg_26, which read the register from the pad and write a byte
 * in hex to it, and debug, paritycheck and crc_enabled, the driver's
 * settings, which read and take a byte in hex. Each reads as "0x", two hex
 * digits and a newline. The pad is disabled while a file reads or writes
 * it. Returns 0, or -errno. Process context.
 */
int elantech_sysfs_add(int dir);

/** Remove the pad's event devices, as Linux's `elantech_disconnect` does,
 * before the port is probed anew. Process context. */
void elantech_disconnect(void);

#endif /* _ELANTECH_H_ */
