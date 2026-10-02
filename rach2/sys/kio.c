#include "kio.h"
#include "display/vga_font.h"
#include "klog.h"
#include "lib/kirq.h"

/*
 * VT100/ANSI terminal state for the VGA console.
 *
 * k_putc runs a small escape-sequence interpreter so the VGA text screen
 * behaves like the terminal the serial line already gets for free from the host
 * emulator. A line editor (busybox ash) and a full-screen program (vi) move the
 * cursor and erase with `ESC [` sequences; without an interpreter those bytes
 * would land on screen as literal `?[...` garbage, and backspace would never
 * erase. Only this VGA path is interpreted. The serial path hands the same
 * bytes to the host terminal, which interprets them itself.
 *
 * Text other than ASCII arrives as UTF-8 and is drawn from the adapter's
 * font: code page 437, or the one `k_font_init` loads, which keeps its box
 * drawing and has the Cyrillic letters. A query for the device's attributes
 * or the cursor's position is answered as a VT102 answers it; the console's
 * tty takes the answer with `k_take_answer` and gives it to the program as
 * input.
 */

#define VT_MAX_PARAMS 8

enum vt_parse {
    VT_NORMAL,  /* outside any escape sequence */
    VT_ESC,     /* saw ESC */
    VT_CSI,     /* saw `ESC [`, collecting numeric parameters */
    VT_ESC_ARG, /* saw `ESC (` and friends, swallow one selector byte */
    VT_STRING   /* saw `ESC ]`, `ESC P`, `ESC _`, `ESC ^` or `ESC X` */
};

static enum vt_parse vt_state = VT_NORMAL;
static unsigned int vt_params[VT_MAX_PARAMS];
static unsigned int vt_nparams;
static unsigned int vt_cur;
static bool vt_private;
static bool vt_interm;
static unsigned int vt_string_len;
static unsigned int vt_saved_x;
static unsigned int vt_saved_y;

static unsigned int vt_utf_left; /* continuation bytes still to come */
static uint32_t vt_utf_code;

static char vt_answer[K_ANSWER_MAX];
static unsigned int vt_answer_len;

/*
 * The characters of code page 437 from 0x80 to 0xFF, by their Unicode
 * values: the adapter's own font draws character 0x80 + i as vt_cp437[i].
 */
static const uint16_t vt_cp437[128] = {
    0x00C7, 0x00FC, 0x00E9, 0x00E2, 0x00E4, 0x00E0, 0x00E5, 0x00E7, 0x00EA, 0x00EB, 0x00E8, 0x00EF,
    0x00EE, 0x00EC, 0x00C4, 0x00C5, 0x00C9, 0x00E6, 0x00C6, 0x00F4, 0x00F6, 0x00F2, 0x00FB, 0x00F9,
    0x00FF, 0x00D6, 0x00DC, 0x00A2, 0x00A3, 0x00A5, 0x20A7, 0x0192, 0x00E1, 0x00ED, 0x00F3, 0x00FA,
    0x00F1, 0x00D1, 0x00AA, 0x00BA, 0x00BF, 0x2310, 0x00AC, 0x00BD, 0x00BC, 0x00A1, 0x00AB, 0x00BB,
    0x2591, 0x2592, 0x2593, 0x2502, 0x2524, 0x2561, 0x2562, 0x2556, 0x2555, 0x2563, 0x2551, 0x2557,
    0x255D, 0x255C, 0x255B, 0x2510, 0x2514, 0x2534, 0x252C, 0x251C, 0x2500, 0x253C, 0x255E, 0x255F,
    0x255A, 0x2554, 0x2569, 0x2566, 0x2560, 0x2550, 0x256C, 0x2567, 0x2568, 0x2564, 0x2565, 0x2559,
    0x2558, 0x2552, 0x2553, 0x256B, 0x256A, 0x2518, 0x250C, 0x2588, 0x2584, 0x258C, 0x2590, 0x2580,
    0x03B1, 0x00DF, 0x0393, 0x03C0, 0x03A3, 0x03C3, 0x00B5, 0x03C4, 0x03A6, 0x0398, 0x03A9, 0x03B4,
    0x221E, 0x03C6, 0x03B5, 0x2229, 0x2261, 0x00B1, 0x2265, 0x2264, 0x2320, 0x2321, 0x00F7, 0x2248,
    0x00B0, 0x2219, 0x00B7, 0x221A, 0x207F, 0x00B2, 0x25A0, 0x00A0};

