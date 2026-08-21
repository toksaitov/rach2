#include "kio.h"

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
 */

#define VT_MAX_PARAMS 8

enum vt_parse {
    VT_NORMAL,    /* outside any escape sequence */
    VT_ESC,       /* saw ESC */
    VT_CSI,       /* saw `ESC [`, collecting numeric parameters */
    VT_ESC_ARG,   /* saw `ESC (` and friends, swallow one selector byte */
    VT_OSC        /* saw `ESC ]`, swallow until BEL */
};

static enum vt_parse vt_state = VT_NORMAL;
static unsigned int vt_params[VT_MAX_PARAMS];
static unsigned int vt_nparams;
static unsigned int vt_cur;
static bool vt_private;
static bool vt_interm;
static unsigned int vt_osc_len;
static unsigned int vt_saved_x;
static unsigned int vt_saved_y;

static unsigned int vt_fg = DEFAULT_FORECOLOR;
static unsigned int vt_bg = DEFAULT_BACKCOLOR;
static bool vt_bold;
static bool vt_reverse;

/*
 * ANSI SGR color index (black, red, green, yellow, blue, magenta, cyan, white)
 * mapped to the VGA palette, whose primaries sit at different slots.
 */
static const uint8_t vt_ansi_color[8] = {
    black, red, green, brown, blue, magenta, cyan, light_grey
};

/**
 * Draw one byte at the cursor with no escape interpretation.
 *
 * BS, HT, LF, and CR move the cursor; any other control byte is shown as
 * `CHAR_NON_PRINTABLE`; a printable byte is written with the current color and
 * the cursor advances, wrapping and scrolling at the screen edges.
 */
static void vga_putc_raw(char c)
{
    if (c < 31 || c == 127) {
        switch(c) {
            case CHAR_BS:
                if (x) --x;
                else if (y) { --y; x = SCREEN_NUMBER_OF_COLUMNS - 1; }

                break;

            case CHAR_HT:
                x = x - x%TAB_SIZE + TAB_SIZE;
                break;

            case CHAR_LF:
                x = 0; ++y;
                break;

            case CHAR_CR:
                x = 0;
                break;

            default:
                c = CHAR_NON_PRINTABLE;
                break;
        }

        if (c != CHAR_NON_PRINTABLE) c = CHAR_NULL;
    }

    if (c) {
        vga_text_write_cell(x, y, c, get_curr_color_attr());
        ++x;
    }

    if (x >= SCREEN_NUMBER_OF_COLUMNS) { x = 0; ++y; }
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

    if (row >= SCREEN_NUMBER_OF_ROWS) return;

    attr = get_curr_color_attr();
    for (c = c0; c <= c1 && c < SCREEN_NUMBER_OF_COLUMNS; ++c) {
        vga_text_write_cell(c, row, ' ', attr);
    }
}

/** Numeric CSI parameter `i`, or `def` when it is absent or zero. */
static unsigned int vt_arg(unsigned int i, unsigned int def)
{
    if (i < vt_nparams && vt_params[i] != 0) return vt_params[i];

    return def;
}

