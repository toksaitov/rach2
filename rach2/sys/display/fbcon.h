/*
 * fbcon.h: the text console on the framebuffer the loader set up, which a
 * machine booted by UEFI always has and one booted by BIOS has when its
 * loader was asked for one.
 *
 * The console's driver, vga_text.c, keeps the scrollback and writes the
 * screen as a text adapter shows it: cells, a character and its colors, in
 * the adapter's memory, a start address that selects the cells on the
 * screen, and a cursor. On a framebuffer fbcon.c keeps those three in place
 * of the adapter and draws what they select: as many cells as the screen
 * holds of vga_font.c's glyphs, eight pixels by sixteen, at the largest
 * whole scale that still gives 80 columns and 24 rows, centered. It draws a
 * cell only when it shows something other than what it showed, since every
 * pixel is a store to the display's memory. It also draws the picture of
 * /fb, which has the screen while a program holds it.
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
 * Take the loader's framebuffer `fb` as the console's screen and clear it,
 * with as many cells as it holds of the font at the largest whole scale that
 * still gives 80 columns and 24 rows, `textcon_cols` and `textcon_rows`, at
 * most `TEXTCON_COLS_MAX` by `TEXTCON_ROWS_MAX`: 85x24 at twice the font's
 * size on a 1366x768 screen. The log records the screen and the cells.
 * Returns 0, or -1, and the console stays on the text adapter, when the
 * framebuffer is neither direct color at 8 to 32 bits a pixel nor indexed
 * color at 8, holds fewer than 80 columns and 24 rows, has a pitch shorter
 * than a row of pixels, or cannot be reached (`fbcon_window`).
 */
int fbcon_init(const struct boot_framebuffer *fb);

/** Draw cell `at` of the memory, if it is on the screen and shows
 * something new; nothing while the picture of /fb has the screen. */
void fbcon_stored(uint32_t at);

/** Start the screen at cell `cell` of the memory, drawing the cells that
 * show something new. */
void fbcon_set_start(uint32_t cell);

/** Put the cursor, two lines at the foot of a cell, at cell `cell` of the
 * memory. */
void fbcon_set_cursor(uint32_t cell);

/**
 * Give the screen to the 320x200 picture of Mode 13h a program draws
 * through /fb (vga_gfx.c), the screen painted black: the cells are not drawn
 * while the picture has it. The picture is drawn at the largest whole
 * number of the screen's pixels to one of its own that fits, three on a
 * 1366x768 screen, in the middle, so that each of its pixels is a sharp
 * square; its colors are the VGA's sixteen until `fbcon_gfx_palette`.
 */
void fbcon_gfx_begin(void);

/** Take the 256 colors `rgb`, 8-bit red, green and blue each, as the
 * picture's palette, for the frames drawn after. */
void fbcon_gfx_palette(const uint8_t *rgb);

/** Draw the picture `pixels`, 320x200 palette indices, on the screen. */
void fbcon_gfx_frame(const uint8_t *pixels);

/** Give the screen back to the cells: painted black, and every cell drawn
 * again, those written while the picture had the screen among them. */
void fbcon_gfx_end(void);

/**
 * The kernel's address of the framebuffer's first byte now, or NULL when
 * the framebuffer cannot be reached. The platform defines it
 * (arch/x86/boot.c), since the address changes when the kernel turns its
 * own page tables on.
 */
volatile uint8_t *fbcon_window(void);

#endif /* _FBCON_H_ */
