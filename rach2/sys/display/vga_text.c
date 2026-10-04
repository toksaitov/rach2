/*
 * VGA text mode (Mode 3) driver.
 *
 * The visible screen is a window onto a ring of scrollback lines. Each line is
 * `textcon_cols` cells, a cell is a character byte plus an attribute byte; the
 * screen is `textcon_rows` lines, 80 by 25 on the VGA and what fbcon.c finds
 * the screen holds on a framebuffer. Lines are numbered by a monotonic logical
 * index; `_history` indexes that number modulo VGA_TEXT_SCROLLBACK_LINES, so
 * the oldest line is overwritten once the ring fills. `_live_top_line` is the
 * logical line at the top of the live (bottom) area where new text lands;
 * `_viewport_top_line` is the line at the top of what is currently displayed.
 * They differ while the user scrolls back through history; `_follow_live` is
 * true when the viewport tracks live output.
 *
 * The adapter's memory is slow: every cell written to it is an uncached bus
 * cycle on hardware and a trapped access under a hypervisor, microseconds where
 * a store to memory is nanoseconds, and a screen of the VGA's is two thousand
 * cells. So the driver writes each cell once. A byte of live output goes to its
 * one cell, and a scroll moves the display's start address through the 32 KiB
 * the adapter maps at VGA_TEXT_BUFFER, which holds eight of the VGA's screens,
 * and writes only the new blank line; the rows that stay are copied to the
 * front of that memory when the window would pass its end, once every 180
 * scrolls on the VGA. `_hw_top` is the cell the display starts at. Only a
 * viewport that moves, a scroll back through history or a clear, is blitted
 * whole by textcon_sync(). Placing the cursor takes four port writes, so a
 * write of many bytes holds it and places it once at the end. While the adapter
 * is in a graphics mode the start address is left alone, since it is the
 * picture's then; leaving graphics mode clears the screen, which sets it again.
 *
 * On a framebuffer the memory, the start address and the cursor are fbcon.c's,
 * which draws what they select, and the economy is the same: a cell drawn is a
 * glyph's pixels stored to the display's memory.
 *
 * The caller keeps interrupts off around a byte's work here: k_putc does, and a
 * keyboard's echo comes through k_putc from its interrupt.
 */

#include "vga_font.h"
#include "../arch/arch.h"
#include "../arch/x86/portio.h"
#include "fbcon.h"
#include "vga_text.h"
#include "vga_gfx.h"
#include "../kio.h"
#include "../lib/kirq.h"

#define VGA_TEXT_SCROLLBACK_LINES 512
#define VGA_TEXT_BLANK_CHAR       ' '

/* The cells the adapter maps at VGA_TEXT_BUFFER, 32 KiB of two-byte cells,
 * and the cells of one screen. */
#define VGA_TEXT_CELLS        16384u
#define VGA_TEXT_SCREEN_CELLS (textcon_rows * textcon_cols)

unsigned int x;
unsigned int y;
unsigned int textcon_cols = VGA_TEXT_COLS;
unsigned int textcon_rows = VGA_TEXT_ROWS;

static display_color_t _fore_color = VGA_TEXT_DEFAULT_FG;
static display_color_t _back_color = VGA_TEXT_DEFAULT_BG;

static uint16_t _history[VGA_TEXT_SCROLLBACK_LINES][TEXTCON_COLS_MAX];
static uint32_t _history_start_line = 0;
static uint32_t _history_line_count = 0;
static uint32_t _live_top_line = 0;
static uint32_t _viewport_top_line = 0;
static bool _follow_live = true;

/* The cell of the adapter's memory the display starts at, and whether the
 * start address has been written since boot. */
static uint32_t _hw_top = 0;
static bool _hw_started = false;

/* Writers holding the cursor, and whether it moved while held. */
static unsigned int _hold = 0;
static bool _cursor_stale = false;

static volatile uint16_t *vga_text_memory(void)
{
    if (fbcon_active) {
        return fbcon_cells;
    }
    return (volatile uint16_t *)VGA_TEXT_BUFFER;
}

/* One cell to the adapter's memory: the only store there, so a host test
   can count them. */
#ifdef VGA_TEXT_HOSTTEST
void vga_text_hosttest_stored(void);
#endif
static void vga_text_store(uint32_t at, uint16_t cell)
{
    vga_text_memory()[at] = cell;
#ifdef VGA_TEXT_HOSTTEST
    vga_text_hosttest_stored();
#endif
    if (fbcon_active) {
        fbcon_stored(at);
    }
}

