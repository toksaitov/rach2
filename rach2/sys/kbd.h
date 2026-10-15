/*
 * kbd.h: the US keymaps, and the ring of raw scancodes a program that holds
 * /fb reads its keys from.
 */
#ifndef _KBD_H_
#define _KBD_H_

#include "kio.h"

#include "lib/kint.h"
#include "lib/kdef.h"

/* US layout, set-1 scancode to ASCII, 0 for a key with no character; the
 * shift table has the shifted glyphs. */
extern uint8_t EN_us[128];
extern uint8_t EN_us_shift[128];

/*
 * The ring holds 63 codes, filled by the input layer with every code from
 * either keyboard, the one on the 8042 and the one on USB, from interrupt
 * context. Its one reader is the process that holds /fb, through read(2) on
 * the console. Neither side takes a lock.
 */

/** Push a scancode to the ring; dropped if the ring is full. */
void kbd_push_scancode(uint8_t sc);

/** Empty the scancode ring, when a process takes /fb. */
void kbd_clear_scancodes(void);

/** Read up to `max_count` scancodes into `buf` without waiting; returns the
 * count (0 if empty). */
int kbd_read_scancodes(uint8_t *buf, int max_count);

#endif
