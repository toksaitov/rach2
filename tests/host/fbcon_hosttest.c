/*
 * fbcon_hosttest.c: the text console on a framebuffer, drawn into memory.
 *
 * host-deps: rach2/sys/lib/kfmt.c rach2/sys/lib/kstd.c
 * host-deps: rach2/sys/lib/kstring.c rach2/sys/lib/kctypes.c
 * host-deps: tests/host/compat/spinlock_host.c
 *
 * The framebuffer is an array here, which `fbcon_window` hands the console as
 * the loader's framebuffer, and the rows read its pixels back against the font:
 * a glyph drawn in its colors, the cursor's two lines, a Cyrillic letter
 * reached through UTF-8, the font at the largest whole scale at which the
 * screen still holds 80 columns and 24 rows with as many cells as fit, within
 * 240 by 64, pixels of 32, 24, 16 and 8 bits, and the cells centered. A scroll
 * must draw the lines that moved and leave a blank cell under a blank one
 * alone, which a pixel marked in such a cell shows; a clear must take the
 * cursor's lines from the cell it left; and the scrollback must draw the lines
 * it shows. The 320x200 picture of Mode 13h must be drawn at the largest whole
 * scale that fits, in the middle, with no cell over it until the console takes
 * the screen back. The text console's driver, the console and the framebuffer's
 * drawing are included whole, as vga_text_hosttest.c includes them.
 */
#include "lib/kint.h"

#define VGA_TEXT_HOSTTEST
#define KIO_HOSTTEST

static uint16_t vram[16384];
#define VGA_TEXT_BUFFER vram

extern int printf(const char *format, ...);
extern void *memset(void *s, int c, unsigned long n);

void vga_text_hosttest_stored(void)
{}

uint32_t kirq_save(void)
{
    return 0;
}

void kirq_restore(uint32_t flags)
{
    (void)flags;
}

#include "display/vga_gfx.h"
bool vga_graphics_mode = false;

#include "display/vga_text.c"
#include "display/fbcon.c"
#include "display/vga_font.c"
#include "kio.c"

/* No processor here has begun a panic, so the console takes its lock. */
int k_panicking(void)
{
    return 0;
}

int vga_font_load(void)
{
    return 0;
}

void klog(int level, const char *subsystem, const char *format, ...)
{
    (void)level;
    (void)subsystem;
    (void)format;
}

static unsigned int port_writes;

void outb(uint16_t port, uint8_t val)
{
    (void)port;
    (void)val;
    port_writes++;
}

/* The framebuffer: room for 2560x1600 at 32 bits, with the 64 bytes past
 * each line that `use_rgb` gives the pitch. */
static uint8_t screen[(2560 * 4 + 64) * 1600];
static int reachable = 1;

volatile uint8_t *fbcon_window(void)
{
    return reachable ? screen : NULL;
}

static int failures;

#define CHECK(condition, detail)                      \
    do {                                              \
        if (!(condition)) {                           \
            printf("fbcon-test: fail: %s\n", detail); \
            failures++;                               \
        }                                             \
    } while (0)

static struct boot_framebuffer fb;

static void put(const char *s)
{
    while (*s) {
        k_putc(*s++);
    }
}

/** The console as the boot leaves it: the font chosen, the screen clear,
 * the colors light grey on black. */
static void start_console(void)
{
    k_font_init();
    put("\033[0m");
    k_cls();
}

/** A direct-color framebuffer of `w` by `h` pixels at `bpp` bits, red at
 * bit 16, green at 8 and blue at 0, or five, six and five bits at 16. */
static int use_rgb(uint32_t w, uint32_t h, uint8_t bpp)
{
    fb.type = BOOT_FB_RGB;
    fb.addr = 0x80000000u;
    fb.width = w;
    fb.height = h;
    fb.bpp = bpp;
    fb.pitch = w * ((bpp + 7u) / 8u) + 64;
    memset(screen, 0x5A, (unsigned long)fb.pitch * h);
    if (bpp == 16) {
        fb.red_shift = 11;
        fb.red_bits = 5;
        fb.green_shift = 5;
        fb.green_bits = 6;
        fb.blue_shift = 0;
        fb.blue_bits = 5;
    } else {
        fb.red_shift = 16;
        fb.red_bits = 8;
        fb.green_shift = 8;
        fb.green_bits = 8;
        fb.blue_shift = 0;
        fb.blue_bits = 8;
    }
    if (fbcon_init(&fb) != 0) {
        return -1;
    }
    start_console();
    return 0;
}

