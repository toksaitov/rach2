/*
 * kd.h: the console's keyboard as Linux's console keeps it: the key event
 * every keyboard feeds, Linux's KD* requests on the console, and the state
 * kept for the screen's owners.
 */
#ifndef _KD_H_
#define _KD_H_

#include "lib/kint.h"
#include "tty/n_tty.h"

/*
 * The requests and structures of Linux's <linux/kd.h>, with its numbers, which
 * kbd's tools and Linux's programs send to the console. `kd_ioctl` answers
 * them; KIOCSOUND, 0x4B2F, is the speaker's and stays with the console's
 * other requests in syscall.c.
 */

#define KDGETLED 0x4B31 /* the LEDs lit, LED_* */
#define KDSETLED 0x4B32 /* light LED_*; a higher bit set follows the locks again */
#define LED_SCR  0x01   /* Scroll Lock's LED */
#define LED_NUM  0x02   /* Num Lock's LED */
#define LED_CAP  0x04   /* Caps Lock's LED */

#define KDGKBTYPE 0x4B33 /* the keyboard's type, into a byte */
#define KB_84     0x01
#define KB_101    0x02 /* what Rach2 and Linux answer */
#define KB_OTHER  0x03

#define KDSETMODE   0x4B3A /* the console's mode, KD_TEXT or KD_GRAPHICS */
#define KDGETMODE   0x4B3B /* the console's mode, into an int */
#define KD_TEXT     0x00
#define KD_GRAPHICS 0x01

#define KDGKBMODE   0x4B44 /* the keyboard's mode, K_*, into an int */
#define KDSKBMODE   0x4B45 /* set the keyboard's mode, K_* in arg */
#define K_RAW       0x00   /* set-1 scancodes, as the keyboard sent them */
#define K_XLATE     0x01   /* the keymap's 8-bit characters */
#define K_MEDIUMRAW 0x02   /* key codes: the code, 0x80 set on release */
#define K_UNICODE   0x03   /* the keymap's characters in UTF-8 */
#define K_OFF       0x04   /* nothing */

#define KDGKBMETA   0x4B62 /* how Alt reaches a program, K_METABIT or K_ESCPREFIX */
#define KDSKBMETA   0x4B63
#define K_METABIT   0x03 /* the character with its top bit set */
#define K_ESCPREFIX 0x04 /* ESC, then the character */

#define KDGKBLED     0x4B64 /* the lock flags, K_*LOCK, into a byte */
#define KDSKBLED     0x4B65 /* set the lock flags, and the default flags in bits 4-6 */
#define K_SCROLLLOCK 0x01
#define K_NUMLOCK    0x02
#define K_CAPSLOCK   0x04

/* One entry of a keymap table, KDGKBENT's and KDSKBENT's: `kb_table` is the
 * table, the modifier combination (keymap.h), and `kb_index` the key code. */
struct kbentry {
    unsigned char kb_table;
    unsigned char kb_index;
    unsigned short kb_value; /* the entry, in keymap.h's encoding */
};
#define K_NORMTAB     0x00
#define K_SHIFTTAB    0x01
#define K_ALTTAB      0x02
#define K_ALTSHIFTTAB 0x03

#define KDGKBENT 0x4B46 /* one entry of a table */
#define KDSKBENT 0x4B47 /* set one; K_NOSUCHMAP at index 0 frees a table but 0 */

/* A function key's string, KDGKBSENT's and KDSKBSENT's. */
struct kbsentry {
    unsigned char kb_func;        /* the string's number, the value of a KT_FN entry */
    unsigned char kb_string[512]; /* null-terminated */
};
#define KDGKBSENT 0x4B48
#define KDSKBSENT 0x4B49

/* The accent table, in the 8-bit form of K_XLATE. */
struct kbdiacr {
    unsigned char diacr, base, result;
};
struct kbdiacrs {
    unsigned int kb_cnt; /* the entries used, at most 256 */
    struct kbdiacr kbdiacr[256];
};
#define KDGKBDIACR 0x4B4A
#define KDSKBDIACR 0x4B4B

/* The accent table in code points, which the console keeps. */
struct kbdiacruc {
    unsigned int diacr, base, result;
};
struct kbdiacrsuc {
    unsigned int kb_cnt; /* the entries used, at most 256 */
    struct kbdiacruc kbdiacruc[256];
};
#define KDGKBDIACRUC 0x4BFA
#define KDSKBDIACRUC 0x4BFB

/* The repeat's times, KDKBDREP's: a time of 0 or less is left as it is, and
 * the request returns the times in force. */
struct kbd_repeat {
    int delay;  /* milliseconds from the press to the first repeat */
    int period; /* milliseconds between repeats */
};
#define KDKBDREP 0x4B52

/* The values of a key event: as evdev's EV_KEY values. */
#define KD_KEY_RELEASE 0
#define KD_KEY_PRESS   1
#define KD_KEY_REPEAT  2

