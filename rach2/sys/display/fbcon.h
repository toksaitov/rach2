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
 * /fb's mode 13h, and gives the screen to a program that writes it at its
 * native mode, while a program holds /fb. The screen is the loader's
 * framebuffer, or the buffer a DRM device scans out for the console
 * (sys/drm/drm_client.c), and while a DRM master has the screen the console
 * draws nothing.
 */

#ifndef _FBCON_H_
#define _FBCON_H_

#include "../boot.h"
#include "../lib/kdef.h"
#include "../lib/kint.h"

struct fb_screen;

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
 * Returns 0, or -1, and the console stays on the text adapter with its
 * cells unchanged, when the framebuffer is neither direct color at 8 to 32
 * bits a pixel nor indexed color at 8, holds fewer than 80 columns and 24
 * rows, has a pitch shorter than a row of pixels, or cannot be reached
 * (`fbcon_window`).
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
 * while the picture has it. The picture fills the screen's height, or its
 * width on a screen narrower than 16:10, at its own 320:200, in the middle:
 * 1228x768 from (69, 0) on a 1366x768 screen. Each of its pixels covers a
 * whole number of the screen's, which differs by one from pixel to pixel,
 * three or four on that screen. Its colors are the VGA's sixteen until
 * `fbcon_gfx_palette`. From the native mode it goes straight to the picture.
 */
void fbcon_gfx_begin(void);

/** Take the 256 colors `rgb`, 8-bit red, green and blue each, as the
 * picture's palette, for the frames drawn after. */
void fbcon_gfx_palette(const uint8_t *rgb);

/** Draw the picture `pixels`, 320x200 palette indices, on the screen;
 * nothing at the native mode. */
void fbcon_gfx_frame(const uint8_t *pixels);

/** Describe the screen in `*s`, its size, pitch and pixel format, as
 * FB_GETINFO reports it; 0, or -1 when the console is on no framebuffer. */
int fbcon_screen(struct fb_screen *s);

/**
 * Give the whole screen, painted black, to a program that writes its pixels
 * at the screen's own format, the native mode of /fb: no cell and no picture
 * is drawn until `fbcon_gfx_end`. Returns the screen's first byte, the
 * console's DRM buffer or the loader's framebuffer as `fbcon_window` gives
 * it, or NULL, with nothing changed, when the console is on no framebuffer,
 * its screen is gone (`fbcon_detach`), or the screen is not RGB at 15, 16,
 * 24 or 32 bits a pixel. A detached console leaves the screen unpainted.
 * From mode 13h's picture it goes straight to the native mode.
 */
volatile uint8_t *fbcon_native_begin(void);

/** Give the screen back to the cells, from the picture or the native mode:
 * painted black, and every cell drawn again, those written while the
 * program had the screen among them. */
void fbcon_gfx_end(void);

/** Paint the screen black and draw no cell while `blank` is set; when it
 * is clear, draw every cell again, as `fbcon_gfx_end` does. */
void fbcon_blank(int blank);

/**
 * Stop drawing, for a DRM master that takes the screen: the cells, the
 * cursor, the blank and the modes of /fb go on changing, and nothing is drawn
 * until `fbcon_attach`. With `forget` set the screen is gone, its device
 * removed or its memory moved, and `fbcon_native_begin` returns NULL until
 * then, so that /fb's native mode cannot write where it was. The caller
 * holds the console's lock.
 */
void fbcon_detach(int forget);

/**
 * Draw on the screen `fb` from now on, at the kernel's address `base`, or
 * at the loader's framebuffer (`fbcon_window`) for NULL, and draw it whole:
 * every cell, or, while a program holds /fb, the screen cleared for
 * `fb_redraw` (fb.h) to draw the program's mode again. The columns and rows
 * stay as they are, at the largest whole scale of the font at which they
 * fit on `fb`, centered; a screen too small for them at scale 1 shows the
 * cells from the top left corner that fit. A console on the VGA's text mode
 * becomes a framebuffer console here, its text drawn again from the
 * scrollback. Returns 0, or -1 with nothing changed for a screen that is
 * neither direct color at 8 to 32 bits a pixel nor indexed color at 8, or
 * one that cannot be reached. The caller holds the console's lock.
 */
int fbcon_attach(const struct boot_framebuffer *fb, volatile uint8_t *base);

/** Add the bytes from `offset` to `offset + len` of the screen, which /fb's
 * native mode wrote, to the damage. The caller holds the console's lock. */
void fbcon_damage(uint32_t offset, uint32_t len);

/**
 * Take the rows of pixels stored to since the last call, for a console
 * whose screen is a shadow that must be copied to the display: 1 with the
 * rows from `*top` up to `*end`, or 0 when none changed. Every store of
 * fbcon counts, and /fb's through `fbcon_damage`. The caller holds the
 * console's lock.
 */
int fbcon_take_damage(uint32_t *top, uint32_t *end);

/** The font's scale on the screen, 0 on the VGA's text mode; for the log. */
uint32_t fbcon_scale(void);

/**
 * The kernel's address of the framebuffer's first byte now, or NULL when
 * the framebuffer cannot be reached. The platform defines it
 * (arch/x86/boot.c), since the address changes when the kernel turns its
 * own page tables on.
 */
volatile uint8_t *fbcon_window(void);

/** The screen the loader described, as the boot record keeps it, for the
 * driver of the firmware's framebuffer (sys/drm/fbdrm.c). The platform
 * defines it (arch/x86/boot.c). */
const struct boot_framebuffer *fbcon_boot_screen(void);

#endif /* _FBCON_H_ */
