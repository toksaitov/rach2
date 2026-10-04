/*
 * fbcon.c: the text console on a framebuffer, as fbcon.h describes it.
 *
 * The screen is the console's cells, as many as it holds of the font at the
 * largest whole scale that still gives 80 columns and 24 rows, which
 * `textcon_cols` and `textcon_rows` record; the window of the memory they
 * come from begins at `start`. `drawn` is what each of the screen's cells
 * shows, and a cell is drawn only when the memory's cell differs from it or
 * the cursor came or went. A scroll moves `start` down a line and so draws
 * every cell whose line differs from the one above it, and no blank cell
 * under a blank one.
 *
 * The caller keeps interrupts off around a byte's work, as vga_text.c says.
 */

#include "fbcon.h"
#include "vga_font.h"
#include "vga_text.h"
#include "../klog.h"
#include "../lib/kstring.h"

#define SCREEN_CELLS (textcon_cols * textcon_rows)
#define GLYPH_WIDTH  8
#define GLYPH_HEIGHT 16

/* The fewest columns and rows a screen gives the console, and a cursor on
 * no cell. */
#define MIN_COLS 80u
#define MIN_ROWS 24u
#define NO_CELL  0xFFFFFFFFu

/* The cursor: the two lines the VGA's text mode underlines a cell with. */
#define CURSOR_FIRST_LINE 13
#define CURSOR_LAST_LINE  14

/* The blank cell, light grey on black, which the screen starts as. */
#define BLANK_CELL 0x0720

bool fbcon_active = false;
uint16_t fbcon_cells[FBCON_CELLS];

/* The sixteen colors of a cell's attribute, in red, green and blue, as the
 * VGA's text palette has them. */
static const uint8_t vga_rgb[16][3] = {
    {0x00, 0x00, 0x00}, {0x00, 0x00, 0xAA}, {0x00, 0xAA, 0x00}, {0x00, 0xAA, 0xAA},
    {0xAA, 0x00, 0x00}, {0xAA, 0x00, 0xAA}, {0xAA, 0x55, 0x00}, {0xAA, 0xAA, 0xAA},
    {0x55, 0x55, 0x55}, {0x55, 0x55, 0xFF}, {0x55, 0xFF, 0x55}, {0x55, 0xFF, 0xFF},
    {0xFF, 0x55, 0x55}, {0xFF, 0x55, 0xFF}, {0xFF, 0xFF, 0x55}, {0xFF, 0xFF, 0xFF}};

/* The screen's geometry and each color as one of its pixels. */
static uint32_t fb_pitch;
static uint32_t fb_width;
static uint32_t fb_height;
static uint32_t fb_bytes;
static uint32_t fb_scale;
static uint32_t grid_left;
static uint32_t grid_top;
static uint32_t pixel[16];

static uint32_t start;
static uint32_t cursor;
static uint16_t drawn[TEXTCON_COLS_MAX * TEXTCON_ROWS_MAX];
static uint32_t drawn_cursor = NO_CELL;

/* The loader's description of the screen, kept for the colors of a
 * picture's palette. */
static struct boot_framebuffer fb_desc;

/*
 * The 320x200 picture of the program that holds /fb (vga_gfx.c), which has
 * the screen to itself while `gfx_on`: each of its 256 colors as one of the
 * screen's pixels, the whole number its pixels are drawn at, the place of
 * its top left corner, and one of its lines drawn at that size, which is
 * copied to the screen as many times as the size.
 */
#define GFX_WIDTH      320u
#define GFX_HEIGHT     200u
#define GFX_LINE_BYTES 16384u
static int gfx_on;
static uint32_t gfx_pixel[256];
static uint32_t gfx_scale;
static uint32_t gfx_left;
static uint32_t gfx_top;
static uint8_t gfx_line[GFX_LINE_BYTES];

/** Color `v`, from 0 to 255, in a field of `bits` bits at bit `shift`. */
static uint32_t color_field(uint32_t v, uint32_t shift, uint32_t bits)
{
    if (bits == 0 || shift + bits > 32) {
        return 0;
    }
    return (bits > 8 ? v << (bits - 8) : v >> (8 - bits)) << shift;
}

