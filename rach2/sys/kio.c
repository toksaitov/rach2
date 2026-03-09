#include "kio.h"

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
