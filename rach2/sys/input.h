/*
 * input.h: where a keystroke goes, whatever keyboard produced it, and the
 * mouse reports of every mouse.
 */
#ifndef _INPUT_H_
#define _INPUT_H_

#include "lib/kint.h"

/*
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
 * Three things this module needs belong to the debug shell, which is
 * `kernel.c`: whether the shell is the one reading, a character for it, and
 * its Ctrl+C. They are declared here and defined there.
 */

/**
 * Route one set-1 scancode from whichever keyboard produced it, in interrupt
 * context: from the PS/2 keyboard's interrupt, or from the USB stack's
 * service of a keyboard's report, which runs from the controller's interrupt
 * or the tick. A code ends a suspend in force (`suspend_wake`).
 *
 * Every code, including the ones consumed here, goes first to the ring a
 * graphics-mode reader drains, since a game needs the modifiers and the
 * releases too. The extended Page Up, Page Down, Home and End scroll the
 * screen whenever the debug shell reads input, and Shift with Page Up or Page
 * Down scrolls it in a session, where any other key returns it to the live
 * screen before it is sent, as on Linux's console. When a reader is parked on
 * the console or a session owns it, Ctrl+C goes through the line discipline
 * like every other key: its ISIG path posts SIGINT to the foreground group and
 * flushes the pending input, as a serial Ctrl+C does. Otherwise Ctrl+C is
 * `shell_interrupt`, out of band, and other keys go to the debug shell's own
 * buffer when it reads.
 *
 * For the discipline, Backspace, '\b' in the keymap, becomes DEL, its erase
 * character, Ctrl with a letter becomes that letter's control byte, Alt puts
 * ESC before a key's character, as Linux's console sends Meta, and the
 * arrows and editing keys become their escape sequences, so line editing, end
 * of file and cursor keys arrive as they would from a terminal. A key nobody
 * is parked for waits in the discipline as type-ahead.
 *
 * The modifier state is one for every keyboard, so a Shift released on one
 * keyboard is released for all.
 */
void input_key(uint8_t scancode);

/**
 * Put one mouse report in the mouse queue (`mouse_push_packet`), for the USB
 * mouse driver; the platform's mouse fills the queue itself.
 *
 * The movements are as the driver gives them, in the device's own units, and
 * the buttons are the low three bits: left, right, middle.
 */
void input_mouse(int dx, int dy, int buttons);

/** Whether any mouse exists, of any kind. `/dev/mouse` opens when one does. */
int input_mouse_present(void);

/** Say that a mouse of some kind has been found. */
void input_mouse_found(void);

/* What the debug shell provides. */

/** Whether the debug shell is the one reading the console: it runs, and no
 * process waits for the console's input. */
int shell_reads_input(void);

/**
 * Give the debug shell one character typed at the console: a newline ends
 * the line, a backspace erases one character, and any other is added to the
 * line and echoed; each returns a scrolled-back screen to the live area.
 * Nothing while the shell is not running.
 */
void shell_input_char(char c);

/**
 * Echo `^C` and send SIGINT to the console's foreground process group, or
 * with none to the running user process when it is of the console's
 * session: the debug shell's Ctrl+C, out of band, when no session owns the
 * console.
 */
void shell_interrupt(void);

#endif
