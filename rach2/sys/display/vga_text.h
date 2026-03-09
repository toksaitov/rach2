/*
 * vga_text.h - VGA Text Mode (Mode 3) driver
 *
 * Provides text mode console operations for the 80x25 VGA display.
 * Handles colors, cursor positioning, scrolling, and screen clearing.
 */

#ifndef _VGA_TEXT_H_
#define _VGA_TEXT_H_

#include "../lib/kint.h"
#include "../lib/kdef.h"

/*
 * VGA Text Mode constants (Mode 3: 80x25, 16 colors)
 */
#define VGA_TEXT_COLS           80
#define VGA_TEXT_ROWS           25
#define VGA_TEXT_BUFFER         0xB8000

#define VGA_TEXT_DEFAULT_FG     0x07    /* Light grey */
#define VGA_TEXT_DEFAULT_BG     0x00    /* Black */

/*
 * Legacy macro names for compatibility
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

/*
 * Scroll the screen content up by one line.
 * Preserves scrollback history and advances the live viewport.
 */
void k_scroll_down(void);

/*
 * Clear the entire screen with the current background color.
 * Resets cursor position to (0, 0).
 */
void k_cls(void);

/*
 * Write a character cell into the live 80x25 text surface.
 * Coordinates are relative to the live screen, not scrollback history.
 */
void vga_text_write_cell(unsigned int col, unsigned int row, char c, uint8_t attr);

/*
 * Re-render the visible viewport and update the hardware cursor.
 */
void vga_text_sync(void);

/*
 * Set the foreground (text) color for subsequent output.
 */
void set_fore_color(display_color_t color);

/*
 * Set the background color for subsequent output.
 */
void set_back_color(display_color_t color);

/*
 * Get the combined color attribute byte for VGA text mode.
 * Format: (background << 4) | foreground
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