/* The font the adapter draws: its own, or the one k_font_init loads. */
static const uint16_t *vt_font = vt_cp437;

/*
 * The Latin letters with accents from U+00C0 and from U+0100, each as its
 * letter without the accent, for a font that lacks them.
 */
static const char vt_latin1[] = "AAAAAAACEEEEIIIIDNOOOOOxOUUUUYTsaaaaaaaceeeeiiiidnooooo/ouuuuyty";
static const char vt_latin_a[] = "AaAaAaCcCcCcCcDdDdEeEeEeEeEeGgGgGgGgHhHhIiIiIiIiIiIiJjKkkLlLlLlL"
                                 "lLlNnNnNnnNnOoOoOoOoRrRrRrSsSsSsSsTtTtTtUuUuUuUuUuUuWwYyYZzZzZzs";

/*
 * Characters drawn as another when the font lacks them: punctuation as its
 * ASCII likeness, and box glyphs that join a single line to a double one,
 * or heavy and rounded ones, as the glyph of their vertical line's kind.
 */
static const struct {
    uint16_t first, last, to;
} vt_near_table[] = {{0x00A0, 0x00A0, ' '},    {0x00AD, 0x00AD, '-'},    {0x00B4, 0x00B4, '\''},
                     {0x2010, 0x2015, '-'},    {0x2018, 0x201B, '\''},   {0x201C, 0x201F, '"'},
                     {0x2026, 0x2026, '.'},    {0x2039, 0x2039, '<'},    {0x203A, 0x203A, '>'},
                     {0x2212, 0x2212, '-'},    {0x2501, 0x2501, 0x2500}, {0x2503, 0x2503, 0x2502},
                     {0x250F, 0x250F, 0x250C}, {0x2513, 0x2513, 0x2510}, {0x2517, 0x2517, 0x2514},
                     {0x251B, 0x251B, 0x2518}, {0x2552, 0x2552, 0x250C}, {0x2553, 0x2553, 0x2554},
                     {0x2555, 0x2555, 0x2510}, {0x2556, 0x2556, 0x2557}, {0x2558, 0x2558, 0x2514},
                     {0x2559, 0x2559, 0x255A}, {0x255B, 0x255B, 0x2518}, {0x255C, 0x255C, 0x255D},
                     {0x255E, 0x255E, 0x251C}, {0x255F, 0x255F, 0x2560}, {0x2561, 0x2561, 0x2524},
                     {0x2562, 0x2562, 0x2563}, {0x2564, 0x2564, 0x252C}, {0x2565, 0x2565, 0x2566},
                     {0x2567, 0x2567, 0x2534}, {0x2568, 0x2568, 0x2569}, {0x256A, 0x256A, 0x253C},
                     {0x256B, 0x256B, 0x256C}, {0x256D, 0x256D, 0x250C}, {0x256E, 0x256E, 0x2510},
                     {0x256F, 0x256F, 0x2518}, {0x2570, 0x2570, 0x2514}};

static unsigned int vt_fg = DEFAULT_FORECOLOR;
static unsigned int vt_bg = DEFAULT_BACKCOLOR;
static bool vt_bold;
static bool vt_reverse;

/*
 * ANSI SGR color index (black, red, green, yellow, blue, magenta, cyan, white)
 * mapped to the VGA palette, whose primaries sit at different slots.
 */
static const uint8_t vt_ansi_color[8] = {black, red, green, brown, blue, magenta, cyan, light_grey};

/**
 * Draw one byte at the cursor with no escape interpretation.
 *
 * BS, HT, LF, and CR move the cursor; NUL and BEL draw nothing, since a
 * program sends them to a terminal as padding and as a warning; any other
 * control byte is shown as `CHAR_NON_PRINTABLE`; a printable byte, or a byte
 * from 0x80 up, which is a character of the font's upper half, is written
 * with the current color and the cursor advances, wrapping and scrolling at
 * the screen edges.
 */
