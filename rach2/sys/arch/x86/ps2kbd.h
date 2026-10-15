#ifndef _PS2KBD_H_
#define _PS2KBD_H_

/*
 * The PS/2 keyboard, on the 8042's first port: IRQ 1, one scancode of set 1
 * a byte, which it gives to the input layer (../../input.h).
 */

/** Take the keyboard's interrupt, vector 33, and unmask IRQ 1, which
 * `init_pic` left masked. Called once, at boot, from
 * `platform_keyboard_init`. */
void kbd_init(void);

#endif /* _PS2KBD_H_ */
