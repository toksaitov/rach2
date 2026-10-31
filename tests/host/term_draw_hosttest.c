/**
 * Host test of term's drawing, term/draw.c, and its font, term/font.c, on a
 * framebuffer in memory: a file of the framebuffer's layout, written with
 * pwrite(2) as /fb is, and read back pixel by pixel.
 *
 * host-deps: term/unifont.c
 * host-cflags: -I vendor/libtsm/libtsm-4.8.0/src/tsm -I rach2/include
 *
 * Both files are included whole. The test packs six glyphs of Unifont
 * 18.0.01 with tools/font/mkunifont.c, also included, and loads them with
 * term_font_load(). In place of libtsm's screen it has a model of one, whose
 * tsm_screen_draw() reports its cells as libtsm's does: the cursor's cell
 * inverted, a space with no underline and a cell of no code point given no
 * code points, a wide character's second cell given width 0. A cell's age
 * says whether it changed, so the test can check that drawing writes what
 * changed and leaves the rest of the framebuffer alone. A clock of its own
 * times the bell and mode 2026's hold.
 *
 * Every pixel is checked against a value worked out here from the format,
 * for RGB at 32, 24, 16 and 15 bits a pixel.
 */
#include "../../term/draw.c"
#include "../../term/font.c"

#define main mkunifont_main
#include "../../tools/font/mkunifont.c"
#undef main

#include <stdio.h>

static int fails;

#define CHECK(cond, what)                          \
    do {                                           \
        if (!(cond)) {                             \
            fails++;                               \
            fprintf(stderr, "  FAIL: %s\n", what); \
        }                                          \
    } while (0)

/* The glyphs, as unifont_all-18.0.01.hex has them. */
static const char font_text[] =
    "0020:00000000000000000000000000000000\n"
    "0041:0000000018242442427E424242420000\n"
    "0301:0C300000000000000000000000000000\n"
    "0416:0000000049492A2A1C1C2A2A49490000\n"
    "4E2D:01000100010001003FF8210821082108210821083FF821080100010001000100\n"
    "FFFD:0000007E665A5A7A76767E76767E0000\n";
static const unsigned char glyph_a[16] = {0x00, 0x00, 0x00, 0x00, 0x18, 0x24, 0x24, 0x42,
                                          0x42, 0x7E, 0x42, 0x42, 0x42, 0x42, 0x00, 0x00};
static const unsigned char glyph_fffd[16] = {0x00, 0x00, 0x00, 0x7E, 0x66, 0x5A, 0x5A, 0x7A,
                                             0x76, 0x76, 0x7E, 0x76, 0x76, 0x7E, 0x00, 0x00};
static const unsigned char glyph_cjk[32] = {
    0x01, 0x00, 0x01, 0x00, 0x01, 0x00, 0x01, 0x00, 0x3F, 0xF8, 0x21, 0x08, 0x21, 0x08, 0x21, 0x08,
    0x21, 0x08, 0x21, 0x08, 0x3F, 0xF8, 0x21, 0x08, 0x01, 0x00, 0x01, 0x00, 0x01, 0x00, 0x01, 0x00};

/* The screen: 6 by 3 cells of 8 by 16 pixels, and 2 pixels of margin on the
 * right and at the bottom, in rows padded by 4 bytes. */
#define COLS   6
#define ROWS   3
#define WIDTH  50
#define HEIGHT 50

/* ---- The model of libtsm's screen ---- */

struct model_cell {
    uint32_t ch[3];
    size_t len;
    unsigned width;
    struct tsm_screen_attr attr;
    tsm_age_t age;
};

struct tsm_screen {
    struct model_cell cell[ROWS][COLS];
    unsigned cols, rows; /* the screen's size, which a resize changes */
    unsigned cursor_x, cursor_y;
    unsigned flags;
    enum tsm_screen_cursor_style style;
    unsigned sb_count, sb_pos;
    tsm_age_t age; /* the age the next change gets */
    bool held, released;
    unsigned draws; /* calls of tsm_screen_draw() */
};

static struct tsm_screen model;
static uint64_t clock_ms = 1000;

static uint64_t model_clock(void)
{
    return clock_ms;
}

unsigned int tsm_screen_get_width(struct tsm_screen *con)
{
    return con->cols;
}

unsigned int tsm_screen_get_height(struct tsm_screen *con)
{
    return con->rows;
}