static void vga_putc_raw(char c)
{
    if ((unsigned char)c < 32 || c == 127) {
        switch (c) {
        case CHAR_NULL:
        case CHAR_BEL:
            break;

        case CHAR_BS:
            if (x)
                --x;
            else if (y) {
                --y;
                x = SCREEN_NUMBER_OF_COLUMNS - 1;
            }

            break;

        case CHAR_HT:
            x = x - x % TAB_SIZE + TAB_SIZE;
            if (x >= SCREEN_NUMBER_OF_COLUMNS)
                x = SCREEN_NUMBER_OF_COLUMNS - 1;
            break;

        case CHAR_LF:
            x = 0;
            ++y;
            break;

        case CHAR_CR:
            x = 0;
            break;

        default:
            c = CHAR_NON_PRINTABLE;
            break;
        }

        if (c != CHAR_NON_PRINTABLE)
            c = CHAR_NULL;
    }

    if (c) {
        vga_text_write_cell(x, y, c, get_curr_color_attr());
        ++x;
    }

    if (x >= SCREEN_NUMBER_OF_COLUMNS) {
        x = 0;
        ++y;
    }
    if (y >= SCREEN_NUMBER_OF_ROWS) {
        k_scroll_down();
        y = SCREEN_NUMBER_OF_ROWS - 1;
    }
}

/** Blank columns [`c0`, `c1`] of live-area row `row` with the current color. */
static void vt_erase_row(unsigned int row, unsigned int c0, unsigned int c1)
{
    uint8_t attr;
    unsigned int c;

    if (row >= SCREEN_NUMBER_OF_ROWS)
        return;

    attr = get_curr_color_attr();
    for (c = c0; c <= c1 && c < SCREEN_NUMBER_OF_COLUMNS; ++c) {
        vga_text_write_cell(c, row, ' ', attr);
    }
}

/**
 * Erase in display, `CSI n J`: 0 erases from the cursor to the end of the
 * screen, 1 from its start to the cursor, and 2 all of it.
 */
static void vt_erase_display(unsigned int n)
{
    unsigned int r;

    if (n == 0) {
        vt_erase_row(y, x, SCREEN_NUMBER_OF_COLUMNS - 1);
        for (r = y + 1; r < SCREEN_NUMBER_OF_ROWS; ++r)
            vt_erase_row(r, 0, SCREEN_NUMBER_OF_COLUMNS - 1);
    } else if (n == 1) {
        for (r = 0; r < y; ++r)
            vt_erase_row(r, 0, SCREEN_NUMBER_OF_COLUMNS - 1);
        vt_erase_row(y, 0, x);
    } else if (n == 2) {
        for (r = 0; r < SCREEN_NUMBER_OF_ROWS; ++r)
            vt_erase_row(r, 0, SCREEN_NUMBER_OF_COLUMNS - 1);
    }
}

/** Numeric CSI parameter `i`, or `def` when it is absent or zero. */
static unsigned int vt_arg(unsigned int i, unsigned int def)
{
    if (i < vt_nparams && vt_params[i] != 0)
        return vt_params[i];

    return def;
}

/** Push the current color state to the driver, applying bold and reverse. */
static void vt_apply_color(void)
{
    unsigned int fg = vt_bold ? (vt_fg | 8) : vt_fg;
    unsigned int bg = vt_bg;

    if (vt_reverse) {
        unsigned int t = fg & 7;
        fg = bg;
        bg = t;
    }

    set_fore_color((display_color_t)(fg & 15));
    set_back_color((display_color_t)(bg & 7));
}

/** Apply one Select-Graphic-Rendition sequence from the collected
 * parameters. */
static void vt_do_sgr(void)
{
    unsigned int i, n, code;

    n = vt_nparams ? vt_nparams : 1;
    for (i = 0; i < n; ++i) {
        code = (i < vt_nparams) ? vt_params[i] : 0;

        if (code == 0) {
            vt_fg = DEFAULT_FORECOLOR;
            vt_bg = DEFAULT_BACKCOLOR;
            vt_bold = false;
            vt_reverse = false;
        } else if (code == 1) {
            vt_bold = true;
        } else if (code == 22) {
            vt_bold = false;
        } else if (code == 7) {
            vt_reverse = true;
        } else if (code == 27) {
            vt_reverse = false;
        } else if (code >= 30 && code <= 37) {
            vt_fg = vt_ansi_color[code - 30];
        } else if (code == 39) {
            vt_fg = DEFAULT_FORECOLOR;
        } else if (code >= 40 && code <= 47) {
            vt_bg = vt_ansi_color[code - 40];
        } else if (code == 49) {
            vt_bg = DEFAULT_BACKCOLOR;
        } else if (code >= 90 && code <= 97) {
            vt_fg = vt_ansi_color[code - 90] | 8;
        } else if (code >= 100 && code <= 107) {
            vt_bg = vt_ansi_color[code - 100];
        }
    }

    vt_apply_color();
}

