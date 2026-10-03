/*
 * VGA text mode (Mode 3) driver: the text console of textcon.h on the
 * adapter's 80x25 text mode, its memory at VGA_TEXT_BUFFER.
 */

#ifndef _VGA_TEXT_H_
#define _VGA_TEXT_H_

#include "textcon.h"

#include "../lib/kint.h"
#include "../lib/kdef.h"

/*
 * VGA Text Mode constants (Mode 3: 80x25, 16 colors)
 */
#define VGA_TEXT_COLS 80
#define VGA_TEXT_ROWS 25
/* The kernel's address of the adapter's memory at physical 0xB8000
 * (arch.h); a host test stands its own memory in for it. */
#ifndef VGA_TEXT_BUFFER
#define VGA_TEXT_BUFFER phys_to_virt(0xB8000)
#endif

#define VGA_TEXT_DEFAULT_FG 0x07 /* Light grey */
#define VGA_TEXT_DEFAULT_BG 0x00 /* Black */

/*
 * VGA_TEXT_BUFFER aliased, for the code that addresses the adapter's memory.
 */
#define VIDEO_COLOR_TEXT_BUFFER VGA_TEXT_BUFFER

#endif /* _VGA_TEXT_H_ */
