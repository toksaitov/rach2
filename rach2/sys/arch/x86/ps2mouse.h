#ifndef _PS2MOUSE_H_
#define _PS2MOUSE_H_

/*
 * The PS/2 mouse, on the 8042's auxiliary port: IRQ 12, three-byte
 * movement packets, which it puts in the mouse queue (../../mouse.h).
 */

/**
 * Bring up the PS/2 mouse: the 8042's auxiliary port, its interrupt, and
 * the mouse's reporting, with the handler on vector 44. Returns 0 once
 * reporting is on, -1 with the reason in the log when the controller or the
 * mouse does not answer. Turns interrupts off for the handshake and on
 * again before it returns, whatever they were. Called once, at boot, from
 * `platform_mouse_init`.
 */
int init_mouse(void);

#endif /* _PS2MOUSE_H_ */
