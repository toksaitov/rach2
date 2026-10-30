/*
 * blank.h: the console's blanking, which darkens the screen and its
 * backlight after a time without input, and undoes it at the next key or
 * mouse move.
 */
#ifndef _BLANK_H_
#define _BLANK_H_

/*
 * The console blanks after `consoleblank=` seconds without a key or a mouse
 * move, 600 unless the boot line says, and never for 0; the escape
 * `ESC [ 9 ; N ]` sets the time to N minutes, at most 60, as on Linux.
 * Blanking paints the screen black, keeping what it showed, and turns every
 * backlight off (backlight.h); the next key or mouse move restores both,
 * except after a blank that `console_blank_now` forced, which only
 * `console_unblank_now` undoes. While a program holds /fb the console does
 * not blank, and taking /fb unblanks it. Output goes on while it is blank
 * and is shown at the unblank.
 */

/**
 * Note a key or a mouse move: restart the countdown to the next blank, and
 * unblank the console if it is blank. The keyboard and mouse paths call it
 * for every code and packet (input.c, mouse.c). Any context, an interrupt
 * handler with interrupts off among them; it does not sleep. The unblank
 * itself is the next tick's (`blank_tick`).
 */
void console_poke(void);

/** Read `consoleblank=` and probe the backlights (`backlight_init`); once,
 * at boot, from `tty_console_init`. */
void blank_init(void);

/** Blank or unblank the console as its countdown and the last poke say;
 * from the boot processor's tick, with interrupts off (`tty_console_poll`). */
void blank_tick(void);

/** Set the time without input after which the console blanks, in seconds,
 * 0 for never, and restart the countdown and unblank, as a poke does, with
 * the cause "by the escape". Any context. */
void console_set_blank_time(unsigned int seconds);

/** Blank the console now, to stay blank until `console_unblank_now`
 * (TIOCLINUX 14); nothing while a program holds /fb. Not from the tick or
 * with the console's lock held. */
void console_blank_now(void);

/** Unblank the console now, if it is blank (TIOCLINUX 4), and restart the
 * countdown, as a poke does; under the same rule as `console_blank_now`. */
void console_unblank_now(void);

/** Whether the console is blank (TIOCLINUX 15). */
int console_is_blank(void);

#endif /* _BLANK_H_ */