/** Add the `n` bytes of `s` to the waiting answer, or drop them if no room. */
static void vt_answer_add(const char *s, unsigned int n)
{
    unsigned int i;

    if (vt_answer_len + n > K_ANSWER_MAX)
        return;
    for (i = 0; i < n; ++i)
        vt_answer[vt_answer_len++] = s[i];
}

/** Append the decimal digits of `n` to `buf` at `*len`. */
static void vt_put_number(char *buf, unsigned int *len, unsigned int n)
{
    char digits[10];
    unsigned int k = 0;

    do {
        digits[k++] = (char)('0' + n % 10);
        n /= 10;
    } while (n != 0);
    while (k > 0)
        buf[(*len)++] = digits[--k];
}

/**
 * Answer a query whose final byte is `c`. The primary device attributes,
 * `CSI c`, are a VT102's, `CSI ? 6 c`. A device status report, `CSI n`, asks
 * with 5 for the status, which is always good, and with 6 for the cursor's
 * position, one-based as `CSI row ; col R`.
 */
static void vt_answer_query(char c)
{
    char buf[K_ANSWER_MAX];
    unsigned int len = 0;
    unsigned int n = vt_arg(0, 0);

    if (c == 'c') {
        if (n == 0)
            vt_answer_add("\033[?6c", 5);
    } else if (n == 5) {
        vt_answer_add("\033[0n", 4);
    } else if (n == 6) {
        buf[len++] = 0x1b;
        buf[len++] = '[';
        vt_put_number(buf, &len, y + 1);
        buf[len++] = ';';
        vt_put_number(buf, &len, x + 1);
        buf[len++] = 'R';
        vt_answer_add(buf, len);
    }
}

unsigned int k_take_answer(char *buf, unsigned int size)
{
    unsigned int i, n;
    uint32_t flags;

    flags = kirq_save();
    n = vt_answer_len <= size ? vt_answer_len : 0;
    for (i = 0; i < n; ++i)
        buf[i] = vt_answer[i];
    vt_answer_len = 0;
    kirq_restore(flags);
    return n;
}

/**
 * Execute a completed CSI sequence whose final byte is `c`.
 *
 * Cursor motion (`A`-`H`, `f`, `d`, `G`), erase (`J`, `K`, `X`), color (`m`),
 * and cursor save/restore (`s`, `u`) are handled. The device attributes
 * (`c`) and the status reports (`n`) are answered by `vt_answer_query`; a
 * program such as a browser waits for those answers before it reads the
 * keyboard. A scroll-region set (`r`), mode toggles (`h`, `l`), and anything
 * else are ignored. Coordinates are one-based on the wire and clamped to the
 * screen.
 */