/**
 * Take one key event for the console: `keycode` is Linux's KEY_* code and
 * `value` KD_KEY_RELEASE, KD_KEY_PRESS or KD_KEY_REPEAT, a repeat made by the
 * input layer's software repeat. `device` names the keyboard the key is on,
 * the same pointer for as long as the keyboard is connected, so the state of
 * each keyboard stays apart.
 *
 * In K_XLATE and K_UNICODE the key goes through the keymap's translator
 * (keymap.h) with the console's locks, and its bytes, as `keymap_encode`
 * writes them under the cursor and keypad modes of `kio_key_modes`, go to
 * the console's input while a reader is parked on it or a session owns it,
 * after the screen comes back from the scrollback; K_XLATE sends a Latin-1
 * byte for a character, and nothing for one past Latin-1, where K_UNICODE
 * sends UTF-8, and K_METABIT sets an Alt character's top bit in place of
 * ESC before it. Otherwise the debug shell takes the key: ETX, Ctrl+C on
 * any layout, is its interrupt (`shell_interrupt`) whether or not it reads,
 * and while it reads a key that sends no escape sequence is its input, DEL
 * as Backspace. The console's own keys are spent before that: Shift with
 * Page Up or Page Down scrolls the screen back and forth, and while the
 * debug shell reads, so do Page Up, Page Down, Home and End. A key that
 * changes the lock flags relights the LEDs while they follow the flags.
 *
 * In K_MEDIUMRAW the key goes to the console's input as Linux sends it: a
 * code below 128 as one byte, with 0x80 set on release, and a higher one as
 * three, 0x00 or 0x80 on release, then the code's high and low seven bits
 * with 0x80 set; a repeat is a press. In K_RAW and K_OFF it does nothing.
 *
 * Called in interrupt context or from the tick, with interrupts off; it does
 * not sleep.
 */
void kd_key_event(const void *device, unsigned int keycode, int value);

/**
 * Give the console one byte of the keyboards' raw view, the set-1 bytes
 * `input_raw_bytes` gives for every press, repeat and release of any
 * keyboard, prefixes among them. In K_RAW the byte goes to the console's
 * input, as Linux's raw mode sends a PS/2 keyboard's bytes; in every other
 * mode it is not used. Interrupt context or the tick, interrupts off.
 */
void kd_raw_code(uint8_t code);

/**
 * Whether the console translates keys into characters: the keyboard is in
 * K_XLATE or K_UNICODE. In K_RAW, K_MEDIUMRAW and K_OFF the translator, the
 * lock keys, the console's own keys, such as its scrollback and the debug
 * shell's Ctrl+C, and COM1's input are all off, and the console's input
 * holds only what `kd_raw_code` and `kd_key_event` give it. Any context.
 */
int kd_translates(void);

/**
 * Answer one of Linux's KD* requests on the console, in the calling process's
 * context, with `arg` its user argument: a pointer, or the value for
 * KDSKBMODE, KDSKBMETA, KDSETLED and KDSKBLED. Returns 0 or -errno: -EFAULT
 * for an argument it cannot read or write, -EINVAL for a value out of range,
 * -ENOMEM, and -ENOTTY for a request that is not its own, so that the console
 * goes on to its other requests.
 *
 * It answers KDGKBTYPE, KDGKBMODE, KDSKBMODE, KDGKBMETA, KDSKBMETA,
 * KDGETLED, KDSETLED, KDGKBLED, KDSKBLED, KDGETMODE, KDGKBENT, KDSKBENT,
 * KDGKBSENT, KDSKBSENT, KDGKBDIACR, KDSKBDIACR, KDGKBDIACRUC, KDSKBDIACRUC
 * and KDKBDREP, with Linux's numbers and structures. KDSETMODE is not among
 * them: a program takes the screen through /fb.
 *
 * The requests that set the mode, the meta setting, the LEDs and the lock
 * flags are open to a process of the console's session, the process that
 * holds /fb and a privileged process, as Linux opens them to the terminal's
 * own processes; the keymap, the strings, the accents and the repeat are the
 * whole system's, and a privileged process's alone. Others get -EPERM. A pty
 * never reaches this function, so every KD* request there is -ENOTTY.
 *
 * Two answers depart from Linux's, so that `loadkeys` loads a Unicode keymap
 * while `term` keeps the keyboard in K_MEDIUMRAW and translates its key codes
 * through the console's keymap: KDGKBENT gives a code point's entry in every
 * mode but K_XLATE, where Linux gives it only in K_UNICODE, and KDSKBENT
 * takes one in every mode, where Linux takes it only in K_UNICODE.
 */
int32_t kd_ioctl(uint32_t request, uintptr_t arg);

/* What the console's keyboard and terminal settings are while one owner of
 * the screen has them, kept when the screen passes to another. */
struct kd_state {
    int kbmode;              /* K_RAW, K_XLATE, K_MEDIUMRAW, K_UNICODE or K_OFF */
    int meta;                /* K_METABIT or K_ESCPREFIX */
    unsigned int locks;      /* the KT_LOCK locks in force, a bit for each KG_* */
    unsigned int slocks;     /* the KT_SLOCK sticky locks pending, likewise */
    unsigned int ledflags;   /* KDGKBLED's lock flags and defaults */
    unsigned int leds;       /* the LEDs set by KDSETLED, 0xFF when they follow the flags */
    int repeat_delay;        /* milliseconds */
    int repeat_period;       /* milliseconds, 0 for no repeat */
    struct ktermios termios; /* the console's terminal settings */
};

/**
 * Store in `st` the console's keyboard mode, meta setting, locks, LEDs and
 * repeat times and its terminal settings, for an owner of the screen that
 * gives it to another. Process context.
 */
void kd_state_save(struct kd_state *st);

/**
 * Put back the state `kd_state_save` stored, for an owner of the screen that
 * has it back: the terminal settings, then the keyboard's meta setting,
 * locks and repeat, the LEDs lit again, and last its mode, which as
 * KDSKBMODE does stops a held key's repeat and discards the console's unread
 * input. Process context.
 */
void kd_state_restore(const struct kd_state *st);

#endif
