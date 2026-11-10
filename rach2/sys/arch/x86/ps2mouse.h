#ifndef _PS2MOUSE_H_
#define _PS2MOUSE_H_

#include "../../lib/kint.h"

/*
 * The PS/2 mouse, on the 8042's auxiliary port: IRQ 12, movement packets of
 * three bytes, or four with a wheel, which it reports through its event
 * device and the mouse queue (../../mouse.h), or a Synaptics touchpad's
 * packets, which synaptics.h reads, or an Elantech touchpad's, which
 * elantech.h reads. Behind a Synaptics touchpad's pass-through port may be
 * a second device, a pointing stick or a mouse, the guest, which is found
 * and reported here as Linux's psmouse finds and reports the device of a
 * serio port of type SERIO_PS_PSTHRU.
 */

/**
 * Bring up the PS/2 mouse: the 8042's auxiliary port, its interrupt, the
 * mouse's protocol or the Synaptics touchpad's (`synaptics_init`), its
 * event device, and its reporting, with the handler on vector 44. Returns
 * 0 once reporting is on, -1 with the reason in the log when the
 * controller or the mouse does not answer. Turns interrupts off for the
 * handshake and on again before it returns, whatever they were. Called
 * once, at boot, from `platform_mouse_init`.
 */
int init_mouse(void);

/* The reply by which the mouse acknowledges a command or its argument. */
#define PS2MOUSE_ACK 0xFA

/**
 * Send the byte `cmd` to the auxiliary device and return its reply: the
 * ACK, or another byte some commands answer with. A request to send again
 * or a timeout tries again, three times in all. Returns the reply, or -1
 * after three failed tries. With interrupts off, during the bring-up.
 */
int ps2mouse_send(uint8_t cmd);

/**
 * Wait for the auxiliary device's next byte, dropping keyboard bytes before
 * it, as a command's data comes after its ACK. Returns the byte, or -1 on
 * the 8042's timeout. With interrupts off, during the bring-up.
 */
int ps2mouse_receive(void);

/*
 * A line to a PS/2 device: the auxiliary port's, or a touchpad's
 * pass-through port. `send` writes one byte and returns the device's reply
 * to it, the ACK or another byte, or -1; `receive` returns the device's
 * next byte, or -1 when none came.
 */
struct ps2io {
    int (*send)(uint8_t cmd);
    int (*receive)(void);
};

/** The auxiliary port as a line, `ps2mouse_send` and `ps2mouse_receive`. */
extern const struct ps2io ps2mouse_aux;

/**
 * Send `cmd` on `io`, then the `nparam` bytes of `param`, each of which the
 * device must acknowledge, then read the `nresult` bytes of its answer into
 * `result`: Linux's `ps2_command` with MAKE_PS2_CMD(nparam, nresult, cmd).
 * Returns 0, or -1 when a byte was refused or did not come.
 */
int ps2io_command(const struct ps2io *io, uint8_t cmd, const uint8_t *param, int nparam,
                  uint8_t *result, int nresult);

/**
 * Begin a run of commands to the mouse after the bring-up, from a process
 * or a kernel thread, with interrupts on: until `ps2mouse_cmd_end`, the IRQ
 * 12 handler leaves the auxiliary device's bytes in the 8042 for the
 * caller's commands, as Linux's psmouse leaves them to `ps2_command` while
 * its state is PSMOUSE_CMD_MODE. One run at a time: a second caller waits
 * for the first to end.
 */
void ps2mouse_cmd_begin(void);

/** End the run `ps2mouse_cmd_begin` began: the handler reads again, and a
 * byte that came after the run's last is taken now, since the 8042 raises
 * IRQ 12 again only once it is read. */
void ps2mouse_cmd_end(void);

/**
 * Probe the pass-through port `io` of the touchpad for a guest, as Linux's
 * `psmouse_connect` probes a port of type SERIO_PS_PSTHRU: the touchpad
 * disabled, its ID read, a TrackPoint's read-ID command (trackpoint.h),
 * then the IntelliMouse Explorer's and IntelliMouse's knocks; set it up
 * with Linux's rate, resolution and scaling; register its event device at
 * "synaptics-pt/serio0/input0" with Linux's name, numbers and
 * capabilities; set the touchpad's mode for the guest's packet size
 * (`synaptics_pt_activate`); and enable the guest's reporting. The caller
 * enables the touchpad's after. With interrupts off during the bring-up,
 * or inside a run of `ps2mouse_cmd_begin`. Returns 1 when a guest answered
 * and reports, 0 when none did.
 */
int ps2mouse_guest_connect(const struct ps2io *io);

/**
 * Report the guest's packet in `bytes`, three or four bytes as its protocol
 * has them, which the touchpad carried in a pass-through packet, as Linux's
 * `psmouse_process_byte` reports a whole packet, with the buttons
 * `ps2mouse_guest_buttons` gave. A guest not reporting yet, or none, takes
 * nothing. In interrupt context.
 */
void ps2mouse_guest_packet(const uint8_t *bytes);

/** The guest's packet size, 3 or 4, or 0 when there is no guest. */
int ps2mouse_guest_packet_size(void);

/**
 * The buttons of the stick that a touchpad wires to itself, in a packet's
 * first byte's places, left, right and middle: Linux's out-of-band
 * PSMOUSE_OOB_EXTRA_BTNS, reported at once on the guest's device and held
 * in each of its packets after. In interrupt context.
 */
void ps2mouse_guest_buttons(uint8_t buttons);

/**
 * Begin a change of the guest's settings, as Linux's
 * `psmouse_attr_set_helper` begins one: a run of commands, the touchpad and
 * then the guest disabled. Returns the guest's line, or NULL, having begun
 * nothing, when no guest reports. Process context.
 */
const struct ps2io *ps2mouse_guest_change_begin(void);

/** End the change `ps2mouse_guest_change_begin` began: the guest and then
 * the touchpad enabled again, and the run ended. */
void ps2mouse_guest_change_end(void);

/** Sleep `ms` milliseconds, as Linux's `ssleep` and `msleep` let a
 * command run wait for a device. Process context; the host tests move the
 * tick instead. */
void ps2mouse_msleep(unsigned int ms);

/**
 * Make the kernel thread kpsmoused, which reconnects a touchpad or mouse
 * that sent five packets out of form in a row, and a device that announced
 * itself anew with 0xAA 0x00 after a reset of its own, as Linux's psmouse
 * asks `serio_reconnect` to. Called once, when the first program starts; a
 * reconnect asked before then waits for the thread.
 */
void ps2mouse_thread_start(void);

/**
 * Reconnect what kpsmoused was asked to, as Linux's serio core reconnects a
 * port and the ports below it. The auxiliary device, after the fifth bad
 * packet in a row or its announcement, or when nothing was asked: a
 * Synaptics touchpad's reset and queries, which must find the same pad,
 * its mode, rate and resolution, and reporting again, then the
 * pass-through port, whose guest is reconnected, or probed anew when its
 * reconnect fails or there was none; an Elantech touchpad's reset and
 * knock, which must find an Elantech pad, its absolute mode, rate and
 * resolution, and reporting again; a mouse's reset, its protocol found
 * again, which must be the one found before, and reporting again. Each
 * keeps its event devices. When the device is not the same or does not
 * answer, its event devices are removed and the port is probed anew, as
 * Linux's serio core rescans a port whose reconnect failed. The guest
 * alone, after its announcement: the pass-through port as above. The work
 * of kpsmoused, in process context; the host tests call it directly.
 */
void ps2mouse_reconnect(void);

#endif /* _PS2MOUSE_H_ */