/**
 * Point the display at cell `cell` of the adapter's memory.
 *
 * CRTC registers 0x0C and 0x0D hold the start address, high byte first, in
 * cells. The port at 0x3D4 selects the register and 0x3D5 writes it.
 */
static void vga_text_set_start(uint32_t cell)
{
    if (fbcon_active) {
        fbcon_set_start(cell);
        _hw_started = true;
        return;
    }
    if (vga_graphics_mode) {
        return;
    }
    outb(0x3D4, 0x0C);
    outb(0x3D5, (uint8_t)(cell >> 8));
    outb(0x3D4, 0x0D);
    outb(0x3D5, (uint8_t)cell);
    _hw_started = true;
}

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
    for (col = 0; col < textcon_cols; ++col) {
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

    for (i = 0; i < textcon_rows; ++i) {
        vga_text_append_blank_line();
    }
}

/**
 * Move the hardware cursor to follow (x, y) within the live area.
 *
 * The cursor is at (x, y) of the live area while following live output.
 * When scrolled back it is placed at the corresponding cell only if that cell
 * is on screen; otherwise it is parked at the bottom-right corner. The cursor
 * location is CRTC registers 0x0E (high byte) and 0x0F (low byte), selected
 * at port 0x3D4 and written at 0x3D5. They count cells from the start of the
 * adapter's memory, not from the display's start address, so `_hw_top` is
 * added.
 */
static void vga_text_update_cursor(void)
{
    uint32_t cursor_line = _live_top_line + y;
    uint16_t cursor_pos;

    if (_follow_live) {
        cursor_pos = (uint16_t)(_hw_top + y * textcon_cols + x);
    } else if (cursor_line >= _viewport_top_line &&
               cursor_line < _viewport_top_line + textcon_rows) {
        cursor_pos = (uint16_t)(_hw_top + (cursor_line - _viewport_top_line) * textcon_cols + x);
    } else {
        cursor_pos = (uint16_t)(_hw_top + VGA_TEXT_SCREEN_CELLS - 1);
    }

    _cursor_stale = false;
    if (fbcon_active) {
        fbcon_set_cursor(cursor_pos);
        return;
    }
    outb(0x3D4, 0xE);
    outb(0x3D5, cursor_pos >> 8);
    outb(0x3D4, 0xF);
    outb(0x3D5, cursor_pos);
}

/**
 * Place the hardware cursor where the next byte will land, or note that it
 * moved when a writer holds it.
 */
void textcon_place_cursor(void)
{
    if (_hold > 0) {
        _cursor_stale = true;
        return;
    }
    vga_text_update_cursor();
}

/**
 * Hold the cursor for a write of many bytes.
 *
 * Each byte of output moves the cursor, and placing it is four port writes;
 * a line placed once at the end costs the same as one byte placed on its own.
 * Holds nest, so a message an interrupt handler prints during a program's
 * write takes its own hold and the cursor is placed when the outermost writer
 * releases. An interrupt's hold and release are balanced, so a count it
 * interrupts between the load and the store is stored as it would have been,
 * and a cursor the interrupt moved is placed by the writer it interrupted.
 */
void textcon_hold(void)
{
    _hold++;
}

/** Release a hold, placing the cursor if it moved while held. */
void textcon_release(void)
{
    uint32_t flags = kirq_save();

    if (_hold > 0) {
        _hold--;
    }
    if (_hold == 0 && _cursor_stale) {
        vga_text_update_cursor();
    }
    kirq_restore(flags);
}

/**
 * Blit the current viewport to the adapter's memory and place the cursor.
 *
 * Each on-screen row is filled from its scrollback line; rows mapping to lines
 * that do not exist (above the start of history) are filled with blanks. The
 * rows go to the window at `_hw_top`, which the display is then pointed at.
 * This is the whole screen, two thousand cells on the VGA, so it is for a
 * viewport that moved: a scroll back through history, or a clear. Live output
 * does not come here.
 */
void textcon_sync(void)
{
    uint32_t line;
    unsigned int row;
    unsigned int col;
    uint16_t *cells;

    for (row = 0; row < textcon_rows; ++row) {
        line = _viewport_top_line + row;
        cells = vga_text_line_ptr(line);
        if (cells != NULL) {
            for (col = 0; col < textcon_cols; ++col) {
                vga_text_store(_hw_top + row * textcon_cols + col, cells[col]);
            }
        } else {
            uint16_t blank = vga_text_blank_cell();
            for (col = 0; col < textcon_cols; ++col) {
                vga_text_store(_hw_top + row * textcon_cols + col, blank);
            }
        }
    }

    vga_text_set_start(_hw_top);
    textcon_place_cursor();
}

