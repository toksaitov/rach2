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
 * or the tick. A code ends a suspend in force (`suspend_wake`) and puts off the
 * console's blanking (`console_poke`).
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

struct evdev;

/*
 * Each keyboard is an event device of its own as well (evdev.h): its keys
 * go to /dev/input/eventN as Linux's KEY_* codes, `set1_keycode`'s, each
 * with the code it came from as MSC_SCAN, beside the console's path above,
 * which every keyboard shares.
 */

/**
 * Register a keyboard as an event device that can report every key
 * `set1_keycode` names, with MSC_SCAN. `name` and `phys` are kept by their
 * pointers, and `bustype`, `vendor` and `product` are what EVIOCGID gives.
 * Any context. Returns the device, or NULL when every node is taken.
 */
struct evdev *input_keyboard_register(const char *name, const char *phys, uint16_t bustype,
                                      uint16_t vendor, uint16_t product);

/* The decoder of a keyboard that sends set-1 codes, as the 8042 gives a
 * translated keyboard's, and its event device. */
struct input_set1 {
    struct evdev *dev;
    uint8_t prefix; /* 0xE0 or 0xE1 when that prefix came last, else 0 */
    uint8_t skip;   /* the byte of Pause's sequence after E1 is still to come */
};

/**
 * Give the event device of `kb` one set-1 code, in interrupt context: a
 * prefix is kept for the code after it, and a key's code is reported as
 * `evdev_report_key` does, with MSC_SCAN the code with its release bit
 * clear and 0x80 set after E0, as Linux's keyboard driver reports it.
 * Pause's E1 1D 45 is the extended 0x45, KEY_PAUSE. The shifts a keyboard
 * wraps a cursor key in, E0 2A and E0 36, and the controller's own bytes,
 * 0x00, 0xFA, 0xFE and 0xFF, are not keys.
 */
void input_set1_event(struct input_set1 *kb, uint8_t code);

/*
 * The repeat and the LEDs of every keyboard, which the console's KD*
 * requests and evdev's EVIOCSREP set. The keyboards repeat in software, in
 * one place for all of them, so that a USB keyboard repeats as a PS/2 one
 * does and both at the same rate.
 */

/**
 * Set the repeat of every keyboard: the first repeat of a held key comes
 * `delay_ms` after its press, and the next ones every `period_ms`. A negative
 * time leaves that one as it is, and a period of 0 turns repeat off, as
 * EVIOCSREP's does; KDKBDREP's 0, which leaves a time as it is, is given as
 * -1. Any context. It sets nothing yet: a PS/2 keyboard repeats by its own
 * typematic and a USB keyboard not at all.
 */
void input_set_repeat(int delay_ms, int period_ms);

/** Store the repeat's times in milliseconds in `*delay_ms` and `*period_ms`:
 * 250 and 33, Linux's, until `input_set_repeat` changes them. */
void input_get_repeat(int *delay_ms, int *period_ms);

/** Stop the repeat of whatever key is held, until it is pressed again: for a
 * change of the keyboard's mode or of the screen's owner. Any context. */
void input_cancel_repeat(void);

/**
 * Light the LEDs of every keyboard, `mask` LED_SCR, LED_NUM and LED_CAP of
 * kd.h, and of a keyboard connected later. Any context: it only records the
 * mask, and each keyboard's driver sends it from its own periodic work, the
 * PS/2 keyboard's from the tick (`ps2kbd_tick`) and a USB keyboard's from
 * the USB stack's step, so a change reaches the keyboards within a few ticks.
 */
void input_set_leds(unsigned int mask);

/** The LEDs every keyboard is to show, as `input_set_leds` last set them:
 * 0, all dark, until it is called. Any context. */
unsigned int input_leds(void);

/**
 * Offer `code`, a byte the PS/2 keyboard sent, to the command waiting for the
 * keyboard's reply, such as the LEDs': ACK, 0xFA, or RESEND, 0xFE. Returns 1
 * when the byte was the reply and is consumed, and 0 when it goes on to the
 * key stream. Called by the keyboard's interrupt before the byte reaches
 * `input_set1_event`, in interrupt context. Defined by the PS/2 keyboard's
 * driver (arch/x86/ps2kbd.c), whose commands they are.
 */
int input_ps2_reply(uint8_t code);

/**
 * Put one mouse report in the mouse queue (`mouse_push_packet`), for the USB
 * mouse driver; the platform's mouse fills the queue itself.
 *
 * The movements are as the driver gives them, in the device's own units, and
 * the buttons are the low three bits: left, right, middle.
 */
void input_mouse(int dx, int dy, int buttons);

/** Whether any mouse exists, of any kind. `/mouse` opens when one does. */
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
