/*
 * draw.c: libtsm's cells drawn with Unifont into a back buffer in RAM, and
 * the bytes that changed written to the framebuffer with pwrite(2).
 */

/* POSIX's calls, which -std=c99 hides. */
#ifndef _POSIX_C_SOURCE
#define _POSIX_C_SOURCE 200809L
#endif

#include <errno.h>
#include <fcntl.h>
#include <stdlib.h>
#include <string.h>
#include <sys/ioctl.h>
#include <time.h>
#include <unistd.h>

#include "term.h"

#define REPLACEMENT 0xFFFDu

/** Milliseconds of the monotonic clock, never 0. */
static uint64_t draw_clock(void)
{
    struct timespec ts;

    clock_gettime(CLOCK_MONOTONIC, &ts);
    return (uint64_t)ts.tv_sec * 1000 + (uint64_t)ts.tv_nsec / 1000000 + 1;
}

/** Channel value `v`, of 8 bits, in a channel of `width` bits at `offset`. */
static uint32_t channel(unsigned v, const struct fb_channel *c)
{
    uint32_t x;

    if (c->width == 0 || c->width > 16)
        return 0;
    if (c->width >= 8)
        x = (uint32_t)v << (c->width - 8) | (uint32_t)v >> (16 - c->width);
    else
        x = (uint32_t)v >> (8 - c->width);
    return x << c->offset;
}

/** The pixel of red `r`, green `g` and blue `b` in the framebuffer's format. */
static uint32_t pixel(const struct term_draw *d, unsigned r, unsigned g, unsigned b)
{
    return channel(r, &d->info.red) | channel(g, &d->info.green) | channel(b, &d->info.blue);
}

/** Store `n` pixels of value `v` at `p`, each `bytes` long, little-endian. */
static void fill(unsigned char *p, unsigned n, unsigned bytes, uint32_t v)
{
    unsigned i;

    if (bytes == 4) {
        for (i = 0; i < n; i++, p += 4) {
            p[0] = (unsigned char)v;
            p[1] = (unsigned char)(v >> 8);
            p[2] = (unsigned char)(v >> 16);
            p[3] = (unsigned char)(v >> 24);
        }
    } else if (bytes == 3) {
        for (i = 0; i < n; i++, p += 3) {
            p[0] = (unsigned char)v;
            p[1] = (unsigned char)(v >> 8);
            p[2] = (unsigned char)(v >> 16);
        }
    } else {
        for (i = 0; i < n; i++, p += 2) {
            p[0] = (unsigned char)v;
            p[1] = (unsigned char)(v >> 8);
        }
    }
}

/** Mark pixel columns `x0` to `x1`, not including it, of cell row `row`. */
static void damage(struct term_draw *d, unsigned row, unsigned x0, unsigned x1)
{
    unsigned *dm = &d->damage[2 * row];

    if (dm[0] == dm[1]) {
        dm[0] = x0;
        dm[1] = x1;
        return;
    }
    if (x0 < dm[0])
        dm[0] = x0;
    if (x1 > dm[1])
        dm[1] = x1;
}

/**
 * OR the glyph of `ucs4` into `rows` at scale 1, at most `cells` cells wide;
 * a glyph wider than that is cut at its right. Returns false when the font
 * has no glyph for it.
 */
static bool add_glyph(const struct term_draw *d, uint32_t ucs4, unsigned cells, uint32_t *rows)
{
    const unsigned char *g;
    unsigned width = 8, y;
    uint32_t mask, bits;

    g = term_font_glyph(d->font, ucs4, &width);
    if (!g)
        return false;
    mask = cells >= 2 ? 0xFFFF0000u : 0xFF000000u;
    for (y = 0; y < UNIFONT_HEIGHT; y++) {
        if (width == 16)
            bits = (uint32_t)g[2 * y] << 24 | (uint32_t)g[2 * y + 1] << 16;
        else
            bits = (uint32_t)g[y] << 24;
        rows[y] |= bits & mask;
    }
    return true;
}

/**
 * The glyph of a cell, rendered at scale 1 and kept in the cache: the code
 * points `ch`, `len` of them, the first a base and the others combining
 * marks drawn over it, `cells` wide, with the shape attributes of `attr`.
 * A glyph of more code points than the cache keeps is rendered into `spare`.
 */
