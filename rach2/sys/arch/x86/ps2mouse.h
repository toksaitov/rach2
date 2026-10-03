#ifndef _PS2MOUSE_H_
#define _PS2MOUSE_H_

/*
 * The PS/2 mouse, on the 8042's auxiliary port: IRQ 12, three-byte
 * movement packets, which it puts in the mouse queue (../../mouse.h).
 */

/** Bring up the PS/2 mouse; 0 on success, -1 on failure. */
int init_mouse(void);

#endif /* _PS2MOUSE_H_ */
