#ifndef _KBD_H_
#define _KBD_H_

#include "isr.h"
#include "kio.h"

#include "lib/kint.h"
#include "lib/kdef.h"

extern uint8_t EN_us[128];
extern uint8_t EN_us_shift[128];

void init_keyboard(void);

/*
 * Raw scancode buffer for games/graphics mode
 */

/* Push a scancode to the buffer (called from keyboard interrupt handler) */
void kbd_push_scancode(uint8_t sc);

/* Clear the scancode buffer (call when entering graphics mode) */
void kbd_clear_scancodes(void);

/* Returns number of scancodes read (0 if empty) */
int kbd_read_scancodes(uint8_t *buf, int max_count);

#endif