static void vt_dispatch_csi(char c)
{
    unsigned int n, row, col;

    switch (c) {
    case 'A':
        n = vt_arg(0, 1);
        y = (y >= n) ? y - n : 0;
        break;
    case 'B':
        n = vt_arg(0, 1);
        y += n;
        if (y >= SCREEN_NUMBER_OF_ROWS)
            y = SCREEN_NUMBER_OF_ROWS - 1;
        break;
    case 'C':
        n = vt_arg(0, 1);
        x += n;
        if (x >= SCREEN_NUMBER_OF_COLUMNS)
            x = SCREEN_NUMBER_OF_COLUMNS - 1;
        break;
    case 'D':
        n = vt_arg(0, 1);
        x = (x >= n) ? x - n : 0;
        break;
    case 'E':
        n = vt_arg(0, 1);
        y += n;
        if (y >= SCREEN_NUMBER_OF_ROWS)
            y = SCREEN_NUMBER_OF_ROWS - 1;
        x = 0;
        break;
    case 'F':
        n = vt_arg(0, 1);
        y = (y >= n) ? y - n : 0;
        x = 0;
        break;

    case 'G':
    case '`':
        col = vt_arg(0, 1);
        x = col - 1;
        if (x >= SCREEN_NUMBER_OF_COLUMNS)
            x = SCREEN_NUMBER_OF_COLUMNS - 1;
        break;
    case 'd':
        row = vt_arg(0, 1);
        y = row - 1;
        if (y >= SCREEN_NUMBER_OF_ROWS)
            y = SCREEN_NUMBER_OF_ROWS - 1;
        break;
    case 'H':
    case 'f':
        row = vt_arg(0, 1);
        col = vt_arg(1, 1);
        y = row - 1;
        x = col - 1;
        if (y >= SCREEN_NUMBER_OF_ROWS)
            y = SCREEN_NUMBER_OF_ROWS - 1;
        if (x >= SCREEN_NUMBER_OF_COLUMNS)
            x = SCREEN_NUMBER_OF_COLUMNS - 1;
        break;

    case 'J':
        vt_erase_display(vt_arg(0, 0));
        break;
    case 'K':
        n = vt_arg(0, 0);
        if (n == 0)
            vt_erase_row(y, x, SCREEN_NUMBER_OF_COLUMNS - 1);
        else if (n == 1)
            vt_erase_row(y, 0, x);
        else if (n == 2)
            vt_erase_row(y, 0, SCREEN_NUMBER_OF_COLUMNS - 1);
        break;
    case 'X':
        n = vt_arg(0, 1);
        vt_erase_row(y, x, x + n - 1);
        break;

    case 'm':
        vt_do_sgr();
        break;
    case 's':
        vt_saved_x = x;
        vt_saved_y = y;
        break;
    case 'u':
        x = vt_saved_x;
        y = vt_saved_y;
        break;

    case 'c':
    case 'n':
        vt_answer_query(c);
        break;

    default:
        break;
    }
}

/** The font's character for Unicode value `code`, or 0 when it has none. */
static unsigned char vt_font_char(uint32_t code)
{
    unsigned int i;

    if (code >= 0xA0) {
        for (i = 0; i < 128; ++i) {
            if (vt_font[i] == code)
                return (unsigned char)(0x80 + i);
        }
    }
    return 0;
}

/**
 * The character to draw for Unicode value `code` when the font lacks it: a
 * letter without its accent, an ASCII likeness, or another Unicode value;
 * `CHAR_NON_PRINTABLE` when there is none.
 */
static uint32_t vt_near(uint32_t code)
{
    unsigned int i;

    if (code >= 0xC0 && code < 0x100)
        return (unsigned char)vt_latin1[code - 0xC0];
    if (code >= 0x100 && code < 0x180)
        return (unsigned char)vt_latin_a[code - 0x100];
    for (i = 0; i < sizeof(vt_near_table) / sizeof(vt_near_table[0]); ++i) {
        if (code >= vt_near_table[i].first && code <= vt_near_table[i].last)
            return vt_near_table[i].to;
    }
    return CHAR_NON_PRINTABLE;
}

/** The font's character for Unicode value `code`, or a stand-in for it. */
static char vt_glyph(uint32_t code)
{
    unsigned char ch = vt_font_char(code);

    if (ch == 0) {
        code = vt_near(code);
        ch = code < 0x80 ? (unsigned char)code : vt_font_char(code);
    }
    return ch != 0 ? (char)ch : CHAR_NON_PRINTABLE;
}

void k_font_init(void)
{
    if (vga_font_load()) {
        vt_font = vga_font_map;
        klog(KLOG_INFO, "vga", "font: loaded: code page 437 box drawing with Cyrillic");
    } else {
        klog(KLOG_INFO, "vga", "font: kept: the adapter's code page 437");
    }
}

/**
 * Take `uc`, a byte from 0x80 up, as part of a UTF-8 sequence, and draw the
 * character when its sequence is complete.
 *
 * The font is code page 437: box drawing, blocks, Latin letters with accents
 * and some Greek. A character it lacks is drawn as `CHAR_NON_PRINTABLE`, once
 * for the character, and so is a byte that cannot begin or continue one.
 * `k_putc` ends a sequence cut short by a byte below 0x80 the same way.
 */
static void vt_utf8(unsigned char uc)
{
    if (uc < 0xC0) {
        if (vt_utf_left == 0) {
            vga_putc_raw(CHAR_NON_PRINTABLE);
            return;
        }
        vt_utf_code = (vt_utf_code << 6) | (uc & 0x3Fu);
        if (--vt_utf_left == 0)
            vga_putc_raw(vt_glyph(vt_utf_code));
    } else if (uc < 0xE0) {
        vt_utf_code = uc & 0x1Fu;
        vt_utf_left = 1;
    } else if (uc < 0xF0) {
        vt_utf_code = uc & 0x0Fu;
        vt_utf_left = 2;
    } else if (uc < 0xF8) {
        vt_utf_code = uc & 0x07u;
        vt_utf_left = 3;
    } else {
        vga_putc_raw(CHAR_NON_PRINTABLE);
    }
}

