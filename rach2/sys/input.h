#ifndef _INPUT_H_
#define _INPUT_H_

#include "lib/kint.h"

/*
 * input.h: where a keystroke goes, whatever produced it.
 *
 * Two drivers produce keystrokes: the PS/2 keyboard on the 8042, and a USB
 * keyboard through the HID class driver. They agree on one thing, a set-1
 * scancode, because that is what the PS/2 hardware sends and what the games
 * that read the raw ring expect. The USB side translates its own usage codes
 * into the same codes rather than the rest of the kernel learning a second
 * alphabet.
 *
 * What happens after that is here and in one place: the modifier state, the
 * interception of Ctrl+C where the debug shell is the supervisor, the
 * scrollback keys, the translation to a character, and the choice between the
 * terminal's line discipline and the shell's own buffer.
 *
 * Two things this module needs belong to the debug shell, which is `kernel.c`:
 * whether the shell is the one reading, and a character for it. They are
 * declared here and defined there.
 */

/**
 * Take one set-1 scancode from whichever keyboard produced it.
 *
 * Runs in interrupt context, from the PS/2 handler or the timer, and puts the
 * code in the ring graphics-mode readers drain before anything else, so a game
 * sees the same stream whichever keyboard produced it.
 *
 * The modifier state is one, not one per keyboard, because this kernel holds
 * one keyboard at a time. Two keyboards would need one state each: releasing a
 * shift on either would otherwise release it for both.
 */
void input_key(uint8_t scancode);

/**
 * Take one mouse report, whatever produced it.
 *
 * The movements are as the device reports them, in its own units, and the
 * buttons are the low three bits: left, right, middle.
 */
void input_mouse(int dx, int dy, int buttons);

/** Whether any mouse exists, of any kind. `/dev/mouse` opens when one does. */
int input_mouse_present(void);

/** Say that a mouse of some kind has been found. */
void input_mouse_found(void);

/* What the debug shell provides. */

/** Whether the debug shell is the one reading the console. */
int shell_reads_input(void);

/** Give the debug shell one character. */
void shell_input_char(char c);

/** Send SIGINT to the foreground group; the shell's own out-of-band Ctrl+C. */
void shell_interrupt(void);

#endif