static uint32_t pixel_at(uint32_t px, uint32_t py)
{
    const uint8_t *p = screen + py * fb.pitch + px * fb_bytes;

    switch (fb_bytes) {
    case 4:
        return p[0] | p[1] << 8 | p[2] << 16 | (uint32_t)p[3] << 24;
    case 3:
        return p[0] | p[1] << 8 | (uint32_t)p[2] << 16;
    case 2:
        return p[0] | (uint32_t)p[1] << 8;
    default:
        return p[0];
    }
}

/**
 * Whether the cell at (`col`, `row`) of the screen shows glyph `ch` in
 * pixels `fg` on `bg`, with the cursor's lines when `cursor` is set.
 */
static int cell_shows(uint32_t col, uint32_t row, unsigned char ch, uint32_t fg, uint32_t bg,
                      int cursor)
{
    uint32_t left = grid_left + col * 8 * fb_scale;
    uint32_t top = grid_top + row * 16 * fb_scale;
    uint32_t r, c;

    for (r = 0; r < 16 * fb_scale; r++) {
        uint32_t bits = vga_font_all[ch].rows[r / fb_scale];

        if (cursor && r / fb_scale >= 13 && r / fb_scale <= 14) {
            bits = 0xFF;
        }
        for (c = 0; c < 8 * fb_scale; c++) {
            uint32_t want = (bits & (0x80u >> (c / fb_scale))) ? fg : bg;

            if (pixel_at(left + c, top + r) != want) {
                return 0;
            }
        }
    }
    return 1;
}

#define GREY  0xAAAAAAu
#define BLACK 0x000000u

static void row_init(void)
{
    uint32_t px, py;
    int clear = 1;

    CHECK(use_rgb(1280, 800, 32) == 0, "a 1280x800 framebuffer is taken");
    CHECK(fbcon_active && textcon_on_framebuffer(), "the console is on the framebuffer");
    CHECK(fb_scale == 2 && textcon_cols == 80 && textcon_rows == 25 && grid_left == 0 &&
              grid_top == 0,
          "the cells are 80x25 at twice the font's size, filling the screen");
    for (py = 0; py < 800 && clear; py++) {
        for (px = 0; px < 1280; px++) {
            int in_first_cell = px < 16 && py < 32;

            if (!in_first_cell && pixel_at(px, py) != BLACK) {
                clear = 0;
                break;
            }
        }
    }
    CHECK(clear, "the whole framebuffer is cleared, outside the cells too");
    CHECK(cell_shows(0, 0, ' ', GREY, BLACK, 1), "and the cursor is at the first cell");
    CHECK(screen[1280 * 4] == 0x5A, "the bytes past a line's pixels are left alone");
    CHECK(textcon_load_font() == vga_font_map, "the font is the console's own");

    fb.width = 600;
    CHECK(fbcon_init(&fb) == -1, "a framebuffer narrower than 640 pixels is refused");
    fb.width = 1280;
    fb.height = 383;
    CHECK(fbcon_init(&fb) == -1, "one shorter than 384 pixels, 24 rows, is refused");
    fb.height = 800;
    fb.pitch = 1280 * 4 - 4;
    CHECK(fbcon_init(&fb) == -1, "a pitch under a line's pixels is refused");
    fb.width = 0x40000280u;
    fb.pitch = 2560;
    CHECK(fbcon_init(&fb) == -1, "a width whose bytes wrap 32 bits to the pitch is refused");
    fb.width = 1280;
    fb.pitch = 1280 * 4;
    fb.type = BOOT_FB_INDEXED;
    fb.bpp = 16;
    CHECK(fbcon_init(&fb) == -1, "an indexed framebuffer of 16 bits is refused");
    fb.type = BOOT_FB_TEXT;
    CHECK(fbcon_init(&fb) == -1, "the text mode is not a framebuffer");
    fb.type = BOOT_FB_RGB;
    fb.bpp = 32;
    reachable = 0;
    CHECK(fbcon_init(&fb) == -1, "a framebuffer out of reach is refused");
    reachable = 1;
}

