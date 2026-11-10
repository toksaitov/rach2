/*
 * input.h: where a keystroke goes, whatever keyboard produced it, and
 * whether a mouse exists; each mouse reports through mouse.h.
 */
#ifndef _INPUT_H_
#define _INPUT_H_

#include "lib/kint.h"

/*
 * Two drivers produce keystrokes: the PS/2 keyboard on the 8042, and a USB
 * keyboard through the HID class driver. Each keyboard is a `struct
 * input_keyboard`, and each gives this module its keys as Linux's key codes,
 * KEY_* of <linux/input-event-codes.h>: the PS/2 keyboard's set-1 bytes
 * through `input_set1_event`, which decodes them, and a USB keyboard's
 * usages through `input_key_event`, which the HID driver calls with the
 * code `set1_keycode` gives for the usage's set-1 code, so that one table
 * names every key.
 *
 * Here and in one place for every keyboard are the keys each one holds, the
 * modifiers derived from them, and the repeat. Each accepted event goes, in
 * this order, to the keyboard's event device (evdev.h), to the console's
 * keyboard (`kd_key_event`, kd.h), which translates it through the keymap,
 * and as the set-1 bytes of the event, the raw view `input_raw_bytes` gives,
 * to the console's raw mode (`kd_raw_code`).
 *
 * Every keyboard's interrupt and the tick run on the boot processor with
 * interrupts off, so events reach their consumers one at a time and in
 * order; a lock keeps the state against the requests of a process on
 * another processor.
 */

struct evdev;

/* The bytes of a set of Linux's key codes, one bit for each of KEY_CNT,
 * 0x300: evdev.h's EVDEV_KEY_BYTES. */
#define INPUT_KEY_BYTES 96

/* A keyboard. Its driver owns the storage, zeroed before
 * `input_keyboard_attach`; the rest is this module's. */
struct input_keyboard {
    struct evdev *dev;             /* its event device, or NULL */
    uint8_t prefix;                /* set 1: 0xE0 or 0xE1 when that came last, else 0 */
    uint8_t skip;                  /* set 1: Pause's byte after E1 is still to come */
    uint8_t down[INPUT_KEY_BYTES]; /* the keys it holds, a bit for each key code */
    uint16_t *keymap;              /* set 1: each scan code's key code, or NULL */
    struct input_keyboard *next;   /* the keyboards attached */
};

/* The entries of a set-1 keymap, as Linux's atkbd has: the scan code, 0x80
 * set after 0xE0, indexes it, and the upper half is never reached. */
#define INPUT_SET1_KEYMAP 512

/* The modifiers `input_modifiers` gives, a bit for each side. */
#define INPUT_MOD_SHIFTL 0x01u /* KEY_LEFTSHIFT */
#define INPUT_MOD_SHIFTR 0x02u /* KEY_RIGHTSHIFT */
#define INPUT_MOD_CTRLL  0x04u /* KEY_LEFTCTRL */
#define INPUT_MOD_CTRLR  0x08u /* KEY_RIGHTCTRL */
#define INPUT_MOD_ALT    0x10u /* KEY_LEFTALT */
#define INPUT_MOD_ALTGR  0x20u /* KEY_RIGHTALT */
#define INPUT_MOD_METAL  0x40u /* KEY_LEFTMETA, the left Super */
#define INPUT_MOD_METAR  0x80u /* KEY_RIGHTMETA */
#define INPUT_MOD_SHIFT  (INPUT_MOD_SHIFTL | INPUT_MOD_SHIFTR)
#define INPUT_MOD_CTRL   (INPUT_MOD_CTRLL | INPUT_MOD_CTRLR)
#define INPUT_MOD_SUPER  (INPUT_MOD_METAL | INPUT_MOD_METAR)

/**
 * Attach the keyboard `kb`, zeroed by its driver: register it as an event
 * device that can report every key `set1_keycode` names, with MSC_SCAN,
 * EV_REP and EV_LED. `name` and `phys` are kept by their pointers, and
 * `bustype`, `vendor` and `product` are what EVIOCGID gives. A keyboard with
 * no event device, every node being taken, still types. Any context.
 */
void input_keyboard_attach(struct input_keyboard *kb, const char *name, const char *phys,
                           uint16_t bustype, uint16_t vendor, uint16_t product);

/**
 * Attach the keyboard `kb`, zeroed by its driver, as `input_keyboard_attach`
 * does, with the event device `dev` its driver registered and described,
 * whose keys are its own: a USB keyboard's, which its report descriptor
 * names. `dev` may be NULL.
 */
void input_keyboard_attach_dev(struct input_keyboard *kb, struct evdev *dev);

/**
 * Detach `kb`, a keyboard that has gone: release every key it holds, so that
 * none stays down for ever, stop the repeat of its key, and take its event
 * device away. Any context. A keyboard not attached is ignored.
 */
void input_keyboard_detach(struct input_keyboard *kb);

/**
 * Release `keycode` on the keyboard whose event device is `dev`, as a key's
 * release from the hardware goes: the held keys, the modifiers, the repeat,
 * the event device and the console all see it. EVIOCSKEYCODE calls it when
 * a held key's code leaves the keymap, as Linux's input_set_keycode passes
 * that release to every handler. Returns 1 when the release went out, 0
 * when no attached keyboard has `dev` or it does not hold the key. Any
 * context; the evdev lock is not held.
 */
int input_keyboard_release(struct evdev *dev, unsigned int keycode);