/**
 * Scroll the display one line without rewriting it.
 *
 * The start address moves down a line, and the line that comes into view at
 * the bottom is written blank. When the window would pass the end of the
 * adapter's memory, the rows that stay are copied to its front first; that
 * copy is the one whole-screen cost, and on the VGA's 80 by 25 it comes once
 * every 180 scrolls, since the window moves 179 times before it would pass
 * the end. The source of the copy begins past the destination's end, so a
 * forward copy is safe.
 */
static void vga_text_scroll_hardware(void)
{
    volatile uint16_t *video_memory = vga_text_memory();
    uint16_t blank = vga_text_blank_cell();
    uint32_t i;

    if (_hw_top + VGA_TEXT_SCREEN_CELLS + textcon_cols > VGA_TEXT_CELLS) {
        for (i = 0; i < VGA_TEXT_SCREEN_CELLS - textcon_cols; i++) {
            vga_text_store(i, video_memory[_hw_top + textcon_cols + i]);
        }
        _hw_top = 0;
    } else {
        _hw_top += textcon_cols;
    }
    for (i = 0; i < textcon_cols; i++) {
        vga_text_store(_hw_top + VGA_TEXT_SCREEN_CELLS - textcon_cols + i, blank);
    }
    vga_text_set_start(_hw_top);
}

/** Clear the screen: the history starts over, and so does the window. */
void k_cls(void)
{
    x = 0;
    y = 0;
    _hw_top = 0;
    vga_text_reset_history();
    textcon_sync();
}

/**
 * Scroll the live area down by one line.
 *
 * Appends a fresh blank line and advances the live top. A viewport following
 * live output scrolls with the display's start address. A viewport scrolled
 * back into history stays where it is, unless the append evicted its top
 * line: the append then moves it to the new oldest line, and it is redrawn.
 */
void k_scroll_down(void)
{
    uint32_t viewport_was = _viewport_top_line;

    vga_text_append_blank_line();
    _live_top_line++;

    if (_follow_live) {
        _viewport_top_line = _live_top_line;
        vga_text_scroll_hardware();
    } else if (_viewport_top_line != viewport_was) {
        textcon_sync();
    }
}

/**
 * Write one character cell at (col, row) of the live area.
 *
 * Coordinates are relative to the live top, not the viewport. Out-of-range
 * coordinates and missing history lines are ignored. The write lands in the
 * history ring and, while the viewport follows live output, in the one cell
 * of the adapter's memory that shows it. The first such write points the
 * display at the front of that memory, where the firmware may not have left
 * it.
 */
void textcon_write_cell(unsigned int col, unsigned int row, char c, uint8_t attr)
{
    uint32_t line = _live_top_line + row;
    uint16_t *cells;
    uint16_t cell;

    if (col >= textcon_cols || row >= textcon_rows) {
        return;
    }

    if (!vga_text_has_line(line)) {
        return;
    }

    cell = vga_text_make_cell(c, attr);
    cells = vga_text_line_ptr(line);
    cells[col] = cell;
    if (_follow_live) {
        if (!_hw_started) {
            vga_text_set_start(_hw_top);
        }
        vga_text_store(_hw_top + row * textcon_cols + col, cell);
    }
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
    if (_viewport_top_line > _history_start_line + textcon_rows) {
        _viewport_top_line -= textcon_rows;
    } else {
        _viewport_top_line = _history_start_line;
    }

    _follow_live = (_viewport_top_line == _live_top_line);
    textcon_sync();
}

void k_scrollback_page_down(void)
{
    if (_viewport_top_line + textcon_rows < _live_top_line) {
        _viewport_top_line += textcon_rows;
        _follow_live = false;
    } else {
        _viewport_top_line = _live_top_line;
        _follow_live = true;
    }

    textcon_sync();
}

void k_scrollback_home(void)
{
    _viewport_top_line = _history_start_line;
    _follow_live = (_viewport_top_line == _live_top_line);
    textcon_sync();
}

void k_scrollback_end(void)
{
    _viewport_top_line = _live_top_line;
    _follow_live = true;
    textcon_sync();
}

bool k_scrollback_is_active(void)
{
    return !_follow_live;
}

const uint16_t *textcon_load_font(void)
{
    if (fbcon_active) {
        return vga_font_map;
    }
    return vga_font_load() ? vga_font_map : NULL;
}

bool textcon_on_framebuffer(void)
{
    return fbcon_active;
}