unsigned int tsm_screen_get_cursor_x(struct tsm_screen *con)
{
    return con->cursor_x;
}

unsigned int tsm_screen_get_cursor_y(struct tsm_screen *con)
{
    return con->cursor_y;
}

unsigned int tsm_screen_get_flags(struct tsm_screen *con)
{
    return con->flags;
}

enum tsm_screen_cursor_style tsm_screen_get_cursor_style(struct tsm_screen *con)
{
    return con->style;
}

unsigned int tsm_screen_sb_get_line_count(struct tsm_screen *con)
{
    return con->sb_count;
}

unsigned int tsm_screen_sb_get_line_pos(struct tsm_screen *con)
{
    return con->sb_pos;
}

/** Report the model's cells as libtsm's tsm_screen_draw() does. */
tsm_age_t tsm_screen_draw(struct tsm_screen *con, tsm_screen_draw_cb draw_cb, void *data)
{
    unsigned x, y, shift = con->sb_count - con->sb_pos;
    struct tsm_screen_attr attr;
    struct model_cell *c;
    size_t len;

    con->draws++;
    for (y = 0; y < con->rows && y < ROWS; y++) {
        for (x = 0; x < con->cols && x < COLS; x++) {
            c = &con->cell[y][x];
            attr = c->attr;
            if (y == con->cursor_y + shift && x == con->cursor_x &&
                !(con->flags & TSM_SCREEN_HIDE_CURSOR))
                attr.inverse = !attr.inverse;
            len = c->len;
            if (c->ch[0] == 0 || (c->ch[0] == ' ' && !attr.underline))
                len = 0;
            draw_cb(con, c->ch[0], c->ch, len, c->width, x, y, &attr, c->age, data);
        }
    }
    return con->age;
}

/* ---- The rest of term that drawing calls ---- */

bool term_screen_held(struct term_screen *s)
{
    return s->screen->held;
}

void term_screen_release(struct term_screen *s)
{
    s->screen->held = false;
    s->screen->released = true;
}

int term_screen_resize(struct term_screen *s, unsigned cols, unsigned rows)
{
    s->screen->cols = cols;
    s->screen->rows = rows;
    return 0;
}

static struct term_pty pty;
static unsigned pty_cols, pty_rows;

int term_pty_resize(struct term_pty *pty, unsigned cols, unsigned rows)
{
    (void)pty;
    pty_cols = cols;
    pty_rows = rows;
    return 0;
}

/* ---- Colors and the model's cells ---- */

struct rgb {
    unsigned r, g, b;
};

static const struct rgb orange = {255, 128, 0}, blue = {0, 0, 255}, black = {0, 0, 0};

/** An attribute of foreground `fg` on background `bg`. */
static struct tsm_screen_attr colors(struct rgb fg, struct rgb bg)
{
    struct tsm_screen_attr a;

    memset(&a, 0, sizeof a);
    a.fccode = -1;
    a.bccode = -1;
    a.fr = (uint8_t)fg.r;
    a.fg = (uint8_t)fg.g;
    a.fb = (uint8_t)fg.b;
    a.br = (uint8_t)bg.r;
    a.bg = (uint8_t)bg.g;
    a.bb = (uint8_t)bg.b;
    return a;
}

/** Put `len` code points at column `x` and row `y`, `width` wide, changed now. */
static void put_cell(unsigned x, unsigned y, const uint32_t *ch, size_t len, unsigned width,
                     struct tsm_screen_attr attr)
{
    struct model_cell *c = &model.cell[y][x];

    memset(c->ch, 0, sizeof c->ch);
    memcpy(c->ch, ch, len * sizeof *ch);
    c->len = len;
    c->width = width;
    c->attr = attr;
    c->age = ++model.age;
}

/** Put the one character `ch` at `x`, `y`. */
static void put_char(unsigned x, unsigned y, uint32_t ch, struct tsm_screen_attr attr)
{
    put_cell(x, y, &ch, 1, 1, attr);
}

/** Fill the model with spaces of orange on blue, all changed: ages go on
 * from the last, as libtsm's do, so that drawing sees the change. */
static void reset_model(void)
{
    tsm_age_t age = model.age;
    unsigned x, y;

    memset(&model, 0, sizeof model);
    model.age = age;
    model.cols = COLS;
    model.rows = ROWS;
    for (y = 0; y < ROWS; y++)
        for (x = 0; x < COLS; x++)
            put_char(x, y, ' ', colors(orange, blue));
    /* The cursor is out of the way, past the cells the cases look at. */
    model.cursor_x = COLS - 1;
    model.cursor_y = ROWS - 1;
}