/**
 * Take `uc`, the byte after an `ESC`: it opens a CSI, a string or a charset
 * selector, or it is a command of its own, save and restore the cursor or
 * reset the terminal. Another `ESC` starts the sequence again.
 */
static void vt_escape(unsigned char uc)
{
    switch (uc) {
    case '[':
        vt_state = VT_CSI;
        vt_nparams = 0;
        vt_cur = 0;
        vt_private = false;
        vt_interm = false;
        break;
    case ']':
    case 'P':
    case '_':
    case '^':
    case 'X':
        vt_state = VT_STRING;
        vt_string_len = 0;
        break;
    case '(':
    case ')':
    case '*':
    case '+':
    case '#':
    case ' ':
        vt_state = VT_ESC_ARG;
        break;
    case '7':
        vt_saved_x = x;
        vt_saved_y = y;
        vt_state = VT_NORMAL;
        break;
    case '8':
        x = vt_saved_x;
        y = vt_saved_y;
        vt_state = VT_NORMAL;
        break;
    case 'c':
        vt_fg = DEFAULT_FORECOLOR;
        vt_bg = DEFAULT_BACKCOLOR;
        vt_bold = false;
        vt_reverse = false;
        vt_apply_color();
        k_cls();
        vt_state = VT_NORMAL;
        break;
    case 0x1b:
        break;
    default:
        vt_state = VT_NORMAL;
        break;
    }
}

/**
 * Take byte `c` of a CSI. A digit or `;` adds to the parameters. `?` and its
 * kin mark a private sequence, and an intermediate byte one this console
 * lacks; a final byte from `@` to `~` ends the sequence, swallowing either
 * kind and executing any other by `vt_dispatch_csi`. An `ESC` starts a new
 * sequence, and any other byte ends this one and is drawn.
 */
static void vt_csi_byte(char c)
{
    unsigned char uc = (unsigned char)c;

    if (uc >= '0' && uc <= '9') {
        vt_cur = vt_cur * 10 + (uc - '0');
        if (vt_cur > 9999)
            vt_cur = 9999;
    } else if (uc == ';') {
        if (vt_nparams < VT_MAX_PARAMS)
            vt_params[vt_nparams++] = vt_cur;
        vt_cur = 0;
    } else if (uc >= 0x3c && uc <= 0x3f) {
        if (vt_nparams == 0 && vt_cur == 0)
            vt_private = true;
    } else if (uc >= 0x20 && uc <= 0x2f) {
        vt_interm = true; /* intermediate byte, as in `ESC [ SP q` */
    } else if (uc >= 0x40 && uc <= 0x7e) {
        if (vt_nparams < VT_MAX_PARAMS)
            vt_params[vt_nparams++] = vt_cur;
        if (!vt_private && !vt_interm)
            vt_dispatch_csi(c);
        vt_state = VT_NORMAL;
    } else if (uc == 0x1b) {
        vt_state = VT_ESC;
    } else {
        vt_state = VT_NORMAL;
        vga_putc_raw(c);
    }
}

/**
 * Write one byte to the VGA console, interpreting VT100/ANSI escapes.
 *
 * A plain byte is drawn by `vga_putc_raw`, and a byte from 0x80 up goes to
 * `vt_utf8`. `ESC` opens an escape sequence: a CSI (`ESC [`) collects
 * semicolon-separated numeric parameters up to its final letter and is
 * executed by `vt_dispatch_csi`; a private CSI (`ESC [ ?`, as in the
 * alternate-screen and cursor-visibility toggles) is swallowed; a charset
 * selector (`ESC (` and kin) swallows its next byte; an OSC (`ESC ]`), a
 * device control string (`ESC P`) and the application and privacy strings
 * (`ESC _`, `ESC ^`, `ESC X`) are swallowed to their BEL or `ESC \`, so the
 * queries a program sends to learn the terminal's features do not reach the
 * screen. The hardware cursor is placed after every byte, so a cursor-only
 * sequence still moves the on-screen caret; a writer of many bytes holds it
 * and it is placed once at the release. The byte's work runs with interrupts
 * off, since a keyboard's echo arrives through this path from its interrupt.
 */