static const uint32_t *render(struct term_draw *d, const uint32_t *ch, size_t len, unsigned cells,
                              const struct tsm_screen_attr *attr, struct term_glyph *spare)
{
    unsigned shape = 0x80u | (attr->bold ? 1u : 0) | (attr->italic ? 2u : 0) |
                     (attr->underline ? 4u : 0) | (cells >= 2 ? 8u : 0);
    uint32_t *rows, mask, h = shape;
    struct term_glyph *gl;
    unsigned y, shift;
    size_t i;

    for (i = 0; i < len; i++)
        h = (h ^ ch[i]) * 0x9E3779B1u;
    gl = spare;
    if (len <= TERM_GLYPH_POINTS) {
        gl = &d->glyphs[(h ^ h >> 15) & (TERM_DRAW_GLYPHS - 1)];
        if (gl->shape == shape && gl->len == len &&
            (len == 0 || memcmp(gl->ch, ch, len * sizeof *ch) == 0))
            return gl->rows;
        gl->len = (unsigned char)len;
        if (len)
            memcpy(gl->ch, ch, len * sizeof *ch);
    }
    gl->shape = 0;
    rows = gl->rows;
    memset(rows, 0, sizeof gl->rows);
    mask = cells >= 2 ? 0xFFFF0000u : 0xFF000000u;
    for (i = 0; i < len; i++)
        if (!add_glyph(d, ch[i], cells, rows) && i == 0 && !add_glyph(d, REPLACEMENT, cells, rows))
            for (y = 2; y < UNIFONT_HEIGHT - 2; y++)
                rows[y] |= mask;
    for (y = 0; y < UNIFONT_HEIGHT; y++) {
        if (attr->bold)
            rows[y] |= rows[y] >> 1;
        if (attr->italic) {
            /* Slant: the top row moves 3 pixels right, the bottom none. */
            shift = (UNIFONT_HEIGHT - 1 - y) / 5;
            rows[y] >>= shift;
        }
        rows[y] &= mask;
    }
    if (attr->underline)
        rows[UNIFONT_HEIGHT - 1] = mask;
    gl->shape = (unsigned char)shape;
    return rows;
}

/**
 * Draw the cell `c` into the back buffer, with its attributes' colors, mark
 * it changed and set its `drawn`. `cursor` is the cursor's shape over it, an
 * enum tsm_screen_cursor_style, or -1 when it has none.
 */
static void draw_cell(struct term_draw *d, struct term_draw_cell *c, int cursor)
{
    const struct tsm_screen_attr *attr = &c->attr;
    unsigned s = d->font->scale, cw = d->font->cell_width, chh = d->font->cell_height;
    unsigned px, py, x0 = c->x * cw, y0 = c->y * chh, w, bar = 2 * s, i, cells = c->width;
    unsigned fr = attr->fr, fg = attr->fg, fb = attr->fb;
    unsigned br = attr->br, bg = attr->bg, bb = attr->bb, t;
    bool inverse = attr->inverse;
    struct term_glyph spare;
    const uint32_t *rows;
    uint32_t fgp, bgp, bits;
    unsigned char *p;

    if (cells == 0)
        cells = 1;
    if (c->x + cells > d->cols)
        cells = d->cols - c->x;
    w = cells * cw;
    if (cursor == TSM_SCREEN_CURSOR_DEFAULT || cursor == TSM_SCREEN_CURSOR_BLOCK_BLINK ||
        cursor == TSM_SCREEN_CURSOR_BLOCK_STEADY)
        inverse = !inverse;
    if (d->bell_until)
        inverse = !inverse;
    if (attr->dim) {
        /* Dim: the foreground halfway to the background. */
        fr = (fr + br) / 2;
        fg = (fg + bg) / 2;
        fb = (fb + bb) / 2;
    }
    if (inverse) {
        t = fr, fr = br, br = t;
        t = fg, fg = bg, bg = t;
        t = fb, fb = bb, bb = t;
    }
    fgp = pixel(d, fr, fg, fb);
    bgp = pixel(d, br, bg, bb);
    rows = render(d, c->ch, c->len, cells, attr, &spare);

    for (py = 0; py < chh; py++) {
        p = d->back + (size_t)(y0 + py) * d->info.pitch + (size_t)x0 * d->bytes;
        bits = rows[py / s];
        if (cursor == TSM_SCREEN_CURSOR_UNDERLINE_BLINK ||
            cursor == TSM_SCREEN_CURSOR_UNDERLINE_STEADY) {
            if (py >= chh - bar)
                bits = 0xFFFFFFFFu;
        }
        for (px = 0; px < w; px += i) {
            /* A run of pixels of one color, `i` long. */
            bool on = bits >> (31 - px / s) & 1;

            if (px < bar &&
                (cursor == TSM_SCREEN_CURSOR_VBAR_BLINK || cursor == TSM_SCREEN_CURSOR_VBAR_STEADY))
                on = true;
            for (i = 1; px + i < w; i++) {
                bool next = bits >> (31 - (px + i) / s) & 1;

                if (px + i < bar && (cursor == TSM_SCREEN_CURSOR_VBAR_BLINK ||
                                     cursor == TSM_SCREEN_CURSOR_VBAR_STEADY))
                    next = true;
                if (next != on)
                    break;
            }
            fill(p + (size_t)px * d->bytes, i, d->bytes, on ? fgp : bgp);
        }
    }
    damage(d, c->y, x0, x0 + w);
    c->drawn = true;
}

