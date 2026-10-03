/*
 * The text console, as the shared kernel draws on it: a grid of character
 * cells with colors, a cursor, scrolling and a scrollback. A platform's
 * console driver defines it: display/vga_text.c on a PC, in the adapter's
 * text mode.
 */

#ifndef _TEXTCON_H_
#define _TEXTCON_H_

#include "../lib/kint.h"
#include "../lib/kdef.h"

/* The screen's size in character cells, and the colors a cleared cell and
 * new text take. */
#define SCREEN_NUMBER_OF_COLUMNS 80
#define SCREEN_NUMBER_OF_ROWS    25
#define DEFAULT_FORECOLOR        0x07 /* Light grey */
#define DEFAULT_BACKCOLOR        0x00 /* Black */

/*
 * The sixteen colors of a cell, a VGA attribute's four-bit values, which the
 * escape sequences of the console's terminal entry select.
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
void textcon_write_cell(unsigned int col, unsigned int row, char c, uint8_t attr);

/** Blit the current viewport to the adapter's memory and place the cursor. */
void textcon_sync(void);

/** Place the hardware cursor where the next byte will land. */
void textcon_place_cursor(void);

/** Hold the cursor across a write of many bytes; holds nest. */
void textcon_hold(void);

/** Release a hold, placing the cursor if it moved while held. */
void textcon_release(void);

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
 * The scrollback: the rows that left the top of the screen, which these move
 * through and back to the live screen.
 */
void k_scrollback_page_up(void);
void k_scrollback_page_down(void);
void k_scrollback_home(void);
void k_scrollback_end(void);
bool k_scrollback_is_active(void);

#endif /* _TEXTCON_H_ */