/* ---- The framebuffer in memory ---- */

struct format {
    const char *name;
    unsigned bpp, bytes;
    struct fb_channel red, green, blue;
};

static const struct format formats[] = {
    {"32 bits, xRGB", 32, 4, {16, 8}, {8, 8}, {0, 8}},
    {"24 bits, BGR", 24, 3, {16, 8}, {8, 8}, {0, 8}},
    {"16 bits, RGB 5:6:5", 16, 2, {11, 5}, {5, 6}, {0, 5}},
    {"15 bits, RGB 5:5:5", 15, 2, {10, 5}, {5, 5}, {0, 5}},
};

static const struct format *fmt;
static int fb_fd = -1;
static unsigned fb_pitch;
static unsigned char fb[(WIDTH * 4 + 4) * HEIGHT];
static char fb_path[4096];

/** The value a pixel of `c` has in the format under test. */
static uint32_t expect(struct rgb c)
{
    unsigned rw = fmt->red.width, gw = fmt->green.width, bw = fmt->blue.width;

    return (uint32_t)(c.r >> (8 - rw)) << fmt->red.offset |
           (uint32_t)(c.g >> (8 - gw)) << fmt->green.offset |
           (uint32_t)(c.b >> (8 - bw)) << fmt->blue.offset;
}

/** Read the framebuffer file into `fb`. */
static void load(void)
{
    size_t n = (size_t)fb_pitch * HEIGHT;

    CHECK(pread(fb_fd, fb, n, 0) == (ssize_t)n, "the framebuffer reads back whole");
}

/** The pixel at `x`, `y` of `fb`. */
static uint32_t at(unsigned x, unsigned y)
{
    const unsigned char *p = fb + (size_t)y * fb_pitch + (size_t)x * fmt->bytes;
    uint32_t v = 0;
    unsigned i;

    for (i = 0; i < fmt->bytes; i++)
        v |= (uint32_t)p[i] << (8 * i);
    return v;
}

/** Overwrite the framebuffer file's pixels of cell `x`, `y` at scale 1 with 0x55. */
static void scribble(unsigned x, unsigned y)
{
    unsigned char junk[16 * 4];
    unsigned py;

    memset(junk, 0x55, sizeof junk);
    for (py = 0; py < 16; py++)
        CHECK(pwrite(fb_fd, junk, 8 * fmt->bytes,
                     (off_t)((y * 16 + py) * fb_pitch + x * 8 * fmt->bytes)) ==
                  (ssize_t)(8 * fmt->bytes),
              "the scribble is written");
}

/** Whether every pixel of cell `x`, `y` at scale 1 is 0x55 bytes. */
static bool scribbled(unsigned x, unsigned y)
{
    const unsigned char *p;
    unsigned py, i;

    for (py = 0; py < 16; py++) {
        p = fb + (size_t)(y * 16 + py) * fb_pitch + (size_t)x * 8 * fmt->bytes;
        for (i = 0; i < 8 * fmt->bytes; i++)
            if (p[i] != 0x55)
                return false;
    }
    return true;
}

/**
 * Whether the cell at pixel `x0`, `y0`, `w` pixels wide, shows the glyph
 * `rows`, of `gw` pixels a row, at `scale`, in `fg` on `bg`.
 */
static bool shows(unsigned x0, unsigned y0, unsigned w, const unsigned char *rows, unsigned gw,
                  unsigned scale, struct rgb fg, struct rgb bg)
{
    unsigned x, y, gx, gy;
    bool on;

    for (y = 0; y < 16 * scale; y++) {
        for (x = 0; x < w; x++) {
            gx = x / scale;
            gy = y / scale;
            on = false;
            if (rows && gx < gw)
                on = rows[gy * (gw / 8) + gx / 8] >> (7 - gx % 8) & 1;
            if (at(x0 + x, y0 + y) != expect(on ? fg : bg))
                return false;
        }
    }
    return true;
}

/** Whether cell `x`, `y` at scale 1 shows glyph `rows` of 8 pixels. */
static bool cell_shows(unsigned x, unsigned y, const unsigned char *rows, struct rgb fg,
                       struct rgb bg)
{
    return shows(x * 8, y * 16, 8, rows, 8, 1, fg, bg);
}