/** The palette entry nearest to color `c`, or `c` itself with no palette,
 * since the VGA's default palette begins with the text colors. */
static uint32_t nearest_index(const struct boot_framebuffer *fb, uint32_t c)
{
    uint32_t i, best = c, best_d = 0xFFFFFFFFu;

    for (i = 0; i < fb->colors; i++) {
        int dr = (int)fb->palette[i][0] - vga_rgb[c][0];
        int dg = (int)fb->palette[i][1] - vga_rgb[c][1];
        int db = (int)fb->palette[i][2] - vga_rgb[c][2];
        uint32_t d = (uint32_t)(dr * dr + dg * dg + db * db);

        if (d < best_d) {
            best_d = d;
            best = i;
        }
    }
    return best;
}

static void put_pixel(volatile uint8_t *p, uint32_t v)
{
    switch (fb_bytes) {
    case 4:
        *(volatile uint32_t *)(uintptr_t)p = v;
        break;
    case 3:
        p[0] = (uint8_t)v;
        p[1] = (uint8_t)(v >> 8);
        p[2] = (uint8_t)(v >> 16);
        break;
    case 2:
        *(volatile uint16_t *)(uintptr_t)p = (uint16_t)v;
        break;
    default:
        *p = (uint8_t)v;
        break;
    }
}

/** Draw screen cell `i` as `cell` shows it, underlined when the cursor is
 * on it. */
static void draw_cell(volatile uint8_t *base, uint32_t i, uint16_t cell, int with_cursor)
{
    const uint8_t *rows = vga_font_all[cell & 0xFF].rows;
    uint32_t fg = pixel[(cell >> 8) & 0xF];
    uint32_t bg = pixel[(cell >> 12) & 0xF];
    uint32_t left = grid_left + (i % textcon_cols) * GLYPH_WIDTH * fb_scale;
    uint32_t top = grid_top + (i / textcon_cols) * GLYPH_HEIGHT * fb_scale;
    volatile uint8_t *line = base + top * fb_pitch + left * fb_bytes;
    uint32_t r, s, c, t;

    for (r = 0; r < GLYPH_HEIGHT; r++) {
        uint32_t bits = rows[r];

        if (with_cursor && r >= CURSOR_FIRST_LINE && r <= CURSOR_LAST_LINE) {
            bits = 0xFF;
        }
        for (s = 0; s < fb_scale; s++, line += fb_pitch) {
            volatile uint8_t *p = line;

            for (c = 0; c < GLYPH_WIDTH; c++) {
                uint32_t v = (bits & (0x80u >> c)) ? fg : bg;

                for (t = 0; t < fb_scale; t++, p += fb_bytes) {
                    put_pixel(p, v);
                }
            }
        }
    }
}

/**
 * Draw screen cell `i` if the memory's cell or the cursor changed there. The
 * cursor is drawn on one cell at a time: one drawn with it elsewhere is drawn
 * again without it first, since a redraw of the screen can reach the
 * cursor's new cell before its old one.
 */
static void refresh(volatile uint8_t *base, uint32_t i)
{
    uint32_t at = start + i;
    uint16_t cell = at < FBCON_CELLS ? fbcon_cells[at] : BLANK_CELL;
    int want_cursor = cursor == at;
    uint32_t was;

    if (drawn[i] == cell && (drawn_cursor == i) == want_cursor) {
        return;
    }
    if (want_cursor && drawn_cursor != i && drawn_cursor != NO_CELL) {
        was = drawn_cursor;
        drawn_cursor = NO_CELL;
        draw_cell(base, was, drawn[was], 0);
    }
    draw_cell(base, i, cell, want_cursor);
    drawn[i] = cell;
    if (want_cursor) {
        drawn_cursor = i;
    } else if (drawn_cursor == i) {
        drawn_cursor = NO_CELL;
    }
}

