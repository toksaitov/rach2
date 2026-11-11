/*
 * vga_text_hosttest.c: the text console driver against a model adapter.
 *
 * host-deps: rach2/sys/lib/kfmt.c rach2/sys/lib/kstd.c
 * host-deps: rach2/sys/lib/kstring.c rach2/sys/lib/kctypes.c
 * host-deps: tests/host/compat/spinlock_host.c rach2/sys/wcwidth.c
 *
 * The adapter's memory is an array here, every store to it is counted, and its
 * ports are recorded, so the rows count what each operation costs. A byte is
 * one cell. A scroll is the new line's eighty cells and the start address, with
 * the rows above left in the cells they were in. The copy to the front of the
 * memory comes only when the window would pass its end, once in 180 scrolls,
 * and carries its content. A scroll back and a clear redraw the screen. A held
 * cursor is placed once, at the release. The twenty-five rows the start address
 * selects are the history's live lines throughout, including across the copy.
 * And the console's own byte path, `k_putc` and the writers over it, costs the
 * same. A byte in the last column leaves the wrap pending, as the terminfo's
 * xenl says: a full last row does not scroll, which tmux's status line needs.
 * Every glyph of the coding agents' recorded interfaces has a stand-in, a
 * wide character takes two cells and a combining mark none, and the console
 * keeps the cursor and keypad modes. The driver and the console are included
 * whole to reach their state.
 */
#include "lib/kint.h"

#define VGA_TEXT_HOSTTEST

static uint16_t vram[16384];
#define VGA_TEXT_BUFFER vram

extern int printf(const char *format, ...);

static unsigned int stores;

/** Count the driver's one store to the adapter's memory. */
void vga_text_hosttest_stored(void)
{
    stores++;
}

/* The flag register's stand-in: the console keeps interrupts off around a
   byte, and the rows watch that it does. */
static int masked;
static int inject_echo;  /* the next selection of 0x0E raises an echo */
static int echo_pending; /* a keyboard interrupt raised, not delivered */
static void deliver_echo(void);

/** Mask interrupts and return the mask the caller had. */
uint32_t kirq_save(void)
{
    uint32_t old = (uint32_t)masked;

    masked = 1;
    return old;
}

/** Restore the mask `flags` and deliver a pending echo. */
void kirq_restore(uint32_t flags)
{
    masked = (int)flags;
    deliver_echo();
}

#include "display/vga_gfx.h"
bool vga_graphics_mode = false;

#include "display/vga_text.c"
#include "display/fbcon.c"
#include "display/vga_font.c"
#include "kio.c"

/** One processor, so the console's lock is never held by another for a
 * wait to outlast: nothing to wait for. */
void k_udelay(uint32_t usec)
{
    (void)usec;
}

/** No processor here has begun a panic, so the console takes its lock. */
int k_panicking(void)
{
    return 0;
}

/** No framebuffer, so the console stays on the adapter; fbcon_hosttest.c
 * gives one. */
volatile uint8_t *fbcon_window(void)
{
    return NULL;
}

static int font_reads_back;

/** The adapter's character memory: whether the glyphs read back as written. */
int vga_font_load(void)
{
    return font_reads_back;
}

/** Discard the kernel log, where the font's load is reported. */
void klog(int level, const char *subsystem, const char *format, ...)
{
    (void)level;
    (void)subsystem;
    (void)format;
}

/* The CRTC's port writes, the writes of the start address's two bytes, the
 * register selected, and the start address and cursor location in cells. */
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

/** A read of the adapter's ports, which reach no device here. */
uint8_t inb(uint16_t port)
{
    (void)port;
    return 0;
}

/** The blank time the escape sets, which no row reads. */
void console_set_blank_time(unsigned int seconds)
{
    (void)seconds;
}

/** The CRTC as the driver sees it: an index port and a data port, of which
 * the start address and the cursor location are kept. Selecting the cursor's
 * high byte raises the echo `inject_echo` asks for. */
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
        textcon_hold();
        textcon_place_cursor();
        masked = initial;
        inject_echo = 1;
        textcon_release();
        CHECK(masked == initial, "release restores caller flags");
        CHECK(_hold == 0 && !_cursor_stale, "release completes outermost hold");
        if (initial) {
            CHECK(echo_pending && cursor_address == 0x00FF, "masked caller retains pending echo");
            kirq_restore(0);
        }
        CHECK(!echo_pending && x == 16 && y == 3, "keyboard echo runs once");
        CHECK(cursor_address == 0x0100, "echo leaves cursor at 0x0100");
        textcon_release();
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
    textcon_write_cell(0, 0, 'A', 0x07);
    CHECK(stores == 1, "a byte stores one cell");
    CHECK(vram[0] == vga_text_make_cell('A', 0x07), "the byte's cell holds the byte");
    CHECK(port_writes == ports, "a byte alone touches no port");
    textcon_place_cursor();
    CHECK(port_writes == ports + 4, "placing the cursor is four port writes");
    CHECK(cursor_address == 1, "the cursor is at the byte after the one written");
}