static struct term_font font;
static struct term_draw draw;
static struct term_screen screen;

/** Open the framebuffer file and drawing on it in the format `f`. */
static int open_fb(const struct format *f)
{
    struct fb_screen info;
    int r;

    fmt = f;
    term_font_scale(&font, 1);
    fb_pitch = WIDTH * f->bytes + 4;
    snprintf(fb_path, sizeof fb_path, "%s/term_draw.XXXXXX",
             getenv("TMPDIR") ? getenv("TMPDIR") : "/tmp");
    fb_fd = mkstemp(fb_path);
    CHECK(fb_fd >= 0, "the framebuffer file is made");
    if (fb_fd < 0)
        return -1;
    CHECK(ftruncate(fb_fd, (off_t)fb_pitch * HEIGHT) == 0, "the framebuffer file is sized");
    memset(&info, 0, sizeof info);
    info.width = WIDTH;
    info.height = HEIGHT;
    info.pitch = fb_pitch;
    info.bpp = f->bpp;
    info.kind = FB_KIND_RGB;
    info.red = f->red;
    info.green = f->green;
    info.blue = f->blue;
    r = term_draw_init(&draw, &font, fb_fd, &info);
    CHECK(r == 0, "term_draw_init takes the format");
    if (r < 0)
        return -1;
    draw.now = model_clock;
    reset_model();
    memset(&screen, 0, sizeof screen);
    screen.screen = &model;
    return 0;
}

/** Close drawing, which closes the file, and remove the file. */
static void close_fb(void)
{
    if (draw.fd < 0 && fb_fd >= 0)
        close(fb_fd);
    term_draw_close(&draw);
    if (fb_path[0])
        unlink(fb_path);
    fb_path[0] = '\0';
    fb_fd = -1;
}

/** Draw a frame and read the framebuffer. */
static void frame(void)
{
    CHECK(term_draw_frame(&draw, &screen) == 0, "the frame is written");
    load();
}

/* ---- The cases ---- */

/** Start a case on a screen of spaces, every cell drawn at the next frame. */
static void begin(void)
{
    reset_model();
    term_draw_redraw(&draw);
}

/** The first frame draws every cell, the margins black. */
static void test_first_frame(void)
{
    unsigned x, y;
    bool ok = true;

    put_char(0, 0, 'A', colors(orange, blue));
    frame();
    CHECK(draw.cols == COLS && draw.rows == ROWS, "6 by 3 cells of 8 by 16 pixels");
    CHECK(cell_shows(0, 0, glyph_a, orange, blue), "A in orange on blue");
    CHECK(cell_shows(1, 0, NULL, orange, blue), "a space in blue");
    for (y = 0; y < HEIGHT; y++)
        for (x = 0; x < WIDTH; x++)
            if ((x >= COLS * 8 || y >= ROWS * 16) && at(x, y) != expect(black))
                ok = false;
    CHECK(ok, "the margins are black");
}

/** A frame writes the cells that changed and nothing else. */
static void test_damage(void)
{
    begin();
    put_char(0, 0, 'A', colors(orange, blue));
    frame();
    scribble(4, 1);
    scribble(0, 2);
    put_char(1, 0, 'A', colors(orange, blue));
    put_char(0, 2, 'A', colors(blue, orange));
    frame();
    CHECK(cell_shows(1, 0, glyph_a, orange, blue), "a changed cell is drawn");
    CHECK(cell_shows(0, 2, glyph_a, blue, orange), "a changed cell of the last row is drawn");
    CHECK(scribbled(4, 1), "a cell that did not change is not written");

    term_draw_redraw(&draw);
    frame();
    CHECK(cell_shows(4, 1, NULL, orange, blue), "a redraw writes every cell");
}

