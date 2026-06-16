/*
 * VGA text mode (Mode 3) driver.
 *
 * The visible screen is a window onto a ring of scrollback lines. Each line
 * is VGA_TEXT_COLS cells, a cell is a character byte plus an attribute byte.
 * Lines are numbered by a monotonic logical index; `_history` indexes that
 * number modulo VGA_TEXT_SCROLLBACK_LINES, so the oldest line is overwritten
 * once the ring fills. `_live_top_line` is the logical line at the top of the
 * live (bottom) area where new text lands; `_viewport_top_line` is the line at
 * the top of what is currently displayed. They differ while the user scrolls
 * back through history; `_follow_live` is true when the viewport tracks live
 * output. vga_text_sync() blits the viewport's lines to the hardware buffer.
 */

#include "vga_text.h"
#include "../kio.h"

#define VGA_TEXT_SCROLLBACK_LINES 512
#define VGA_TEXT_BLANK_CHAR       ' '

unsigned int x;
unsigned int y;

static display_color_t _fore_color = VGA_TEXT_DEFAULT_FG;
static display_color_t _back_color = VGA_TEXT_DEFAULT_BG;

static uint16_t _history[VGA_TEXT_SCROLLBACK_LINES][VGA_TEXT_COLS];
static uint32_t _history_start_line = 0;
static uint32_t _history_line_count = 0;
static uint32_t _live_top_line = 0;
static uint32_t _viewport_top_line = 0;
static bool _follow_live = true;

static uint16_t vga_text_make_cell(char c, uint8_t attr)
{
    return (uint16_t)((uint8_t)c | ((uint16_t)attr << 8));
}

static uint16_t vga_text_blank_cell(void)
{
    return vga_text_make_cell(VGA_TEXT_BLANK_CHAR, get_curr_color_attr());
}

static uint32_t vga_text_last_line(void)
{
    if (_history_line_count == 0) {
        return 0;
    }

    return _history_start_line + _history_line_count - 1;
}

static bool vga_text_has_line(uint32_t line)
{
    if (_history_line_count == 0) {
        return false;
    }

    return line >= _history_start_line && line <= vga_text_last_line();
}

static uint16_t *vga_text_line_ptr(uint32_t line)
{
    uint32_t idx;

    if (!vga_text_has_line(line)) {
        return (uint16_t *)0;
    }

    idx = line % VGA_TEXT_SCROLLBACK_LINES;
    return _history[idx];
}

static void vga_text_clear_line(uint32_t line)
{
    uint16_t *cells;
    uint16_t blank;
    unsigned int col;

    cells = vga_text_line_ptr(line);
    if (!cells) {
        return;
    }

    blank = vga_text_blank_cell();
    for (col = 0; col < VGA_TEXT_COLS; ++col) {
        cells[col] = blank;
    }
}

/**
 * Append one blank line to the bottom of the scrollback history.
 *
 * While the ring has room the line count grows. Once it is full the oldest
 * line is evicted by advancing `_history_start_line`, so the new line reuses
 * the freed slot. If the viewport pointed at the evicted line it is dragged
 * forward to the new oldest line.
 */
static void vga_text_append_blank_line(void)
{
    uint32_t line;

    line = _history_start_line + _history_line_count;
    if (_history_line_count == VGA_TEXT_SCROLLBACK_LINES) {
        _history_start_line++;
        line = _history_start_line + _history_line_count - 1;
    } else {
        _history_line_count++;
    }

    vga_text_clear_line(line);

    if (_viewport_top_line < _history_start_line) {
        _viewport_top_line = _history_start_line;
    }
}

static void vga_text_reset_history(void)
{
    uint32_t i;

    _history_start_line = 0;
    _history_line_count = 0;
    _live_top_line = 0;
    _viewport_top_line = 0;
    _follow_live = true;

    for (i = 0; i < VGA_TEXT_ROWS; ++i) {
        vga_text_append_blank_line();
    }
}

/**
 * Move the hardware cursor to follow (x, y) within the live area.
 *
 * The cursor sits at (x, y) of the live area while following live output.
 * When scrolled back it is placed at the corresponding cell only if that cell
 * is on screen; otherwise it is parked in the last cell so it stays visible
 * but out of the way.
 */