static void row_glyphs(void)
{
    use_rgb(1280, 800, 32);
    put("A");
    CHECK(cell_shows(0, 0, 'A', GREY, BLACK, 0), "A in light grey on black");
    textcon_place_cursor();
    CHECK(cell_shows(1, 0, ' ', GREY, BLACK, 1), "the cursor underlines the next cell");
    put("\033[1;33;44mZ\033[0m");
    CHECK(cell_shows(1, 0, 'Z', 0xFFFF55u, 0x0000AAu, 0), "Z in yellow on blue");
    textcon_place_cursor();
    CHECK(cell_shows(1, 0, 'Z', 0xFFFF55u, 0x0000AAu, 0) && cell_shows(2, 0, ' ', GREY, BLACK, 1),
          "the cursor leaves the cell it was on");
    put("\xD0\x96");
    CHECK(cell_shows(2, 0, 0x86, GREY, BLACK, 0) && vga_font_all[0x86].rows[6] == 0x2A,
          "the Cyrillic Zhe from UTF-8, at its place in the font");
    put("\xE2\x95\x94");
    CHECK(cell_shows(3, 0, 0xC9, GREY, BLACK, 0), "a box corner, which the adapter's own font had");
}

static void row_pixel_sizes(void)
{
    CHECK(use_rgb(1024, 768, 16) == 0, "a 16-bit framebuffer is taken");
    put("\033[31mR");
    CHECK(cell_shows(0, 0, 'R', (0xAAu >> 3) << 11, 0, 0), "red at five bits of red");
    put("\033[0;1;32mG");
    CHECK(cell_shows(1, 0, 'G', (0x55u >> 3) << 11 | (0xFFu >> 2) << 5 | 0x55u >> 3, 0, 0),
          "light green at five, six and five bits");

    CHECK(use_rgb(800, 600, 24) == 0, "a 24-bit framebuffer is taken");
    CHECK(fb_scale == 1 && textcon_cols == 100 && textcon_rows == 37 && grid_left == 0 &&
              grid_top == 4,
          "800x600 holds 100x37 cells at the font's size, centered");
    put("\033[0m\033[45mM");
    CHECK(cell_shows(0, 0, 'M', GREY, 0xAA00AAu, 0), "magenta in three bytes a pixel");

    /* Indexed, eight bits, with a palette whose nearest entries are not at
       the colors' own numbers. */
    {
        unsigned i;

        for (i = 0; i < 256; i++) {
            fb.palette[i][0] = 0x10;
            fb.palette[i][1] = 0x10;
            fb.palette[i][2] = 0x10;
        }
        fb.palette[200][0] = 0xAA;
        fb.palette[200][1] = 0xAA;
        fb.palette[200][2] = 0xAA;
        fb.palette[9][0] = 0;
        fb.palette[9][1] = 0;
        fb.palette[9][2] = 0;
        fb.colors = 256;
        fb.type = BOOT_FB_INDEXED;
        fb.bpp = 8;
        fb.width = 640;
        fb.height = 480;
        fb.pitch = 640;
        CHECK(fbcon_init(&fb) == 0, "an indexed framebuffer of 8 bits is taken");
        start_console();
        put("I");
        CHECK(cell_shows(0, 0, 'I', 200, 9, 0), "light grey and black at their nearest entries");
        fb.colors = 0;
        CHECK(fbcon_init(&fb) == 0, "an indexed framebuffer without a palette is taken");
        start_console();
        put("\033[34mI");
        CHECK(cell_shows(0, 0, 'I', 1, 0, 0), "without a palette, a color is its own index");
        put("\033[0m");
    }
}

/**
 * The font's scale is the largest whole number at which the screen still
 * holds 80 columns and 24 rows, and the cells are as many as fit at it,
 * centered, within 240 columns and 64 rows.
 */