/** Bold, italic, underline, dim and inverse, each drawn apart. */
static void test_attributes(void)
{
    struct tsm_screen_attr a;
    unsigned char want[16];
    unsigned y;

    begin();
    a = colors(orange, blue);
    a.bold = 1;
    put_char(0, 0, 'A', a);
    for (y = 0; y < 16; y++)
        want[y] = (unsigned char)(glyph_a[y] | glyph_a[y] >> 1);
    a = colors(orange, blue);
    a.underline = 1;
    put_char(1, 0, 'A', a);
    a = colors(orange, blue);
    a.inverse = 1;
    put_char(2, 0, 'A', a);
    a = colors(orange, blue);
    a.dim = 1;
    put_char(3, 0, 'A', a);
    a = colors(orange, blue);
    a.underline = 1;
    put_char(4, 0, ' ', a);
    frame();
    CHECK(cell_shows(0, 0, want, orange, blue), "bold widens each stroke by a pixel");
    memcpy(want, glyph_a, 16);
    want[15] = 0xFF;
    CHECK(cell_shows(1, 0, want, orange, blue), "underline fills the last row");
    CHECK(cell_shows(2, 0, glyph_a, blue, orange), "inverse swaps the colors");
    {
        struct rgb half = {127, 64, 127};
        CHECK(cell_shows(3, 0, glyph_a, half, blue), "dim puts the foreground halfway");
    }
    memset(want, 0, 16);
    want[15] = 0xFF;
    CHECK(cell_shows(4, 0, want, orange, blue), "an underlined space shows its underline");

    a = colors(orange, blue);
    a.italic = 1;
    put_char(0, 1, 'A', a);
    frame();
    for (y = 0; y < 16; y++)
        want[y] = (unsigned char)(glyph_a[y] >> (15 - y) / 5);
    CHECK(cell_shows(0, 1, want, orange, blue), "italic slants the rows above the base");
}

/** A wide character covers two cells; a change of its second redraws it. */
static void test_wide(void)
{
    uint32_t cjk = 0x4E2D, none = 0;

    begin();
    put_cell(0, 1, &cjk, 1, 2, colors(orange, blue));
    put_cell(1, 1, &none, 0, 0, colors(orange, blue));
    frame();
    CHECK(shows(0, 16, 16, glyph_cjk, 16, 1, orange, blue), "the CJK glyph spans two cells");

    scribble(1, 1);
    model.cell[1][1].age = ++model.age;
    frame();
    CHECK(shows(0, 16, 16, glyph_cjk, 16, 1, orange, blue),
          "a change of the second cell redraws the whole glyph");

    /* An A written over the first cell leaves the second of width 0. */
    put_char(0, 1, 'A', colors(orange, blue));
    frame();
    CHECK(cell_shows(0, 1, glyph_a, orange, blue), "A replaces the first half");
    CHECK(cell_shows(1, 1, NULL, orange, blue), "the orphaned second half is drawn empty");

    /* The last column cuts a wide glyph at the screen's edge. */
    put_cell(COLS - 1, 0, &cjk, 1, 2, colors(orange, blue));
    frame();
    CHECK(shows((COLS - 1) * 8, 0, 8, glyph_cjk, 16, 1, orange, blue),
          "a wide glyph in the last column shows its left half");
}

/** A combining mark is drawn over its base; a missing glyph is U+FFFD's. */
static void test_combining(void)
{
    uint32_t seq[2] = {'A', 0x301};
    unsigned char want[16];

    begin();
    put_cell(2, 1, seq, 2, 1, colors(orange, blue));
    put_char(3, 1, 0x2603, colors(orange, blue));
    frame();
    memcpy(want, glyph_a, 16);
    want[0] |= 0x0C;
    want[1] |= 0x30;
    CHECK(cell_shows(2, 1, want, orange, blue), "the acute is drawn over the A");
    CHECK(cell_shows(3, 1, glyph_fffd, orange, blue), "a character the font lacks is U+FFFD");
}

