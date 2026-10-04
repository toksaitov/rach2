/*
 * The text console, as the shared kernel draws on it: a grid of character
 * cells with colors, a cursor, scrolling and a scrollback. A platform's
 * console driver defines it: display/vga_text.c on a PC, in the adapter's
 * text mode or, through display/fbcon.c, on the loader's framebuffer.
 */

#ifndef _TEXTCON_H_
#define _TEXTCON_H_

#include "../lib/kint.h"
#include "../lib/kdef.h"

/* The screen's size in character cells, and the colors a cleared cell and
 * new text take. */
/* The console's columns and rows: the VGA's 80 by 25 in text mode, and on a
 * framebuffer as many as its screen holds at the font's scale (fbcon.c),
 * within the bounds below, whose cells and a line more fit the 16384 cells
 * of the adapter's memory the console scrolls through. */
extern unsigned int textcon_cols;
extern unsigned int textcon_rows;
#define TEXTCON_COLS_MAX  240u
#define TEXTCON_ROWS_MAX  64u
#define DEFAULT_FORECOLOR 0x07 /* Light grey */
#define DEFAULT_BACKCOLOR 0x00 /* Black */

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

/**
 * Load the console's font, with the Cyrillic letters, into the display and
 * return its map from code page 437's upper half to the glyphs it now has,
 * or NULL when the display keeps its own font.
 */
const uint16_t *textcon_load_font(void);

/** Whether the console draws its font on a framebuffer rather than in a
 * text adapter. */
bool textcon_on_framebuffer(void);

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