/**
 * Whether the cell at column `x` and row `y`, of libtsm's age `age`, is drawn
 * in this frame: when it changed since the last frame, when the cursor is on
 * it (`here`) or was, or when every cell is drawn.
 */
static bool redrawn(const struct term_draw *d, unsigned x, unsigned y, tsm_age_t age, bool here)
{
    bool was = d->cursor_shown && x == d->cursor_x && y == d->cursor_y;

    return d->full || !age || !d->age || age > d->age || here || was;
}

/**
 * tsm_screen_draw()'s callback, once for each cell, row by row: draws the
 * cell when it changed since the last frame, when the cursor came or left,
 * or when every cell is drawn.
 *
 * A wide character's first cell has width 2 and its second width 0 and no
 * code point. The second is drawn by drawing the first, so that a change of
 * either redraws the whole glyph; a cell of width 0 that no wide character
 * covers, which an overwritten first cell leaves, is drawn empty whenever
 * the cell before it is drawn. A cell drawn after a wide one that covers it
 * is drawn over it again.
 */
static int draw_cb(struct tsm_screen *con, uint64_t id, const uint32_t *ch, size_t len,
                   unsigned int width, unsigned int posx, unsigned int posy,
                   const struct tsm_screen_attr *attr, tsm_age_t age, void *data)
{
    struct term_draw *d = data;
    struct term_draw_cell *prev = &d->prev, cur;
    bool here, changed, covered;
    int cursor = -1;

    (void)con;
    (void)id;
    if (posx >= d->cols || posy >= d->rows)
        return 0;
    cur.valid = true;
    cur.drawn = false;
    cur.ch = ch;
    cur.len = width ? len : 0;
    cur.width = width;
    cur.x = posx;
    cur.y = posy;
    cur.attr = *attr;

    here = d->frame_cursor && posx == d->frame_x && posy == d->frame_y;
    if (here) {
        /* libtsm marks the cursor by inverting its cell; drawing draws it. */
        cur.attr.inverse = !cur.attr.inverse;
        cursor = d->cursor_style;
    }
    changed = redrawn(d, posx, posy, age, here);
    covered = prev->valid && prev->y == posy && prev->width >= 2 && prev->x + prev->width > posx;

    if (width == 0 && covered) {
        /* The cursor on the second half is drawn over the whole glyph. */
        if (here)
            draw_cell(d, prev, cursor);
        else if (changed && !prev->drawn)
            draw_cell(d, prev, -1);
        return 0;
    }
    /* Drawn over a wide glyph's right half, or the orphan of a wide
     * character that a narrow one replaced, whose age libtsm leaves. */
    if (prev->valid && prev->y == posy && prev->drawn && (covered || width == 0))
        changed = true;
    if (changed)
        draw_cell(d, &cur, cursor);
    *prev = cur;
    return 0;
}

/**
 * Write the bytes of the back buffer from `start` to `end`, not including
 * it, to the framebuffer. Returns 0 or a negative errno value.
 */
static int flush_range(struct term_draw *d, size_t start, size_t end)
{
    ssize_t n;

    while (start < end) {
        n = pwrite(d->fd, d->back + start, end - start, (off_t)start);
        if (n < 0 && errno == EINTR)
            continue;
        if (n < 0)
            return -errno;
        if (n == 0)
            return -EIO;
        start += (size_t)n;
    }
    return 0;
}

