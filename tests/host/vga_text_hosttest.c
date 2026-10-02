/*
 * vga_text_hosttest.c: the text console driver against a model adapter.
 *
 * host-deps: rach2/sys/lib/kfmt.c rach2/sys/lib/kstd.c
 * host-deps: rach2/sys/lib/kstring.c rach2/sys/lib/kctypes.c
 *
 * The adapter's memory is an array here, every store to it is counted, and
 * its ports are recorded, so the rows count what each operation costs. A
 * byte is one cell. A scroll is the new line's eighty cells and the start
 * address, with the rows above left in the cells they were in. The copy to
 * the front of the memory comes only when the window would pass its end,
 * once in 180 scrolls, and carries its content. A scroll back and a clear
 * redraw the screen. A held cursor is placed once, at the release. The
 * twenty-five rows the start address selects are the history's live lines
 * throughout, including across the copy. And the console's own byte path,
 * `k_putc` and the writers over it, costs the same, which is what the
 * driver was rewritten for. The driver and the console are included whole
 * to reach their state.
 */
#include "lib/kint.h"

#define VGA_TEXT_HOSTTEST
#define KIO_HOSTTEST

static uint16_t vram[16384];
#define VGA_TEXT_BUFFER vram

extern int printf(const char *format, ...);

static unsigned int stores;

/* The driver's one store to the adapter's memory, counted. */
void vga_text_hosttest_stored(void)
{
    stores++;
}

/* The flag register's stand-in: the console keeps interrupts off around a
   byte, and the rows watch that it does. */
static int masked;
static int inject_echo;
static int echo_pending;
static void deliver_echo(void);

uint32_t kirq_save(void)
{
    uint32_t old = (uint32_t)masked;

    masked = 1;
    return old;
}

void kirq_restore(uint32_t flags)
{
    masked = (int)flags;
    deliver_echo();
}

#include "display/vga_gfx.h"
bool vga_graphics_mode = false;

#include "display/vga_text.c"
#include "display/vga_font.c"
#include "kio.c"

static int font_reads_back;

/* The adapter's character memory: whether the glyphs read back as written. */
int vga_font_load(void)
{
    return font_reads_back;
}

/* The font's load is said in the kernel log, which the rows do not read. */
void klog(int level, const char *subsystem, const char *format, ...)
{
    (void)level;
    (void)subsystem;
    (void)format;
}

static unsigned int port_writes;
static unsigned int start_writes;
static uint8_t crtc_index;
static uint16_t start_address;
static uint16_t cursor_address;
static int failures;

#define CHECK(condition, detail)                         \
    do {                                                 \
        if (!(condition)) {                              \
            printf("vga-text-test: fail: %s\n", detail); \
            failures++;                                  \
        }                                                \
    } while (0)

/* The CRTC as the driver sees it: an index port and a data port, of which
   the start address and the cursor location are kept. */
void outb(uint16_t port, uint8_t val)
{
    port_writes++;
    if (port == 0x3D4) {
        crtc_index = val;
        if (val == 0x0E && inject_echo) {
            inject_echo = 0;
            echo_pending = 1;
            deliver_echo();
        }
    } else if (port == 0x3D5) {
        switch (crtc_index) {
        case 0x0C:
            start_address = (uint16_t)((start_address & 0x00FFu) | ((uint16_t)val << 8));
            start_writes++;
            break;
        case 0x0D:
            start_address = (uint16_t)((start_address & 0xFF00u) | val);
            start_writes++;
            break;
        case 0x0E:
            cursor_address = (uint16_t)((cursor_address & 0x00FFu) | ((uint16_t)val << 8));
            break;
        case 0x0F:
            cursor_address = (uint16_t)((cursor_address & 0xFF00u) | val);
            break;
        default:
            break;
        }
    }
}

/** A pending keyboard interrupt echoes one character with interrupts masked. */
static void deliver_echo(void)
{
    if (echo_pending && !masked) {
        echo_pending = 0;
        masked = 1;
        k_putc('x');
        masked = 0;
    }
}

