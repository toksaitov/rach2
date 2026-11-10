#ifndef _PS2MOUSE_H_
#define _PS2MOUSE_H_

#include "../../lib/kint.h"

/*
 * The PS/2 mouse, on the 8042's auxiliary port: IRQ 12, movement packets of
 * three bytes, or four with a wheel, which it reports through its event
 * device and the mouse queue (../../mouse.h), or a Synaptics touchpad's
 * packets, which synaptics.h reads, or an Elantech touchpad's, which
 * elantech.h reads.
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

#endif /* _PS2MOUSE_H_ */
