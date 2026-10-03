#ifndef _KBD_H_
#define _KBD_H_

#include "arch/x86/isr.h"
#include "kio.h"

#include "lib/kint.h"
#include "lib/kdef.h"

/* US layout, scancode to ASCII; the shift table has the shifted glyphs. */
extern uint8_t EN_us[128];
extern uint8_t EN_us_shift[128];

/*
 * Raw scancode ring for graphics-mode readers, filled by the input layer with
 * every code from either keyboard: the one on the 8042 and the one on USB.
 */

/** Push a scancode to the ring; dropped if the ring is full. */
void kbd_push_scancode(uint8_t sc);

/** Empty the scancode ring; call when entering graphics mode. */
void kbd_clear_scancodes(void);

/** Read up to `max_count` scancodes into `buf`; returns the count (0 if
 * empty). */
int kbd_read_scancodes(uint8_t *buf, int max_count);

#endif