/** Echo across a cursor-byte carry must not split the final hold's update. */
static void row_release_echo(void)
{
    int initial;

    for (initial = 0; initial <= 1; initial++) {
        k_cls();
        x = 15;
        y = 3;
        vga_text_hold();
        vga_text_place_cursor();
        masked = initial;
        inject_echo = 1;
        vga_text_release();
        CHECK(masked == initial, "release restores caller flags");
        CHECK(_hold == 0 && !_cursor_stale, "release completes outermost hold");
        if (initial) {
            CHECK(echo_pending && cursor_address == 0x00FF, "masked caller retains pending echo");
            kirq_restore(0);
        }
        CHECK(!echo_pending && x == 16 && y == 3, "keyboard echo runs once");
        CHECK(cursor_address == 0x0100, "echo leaves cursor at 0x0100");
        vga_text_release();
        CHECK(_hold == 0 && masked == 0, "extra release preserves state");
    }
}

/**
 * Whether the window the start address selects shows the viewport's lines,
 * cell for cell, from the history ring.
 */
static int window_shows_viewport(void)
{
    unsigned int row, col;

    if (start_address != _hw_top) {
        return 0;
    }
    for (row = 0; row < VGA_TEXT_ROWS; row++) {
        uint32_t line = _viewport_top_line + row;
        uint16_t *cells = vga_text_line_ptr(line);

        for (col = 0; col < VGA_TEXT_COLS; col++) {
            uint16_t want = cells != 0 ? cells[col] : vga_text_blank_cell();

            if (vram[_hw_top + row * VGA_TEXT_COLS + col] != want) {
                return 0;
            }
        }
    }
    return 1;
}

/** A clear draws the screen once and points the display at it. */
static void row_clear(void)
{
    stores = 0;
    k_cls();
    CHECK(stores == VGA_TEXT_SCREEN_CELLS, "a clear stores one screen");
    CHECK(_hw_top == 0 && start_address == 0,
          "a clear starts the display at the front of the memory");
    CHECK(window_shows_viewport(), "a clear shows the blank history");
}

/** A byte is one cell and, placed, four port writes. */
static void row_byte(void)
{
    unsigned int ports;

    x = 1;
    y = 0;
    stores = 0;
    ports = port_writes;
    vga_text_write_cell(0, 0, 'A', 0x07);
    CHECK(stores == 1, "a byte stores one cell");
    CHECK(vram[0] == vga_text_make_cell('A', 0x07), "the byte's cell holds the byte");
    CHECK(port_writes == ports, "a byte alone touches no port");
    vga_text_place_cursor();
    CHECK(port_writes == ports + 4, "placing the cursor is four port writes");
    CHECK(cursor_address == 1, "the cursor is at the byte after the one written");
}

/** Held, the cursor is placed once at the release, wherever it went. */
static void row_hold(void)
{
    unsigned int ports = port_writes;

    vga_text_hold();
    vga_text_hold();
    x = 5;
    vga_text_place_cursor();
    x = 7;
    vga_text_place_cursor();
    vga_text_release();
    CHECK(port_writes == ports, "a nested hold keeps the cursor where it was");
    vga_text_release();
    CHECK(port_writes == ports + 4, "the last release places the cursor once");
    CHECK(cursor_address == 7, "and places it where it last went");
    ports = port_writes;
    vga_text_hold();
    vga_text_release();
    CHECK(port_writes == ports, "a hold in which the cursor did not move places nothing");
}

/**
 * 179 scrolls move the window down the memory, eighty stores and the start
 * address each, with the rows above left in their cells; the 180th would
 * leave the memory, so the rows that stay are copied to the front. Each
 * line gets a mark before the scroll, so the copy is checked by content.
 */