static void row_double(void)
{
    CHECK(use_rgb(1366, 768, 32) == 0 && fb_scale == 2 && textcon_cols == 85 &&
              textcon_rows == 24 && grid_left == 3 && grid_top == 0,
          "1366x768 holds 85x24 cells at twice the font's size");
    put("\xD0\xAF");
    CHECK(cell_shows(0, 0, 0x9F, GREY, BLACK, 0), "Ya in squares of four pixels");
    textcon_place_cursor();
    CHECK(cell_shows(1, 0, ' ', GREY, BLACK, 1), "the cursor's two lines are four");

    CHECK(use_rgb(2560, 1600, 32) == 0 && fb_scale == 4 && textcon_cols == 80 &&
              textcon_rows == 25 && grid_left == 0 && grid_top == 0,
          "2560x1600 holds 80x25 at four times the size");
    CHECK(use_rgb(2048, 768, 32) == 0 && fb_scale == 2 && textcon_cols == 128 &&
              textcon_rows == 24,
          "a screen wide but low takes twice the size, which still gives 24 rows");
    CHECK(use_rgb(1024, 768, 32) == 0 && fb_scale == 1 && textcon_cols == 128 &&
              textcon_rows == 48,
          "1024x768 keeps the font's size, since twice gives 64 columns");
    CHECK(use_rgb(2560, 384, 32) == 0 && fb_scale == 1 && textcon_cols == 240 &&
              grid_left == 320,
          "a screen of more than 240 columns gives 240, centered");
    CHECK(use_rgb(640, 1600, 32) == 0 && fb_scale == 1 && textcon_rows == 64 && grid_top == 288,
          "and one of more than 64 rows gives 64");
}

static void row_scroll(void)
{
    char line[16];
    unsigned n, marked;

    use_rgb(1280, 800, 32);
    for (n = 0; n < 25; n++) {
        line[0] = 'a' + n;
        line[1] = '\0';
        put(line);
        if (n < 24) {
            put("\n");
        }
    }
    CHECK(cell_shows(0, 0, 'a', GREY, BLACK, 0) && cell_shows(0, 24, 'y', GREY, BLACK, 0),
          "a screen of lines a to y");
    /* A pixel marked in a blank cell that stays blank. */
    marked = (grid_top + 10 * 16 + 3) * fb.pitch + (grid_left + 70 * 8 + 3) * 4;
    screen[marked] = 0x77;
    /* And in the cell of a line that moves. */
    screen[(grid_top + 3) * fb.pitch + (grid_left + 3) * 4] = 0x77;
    put("\nz");
    CHECK(cell_shows(0, 0, 'b', GREY, BLACK, 0) && cell_shows(0, 23, 'y', GREY, BLACK, 0) &&
              cell_shows(0, 24, 'z', GREY, BLACK, 0),
          "a scroll moves every line up one");
    CHECK(screen[marked] == 0x77, "a blank cell under a blank one is not drawn again");
    CHECK(cell_shows(0, 0, 'b', GREY, BLACK, 0), "a cell whose line moved is drawn again");

    /* Past the end of the adapter's memory: the copy to its front. */
    for (n = 0; n < 200; n++) {
        line[0] = '0' + n % 10;
        line[1] = '\0';
        put("\n");
        put(line);
    }
    CHECK(cell_shows(0, 24, '9', GREY, BLACK, 0) && cell_shows(0, 23, '8', GREY, BLACK, 0) &&
              cell_shows(0, 0, '5', GREY, BLACK, 0),
          "two hundred scrolls, across the copy, show the last 25 lines");

    k_scrollback_page_up();
    CHECK(cell_shows(0, 0, '0', GREY, BLACK, 0) && cell_shows(0, 24, '4', GREY, BLACK, 0),
          "a page back shows the 25 lines before");
    k_scrollback_end();
    CHECK(cell_shows(0, 24, '9', GREY, BLACK, 0), "and the end shows the live screen again");
}

/** A redraw of the screen takes the cursor from the cell it was on,
 * wherever it meets the new one first. */
