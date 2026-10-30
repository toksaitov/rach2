/*
 * blank.h: the console's blanking, which darkens the screen and its
 * backlight after a time without input, and undoes it at the next key or
 * mouse move.
 */
#ifndef _BLANK_H_
#define _BLANK_H_

/**
 * Note a key or a mouse move: restart the countdown to the next blank, and
 * unblank the console if it is blank. The keyboard and mouse paths call it
 * for every code and packet (input.c, mouse.c). Any context, an interrupt
 * handler with interrupts off among them; it does not sleep.
 */
void console_poke(void);

#endif /* _BLANK_H_ */