static void row_scrolls(void)
{
    unsigned int n, ports;

    for (n = 0; n < 179; n++) {
        vga_text_write_cell(0, VGA_TEXT_ROWS - 1, (char)('a' + (n % 26)), 0x07);
        stores = 0;
        ports = port_writes;
        k_scroll_down();
        CHECK(stores == VGA_TEXT_COLS, "a scroll stores the new line's cells and no other");
        CHECK(port_writes == ports + 4, "a scroll writes the start address and nothing else");
        CHECK(_hw_top == (n + 1) * VGA_TEXT_COLS, "a scroll moves the window down a line");
        CHECK(window_shows_viewport(), "the window shows the live lines after a scroll");
        if (failures) {
            printf("vga-text-test: at scroll %u\n", n + 1);
            return;
        }
    }
    CHECK(_hw_top == 179 * VGA_TEXT_COLS, "179 scrolls reach the end of the memory");
    vga_text_write_cell(0, VGA_TEXT_ROWS - 1, 'Z', 0x07);
    stores = 0;
    ports = port_writes;
    k_scroll_down();
    CHECK(_hw_top == 0 && start_address == 0,
          "the scroll at the end returns the window to the front");
    CHECK(stores == VGA_TEXT_SCREEN_CELLS,
          "the copy stores the rows that stay and the new line, one screen");
    CHECK(port_writes == ports + 4, "the copy writes the start address once");
    CHECK(window_shows_viewport(), "the window shows the live lines after the copy");
    CHECK(vram[(VGA_TEXT_ROWS - 2) * VGA_TEXT_COLS] == vga_text_make_cell('Z', 0x07),
          "the copied rows carry their content");
}

/**
 * Scrolled back, the screen is redrawn from history, live output does not
 * touch the adapter, and the return redraws the live lines. A viewport at
 * the oldest line is dragged forward when that line is evicted, and the
 * screen is redrawn then too.
 */
static void row_scrollback(void)
{
    unsigned int n;

    k_scrollback_page_up();
    CHECK(!_follow_live, "page up leaves the live lines");
    CHECK(window_shows_viewport(), "the window shows the history page");
    stores = 0;
    vga_text_write_cell(0, 0, 'Q', 0x07);
    CHECK(stores == 0, "live output while scrolled back leaves the adapter alone");
    k_scroll_down();
    CHECK(stores == 0, "a scroll while scrolled back leaves the adapter alone");
    k_scrollback_end();
    CHECK(_follow_live, "end returns to the live lines");
    CHECK(window_shows_viewport(), "the return shows the live lines, the byte among them");

    /* Fill the history, go to its oldest line, and scroll once more. */
    for (n = 0; n < VGA_TEXT_SCROLLBACK_LINES; n++) {
        k_scroll_down();
    }
    k_scrollback_home();
    CHECK(!_follow_live && _viewport_top_line == _history_start_line, "home is the oldest line");
    stores = 0;
    k_scroll_down();
    CHECK(_viewport_top_line == _history_start_line,
          "the viewport is dragged off the evicted line");
    CHECK(stores == VGA_TEXT_SCREEN_CELLS, "and the screen is redrawn for it");
    CHECK(window_shows_viewport(), "showing the oldest lines there are");
    k_scrollback_end();
}

/**
 * The console's own path: a byte through `k_putc` is one cell and one
 * placement, a line through `k_puts` is its cells and one placement, and
 * the work runs with interrupts off. `k_printf` reads its arguments off an
 * i386 stack and is not run here.
 */
static void row_console(void)
{
    unsigned int ports;

    k_cls();
    stores = 0;
    ports = port_writes;
    masked = 0;
    k_putc('A');
    CHECK(stores == 1, "a byte through k_putc stores one cell");
    CHECK(port_writes == ports + 4, "and places the cursor once");
    CHECK(masked == 0, "and gives interrupts back");
    stores = 0;
    ports = port_writes;
    k_puts("abc");
    CHECK(stores == 3, "a line through k_puts stores its cells");
    CHECK(port_writes == ports + 4, "and places the cursor once for the line");
}

/** Write `s` through the console's byte path. */
static void put_string(const char *s)
{
    while (*s != '\0') {
        k_putc(*s++);
    }
}

/** Whether the answer waiting is the `n` bytes of `want`, and take it. */
static int answer_is(const char *want, unsigned int n)
{
    char buf[K_ANSWER_MAX];
    unsigned int i;

    if (k_take_answer(buf, sizeof buf) != n) {
        return 0;
    }
    for (i = 0; i < n; i++) {
        if (buf[i] != want[i]) {
            return 0;
        }
    }
    return 1;
}