static void vga_text_update_cursor(void)
{
    uint32_t cursor_line = _live_top_line + y;
    uint16_t cursor_pos;

    if (_follow_live) {
        cursor_pos = (uint16_t)(y * VGA_TEXT_COLS + x);
    } else if (cursor_line >= _viewport_top_line &&
               cursor_line < _viewport_top_line + VGA_TEXT_ROWS) {
        cursor_pos = (uint16_t)((cursor_line - _viewport_top_line) * VGA_TEXT_COLS + x);
    } else {
        cursor_pos = (uint16_t)(VGA_TEXT_ROWS * VGA_TEXT_COLS - 1);
    }

    /* CRTC port 0x3D4 selects a register, 0x3D5 reads/writes it; cursor
       location is the 16-bit value in registers 0xE (high) and 0xF (low). */
    outb(0x3D4, 0xE);
    outb(0x3D5, cursor_pos >> 8);
    outb(0x3D4, 0xF);
    outb(0x3D5, cursor_pos);
}

/**
 * Blit the current viewport to the VGA text buffer and update the cursor.
 *
 * Each on-screen row is filled from its scrollback line; rows mapping to lines
 * that do not exist (above the start of history) are filled with blanks. Call
 * this after any change to the history or viewport pointers to make it visible.
 */
void vga_text_sync(void)
{
    volatile uint16_t *video_memory;
    uint32_t line;
    unsigned int row;
    unsigned int col;

    video_memory = (volatile uint16_t *)VGA_TEXT_BUFFER;
    for (row = 0; row < VGA_TEXT_ROWS; ++row) {
        line = _viewport_top_line + row;

        if (vga_text_has_line(line)) {
            uint16_t *cells = vga_text_line_ptr(line);
            for (col = 0; col < VGA_TEXT_COLS; ++col) {
                video_memory[row * VGA_TEXT_COLS + col] = cells[col];
            }
        } else {
            uint16_t blank = vga_text_blank_cell();
            for (col = 0; col < VGA_TEXT_COLS; ++col) {
                video_memory[row * VGA_TEXT_COLS + col] = blank;
            }
        }
    }

    vga_text_update_cursor();
}

void k_cls(void)
{
    x = 0;
    y = 0;
    vga_text_reset_history();
    vga_text_sync();
}

/**
 * Scroll the live area down by one line.
 *
 * Appends a fresh blank line and advances the live top. The viewport follows
 * only when tracking live output; a user scrolled back into history stays put,
 * except that it is pulled forward if its line has just been evicted.
 */
void k_scroll_down(void)
{
    vga_text_append_blank_line();
    _live_top_line++;

    if (_follow_live) {
        _viewport_top_line = _live_top_line;
    } else if (_viewport_top_line < _history_start_line) {
        _viewport_top_line = _history_start_line;
    }

    vga_text_sync();
}

/**
 * Write one character cell at (col, row) of the live area.
 *
 * Coordinates are relative to the live top, not the viewport. Out-of-range
 * coordinates and missing history lines are ignored. The write lands in the
 * history ring; vga_text_sync() is needed to make it visible.
 */
void vga_text_write_cell(unsigned int col, unsigned int row, char c, uint8_t attr)
{
    uint32_t line = _live_top_line + row;
    uint16_t *cells;

    if (col >= VGA_TEXT_COLS || row >= VGA_TEXT_ROWS) {
        return;
    }

    if (!vga_text_has_line(line)) {
        return;
    }

    cells = vga_text_line_ptr(line);
    cells[col] = vga_text_make_cell(c, attr);
}

void set_fore_color(display_color_t color)
{
    _fore_color = color;
}

void set_back_color(display_color_t color)
{
    _back_color = color;
}

uint8_t get_curr_color_attr(void)
{
    return (uint8_t)((_fore_color & 0xF) | (_back_color << 4));
}

void k_scrollback_page_up(void)
{
    if (_viewport_top_line > _history_start_line + VGA_TEXT_ROWS) {
        _viewport_top_line -= VGA_TEXT_ROWS;
    } else {
        _viewport_top_line = _history_start_line;
    }

    _follow_live = (_viewport_top_line == _live_top_line);
    vga_text_sync();
}

void k_scrollback_page_down(void)
{
    if (_viewport_top_line + VGA_TEXT_ROWS < _live_top_line) {
        _viewport_top_line += VGA_TEXT_ROWS;
        _follow_live = false;
    } else {
        _viewport_top_line = _live_top_line;
        _follow_live = true;
    }

    vga_text_sync();
}

void k_scrollback_home(void)
{
    _viewport_top_line = _history_start_line;
    _follow_live = (_viewport_top_line == _live_top_line);
    vga_text_sync();
}

void k_scrollback_end(void)
{
    _viewport_top_line = _live_top_line;
    _follow_live = true;
    vga_text_sync();
}

bool k_scrollback_is_active(void)
{
    return !_follow_live;
}