int fbcon_init(const struct boot_framebuffer *fb)
{
    volatile uint8_t *base;
    uint32_t i, col, row;

    if (fb->type == BOOT_FB_RGB) {
        if (fb->bpp < 8 || fb->bpp > 32) {
            return -1;
        }
    } else if (fb->type != BOOT_FB_INDEXED || fb->bpp != 8) {
        return -1;
    }
    fb_bytes = (fb->bpp + 7) / 8;
    if (fb->width / GLYPH_WIDTH < MIN_COLS || fb->height / GLYPH_HEIGHT < MIN_ROWS ||
        fb->width > fb->pitch / fb_bytes) {
        return -1;
    }
    for (fb_scale = 1; fb->width / (GLYPH_WIDTH * (fb_scale + 1)) >= MIN_COLS &&
                       fb->height / (GLYPH_HEIGHT * (fb_scale + 1)) >= MIN_ROWS;
         fb_scale++) {
    }
    textcon_cols = fb->width / (GLYPH_WIDTH * fb_scale);
    textcon_rows = fb->height / (GLYPH_HEIGHT * fb_scale);
    if (textcon_cols > TEXTCON_COLS_MAX) {
        textcon_cols = TEXTCON_COLS_MAX;
    }
    if (textcon_rows > TEXTCON_ROWS_MAX) {
        textcon_rows = TEXTCON_ROWS_MAX;
    }
    fb_desc = *fb;
    fb_pitch = fb->pitch;
    fb_width = fb->width;
    fb_height = fb->height;
    grid_left = (fb_width - textcon_cols * GLYPH_WIDTH * fb_scale) / 2;
    grid_top = (fb_height - textcon_rows * GLYPH_HEIGHT * fb_scale) / 2;
    for (i = 0; i < 16; i++) {
        if (fb->type == BOOT_FB_RGB) {
            pixel[i] = color_field(vga_rgb[i][0], fb->red_shift, fb->red_bits) |
                       color_field(vga_rgb[i][1], fb->green_shift, fb->green_bits) |
                       color_field(vga_rgb[i][2], fb->blue_shift, fb->blue_bits);
        } else {
            pixel[i] = nearest_index(fb, i);
        }
    }

    base = fbcon_window();
    if (base == NULL) {
        return -1;
    }
    for (row = 0; row < fb_height; row++) {
        volatile uint8_t *p = base + row * fb_pitch;

        for (col = 0; col < fb_width; col++, p += fb_bytes) {
            put_pixel(p, pixel[0]);
        }
    }
    for (i = 0; i < FBCON_CELLS; i++) {
        fbcon_cells[i] = BLANK_CELL;
    }
    for (i = 0; i < SCREEN_CELLS; i++) {
        drawn[i] = BLANK_CELL;
    }
    start = 0;
    cursor = 0;
    drawn_cursor = NO_CELL;
    fbcon_active = true;
    klog(KLOG_INFO, "console", "framebuffer: %dx%d, %d bits a pixel; the font at %dx, %dx%d cells",
         (int)fb_width, (int)fb_height, (int)fb->bpp, (int)fb_scale, (int)textcon_cols,
         (int)textcon_rows);
    return 0;
}

void fbcon_stored(uint32_t at)
{
    volatile uint8_t *base;

    if (!fbcon_active || gfx_on || at < start || at >= start + SCREEN_CELLS) {
        return;
    }
    base = fbcon_window();
    if (base != NULL) {
        refresh(base, at - start);
    }
}

void fbcon_set_start(uint32_t cell)
{
    volatile uint8_t *base;
    uint32_t i;

    start = cell;
    base = fbcon_window();
    if (!fbcon_active || gfx_on || base == NULL) {
        return;
    }
    for (i = 0; i < SCREEN_CELLS; i++) {
        refresh(base, i);
    }
}

void fbcon_set_cursor(uint32_t cell)
{
    volatile uint8_t *base;
    uint32_t was = drawn_cursor;

    cursor = cell;
    base = fbcon_window();
    if (!fbcon_active || gfx_on || base == NULL) {
        return;
    }
    if (was != NO_CELL) {
        refresh(base, was);
    }
    if (cell >= start && cell < start + SCREEN_CELLS) {
        refresh(base, cell - start);
    }
}

