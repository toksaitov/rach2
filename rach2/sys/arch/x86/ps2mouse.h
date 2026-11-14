#ifndef _PS2MOUSE_H_
#define _PS2MOUSE_H_

#include "../../lib/kint.h"

/*
 * The PS/2 mouse, on the 8042's auxiliary port: IRQ 12, movement packets of
 * three bytes, or four with a wheel, which it reports through its event
 * device and the mouse queue (../../mouse.h), or a Synaptics touchpad's
 * packets, which synaptics.h reads, or an Elantech touchpad's, which
 * elantech.h reads, or an ALPS touchpad's, which alps.h reads. Behind a
 * Synaptics touchpad's pass-through port may be a second device, a pointing
 * stick or a mouse, the guest, which is found and reported here as Linux's
 * psmouse finds and reports the device of a serio port of type
 * SERIO_PS_PSTHRU. Each port has Linux's psmouse files, `protocol`, `rate`,
 * `resolution`, `resetafter` and `resync_time`.
 */

/**
 * Bring up the PS/2 mouse: the 8042's auxiliary port, its interrupt, the
 * mouse's protocol or the Synaptics touchpad's (`synaptics_init`), its
 * event device, and its reporting, with the handler on vector 44. Returns
 * 0 once reporting is on, -1 with the reason in the log when the
 * controller or the mouse does not answer; a timeout of the configuration
 * byte's read or write also fails the controller's setup (`i8042_fail`)
 * and gives up the keyboard (`kbd_stop`) before interrupts come on again,
 * so that neither IRQ 1 nor the LED tick reaches the controller in between.
 * Turns interrupts off for the handshake and on again before it returns,
 * whatever they were. Called once, at boot, from `platform_mouse_init`.
 */
int init_mouse(void);

struct evdev;

/**
 * Register the port's main input device with `name`, the psmouse protocol
 * number `product` and EVDEV_KEY_BYTES of `keys`. During a protocol switch
 * this consumes its reserved device and cannot fail. At boot or reconnect
 * it returns NULL when every slot is taken. Only the main device uses
 * this call; a pad's stick registers separately. Called inside the command
 * lock or during bring-up, before the main device reports.
 */
struct evdev *ps2mouse_register(const char *name, uint16_t product, const uint8_t *keys);

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
 * for the first to end. The packet under way, the driver's and the
 * mouse's, is dropped, as it is again at the run's end, as Linux's
 * `__psmouse_set_state` drops it.
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
 * Begin a change of a TrackPoint's settings, as Linux's
 * `psmouse_attr_set_helper` begins one: a run of commands, the touchpad and
 * then the guest disabled, or the port's device alone when it is the
 * TrackPoint. Returns the stick's line, or NULL, having begun nothing, when
 * no guest reports and the port's device is no TrackPoint, or when the
 * stick the file was made for was unbound, or its protocol replaced, while
 * the call waited for the command lock. Process context.
 */
const struct ps2io *ps2mouse_guest_change_begin(void);

/** End the change `ps2mouse_guest_change_begin` began: the guest and then
 * the touchpad enabled again, or the port's TrackPoint, and the run
 * ended. */
void ps2mouse_guest_change_end(void);

/** Sleep `ms` milliseconds, as Linux's `ssleep` and `msleep` let a
 * command run wait for a device: in a run of commands, the caller sleeps;
 * during `init_mouse`, whose process cannot sleep, it waits with
 * interrupts on, so that the ticks run. The host tests move the tick
 * instead. */
void ps2mouse_msleep(unsigned int ms);

/**
 * Linux's `psmouse_set_rate` on the auxiliary port: the highest of the
 * rates 200, 100, 80, 60, 40, 20, 10 and 0 that is not above `rate`, sent,
 * a refusal ignored. Returns the rate sent, which the caller keeps as the
 * port's. Inside a run of commands, or during the bring-up.
 */
unsigned int ps2mouse_set_rate(unsigned int rate);

/**
 * Linux's `psmouse_initialize` on the auxiliary port: the port's rate
 * through `set_rate`, the protocol's own, or `ps2mouse_set_rate` for NULL,
 * then its resolution and 1:1 scaling, keeping the rate and resolution
 * sent as the port's, which its `rate` and `resolution` files show. Called
 * by a protocol's set-up and reconnect, inside a run of commands or during
 * the bring-up.
 */
void ps2mouse_initialize(unsigned int (*set_rate)(unsigned int));

/** The auxiliary port's rate, which a protocol's set-up reads, and its
 * setting to `rate`, as Linux's touchpads set `psmouse->rate`. */
unsigned int ps2mouse_port_rate(void);
void ps2mouse_port_rate_set(unsigned int rate);

/**
 * Begin a change through a file of the auxiliary port, as Linux's
 * `psmouse_attr_set_helper` begins one: a run of commands, and the device
 * disabled. Returns 0, or -ENODEV, having begun nothing, while the port
 * waits for its reconnect, has no driver, or lost the driver it had on
 * entry while the call waited for the command lock. Process context; it
 * may sleep.
 */
int ps2mouse_change_begin(void);

/** End the change `ps2mouse_change_begin` began, whose result is `rc`: the
 * device enabled again unless `rc` is -ENODEV, and the run ended. */
void ps2mouse_change_end(int rc);

/**
 * Parse the `len` bytes at `buf` as Linux's `kstrtouint` and `kstrtou8`
 * do: an optional '+', in base 16 an optional "0x", digits of `base`, 10
 * or 16, and an optional newline, nothing after. Returns 0 with the value
 * in `*out`, -EINVAL for text that is no number, or -ERANGE for one above
 * `max`.
 */
int ps2mouse_parse_uint(const char *buf, uint32_t len, unsigned int base, uint32_t max,
                        uint32_t *out);

/**
 * Resync what the handler asked kpsmoused to, as Linux's `psmouse_resync`
 * does: the port's device after a pause of half a second inside a mouse's
 * packet or a first byte after `resync_time` seconds idle, or the guest
 * after its idle time; each is disabled, polled for a packet, which must
 * have its protocol's form, and enabled again, and one that fails is
 * reconnected. A resync a change of the port overtook is not done. The
 * work of kpsmoused, in process context; the host tests call it directly.
 */
void ps2mouse_resync(void);

/**
 * Make the kernel thread kpsmoused, which reconnects a touchpad or mouse
 * that sent `resetafter` packets out of form in a row, and a device that
 * announced itself anew with 0xAA 0x00 after a reset of its own, as Linux's
 * psmouse asks `serio_reconnect` to, and resyncs one as `ps2mouse_resync`
 * says. Called once, when the first program starts; a reconnect asked
 * before then waits for the thread.
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
 * resolution, and reporting again; an ALPS touchpad's reset and
 * identification, which must find a pad Linux drives, its set-up, rate
 * and resolution, and reporting again; a mouse's reset, its protocol found
 * again, which must be the one found before, and reporting again. Each
 * keeps its event devices. When the device is not the same or does not
 * answer, its event devices are removed and the port is probed anew, as
 * Linux's serio core rescans a port whose reconnect failed. The guest
 * alone, after its announcement: the pass-through port as above. The work
 * of kpsmoused, in process context; the host tests call it directly.
 */
void ps2mouse_reconnect(void);

#endif /* _PS2MOUSE_H_ */