/** The cursor's shapes, its moves, its visibility and the scrollback's shift. */
static void test_cursor(void)
{
    unsigned char want[16];

    begin();
    model.cursor_x = 1;
    model.cursor_y = 0;
    model.style = TSM_SCREEN_CURSOR_BLOCK_STEADY;
    frame();
    CHECK(cell_shows(1, 0, NULL, blue, orange), "a block cursor inverts its cell");

    model.cursor_x = 2;
    frame();
    CHECK(cell_shows(1, 0, NULL, orange, blue), "the cell the cursor left is drawn again");
    CHECK(cell_shows(2, 0, NULL, blue, orange), "the cursor is drawn where it went");

    model.style = TSM_SCREEN_CURSOR_UNDERLINE_BLINK;
    frame();
    memset(want, 0, 16);
    want[14] = want[15] = 0xFF;
    CHECK(cell_shows(2, 0, want, orange, blue), "an underline cursor fills two rows");

    model.style = TSM_SCREEN_CURSOR_VBAR_STEADY;
    frame();
    memset(want, 0xC0, 16);
    CHECK(cell_shows(2, 0, want, orange, blue), "a bar cursor fills two columns");

    model.flags |= TSM_SCREEN_HIDE_CURSOR;
    frame();
    CHECK(cell_shows(2, 0, NULL, orange, blue), "a hidden cursor is not drawn");
    model.flags &= ~(unsigned)TSM_SCREEN_HIDE_CURSOR;

    /* Two lines of scrollback shown move the cursor down two rows. */
    model.style = TSM_SCREEN_CURSOR_DEFAULT;
    model.sb_count = 5;
    model.sb_pos = 3;
    model.cursor_x = 0;
    model.cursor_y = 0;
    frame();
    CHECK(cell_shows(0, 2, NULL, blue, orange), "the cursor moves down with the scrollback");
    CHECK(cell_shows(0, 0, NULL, orange, blue), "and not on the row it has on the screen");
}

/** The bell shows the screen inverted for TERM_DRAW_BELL_MS. */
static void test_bell(void)
{
    begin();
    frame();
    CHECK(term_draw_timeout(&draw) == -1, "no frame is due after the screen is drawn");
    screen.bell = true;
    frame();
    CHECK(!screen.bell, "drawing takes the bell");
    CHECK(cell_shows(1, 1, NULL, blue, orange), "the bell inverts every cell");
    CHECK(term_draw_timeout(&draw) == TERM_DRAW_BELL_MS, "a frame is due when the bell ends");
    clock_ms += TERM_DRAW_BELL_MS;
    CHECK(term_draw_timeout(&draw) == 0, "the bell's end is due");
    frame();
    CHECK(cell_shows(1, 1, NULL, orange, blue), "the screen comes back after the bell");
    CHECK(term_draw_timeout(&draw) == -1, "nothing is due after the bell");
}

/** Mode 2026 holds the frame until it ends or its bound passes. */
static void test_hold(void)
{
    unsigned draws;

    begin();
    frame();
    model.held = true;
    put_char(1, 1, 'A', colors(orange, blue));
    draws = model.draws;
    frame();
    CHECK(model.draws == draws, "a held screen is not drawn");
    CHECK(cell_shows(1, 1, NULL, orange, blue), "the last frame stays while the hold lasts");
    clock_ms += 100;
    screen.bell = true;
    CHECK(term_draw_frame(&draw, &screen) == 0 && model.draws == draws,
          "the hold keeps the bell's frame back");
    CHECK(term_draw_timeout(&draw) == TERM_DRAW_HOLD_MS - 100, "a frame is due at the bound");
    clock_ms += TERM_DRAW_HOLD_MS - 100;
    frame();
    CHECK(model.released && !model.held, "the hold is ended at its bound");
    CHECK(model.draws == draws + 1, "and the screen is drawn");
    CHECK(cell_shows(1, 1, glyph_a, blue, orange), "with what changed, under the bell");
    clock_ms += TERM_DRAW_BELL_MS;
    frame();
}

/** Changing the scale resizes the screen and the pty and keeps the scale. */
static void test_scale(const char *dir)
{
    char conf[4096];
    FILE *f;
    char text[256];
    size_t n;

    begin();
    snprintf(conf, sizeof conf, "%s/term.conf", dir);
    f = fopen(conf, "w");
    CHECK(f != NULL, "the configuration file is made");
    if (!f)
        return;
    fputs("# term's settings\nfont-scale=1\nother=x", f);
    fclose(f);
    CHECK(term_font_conf_scale(conf) == 1, "the configuration reads font-scale=1");

    put_char(0, 0, 'A', colors(orange, blue));
    screen.pty = &pty; /* the model of term_pty_resize() reads nothing of it */
    CHECK(term_draw_rescale(&draw, &screen, 2, conf) == 0, "the scale changes to 2");
    CHECK(draw.cols == 3 && draw.rows == 1, "at 2x the screen is 3 by 1 cells");
    CHECK(model.cols == 3 && model.rows == 1, "libtsm's screen is told the new size");
    CHECK(pty_cols == 3 && pty_rows == 1, "the pty is told the new size");
    frame();
    CHECK(shows(0, 0, 16, glyph_a, 8, 2, orange, blue), "A is drawn at 2x");
    CHECK(term_font_conf_scale(conf) == 2, "the configuration keeps font-scale=2");
    f = fopen(conf, "r");
    n = f ? fread(text, 1, sizeof text - 1, f) : 0;
    if (f)
        fclose(f);
    text[n] = '\0';
    CHECK(strcmp(text, "# term's settings\nfont-scale=2\nother=x") == 0,
          "the other lines of the configuration are kept");
    CHECK(term_draw_rescale(&draw, &screen, 3, NULL) == -EINVAL, "a scale of 3 is refused");
    CHECK(draw.cols == 3, "and leaves the screen as it was");
    unlink(conf);

    CHECK(term_font_conf_save(conf, 1) == 0, "a missing configuration is made");
    CHECK(term_font_conf_scale(conf) == 1, "with the scale");
    unlink(conf);
    CHECK(term_font_conf_scale(conf) == 0, "a missing configuration names no scale");
    screen.pty = NULL;
}