static void row_cursor_redraw(void)
{
    use_rgb(1280, 800, 32);
    put("\033[25;23H");
    textcon_place_cursor();
    k_scroll_down();
    CHECK(cell_shows(22, 23, ' ', GREY, BLACK, 1),
          "a scroll carries the placed cursor up a line with its cell");
    CHECK(cell_shows(22, 24, ' ', GREY, BLACK, 0),
          "and takes it from the blank cell below, which the redraw meets after");
    put("\033[25;23H");
    textcon_place_cursor();
    CHECK(cell_shows(22, 24, ' ', GREY, BLACK, 1), "the cursor at the foot of the screen");
    put("\033[H\033[J");
    textcon_place_cursor();
    CHECK(cell_shows(22, 24, ' ', GREY, BLACK, 0), "a clear leaves no cursor at the foot");
    CHECK(cell_shows(0, 0, ' ', GREY, BLACK, 1), "and puts it at the top");
    put("\033[25;23H");
    textcon_place_cursor();
    k_cls();
    textcon_place_cursor();
    CHECK(cell_shows(22, 24, ' ', GREY, BLACK, 0) && cell_shows(0, 0, ' ', GREY, BLACK, 1),
          "nor does the kernel's own clear");
}

/** The panic screen: every cell a background color, white on black after. */
static void row_panic_colors(void)
{
    unsigned i;

    use_rgb(1280, 800, 32);
    for (i = 0; i < 80; i++) {
        set_back_color(i % 2 ? blue : white);
        k_putc(' ');
    }
    CHECK(cell_shows(0, 0, ' ', GREY, 0xFFFFFFu, 0) && cell_shows(1, 0, ' ', GREY, 0x0000AAu, 0),
          "a white and a blue background, as the panic picture has them");
    set_back_color(black);
}

/**
 * The 320x200 picture of Mode 13h on a 1366x768 screen of 32 bits: drawn at
 * three pixels to one, the largest whole number that fits, in the middle,
 * 960x600 from (203, 84), each of its pixels a square of its palette color;
 * the screen around it black; no cell drawn while it has the screen, and the
 * cells, those written meanwhile among them, drawn again on a black screen
 * once it gives the screen back.
 */
static void row_picture(void)
{
    static uint8_t rgb[768], picture[320 * 200];
    uint32_t r, c, square = 1;

    use_rgb(1366, 768, 32);
    put("text");
    rgb[5 * 3 + 0] = 0x10;
    rgb[5 * 3 + 1] = 0x20;
    rgb[5 * 3 + 2] = 0x30;
    picture[0] = 5;
    picture[320 * 200 - 1] = 5;
    fbcon_gfx_begin();
    fbcon_gfx_palette(rgb);
    fbcon_gfx_frame(picture);
    for (r = 0; r < 3; r++) {
        for (c = 0; c < 3; c++) {
            square = square && pixel_at(203 + c, 84 + r) == 0x102030u &&
                     pixel_at(203 + 957 + c, 84 + 597 + r) == 0x102030u;
        }
    }
    CHECK(square, "the picture's corner pixels are squares of three at (203, 84) and its far corner");
    CHECK(pixel_at(206, 84) == BLACK && pixel_at(202, 84) == BLACK && pixel_at(203, 83) == BLACK &&
              pixel_at(0, 0) == BLACK && pixel_at(1365, 767) == BLACK,
          "the picture's next pixel and the screen around it are black");
    put("more");
    CHECK(pixel_at(203, 84) == 0x102030u && !cell_shows(4, 0, 'm', GREY, BLACK, 0),
          "no cell is drawn while the picture has the screen");
    fbcon_gfx_end();
    CHECK(pixel_at(203, 84) == BLACK, "the screen is black once the picture gives it back");
    CHECK(cell_shows(0, 0, 't', GREY, BLACK, 0) && cell_shows(4, 0, 'm', GREY, BLACK, 0),
          "and the cells are drawn again, one written while the picture had the screen too");
}

int main(void)
{
    row_init();
    row_glyphs();
    row_pixel_sizes();
    row_double();
    row_scroll();
    row_cursor_redraw();
    row_panic_colors();
    row_picture();
    if (failures) {
        printf("fbcon-test: %d failure(s)\n", failures);
        return 1;
    }
    printf("fbcon-test: OK\n");
    return 0;
}