/**
 * A query is answered and not drawn: the device attributes as a VT102's, the
 * cursor's position one-based, and the status as good. A device control
 * string and an application string, which a browser sends to learn the
 * terminal's features, leave no cell behind.
 */
static void row_answers(void)
{
    k_cls();
    put_string("ab\033[c");
    CHECK(answer_is("\033[?6c", 5), "the device attributes are a VT102's");
    CHECK(answer_is("", 0), "an answer is taken once");
    put_string("\033[3;7H\033[6n");
    CHECK(answer_is("\033[3;7R", 6), "the cursor's position is one-based");
    put_string("\033[5n");
    CHECK(answer_is("\033[0n", 4), "the status is good");
    stores = 0;
    put_string("\033P+q524742\033\\\033_Gi=1,a=q\033\\\033]11;?\007");
    CHECK(stores == 0 && x == 6 && y == 2, "the strings draw nothing");
}

/**
 * UTF-8 is drawn from the font, code page 437, a cell for each character: the
 * font's byte for one it has, and `CHAR_NON_PRINTABLE` for one it lacks and
 * for a sequence an ASCII byte cuts short.
 */
static void row_utf8(void)
{
    k_cls();
    put_string("\303\251\342\224\200\320\264\303a");
    CHECK((vram[0] & 0xFF) == 0x82, "an e with an acute accent is the font's 0x82");
    CHECK((vram[1] & 0xFF) == 0xC4, "a horizontal line is the font's 0xC4");
    CHECK((vram[2] & 0xFF) == '?', "a Cyrillic letter the font lacks is one '?'");
    CHECK((vram[3] & 0xFF) == '?' && (vram[4] & 0xFF) == 'a',
          "a sequence cut short is a '?' before the byte that cut it");
    CHECK(x == 5, "five characters are five cells");
}

/**
 * The loaded font draws the Cyrillic letters, those of Kyrgyz among them,
 * from its own characters, a box glyph that joins a single line to a double
 * one as the glyph of its vertical line, and a Latin letter with an accent
 * without it; an adapter that keeps its own font keeps code page 437.
 */
static void row_font(void)
{
    font_reads_back = 0;
    k_font_init();
    k_cls();
    put_string("\303\251");
    CHECK((vram[0] & 0xFF) == 0x82, "the adapter's font keeps code page 437");
    font_reads_back = 1;
    k_font_init();
    k_cls();
    put_string("\320\264\322\243\342\225\241\303\251\302\253\342\200\224");
    CHECK((vram[0] & 0xFF) == 0xA4, "a Russian letter is the loaded font's");
    CHECK((vram[1] & 0xFF) == 0xB6, "a Kyrgyz letter is the loaded font's");
    CHECK((vram[2] & 0xFF) == 0xB4, "a box glyph of mixed lines is its vertical line's");
    CHECK((vram[3] & 0xFF) == 'e', "a Latin letter is drawn without its accent");
    CHECK((vram[4] & 0xFF) == 0xFB && (vram[5] & 0xFF) == '-',
          "a guillemet is the font's and a dash is ASCII's");
    vt_font = vt_cp437;
}

/**
 * While the adapter is in a graphics mode a scroll leaves the start address
 * alone, since it is the picture's; the return to text redraws and sets it.
 */
static void row_graphics(void)
{
    unsigned int starts;

    k_cls();
    vga_graphics_mode = true;
    starts = start_writes;
    k_scroll_down();
    CHECK(start_writes == starts, "a scroll in a graphics mode writes no start address");
    vga_graphics_mode = false;
    k_cls();
    CHECK(start_writes == starts + 2 && start_address == 0,
          "the clear on the return sets the start address");
}

int main(void)
{
    row_clear();
    row_byte();
    row_hold();
    row_scrolls();
    row_scrollback();
    row_console();
    row_answers();
    row_utf8();
    row_font();
    row_graphics();
    row_release_echo();
    if (failures) {
        printf("vga-text-test: %d failure(s)\n", failures);
        return 1;
    }
    printf("vga-text-test: OK\n");
    return 0;
}