/** Held, the cursor is placed once at the release, wherever it went. */
static void row_hold(void)
{
    unsigned int ports = port_writes;

    textcon_hold();
    textcon_hold();
    x = 5;
    textcon_place_cursor();
    x = 7;
    textcon_place_cursor();
    textcon_release();
    CHECK(port_writes == ports, "a nested hold keeps the cursor where it was");
    textcon_release();
    CHECK(port_writes == ports + 4, "the last release places the cursor once");
    CHECK(cursor_address == 7, "and places it where it last went");
    ports = port_writes;
    textcon_hold();
    textcon_release();
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
        textcon_write_cell(0, VGA_TEXT_ROWS - 1, (char)('a' + (n % 26)), 0x07);
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
    textcon_write_cell(0, VGA_TEXT_ROWS - 1, 'Z', 0x07);
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
    textcon_write_cell(0, 0, 'Q', 0x07);
    CHECK(stores == 0, "live output while scrolled back leaves the adapter alone");
    k_scroll_down();
    CHECK(stores == 0, "a scroll while scrolled back leaves the adapter alone");
    k_scrollback_end();
    CHECK(_follow_live, "end returns to the live lines");
    CHECK(window_shows_viewport(), "the return shows the live lines, the byte among them");

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
 * the work runs with interrupts off. `k_printf` writes through `k_putc`
 * inside one hold and is not run here.
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

/** The cell at column `col` of live row `row`. */
static uint16_t live_cell(unsigned int col, unsigned int row)
{
    return vram[(_hw_top + row * VGA_TEXT_COLS + col) % VGA_TEXT_CELLS];
}

/** The bytes of `s` through the console, with no line feed after them. */
static void put_str(const char *s)
{
    while (*s) {
        k_putc(*s++);
    }
}

/** `n` bytes `c` through the console. */
static void put_run(char c, unsigned int n)
{
    while (n--) {
        k_putc(c);
    }
}

/**
 * A byte in the last column leaves the wrap pending, as Linux's console and
 * the terminfo's xenl do: a full last row does not scroll, which tmux's
 * status line needs, the next printable byte wraps, CR and LF after a full
 * line give no blank line, BS only cancels the wrap, and a cursor move
 * cancels it.
 */
static void row_wrap(void)
{
    uint32_t top;

    k_cls();
    put_str("\033[25;1H");
    top = _hw_top;
    put_run('x', VGA_TEXT_COLS);
    CHECK(_hw_top == top, "a full last row does not scroll");
    CHECK(x == VGA_TEXT_COLS - 1 && y == VGA_TEXT_ROWS - 1,
          "the cursor stays in the last column with the wrap pending");
    k_putc('y');
    CHECK(_hw_top == top + VGA_TEXT_COLS, "the next byte wraps and scrolls");
    CHECK(live_cell(0, VGA_TEXT_ROWS - 1) == vga_text_make_cell('y', get_curr_color_attr()),
          "and is written at the start of the new line");
    k_cls();
    put_run('a', VGA_TEXT_COLS);
    put_str("\r\nz");
    CHECK(live_cell(0, 1) == vga_text_make_cell('z', get_curr_color_attr()),
          "CR and LF after a full line leave no blank line");
    put_str("\033[3;1H");
    put_run('b', VGA_TEXT_COLS);
    k_putc('\b');
    CHECK(x == VGA_TEXT_COLS - 1 && y == 2, "BS with the wrap pending stays on the last cell");
    put_str("\033[5;1H");
    put_run('c', VGA_TEXT_COLS);
    put_str("\033[5;80Hq");
    CHECK(y == 4 &&
              live_cell(VGA_TEXT_COLS - 1, 4) == vga_text_make_cell('q', get_curr_color_attr()),
          "a cursor move cancels the wrap");
}

/** Write Unicode value `code` through the console as UTF-8. */
static void put_code(uint32_t code)
{
    if (code < 0x80) {
        k_putc((char)code);
    } else if (code < 0x800) {
        k_putc((char)(0xC0 | (code >> 6)));
        k_putc((char)(0x80 | (code & 0x3F)));
    } else if (code < 0x10000) {
        k_putc((char)(0xE0 | (code >> 12)));
        k_putc((char)(0x80 | ((code >> 6) & 0x3F)));
        k_putc((char)(0x80 | (code & 0x3F)));
    } else {
        k_putc((char)(0xF0 | (code >> 18)));
        k_putc((char)(0x80 | ((code >> 12) & 0x3F)));
        k_putc((char)(0x80 | ((code >> 6) & 0x3F)));
        k_putc((char)(0x80 | (code & 0x3F)));
    }
}

/** The character of the font in use that draws Unicode value `code`, 0 when
 * it has none. */
static unsigned int font_char(uint32_t code)
{
    unsigned int i;

    for (i = 0; i < 128; i++) {
        if (vt_font[i] == code) {
            return 0x80 + i;
        }
    }
    return 0;
}

/* The glyphs of Claude Code's interface in tests/fixtures/agents/, with
 * those the plan names for the other agents: their marks, spinners, menus'
 * pointers, the corner under a tool's call and the logo's blocks. */
static const uint32_t agent_glyphs[] = {
    0x00A0, 0x00B7, 0x2022, 0x2026, 0x203A, 0x2190, 0x2192, 0x2193, 0x23BF, 0x23F5, 0x23FA,
    0x2500, 0x2580, 0x2588, 0x2590, 0x2594, 0x259B, 0x259C, 0x259D, 0x25CB, 0x25CF, 0x25D0,
    0x25D1, 0x2705, 0x2713, 0x2714, 0x2717, 0x2718, 0x2722, 0x2733, 0x2736, 0x273B, 0x273D,
    0x276F, 0x2800, 0x2807, 0x280B, 0x2819, 0x28FF, 0x2581, 0x2596, 0x2597, 0x2598, 0x2599,
    0x259A, 0x259E, 0x259F, 0x256D, 0x256E, 0x256F, 0x2570};

/**
 * Every glyph the agents draw their own interfaces with has a stand-in in
 * both fonts, so none is `CHAR_NON_PRINTABLE`: a bullet as the font's bullet,
 * the spinners' stars as `*` and braille dots as `.`, the menus' pointer as
 * `>`, the corner under a tool's call as a box corner, the logo's quadrants
 * as the nearest half or full block, and check marks as `v`.
 */
static void row_stand_ins(void)
{
    unsigned int i, f;

    font_reads_back = 1;
    for (f = 0; f < 2; f++) {
        k_font_init();
        if (f == 1) {
            vt_font = vt_cp437;
        }
        for (i = 0; i < sizeof agent_glyphs / sizeof agent_glyphs[0]; i++) {
            k_cls();
            put_code(agent_glyphs[i]);
            if ((vram[0] & 0xFF) == '?') {
                printf("vga-text-test: fail: U+%04X has no stand-in in font %u\n",
                       (unsigned)agent_glyphs[i], f);
                failures++;
            }
        }
        k_cls();
        put_code(0x25CF);
        CHECK((vram[0] & 0xFF) == font_char(f == 0 ? 0x2022 : 0x2219),
              "a black circle is the font's bullet");
        put_code(0x273B);
        put_code(0x280B);
        put_code(0x276F);
        CHECK((vram[1] & 0xFF) == '*' && (vram[2] & 0xFF) == '.' && (vram[3] & 0xFF) == '>',
              "a spinner's star, a braille dot and the menus' pointer are ASCII's");
        put_code(0x23BF);
        put_code(0x259B);
        put_code(0x259D);
        put_code(0x2596);
        CHECK((vram[4] & 0xFF) == font_char(0x2514), "the corner under a call is a box corner");
        CHECK((vram[5] & 0xFF) == font_char(0x2588) && (vram[6] & 0xFF) == font_char(0x2580) &&
                  (vram[7] & 0xFF) == font_char(0x2584),
              "three quadrants are a full block and one an upper or lower half");
        put_code(0x2714);
        put_code(0x2718);
        CHECK((vram[8] & 0xFF) == 'v' && (vram[9] & 0xFF) == 'x',
              "a check mark is v and a cross x");
        put_code(0x1F680);
        CHECK((vram[10] & 0xFF) == '?' && (vram[11] & 0xFF) == ' ' && x == 12,
              "an emoji with no stand-in is a '?' and a blank");
    }
    vt_font = vt_cp437;
}

/**
 * The cells a character takes are the width table's: 1 for a letter, a
 * box glyph and the agents' marks, 2 for CJK and emoji, 0 for a combining
 * mark, a variation selector and a zero-width joiner, -1 for a control. On
 * the screen a wide character is its glyph or a stand-in and a blank; one
 * that would start in the last column begins the next line and leaves that
 * column alone; one that ends in the last column leaves the wrap pending;
 * and a combining mark draws nothing, so the row does not shift.
 */
static void row_widths(void)
{
    CHECK(k_wcwidth('a') == 1 && k_wcwidth(0x0434) == 1 && k_wcwidth(0x2500) == 1,
          "a letter and a box glyph take one cell");
    CHECK(k_wcwidth(0x25CF) == 1 && k_wcwidth(0x276F) == 1 && k_wcwidth(0x273B) == 1 &&
              k_wcwidth(0x23BF) == 1 && k_wcwidth(0x2819) == 1,
          "the agents' marks, pointer, stars, corner and braille take one cell");
    CHECK(k_wcwidth(0x4F60) == 2 && k_wcwidth(0xAC00) == 2 && k_wcwidth(0xFF21) == 2,
          "CJK, Hangul and full-width letters take two cells");
    CHECK(k_wcwidth(0x1F680) == 2 && k_wcwidth(0x2705) == 2 && k_wcwidth(0x23FA) == 1,
          "emoji take two cells, a record button one");
    CHECK(k_wcwidth(0x0301) == 0 && k_wcwidth(0xFE0F) == 0 && k_wcwidth(0x200D) == 0 &&
              k_wcwidth(0) == 0,
          "a combining mark, a variation selector, a joiner and NUL take none");
    CHECK(k_wcwidth(0x07) == -1 && k_wcwidth(0x9B) == -1, "a C0 or C1 control is -1");

    k_cls();
    put_str("a\344\275\240b");
    CHECK((vram[0] & 0xFF) == 'a' && (vram[1] & 0xFF) == '?' && (vram[2] & 0xFF) == ' ' &&
              (vram[3] & 0xFF) == 'b' && x == 4,
          "a CJK character is a '?' and a blank, and what follows it is not shifted");
    k_cls();
    put_str("e\314\201x\342\234\205\357\270\217y");
    CHECK((vram[0] & 0xFF) == 'e' && (vram[1] & 0xFF) == 'x' && (vram[2] & 0xFF) == 'v' &&
              (vram[3] & 0xFF) == ' ' && (vram[4] & 0xFF) == 'y' && x == 5,
          "a combining mark and a variation selector take no cell");

    k_cls();
    put_str("\033[1;80H#\033[1;80H");
    put_code(0x4F60);
    CHECK(live_cell(VGA_TEXT_COLS - 1, 0) == vga_text_make_cell('#', get_curr_color_attr()),
          "a wide character in the last column leaves it alone");
    CHECK(y == 1 && x == 2 && (live_cell(0, 1) & 0xFF) == '?' && (live_cell(1, 1) & 0xFF) == ' ',
          "and begins the next line in two cells");
    k_cls();
    put_str("\033[1;79H");
    put_code(0x1F680);
    CHECK(y == 0 && x == VGA_TEXT_COLS - 1 && vt_wrap_pending,
          "a wide character that ends in the last column leaves the wrap pending");
    k_putc('z');
    CHECK(y == 1 && (live_cell(0, 1) & 0xFF) == 'z', "and the next character wraps");
}

/**
 * DECCKM, `CSI ? 1 h` and `l`, and DECKPAM and DECKPNM, `ESC =` and
 * `ESC >`, set and reset the key modes `kio_key_modes` gives, among other
 * private modes in one sequence too; `ESC c` resets both; none draws a cell,
 * and a private sequence with another marker changes no mode. SO and SI,
 * which Claude Code writes after `ESC ( B`, draw nothing either.
 */
static void row_modes(void)
{
    put_str("\033c");
    CHECK(kio_key_modes() == 0, "the modes are off after a reset");
    stores = 0;
    put_str("\033[?1h");
    CHECK(kio_key_modes() == KM_MODE_CURSOR, "CSI ? 1 h sets the cursor mode");
    put_str("\033=");
    CHECK(kio_key_modes() == (KM_MODE_CURSOR | KM_MODE_KEYPAD), "ESC = sets the keypad mode");
    put_str("\033[?1l");
    CHECK(kio_key_modes() == KM_MODE_KEYPAD, "CSI ? 1 l resets the cursor mode");
    put_str("\033>");
    CHECK(kio_key_modes() == 0, "ESC > resets the keypad mode");
    put_str("\033[?25;1;2004h");
    CHECK(kio_key_modes() == KM_MODE_CURSOR, "the cursor mode is set among other modes");
    put_str("\033[>1l\033[1l");
    CHECK(kio_key_modes() == KM_MODE_CURSOR, "another marker, or none, changes no mode");
    put_str("\033(B\017\016");
    CHECK(stores == 0, "the modes, SO and SI draw nothing");
    put_str("\033=\033c");
    CHECK(kio_key_modes() == 0, "ESC c resets both modes");
}

/** Run the rows: 0 when every check passes, 1 otherwise. */
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
    row_wrap();
    row_stand_ins();
    row_widths();
    row_modes();
    if (failures) {
        printf("vga-text-test: %d failure(s)\n", failures);
        return 1;
    }
    printf("vga-text-test: OK\n");
    return 0;
}
