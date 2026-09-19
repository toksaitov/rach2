/*
 * VGA text mode (Mode 3) driver: text console operations for the 80x25
 * display, with colors, cursor positioning, scrolling, and screen clearing.
 */

#ifndef _VGA_TEXT_H_
#define _VGA_TEXT_H_

#include "../lib/kint.h"
#include "../lib/kdef.h"

/*
 * VGA Text Mode constants (Mode 3: 80x25, 16 colors)
 */
#define VGA_TEXT_COLS 80
#define VGA_TEXT_ROWS 25
/* A host test stands its own memory in for the adapter's. */
#ifndef VGA_TEXT_BUFFER
#define VGA_TEXT_BUFFER 0xB8000
#endif

#define VGA_TEXT_DEFAULT_FG 0x07 /* Light grey */
#define VGA_TEXT_DEFAULT_BG 0x00 /* Black */

/*
 * Aliases for the constants above.
 */
#define SCREEN_NUMBER_OF_COLUMNS VGA_TEXT_COLS
#define SCREEN_NUMBER_OF_ROWS    VGA_TEXT_ROWS
#define VIDEO_COLOR_TEXT_BUFFER  VGA_TEXT_BUFFER
#define DEFAULT_FORECOLOR        VGA_TEXT_DEFAULT_FG
#define DEFAULT_BACKCOLOR        VGA_TEXT_DEFAULT_BG

/*
 * VGA text mode color palette (4-bit values)
 */
typedef enum display_color {
    black,
    blue,
    green,
    cyan,
    red,
    magenta,
    brown,
    light_grey,
    dark_grey,
    light_blue,
    light_green,
    light_cyan,
    light_red,
    light_magenta,
    light_brown,
    white
} display_color_t;

/*
 * Cursor position (character coordinates)
 */
extern unsigned int x;
extern unsigned int y;

/** Scroll the live area down by one line. */
void k_scroll_down(void);

/*
 * Clear the screen with the current background color and reset the cursor
 * to (0, 0).
 */
void k_cls(void);

/** Write one character cell at (col, row) of the live area. */
void vga_text_write_cell(unsigned int col, unsigned int row, char c, uint8_t attr);

/** Blit the current viewport to the adapter's memory and place the cursor. */
void vga_text_sync(void);

/** Place the hardware cursor where the next byte will land. */
void vga_text_place_cursor(void);

/** Hold the cursor across a write of many bytes; holds nest. */
void vga_text_hold(void);

/** Release a hold, placing the cursor if it moved while held. */
void vga_text_release(void);

/*
 * Set the foreground (text) color for subsequent output.
 */
void set_fore_color(display_color_t color);

/*
 * Set the background color for subsequent output.
 */
void set_back_color(display_color_t color);

/*
 * Return the combined color attribute byte: (background << 4) | foreground.
 */
uint8_t get_curr_color_attr(void);

/*
 * VGA text scrollback controls.
 */
void k_scrollback_page_up(void);
void k_scrollback_page_down(void);
void k_scrollback_home(void);
void k_scrollback_end(void);
bool k_scrollback_is_active(void);

#endif /* _VGA_TEXT_H_ */