/** Push the current color state to the driver, applying bold and reverse. */
static void vt_apply_color(void)
{
    unsigned int fg = vt_bold ? (vt_fg | 8) : vt_fg;
    unsigned int bg = vt_bg;

    if (vt_reverse) { unsigned int t = fg & 7; fg = bg; bg = t; }

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
            vt_fg = DEFAULT_FORECOLOR; vt_bg = DEFAULT_BACKCOLOR;
            vt_bold = false; vt_reverse = false;
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

/**
 * Execute a completed CSI sequence whose final byte is `c`.
 *
 * Cursor motion (`A`-`H`, `f`, `d`, `G`), erase (`J`, `K`, `X`), color (`m`),
 * and cursor save/restore (`s`, `u`) are handled; a scroll-region set (`r`),
 * mode toggles (`h`, `l`), and anything else are ignored. Coordinates are
 * one-based on the wire and clamped to the screen.
 */
static void vt_dispatch_csi(char c)
{
    unsigned int n, row, col, r;

    switch (c) {
        case 'A': n = vt_arg(0, 1); y = (y >= n) ? y - n : 0; break;
        case 'B': n = vt_arg(0, 1); y += n;
                  if (y >= SCREEN_NUMBER_OF_ROWS) y = SCREEN_NUMBER_OF_ROWS - 1;
                  break;
        case 'C': n = vt_arg(0, 1); x += n;
                  if (x >= SCREEN_NUMBER_OF_COLUMNS) x = SCREEN_NUMBER_OF_COLUMNS - 1;
                  break;
        case 'D': n = vt_arg(0, 1); x = (x >= n) ? x - n : 0; break;
        case 'E': n = vt_arg(0, 1); y += n;
                  if (y >= SCREEN_NUMBER_OF_ROWS) y = SCREEN_NUMBER_OF_ROWS - 1;
                  x = 0;
                  break;
        case 'F': n = vt_arg(0, 1); y = (y >= n) ? y - n : 0; x = 0; break;

        case 'G': case '`':
            col = vt_arg(0, 1); x = col - 1;
            if (x >= SCREEN_NUMBER_OF_COLUMNS) x = SCREEN_NUMBER_OF_COLUMNS - 1;
            break;
        case 'd':
            row = vt_arg(0, 1); y = row - 1;
            if (y >= SCREEN_NUMBER_OF_ROWS) y = SCREEN_NUMBER_OF_ROWS - 1;
            break;
        case 'H': case 'f':
            row = vt_arg(0, 1); col = vt_arg(1, 1);
            y = row - 1; x = col - 1;
            if (y >= SCREEN_NUMBER_OF_ROWS) y = SCREEN_NUMBER_OF_ROWS - 1;
            if (x >= SCREEN_NUMBER_OF_COLUMNS) x = SCREEN_NUMBER_OF_COLUMNS - 1;
            break;

        case 'J':
            n = vt_arg(0, 0);
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
            break;
        case 'K':
            n = vt_arg(0, 0);
            if (n == 0)      vt_erase_row(y, x, SCREEN_NUMBER_OF_COLUMNS - 1);
            else if (n == 1) vt_erase_row(y, 0, x);
            else if (n == 2) vt_erase_row(y, 0, SCREEN_NUMBER_OF_COLUMNS - 1);
            break;
        case 'X':
            n = vt_arg(0, 1);
            vt_erase_row(y, x, x + n - 1);
            break;

        case 'm': vt_do_sgr(); break;
        case 's': vt_saved_x = x; vt_saved_y = y; break;
        case 'u': x = vt_saved_x; y = vt_saved_y; break;

        default: break;
    }
}

/**
 * Write one byte to the VGA console, interpreting VT100/ANSI escapes.
 *
 * A plain byte is drawn by `vga_putc_raw`. `ESC` opens an escape sequence: a
 * CSI (`ESC [`) collects semicolon-separated numeric parameters up to its
 * final letter and is executed by `vt_dispatch_csi`; a private CSI (`ESC [ ?`,
 * as in the alternate-screen and cursor-visibility toggles) is swallowed; a
 * charset selector (`ESC (` and kin) swallows its next byte; an OSC (`ESC ]`)
 * is swallowed to its terminating BEL. The hardware cursor is synced after
 * every byte, so a cursor-only sequence still moves the on-screen caret.
 */
void k_putc(char c)
{
    unsigned char uc = (unsigned char) c;

    switch (vt_state) {
        case VT_NORMAL:
            if (uc == 0x1b) vt_state = VT_ESC;
            else            vga_putc_raw(c);
            break;

        case VT_ESC:
            switch (uc) {
                case '[':
                    vt_state = VT_CSI;
                    vt_nparams = 0; vt_cur = 0;
                    vt_private = false; vt_interm = false;
                    break;
                case ']':
                    vt_state = VT_OSC;
                    vt_osc_len = 0;
                    break;
                case '(': case ')': case '*': case '+': case '#': case ' ':
                    vt_state = VT_ESC_ARG;
                    break;
                case '7':
                    vt_saved_x = x; vt_saved_y = y;
                    vt_state = VT_NORMAL;
                    break;
                case '8':
                    x = vt_saved_x; y = vt_saved_y;
                    vt_state = VT_NORMAL;
                    break;
                case 'c':
                    vt_fg = DEFAULT_FORECOLOR; vt_bg = DEFAULT_BACKCOLOR;
                    vt_bold = false; vt_reverse = false; vt_apply_color();
                    k_cls();
                    vt_state = VT_NORMAL;
                    break;
                case 0x1b:
                    break;
                default:
                    vt_state = VT_NORMAL;
                    break;
            }
            break;

        case VT_ESC_ARG:
            vt_state = VT_NORMAL;
            break;

        case VT_OSC:
            if (uc == 0x07)      vt_state = VT_NORMAL;
            else if (uc == 0x1b) vt_state = VT_ESC;
            else if (uc < 0x20)  vt_state = VT_NORMAL;
            else if (++vt_osc_len > 256) vt_state = VT_NORMAL;
            break;

        case VT_CSI:
            if (uc >= '0' && uc <= '9') {
                vt_cur = vt_cur * 10 + (uc - '0');
                if (vt_cur > 9999) vt_cur = 9999;
            } else if (uc == ';') {
                if (vt_nparams < VT_MAX_PARAMS) vt_params[vt_nparams++] = vt_cur;
                vt_cur = 0;
            } else if (uc >= 0x3c && uc <= 0x3f) {
                if (vt_nparams == 0 && vt_cur == 0) vt_private = true;
            } else if (uc >= 0x20 && uc <= 0x2f) {
                vt_interm = true;   /* intermediate byte, as in `ESC [ SP q` */
            } else if (uc >= 0x40 && uc <= 0x7e) {
                if (vt_nparams < VT_MAX_PARAMS) vt_params[vt_nparams++] = vt_cur;
                if (!vt_private && !vt_interm) vt_dispatch_csi(c);
                vt_state = VT_NORMAL;
            } else if (uc == 0x1b) {
                vt_state = VT_ESC;
            } else {
                vt_state = VT_NORMAL;
                vga_putc_raw(c);
            }
            break;
    }

    vga_text_sync();
}

void k_puts(const char *str)
{
    if (str == NULL) return;

    while (*str) { k_putc(*str++); } k_putc(CHAR_LF);
}

/**
 * Format `format` against the varargs in `args`, emitting each byte through
 * `putc_fn`.
 *
 * Supported conversions are `%d` (signed decimal), `%b`, `%o`, `%x`, `%X`
 * (binary, octal, lower- and upper-case hex), `%s` (a string, with a NULL
 * pointer rendered as `(null)`), `%c` (a character), and `%%` (a literal
 * percent). An unknown conversion is emitted verbatim. `args` points at the
 * first argument and is consumed left to right; each consumed slot is one
 * stack word, which is why integers and characters are read as `int`.
 */
void k_vprintf(putc_fn_t putc_fn, const char *format, char **args)
{
    char c, buffer[_K_PRINTF_BUFFER_SIZE_];
    const char *str;

    if (format == NULL) return;

    while ((c = *(format++))) {
        if (c == '%') {
            c = *(format++);

            switch(c) {
                case 'd':
                    k_itoa(*((unsigned int *) args++), buffer,
                           _K_PRINTF_BUFFER_SIZE_, c, false);
                    str = buffer;
                    goto string;
                case 'b':
                case 'o':
                case 'x':
                case 'X':
                    k_itoa(*((int *) args++), buffer,
                           _K_PRINTF_BUFFER_SIZE_, c, false);
                    str = buffer;
                    goto string;

                case 's':
                    str = *args++;
                    if (!str) str = "(null)";

                string:
                    while (*str) putc_fn(*str++);
                    break;

                case 'c':
                    putc_fn((char) *((int *) args++));
                    break;

                case 0:
                    break;

                case '%':
                default:
                    putc_fn(c);
                    break;
            }
        }
        else putc_fn(c);
    }
}

void k_printf(const char *format, ...)
{
    char **args = (char **) &format; ++args;
    k_vprintf(k_putc, format, args);
}

void outb(uint16_t port, uint8_t val)
{
    __asm__ __volatile__ ("outb %1, %0" : : "dN" (port) , "a" (val));
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
    __asm__ __volatile__ ("outw %1, %0" : : "dN" (port) , "a" (val));
}

void outd(uint16_t port, uint32_t val)
{
    __asm__ __volatile__ ("outl %1, %0" : : "dN" (port) , "a" (val));
}

uint8_t inb(uint16_t port)
{
    uint8_t result;

    __asm__ __volatile__ ("inb %1, %0" : "=a" (result) : "dN" (port));

    return result;
}

uint16_t inw(uint16_t port)
{
    uint16_t result;

    __asm__ __volatile__ ("inw %1, %0" : "=a" (result) : "dN" (port));

    return result;
}

uint32_t ind(uint16_t port)
{
    uint32_t result;

    __asm__ __volatile__ ("inl %1, %0" : "=a" (result) : "dN" (port));

    return result;
}

void insw(uint16_t port, void *buffer, uint32_t count)
{
    __asm__ __volatile__ (
        "cld\n\t"
        "rep insw"
        : "+D" (buffer), "+c" (count)
        : "d" (port)
        : "memory", "cc"
    );
}

void outsw(uint16_t port, const void *buffer, uint32_t count)
{
    __asm__ __volatile__ (
        "cld\n\t"
        "rep outsw"
        : "+S" (buffer), "+c" (count)
        : "d" (port)
        : "memory", "cc"
    );
}