/**
 * Take one key of `kb`, in interrupt context or from the tick: `keycode` is
 * Linux's key code and `down` nonzero for a press, 0 for a release; `scan`
 * is the code the keyboard sent, which the event device reports as MSC_SCAN.
 * A `keycode` of 0, a key no table names, reaches the event device as its
 * MSC_SCAN alone. A press of a key `kb` holds already, a PS/2 keyboard's own
 * repeat, and a release of a key it does not hold are dropped. Every event
 * ends a suspend in force (`suspend_wake`) and puts off the console's
 * blanking (`console_poke`). A press starts the repeat of its key, in place
 * of any other's, unless it is a modifier or a lock; a release of the
 * repeating key stops it.
 */
void input_key_event(struct input_keyboard *kb, uint32_t scan, unsigned int keycode, int down);

/**
 * Give the PS/2 keyboard `kb` one set-1 code, in interrupt context: a prefix
 * is kept for the code after it, and a key's code goes to `input_key_event`
 * with the key code its keymap gives, or `set1_keycode` without one, and
 * with MSC_SCAN the code with its release bit clear and 0x80 set after E0,
 * as Linux's keyboard driver reports it. Pause's E1 1D 45 is the extended
 * 0x45, KEY_PAUSE. The shifts a keyboard wraps a cursor key in, E0 2A and
 * E0 36, and the controller's own bytes, 0x00, 0xFA, 0xFE and 0xFF, are not
 * keys.
 */
void input_set1_event(struct input_keyboard *kb, uint8_t code);

/**
 * Give the attached keyboard `kb`, whose bytes are set 1, the keymap `map`
 * of INPUT_SET1_KEYMAP entries, filled here with the key codes
 * `set1_keycode` gives, so that EVIOCSKEYCODE can change what a scan code
 * reports, as Linux's atkbd keeps its keycode table; `input_set1_event`
 * reads it from then on. The caller owns `map` while `kb` is attached.
 */
void input_keyboard_set1_keymap(struct input_keyboard *kb, uint16_t *map);

/**
 * Store in `out`, of 3 bytes, the set-1 bytes a PS/2 keyboard sends for the
 * event `value`, KD_KEY_PRESS, KD_KEY_REPEAT or KD_KEY_RELEASE, of Linux's
 * key `keycode`: the make code for a press or a repeat, the break code for a
 * release, each after E0 for an extended key, and Pause's E1 1D 45 and E1 9D
 * C5. This is the raw view of every keyboard, USB ones among them, for K_RAW.
 * Returns the bytes stored, 0 for a key no set-1 code names.
 */
int input_raw_bytes(unsigned int keycode, int value, uint8_t *out);

/** The modifiers some keyboard holds now, INPUT_MOD_*: a side stays held
 * while any keyboard holds it. */
unsigned int input_modifiers(void);

/**
 * Make the repeat of the held key due by now, from the boot processor's tick
 * with interrupts off: a repeat goes out as `input_key_event`'s events do, with
 * the value KD_KEY_REPEAT, and the next is due a period after the last was due,
 * not after the tick that sent it, so that a tick longer than a millisecond
 * keeps a 33 ms period on average. A tick that comes late sends at most four
 * and starts the count again from now.
 */
void input_tick(void);

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
 * -1. Any context. Each keyboard's event device reports the times that
 * changed as EV_REP events.
 */
void input_set_repeat(int delay_ms, int period_ms);

/** Store the repeat's times in milliseconds in `*delay_ms` and `*period_ms`:
 * 250 and 33, Linux's, until `input_set_repeat` changes them. */
void input_get_repeat(int *delay_ms, int *period_ms);

/** Stop the repeat of whatever key is held, until a key is pressed again:
 * for a change of the keyboard's mode or of the screen's owner. Any
 * context. */
void input_cancel_repeat(void);

/**
 * Light the LEDs of every keyboard, `mask` LED_SCR, LED_NUM and LED_CAP of
 * kd.h, and of a keyboard connected later. Any context: it only records the
 * mask, and each keyboard's driver sends it from its own periodic work, the
 * PS/2 keyboard's from the tick (`ps2kbd_tick`) and a USB keyboard's from
 * the USB stack's step, so a change reaches the keyboards within a few ticks.
 * Each keyboard's event device reports the LEDs that changed as EV_LED
 * events.
 */
void input_set_leds(unsigned int mask);

/**
 * An event a program wrote to a keyboard's event device (`evdev_write`):
 * EV_LED lights or puts out the LED `code`, LED_NUML, LED_CAPSL or
 * LED_SCROLLL of evdev.h, of every keyboard, as `input_set_leds`; EV_REP
 * sets the repeat's REP_DELAY or REP_PERIOD to `value`, as
 * `input_set_repeat`; and EV_KEY, a key that passed the input core's
 * filters with no grab held, goes to the console's keyboard and its raw
 * view, `value` KD_KEY_*, as a key of a keyboard does. Process context.
 */
void input_written(uint16_t type, uint16_t code, int32_t value);

/**
 * Give the console's keyboard and its raw view the key `keycode`, `value`
 * KD_KEY_*, of `device`, an event device that is no keyboard but whose keys
 * Linux's console takes, as a keyboard's key goes to them; `evdev_report`
 * calls it once the key passed the input core's filters with no grab held.
 * Any context.
 */
void input_console_key(const void *device, unsigned int keycode, int value);

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