void k_putc(char c)
{
    unsigned char uc = (unsigned char)c;
    uint32_t flags;

    /* With interrupts off: a keyboard's echo comes from its interrupt and
       through this same path, and a scroll or an escape sequence cut in
       two by it would leave the cursor and the display's start apart. A
       byte is one cell and a scroll one line; the copy at the memory's end
       is the one long piece, once in 180 lines. */
    flags = kirq_save();
    if (vt_utf_left != 0 && (uc < 0x80 || uc >= 0xC0)) {
        vt_utf_left = 0;
        vga_putc_raw(CHAR_NON_PRINTABLE);
    }
    switch (vt_state) {
    case VT_NORMAL:
        if (uc == 0x1b)
            vt_state = VT_ESC;
        else if (uc >= 0x80)
            vt_utf8(uc);
        else
            vga_putc_raw(c);
        break;

    case VT_ESC:
        vt_escape(uc);
        break;

    case VT_ESC_ARG:
        vt_state = VT_NORMAL;
        break;

    case VT_STRING:
        if (uc == 0x07)
            vt_state = VT_NORMAL;
        else if (uc == 0x1b)
            vt_state = VT_ESC;
        else if (uc < 0x20)
            vt_state = VT_NORMAL;
        else if (++vt_string_len > 256)
            vt_state = VT_NORMAL;
        break;

    case VT_CSI:
        vt_csi_byte(c);
        break;
    default:
        break;
    }

    vga_text_place_cursor();
    kirq_restore(flags);
}

void k_puts(const char *str)
{
    if (str == NULL)
        return;

    vga_text_hold();
    while (*str) {
        k_putc(*str++);
    }
    k_putc(CHAR_LF);
    vga_text_release();
}

void k_printf(const char *format, ...)
{
    va_list ap;

    va_start(ap, format);
    vga_text_hold();
    k_vprintf(k_putc, format, ap);
    vga_text_release();
    va_end(ap);
}

/* The port instructions, which a host test that includes this file stands
   in for, since its host need not be an x86. */
#ifndef KIO_HOSTTEST
void outb(uint16_t port, uint8_t val)
{
    __asm__ __volatile__("outb %1, %0" : : "dN"(port), "a"(val));
}

#ifdef RACH2_HARNESS
/**
 * Report a process exit status to QEMU's isa-debug-exit device (harness only).
 *
 * Writing the status to I/O port 0xF4 makes the test harness read a real exit
 * code instead of scraping the serial log: a hang or triple-fault then surfaces
 * as a wall-clock timeout, not a missing marker. Only ring 0 may write
 * this port, so a ring-3 probe cannot fake it. QEMU exits with the value
 * `((status << 1) | 1)`, so an exit code of 1 means a status of 0.
 */
void harness_exit(uint8_t status)
{
    outb(0xF4, status);
}
#endif

void outw(uint16_t port, uint16_t val)
{
    __asm__ __volatile__("outw %1, %0" : : "dN"(port), "a"(val));
}

void outd(uint16_t port, uint32_t val)
{
    __asm__ __volatile__("outl %1, %0" : : "dN"(port), "a"(val));
}

uint8_t inb(uint16_t port)
{
    uint8_t result;

    __asm__ __volatile__("inb %1, %0" : "=a"(result) : "dN"(port));

    return result;
}

uint16_t inw(uint16_t port)
{
    uint16_t result;

    __asm__ __volatile__("inw %1, %0" : "=a"(result) : "dN"(port));

    return result;
}

uint32_t ind(uint16_t port)
{
    uint32_t result;

    __asm__ __volatile__("inl %1, %0" : "=a"(result) : "dN"(port));

    return result;
}

void insw(uint16_t port, void *buffer, uint32_t count)
{
    __asm__ __volatile__("cld\n\t"
                         "rep insw"
                         : "+D"(buffer), "+c"(count)
                         : "d"(port)
                         : "memory", "cc");
}

void outsw(uint16_t port, const void *buffer, uint32_t count)
{
    __asm__ __volatile__("cld\n\t"
                         "rep outsw"
                         : "+S"(buffer), "+c"(count)
                         : "d"(port)
                         : "memory", "cc");
}
#endif