/**
 * Write the cell rows that changed to the framebuffer and forget their
 * damage. A narrow change is written a pixel row at a time; a wide one as
 * one range from its first changed byte to its last, the bytes between them
 * already on the screen, and joined with the rows next to it.
 */
static int flush(struct term_draw *d)
{
    size_t pitch = d->info.pitch, B = d->bytes, chh = d->font->cell_height;
    size_t start = 0, end = 0, a, b, y0, span;
    unsigned row, py, *dm;
    bool open = false;
    int r = 0;

    for (row = 0; row < d->rows && r == 0; row++) {
        dm = &d->damage[2 * row];
        if (dm[0] == dm[1]) {
            if (open)
                r = flush_range(d, start, end);
            open = false;
            continue;
        }
        y0 = row * chh;
        span = (dm[1] - dm[0]) * B;
        if (span * 4 < pitch) {
            if (open)
                r = flush_range(d, start, end);
            open = false;
            for (py = 0; py < chh && r == 0; py++) {
                a = (y0 + py) * pitch + dm[0] * B;
                r = flush_range(d, a, a + span);
            }
        } else {
            a = y0 * pitch + dm[0] * B;
            b = (y0 + chh - 1) * pitch + dm[1] * B;
            if (open) {
                end = b;
            } else {
                start = a;
                end = b;
                open = true;
            }
        }
        dm[0] = dm[1] = 0;
    }
    if (open && r == 0)
        r = flush_range(d, start, end);
    if (r < 0)
        memset(d->damage, 0, 2 * d->rows * sizeof *d->damage);
    return r;
}

/** Allocate the damage of the screen's cell rows at the font's scale. */
static int size_cells(struct term_draw *d)
{
    unsigned *dm;

    term_draw_cells(d, &d->cols, &d->rows);
    if (d->cols == 0 || d->rows == 0)
        return -EINVAL;
    dm = calloc(2 * (size_t)d->rows, sizeof *dm);
    if (!dm)
        return -ENOMEM;
    free(d->damage);
    d->damage = dm;
    d->full = true;
    return 0;
}

int term_draw_init(struct term_draw *d, struct term_font *font, int fd,
                   const struct fb_screen *info)
{
    int r;

    memset(d, 0, sizeof *d);
    d->fd = -1;
    d->font = font;
    d->now = draw_clock;
    if (info->kind != FB_KIND_RGB || info->width == 0 || info->height == 0)
        return -ENODEV;
    switch (info->bpp) {
    case 15:
    case 16:
        d->bytes = 2;
        break;
    case 24:
        d->bytes = 3;
        break;
    case 32:
        d->bytes = 4;
        break;
    default:
        return -ENODEV;
    }
    if (info->pitch < info->width * d->bytes)
        return -ENODEV;
    d->info = *info;
    d->back = calloc((size_t)info->pitch, info->height);
    d->glyphs = calloc(TERM_DRAW_GLYPHS, sizeof *d->glyphs);
    r = d->back && d->glyphs ? size_cells(d) : -ENOMEM;
    if (r < 0) {
        term_draw_close(d);
        return r;
    }
    d->fd = fd;
    return 0;
}

int term_draw_open(struct term_draw *d, struct term_font *font)
{
    struct fb_screen info;
    int fd, r;

    d->fd = -1;
    d->back = NULL;
    d->glyphs = NULL;
    d->damage = NULL;
    fd = open("/fb", O_RDWR | O_CLOEXEC);
    if (fd < 0)
        return -errno;
    if (ioctl(fd, FB_GETINFO, &info) < 0) {
        r = -errno;
        close(fd);
        return r;
    }
    r = term_draw_init(d, font, fd, &info);
    if (r == 0 && ioctl(fd, FB_MODE_NATIVE, 0) < 0)
        r = -errno;
    if (r < 0) {
        if (d->fd < 0)
            close(fd);
        term_draw_close(d);
    }
    return r;
}

void term_draw_cells(const struct term_draw *d, unsigned *cols, unsigned *rows)
{
    *cols = 0;
    *rows = 0;
    if (d->font && d->font->cell_width && d->font->cell_height) {
        *cols = d->info.width / d->font->cell_width;
        *rows = d->info.height / d->font->cell_height;
    }
}

/**
 * Clear the back buffer to black, which is libtsm's default background, so
 * that the margins past the last column and row are, and mark it all
 * changed. The cells are drawn over it.
 */