/** term_draw_init refuses what it cannot draw on. */
static void test_refused(void)
{
    struct fb_screen info;
    struct term_draw d;

    memset(&info, 0, sizeof info);
    info.width = WIDTH;
    info.height = HEIGHT;
    info.pitch = WIDTH;
    info.bpp = 8;
    info.kind = FB_KIND_INDEXED;
    CHECK(term_draw_init(&d, &font, -1, &info) == -ENODEV, "an indexed framebuffer is refused");
    info.kind = FB_KIND_RGB;
    CHECK(term_draw_init(&d, &font, -1, &info) == -ENODEV, "RGB at 8 bits is refused");
    info.bpp = 32;
    CHECK(term_draw_init(&d, &font, -1, &info) == -ENODEV, "a pitch short of a row is refused");
}

/** Write the packed glyphs to a file in `dir` and load them as the font. */
static int load_font(const char *dir)
{
    unsigned long size, line;
    const char *err = "";
    unsigned char *data;
    char path[4096];
    FILE *f;
    int r;

    data = mkunifont(font_text, strlen(font_text), &size, &line, &err);
    CHECK(data != NULL, "the glyphs pack");
    if (!data)
        return -1;
    snprintf(path, sizeof path, "%s/unifont.font", dir);
    f = fopen(path, "wb");
    CHECK(f != NULL && fwrite(data, 1, size, f) == size, "the font is written");
    if (f)
        fclose(f);
    free(data);
    CHECK(term_font_load(&font, path, 3) == -EINVAL, "a scale of 3 is refused");
    r = term_font_load(&font, path, 1);
    CHECK(r == 0, "the font loads at 1x");
    unlink(path);
    return r;
}

/** term_font_load() refuses a missing file and one that is not a font. */
static void test_font_refused(const char *dir)
{
    struct term_font f;
    char path[4096];
    FILE *out;

    snprintf(path, sizeof path, "%s/none.font", dir);
    CHECK(term_font_load(&f, path, 1) == -ENOENT, "a missing font is ENOENT");
    out = fopen(path, "w");
    if (out) {
        fputs("R2UFONT0 is not the magic", out);
        fclose(out);
    }
    CHECK(term_font_load(&f, path, 1) == -EINVAL && f.data == NULL,
          "a file that is not a packed font is EINVAL");
    unlink(path);
}

int main(void)
{
    char dir[4096];
    unsigned i;

    snprintf(dir, sizeof dir, "%s/term_draw_dir.XXXXXX",
             getenv("TMPDIR") ? getenv("TMPDIR") : "/tmp");
    if (!mkdtemp(dir)) {
        perror("term_draw: mkdtemp");
        return 1;
    }
    test_font_refused(dir);
    if (load_font(dir) == 0) {
        test_refused();
        for (i = 0; i < sizeof formats / sizeof formats[0]; i++) {
            fprintf(stderr, "term_draw: %s\n", formats[i].name);
            if (open_fb(&formats[i]) < 0) {
                close_fb();
                continue;
            }
            test_first_frame();
            test_damage();
            test_attributes();
            test_wide();
            test_combining();
            test_cursor();
            test_bell();
            test_hold();
            if (i == 0)
                test_scale(dir);
            close_fb();
        }
        term_font_free(&font);
    }
    rmdir(dir);
    if (fails) {
        fprintf(stderr, "term_draw: %d failures\n", fails);
        return 1;
    }
    printf("term_draw: ok\n");
    return 0;
}
