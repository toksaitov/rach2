#include "kio.h"

/**
 * Write one character to the VGA text screen and advance the cursor.
 *
 * Printable characters are placed at the cursor and the cursor moves right,
 * wrapping and scrolling at the screen edges. The control characters BS, HT,
 * LF, and CR move the cursor without drawing; any other control character is
 * shown as `CHAR_NON_PRINTABLE`. The hardware cursor is synced after each call.
 */
void k_putc(char c)
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
 * as a wall-clock timeout rather than a missing marker. Only ring 0 may write
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