/** Paint the whole screen color 0, black, and mark every cell drawn blank,
 * which a blank cell on black is. */
static void clear_screen(volatile uint8_t *base)
{
    uint32_t row, col, i;

    for (row = 0; row < fb_height; row++) {
        volatile uint8_t *p = base + row * fb_pitch;

        for (col = 0; col < fb_width; col++, p += fb_bytes) {
            put_pixel(p, pixel[0]);
        }
    }
    for (i = 0; i < SCREEN_CELLS; i++) {
        drawn[i] = BLANK_CELL;
    }
    drawn_cursor = NO_CELL;
}

/** The pixel of the 8-bit red, green and blue `rgb` on this screen: its
 * fields, or the nearest entry of an indexed screen's palette. */
static uint32_t rgb_pixel(const uint8_t *rgb)
{
    uint32_t i, best = 0, best_d = 0xFFFFFFFFu;

    if (fb_desc.type == BOOT_FB_RGB) {
        return color_field(rgb[0], fb_desc.red_shift, fb_desc.red_bits) |
               color_field(rgb[1], fb_desc.green_shift, fb_desc.green_bits) |
               color_field(rgb[2], fb_desc.blue_shift, fb_desc.blue_bits);
    }
    for (i = 0; i < fb_desc.colors; i++) {
        int dr = (int)fb_desc.palette[i][0] - rgb[0];
        int dg = (int)fb_desc.palette[i][1] - rgb[1];
        int db = (int)fb_desc.palette[i][2] - rgb[2];
        uint32_t d = (uint32_t)(dr * dr + dg * dg + db * db);

        if (d < best_d) {
            best_d = d;
            best = i;
        }
    }
    return best;
}

void fbcon_gfx_begin(void)
{
    volatile uint8_t *base = fbcon_window();
    uint32_t i;

    if (!fbcon_active || base == NULL) {
        return;
    }
    gfx_scale = fb_width / GFX_WIDTH < fb_height / GFX_HEIGHT ? fb_width / GFX_WIDTH
                                                               : fb_height / GFX_HEIGHT;
    while (gfx_scale > 1 && GFX_WIDTH * gfx_scale * fb_bytes > GFX_LINE_BYTES) {
        gfx_scale--;
    }
    gfx_left = (fb_width - GFX_WIDTH * gfx_scale) / 2;
    gfx_top = (fb_height - GFX_HEIGHT * gfx_scale) / 2;
    for (i = 0; i < 256; i++) {
        gfx_pixel[i] = pixel[i < 16 ? i : 0];
    }
    gfx_on = 1;
    clear_screen(base);
}

void fbcon_gfx_palette(const uint8_t *rgb)
{
    uint32_t i;

    for (i = 0; i < 256; i++) {
        gfx_pixel[i] = rgb_pixel(rgb + 3 * i);
    }
}

void fbcon_gfx_frame(const uint8_t *pixels)
{
    volatile uint8_t *base = fbcon_window();
    uint32_t line, col, t, n = GFX_WIDTH * gfx_scale * fb_bytes;

    if (!gfx_on || base == NULL) {
        return;
    }
    for (line = 0; line < GFX_HEIGHT; line++) {
        uint8_t *q = gfx_line;
        volatile uint8_t *row = base + (gfx_top + line * gfx_scale) * fb_pitch + gfx_left * fb_bytes;

        for (col = 0; col < GFX_WIDTH; col++) {
            uint32_t v = gfx_pixel[pixels[line * GFX_WIDTH + col]];

            for (t = 0; t < gfx_scale; t++, q += fb_bytes) {
                put_pixel(q, v);
            }
        }
        for (t = 0; t < gfx_scale; t++, row += fb_pitch) {
            k_memcpy((void *)(uintptr_t)row, gfx_line, n);
        }
    }
}

void fbcon_gfx_end(void)
{
    volatile uint8_t *base = fbcon_window();
    uint32_t i;

    if (!gfx_on) {
        return;
    }
    gfx_on = 0;
    if (base != NULL) {
        clear_screen(base);
        for (i = 0; i < SCREEN_CELLS; i++) {
            refresh(base, i);
        }
    }
}
