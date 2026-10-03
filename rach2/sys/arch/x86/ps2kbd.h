#ifndef _PS2KBD_H_
#define _PS2KBD_H_

/*
 * The PS/2 keyboard, on the 8042's first port: IRQ 1, one scancode of set 1
 * a byte, which it gives to the input layer (../../input.h).
 */

/** Take the keyboard's interrupt and unmask its line. */
void kbd_init(void);

#endif /* _PS2KBD_H_ */
