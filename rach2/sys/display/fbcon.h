/*
 * fbcon: the text console on the framebuffer the loader set up, which a
 * machine booted by UEFI always has and one booted by BIOS has when its
 * loader was asked for one.
 *
 * The console's driver, vga_text.c, keeps the scrollback and writes the
 * screen as a text adapter shows it: cells, a character and its colors, in
 * the adapter's memory, a start address that selects the cells on the
 * screen, and a cursor. On a framebuffer this file keeps those three in
 * place of the adapter and draws what they select: the 80x25 cells,
 * centered, each glyph of vga_font.c eight pixels by sixteen, or twice
 * that on a screen 2000 pixels wide or more. It draws a cell only when it
 * shows something other than what it showed, since every pixel is a store
 * to the display's memory.
 */

#ifndef _FBCON_H_
#define _FBCON_H_

#include "../boot.h"
#include "../lib/kdef.h"
#include "../lib/kint.h"

/* The cells of a text adapter's memory, 32 KiB, which vga_text.c moves the
 * screen's window through. */
#define FBCON_CELLS 16384u

/* Whether the console is on a framebuffer. */
extern bool fbcon_active;

/* The adapter's memory, as vga_text.c writes it. */
extern uint16_t fbcon_cells[FBCON_CELLS];

/**
 * Take the loader's framebuffer `fb` as the console's screen and clear it.
 * Returns 0, or -1 when it is a kind this console cannot draw or too small
 * for the 80x25 cells, and the console stays on the text adapter.
 */
int fbcon_init(const struct boot_framebuffer *fb);

/** Draw cell `at` of the memory, if it is on the screen and shows
 * something new. */
void fbcon_stored(uint32_t at);

/** Start the screen at cell `cell` of the memory, drawing the cells that
 * show something new. */
void fbcon_set_start(uint32_t cell);

/** Put the cursor, two lines at the foot of a cell, at cell `cell` of the
 * memory. */
void fbcon_set_cursor(uint32_t cell);

/**
 * The kernel's address of the framebuffer's first byte now, or NULL when
 * the framebuffer cannot be reached. The platform defines it (x86/boot.c),
 * since the address changes when the kernel turns its own page tables on.
 */
volatile uint8_t *fbcon_window(void);

#endif /* _FBCON_H_ */
