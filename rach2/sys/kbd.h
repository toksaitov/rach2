/*
 * kbd.h: the map between set-1 codes, which a PS/2 keyboard sends and the
 * console's raw view gives, and Linux's key codes, which name every key.
 */
#ifndef _KBD_H_
#define _KBD_H_

#include "kio.h"

#include "lib/kint.h"
#include "lib/kdef.h"

/**
 * Linux's key code, a KEY_* of <linux/input-event-codes.h>, for the set-1
 * code `scan` with its release bit clear and 0x80 set when an E0 prefix came
 * before it, which is how Linux's keyboard driver numbers a translated
 * keyboard's keys and the MSC_SCAN it reports: 0x1E is A, 0xB0 is Volume
 * Up. 0 for a code that names no key.
 */
int set1_keycode(unsigned scan);

/**
 * The set-1 code of Linux's key `keycode`, as `set1_keycode` takes it: 0x80
 * set for a key sent after an E0 prefix, and 0 for a key no set-1 code
 * names. Of the two codes of KEY_SYSRQ it gives Print Screen's, E0 37.
 */
unsigned set1_scancode(int keycode);

/** Set in `keys`, a set of EVDEV_KEY_BYTES bytes, the bit of every key
 * `set1_keycode` names, for a keyboard's registration. */
void set1_keys(uint8_t *keys);

#endif