static void clear(struct term_draw *d)
{
    unsigned row;

    memset(d->back, 0, (size_t)d->info.pitch * d->info.height);
    for (row = 0; row < d->rows; row++)
        damage(d, row, 0, d->info.width);
}

/**
 * Write the margin below the last cell row, which no cell row's damage
 * covers. Returns 0 or a negative errno value.
 */
static int flush_bottom(struct term_draw *d)
{
    size_t top = (size_t)d->rows * d->font->cell_height * d->info.pitch;
    size_t all = (size_t)d->info.height * d->info.pitch;

    return top < all ? flush_range(d, top, all) : 0;
}

int term_draw_frame(struct term_draw *d, struct term_screen *s)
{
    struct tsm_screen *con = s->screen;
    uint64_t now = d->now();
    unsigned shift, count, pos;
    bool full = false;
    int r;

    if (term_screen_held(s)) {
        if (!d->held_since)
            d->held_since = now;
        if (now - d->held_since < TERM_DRAW_HOLD_MS)
            return 0;
        term_screen_release(s);
    }
    d->held_since = 0;
    if (d->bell_until == 1) {
        /* The bell rang: the frame shows it from now. */
        d->bell_until = now + TERM_DRAW_BELL_MS;
        d->full = true;
    } else if (d->bell_until && now >= d->bell_until) {
        d->bell_until = 0;
        d->full = true;
    }

    /* Where tsm_screen_draw() inverts a cell for the cursor: its column and
     * row, cut to the screen, moved down by the scrollback lines shown. */
    count = tsm_screen_sb_get_line_count(con);
    pos = tsm_screen_sb_get_line_pos(con);
    shift = pos < count ? count - pos : 0;
    d->frame_x = tsm_screen_get_cursor_x(con);
    d->frame_y = tsm_screen_get_cursor_y(con);
    if (d->frame_x >= tsm_screen_get_width(con))
        d->frame_x = tsm_screen_get_width(con) - 1;
    if (d->frame_y >= tsm_screen_get_height(con))
        d->frame_y = tsm_screen_get_height(con) - 1;
    d->frame_y += shift;
    d->frame_cursor = !(tsm_screen_get_flags(con) & TSM_SCREEN_HIDE_CURSOR) &&
                      d->frame_y < tsm_screen_get_height(con);
    d->cursor_style = (int)tsm_screen_get_cursor_style(con);

    if (d->full || !d->age) {
        clear(d);
        full = true;
    }
    d->prev.valid = false;
    d->age = tsm_screen_draw(con, draw_cb, d);
    d->full = false;
    d->cursor_shown = d->frame_cursor;
    d->cursor_x = d->frame_x;
    d->cursor_y = d->frame_y;
    d->prev.valid = false;

    r = flush(d);
    if (r == 0 && full)
        r = flush_bottom(d);
    if (r < 0) {
        d->full = true;
        d->age = 0;
    }
    return r;
}

int term_draw_timeout(const struct term_draw *d)
{
    uint64_t now, when;

    /* A hold keeps every frame back, the bell's among them. */
    if (d->held_since)
        when = d->held_since + TERM_DRAW_HOLD_MS;
    else if (d->bell_until == 1)
        return 0;
    else if (d->bell_until)
        when = d->bell_until;
    else
        return -1;
    now = d->now();
    return when <= now ? 0 : (int)(when - now);
}

void term_draw_bell(struct term_draw *d)
{
    /* 1 asks the next frame to start the bell; a bell that shows goes on. */
    if (!d->bell_until)
        d->bell_until = 1;
}

void term_draw_redraw(struct term_draw *d)
{
    d->full = true;
}

int term_draw_rescale(struct term_draw *d, unsigned scale, const char *conf)
{
    unsigned old = d->font->scale;
    int r;

    r = term_font_scale(d->font, scale);
    if (r < 0)
        return r;
    r = size_cells(d);
    if (r < 0) {
        term_font_scale(d->font, old);
        term_draw_cells(d, &d->cols, &d->rows);
        return r;
    }
    d->cursor_shown = false;
    return conf ? term_font_conf_save(conf, scale) : 0;
}

void term_draw_close(struct term_draw *d)
{
    if (d->fd >= 0)
        close(d->fd);
    d->fd = -1;
    free(d->back);
    free(d->glyphs);
    free(d->damage);
    d->back = NULL;
    d->glyphs = NULL;
    d->damage = NULL;
}
